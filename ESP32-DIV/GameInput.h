#pragma once

#include <Arduino.h>
#include "Switches.h"

namespace GameInput {

constexpr uint16_t GAME_DEBOUNCE_MS = 25;
constexpr uint16_t GAME_HOLD_MS = 1000;

inline bool isDown(Switches& button) { return button.isDown(); }

inline void waitForRelease(Switches& button) {
  bool releaseTiming = false;
  uint32_t releasedAt = 0;
  while (button.isDown() || !releaseTiming ||
         (uint32_t)(millis() - releasedAt) < GAME_DEBOUNCE_MS) {
    if (button.isDown()) {
      releaseTiming = false;
    } else if (!releaseTiming) {
      releaseTiming = true;
      releasedAt = millis();
    }
    button.justPressed();
    delay(3);
  }
}

enum class CenterEvent : uint8_t { None, Tap, Hold };

class TapHoldButton {
 public:
  explicit TapHoldButton(Switches& button) : button_(button) {}

  void reset(uint32_t now = millis()) {
    rawDown_ = button_.isDown();
    stableDown_ = rawDown_;
    rawChangedAt_ = pressedAt_ = now;
    holdReported_ = false;
    button_.justPressed();
  }

  CenterEvent poll(uint32_t now) {
    const bool rawDown = button_.isDown();
    button_.justPressed();
    if (rawDown != rawDown_) {
      rawDown_ = rawDown;
      rawChangedAt_ = now;
    }
    if (rawDown_ != stableDown_ &&
        (uint32_t)(now - rawChangedAt_) >= GAME_DEBOUNCE_MS) {
      stableDown_ = rawDown_;
      if (stableDown_) {
        pressedAt_ = now;
        holdReported_ = false;
      } else if (!holdReported_) {
        return CenterEvent::Tap;
      }
    }
    if (stableDown_ && !holdReported_ &&
        (uint32_t)(now - pressedAt_) >= GAME_HOLD_MS) {
      holdReported_ = true;
      return CenterEvent::Hold;
    }
    return CenterEvent::None;
  }

 private:
  Switches& button_;
  bool rawDown_ = false;
  bool stableDown_ = false;
  bool holdReported_ = false;
  uint32_t rawChangedAt_ = 0;
  uint32_t pressedAt_ = 0;
};

}  // namespace GameInput

