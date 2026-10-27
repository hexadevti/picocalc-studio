// host_snd.cpp - picocalc.h snd_* for the screenshot harness: silent.
#include "../../../firmware/picocalc_studio/src/sdk/picocalc.h"
#include "../../../firmware/picocalc_studio/src/sdk/sdk_internal.h"

void sdk_snd_init() {}
void sdk_snd_reset() {}
void snd_volume(int) {}
void snd_tone(int, int) {}
void snd_stop(void) {}
bool snd_busy(void) { return false; }
// A queue that plays in real time and holds ~100 ms, like the device's: snd_play blocks only while
// it is full, so a program paced by its audio keeps its speed.
int snd_play(const int16_t *, int count, int rate_hz) {
  static uint64_t s_end;   // when the queued audio runs out (us)
  if (rate_hz <= 0) return count;
  const uint64_t now = sys_micros();
  if (s_end < now) s_end = now;
  if (s_end > now + 100000) sys_delay((uint32_t)((s_end - now - 100000) / 1000));
  s_end += (uint64_t)count * 1000000 / rate_hz;
  return count;
}
