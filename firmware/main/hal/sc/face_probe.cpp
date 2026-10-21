/*
 * SPDX-FileCopyrightText: 2026 chunhuan-lu/StackChan contributors
 *
 * SPDX-License-Identifier: MIT
 */
#include "face_probe.h"
#include <hal/hal.h>
#include <hal/board/hal_bridge.h>
#include "human_face_detect.hpp"
#include <hal/board/cores3_audio_codec.h>
#include <stackchan/sc/sc_settings.h>
#include <esp_heap_caps.h>
#include <esp_log.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <lvgl.h>
#include <algorithm>
#include <cstdio>
#include <cstring>

namespace sc::face_probe {

static const char* TAG = "FaceProbe";

static constexpr uint32_t kIdleWaitMs = 500;  // polling while the AI Agent is busy
// Detection slows the wake word down (PSRAM and cache contention): start one only when the microphone buffer has been
// read down to this.
static constexpr uint32_t kMaxBacklogMs = 80;
static constexpr int64_t kStatsUs     = 10 * 1000 * 1000;
static constexpr bool kMirror         = true;  // the camera looks at the user: draw the box like a mirror
// Diagnostic: step through these rates, one statistics window each, instead of using the setting.
static constexpr bool kRateSweep = false;
static constexpr int kSweep[]    = {0, 1, 2, 4};

static lv_obj_t* s_box   = nullptr;
static lv_obj_t* s_label = nullptr;

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

// Luma varies across the picture, chroma stays near 128: the byte lane with the larger variance holds Y.
static bool luma_first(const uint8_t* src, size_t len)
{
    double sum[2] = {0, 0}, sq[2] = {0, 0};
    size_t n      = 0;
    for (size_t i = 0; i + 1 < len; i += 64, ++n) {
        for (int k = 0; k < 2; ++k) {
            sum[k] += src[i + k];
            sq[k] += static_cast<double>(src[i + k]) * src[i + k];
        }
    }
    double var[2];
    for (int k = 0; k < 2; ++k) {
        const double mean = sum[k] / n;
        var[k]            = sq[k] / n - mean * mean;
    }
    ESP_LOGI(TAG, "byte lanes: even var %.0f, odd var %.0f -> luma %s", var[0], var[1], var[0] >= var[1] ? "even" : "odd");
    return var[0] >= var[1];
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
    uint32_t overflows;  // microphone buffers lost
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

static void log_cpu(const CpuSample& from, const CpuSample& to, int rate, int frames, uint32_t max_backlog, int held)
{
    const double us = static_cast<double>(to.at - from.at);
    ESP_LOGI(TAG,
             "cpu (rate %d, %d frames): audio_input %.0f%%, idle core 0 %.0f%%, idle core 1 %.0f%%; mic lost %u x 10 ms, "
             "backlog max %u ms, held back %d x",
             rate, frames, (to.audio - from.audio) * 100.0 / us, (to.idle0 - from.idle0) * 100.0 / us,
             (to.idle1 - from.idle1) * 100.0 / us, (unsigned)(to.overflows - from.overflows), (unsigned)max_backlog,
             held);
}

static void probe_task(void*)
{
    create_overlay();
    log_memory("start");

    HumanFaceDetect* detect = nullptr;
    uint8_t* rgb            = nullptr;
    size_t rgb_size         = 0;
    int lane                = -1;  // 1: luma in even bytes

    // Statistics per window
    int frames = 0, faces = 0;
    int64_t sum_grab = 0, sum_conv = 0, sum_det = 0, max_det = 0;
    int64_t window_start = esp_timer_get_time();
    bool had_face        = false;
    int64_t next_box_log = 0;

    int rate_logged     = -1;
    CpuSample cpu_from  = cpu_sample();
    int cpu_frames      = 0;
    uint32_t max_backlog = 0;
    int held             = 0;
    size_t sweep        = 0;
    while (true) {
        // Checks per second from the settings panel (prototype): 0 pauses detection.
        int rate = kRateSweep ? kSweep[sweep] : stackchan::sc::setting(stackchan::sc::SettingId::FaceProbeRate);
        if (esp_timer_get_time() - cpu_from.at >= kStatsUs) {
            const CpuSample now = cpu_sample();
            log_cpu(cpu_from, now, rate, cpu_frames, max_backlog, held);
            cpu_from    = now;
            cpu_frames  = 0;
            max_backlog = 0;
            held        = 0;
            if (kRateSweep) {
                sweep = (sweep + 1) % (sizeof(kSweep) / sizeof(kSweep[0]));
                rate  = kSweep[sweep];
            }
        }
        if (rate != rate_logged) {
            ESP_LOGI(TAG, "rate %d/s", rate);
            rate_logged = rate;
        }
        if (rate <= 0 || !hal_bridge::is_xiaozhi_ready() || !hal_bridge::is_xiaozhi_idle()) {
            if (had_face) {
                show_box(false, 0, 0, 0, 0, 0, 0, 0);
                had_face = false;
            }
            vTaskDelay(pdMS_TO_TICKS(kIdleWaitMs));
            continue;
        }
        if (cores3_audio_rx_backlog_ms() > kMaxBacklogMs) {
            ++held;
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }
        auto* camera = hal_bridge::board_get_camera();
        if (!camera) {
            ESP_LOGW(TAG, "no camera, stopping");
            break;
        }

        const int64_t t0 = esp_timer_get_time();
        if (!camera->StreamCaptures()) {
            vTaskDelay(pdMS_TO_TICKS(kIdleWaitMs));
            continue;
        }
        const int64_t t1   = esp_timer_get_time();
        const uint8_t* yuv = camera->GetFrameData();
        const int w = camera->GetFrameWidth(), h = camera->GetFrameHeight();
        const size_t len = camera->GetFrameSize();
        if (!yuv || w <= 0 || h <= 0 || len < static_cast<size_t>(w) * h * 2) {
            ESP_LOGW(TAG, "unexpected frame: %dx%d, %u bytes, format 0x%08x", w, h, (unsigned)len,
                     camera->GetFrameFormat());
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }
        if (lane < 0) {
            ESP_LOGI(TAG, "frames: %dx%d, %u bytes, format 0x%08x", w, h, (unsigned)len, camera->GetFrameFormat());
            lane = luma_first(yuv, len) ? 1 : 0;
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
        const int64_t t2 = esp_timer_get_time();

        if (!detect) {
            detect = new HumanFaceDetect();
        }
        dl::image::img_t img = {rgb, static_cast<uint16_t>(dw), static_cast<uint16_t>(dh),
                                dl::image::DL_IMAGE_PIX_TYPE_RGB888};
        auto& results    = detect->run(img);
        const int64_t t3 = esp_timer_get_time();
        max_backlog      = std::max(max_backlog, cores3_audio_rx_backlog_ms());
        static bool loaded_logged = false;
        if (!loaded_logged) {
            loaded_logged = true;
            log_memory("after the first detection (model loaded)");
        }

        // The biggest face is the closest one.
        const dl::detect::result_t* best = nullptr;
        for (const auto& r : results) {
            if (r.box.size() >= 4 && (!best || r.box_area() > best->box_area())) {  // boxes in detection size
                best = &r;
            }
        }
        const int det_ms = static_cast<int>((t3 - t2) / 1000);
        if (best) {
            show_box(true, best->box[0] * 2, best->box[1] * 2, best->box[2] * 2, best->box[3] * 2, w, best->score, det_ms);
            if (!had_face || t3 >= next_box_log) {
                ESP_LOGI(TAG, "face %d/%u: box (%d,%d)-(%d,%d), score %.2f, detect %d ms", had_face ? 1 : 0,
                         (unsigned)results.size(), best->box[0], best->box[1], best->box[2], best->box[3], best->score,
                         det_ms);
                next_box_log = t3 + 1000 * 1000;
            }
            ++faces;
        } else if (had_face) {
            show_box(false, 0, 0, 0, 0, 0, 0, 0);
            ESP_LOGI(TAG, "face lost");
        }
        had_face = best != nullptr;

        ++frames;
        ++cpu_frames;
        sum_grab += t1 - t0;
        sum_conv += t2 - t1;
        sum_det += t3 - t2;
        max_det = std::max(max_det, t3 - t2);
        if (t3 - window_start >= kStatsUs) {
            const double secs = (t3 - window_start) / 1e6;
            ESP_LOGI(TAG, "%d frames in %.1f s (%.1f fps), face in %d; avg grab %d ms, convert %d ms, detect %d ms (max %d)",
                     frames, secs, frames / secs, faces, static_cast<int>(sum_grab / frames / 1000),
                     static_cast<int>(sum_conv / frames / 1000), static_cast<int>(sum_det / frames / 1000),
                     static_cast<int>(max_det / 1000));
            log_memory("memory");
            frames = faces = 0;
            sum_grab = sum_conv = sum_det = max_det = 0;
            window_start = t3;
        }

        const uint32_t period = 1000u / static_cast<uint32_t>(rate);
        const uint32_t spent  = static_cast<uint32_t>((esp_timer_get_time() - t0) / 1000);
        vTaskDelay(pdMS_TO_TICKS(spent < period ? period - spent : 20));
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
