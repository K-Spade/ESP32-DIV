#include "GameFrameBuffer.h"

#include "utils.h"

namespace {
TFT_eSprite* s_gameSprite = nullptr;
TFT_eSPI* s_gameDisplay = nullptr;
int16_t s_reservedWidth = 0;
int16_t s_reservedHeight = 0;

void installPalette(TFT_eSprite& sprite) {
  static const uint16_t palette[16] = {
      0x0000, 0x000F, 0x03E0, 0x03EF,
      0x7800, 0x780F, 0xFD20, 0xC618,
      0x7BEF, 0x001F, 0x07E0, 0x07FF,
      0xF800, 0xF81F, 0xFFE0, 0xFFFF,
  };
  for (uint8_t i = 0; i < 16; ++i) sprite.setPaletteColor(i, palette[i]);
}
}

bool reserveGameFrameBuffer(TFT_eSPI& display, int16_t width, int16_t height) {
  if (s_gameSprite && s_gameDisplay == &display &&
      s_reservedWidth == width && s_reservedHeight == height) {
    return true;
  }

  releaseReservedGameFrameBuffer();
  s_gameDisplay = &display;
  s_gameSprite = new TFT_eSprite(&display);
  if (!s_gameSprite) return false;

  s_gameSprite->setColorDepth(4);
  if (!s_gameSprite->createSprite(width, height)) {
    delete s_gameSprite;
    s_gameSprite = nullptr;
    s_gameDisplay = nullptr;
    return false;
  }

  s_reservedWidth = width;
  s_reservedHeight = height;
  installPalette(*s_gameSprite);
  s_gameSprite->setTextFont(1);
  s_gameSprite->setTextWrap(false);
  s_gameSprite->fillSprite(0);
  Serial.printf("[games] reserved 4-bit framebuffer %dx%d\n", width, height);
  return true;
}

void releaseReservedGameFrameBuffer() {
  if (s_gameSprite) {
    s_gameSprite->deleteSprite();
    delete s_gameSprite;
  }
  s_gameSprite = nullptr;
  s_gameDisplay = nullptr;
  s_reservedWidth = 0;
  s_reservedHeight = 0;
}

bool GameFrameBuffer::begin(int16_t width, int16_t height) {
  ready_ = reserveGameFrameBuffer(display_, width, height);
  if (!ready_) return false;
  s_gameSprite->setTextFont(1);
  s_gameSprite->setTextWrap(false);
  s_gameSprite->fillSprite(0);
  return true;
}

void GameFrameBuffer::end() {
  // Retain the allocation for the next game to keep the heap layout stable.
  ready_ = false;
}

TFT_eSprite& GameFrameBuffer::canvas() { return *s_gameSprite; }

void GameFrameBuffer::present() {
  if (!ready_ || !s_gameSprite) return;
  s_gameSprite->pushSprite(0, 0);
  redrawTouchButtonBar();
}
