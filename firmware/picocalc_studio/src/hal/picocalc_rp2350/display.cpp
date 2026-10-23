// display.cpp - the PicoCalc's ST7365P/ILI9488 320x320 SPI panel behind an RP2350.
//
// Transport as in emu8 (src/picocalc/display_picocalc.cpp):
//  * Raw pico-sdk spi1, not the Arduino SPI1 object. CS is a plain GPIO held low for good: the
//    panel is alone on this bus, and no CS edge can then land between a RAMWR and its pixel data.
//  * Pixels go out in 16-bit SPI frames by DMA straight from the caller's RGB565 (no byte swap);
//    commands in 8-bit frames with blocking writes. The format only changes with the SPI idle.
//  * clk_peri is moved to clk_sys first, or the SPI is capped at 24 MHz.
#ifdef ARDUINO_ARCH_RP2040
#include "../../../board.h"
#include <Arduino.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "hardware/spi.h"
#include "hardware/dma.h"
#include "hardware/gpio.h"
#include "hardware/clocks.h"
#include "../hal.h"

#define LCD_SPI spi1

static int s_dma = -1;
static dma_channel_config s_cfgInc;    // read-increment on (a pixel buffer)
static dma_channel_config s_cfgSolid;  // read-increment off (one color repeated)
static uint16_t s_solid = 0;           // DMA source for solid fills
static bool s_fmt16 = false;
static bool s_up = false;
static SemaphoreHandle_t s_lock = nullptr;

static inline void dcCmd()  { gpio_put(LCD_DC_PIN, 0); }
static inline void dcData() { gpio_put(LCD_DC_PIN, 1); }

// Wait for the shifter to go idle and drain/clear the RX side. Required before spi_set_format().
static void spiIdle() {
  while (spi_is_readable(LCD_SPI)) (void)spi_get_hw(LCD_SPI)->dr;
  while (spi_is_busy(LCD_SPI)) tight_loop_contents();
  while (spi_is_readable(LCD_SPI)) (void)spi_get_hw(LCD_SPI)->dr;
  spi_get_hw(LCD_SPI)->icr = SPI_SSPICR_RORIC_BITS;   // clear the RX overrun of the discarded reads
}

static inline void txWait() { dma_channel_wait_for_finish_blocking(s_dma); }
static inline void txBuf(const uint16_t *src, uint32_t n) {
  dma_channel_configure(s_dma, &s_cfgInc, &spi_get_hw(LCD_SPI)->dr, src, n, true);
}
static inline void txSolid(uint16_t c, uint32_t n) {
  s_solid = c;
  dma_channel_configure(s_dma, &s_cfgSolid, &spi_get_hw(LCD_SPI)->dr, &s_solid, n, true);
}
static void fmt16() {
  if (s_fmt16) return;
  spiIdle();
  spi_set_format(LCD_SPI, 16, SPI_CPOL_0, SPI_CPHA_0, SPI_MSB_FIRST);
  s_fmt16 = true;
}
static void fmt8() {
  if (!s_fmt16) return;
  txWait();
  spiIdle();
  spi_set_format(LCD_SPI, 8, SPI_CPOL_0, SPI_CPHA_0, SPI_MSB_FIRST);
  s_fmt16 = false;
}

static void wrCmd(uint8_t c) {
  fmt8();
  dcCmd();
  spi_write_blocking(LCD_SPI, &c, 1);   // returns once the byte has shifted out
  dcData();
}
static void wrBytes(const uint8_t *d, size_t n) {
  if (n) spi_write_blocking(LCD_SPI, d, n);
}

// CASET / RASET / RAMWR (inclusive bounds).
static void panelWindow(int x0, int y0, int x1, int y1) {
  uint8_t b[4];
  wrCmd(0x2A);
  b[0] = (uint8_t)(x0 >> 8); b[1] = (uint8_t)x0; b[2] = (uint8_t)(x1 >> 8); b[3] = (uint8_t)x1;
  wrBytes(b, 4);
  wrCmd(0x2B);
  b[0] = (uint8_t)(y0 >> 8); b[1] = (uint8_t)y0; b[2] = (uint8_t)(y1 >> 8); b[3] = (uint8_t)y1;
  wrBytes(b, 4);
  wrCmd(0x2C);
  fmt16();
}

// Before anything sets a baud rate from clk_peri (the UART console included).
void rpClockInit() {
  if (clock_get_hz(clk_peri) != clock_get_hz(clk_sys))
    clock_configure(clk_peri, 0, CLOCKS_CLK_PERI_CTRL_AUXSRC_VALUE_CLK_SYS, clock_get_hz(clk_sys),
                    clock_get_hz(clk_sys));
}

static void lcdHwInit() {
  gpio_init(LCD_CS_PIN);  gpio_set_dir(LCD_CS_PIN,  GPIO_OUT); gpio_put(LCD_CS_PIN,  1);
  gpio_init(LCD_DC_PIN);  gpio_set_dir(LCD_DC_PIN,  GPIO_OUT); gpio_put(LCD_DC_PIN,  1);
  gpio_init(LCD_RST_PIN); gpio_set_dir(LCD_RST_PIN, GPIO_OUT); gpio_put(LCD_RST_PIN, 1);

  const uint32_t hz = spi_init(LCD_SPI, LCD_SPI_HZ);
  spi_set_format(LCD_SPI, 8, SPI_CPOL_0, SPI_CPHA_0, SPI_MSB_FIRST);
  s_fmt16 = false;
  gpio_set_function(LCD_SCK_PIN,  GPIO_FUNC_SPI);
  gpio_set_function(LCD_MOSI_PIN, GPIO_FUNC_SPI);
  gpio_set_function(LCD_MISO_PIN, GPIO_FUNC_SPI);

  s_dma = dma_claim_unused_channel(true);
  s_cfgInc = dma_channel_get_default_config(s_dma);
  channel_config_set_transfer_data_size(&s_cfgInc, DMA_SIZE_16);
  channel_config_set_dreq(&s_cfgInc, spi_get_dreq(LCD_SPI, true));
  channel_config_set_read_increment(&s_cfgInc, true);
  channel_config_set_write_increment(&s_cfgInc, false);
  s_cfgSolid = s_cfgInc;
  channel_config_set_read_increment(&s_cfgSolid, false);

  gpio_put(LCD_RST_PIN, 1); delay(10);       // ClockworkPi reset timings: 10ms low, 200ms settle
  gpio_put(LCD_RST_PIN, 0); delay(10);
  gpio_put(LCD_RST_PIN, 1); delay(200);
  gpio_put(LCD_CS_PIN, 0);                   // asserted from here on, for good
  hal_log("LCD: SPI at %lu kHz", (unsigned long)(hz / 1000));
}

// ST7365P/ILI9488 init: {cmd, nargs, args...}. MADCTL 0x48, COLMOD 0x55 = RGB565, INVON (the
// panel needs inversion on).
static const uint8_t kInit[] = {
  0xE0, 15, 0x00, 0x03, 0x09, 0x08, 0x16, 0x0A, 0x3F, 0x78, 0x4C, 0x09, 0x0A, 0x08, 0x16, 0x1A, 0x0F,
  0xE1, 15, 0x00, 0x16, 0x19, 0x03, 0x0F, 0x05, 0x32, 0x45, 0x46, 0x04, 0x0E, 0x0D, 0x35, 0x37, 0x0F,
  0xC0, 2, 0x17, 0x15,
  0xC1, 1, 0x41,
  0xC5, 3, 0x00, 0x12, 0x80,
  0x36, 1, 0x48,
  0x3A, 1, 0x55,
  0xB0, 1, 0x00,
  0xB1, 1, 0xA0,
  0x21, 0,
  0xB4, 1, 0x02,
  0xB6, 3, 0x02, 0x02, 0x3B,
  0xB7, 1, 0xC6,
  0xE9, 1, 0x00,
  0xF7, 4, 0xA9, 0x51, 0x2C, 0x82,
};

void hal_lcd_fill(int x, int y, int w, int h, uint16_t c) {
  if (x < 0) { w += x; x = 0; }
  if (y < 0) { h += y; y = 0; }
  if (x + w > LCD_W) w = LCD_W - x;
  if (y + h > LCD_H) h = LCD_H - y;
  if (w <= 0 || h <= 0 || !s_up) return;
  xSemaphoreTake(s_lock, portMAX_DELAY);
  panelWindow(x, y, x + w - 1, y + h - 1);
  txSolid(c, (uint32_t)w * (uint32_t)h);
  txWait();
  xSemaphoreGive(s_lock);
}

void hal_lcd_push(int x, int y, int w, int h, const uint16_t *px, int stride) {
  if (x < 0) { px -= x; w += x; x = 0; }
  if (y < 0) { px -= y * stride; h += y; y = 0; }
  if (x + w > LCD_W) w = LCD_W - x;
  if (y + h > LCD_H) h = LCD_H - y;
  if (w <= 0 || h <= 0 || !s_up) return;
  xSemaphoreTake(s_lock, portMAX_DELAY);
  panelWindow(x, y, x + w - 1, y + h - 1);
  if (stride == w) {
    txBuf(px, (uint32_t)w * (uint32_t)h);
    txWait();
  } else {
    for (int r = 0; r < h; r++, px += stride) {
      txBuf(px, (uint32_t)w);
      txWait();
    }
  }
  xSemaphoreGive(s_lock);
}

bool rpDisplayInit() {
  s_lock = xSemaphoreCreateMutex();
  lcdHwInit();
  for (size_t i = 0; i < sizeof(kInit);) {
    const uint8_t cmd = kInit[i], n = kInit[i + 1];
    wrCmd(cmd);
    wrBytes(&kInit[i + 2], n);
    i += 2 + n;
  }
  wrCmd(0x11); delay(120);               // SLPOUT
  wrCmd(0x29); delay(120);               // DISPON
  s_up = true;
  hal_lcd_fill(0, 0, LCD_W, LCD_H, 0x0000);
  return true;
}

#endif  // ARDUINO_ARCH_RP2040
