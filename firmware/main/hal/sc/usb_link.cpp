/*
 * SPDX-FileCopyrightText: 2026 chunhuan-lu/StackChan contributors
 *
 * SPDX-License-Identifier: MIT
 */
#include "usb_link.h"
#include <hal/hal.h>
#include <hal/board/hal_bridge.h>
#include <stackchan/sc/claude_status.h>
#include <ArduinoJson.hpp>
#include <driver/usb_serial_jtag.h>
#include <esp_app_desc.h>
#include <esp_heap_caps.h>
#include <esp_log.h>
#include <esp_random.h>
#include <esp_rom_crc.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <fcntl.h>
#include <unistd.h>
#include <algorithm>
#include <cinttypes>
#include <cstdio>
#include <cstring>

namespace sc::usb_link {

using stackchan::sc::ClaudeAct;
using stackchan::sc::ClaudeEv;
using stackchan::sc::ClaudeEvent;
using stackchan::sc::ClaudeSt;
using stackchan::sc::ClaudeStatus;
using stackchan::sc::GetClaudeStatus;

static const char* TAG = "USB-Link";

static constexpr size_t kMaxLine    = 1024;  // longer lines are dropped
static constexpr size_t kMaxJsonIn  = 768;
static constexpr size_t kMaxJsonOut = 256;
static constexpr int kVersion       = 1;
static constexpr int kNameChars     = 12;
static constexpr int kTextChars     = 40;

// Keeps ArduinoJson's per-frame allocations out of the scarce internal RAM (small mallocs would go
// there first with SPIRAM_MALLOC_ALWAYSINTERNAL).
struct PsramAllocator : ArduinoJson::Allocator {
    void* allocate(size_t size) override
    {
        return heap_caps_malloc(size, MALLOC_CAP_SPIRAM);
    }
    void deallocate(void* ptr) override
    {
        heap_caps_free(ptr);
    }
    void* reallocate(void* ptr, size_t new_size) override
    {
        return heap_caps_realloc(ptr, new_size, MALLOC_CAP_SPIRAM);
    }
};
static PsramAllocator s_allocator;

static int s_tx_fd          = -1;
static uint32_t s_boot_id   = 0;
static bool s_have_last_id  = false;
static uint32_t s_last_id   = 0;
static char* s_line         = nullptr;  // PSRAM line buffer
static size_t s_line_len    = 0;
static bool s_line_overflow = false;
static char s_logged_state[160] = {};

// Copies at most max_cp UTF-8 code points into dst (always terminated); drops control characters and
// malformed sequences.
static void copy_utf8(char* dst, size_t dst_size, const char* src, int max_cp)
{
    size_t out = 0;
    int cps    = 0;
    for (const auto* p = reinterpret_cast<const uint8_t*>(src); *p && cps < max_cp;) {
        const uint8_t c = *p;
        const size_t n  = c < 0x80 ? 1 : (c & 0xE0) == 0xC0 ? 2 : (c & 0xF0) == 0xE0 ? 3 : (c & 0xF8) == 0xF0 ? 4 : 0;
        bool valid      = n > 0;
        for (size_t i = 1; valid && i < n; ++i) {
            valid = (p[i] & 0xC0) == 0x80;
        }
        if (!valid) {
            ++p;
            continue;
        }
        if (n == 1 && (c < 0x20 || c == 0x7F)) {
            ++p;
            continue;
        }
        if (out + n >= dst_size) {
            break;
        }
        std::memcpy(dst + out, p, n);
        out += n;
        p += n;
        ++cps;
    }
    dst[out] = '\0';
}

static bool parse_st(const char* s, ClaudeSt& st)
{
    static constexpr struct {
        const char* name;
        ClaudeSt st;
    } kMap[] = {{"idle", ClaudeSt::Idle}, {"work", ClaudeSt::Work}, {"wait", ClaudeSt::Wait},
                {"done", ClaudeSt::Done}, {"err", ClaudeSt::Err}};
    for (const auto& m : kMap) {
        if (std::strcmp(s, m.name) == 0) {
            st = m.st;
            return true;
        }
    }
    return false;
}

static ClaudeAct parse_act(const char* s)
{
    static constexpr struct {
        const char* name;
        ClaudeAct act;
    } kMap[] = {{"think", ClaudeAct::Think}, {"edit", ClaudeAct::Edit}, {"run", ClaudeAct::Run},
                {"read", ClaudeAct::Read},   {"web", ClaudeAct::Web},   {"test", ClaudeAct::Test},
                {"agents", ClaudeAct::Agents}};
    for (const auto& m : kMap) {
        if (std::strcmp(s, m.name) == 0) {
            return m.act;
        }
    }
    return ClaudeAct::None;  // absent, empty or unknown
}

static bool parse_ev(const char* s, ClaudeEv& ev)
{
    static constexpr struct {
        const char* name;
        ClaudeEv ev;
    } kMap[] = {{"needs", ClaudeEv::Needs}, {"done", ClaudeEv::Done},       {"fail", ClaudeEv::Fail},
                {"err", ClaudeEv::Err},     {"compact", ClaudeEv::Compact}, {"pass", ClaudeEv::Pass}};
    for (const auto& m : kMap) {
        if (std::strcmp(s, m.name) == 0) {
            ev = m.ev;
            return true;
        }
    }
    return false;
}

static const char* robot_state()
{
    if (!hal_bridge::is_xiaozhi_ready()) {
        return "other";
    }
    return hal_bridge::is_xiaozhi_idle() ? "idle" : "talk";
}

// One write() per frame: the VFS write lock keeps it in one piece next to log lines.
static void send_json(const ArduinoJson::JsonDocument& doc)
{
    if (s_tx_fd < 0 || !usb_serial_jtag_is_connected() || ArduinoJson::measureJson(doc) > kMaxJsonOut) {
        return;
    }
    char json[kMaxJsonOut + 1];
    const size_t n = ArduinoJson::serializeJson(doc, json, sizeof(json));
    if (n == 0) {
        return;
    }
    char frame[kMaxJsonOut + 16];
    const uint32_t crc = esp_rom_crc32_le(0, reinterpret_cast<const uint8_t*>(json), n);
    const int len      = snprintf(frame, sizeof(frame), "@@sc%.*s*%08" PRIX32 "\n", static_cast<int>(n), json, crc);
    if (len > 0 && static_cast<size_t>(len) < sizeof(frame)) {
        write(s_tx_fd, frame, len);
    }
}

// boot and n go into every ack, so the agent notices a restart that kept the USB port open and a
// device-side drop of the sessions (staleness) without waiting for a failed request.
static void send_ack(uint32_t id, const char* err, bool hello)
{
    ArduinoJson::JsonDocument doc(&s_allocator);
    doc["t"]  = "ack";
    doc["id"] = id;
    doc["ok"] = err == nullptr;
    if (err) {
        doc["err"] = err;
    }
    doc["st"]   = robot_state();
    doc["boot"] = s_boot_id;
    doc["n"]    = GetClaudeStatus().usedCount();
    if (hello) {
        doc["v"]  = kVersion;
        doc["fw"] = esp_app_get_description()->version;
        doc["up"] = static_cast<uint32_t>(esp_timer_get_time() / 1000000);
    }
    send_json(doc);
}

static const char* handle_state(const ArduinoJson::JsonDocument& doc, uint32_t now)
{
    if (!doc["sessions"].is<ArduinoJson::JsonArrayConst>()) {
        return "sessions";
    }
    ClaudeStatus::Sessions sessions{};
    char summary[sizeof(s_logged_state)] = {};
    size_t used                          = 0;
    for (ArduinoJson::JsonObjectConst s : doc["sessions"].as<ArduinoJson::JsonArrayConst>()) {
        const int slot = s["slot"] | -1;
        ClaudeSt st;
        if (slot < 0 || slot >= ClaudeStatus::kSlots || !parse_st(s["st"] | "", st)) {
            continue;
        }
        auto& out = sessions[slot];
        out.used  = true;
        out.st    = st;
        out.act   = parse_act(s["act"] | "");
        out.ctx   = static_cast<uint8_t>(std::clamp(s["ctx"] | 0, 0, 100));
        copy_utf8(out.name, sizeof(out.name), s["name"] | "", kNameChars);
        const int w = snprintf(summary + used, sizeof(summary) - used, "[%d %s %s]", slot, out.name, s["st"] | "");
        used        = w > 0 ? std::min(sizeof(summary) - 1, used + static_cast<size_t>(w)) : used;
    }
    GetClaudeStatus().setSessions(sessions, now);
    if (std::strcmp(summary, s_logged_state) != 0) {
        std::strcpy(s_logged_state, summary);
        ESP_LOGI(TAG, "claude sessions: %s", summary[0] ? summary : "none");
    }
    return nullptr;
}

static const char* handle_event(const ArduinoJson::JsonDocument& doc, uint32_t now)
{
    ClaudeEvent event;
    if (!parse_ev(doc["ev"] | "", event.ev)) {
        return "ev";
    }
    const int slot = doc["slot"] | 0;
    event.slot     = slot >= 0 && slot < ClaudeStatus::kSlots ? slot : 0;
    copy_utf8(event.text, sizeof(event.text), doc["text"] | "", kTextChars);
    event.sound = doc["snd"] | false;
    event.ms    = now;
    GetClaudeStatus().pushEvent(event);
    ESP_LOGI(TAG, "claude event: %s (%s)", doc["ev"] | "", event.text);
    return nullptr;
}

static void dispatch(const ArduinoJson::JsonDocument& doc)
{
    const uint32_t now = GetHAL().millis();
    GetClaudeStatus().touch(now);

    const char* type = doc["t"] | "";
    const uint32_t id = doc["id"] | 0u;
    const bool hello  = std::strcmp(type, "hello") == 0;
    if (hello) {
        s_have_last_id = false;  // a new agent run starts its ids again
    } else if (s_have_last_id && id == s_last_id) {
        send_ack(id, nullptr, false);  // retry of a message that was already applied
        return;
    }
    s_have_last_id = true;
    s_last_id      = id;

    const char* err = nullptr;
    if (hello) {
        ESP_LOGI(TAG, "agent connected: %s", doc["agent"] | "?");
    } else if (std::strcmp(type, "ping") == 0) {
        // keepalive only
    } else if (std::strcmp(type, "state") == 0) {
        err = handle_state(doc, now);
    } else if (std::strcmp(type, "event") == 0) {
        err = handle_event(doc, now);
    } else if (std::strcmp(type, "clear") == 0) {
        GetClaudeStatus().clear();
        s_logged_state[0] = '\0';
    } else {
        err = "type";
    }
    send_ack(id, err, hello);
}

static int hex_value(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return -1;
}

// "@@sc<json>*<crc>" may follow log text in the same line; a stale fragment can precede a fresh frame,
// so the last marker counts.
static void handle_line(const char* line, size_t len)
{
    const char* start = nullptr;
    for (size_t i = 0; i + 4 <= len; ++i) {
        if (std::memcmp(line + i, "@@sc", 4) == 0) {
            start = line + i;
        }
    }
    if (!start) {
        return;
    }
    const char* json = start + 4;
    const char* end  = line + len;
    while (end > json && (end[-1] == '\r' || end[-1] == ' ')) {
        --end;
    }
    if (end - json < 10 || end[-9] != '*') {
        return;
    }
    uint32_t crc = 0;
    for (const char* p = end - 8; p < end; ++p) {
        const int v = hex_value(*p);
        if (v < 0) {
            return;
        }
        crc = crc << 4 | static_cast<uint32_t>(v);
    }
    const size_t json_len = static_cast<size_t>(end - 9 - json);
    if (json_len == 0 || json_len > kMaxJsonIn ||
        esp_rom_crc32_le(0, reinterpret_cast<const uint8_t*>(json), json_len) != crc) {
        return;
    }
    ArduinoJson::JsonDocument doc(&s_allocator);
    if (ArduinoJson::deserializeJson(doc, json, json_len) != ArduinoJson::DeserializationError::Ok) {
        return;
    }
    dispatch(doc);
}

// Runs from a PSRAM stack: no flash or NVS operations in here or in anything it calls.
static void link_task(void*)
{
    s_tx_fd = open("/dev/secondary", O_WRONLY);
    if (s_tx_fd < 0) {
        ESP_LOGW(TAG, "/dev/secondary not available, replies disabled");
    }
    ESP_LOGI(TAG, "ready (protocol v%d)", kVersion);

    uint8_t buffer[64];
    for (;;) {
        const int n = usb_serial_jtag_read_bytes(buffer, sizeof(buffer), pdMS_TO_TICKS(100));
        for (int i = 0; i < n; ++i) {
            const char c = static_cast<char>(buffer[i]);
            if (c == '\n') {
                if (!s_line_overflow) {
                    handle_line(s_line, s_line_len);
                }
                s_line_len      = 0;
                s_line_overflow = false;
            } else if (s_line_len < kMaxLine) {
                s_line[s_line_len++] = c;
            } else {
                s_line_overflow = true;
            }
        }
    }
}

void start()
{
    static bool started = false;
    if (started) {
        return;
    }
    started   = true;
    s_boot_id = esp_random();

    // Install here rather than in the task, so a failure leaves nothing behind. The RX interrupt only
    // fills the ring buffer; the log output keeps its own direct path.
    usb_serial_jtag_driver_config_t config = {};
    config.tx_buffer_size                  = 64;  // unused: replies go through the VFS like the log
    config.rx_buffer_size                  = 1024;
    if (usb_serial_jtag_driver_install(&config) != ESP_OK) {
        ESP_LOGE(TAG, "driver install failed, link disabled");
        return;
    }
    s_line = static_cast<char*>(heap_caps_malloc(kMaxLine, MALLOC_CAP_SPIRAM));
    if (!s_line ||
        xTaskCreatePinnedToCoreWithCaps(link_task, "sc_usb", 6144, nullptr, 2, nullptr, 1, MALLOC_CAP_SPIRAM) !=
            pdPASS) {
        ESP_LOGE(TAG, "start failed");
    }
}

}  // namespace sc::usb_link
