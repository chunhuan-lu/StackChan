/*
 * SPDX-FileCopyrightText: 2026 chunhuan-lu/StackChan contributors
 *
 * SPDX-License-Identifier: MIT
 */
#include "face_probe.h"
#include "frame_cues.h"
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
#include <cmath>
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
// Between detections: the last face's patch is looked for this often while it is younger than kTrackHoldMs (a
// detection renews it), and motion this often while the director searches. Lighter than a detection, but they still
// wait for the wake word when its backlog passes kCueBacklogMs.
static constexpr uint32_t kTrackPeriodMs  = 250;
static constexpr uint32_t kTrackHoldMs    = 4000;
static constexpr uint32_t kMotionPeriodMs = 400;
static constexpr uint32_t kCueBacklogMs   = 400;  // cue frames read a quarter of a frame in place: light
static constexpr uint32_t kHeadNoteMs     = 500;  // older head angles are unknown
// While a face is being tracked, the patch gives the position and a detection only renews it: at most this often.
static constexpr uint32_t kTrackingDetectMs = 1500;
// Detector thresholds (two stages, MSR and MNP; the component's default is 0.5 for both). Lower, the detector also
// finds faces turned aside or looking down; below kStrongScore a face renews the template only near the tracked
// patch, and the director takes it only near where the person is.
static constexpr float kMsrScore  = 0.3f;
static constexpr float kMnpScore  = 0.35f;
static constexpr float kNearPatch = 0.15f;  // of the frame
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
static std::atomic<uint32_t> s_search_at{0};
static std::atomic<bool> s_check{false};

struct HeadNote {
    uint32_t at = 0;
    int yaw = 0, pitch = 0;
    bool moving = true;
};
static HeadNote s_head;  // under s_mutex

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

void keep_running(bool search)
{
    const uint32_t now = GetHAL().millis() | 1;  // 0 = never
    s_wanted_at        = now;
    if (search) {
        s_search_at = now;
    }
}

void note_head(uint32_t at, int yaw, int pitch, bool moving)
{
    std::lock_guard<std::mutex> lock(s_mutex);
    s_head = {at, yaw, pitch, moving};
}

static HeadNote head_note()
{
    std::lock_guard<std::mutex> lock(s_mutex);
    return s_head;
}

void request_check()
{
    s_check = true;
}

// at may be a millisecond ahead of now (| 1, or written after now was read).
static bool recent(uint32_t at, uint32_t now)
{
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

// Debug overlay (setting "face_box"): the face box in screen coordinates, mirrored; green for a detected face,
// yellow for its tracked patch.
static void show_box(bool found, int x1, int y1, int x2, int y2, int w, float score, int det_ms, bool tracked = false)
{
    LvglLockGuard lock;
    if (!found) {
        lv_obj_add_flag(s_box, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_label, LV_OBJ_FLAG_HIDDEN);
        return;
    }
    const lv_color_t color = lv_color_hex(tracked ? 0xF0D000 : 0x00E060);
    lv_obj_set_style_border_color(s_box, color, 0);
    lv_obj_set_style_text_color(s_label, color, 0);
    if (kMirror) {
        const int l = w - 1 - x2, r = w - 1 - x1;
        x1 = l;
        x2 = r;
    }
    lv_obj_set_pos(s_box, x1, y1);
    lv_obj_set_size(s_box, std::max(4, x2 - x1), std::max(4, y2 - y1));
    lv_obj_remove_flag(s_box, LV_OBJ_FLAG_HIDDEN);
    char text[32];
    if (tracked) {
        snprintf(text, sizeof(text), "track %.2f", score);
    } else {
        snprintf(text, sizeof(text), "%.2f  %d ms", score, det_ms);
    }
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
    int frames = 0, faces = 0, held = 0, dark = 0, tracks = 0, track_misses = 0, motions = 0, cue_frames = 0;
    int64_t sum_det = 0, max_det = 0;
    uint32_t max_backlog = 0;

    void log(const CpuSample& to)
    {
        const double us = static_cast<double>(to.at - from.at);
        ESP_LOGI(TAG,
                 "%d detections in %.0f s (%d frames dark or flat), face in %d; %d cue frames, tracked %d (lost %d), "
                 "motion %d; detect avg %d ms (max %d); audio_input %.0f%%, idle core 0 %.0f%%, core 1 %.0f%%; mic "
                 "lost %u x 10 ms, backlog max %u ms, held back %d x",
                 frames, us / 1e6, dark, faces, cue_frames, tracks, track_misses, motions, frames ? static_cast<int>(sum_det / frames / 1000) : 0,
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

    FrameCues cues;
    if (!cues.begin()) {
        ESP_LOGE(TAG, "no memory for the small frames");
        vTaskDelete(nullptr);
        return;
    }

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
        // network_busy(): the version check and the server connection need the internal RAM a detection takes.
        if (rate <= 0 || !recent(s_wanted_at, now) || !hal_bridge::is_xiaozhi_ready() || !hal_bridge::is_xiaozhi_idle() ||
            hal_bridge::network_busy()) {
            if (box_shown) {
                show_box(false, 0, 0, 0, 0, 0, 0, 0);
                box_shown = false;
            }
            s_check = false;
            cues.drop_template();
            if (detect && hal_bridge::network_busy()) {
                // The network start (HTTPS, then the server connection) needs the internal RAM the model holds.
                delete detect;
                detect = nullptr;
                log_memory("model unloaded for the network start");
            }
            vTaskDelay(pdMS_TO_TICKS(kIdleWaitMs));
            continue;
        }
        // Next frame: a detection at the rate, or on request (the head has settled) but never back to back; in
        // between the cheaper cues while a face was just seen (tracking) or the director searches (motion).
        const uint32_t since_detect = now - last_detect, since_grab = now - last_grab;
        const uint32_t period = cues.tracking(now, kTrackHoldMs) ? std::max(1000u / static_cast<uint32_t>(rate), kTrackingDetectMs)
                                                                 : 1000u / static_cast<uint32_t>(rate);
        const bool detect_due = since_detect >= period || (s_check && since_detect >= kMinGapMs);
        const bool cue_due    = (cues.tracking(now, kTrackHoldMs) && since_grab >= kTrackPeriodMs) ||
                             (recent(s_search_at, now) && since_grab >= kMotionPeriodMs);
        const uint32_t backlog  = cores3_audio_rx_backlog_ms();
        const bool run_detector = detect_due && backlog <= kMaxBacklogMs;
        if (detect_due && !run_detector) {
            ++stats.held;
        }
        if (!run_detector && !(cue_due && backlog <= kCueBacklogMs)) {
            vTaskDelay(pdMS_TO_TICKS(kPollMs));
            continue;
        }
        if (run_detector) {
            s_check = false;
        }

        auto* camera = hal_bridge::board_get_camera();
        if (!camera) {
            ESP_LOGW(TAG, "no camera, stopping");
            break;
        }
        const int w = camera->GetFrameWidth(), h = camera->GetFrameHeight();
        const int dw = w / 2, dh = h / 2;  // detection size
        if (run_detector && rgb_size < static_cast<size_t>(dw) * dh * 3) {
            heap_caps_free(rgb);
            rgb_size = static_cast<size_t>(dw) * dh * 3;
            rgb      = static_cast<uint8_t*>(heap_caps_aligned_alloc(16, rgb_size, MALLOC_CAP_SPIRAM));
            if (!rgb) {
                ESP_LOGE(TAG, "no memory for the RGB frame");
                break;
            }
        }
        if (now - last_grab > kFreshMs) {
            camera->PeekFrame(nullptr);  // hand the frame that waited in the buffer back unread
        }
        // Everything that reads the frame runs while the driver's buffer is held: nothing is copied (a copy is
        // 150 KB through PSRAM, and PSRAM traffic slows the wake word down).
        uint32_t taken = 0;
        size_t len     = 0;
        bool frame_ok = false, flat = false, head_still = false, lane_found = false;
        FrameLook look{};
        const bool grabbed = camera->PeekFrame([&](const uint8_t* yuv, size_t size) {
            taken = GetHAL().millis() - kFrameMs;
            len   = size;
            if (w <= 0 || h <= 0 || size < static_cast<size_t>(w) * h * 2) {
                return;
            }
            frame_ok             = true;
            look                 = look_at(yuv, size);
            const double var_max = std::max(look.var[0], look.var[1]);
            if (!lane_known && var_max >= 100 && var_max >= 2 * std::min(look.var[0], look.var[1])) {
                lane       = look.var[0] >= look.var[1] ? 1 : 0;
                lane_known = lane_found = true;
            }
            flat = var_max < kFlatVar || look.mean[lane == 1 ? 0 : 1] < kDarkLuma;
            if (flat) {
                return;
            }
            const HeadNote head   = head_note();
            const bool head_known = static_cast<int32_t>(taken - head.at) < static_cast<int32_t>(kHeadNoteMs);
            head_still            = head_known && !head.moving;
            cues.add_frame(yuv, w, h, lane == 1, taken, head.yaw, head.pitch, head_still);
            if (run_detector) {
                yuv422_to_rgb888_half(yuv, rgb, w, h, lane == 1);
            }
        });
        last_grab = GetHAL().millis();
        if (!grabbed) {
            vTaskDelay(pdMS_TO_TICKS(kIdleWaitMs));
            continue;
        }
        if (!frame_ok) {
            ESP_LOGW(TAG, "unexpected frame: %dx%d, %u bytes", w, h, (unsigned)len);
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }
        if (first_frame) {
            first_frame = false;
            ESP_LOGI(TAG, "frames: %dx%d, %u bytes, format 0x%08x; even bytes mean %.0f var %.0f, odd %.0f / %.0f", w, h,
                     (unsigned)len, camera->GetFrameFormat(), look.mean[0], look.var[0], look.mean[1], look.var[1]);
        }
        if (lane_found) {
            ESP_LOGI(TAG, "luma in the %s bytes (variance %.0f / %.0f)", lane == 1 ? "even" : "odd", look.var[0],
                     look.var[1]);
        }
        Sighting s;
        s.seq = ++seq;
        s.at  = taken;
        if (flat) {
            if (run_detector) {
                last_detect = GetHAL().millis();
            }
            cues.drop_template();
            publish(s);  // nothing to find: "nothing" without running anything
            ++stats.dark;
            continue;
        }

        const dl::detect::result_t* best = nullptr;
        int det_ms                       = 0;
        if (run_detector) {
            if (!detect) {
                detect = new HumanFaceDetect();
                detect->set_score_thr(kMsrScore, 0);
                detect->set_score_thr(kMnpScore, 1);
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
            // The biggest face is the closest one (boxes in detection size); a confident one before a weak one.
            for (const auto& r : results) {
                if (r.box.size() < 4) {
                    continue;
                }
                const bool strong = r.score >= kStrongScore, best_strong = best && best->score >= kStrongScore;
                if (!best || (strong && !best_strong) || (strong == best_strong && r.box_area() > best->box_area())) {
                    best = &r;
                }
            }
            if (best) {
                const float x1 = std::clamp(best->box[0], 0, dw) / static_cast<float>(dw);
                const float y1 = std::clamp(best->box[1], 0, dh) / static_cast<float>(dh);
                const float x2 = std::clamp(best->box[2], 0, dw) / static_cast<float>(dw);
                const float y2 = std::clamp(best->box[3], 0, dh) / static_cast<float>(dh);
                s.kind  = SightKind::Face;
                s.cx    = (x1 + x2) * 0.5f;
                s.cy    = (y1 + y2) * 0.5f;
                s.size  = x2 - x1;
                s.score = best->score;
                float tx, ty;
                if (best->score >= kStrongScore ||
                    (cues.tracked_centre(tx, ty) && std::fabs(tx - s.cx) < kNearPatch && std::fabs(ty - s.cy) < kNearPatch)) {
                    cues.set_template(x1, y1, x2, y2, taken);
                }
                ++stats.faces;
            }
            det_ms = static_cast<int>((t1 - t0) / 1000);
            ++stats.frames;
            stats.sum_det += t1 - t0;
            stats.max_det = std::max(stats.max_det, t1 - t0);
        } else {
            ++stats.cue_frames;
        }
        // While the head turns the picture is blurred and the head angles lag: no patch search (and no miss).
        if (s.kind == SightKind::None && head_still && cues.tracking(now, kTrackHoldMs)) {
            float cx, cy, quality;
            if (cues.track(cx, cy, quality)) {
                s.kind  = SightKind::Track;
                s.cx    = cx;
                s.cy    = cy;
                s.score = quality;
                ++stats.tracks;
            } else {
                ++stats.track_misses;
            }
        }
        if (s.kind == SightKind::None) {
            float cx, cy, area;
            if (cues.motion(cx, cy, area)) {
                s.kind = SightKind::Motion;
                s.cx   = cx;
                s.cy   = cy;
                s.size = area;
                ++stats.motions;
            }
        }
        publish(s);

        if (box_on && best) {
            show_box(true, best->box[0] * 2, best->box[1] * 2, best->box[2] * 2, best->box[3] * 2, w, best->score,
                     det_ms);
            box_shown = true;
        } else if (box_on && s.kind == SightKind::Track) {
            const int bw = static_cast<int>(w * 0.15f);  // the patch size is not kept here: a fixed box at its centre
            const int x = static_cast<int>(s.cx * w), y = static_cast<int>(s.cy * h);
            show_box(true, x - bw / 2, y - bw / 2, x + bw / 2, y + bw / 2, w, s.score, 0, true);
            box_shown = true;
        } else if (box_shown && (run_detector || s.kind != SightKind::Track)) {
            show_box(false, 0, 0, 0, 0, 0, 0, 0);
            box_shown = false;
        }
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
void keep_running(bool) {}
void request_check() {}
void note_head(uint32_t, int, int, bool) {}

}  // namespace sc::face_probe

#endif  // CONFIG_SC_FACE_PROBE
