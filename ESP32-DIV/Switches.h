#pragma once

#include <Arduino.h>
#include "utils.h"

// Input adapter for the ported games.  It deliberately routes through the
// DIV input layer so physical PCF8574 buttons and the touch footer behave as
// the same five controls and inherit the existing debounce/recovery logic.
class Switches {
 public:
  Switches() = default;
  explicit Switches(int pin, uint32_t holdLimit = 1000, bool = true,
                    uint8_t = 0)
      : pin_(pin), holdLimit_(holdLimit) {}

  int getPin() const { return pin_; }
  bool getPullup() const { return true; }
  bool isDown() const { return isButtonPressed(pin_); }

  bool justPressed() {
    const bool down = isDown();
    const uint32_t now = millis();
    const bool edge = down && !pressed_;
    if (edge) {
      pressedAt_ = now;
      held_ = false;
    } else if (down && !held_ && (uint32_t)(now - pressedAt_) >= holdLimit_) {
      held_ = true;
    }
    pressed_ = down;
    if (!down) held_ = false;
    return edge;
  }

  bool justReleased() {
    const bool down = isDown();
    const bool edge = !down && pressed_;
    pressed_ = down;
    if (!down) held_ = false;
    return edge;
  }

  bool isHeld() {
    const bool down = isDown();
    if (down && !pressed_) {
      pressed_ = true;
      pressedAt_ = millis();
    }
    if (down && !held_ && (uint32_t)(millis() - pressedAt_) >= holdLimit_) {
      held_ = true;
    }
    if (!down) {
      pressed_ = false;
      held_ = false;
    }
    return held_;
  }

 private:
  int pin_ = -1;
  uint32_t holdLimit_ = 1000;
  uint32_t pressedAt_ = 0;
  bool pressed_ = false;
  bool held_ = false;
};

extern Switches u_btn;
extern Switches d_btn;
extern Switches l_btn;
extern Switches r_btn;
extern Switches c_btn;

