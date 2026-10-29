#pragma once

#include <TFT_eSPI.h>
#include "GamePalette.h"

// Allocate the indexed 240x300 surface once during early boot, before the
// radio stacks fragment the internal heap. Every ported game reuses it, so
// entering a game never performs a large late allocation.
bool reserveGameFrameBuffer(TFT_eSPI& display, int16_t width, int16_t height);
void releaseReservedGameFrameBuffer();

class GameFrameBuffer {
 public:
  explicit GameFrameBuffer(TFT_eSPI& display) : display_(display) {}
  ~GameFrameBuffer() { end(); }

  bool begin(int16_t width, int16_t height);
  void end();
  TFT_eSprite& canvas();
  void present();

 private:
  TFT_eSPI& display_;
  bool ready_ = false;
};

