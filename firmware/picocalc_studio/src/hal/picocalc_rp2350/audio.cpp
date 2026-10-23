// audio.cpp - PicoCalc speakers from an RP2350: PWM on GPIO26/27 (one slice, PWM5A/B) through
// arduino-pico's PWMAudio in stereo mode, DMA-fed from a ring of buffers. As in emu8
// (src/picocalc/audio_picocalc.cpp): the library's blocking write spins, so writes check for room
// first and sleep while the ring is full; and its underflow "silence" is refilled with the scaled
// mid level, or every underflow pops.
//
// The output always runs at AUDIO_OUT_RATE; the caller's rate is brought to it by linear
// interpolation in 16.16 fixed point, as on the P4.
#ifdef ARDUINO_ARCH_RP2040
#include "../../../board.h"
#include <Arduino.h>
#define private public                 // ampFixSilence() reaches the library's buffer manager
#include <PWMAudio.h>
#undef private
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "../hal.h"

#define BUFFERS      8
#define BUFFER_WORDS 128               // one stereo frame per word: ~23 ms of queue

static PWMAudio s_dac(AUDIO_L_PIN, true);
static bool     s_up = false;
static uint32_t s_step = 1u << 16;     // input-to-output rate ratio, 16.16
static uint32_t s_phase = 0;
static int32_t  s_prev = 0;
static int32_t  s_gain = 212;          // 8.8
static int16_t  s_out[128];            // mono output frames
static int      s_outN = 0;
static int      s_idleRoom = 0;      // ring room with nothing queued (the DMA holds some buffers)

static const int16_t kVolume[11] = { 0, 16, 32, 52, 76, 104, 136, 172, 212, 256, 320 };

void hal_audio_volume(int v) {
  if (v < 0) v = 0;
  if (v > 10) v = 10;
  s_gain = kVolume[v];
}

static void fixSilence() {
  const uint32_t mid = (0x8000u * s_dac._pwmScale) >> 16;
  AudioBufferManager *arb = s_dac._arb;
  if (!arb || !arb->_silence) return;
  for (size_t i = 0; i < arb->_wordsPerBuffer; i++) arb->_silence->buff[i] = mid | (mid << 16);
}

void rpAudioInit() {
  s_dac.setBuffers(BUFFERS, BUFFER_WORDS);
  if (!s_dac.begin((long)AUDIO_OUT_RATE)) { hal_log("audio: PWMAudio begin failed"); return; }
  fixSilence();
  s_idleRoom = s_dac.availableForWrite() / 4;
  s_up = true;
}

void hal_audio_start(uint32_t rate) {
  if (!rate) rate = AUDIO_OUT_RATE;
  s_step = (uint32_t)(((uint64_t)rate << 16) / AUDIO_OUT_RATE);
  s_phase = 0; s_prev = 0; s_outN = 0;
}

// Each frame is one 32-bit word in the ring: wait (sleeping) until all of them fit, then write.
static void flushOut() {
  if (s_up && s_outN) {
    while (s_dac.availableForWrite() / 4 < s_outN) vTaskDelay(1);
    for (int i = 0; i < s_outN; i++) {
      s_dac.write(s_out[i]);           // left: latched
      s_dac.write(s_out[i]);           // right: completes the word
    }
  }
  s_outN = 0;
}

static inline void put(int32_t x) {
  while (s_phase < (1u << 16)) {
    int32_t y = s_prev + (((x - s_prev) * (int32_t)(s_phase >> 2)) >> 14);   // fits int32 for any two samples
    y = (y * s_gain) >> 8;
    if (y > 32767) y = 32767;
    if (y < -32768) y = -32768;
    s_out[s_outN] = (int16_t)y;
    if (++s_outN == (int)(sizeof s_out / sizeof s_out[0])) flushOut();
    s_phase += s_step;
  }
  s_phase -= 1u << 16;
  s_prev = x;
}

void hal_audio_write(const int16_t *mono, int n) {
  for (int i = 0; i < n; i++) put(mono[i]);
  flushOut();
}

// Room taken in the ring; the buffers the DMA is playing are not counted.
uint32_t hal_audio_queued_us() {
  if (!s_up) return 0;
  const int q = s_idleRoom - s_dac.availableForWrite() / 4;
  return q > 0 ? (uint32_t)((uint64_t)q * 1000000 / AUDIO_OUT_RATE) : 0;
}

#endif  // ARDUINO_ARCH_RP2040
