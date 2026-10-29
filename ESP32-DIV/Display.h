#pragma once

#include <TFT_eSPI.h>
#include "LocalizedTFT.h"

extern LocalizedTFT tft;

// Small adapter matching the display_obj.tft surface used by the upstream
// games without importing Marauder's menu, SD or radio subsystems.
class Display {
 public:
  explicit Display(TFT_eSPI& screen) : tft(screen) {}
  TFT_eSPI& tft;
};

extern Display display_obj;

