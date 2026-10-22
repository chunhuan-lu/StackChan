#ifndef _BOX_AUDIO_CODEC_H
#define _BOX_AUDIO_CODEC_H

#include "audio_codec.h"

#include <esp_codec_dev.h>
#include <esp_codec_dev_defaults.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/stream_buffer.h>

// [StackChan fork] Microphone audio lost because nobody read it in time, in 10 ms chunks (REQ-008).
uint32_t cores3_audio_rx_overflows();
// [StackChan fork] Microphone audio waiting in the buffer (CONFIG_SC_MIC_BUFFER_MS), in ms.
uint32_t cores3_audio_rx_backlog_ms();

class CoreS3AudioCodec : public AudioCodec {
private:
    const audio_codec_data_if_t* data_if_ = nullptr;
    const audio_codec_ctrl_if_t* out_ctrl_if_ = nullptr;
    const audio_codec_if_t* out_codec_if_ = nullptr;
    const audio_codec_ctrl_if_t* in_ctrl_if_ = nullptr;
    const audio_codec_if_t* in_codec_if_ = nullptr;
    const audio_codec_gpio_if_t* gpio_if_ = nullptr;

    esp_codec_dev_handle_t output_dev_ = nullptr;
    esp_codec_dev_handle_t input_dev_ = nullptr;

    // [StackChan fork] Microphone buffer (CONFIG_SC_MIC_BUFFER_MS)
    StreamBufferHandle_t mic_buf_ = nullptr;
    SemaphoreHandle_t mic_dev_lock_ = nullptr;  // the pump's reads against opening and closing the input
    TaskHandle_t mic_pump_ = nullptr;
    int16_t* mic_chunk_ = nullptr;
    size_t mic_chunk_bytes_ = 0;

    void CreateDuplexChannels(gpio_num_t mclk, gpio_num_t bclk, gpio_num_t ws, gpio_num_t dout, gpio_num_t din);
    void StartMicPump();
    void MicPump();

    virtual int Read(int16_t* dest, int samples) override;
    virtual int Write(const int16_t* data, int samples) override;

public:
    CoreS3AudioCodec(void* i2c_master_handle, int input_sample_rate, int output_sample_rate,
        gpio_num_t mclk, gpio_num_t bclk, gpio_num_t ws, gpio_num_t dout, gpio_num_t din,
        uint8_t aw88298_addr, uint8_t es7210_addr, bool input_reference);
    virtual ~CoreS3AudioCodec();

    virtual void SetOutputVolume(int volume) override;
    virtual void EnableInput(bool enable) override;
    virtual void EnableOutput(bool enable) override;
};

#endif // _BOX_AUDIO_CODEC_H
