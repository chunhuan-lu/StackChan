/*
 * SPDX-FileCopyrightText: 2026 chunhuan-lu/StackChan contributors
 *
 * SPDX-License-Identifier: MIT
 */
#include "claude_status.h"
#include <esp_heap_caps.h>
#include <cstring>
#include <new>

namespace stackchan::sc {

static constexpr int32_t kStaleMs     = 30 * 1000;  // protocol: no valid frame for 30 s = agent gone
static constexpr int32_t kEventMaxAge = 30 * 1000;  // events waiting through a conversation expire

ClaudeStatus& GetClaudeStatus()
{
    // About 1 KB: keep it out of the scarce internal RAM. Never touched from an ISR or with the cache
    // disabled, so PSRAM is fine; it is never destroyed.
    static ClaudeStatus* status = []() {
        void* memory = heap_caps_malloc(sizeof(ClaudeStatus), MALLOC_CAP_SPIRAM);
        return memory ? new (memory) ClaudeStatus() : new ClaudeStatus();
    }();
    return *status;
}

void ClaudeStatus::setSessions(const Sessions& sessions, uint32_t now)
{
    std::lock_guard<std::mutex> lock(_mutex);
    for (int i = 0; i < kSlots; ++i) {
        const auto& in       = sessions[i];
        auto& cur            = _sessions[i];
        const bool same      = cur.used && in.used && cur.st == in.st && std::strcmp(cur.name, in.name) == 0;
        const uint32_t since = same ? cur.stSinceMs : now;
        cur                  = in;
        cur.stSinceMs        = since;
    }
}

void ClaudeStatus::clear()
{
    std::lock_guard<std::mutex> lock(_mutex);
    _sessions    = Sessions{};
    _event_count = 0;
}

void ClaudeStatus::pushEvent(const ClaudeEvent& event)
{
    std::lock_guard<std::mutex> lock(_mutex);
    if (_event_count == kMaxEvents) {
        _event_head = (_event_head + 1) % kMaxEvents;  // drop the oldest
        --_event_count;
    }
    _events[(_event_head + _event_count) % kMaxEvents] = event;
    ++_event_count;
}

void ClaudeStatus::touch(uint32_t now)
{
    std::lock_guard<std::mutex> lock(_mutex);
    _last_frame_ms = now;
    _ever_touched  = true;
}

int ClaudeStatus::usedCount()
{
    std::lock_guard<std::mutex> lock(_mutex);
    int n = 0;
    for (const auto& s : _sessions) {
        n += s.used ? 1 : 0;
    }
    return n;
}

void ClaudeStatus::drop_if_stale(uint32_t now)
{
    if (_ever_touched && age_ms(now, _last_frame_ms) > kStaleMs) {
        _sessions     = Sessions{};
        _event_count  = 0;
        _ever_touched = false;  // stays empty until the agent talks again
    }
}

bool ClaudeStatus::snapshot(Sessions& out, uint32_t now)
{
    std::lock_guard<std::mutex> lock(_mutex);
    drop_if_stale(now);
    bool any = false;
    for (const auto& s : _sessions) {
        any = any || s.used;
    }
    out = _sessions;
    return any;
}

bool ClaudeStatus::popEvent(ClaudeEvent& out, uint32_t now)
{
    std::lock_guard<std::mutex> lock(_mutex);
    drop_if_stale(now);
    while (_event_count > 0) {
        out         = _events[_event_head];
        _event_head = (_event_head + 1) % kMaxEvents;
        --_event_count;
        if (age_ms(now, out.ms) <= kEventMaxAge) {  // also true for a stamp newer than now
            return true;
        }
    }
    return false;
}

}  // namespace stackchan::sc
