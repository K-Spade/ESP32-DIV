#pragma once

#include "DivRfEngine.h"

#include <Arduino.h>

// UI-independent adapter. The DIV screen should use this controller instead
// of owning RF24 instances or touching the shared SPI bus.
namespace DivRfProfilesController {

constexpr uint8_t kProfileCount = 8;

void reset(uint8_t initialIndex = 0);
uint8_t selectedIndex();
DivRfEngine::Profile selectedProfile();
const char* selectedName();

// Changes selection. If the engine is active it performs one controlled
// stop/restart transition and leaves carriers off if the restart fails.
bool selectIndex(uint8_t index);
bool selectNext();
bool selectPrevious();

bool toggle();
bool startSelected();
void service();
void exit();

}  // namespace DivRfProfilesController
