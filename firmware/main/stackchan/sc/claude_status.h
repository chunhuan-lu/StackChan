/*
 * SPDX-FileCopyrightText: 2026 chunhuan-lu/StackChan contributors
 *
 * SPDX-License-Identifier: MIT
 */
#pragma once
#include <array>
#include <cstdint>
#include <mutex>

namespace stackchan::sc {

/**
 * @brief Claude Code session state pushed by the PC agent (REQ-003, USB link protocol v1).
 *
 * Written by the USB link task, read by the idle director in the LVGL context. Lives as a
 * singleton because the director is destroyed and re-created around every conversation.
 * Time stamps come from GetHAL().millis() in two tasks, so ages are compared as signed
 * differences (a writer stamp may be newer than the reader's cached "now").
 */
enum class ClaudeSt : uint8_t { Idle, Work, Wait, Done, Err };
enum class ClaudeEv : uint8_t { Needs, Done, Fail, Err, Compact, Pass };
// Protocol v1.1: what a working session is doing right now.
enum class ClaudeAct : uint8_t { None, Think, Edit, Run, Read, Web, Test, Agents };

struct ClaudeSession {
    bool used          = false;
    ClaudeSt st        = ClaudeSt::Idle;
    ClaudeAct act      = ClaudeAct::None;
    uint8_t ctx        = 0;   // context window usage in percent (0 = unknown)
    char name[52]      = {};  // UTF-8, at most 12 code points
    uint32_t stSinceMs = 0;   // when st last changed
};

struct ClaudeEvent {
    ClaudeEv ev    = ClaudeEv::Done;
    int slot       = 0;
    char text[164] = {};  // UTF-8, at most 40 code points
    bool sound     = false;
    uint32_t ms    = 0;  // arrival time
};

// Signed age of a millis() stamp; negative when the stamp is newer than now.
inline int32_t age_ms(uint32_t now, uint32_t stamp)
{
    return static_cast<int32_t>(now - stamp);
}

class ClaudeStatus {
public:
    static constexpr int kSlots     = 5;
    static constexpr int kMaxEvents = 4;
    using Sessions                  = std::array<ClaudeSession, kSlots>;

    // Writer side (USB link task)
    void setSessions(const Sessions& sessions, uint32_t now);
    void clear();
    void pushEvent(const ClaudeEvent& event);
    void touch(uint32_t now);  // any valid frame from the agent
    int usedCount();

    // Reader side (idle director). snapshot() returns false when there is no session or the agent
    // has been silent for too long (the sessions are dropped then).
    bool snapshot(Sessions& out, uint32_t now);
    bool popEvent(ClaudeEvent& out, uint32_t now);

private:
    void drop_if_stale(uint32_t now);

    std::mutex _mutex;
    Sessions _sessions{};
    std::array<ClaudeEvent, kMaxEvents> _events{};  // ring buffer, no heap
    int _event_head         = 0;
    int _event_count        = 0;
    uint32_t _last_frame_ms = 0;
    bool _ever_touched      = false;
};

ClaudeStatus& GetClaudeStatus();

}  // namespace stackchan::sc
