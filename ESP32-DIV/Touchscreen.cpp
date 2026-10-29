#include "SettingsStore.h"
#include "Touchscreen.h"
#include <TFT_eSPI.h>

#include "LocalizedTFT.h"
extern LocalizedTFT tft;

#if defined(BOARD_CYD) || defined(BOARD_ESP32_DIV_V1)
// Dedicated VSPI bus for XPT2046 — must not share HSPI with TFT_eSPI on classic ESP32.
SPIClass touchscreenSPI = SPIClass(VSPI);
#else
SPIClass touchscreenSPI = SPIClass(HSPI);
#endif

#ifndef XPT2046_IRQ
#define XPT2046_IRQ 255
#endif

XPT2046_Touchscreen ts(XPT2046_CS, XPT2046_IRQ);
bool feature_active = false;

static bool s_touchInitialized = false;
static bool s_touchSpiBegun = false;

#ifndef TOUCH_ROTATION
#if defined(BOARD_ESP32_DIV_V2)
// The DIV 2.1 panel is mounted 90 degrees relative to the generic XPT2046
// convention used by TFT_eSPI's raw reader. Four-corner measurements on the
// production PCB show X decreasing left-to-right and Y decreasing top-to-bottom
// before this transform; rotation 3 makes screen X/Y advance on the same axes.
#define TOUCH_ROTATION 3
#else
#define TOUCH_ROTATION TFT_ROTATION
#endif
#endif

#if TOUCH_USES_TFT_SPI
static void applyTouchRotation(int16_t rawX, int16_t rawY, int16_t& x, int16_t& y) {
  switch (TOUCH_ROTATION) {
    case 0: x = 4095 - rawY; y = rawX; break;
    case 1: x = rawX; y = rawY; break;
    case 2: x = rawY; y = 4095 - rawX; break;
    default: x = 4095 - rawX; y = 4095 - rawY; break;
  }
}

static bool readSharedTouchSample(int16_t& x, int16_t& y, int16_t& z, uint16_t zThresh) {
  if (!s_touchInitialized) {
    return false;
  }

  tft.endWrite();
  z = (int16_t)tft.getTouchRawZ();
  if (z < (int16_t)zThresh) {
    x = 0;
    y = 0;
    return false;
  }

  // Match the vendor/official DIV 1.7.2 reader exactly. TFT_eSPI already
  // oversamples and filters each XPT2046 conversion internally; requesting
  // three complete samples here made a single physical tap span several SPI
  // transactions and produced displaced/phantom coordinates on DIV 2.1.
  uint16_t rawX = 0;
  uint16_t rawY = 0;
  tft.getTouchRaw(&rawX, &rawY);
  applyTouchRotation((int16_t)rawX, (int16_t)rawY, x, y);
  return true;
}
#endif

static void ensureTouchSpiReady() {
#if !TOUCH_USES_TFT_SPI
  if (!s_touchSpiBegun) {
    touchscreenSPI.begin(XPT2046_CLK, XPT2046_MISO, XPT2046_MOSI, XPT2046_CS);
    s_touchSpiBegun = true;
  }
#endif
}

static int16_t median3(int16_t a, int16_t b, int16_t c) {
  if (a > b) { const int16_t t = a; a = b; b = t; }
  if (b > c) { const int16_t t = b; b = c; c = t; }
  if (a > b) { const int16_t t = a; a = b; b = t; }
  return b;
}

static int16_t bestTwoAverage(int16_t a, int16_t b, int16_t c) {
  const int16_t ab = abs(a - b);
  const int16_t ac = abs(a - c);
  const int16_t bc = abs(b - c);
  if (ab <= ac && ab <= bc) return (a + b) >> 1;
  if (ac <= ab && ac <= bc) return (a + c) >> 1;
  return (b + c) >> 1;
}

static bool readDedicatedTouchSample(uint16_t zThresh, int16_t& rawX, int16_t& rawY) {
  int16_t data[6] = {};
  int32_t z = 0;
  touchscreenSPI.beginTransaction(SPISettings(2000000, MSBFIRST, SPI_MODE0));
  digitalWrite(XPT2046_CS, LOW);
  touchscreenSPI.transfer(0xB1);
  const int16_t z1 = touchscreenSPI.transfer16(0xC1) >> 3;
  z = z1 + 4095;
  const int16_t z2 = touchscreenSPI.transfer16(0x91) >> 3;
  z -= z2;
  if (z >= (int32_t)zThresh) {
    touchscreenSPI.transfer16(0x91);
    data[0] = touchscreenSPI.transfer16(0xD1) >> 3;
    data[1] = touchscreenSPI.transfer16(0x91) >> 3;
    data[2] = touchscreenSPI.transfer16(0xD1) >> 3;
    data[3] = touchscreenSPI.transfer16(0x91) >> 3;
  }
  data[4] = touchscreenSPI.transfer16(0xD0) >> 3;
  data[5] = touchscreenSPI.transfer16(0) >> 3;
  digitalWrite(XPT2046_CS, HIGH);
  touchscreenSPI.endTransaction();

  if (z < (int32_t)zThresh) return false;
  const int16_t x = bestTwoAverage(data[0], data[2], data[4]);
  const int16_t y = bestTwoAverage(data[1], data[3], data[5]);
  switch (TOUCH_ROTATION) {
    case 0: rawX = 4095 - y; rawY = x; break;
    case 1: rawX = x; rawY = y; break;
    case 2: rawX = y; rawY = 4095 - x; break;
    default: rawX = 4095 - x; rawY = 4095 - y; break;
  }
  return true;
}

static bool touchSampleOk(uint16_t zThresh, int16_t& rawX, int16_t& rawY) {
#if TOUCH_USES_TFT_SPI
  int16_t z = 0;
  return readSharedTouchSample(rawX, rawY, z, zThresh);
#else
  ensureTouchSpiReady();
#if defined(XPT2046_IRQ) && (XPT2046_IRQ < 255)
  if (!ts.tirqTouched()) {
    return false;
  }
#endif
  // Each raw conversion already rejects one of three outliers; a median of
  // three complete conversions removes occasional shared-clock spikes.
  int16_t xs[3];
  int16_t ys[3];
  for (uint8_t i = 0; i < 3; ++i) {
    if (!readDedicatedTouchSample(zThresh, xs[i], ys[i])) return false;
    if (i < 2) delayMicroseconds(250);
  }
  const int16_t minX = min(xs[0], min(xs[1], xs[2]));
  const int16_t maxX = max(xs[0], max(xs[1], xs[2]));
  const int16_t minY = min(ys[0], min(ys[1], ys[2]));
  const int16_t maxY = max(ys[0], max(ys[1], ys[2]));
  if ((maxX - minX) > 650 || (maxY - minY) > 650) return false;
  rawX = median3(xs[0], xs[1], xs[2]);
  rawY = median3(ys[0], ys[1], ys[2]);
  return true;
#endif
}

void setupTouchscreen() {
  if (s_touchInitialized) {
    return;
  }

#if TOUCH_USES_TFT_SPI
  pinMode(XPT2046_CS, OUTPUT);
  digitalWrite(XPT2046_CS, HIGH);
#else
  // Bind HSPI once with the actual DIV 2.1 pins.  XPT2046_Touchscreen::begin()
  // calls SPI.begin() without pins on ESP32-S3 and both re-registers the APB
  // callback and tries invalid default pins, so dedicated reads are performed
  // directly above.
  ensureTouchSpiReady();
#endif

  s_touchInitialized = true;
}

extern XPT2046_Touchscreen ts;

bool isTouchDown(uint16_t zThresh) {
  int16_t x = 0;
  int16_t y = 0;
  return touchSampleOk(zThresh, x, y);
}

bool isTouchDownDismiss(uint16_t zThresh) {
  return isTouchDown(zThresh);
}

bool readTouchRawXY(int16_t& x, int16_t& y, uint16_t zThresh) {
  return touchSampleOk(zThresh, x, y);
}

static void mapTouchToScreen(int16_t rawX, int16_t rawY, int& x, int& y) {
  auto& s = settings();
#if defined(BOARD_CYD)
  // Same axis order as the RNT CYD touch test (no inverted Y).
  x = ::map(rawX, s.touchXMin, s.touchXMax, 0, TFT_WIDTH - 1);
  y = ::map(rawY, s.touchYMin, s.touchYMax, 0, TFT_HEIGHT - 1);
#else
  x = ::map(rawX, s.touchXMin, s.touchXMax, 0, TFT_WIDTH - 1);
  y = ::map(rawY, s.touchYMax, s.touchYMin, 0, TFT_HEIGHT - 1);
#endif
  x = constrain(x, 0, TFT_WIDTH - 1);
  y = constrain(y, 0, TFT_HEIGHT - 1);
}

bool readTouchXY(int& x, int& y) {
  int16_t rawX = 0;
  int16_t rawY = 0;
  if (!touchSampleOk(200, rawX, rawY)) {
    return false;
  }
  mapTouchToScreen(rawX, rawY, x, y);
  return true;
}

bool readTouchXYDismiss(int& x, int& y) {
  int16_t rawX = 0;
  int16_t rawY = 0;
  if (!touchSampleOk(120, rawX, rawY)) {
    return false;
  }
  mapTouchToScreen(rawX, rawY, x, y);
  return true;
}
