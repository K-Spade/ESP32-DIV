#pragma once

#include <Arduino.h>
#include <TFT_eSPI.h>

struct UnicodeTextMetrics {
  uint16_t width = 0;
  uint16_t height = 0;
};

bool unicodeTextMetrics(const char* utf8, UnicodeTextMetrics& out);
int16_t unicodeTextDraw(TFT_eSPI& display, const char* utf8, int32_t x,
                        int32_t y, uint16_t foreground,
                        uint16_t background);
