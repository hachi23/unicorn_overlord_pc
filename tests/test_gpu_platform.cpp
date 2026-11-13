// SPDX-License-Identifier: GPL-2.0-or-later
// The PS4 libraries of gpu/platform the game calls (gpu/platform/gnm.cpp, equeue.cpp), through
// the functions they register by NID, as the game reaches them. VideoOut is replaced by the
// small stand-ins below: these tests submit no flips.
#include <cassert>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <thread>
#include "platform.h"

using namespace Platform;

namespace {
std::map<std::string, void*> symbols;
u64 labels[16];

template <typename F>
F Nid(const char* nid) {
    const auto it = symbols.find(nid);
    assert(it != symbols.end());
    return reinterpret_cast<F>(it->second);
}

// PM4 type-3 packet header for `body` dwords.
constexpr u32 Packet(u32 opcode, u32 body) {
    return 3u << 30 | (body - 1) << 16 | opcode << 8;
}
constexpr u32 IndirectBuffer = 0x3f, WriteData = 0x37;

// Waits up to a second for the command thread to write `value` at `label`.
bool Reaches(const u32& label, u32 value) {
    for (int i = 0; i < 1000; ++i) {
        if (__atomic_load_n(&label, __ATOMIC_ACQUIRE) == value) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return false;
}

using Submit = s32 PS4_ABI (*)(u32, const u32* const*, const u32*, const u32* const*, const u32*);

// A command buffer that calls itself as an indirect buffer must not recurse forever: the
// nesting stops (as on the GPU, a few levels deep) and the packets after the call still run.
void an_indirect_buffer_that_calls_itself_ends() {
    alignas(16) static u32 label;
    alignas(16) static u32 dcb[16];
    u32 n = 0;
    dcb[n++] = Packet(IndirectBuffer, 3);
    dcb[n++] = u32(reinterpret_cast<u64>(dcb));
    dcb[n++] = u32(reinterpret_cast<u64>(dcb) >> 32) & 0xffff;
    const u32 size_at = n++;
    dcb[n++] = Packet(WriteData, 4);
    dcb[n++] = 5u << 8;   // dst_sel 5: memory
    dcb[n++] = u32(reinterpret_cast<u64>(&label));
    dcb[n++] = u32(reinterpret_cast<u64>(&label) >> 32);
    dcb[n++] = 7;
    dcb[size_at] = n;
    const u32* buffers[] = {dcb};
    const u32 bytes[] = {n * 4};
    assert(Nid<Submit>("zwY0YV91TTI")(1, buffers, bytes, nullptr, nullptr) == OK);
    assert(Reaches(label, 7));
}
using CreateEqueue = s32 PS4_ABI (*)(s64*, const char*);
using WaitEqueue = s32 PS4_ABI (*)(s64, Equeue::Event*, int, int*, const u32*);
using AddEqEvent = s32 PS4_ABI (*)(s64, u64, void*);
constexpr u32 EventWriteEop = 0x47;

// The end-of-pipe interrupt of a submitted buffer wakes a thread waiting on the queue, with the
// label already written; reading the event clears it.
void an_end_of_pipe_interrupt_wakes_the_waiter() {
    s64 queue = 0;
    assert(Nid<CreateEqueue>("D0OdFMjp46I")(&queue, "gfx") == OK && queue);
    int marker = 0;
    assert(Nid<AddEqEvent>("b0xyllnVY-I")(queue, GfxEop, &marker) == OK);

    alignas(16) static u32 label;
    alignas(16) static u32 dcb[8];
    dcb[0] = Packet(EventWriteEop, 5);
    dcb[1] = 0x504;   // the event (bottom-of-pipe timestamp)
    dcb[2] = u32(reinterpret_cast<u64>(&label));
    dcb[3] = (u32(reinterpret_cast<u64>(&label) >> 32) & 0xffff) | 2u << 24 | 1u << 29;   // interrupt, 32-bit data
    dcb[4] = 42;
    dcb[5] = 0;
    const u32* buffers[] = {dcb};
    const u32 bytes[] = {6 * 4};

    Equeue::Event event{};
    int count = -1;
    std::thread waiter([&] {
        const u32 second = 1'000'000;
        assert(Nid<WaitEqueue>("fzyMKs9kim0")(queue, &event, 1, &count, &second) == OK);
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(20));   // the waiter blocks first
    assert(Nid<Submit>("zwY0YV91TTI")(1, buffers, bytes, nullptr, nullptr) == OK);
    waiter.join();
    assert(count == 1 && event.ident == GfxEop && event.filter == Equeue::GraphicsCore);
    assert(event.udata == &marker && label == 42);

    const u32 none = 0;
    assert(Nid<WaitEqueue>("fzyMKs9kim0")(queue, &event, 1, &count, &none) == KERNEL_ETIMEDOUT);
    assert(count == 0);
}

// A wait with a timeout and nothing triggered returns ETIMEDOUT after about that long.
void a_wait_with_nothing_pending_times_out() {
    s64 queue = 0;
    assert(Nid<CreateEqueue>("D0OdFMjp46I")(&queue, "idle") == OK);
    Equeue::Event event{};
    int count = -1;
    const u32 timeout = 20'000;
    const auto start = std::chrono::steady_clock::now();
    assert(Nid<WaitEqueue>("fzyMKs9kim0")(queue, &event, 1, &count, &timeout) == KERNEL_ETIMEDOUT);
    const auto waited = std::chrono::steady_clock::now() - start;
    assert(count == 0 && waited >= std::chrono::milliseconds(19) && waited < std::chrono::seconds(1));
}
} // namespace

// ---- Stand-ins for the rest of the platform ----
namespace Platform {
void AddSymbol(const char* nid, void* function) {
    symbols[nid] = function;
}
namespace VideoOut {
u64* BufferLabels() {
    return labels;
}
bool SubmitEopFlip(s32, s32, s64) {
    return true;
}
bool ReserveFlip(s32) {
    return true;
}
} // namespace VideoOut
} // namespace Platform
extern "C" int runtime_neo_mode(void) {
    return 0;
}

int main() {
    RegisterGnm();
    RegisterEqueue();
    an_indirect_buffer_that_calls_itself_ends();
    an_end_of_pipe_interrupt_wakes_the_waiter();
    a_wait_with_nothing_pending_times_out();
    std::puts("gpu platform: ok");
    std::fflush(stdout);
    // As the game: the command thread never stops, so no static destructors run under it.
    std::_Exit(0);
}
