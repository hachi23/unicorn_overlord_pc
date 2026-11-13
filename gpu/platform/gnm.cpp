// SPDX-License-Identifier: GPL-2.0-or-later
// GnmDriver: command buffer submission and the GPU events around it.
//
// The game's draws never reach these command buffers (src/native_rec.c records the graphics
// library calls and src/native_vk.c draws them), so what the command thread does with a
// submission is the synchronisation in it, in order: memory writes (WRITE_DATA, end-of-pipe and
// end-of-shader writes, DMA from data or memory), end-of-pipe interrupts (the GfxEop event),
// waits on memory (WAIT_REG_MEM: the flip-done wait, compute queue waits), calls of other
// command buffers, and the flip marker that sceGnmSubmitAndFlipCommandBuffers leaves. Every
// other packet is state or work for the GPU and is skipped. The first sighting of each packet
// type is logged once (VW_GNM_LOG=1) to show what the game sends.
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <functional>
#include <thread>
#include <vector>
#include "platform.h"

namespace Platform::Gnm {
namespace {

// PM4 type-3 opcodes acted on.
enum : u32 {
    Nop = 0x10, IndirectBufferConst = 0x33, WriteData = 0x37, WaitRegMem = 0x3c, IndirectBuffer = 0x3f,
    EventWriteEop = 0x47, EventWriteEos = 0x48, ReleaseMem = 0x49, DmaData = 0x50,
};
// Payloads of the NOP packets the system library places (the flip marker 64 dwords from the
// end of the last command buffer of a flip submission).
enum : u32 {
    PrepareFlip = 0x68750777u, PrepareFlipLabel = 0x68750778u, PrepareFlipInterrupt = 0x68750780u,
    PrepareFlipInterruptLabel = 0x68750781u,
};
constexpr u32 FlipMarkerHeader = 0xc03e1000u;   // NOP with 63 dwords of payload

struct Flip {
    s32 handle, index;
    s64 arg;
    u32 payload[5];   // the marker's payload type and its label/interrupt words
};
struct Work {
    // Copies of the submitted command buffers: the game may rewrite a buffer as soon as it sees
    // a label from it, while later packets of the same buffer are still to be read.
    std::vector<u32> words;
    std::vector<std::pair<size_t, u32>> buffers;        // offset into words, dwords
    void Add(const u32* cmd, u32 dwords) {
        buffers.emplace_back(words.size(), dwords);
        words.insert(words.end(), cmd, cmd + dwords);
    }
    bool has_flip{};
    Flip flip{};
    u32 marker_at{};                                    // dword offset of the marker in the last buffer
    u32* read_pointer{};                                // compute ring: advanced once run
    u32 read_value{};
    std::function<void()> call;                         // VideoOut: a flip after earlier work
};

std::mutex mutex;
std::condition_variable work_cv, idle_cv;
std::deque<Work> queue;
bool busy;
bool command_thread_started;
const bool log_packets = [] {
    const char* v = std::getenv("VW_GNM_LOG");
    return v && *v == '1';
}();

u64 GpuClock() {
    return u64(std::chrono::duration_cast<std::chrono::nanoseconds>(
                   std::chrono::steady_clock::now().time_since_epoch())
                   .count());
}

void Note(u32 opcode, const char* what) {
    static std::atomic<u64> seen[4];
    if (!log_packets) return;
    const u64 bit = 1ull << (opcode & 63);
    if (seen[opcode >> 6].fetch_or(bit) & bit) return;
    std::printf("Gnm: packet 0x%02x %s\n", opcode, what);
}

// Labels the game polls from its own threads: release stores, so what the command thread did
// before (earlier labels, flips) is visible once the game sees the value. Labels are aligned.
void Write(u64 address, u64 value, u32 bytes) {
    if (!address) return;
    if (address % bytes != 0) {   // not seen from the game; kept byte-exact
        std::atomic_thread_fence(std::memory_order_release);
        std::memcpy(reinterpret_cast<void*>(address), &value, bytes);
    } else if (bytes == 8) {
        __atomic_store_n(reinterpret_cast<u64*>(address), value, __ATOMIC_RELEASE);
    } else {
        __atomic_store_n(reinterpret_cast<u32*>(address), u32(value), __ATOMIC_RELEASE);
    }
}

// End-of-pipe and RELEASE_MEM data: 1 low 32 bits, 2 64 bits, 3 the GPU clock, 4 the "system"
// clock (a 100 MHz counter on the console).
void WriteSelected(u64 address, u32 data_sel, u32 lo, u32 hi) {
    switch (data_sel) {
    case 1: Write(address, lo, 4); break;
    case 2: Write(address, u64(lo) | u64(hi) << 32, 8); break;
    case 3: Write(address, GpuClock(), 8); break;
    case 4: Write(address, GpuClock() / 10, 8); break;
    default: break;
    }
}

bool Compare(u32 function, u32 value, u32 ref) {
    switch (function) {
    case 0: return true;
    case 1: return value < ref;
    case 2: return value <= ref;
    case 3: return value == ref;
    case 4: return value != ref;
    case 5: return value >= ref;
    case 6: return value > ref;
    default: return true;
    }
}

void Interrupt() {
    Equeue::Trigger(GfxEop, Equeue::GraphicsCore, GfxEop);
}

void Run(const u32* cmd, u32 dwords, int depth);

// depth: how many indirect buffers deep this packet is (0: the submitted buffer).
void RunPacket(u32 opcode, const u32* body, u32 count, int depth) {
    switch (opcode) {
    case WriteData: {   // control, address lo, hi, data...
        Note(opcode, "WRITE_DATA");
        const u32 dst_sel = (body[0] >> 8) & 0xf;
        if (dst_sel != 1 && dst_sel != 2 && dst_sel != 5) return;   // memory only
        const u64 address = u64(body[1]) | u64(body[2]) << 32;
        for (u32 i = 3; i < count; ++i) Write(address + (i - 3) * 4, body[i], 4);
        return;
    }
    case EventWriteEop: {   // event, address lo, address hi | int_sel | data_sel, data lo, hi
        Note(opcode, "EVENT_WRITE_EOP");
        const u64 address = u64(body[1] & ~3u) | u64(body[2] & 0xffff) << 32;
        const u32 data_sel = body[2] >> 29, int_sel = (body[2] >> 24) & 3;
        WriteSelected(address, data_sel, body[3], body[4]);
        if (int_sel) Interrupt();
        return;
    }
    case ReleaseMem: {   // event, data_sel | int_sel | dst_sel, address lo, hi, data lo, hi
        Note(opcode, "RELEASE_MEM");
        const u32 data_sel = body[1] >> 29, int_sel = (body[1] >> 24) & 3;
        const u64 address = u64(body[2] & ~3u) | u64(body[3]) << 32;
        WriteSelected(address, data_sel, body[4], body[5]);
        if (int_sel) Interrupt();
        return;
    }
    case EventWriteEos: {   // event, address lo, address hi | command, data
        Note(opcode, "EVENT_WRITE_EOS");
        const u32 command = body[2] >> 29;
        if (command == 2) Write(u64(body[1] & ~3u) | u64(body[2] & 0xffff) << 32, body[3], 4);
        return;
    }
    case WaitRegMem: {   // function | mem_space, address lo, hi, ref, mask, interval
        Note(opcode, "WAIT_REG_MEM");
        if (!((body[0] >> 4) & 1)) return;   // registers: nothing to wait for
        const u32 function = body[0] & 7;
        const auto* p = reinterpret_cast<const volatile u32*>(u64(body[1] & ~3u) | u64(body[2]) << 32);
        const u32 ref = body[3], mask = body[4];
        for (u32 spins = 0; !Compare(function, *p & mask, ref); ++spins) {
            if (spins < 64) std::this_thread::yield();
            else std::this_thread::sleep_for(std::chrono::microseconds(100));
        }
        return;
    }
    case DmaData: {   // control, src lo (or data), src hi, dst lo, dst hi, command (bytes)
        Note(opcode, "DMA_DATA");
        const u32 src_sel = (body[0] >> 29) & 3, dst_sel = (body[0] >> 20) & 3;
        const u32 bytes = body[5] & 0x1fffff;
        const u64 dst = u64(body[3]) | u64(body[4]) << 32;
        if (dst_sel != 0 && dst_sel != 3) return;   // GDS: not used by the renderer
        // DAS/SAS: the destination or source is a register offset (the game copies register
        // state this way), not memory. GPU registers mean nothing here.
        if ((body[5] >> 27) & 1) return;
        if (((body[5] >> 26) & 1) && src_sel != 2) return;
        if (src_sel == 2) {
            for (u32 i = 0; i + 4 <= bytes; i += 4) std::memcpy(reinterpret_cast<u8*>(dst) + i, &body[1], 4);
        } else if (src_sel == 0 || src_sel == 3) {
            std::memmove(reinterpret_cast<void*>(dst), reinterpret_cast<const void*>(u64(body[1]) | u64(body[2]) << 32), bytes);
        }
        return;
    }
    case IndirectBuffer: case IndirectBufferConst: {   // address lo, hi, size
        Note(opcode, "INDIRECT_BUFFER");
        const u64 address = u64(body[0] & ~3u) | u64(body[1] & 0xffff) << 32;
        if (opcode == IndirectBuffer) Run(reinterpret_cast<const u32*>(address), body[2] & 0xfffff, depth + 1);
        return;
    }
    case Nop: return;
    default: Note(opcode, "skipped"); return;
    }
}

void Run(const u32* cmd, u32 dwords, int depth) {
    for (u32 at = 0; at < dwords;) {
        const u32 header = cmd[at];
        const u32 type = header >> 30;
        if (type == 2) { ++at; continue; }
        const u32 count = ((header >> 16) & 0x3fff) + 1;   // body dwords
        if (type == 3 && depth < 4) RunPacket((header >> 8) & 0xff, cmd + at + 1, count, depth);
        at += 1 + count;
    }
}

// The flip marker: the buffer label goes to 1, the flip is queued, then the payload's label
// write and interrupt.
void RunFlip(const Flip& f) {
    u64* labels = VideoOut::BufferLabels();
    std::atomic_ref<u64>(labels[f.index]).store(1, std::memory_order_release);
    VideoOut::SubmitEopFlip(f.handle, f.index, f.arg);
    const u32 type = f.payload[0];
    if (type == PrepareFlipLabel || type == PrepareFlipInterruptLabel) {
        const u64 address = u64(f.payload[1] & ~3u) | u64(f.payload[2]) << 32;
        Write(address, f.payload[3], 4);
    }
    if (type == PrepareFlipInterrupt || type == PrepareFlipInterruptLabel) Interrupt();
}

void CommandThread() {
    for (;;) {
        Work work;
        {
            std::unique_lock lock{mutex};
            work_cv.wait(lock, [] { return !queue.empty(); });
            work = std::move(queue.front());
            queue.pop_front();
            busy = true;
        }
        for (size_t i = 0; i < work.buffers.size(); ++i) {
            const auto [offset, dwords] = work.buffers[i];
            const u32* cmd = work.words.data() + offset;
            const bool last = i + 1 == work.buffers.size();
            if (last && work.has_flip) {
                Run(cmd, work.marker_at, 0);
                RunFlip(work.flip);
                Run(cmd + work.marker_at + 64, dwords - work.marker_at - 64, 0);
            } else {
                Run(cmd, dwords, 0);
            }
        }
        if (work.call) work.call();
        if (work.read_pointer) std::atomic_ref<u32>(*work.read_pointer).store(work.read_value, std::memory_order_release);
        {
            std::scoped_lock lock{mutex};
            busy = false;
        }
        idle_cv.notify_all();
    }
}

void Queue(Work&& work) {
    {
        std::scoped_lock lock{mutex};
        if (!command_thread_started) {
            command_thread_started = true;
            std::thread(CommandThread).detach();
        }
        queue.push_back(std::move(work));
    }
    work_cv.notify_one();
}

constexpr s32 ErrorBadCommandBuffer = s32(0x80d11000);

s32 Submit(u32 count, const u32* const* dcb, const u32* dcb_bytes, Work& work) {
    if (count && (!dcb || !dcb_bytes)) return ErrorBadCommandBuffer;
    for (u32 i = 0; i < count; ++i) {
        if (!dcb_bytes[i] || dcb_bytes[i] > 0x3ffffc) return ErrorBadCommandBuffer;
        work.Add(dcb[i], dcb_bytes[i] / 4);
    }
    // Constant command buffers only hold CE state for the GPU: nothing to do.
    Queue(std::move(work));
    return OK;
}

s32 PS4_ABI sceGnmSubmitCommandBuffers(u32 count, const u32* const* dcb, const u32* dcb_bytes,
                                       const u32* const*, const u32*) {
    Work work;
    return Submit(count, dcb, dcb_bytes, work);
}

s32 PS4_ABI sceGnmSubmitAndFlipCommandBuffers(u32 count, const u32* const* dcb, const u32* dcb_bytes,
                                              const u32* const*, const u32*, u32 handle, u32 index,
                                              u32 /*flip_mode*/, s64 arg) {
    if (!count || !dcb || !dcb_bytes || dcb_bytes[count - 1] / 4 < 64) return ErrorBadCommandBuffer;
    Work work;
    const u32 dwords = dcb_bytes[count - 1] / 4;
    const u32* marker = dcb[count - 1] + dwords - 64;
    if (marker[0] != FlipMarkerHeader) {
        std::printf("Gnm: no flip marker at the end of the command buffer (%08x)\n", marker[0]);
        return ErrorBadCommandBuffer;
    }
    if (!VideoOut::ReserveFlip(s32(handle))) return s32(0x80d11081);   // flip queue full
    work.has_flip = true;
    work.marker_at = dwords - 64;
    work.flip = {s32(handle), s32(index), arg, {marker[1], marker[2], marker[3], marker[4], marker[5]}};
    return Submit(count, dcb, dcb_bytes, work);
}

s32 PS4_ABI sceGnmSubmitCommandBuffersForWorkload(u64, u32 count, const u32* const* dcb, const u32* dcb_bytes,
                                                   const u32* const* ccb, const u32* ccb_bytes) {
    return sceGnmSubmitCommandBuffers(count, dcb, dcb_bytes, ccb, ccb_bytes);
}
s32 PS4_ABI sceGnmSubmitAndFlipCommandBuffersForWorkload(u64, u32 count, const u32* const* dcb,
                                                         const u32* dcb_bytes, const u32* const* ccb,
                                                         const u32* ccb_bytes, u32 handle, u32 index,
                                                         u32 flip_mode, s64 arg) {
    return sceGnmSubmitAndFlipCommandBuffers(count, dcb, dcb_bytes, ccb, ccb_bytes, handle, index, flip_mode, arg);
}

s32 PS4_ABI sceGnmSubmitDone() {
    return OK;
}
s32 PS4_ABI sceGnmAreSubmitsAllowed() {
    return 1;
}

// Packet builders whose GPU state the native renderer does not need: the space the caller
// reserved becomes one NOP.
void FillNop(u32* cmd, u32 dwords) {
    if (!cmd || !dwords) return;
    if (dwords == 1) {
        cmd[0] = 0x80000000u;   // type-2 NOP
        return;
    }
    cmd[0] = 0xc0000000u | ((dwords - 2) << 16) | (Nop << 8);
    std::memset(cmd + 1, 0, (dwords - 1) * 4);
}
constexpr u32 HardwareStateDwords = 0x100;
u32 PS4_ABI sceGnmDrawInitDefaultHardwareState200(u32* cmd, u32 dwords) {
    if (dwords < HardwareStateDwords) return 0;
    FillNop(cmd, HardwareStateDwords);
    return HardwareStateDwords;
}
u32 PS4_ABI sceGnmDispatchInitDefaultHardwareState(u32* cmd, u32 dwords) {
    if (dwords < HardwareStateDwords) return 0;
    FillNop(cmd, HardwareStateDwords);
    return HardwareStateDwords;
}
s32 PS4_ABI FillShader(u32* cmd, u32 dwords) {
    FillNop(cmd, dwords);
    return OK;
}
s32 PS4_ABI sceGnmInsertPopMarker(u32* cmd, u32 dwords) {
    FillNop(cmd, dwords);
    return OK;
}
s32 PS4_ABI sceGnmInsertMarker(u32* cmd, u32 dwords, const char*) {
    FillNop(cmd, dwords);
    return OK;
}

// The wait for a display buffer to stop being shown (its label back to 0).
s32 PS4_ABI sceGnmInsertWaitFlipDone(u32* cmd, u32 dwords, s32 /*handle*/, u32 index) {
    if (dwords != 7) return -1;
    const u64 address = reinterpret_cast<u64>(VideoOut::BufferLabels() + index);
    cmd[0] = 0xc0000000u | (5u << 16) | (WaitRegMem << 8);
    cmd[1] = 0x13;   // memory, equal
    cmd[2] = u32(address);
    cmd[3] = u32(address >> 32);
    cmd[4] = 0;      // ref
    cmd[5] = ~0u;    // mask
    cmd[6] = 10;     // poll interval
    return OK;
}

// A compute queue's wait on memory: NOP, WAIT_REG_MEM, NOP (14 dwords).
s32 PS4_ABI sceGnmComputeWaitOnAddress(u32* cmd, u32 dwords, u64 address, u32 mask, u32 function, u32 ref) {
    if (!cmd || dwords != 14) return -1;
    const u32 memory = address > 0xffff;
    FillNop(cmd, 3);
    cmd[3] = 0xc0000000u | (5u << 16) | (WaitRegMem << 8);
    cmd[4] = memory << 4 | (function & 7);
    cmd[5] = u32(address & (memory ? 0xfffffffcu : 0xffffu));
    cmd[6] = u32(address >> 32);
    cmd[7] = ref;
    cmd[8] = mask;
    cmd[9] = 10;
    FillNop(cmd + 10, 4);
    return OK;
}
u32 PS4_ABI sceGnmDrawInitToDefaultContextState400(u32* cmd, u32 dwords) {
    if (dwords != HardwareStateDwords) return 0;
    FillNop(cmd, HardwareStateDwords);
    return HardwareStateDwords;
}
s32 PS4_ABI sceGnmDispatchIndirectOnMec(u32* cmd, u32 dwords, u64 args, u32) {
    if (!cmd || dwords != 8 || !args || (args & 3)) return -1;
    FillNop(cmd, dwords);
    return OK;
}
s32 PS4_ABI sceGnmInsertDingDongMarker(u32* cmd, u32 dwords) {
    if (!cmd || dwords != 4) return -1;
    FillNop(cmd, dwords);
    return OK;
}

s32 PS4_ABI sceGnmAddEqEvent(s64 queue, u64 id, void* udata) {
    return Equeue::AddEvent(queue, id, Equeue::GraphicsCore, udata) ? OK : KERNEL_EBADF;
}
s32 PS4_ABI sceGnmDeleteEqEvent(s64 queue, u64 id) {
    return Equeue::RemoveEvent(queue, id, Equeue::GraphicsCore) ? OK : KERNEL_EBADF;
}
s32 PS4_ABI sceGnmGetEqEventType(const Equeue::Event* event) {
    return event ? s32(event->data) : 0;
}

// Compute queues (sceGnmMapComputeQueue, DingDong): rings of command buffers the game writes
// and announces; the command thread runs them like graphics submissions.
struct Ring {
    const u32* base;
    u32 dwords;
    u32* read_pointer;
    u32 offset;
};
std::vector<Ring> rings;
std::mutex rings_mutex;

s32 PS4_ABI sceGnmMapComputeQueue(u32 pipe, u32 queue_id, u64 base, u32 dwords, u32* read_pointer) {
    if (pipe >= 7) return s32(0x80d17000);
    if (queue_id >= 8) return s32(0x80d17001);
    if (base % 256) return s32(0x80d17002);
    if (!dwords || (dwords & (dwords - 1))) return s32(0x80d17003);
    if (reinterpret_cast<u64>(read_pointer) % 4) return s32(0x80d17004);
    std::scoped_lock lock{rings_mutex};
    rings.push_back({reinterpret_cast<const u32*>(base), dwords, read_pointer, 0});
    if (read_pointer) *read_pointer = 0;
    return s32(rings.size());   // 1-based
}
s32 PS4_ABI sceGnmMapComputeQueueWithPriority(u32 pipe, u32 queue_id, u64 base, u32 dwords, u32* read_pointer, u32) {
    return sceGnmMapComputeQueue(pipe, queue_id, base, dwords, read_pointer);
}
s32 PS4_ABI sceGnmUnmapComputeQueue(u32) {
    return OK;
}
void PS4_ABI sceGnmDingDong(u32 id, u32 next) {
    Work work;
    {
        std::scoped_lock lock{rings_mutex};
        if (!id || id > rings.size()) return;
        Ring& r = rings[id - 1];
        if (next == r.offset) return;
        if (next < r.offset && next != 0) {   // wrapped: the end of the ring, then its start
            work.Add(r.base + r.offset, r.dwords - r.offset);
            r.offset = 0;
        }
        work.Add(r.base + r.offset, (next ? next : r.dwords) - r.offset);
        r.offset = next;
        work.read_pointer = r.read_pointer;
        work.read_value = next;
    }
    Queue(std::move(work));
}

s32 PS4_ABI sceGnmLogicalCuMaskToPhysicalCuMask(s64, s32 mask) {
    return mask;
}
// Not in retail firmware (resource registration, capture, validation): the values shadPS4 gives.
constexpr s32 ErrorFailure = s32(0x8EEE00FF), ErrorValidationNotEnabled = s32(0x80D13FFF),
              ErrorCaptureFailed = s32(0x80D1500F);
s32 PS4_ABI Failure() {
    return ErrorFailure;
}
s32 PS4_ABI ValidationNotEnabled() {
    return ErrorValidationNotEnabled;
}
s32 PS4_ABI CaptureFailed() {
    return ErrorCaptureFailed;
}
bool PS4_ABI False() {
    return false;
}
extern "C" int runtime_neo_mode(void);
u32 PS4_ABI sceGnmGetGpuCoreClockFrequency() {
    return runtime_neo_mode() ? 911'000'000 : 800'000'000;
}
u32 PS4_ABI sceGnmGetOffChipTessellationBufferSize() {
    return 0x800000;
}
s32 PS4_ABI sceGnmCreateWorkloadStream(u64 name, u32* stream) {
    if (!name || !stream) return 3;
    *stream = 1;
    return OK;
}
s32 PS4_ABI sceGnmBeginWorkload(u32 stream, u64* workload) {
    if (!workload) return 3;
    *workload = stream < 0x10 ? 1 : 0;
    return stream > 0xf;
}
s32 PS4_ABI sceGnmEndWorkload(u64 workload) {
    return workload ? (((workload >> 56) & 0xff) > 0xf) * 2 : 2;
}
void PS4_ABI sceGnmDingDongForWorkload(u32 id, u32 next, u64) {
    sceGnmDingDong(id, next);
}
s32 PS4_ABI LogicalCuIndexToPhysical(s32 index) {
    return index;
}
s32 PS4_ABI Zero() {
    return OK;
}
u64 PS4_ABI sceGnmGetTheTessellationFactorRingBufferBaseAddress() {
    alignas(256) static u8 ring[0x20000];
    return reinterpret_cast<u64>(ring);
}

} // namespace

void QueueCall(std::function<void()> call) {
    Work work;
    work.call = std::move(call);
    Queue(std::move(work));
}
} // namespace Platform::Gnm

namespace Platform {
void RegisterGnm() {
    using namespace Gnm;
    AddSymbol("0H2vBYbTLHI", (void*)sceGnmDrawInitDefaultHardwareState200);
    AddSymbol("nF6bFRUBRAU", (void*)sceGnmDispatchInitDefaultHardwareState);
    // Shader programs: the native renderer binds them from its own records.
    for (const char* nid : {"bQVd5YzCal0", "4MgRw-bVNQU", "gAhCn6UiU4Y", "V31V01UiScY", "FUHG8sQ3R58",
                            "VJNjFtqiF5w", "GNlx+y7xPdE", "vckdzbQ46SI", "KXltnCwEJHQ", "UJwNuMBcUAk",
                            "nLM2i2+65hA"}) {
        AddSymbol(nid, (void*)FillShader);
    }
    // Draws, dispatches and state the game's graphics library would write; the native renderer
    // draws from its own records, so they only reserve space in the command buffer.
    for (const char* nid : {"HlTPoZ-oY7Y", "GGsn7jMTxw4", "ED9-Fjr8Ta4", "oYM+YzfCm2Y", "4v+otIIdjqg",
                            "stDSYW2SBVM", "0BzLGljcwBo", "Z43vKp5k7r0", "X9Omw9dwv5M", "+AFvOEXrKJk",
                            "5uFKckiJYRM", "mLVL7N7BVBg", "cFCp0NX8wf0", "MYRtYhojKdA"}) {
        AddSymbol(nid, (void*)FillShader);
    }
    AddSymbol("yb2cRhagD1I", (void*)sceGnmDrawInitDefaultHardwareState200);   // ...350: the same NOPs
    AddSymbol("im2ZuItabu4", (void*)sceGnmDrawInitToDefaultContextState400);
    AddSymbol("ffrNQOshows", (void*)sceGnmComputeWaitOnAddress);
    AddSymbol("wED4ZXCFJT0", (void*)sceGnmDispatchIndirectOnMec);
    AddSymbol("NfvOrNzy6sk", (void*)sceGnmInsertDingDongMarker);
    // DrawIndexIndirectCountMulti, DrawIndexMultiInstanced, DrawIndirectCountMulti,
    // SetCsShaderWithModifier: reserved space only.
    for (const char* nid : {"thbPcG7E7qk", "jHdPvIzlpKc", "cUCo8OvArrw", "Kx-h-nWQJ8A"}) {
        AddSymbol(nid, (void*)FillShader);
    }
    AddSymbol("jRcI8VcgTz4", (void*)sceGnmSubmitCommandBuffersForWorkload);
    AddSymbol("Ga6r7H6Y0RI", (void*)sceGnmSubmitAndFlipCommandBuffersForWorkload);
    AddSymbol("Fwvh++m9IQI", (void*)sceGnmGetGpuCoreClockFrequency);
    AddSymbol("FFVZcCu3zWU", (void*)sceGnmGetOffChipTessellationBufferSize);
    AddSymbol("5udAm+6boVg", (void*)sceGnmCreateWorkloadStream);
    // ComputeWaitSemaphore, DestroyWorkloadStream, GetLastWaitedAddress, GetProtectionFaultTimeStamp,
    // GetShaderProgramBaseAddress, LogicalTcaUnitToPhysical, RequestFlipAndSubmitDoneForWorkload,
    // SetWaveLimitMultipliers.
    for (const char* nid : {"EJapNl2+pgU", "UtObDRQiGbs", "iFirFzgYsvw", "fzJdEihTFV4", "nEyFbYUloIM",
                            "Kl0Z3LH07QI", "6YRHhh5mHCs", "XiyzNZ9J4nQ"}) {
        AddSymbol(nid, (void*)Zero);
    }
    // DriverTraceInProgress, ValidateOnSubmitEnabled.
    for (const char* nid : {"R6z1xM3pW-w", "rTIV11nMQuM"}) {
        AddSymbol(nid, (void*)False);
    }
    // GetPhysicalCounterFromVirtualized, GetResourceBaseAddressAndSizeInBytes, GetResourceShaderGuid,
    // QueryResourceRegistrationUserMemoryRequirements, RegisterOwnerForSystem,
    // SetResourceRegistrationUserMemory, UnregisterAllResourcesForOwner, UnregisterOwnerAndResources.
    for (const char* nid : {"dewXw5roLs0", "4PKnYXOhcx4", "UBv7FkVfzcQ", "6IMbpR7nTzA", "j6mSQs3UgaY",
                            "q-qhDxP67Hg", "yhFCnaz5daw", "fhKwCVVj9nk"}) {
        AddSymbol(nid, (void*)Failure);
    }
    // ValidateDisableDiagnostics(2), ValidateDispatch/DrawCommandBuffers, ValidateGetDiagnosticInfo,
    // ValidateGetDiagnostics, ValidationRegisterMemoryCheckCallback.
    for (const char* nid : {"SXw4dZEkgpA", "BgM3t3LvcNk", "qGP74T5OWJc", "hsZPf1lON7E", "RX7XCNSaL6I",
                            "5SHGNwLXBV4", "Q7t4VEYLafI"}) {
        AddSymbol(nid, (void*)ValidationNotEnabled);
    }
    AddSymbol("ihxrbsoSKWc", (void*)sceGnmBeginWorkload);
    AddSymbol("Fa3x75OOLRA", (void*)sceGnmEndWorkload);
    AddSymbol("byXlqupd8cE", (void*)sceGnmDingDongForWorkload);
    // FindResourcesPublic, GetOwnerName, GetResourceName/Type/UserData, RegisterGdsResource,
    // RegisterOwner, RegisterResource, SetResourceUserData, UnregisterResource.
    for (const char* nid : {"4Mv9OXypBG8", "QJjPjlmPAL0", "O0S96YnD04U", "bdqdvIkLPIU", "UoBuWAhKk7U",
                            "9Mv61HaMhfA", "ZFqKFl23aMc", "nvEwfYAImTs", "K3BKBBYKUSE", "k8EXkhIP+lM"}) {
        AddSymbol(nid, (void*)Failure);
    }
    AddSymbol("MBMa6EFu4Ko", (void*)ValidationNotEnabled);   // ValidateResetState
    for (const char* nid : {"u9YKpRRHe-M", "4UFagYlfuAM", "xeTLfxVIQO4", "9thMn+uB1is"}) {
        AddSymbol(nid, (void*)CaptureFailed);                // sceRazorCapture*
    }
    AddSymbol("f33OrruQYbM", (void*)False);                  // sceRazorIsLoaded
    AddSymbol("jiItzS6+22g", (void*)sceGnmInsertMarker);        // SetMarker
    AddSymbol("W1Etj-jlW7Y", (void*)sceGnmInsertMarker);        // PushMarker
    AddSymbol("aPIZJTXC+cU", (void*)sceGnmInsertMarker);        // PushColorMarker (color ignored)
    AddSymbol("aj3L-iaFmyk", (void*)sceGnmInsertMarker);        // SetColorMarker
    AddSymbol("7qZVNgEu+SY", (void*)sceGnmInsertPopMarker);
    AddSymbol("1qXLHIpROPE", (void*)sceGnmInsertWaitFlipDone);
    AddSymbol("zwY0YV91TTI", (void*)sceGnmSubmitCommandBuffers);
    AddSymbol("xbxNatawohc", (void*)sceGnmSubmitAndFlipCommandBuffers);
    AddSymbol("yvZ73uQUqrk", (void*)sceGnmSubmitDone);
    AddSymbol("b08AgtPlHPg", (void*)sceGnmAreSubmitsAllowed);
    AddSymbol("b0xyllnVY-I", (void*)sceGnmAddEqEvent);
    AddSymbol("PVT+fuoS9gU", (void*)sceGnmDeleteEqEvent);
    AddSymbol("UoYY0DWMC0U", (void*)sceGnmGetEqEventType);
    AddSymbol("29oKvKXzEZo", (void*)sceGnmMapComputeQueue);
    AddSymbol("A+uGq+3KFtQ", (void*)sceGnmMapComputeQueueWithPriority);
    AddSymbol("ArSg-TGinhk", (void*)sceGnmUnmapComputeQueue);
    AddSymbol("bX5IbRvECXk", (void*)sceGnmDingDong);
    AddSymbol("RU74kek-N0c", (void*)sceGnmLogicalCuMaskToPhysicalCuMask);
    AddSymbol("ln33zjBrfjk", (void*)sceGnmGetTheTessellationFactorRingBufferBaseAddress);
    // Diagnostics, capture and statistics the console offers: nothing to do.
    AddSymbol("26PM5Mzl8zc", (void*)LogicalCuIndexToPhysical);
    for (const char* nid : {"oL4hGI1PMpw", "tZCSL5ulnB4", "KnldROUkWJY", "k7iGTvDQPLQ", "HzMN7ANqYEc",
                            "iCO804ZgzdA", "H7-fgvEutM0", "iBt3Oe00Kvc", "qpGITzPE+Zc",
                            "gObODli-OH8", "+xuDhxlWRPg", "f85orjx7qts", "HHo1BAljZO8", "jtkqXpAOY6w",
                            "d88anrgNoKY", "TLV4mswiZ4A", "jg33rEKLfVs"}) {
        AddSymbol(nid, (void*)Zero);
    }
}
} // namespace Platform
