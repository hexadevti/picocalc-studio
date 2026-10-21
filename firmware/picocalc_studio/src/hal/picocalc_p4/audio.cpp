// audio.cpp - PicoCalc speakers from an ESP32-P4: I2S0 in PDM TX, DAC line mode, which turns
// 16-bit PCM into a sigma-delta bitstream on two pins (left on dout, right on dout2) straight into
// the mainboard's RC filter + amp. From pico-386 (audio_p4.cpp, itself from emu8).
//
// The PDM side always runs at AUDIO_OUT_RATE; the caller's rate is brought to it by linear
// interpolation in 16.16 fixed point (zero or more output frames per input sample). Writes block
// on the DMA queue.
#include "../../../board.h"
#include <Arduino.h>
#include "driver/i2s_pdm.h"
#include "../hal.h"

static i2s_chan_handle_t s_tx = nullptr;
static uint32_t s_step = 1u << 16;     // input-to-output rate ratio, 16.16
static uint32_t s_phase = 0;
static int32_t  s_prev = 0;
static int32_t  s_gain = 212;          // 8.8
static int16_t  s_out[256];
static int      s_outN = 0;
static const int kDmaFrames = 256;
// Output frames handed to the driver and played by the DMA: the difference is what is queued.
static volatile uint32_t s_written = 0, s_played = 0;

static bool IRAM_ATTR onSent(i2s_chan_handle_t, i2s_event_data_t *, void *) {
  s_played += kDmaFrames;
  return false;
}

// The TX queue ran empty: the DMA plays a cleared (silent) buffer. Nothing is queued now.
static bool IRAM_ATTR onSendEmpty(i2s_chan_handle_t, i2s_event_data_t *, void *) {
  s_played = s_written;
  return false;
}

static const int16_t kVolume[11] = { 0, 16, 32, 52, 76, 104, 136, 172, 212, 256, 320 };

void hal_audio_volume(int v) {
  if (v < 0) v = 0;
  if (v > 10) v = 10;
  s_gain = kVolume[v];
}

static void audioHwInit() {
  i2s_chan_config_t cc = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
  cc.dma_desc_num  = 8;                // ~46 ms of queue
  cc.dma_frame_num = kDmaFrames;
  cc.auto_clear    = true;             // an underrun plays silence, not the last buffer again
  esp_err_t e = i2s_new_channel(&cc, &s_tx, nullptr);

  i2s_pdm_tx_config_t pc = {
    .clk_cfg  = I2S_PDM_TX_CLK_DAC_DEFAULT_CONFIG(AUDIO_OUT_RATE),
    .slot_cfg = I2S_PDM_TX_SLOT_PCM_FMT_DAC_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
    .gpio_cfg = {
      .clk  = I2S_GPIO_UNUSED,
      .dout = (gpio_num_t)AUDIO_L_PIN,
      .dout2 = (gpio_num_t)AUDIO_R_PIN,
      .invert_flags = { .clk_inv = 0 },
    },
  };
  if (e == ESP_OK) e = i2s_channel_init_pdm_tx_mode(s_tx, &pc);
  if (e == ESP_OK) {
    i2s_event_callbacks_t cbs = {};
    cbs.on_sent = onSent;
    cbs.on_send_q_ovf = onSendEmpty;
    e = i2s_channel_register_event_callback(s_tx, &cbs, nullptr);
  }
  if (e == ESP_OK) e = i2s_channel_enable(s_tx);
  if (e != ESP_OK) {
    hal_log("audio: PDM TX init failed (%s)", esp_err_to_name(e));
    if (s_tx) { i2s_del_channel(s_tx); s_tx = nullptr; }
  }
}

// Made at boot, while there is DMA-capable SRAM for the descriptors.
void p4AudioInit() { audioHwInit(); }

void hal_audio_start(uint32_t rate) {
  if (!rate) rate = AUDIO_OUT_RATE;
  s_step = (uint32_t)(((uint64_t)rate << 16) / AUDIO_OUT_RATE);
  s_phase = 0; s_prev = 0; s_outN = 0;
}

static void flushOut() {
  size_t wrote;
  if (s_tx && s_outN) {
    i2s_channel_write(s_tx, s_out, (size_t)s_outN * 2 * sizeof(int16_t), &wrote, portMAX_DELAY);
    s_written += s_outN;
  }
  s_outN = 0;
}

static inline void put(int32_t x) {
  while (s_phase < (1u << 16)) {
    int32_t y = s_prev + (((x - s_prev) * (int32_t)(s_phase >> 2)) >> 14);   // fits int32 for any two samples
    y = (y * s_gain) >> 8;
    if (y > 32767) y = 32767;
    if (y < -32768) y = -32768;
    s_out[s_outN * 2] = (int16_t)y; s_out[s_outN * 2 + 1] = (int16_t)y;
    if (++s_outN == 128) flushOut();
    s_phase += s_step;
  }
  s_phase -= 1u << 16;
  s_prev = x;
}

void hal_audio_write(const int16_t *mono, int n) {
  for (int i = 0; i < n; i++) put(mono[i]);
  flushOut();
}

uint32_t hal_audio_queued_us() {
  const int32_t q = (int32_t)(s_written - s_played);
  return q > 0 ? (uint32_t)((uint64_t)q * 1000000 / AUDIO_OUT_RATE) : 0;
}
