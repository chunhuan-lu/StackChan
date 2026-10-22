#include "cores3_audio_codec.h"

#include <esp_log.h>
#include <esp_heap_caps.h>
#include <driver/i2c_master.h>
#include <driver/i2s_tdm.h>
#include <cstring>

#define TAG "CoreS3AudioCodec"

// [StackChan fork] Counts microphone audio lost because the reader fell behind: DMA buffers the I2S driver dropped,
// and pump chunks that did not fit into the microphone buffer.
static volatile uint32_t s_rx_overflows = 0;
static StreamBufferHandle_t s_mic_buf   = nullptr;
static size_t s_mic_bytes_per_ms        = 1;

static bool IRAM_ATTR on_rx_overflow(i2s_chan_handle_t, i2s_event_data_t*, void*)
{
    s_rx_overflows = s_rx_overflows + 1;
    return false;
}

uint32_t cores3_audio_rx_overflows()
{
    return s_rx_overflows;
}

uint32_t cores3_audio_rx_backlog_ms()
{
    return s_mic_buf ? xStreamBufferBytesAvailable(s_mic_buf) / s_mic_bytes_per_ms : 0;
}

CoreS3AudioCodec::CoreS3AudioCodec(void* i2c_master_handle, int input_sample_rate, int output_sample_rate,
    gpio_num_t mclk, gpio_num_t bclk, gpio_num_t ws, gpio_num_t dout, gpio_num_t din,
    uint8_t aw88298_addr, uint8_t es7210_addr, bool input_reference) {
    duplex_ = true; // 是否双工
    input_reference_ = input_reference; // 是否使用参考输入，实现回声消除
    input_channels_ = input_reference_ ? 2 : 1; // 输入通道数
    input_sample_rate_ = input_sample_rate;
    output_sample_rate_ = output_sample_rate;
    input_gain_ = 60;

    CreateDuplexChannels(mclk, bclk, ws, dout, din);

    // Do initialize of related interface: data_if, ctrl_if and gpio_if
    audio_codec_i2s_cfg_t i2s_cfg = {
        .port = I2S_NUM_0,
        .rx_handle = rx_handle_,
        .tx_handle = tx_handle_,
    };
    data_if_ = audio_codec_new_i2s_data(&i2s_cfg);
    assert(data_if_ != NULL);

    // Audio Output(Speaker)
    audio_codec_i2c_cfg_t i2c_cfg = {
        .port = (i2c_port_t)1,
        .addr = aw88298_addr,
        .bus_handle = i2c_master_handle,
    };
    out_ctrl_if_ = audio_codec_new_i2c_ctrl(&i2c_cfg);
    assert(out_ctrl_if_ != NULL);

    gpio_if_ = audio_codec_new_gpio();
    assert(gpio_if_ != NULL);

    aw88298_codec_cfg_t aw88298_cfg = {};
    aw88298_cfg.ctrl_if = out_ctrl_if_;
    aw88298_cfg.gpio_if = gpio_if_;
    aw88298_cfg.reset_pin = GPIO_NUM_NC;
    aw88298_cfg.hw_gain.pa_voltage = 5.0;
    aw88298_cfg.hw_gain.codec_dac_voltage = 3.3;
    aw88298_cfg.hw_gain.pa_gain = 1;
    out_codec_if_ = aw88298_codec_new(&aw88298_cfg);
    assert(out_codec_if_ != NULL);

    esp_codec_dev_cfg_t dev_cfg = {
        .dev_type = ESP_CODEC_DEV_TYPE_OUT,
        .codec_if = out_codec_if_,
        .data_if = data_if_,
    };
    output_dev_ = esp_codec_dev_new(&dev_cfg);
    assert(output_dev_ != NULL);

    // Audio Input(Microphone)
    i2c_cfg.addr = es7210_addr;
    in_ctrl_if_ = audio_codec_new_i2c_ctrl(&i2c_cfg);
    assert(in_ctrl_if_ != NULL);

    es7210_codec_cfg_t es7210_cfg = {};
    es7210_cfg.ctrl_if = in_ctrl_if_;
    es7210_cfg.mic_selected = ES7210_SEL_MIC1 | ES7210_SEL_MIC2 | ES7210_SEL_MIC3;
    in_codec_if_ = es7210_codec_new(&es7210_cfg);
    assert(in_codec_if_ != NULL);

    dev_cfg.dev_type = ESP_CODEC_DEV_TYPE_IN;
    dev_cfg.codec_if = in_codec_if_;
    input_dev_ = esp_codec_dev_new(&dev_cfg);
    assert(input_dev_ != NULL);

    StartMicPump();
    ESP_LOGI(TAG, "CoreS3AudioCodec initialized");
}

// [StackChan fork] The audio task runs the wake word between its reads; when camera work on the other core slows it
// down, the six I2S DMA buffers (60 ms) overflow. A high-priority pump on core 1 keeps draining them into a PSRAM
// buffer that Read() takes from instead.
void CoreS3AudioCodec::StartMicPump() {
#if CONFIG_SC_MIC_BUFFER_MS > 0
    const size_t channels = input_reference_ ? 2 : 1;
    mic_chunk_bytes_ = output_sample_rate_ / 100 * channels * sizeof(int16_t);  // 10 ms, as the DMA buffers
    s_mic_bytes_per_ms = mic_chunk_bytes_ / 10;
    mic_chunk_ = static_cast<int16_t*>(heap_caps_malloc(mic_chunk_bytes_, MALLOC_CAP_SPIRAM));
    mic_buf_ = xStreamBufferCreateWithCaps(s_mic_bytes_per_ms * CONFIG_SC_MIC_BUFFER_MS, 1, MALLOC_CAP_SPIRAM);
    mic_dev_lock_ = xSemaphoreCreateMutex();
    if (!mic_chunk_ || !mic_buf_ || !mic_dev_lock_ ||
        xTaskCreatePinnedToCoreWithCaps([](void* self) { static_cast<CoreS3AudioCodec*>(self)->MicPump(); }, "mic_pump",
                                        3072, this, 9, &mic_pump_, 1, MALLOC_CAP_SPIRAM) != pdPASS) {
        ESP_LOGE(TAG, "mic buffer: no memory, reading the I2S channel directly");
        if (mic_buf_) {
            vStreamBufferDeleteWithCaps(mic_buf_);
            mic_buf_ = nullptr;
        }
        return;
    }
    s_mic_buf = mic_buf_;
    ESP_LOGI(TAG, "mic buffer: %d ms", CONFIG_SC_MIC_BUFFER_MS);
#endif
}

void CoreS3AudioCodec::MicPump() {
    while (true) {
        if (!input_enabled_) {
            ulTaskNotifyTake(pdTRUE, portMAX_DELAY);  // EnableInput(true) wakes it
            continue;
        }
        xSemaphoreTake(mic_dev_lock_, portMAX_DELAY);
        const int ret = input_enabled_ ? esp_codec_dev_read(input_dev_, mic_chunk_, mic_chunk_bytes_) : ESP_CODEC_DEV_WRONG_STATE;
        xSemaphoreGive(mic_dev_lock_);
        if (ret != ESP_CODEC_DEV_OK) {
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }
        if (xStreamBufferSpacesAvailable(mic_buf_) < mic_chunk_bytes_) {
            s_rx_overflows = s_rx_overflows + 1;
            continue;
        }
        xStreamBufferSend(mic_buf_, mic_chunk_, mic_chunk_bytes_, 0);
    }
}

CoreS3AudioCodec::~CoreS3AudioCodec() {
    ESP_ERROR_CHECK(esp_codec_dev_close(output_dev_));
    esp_codec_dev_delete(output_dev_);
    ESP_ERROR_CHECK(esp_codec_dev_close(input_dev_));
    esp_codec_dev_delete(input_dev_);

    audio_codec_delete_codec_if(in_codec_if_);
    audio_codec_delete_ctrl_if(in_ctrl_if_);
    audio_codec_delete_codec_if(out_codec_if_);
    audio_codec_delete_ctrl_if(out_ctrl_if_);
    audio_codec_delete_gpio_if(gpio_if_);
    audio_codec_delete_data_if(data_if_);
}

void CoreS3AudioCodec::CreateDuplexChannels(gpio_num_t mclk, gpio_num_t bclk, gpio_num_t ws, gpio_num_t dout, gpio_num_t din) {
    assert(input_sample_rate_ == output_sample_rate_);

    ESP_LOGI(TAG, "Audio IOs: mclk: %d, bclk: %d, ws: %d, dout: %d, din: %d", mclk, bclk, ws, dout, din);

    i2s_chan_config_t chan_cfg = {
        .id = I2S_NUM_0,
        .role = I2S_ROLE_MASTER,
        .dma_desc_num = AUDIO_CODEC_DMA_DESC_NUM,
        .dma_frame_num = AUDIO_CODEC_DMA_FRAME_NUM,
        .auto_clear_after_cb = true,
        .auto_clear_before_cb = false,
        .intr_priority = 0,
    };
    ESP_ERROR_CHECK(i2s_new_channel(&chan_cfg, &tx_handle_, &rx_handle_));

    i2s_std_config_t std_cfg = {
        .clk_cfg = {
            .sample_rate_hz = (uint32_t)output_sample_rate_,
            .clk_src = I2S_CLK_SRC_DEFAULT,
            .ext_clk_freq_hz = 0,
            .mclk_multiple = I2S_MCLK_MULTIPLE_256
        },
        .slot_cfg = {
            .data_bit_width = I2S_DATA_BIT_WIDTH_16BIT,
            .slot_bit_width = I2S_SLOT_BIT_WIDTH_AUTO,
            .slot_mode = I2S_SLOT_MODE_STEREO,
            .slot_mask = I2S_STD_SLOT_BOTH,
            .ws_width = I2S_DATA_BIT_WIDTH_16BIT,
            .ws_pol = false,
            .bit_shift = true,
            .left_align = true,
            .big_endian = false,
            .bit_order_lsb = false
        },
        .gpio_cfg = {
            .mclk = mclk,
            .bclk = bclk,
            .ws = ws,
            .dout = dout,
            .din = I2S_GPIO_UNUSED,
            .invert_flags = {
                .mclk_inv = false,
                .bclk_inv = false,
                .ws_inv = false
            }
        }
    };

    i2s_tdm_config_t tdm_cfg = {
        .clk_cfg = {
            .sample_rate_hz = (uint32_t)input_sample_rate_,
            .clk_src = I2S_CLK_SRC_DEFAULT,
            .ext_clk_freq_hz = 0,
            .mclk_multiple = I2S_MCLK_MULTIPLE_256,
            .bclk_div = 8,
        },
        .slot_cfg = {
            .data_bit_width = I2S_DATA_BIT_WIDTH_16BIT,
            .slot_bit_width = I2S_SLOT_BIT_WIDTH_AUTO,
            .slot_mode = I2S_SLOT_MODE_STEREO,
            .slot_mask = i2s_tdm_slot_mask_t(I2S_TDM_SLOT0 | I2S_TDM_SLOT1 | I2S_TDM_SLOT2 | I2S_TDM_SLOT3),
            .ws_width = I2S_TDM_AUTO_WS_WIDTH,
            .ws_pol = false,
            .bit_shift = true,
            .left_align = false,
            .big_endian = false,
            .bit_order_lsb = false,
            .skip_mask = false,
            .total_slot = I2S_TDM_AUTO_SLOT_NUM
        },
        .gpio_cfg = {
            .mclk = mclk,
            .bclk = bclk,
            .ws = ws,
            .dout = I2S_GPIO_UNUSED,
            .din = din,
            .invert_flags = {
                .mclk_inv = false,
                .bclk_inv = false,
                .ws_inv = false
            }
        }
    };

    ESP_ERROR_CHECK(i2s_channel_init_std_mode(tx_handle_, &std_cfg));
    ESP_ERROR_CHECK(i2s_channel_init_tdm_mode(rx_handle_, &tdm_cfg));
    i2s_event_callbacks_t rx_cbs = {};
    rx_cbs.on_recv_q_ovf         = on_rx_overflow;
    ESP_ERROR_CHECK(i2s_channel_register_event_callback(rx_handle_, &rx_cbs, nullptr));
    ESP_ERROR_CHECK(i2s_channel_enable(tx_handle_));
    ESP_ERROR_CHECK(i2s_channel_enable(rx_handle_));
    ESP_LOGI(TAG, "Duplex channels created");
}

void CoreS3AudioCodec::SetOutputVolume(int volume) {
    ESP_ERROR_CHECK(esp_codec_dev_set_out_vol(output_dev_, volume));
    AudioCodec::SetOutputVolume(volume);
}

void CoreS3AudioCodec::EnableInput(bool enable) {
    if (enable == input_enabled_) {
        return;
    }
    if (mic_buf_) {
        xSemaphoreTake(mic_dev_lock_, portMAX_DELAY);
    }
    if (enable) {
        esp_codec_dev_sample_info_t fs = {
            .bits_per_sample = 16,
            .channel = 2,
            .channel_mask = ESP_CODEC_DEV_MAKE_CHANNEL_MASK(0),
            .sample_rate = (uint32_t)output_sample_rate_,
            .mclk_multiple = 0,
        };
        if (input_reference_) {
            fs.channel_mask |= ESP_CODEC_DEV_MAKE_CHANNEL_MASK(1);
        }
        ESP_ERROR_CHECK(esp_codec_dev_open(input_dev_, &fs));
        ESP_ERROR_CHECK(esp_codec_dev_set_in_channel_gain(input_dev_, ESP_CODEC_DEV_MAKE_CHANNEL_MASK(0), input_gain_));
    } else {
        ESP_ERROR_CHECK(esp_codec_dev_close(input_dev_));
    }
    AudioCodec::EnableInput(enable);
    if (mic_buf_) {
        if (enable) {
            xStreamBufferReset(mic_buf_);  // drop what is left from before; fails harmlessly while Read() waits
        }
        xSemaphoreGive(mic_dev_lock_);
        if (enable) {
            xTaskNotifyGive(mic_pump_);
        }
    }
}

void CoreS3AudioCodec::EnableOutput(bool enable) {
    if (enable == output_enabled_) {
        return;
    }
    if (enable) {
        // Play 16bit 1 channel
        esp_codec_dev_sample_info_t fs = {
            .bits_per_sample = 16,
            .channel = 1,
            .channel_mask = 0,
            .sample_rate = (uint32_t)output_sample_rate_,
            .mclk_multiple = 0,
        };
        ESP_ERROR_CHECK(esp_codec_dev_open(output_dev_, &fs));
        ESP_ERROR_CHECK(esp_codec_dev_set_out_vol(output_dev_, output_volume_));
    } else {
        ESP_ERROR_CHECK(esp_codec_dev_close(output_dev_));
    }
    AudioCodec::EnableOutput(enable);
}

int CoreS3AudioCodec::Read(int16_t* dest, int samples) {
    if (!input_enabled_) {
        return samples;
    }
    if (!mic_buf_) {
        ESP_ERROR_CHECK_WITHOUT_ABORT(esp_codec_dev_read(input_dev_, (void*)dest, samples * sizeof(int16_t)));
        return samples;
    }
    auto* out = reinterpret_cast<uint8_t*>(dest);
    const size_t want = samples * sizeof(int16_t);
    size_t got = 0;
    while (got < want) {
        const size_t n = xStreamBufferReceive(mic_buf_, out + got, want - got, pdMS_TO_TICKS(100));
        if (n == 0) {  // the input was closed meanwhile
            memset(out + got, 0, want - got);
            break;
        }
        got += n;
    }
    return samples;
}

int CoreS3AudioCodec::Write(const int16_t* data, int samples) {
    if (output_enabled_) {
        ESP_ERROR_CHECK_WITHOUT_ABORT(esp_codec_dev_write(output_dev_, (void*)data, samples * sizeof(int16_t)));
    }
    return samples;
}
