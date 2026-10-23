// snd.cpp - picocalc.h snd_*. A task on core 0 owns the audio output: it plays either the current
// snd_tone() square wave or samples queued by snd_play() in a stream buffer, at that stream's
// rate (the HAL resamples to the output rate). snd_play() blocks while the buffer is full, which
// paces a program that generates sound in its loop.
#include <Arduino.h>
#include "freertos/stream_buffer.h"
#include "picocalc.h"
#include "sdk_internal.h"
#include "../hal/hal.h"

#define TONE_RATE    22050
#define STREAM_BYTES 8192        // ~93 ms at 44.1 kHz mono
#define CHUNK        256

static StreamBufferHandle_t s_stream;
static volatile uint32_t s_streamRate = 22050;
static volatile uint32_t s_toneLeft;     // samples of tone still to play
static volatile uint32_t s_toneStep;     // phase step, 16.16 of a period per sample
static volatile bool s_stop;
static volatile bool s_playing;          // the task is writing (tone or stream) right now

static void sndTask(void *) {
  static int16_t buf[CHUNK];
  uint32_t rate = 0, phase = 0;
  for (;;) {
    if (s_stop) {
      s_toneLeft = 0;
      while (xStreamBufferReceive(s_stream, buf, sizeof buf, 0)) {}
      s_stop = false;
    }
    if (s_toneLeft) {
      if (rate != TONE_RATE) { rate = TONE_RATE; hal_audio_start(rate); }
      uint32_t n = s_toneLeft < CHUNK ? s_toneLeft : CHUNK;
      const uint32_t step = s_toneStep;
      for (uint32_t i = 0; i < n; i++) {
        buf[i] = (phase & 0x8000) ? 6000 : -6000;
        phase += step;
      }
      s_playing = true;
      hal_audio_write(buf, (int)n);
      s_toneLeft = s_toneLeft > n ? s_toneLeft - n : 0;
      s_playing = false;
      continue;
    }
    size_t got = xStreamBufferReceive(s_stream, buf, sizeof buf, pdMS_TO_TICKS(10));
    got &= ~(size_t)1;
    if (!got) continue;
    if (rate != s_streamRate) { rate = s_streamRate; hal_audio_start(rate); }
    s_playing = true;
    hal_audio_write(buf, (int)(got / 2));
    s_playing = false;
  }
}

void sdk_snd_init() {
  s_stream = xStreamBufferCreate(STREAM_BYTES, 2);
  hal_audio_start(TONE_RATE);
  xTaskCreatePinnedToCore(sndTask, "snd", 4096, nullptr, 5, nullptr, 0);
}

void sdk_snd_reset() {
  snd_stop();
  snd_volume(7);
}

void snd_volume(int level) { hal_audio_volume(level); }

void snd_tone(int freq_hz, int ms) {
  if (freq_hz <= 0 || ms <= 0) { s_toneLeft = 0; return; }
  if (freq_hz > TONE_RATE / 2) freq_hz = TONE_RATE / 2;
  s_toneStep = (uint32_t)(((uint64_t)freq_hz << 16) / TONE_RATE);
  s_toneLeft = (uint32_t)((uint64_t)ms * TONE_RATE / 1000);
}

void snd_stop(void) {
  s_stop = true;
  for (int i = 0; i < 50 && s_stop; i++) hal_delay(2);   // the task clears it within one tick
}

bool snd_busy(void) {
  return s_toneLeft || s_playing || !xStreamBufferIsEmpty(s_stream) || hal_audio_queued_us() > 0;
}

int snd_play(const int16_t *samples, int count, int rate_hz) {
  if (!samples || count <= 0) return 0;
  if (rate_hz < 4000) rate_hz = 4000;
  if (rate_hz > 96000) rate_hz = 96000;
  if ((uint32_t)rate_hz != s_streamRate) {
    // Samples already queued were meant for the old rate: let them play out first.
    while (!xStreamBufferIsEmpty(s_stream) || s_playing) hal_delay(1);
    s_streamRate = (uint32_t)rate_hz;
  }
  const uint8_t *p = (const uint8_t *)samples;
  size_t left = (size_t)count * 2;
  while (left) {
    size_t n = xStreamBufferSend(s_stream, p, left, pdMS_TO_TICKS(50));
    p += n;
    left -= n;
    if (!n) sdk_kbd_pump();   // waiting: still let Break through
  }
  return count;
}
