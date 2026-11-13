// SPDX-License-Identifier: GPL-2.0-or-later
// Kernel event queues (sceKernelCreateEqueue / WaitEqueue / DeleteEqueue). The game waits on
// them for GPU end-of-pipe events (sceGnmAddEqEvent) and flips (sceVideoOutAddFlipEvent).
// Events are kept per queue; a trigger marks the event and wakes the queue's waiters; a wait
// returns the triggered events, clearing those with the Clear flag (all but user events).
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <map>
#include <memory>
#include <vector>
#include "platform.h"

namespace Platform::Equeue {
namespace {

struct Entry {
    Event event;
    bool triggered;
};
struct Queue {
    std::string name;
    std::mutex mutex;
    std::condition_variable cv;
    std::vector<Entry> events;
};

std::mutex queues_mutex;
std::map<s64, std::shared_ptr<Queue>> queues;
s64 next_handle = 0x7000;   // their own range, apart from the file table

std::shared_ptr<Queue> Find(s64 handle) {
    std::scoped_lock lock{queues_mutex};
    const auto it = queues.find(handle);
    return it == queues.end() ? nullptr : it->second;
}

int Collect(Queue& q, Event* out, int count) {
    int n = 0;
    for (auto it = q.events.begin(); it != q.events.end() && n < count;) {
        if (!it->triggered) {
            ++it;
            continue;
        }
        out[n++] = it->event;
        if (it->event.flags & Clear) {
            it->triggered = false;
            it->event.data = 0;
            it->event.fflags = 0;
        }
        if (it->event.flags & OneShot) {
            it = q.events.erase(it);
        } else {
            ++it;
        }
    }
    return n;
}

s32 PS4_ABI sceKernelCreateEqueue(s64* handle, const char* name) {
    if (!handle) return KERNEL_EINVAL;
    if (!name) return KERNEL_EINVAL;
    if (std::strlen(name) > 32) return KERNEL_ENAMETOOLONG;
    auto q = std::make_shared<Queue>();
    q->name = name;
    std::scoped_lock lock{queues_mutex};
    *handle = next_handle++;
    queues[*handle] = std::move(q);
    return OK;
}

s32 PS4_ABI sceKernelDeleteEqueue(s64 handle) {
    std::scoped_lock lock{queues_mutex};
    return queues.erase(handle) ? OK : KERNEL_EBADF;
}

s32 PS4_ABI sceKernelWaitEqueue(s64 handle, Event* events, int count, int* out, const u32* timeout_us) {
    const auto q = Find(handle);
    if (!q) return KERNEL_EBADF;
    if (!events) return KERNEL_EFAULT;
    if (count < 1) {
        if (out) *out = 0;
        return KERNEL_EINVAL;
    }
    int n = 0;
    std::unique_lock lock{q->mutex};
    const auto ready = [&] { return (n = Collect(*q, events, count)) > 0; };
    if (timeout_us && *timeout_us == 0) {
        ready();
    } else if (timeout_us) {
        q->cv.wait_for(lock, std::chrono::microseconds(*timeout_us), ready);
    } else {
        q->cv.wait(lock, ready);
    }
    if (out) *out = n;
    return n == 0 && timeout_us ? KERNEL_ETIMEDOUT : OK;
}

constexpr s32 KERNEL_ENOENT = s32(0x80020002);

// User events: level (stays triggered until deleted) or edge (cleared when read). A trigger
// hands its udata to the waiter.
s32 AddUser(s64 handle, int id, u16 flags) {
    const auto q = Find(handle);
    if (!q) return KERNEL_EBADF;
    std::scoped_lock lock{q->mutex};
    for (Entry& e : q->events)
        if (e.event.ident == u64(id) && e.event.filter == User) {
            e.event.udata = nullptr;
            return OK;
        }
    q->events.push_back({Event{u64(id), User, flags, 0, 0, nullptr}, false});
    return OK;
}
s32 PS4_ABI sceKernelAddUserEvent(s64 handle, int id) {
    return AddUser(handle, id, Add);
}
s32 PS4_ABI sceKernelAddUserEventEdge(s64 handle, int id) {
    return AddUser(handle, id, Add | Clear);
}
s32 PS4_ABI sceKernelTriggerUserEvent(s64 handle, int id, void* udata) {
    const auto q = Find(handle);
    if (!q) return KERNEL_EBADF;
    bool found = false;
    {
        std::scoped_lock lock{q->mutex};
        for (Entry& e : q->events)
            if (e.event.ident == u64(id) && e.event.filter == User) {
                e.triggered = true;
                e.event.udata = udata;
                found = true;
            }
    }
    if (!found) return KERNEL_ENOENT;
    q->cv.notify_all();
    return OK;
}
s32 PS4_ABI sceKernelDeleteUserEvent(s64 handle, int id) {
    if (!Find(handle)) return KERNEL_EBADF;
    return RemoveEvent(handle, u64(id), User) ? OK : KERNEL_ENOENT;
}
void* PS4_ABI sceKernelGetEventUserData(const Event* e) {
    return e->udata;
}
u64 PS4_ABI sceKernelGetEventId(const Event* e) {
    return e->ident;
}
s32 PS4_ABI sceKernelGetEventFilter(const Event* e) {
    return e->filter;
}
u64 PS4_ABI sceKernelGetEventData(const Event* e) {
    return e->data;
}

} // namespace

bool AddEvent(s64 handle, u64 ident, s16 filter, void* udata) {
    const auto q = Find(handle);
    if (!q) return false;
    std::scoped_lock lock{q->mutex};
    for (Entry& e : q->events) {
        if (e.event.ident == ident && e.event.filter == filter) {
            e.event.udata = udata;   // an existing event keeps its state
            return true;
        }
    }
    const u16 flags = filter == User ? 0 : Clear;
    q->events.push_back({Event{ident, filter, flags, 0, 0, udata}, false});
    return true;
}

bool RemoveEvent(s64 handle, u64 ident, s16 filter) {
    const auto q = Find(handle);
    if (!q) return false;
    std::scoped_lock lock{q->mutex};
    for (auto it = q->events.begin(); it != q->events.end(); ++it) {
        if (it->event.ident == ident && it->event.filter == filter) {
            q->events.erase(it);
            return true;
        }
    }
    return false;
}

void Trigger(u64 ident, s16 filter, u64 data) {
    std::vector<std::shared_ptr<Queue>> all;
    {
        std::scoped_lock lock{queues_mutex};
        for (auto& [handle, q] : queues) all.push_back(q);
    }
    for (auto& q : all) {
        bool found = false;
        {
            std::scoped_lock lock{q->mutex};
            for (Entry& e : q->events) {
                if (e.event.ident != ident || e.event.filter != filter) continue;
                e.triggered = true;
                if (filter == VideoOut) {
                    // data: event id | arg << 16; the event keeps a 4-bit count of triggers
                    // since it was last read and the low 12 bits of the time stamp.
                    const u64 count = (e.event.data >> 12) & 0xf;
                    const u64 time = u64(std::chrono::steady_clock::now().time_since_epoch().count());
                    e.event.data = (time & 0xfff) | ((count == 0xf ? count : count + 1) << 12) |
                                   (data & ~u64(0xffff));
                } else {
                    e.event.data = data;
                }
                found = true;
            }
        }
        if (found) q->cv.notify_all();
    }
}

} // namespace Platform::Equeue

namespace Platform {
void RegisterEqueue() {
    using namespace Equeue;
    AddSymbol("D0OdFMjp46I", (void*)sceKernelCreateEqueue);
    AddSymbol("jpFjmgAC5AE", (void*)sceKernelDeleteEqueue);
    AddSymbol("fzyMKs9kim0", (void*)sceKernelWaitEqueue);
    AddSymbol("4R6-OvI2cEA", (void*)sceKernelAddUserEvent);
    AddSymbol("WDszmSbWuDk", (void*)sceKernelAddUserEventEdge);
    AddSymbol("F6e0kwo4cnk", (void*)sceKernelTriggerUserEvent);
    AddSymbol("LJDwdSNTnDg", (void*)sceKernelDeleteUserEvent);
    AddSymbol("vz+pg2zdopI", (void*)sceKernelGetEventUserData);
    AddSymbol("mJ7aghmgvfc", (void*)sceKernelGetEventId);
    AddSymbol("23CPPI1tyBY", (void*)sceKernelGetEventFilter);
    AddSymbol("kwGyyjohI50", (void*)sceKernelGetEventData);
}
} // namespace Platform
