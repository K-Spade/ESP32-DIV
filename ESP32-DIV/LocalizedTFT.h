#pragma once

#include <TFT_eSPI.h>
#include "Language.h"
#include "UnicodeTextRenderer.h"

// Translate strings at the display boundary. This makes existing feature
// screens participate in the language system without duplicating drawing code.
// Dynamic values and serial diagnostics remain untouched.
class LocalizedTFT : public TFT_eSPI {
 public:
  using TFT_eSPI::TFT_eSPI;
  using TFT_eSPI::drawCentreString;
  using TFT_eSPI::drawRightString;
  using TFT_eSPI::drawString;
  using TFT_eSPI::print;
  using TFT_eSPI::println;
  using TFT_eSPI::textWidth;

  size_t print(const char* text) {
    const char* value = tr(text);
    UnicodeTextMetrics metrics;
    if (unicodeTextMetrics(value, metrics)) {
      unicodeTextDraw(*this, value, cursor_x, cursor_y,
                      (uint16_t)textcolor, (uint16_t)textbgcolor);
      cursor_x += metrics.width;
      return text ? strlen(text) : 0;
    }
    return TFT_eSPI::print(value);
  }
  size_t print(const String& text) { return print(text.c_str()); }
  size_t println(const char* text) {
    const char* value = tr(text);
    UnicodeTextMetrics metrics;
    if (unicodeTextMetrics(value, metrics)) {
      unicodeTextDraw(*this, value, cursor_x, cursor_y,
                      (uint16_t)textcolor, (uint16_t)textbgcolor);
      // Generated Unicode assets use a 20 px line box.  TFT_eSPI's empty
      // println() advances only the active ASCII font height (normally 8 px),
      // which made Chinese/Japanese/Thai/Korean/Russian lines overlap.  Advance
      // by the real rendered height and retain a small inter-line gap.
      int16_t advance = (int16_t)fontHeight();
      if ((int16_t)metrics.height > advance) {
        advance = (int16_t)metrics.height;
      }
      advance += 2;
      cursor_x = 0;
      cursor_y += advance;
      return (text ? strlen(text) : 0u) + 1u;
    }
    return TFT_eSPI::println(value);
  }
  size_t println(const String& text) { return println(text.c_str()); }

  int16_t drawString(const char* text, int32_t x, int32_t y, uint8_t font) {
    const char* value = tr(text);
    UnicodeTextMetrics metrics;
    if (!unicodeTextMetrics(value, metrics)) {
      return TFT_eSPI::drawString(value, x, y, font);
    }
    adjustDatum(x, y, metrics);
    return unicodeTextDraw(*this, value, x, y, (uint16_t)textcolor,
                           (uint16_t)textbgcolor);
  }
  int16_t drawString(const char* text, int32_t x, int32_t y) {
    return drawString(text, x, y, textfont);
  }
  int16_t drawString(const String& text, int32_t x, int32_t y, uint8_t font) {
    return drawString(text.c_str(), x, y, font);
  }
  int16_t drawString(const String& text, int32_t x, int32_t y) {
    return drawString(text.c_str(), x, y, textfont);
  }
  int16_t textWidth(const char* text, uint8_t font) {
    const char* value = tr(text);
    UnicodeTextMetrics metrics;
    return unicodeTextMetrics(value, metrics) ? metrics.width
                                               : TFT_eSPI::textWidth(value, font);
  }
  int16_t textWidth(const char* text) {
    return textWidth(text, textfont);
  }
  int16_t textWidth(const String& text, uint8_t font) {
    return textWidth(text.c_str(), font);
  }
  int16_t textWidth(const String& text) {
    return textWidth(text.c_str(), textfont);
  }
  int16_t drawCentreString(const char* text, int32_t x, int32_t y,
                           uint8_t font) {
    const char* value = tr(text);
    UnicodeTextMetrics metrics;
    if (unicodeTextMetrics(value, metrics)) {
      return unicodeTextDraw(*this, value, x - metrics.width / 2, y,
                             (uint16_t)textcolor, (uint16_t)textbgcolor);
    }
    return TFT_eSPI::drawCentreString(value, x, y, font);
  }
  int16_t drawCentreString(const String& text, int32_t x, int32_t y,
                           uint8_t font) {
    return drawCentreString(text.c_str(), x, y, font);
  }
  int16_t drawRightString(const char* text, int32_t x, int32_t y,
                          uint8_t font) {
    const char* value = tr(text);
    UnicodeTextMetrics metrics;
    if (unicodeTextMetrics(value, metrics)) {
      return unicodeTextDraw(*this, value, x - metrics.width, y,
                             (uint16_t)textcolor, (uint16_t)textbgcolor);
    }
    return TFT_eSPI::drawRightString(value, x, y, font);
  }
  int16_t drawRightString(const String& text, int32_t x, int32_t y,
                          uint8_t font) {
    return drawRightString(text.c_str(), x, y, font);
  }

 private:
  void adjustDatum(int32_t& x, int32_t& y,
                   const UnicodeTextMetrics& metrics) const {
    switch (textdatum) {
      case TC_DATUM:
      case MC_DATUM:
      case BC_DATUM: x -= metrics.width / 2; break;
      case TR_DATUM:
      case MR_DATUM:
      case BR_DATUM: x -= metrics.width; break;
      default: break;
    }
    switch (textdatum) {
      case ML_DATUM:
      case MC_DATUM:
      case MR_DATUM: y -= metrics.height / 2; break;
      case BL_DATUM:
      case BC_DATUM:
      case BR_DATUM: y -= metrics.height; break;
      default: break;
    }
  }
};
