/*
 * SPDX-FileCopyrightText: 2026 chunhuan-lu/StackChan contributors
 *
 * SPDX-License-Identifier: MIT
 */
#include "face_probe.h"
#include <sdkconfig.h>

#if CONFIG_SC_FACE_PROBE
#include <hal/hal.h>
#include <hal/board/hal_bridge.h>
#include <hal/board/cores3_audio_codec.h>
#include <stackchan/sc/sc_settings.h>
#include "human_face_detect.hpp"
#include <esp_heap_caps.h>
#include <esp_log.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <lvgl.h>
#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <mutex>

namespace sc::face_probe {

using stackchan::sc::setting;
using stackchan::sc::SettingId;

static const char* TAG = "FaceProbe";

static constexpr uint32_t kIdleWaitMs = 500;  // polling while nobody wants detections
static constexpr uint32_t kPollMs     = 20;
// Detection slows the wake word down (PSRAM and cache contention): start one only when the microphone buffer has been
// read down to this, and leave at least kMinGapMs between two even when the director asks for one.
static constexpr uint32_t kMaxBacklogMs = 80;
static constexpr uint32_t kMinGapMs     = 300;
static constexpr uint32_t kWantedMs     = 2000;
// The DVP camera has one buffer: the frame waiting in it was taken right after the previous grab. Older than this,
// it is dropped and the next one taken; a frame takes 50 ms at 20 fps.
static constexpr uint32_t kFreshMs   = 100;
static constexpr uint32_t kFrameMs   = 50;
static constexpr int64_t kStatsUs    = 30 * 1000 * 1000;
static constexpr int kDarkLuma       = 40;   // mean luma below this: too dark to find a face (black level is ~28)
static constexpr double kFlatVar     = 30;   // both byte lanes vary less: a flat frame (black level), no detection
static constexpr bool kMirror        = true;  // the camera looks at the user: draw the box like a mirror

static std::mutex s_mutex;
static Sighting s_latest;
static bool s_have_latest = false;
static std::atomic<uint32_t> s_wanted_at{0};
static std::atomic<bool> s_check{false};

static lv_obj_t* s_box   = nullptr;
static lv_obj_t* s_label = nullptr;

bool latest(Sighting& out)
{
    std::lock_guard<std::mutex> lock(s_mutex);
    if (!s_have_latest) {
        return false;
    }
    out = s_latest;
    return true;
}

void keep_running()
{
    s_wanted_at = GetHAL().millis() | 1;  // 0 = never
}

void request_check()
{
    s_check = true;
}

static bool wanted(uint32_t now)
{
    const uint32_t at = s_wanted_at;  // may be a millisecond ahead of now (| 1, or written after now was read)
    return at != 0 && static_cast<int32_t>(now - at) < static_cast<int32_t>(kWantedMs);
}

static void publish(const Sighting& s)
{
    std::lock_guard<std::mutex> lock(s_mutex);
    s_latest      = s;
    s_have_latest = true;
}

static inline uint8_t clamp8(int v)
{
    return static_cast<uint8_t>(v < 0 ? 0 : v > 255 ? 255 : v);
}

// Packed YUV 4:2:2 (GC0308: [Y Cb Y Cr], or [Cb Y Cr Y] with the byte swap option) to RGB888 at half size: one
// output pixel per 2x2 block (one YUYV group of every other row), JPEG (full range) coefficients in 8-bit fixed
// point. 160 x 120 is the first-stage model's input size, so the detector does not resize.
static void yuv422_to_rgb888_half(const uint8_t* src, uint8_t* dst, int w, int h, bool y_first)
{
    for (int y = 0; y < h; y += 2) {
        const uint8_t* p = src + static_cast<size_t>(y) * w * 2;
        for (int x = 0; x < w; x += 2, p += 4) {
            const int yy = y_first ? (p[0] + p[2] + 1) >> 1 : (p[1] + p[3] + 1) >> 1;
            const int u  = (y_first ? p[1] : p[0]) - 128;
            const int v  = (y_first ? p[3] : p[2]) - 128;
            *dst++       = clamp8(yy + ((359 * v) >> 8));
            *dst++       = clamp8(yy - ((88 * u + 183 * v) >> 8));
            *dst++       = clamp8(yy + ((454 * u) >> 8));
        }
    }
}

// Mean and variance of the even and the odd bytes, from every 64th byte pair. Luma varies across the picture, chroma
// stays near 128: the lane with the larger variance holds Y. A flat frame (dark room, covered lens) has neither.
struct FrameLook {
    double mean[2];
    double var[2];
};

static FrameLook look_at(const uint8_t* src, size_t len)
{
    double sum[2] = {0, 0}, sq[2] = {0, 0};
    size_t n      = 0;
    for (size_t i = 0; i + 1 < len; i += 64, ++n) {
        for (int k = 0; k < 2; ++k) {
            sum[k] += src[i + k];
            sq[k] += static_cast<double>(src[i + k]) * src[i + k];
        }
    }
    FrameLook look{};
    for (int k = 0; k < 2 && n > 0; ++k) {
        look.mean[k] = sum[k] / n;
        look.var[k]  = sq[k] / n - look.mean[k] * look.mean[k];
    }
    return look;
}

static void create_overlay()
{
    LvglLockGuard lock;
    s_box = lv_obj_create(lv_layer_top());
    lv_obj_remove_style_all(s_box);
    lv_obj_remove_flag(s_box, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(s_box, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_border_color(s_box, lv_color_hex(0x00E060), 0);
    lv_obj_set_style_border_width(s_box, 3, 0);
    lv_obj_set_style_border_opa(s_box, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(s_box, 4, 0);
    lv_obj_add_flag(s_box, LV_OBJ_FLAG_HIDDEN);

    s_label = lv_label_create(lv_layer_top());
    lv_obj_set_style_text_color(s_label, lv_color_hex(0x00E060), 0);
    lv_obj_set_style_text_font(s_label, &lv_font_montserrat_14, 0);
    lv_obj_add_flag(s_label, LV_OBJ_FLAG_HIDDEN);
}

// Debug overlay (setting "face_box"): the face box in screen coordinates, mirrored.
static void show_box(bool found, int x1, int y1, int x2, int y2, int w, float score, int det_ms)
{
    LvglLockGuard lock;
    if (!found) {
        lv_obj_add_flag(s_box, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_label, LV_OBJ_FLAG_HIDDEN);
        return;
    }
    if (kMirror) {
        const int l = w - 1 - x2, r = w - 1 - x1;
        x1 = l;
        x2 = r;
    }
    lv_obj_set_pos(s_box, x1, y1);
    lv_obj_set_size(s_box, std::max(4, x2 - x1), std::max(4, y2 - y1));
    lv_obj_remove_flag(s_box, LV_OBJ_FLAG_HIDDEN);
    char text[32];
    snprintf(text, sizeof(text), "%.2f  %d ms", score, det_ms);
    lv_label_set_text(s_label, text);
    lv_obj_set_pos(s_label, x1, std::min(y2 + 4, 222));
    lv_obj_remove_flag(s_label, LV_OBJ_FLAG_HIDDEN);
}

static void log_memory(const char* when)
{
    ESP_LOGI(TAG, "%s: internal free %u (min %u), PSRAM free %u, stack left %u", when,
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM), (unsigned)uxTaskGetStackHighWaterMark(nullptr));
}

// CPU time of the wake word task and of both idle tasks, to see what detection takes away from the wake word.
struct CpuSample {
    int64_t at;
    uint32_t audio, idle0, idle1;
    uint32_t overflows;  // microphone audio lost, in 10 ms chunks
};

static CpuSample cpu_sample()
{
    static constexpr UBaseType_t kMaxTasks = 48;
    static auto* tasks = static_cast<TaskStatus_t*>(heap_caps_malloc(kMaxTasks * sizeof(TaskStatus_t), MALLOC_CAP_SPIRAM));
    CpuSample s{esp_timer_get_time(), 0, 0, 0, cores3_audio_rx_overflows()};
    if (!tasks) {
        return s;
    }
    uint32_t total      = 0;
    const UBaseType_t n = uxTaskGetSystemState(tasks, kMaxTasks, &total);
    for (UBaseType_t i = 0; i < n; ++i) {
        const char* name = tasks[i].pcTaskName;
        if (strcmp(name, "audio_input") == 0) {
            s.audio = tasks[i].ulRunTimeCounter;
        } else if (strcmp(name, "IDLE0") == 0) {
            s.idle0 = tasks[i].ulRunTimeCounter;
        } else if (strcmp(name, "IDLE1") == 0) {
            s.idle1 = tasks[i].ulRunTimeCounter;
        }
    }
    return s;
}

struct Stats {
    CpuSample from{};
    int frames = 0, faces = 0, held = 0, dark = 0;
    int64_t sum_det = 0, max_det = 0;
    uint32_t max_backlog = 0;

    void log(const CpuSample& to)
    {
        const double us = static_cast<double>(to.at - from.at);
        ESP_LOGI(TAG,
                 "%d detections in %.0f s (%d frames dark or flat), face in %d, detect avg %d ms (max %d); audio_input "
                 "%.0f%%, idle core 0 %.0f%%, core 1 %.0f%%; mic lost %u x 10 ms, backlog max %u ms, held back %d x",
                 frames, us / 1e6, dark, faces, frames ? static_cast<int>(sum_det / frames / 1000) : 0,
                 static_cast<int>(max_det / 1000), (to.audio - from.audio) * 100.0 / us,
                 (to.idle0 - from.idle0) * 100.0 / us, (to.idle1 - from.idle1) * 100.0 / us,
                 (unsigned)(to.overflows - from.overflows), (unsigned)max_backlog, held);
        *this = Stats{};
        from  = to;
    }
};

static void probe_task(void*)
{
    create_overlay();
    log_memory("start");

    HumanFaceDetect* detect = nullptr;
    uint8_t* rgb            = nullptr;
    size_t rgb_size         = 0;
    int lane                = 1;  // luma in the even bytes (GC0308 here), until a clear frame says otherwise
    bool lane_known         = false;
    bool first_frame        = true;
    uint32_t seq            = 0;
    uint32_t last_grab      = 0;  // end of the last frame grab
    uint32_t last_detect    = 0;  // end of the last detection
    bool box_shown          = false;
    int rate_logged         = -1;

    Stats stats;
    stats.from = cpu_sample();
    while (true) {
        if (esp_timer_get_time() - stats.from.at >= kStatsUs) {
            stats.log(cpu_sample());
        }
        const int rate     = setting(SettingId::FaceProbeRate);
        const bool box_on  = setting(SettingId::FaceBox) != 0;
        const uint32_t now = GetHAL().millis();
        if (rate != rate_logged) {
            ESP_LOGI(TAG, "rate %d/s", rate);
            rate_logged = rate;
        }
        if (box_shown && !box_on) {
            show_box(false, 0, 0, 0, 0, 0, 0, 0);
            box_shown = false;
        }
        if (rate <= 0 || !wanted(now) || !hal_bridge::is_xiaozhi_ready() || !hal_bridge::is_xiaozhi_idle()) {
            if (box_shown) {
                show_box(false, 0, 0, 0, 0, 0, 0, 0);
                box_shown = false;
            }
            s_check = false;
            vTaskDelay(pdMS_TO_TICKS(kIdleWaitMs));
            continue;
        }
        // Next detection: at the rate, or on request (the head has settled), but never back to back.
        const uint32_t since = now - last_detect;
        if (since < 1000u / static_cast<uint32_t>(rate) && !(s_check && since >= kMinGapMs)) {
            vTaskDelay(pdMS_TO_TICKS(kPollMs));
            continue;
        }
        if (cores3_audio_rx_backlog_ms() > kMaxBacklogMs) {
            ++stats.held;
            vTaskDelay(pdMS_TO_TICKS(kPollMs));
            continue;
        }
        s_check = false;

        auto* camera = hal_bridge::board_get_camera();
        if (!camera) {
            ESP_LOGW(TAG, "no camera, stopping");
            break;
        }
        if (now - last_grab > kFreshMs) {
            camera->StreamCaptures();  // drop the frame that waited in the buffer
        }
        const bool grabbed = camera->StreamCaptures();
        last_grab          = GetHAL().millis();
        if (!grabbed) {
            vTaskDelay(pdMS_TO_TICKS(kIdleWaitMs));
            continue;
        }
        const uint32_t taken = last_grab - kFrameMs;
        const uint8_t* yuv   = camera->GetFrameData();
        const int w = camera->GetFrameWidth(), h = camera->GetFrameHeight();
        const size_t len = camera->GetFrameSize();
        if (!yuv || w <= 0 || h <= 0 || len < static_cast<size_t>(w) * h * 2) {
            ESP_LOGW(TAG, "unexpected frame: %dx%d, %u bytes, format 0x%08x", w, h, (unsigned)len,
                     camera->GetFrameFormat());
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }
        const FrameLook look = look_at(yuv, len);
        const double var_max = std::max(look.var[0], look.var[1]);
        if (first_frame) {
            first_frame = false;
            ESP_LOGI(TAG, "frames: %dx%d, %u bytes, format 0x%08x; even bytes mean %.0f var %.0f, odd %.0f / %.0f", w, h,
                     (unsigned)len, camera->GetFrameFormat(), look.mean[0], look.var[0], look.mean[1], look.var[1]);
        }
        if (!lane_known && var_max >= 100 && var_max >= 2 * std::min(look.var[0], look.var[1])) {
            lane       = look.var[0] >= look.var[1] ? 1 : 0;
            lane_known = true;
            ESP_LOGI(TAG, "luma in the %s bytes (variance %.0f / %.0f)", lane == 1 ? "even" : "odd", look.var[0],
                     look.var[1]);
        }
        if (var_max < kFlatVar || look.mean[lane == 1 ? 0 : 1] < kDarkLuma) {
            Sighting dark;  // nothing to find: publish "no face" without running the detector
            dark.seq    = ++seq;
            dark.at     = taken;
            last_detect = GetHAL().millis();
            publish(dark);
            ++stats.dark;
            continue;
        }
        const int dw = w / 2, dh = h / 2;  // detection size
        if (rgb_size < static_cast<size_t>(dw) * dh * 3) {
            heap_caps_free(rgb);
            rgb_size = static_cast<size_t>(dw) * dh * 3;
            rgb      = static_cast<uint8_t*>(heap_caps_aligned_alloc(16, rgb_size, MALLOC_CAP_SPIRAM));
            if (!rgb) {
                ESP_LOGE(TAG, "no memory for the RGB frame");
                break;
            }
        }
        yuv422_to_rgb888_half(yuv, rgb, w, h, lane == 1);

        if (!detect) {
            detect = new HumanFaceDetect();
        }
        const int64_t t0     = esp_timer_get_time();
        dl::image::img_t img = {rgb, static_cast<uint16_t>(dw), static_cast<uint16_t>(dh),
                                dl::image::DL_IMAGE_PIX_TYPE_RGB888};
        auto& results    = detect->run(img);
        const int64_t t1 = esp_timer_get_time();
        last_detect      = GetHAL().millis();
        stats.max_backlog = std::max(stats.max_backlog, cores3_audio_rx_backlog_ms());
        static bool loaded_logged = false;
        if (!loaded_logged) {
            loaded_logged = true;
            log_memory("model loaded");
        }

        // The biggest face is the closest one (boxes in detection size).
        const dl::detect::result_t* best = nullptr;
        for (const auto& r : results) {
            if (r.box.size() >= 4 && (!best || r.box_area() > best->box_area())) {
                best = &r;
            }
        }
        Sighting s;
        s.seq = ++seq;
        s.at  = taken;
        if (best) {
            const int x1 = std::clamp(best->box[0], 0, dw), y1 = std::clamp(best->box[1], 0, dh);
            const int x2 = std::clamp(best->box[2], 0, dw), y2 = std::clamp(best->box[3], 0, dh);
            s.found = true;
            s.cx    = (x1 + x2) * 0.5f / dw;
            s.cy    = (y1 + y2) * 0.5f / dh;
            s.size  = static_cast<float>(x2 - x1) / dw;
            s.score = best->score;
            ++stats.faces;
        }
        publish(s);

        const int det_ms = static_cast<int>((t1 - t0) / 1000);
        if (box_on && best) {
            show_box(true, best->box[0] * 2, best->box[1] * 2, best->box[2] * 2, best->box[3] * 2, w, best->score,
                     det_ms);
            box_shown = true;
        } else if (box_shown) {
            show_box(false, 0, 0, 0, 0, 0, 0, 0);
            box_shown = false;
        }
        ++stats.frames;
        stats.sum_det += t1 - t0;
        stats.max_det = std::max(stats.max_det, t1 - t0);
    }
    delete detect;
    heap_caps_free(rgb);
    vTaskDelete(nullptr);
}

void start()
{
    static bool started = false;
    if (started) {
        return;
    }
    started = true;
    esp_log_level_set("StackChanCamera", ESP_LOG_ERROR);  // it warns about the buffer size on every frame
    // Core 1 at the lowest priority: the UI preempts it and the wake word (core 0) is not touched.
    if (xTaskCreatePinnedToCoreWithCaps(probe_task, "face_probe", 12 * 1024, nullptr, 1, nullptr, 1,
                                        MALLOC_CAP_SPIRAM) != pdPASS) {
        ESP_LOGE(TAG, "task start failed");
    }
}

}  // namespace sc::face_probe

#else  // CONFIG_SC_FACE_PROBE

namespace sc::face_probe {

void start() {}
bool latest(Sighting&)
{
    return false;
}
void keep_running() {}
void request_check() {}

}  // namespace sc::face_probe

#endif  // CONFIG_SC_FACE_PROBE
