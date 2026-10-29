#pragma once

#include <Arduino.h>
#include "shared.h"
#include "Language.h"

#ifndef SETTINGS_PATH
#define SETTINGS_PATH "/config/settings.json"
#endif

struct AppSettings {

  uint8_t  brightness = BKL_LEVEL_MED;
  Theme    theme      = Theme::Dark;
  // MK Green is the factory/default accent (preset index 2). Existing saved
  // user selections remain respected when settings.json is loaded.
  uint8_t  accentColor = 2;
  UiLanguage language = UiLanguage::English;

  bool     neopixelEnabled = false;
  bool     soundEnabled    = true;

  // Background scans are opt-in: keeping both 2.4 GHz stacks running at the
  // menu wastes battery and can contend with GPS/SD/nRF feature startup.
  bool     autoWifiScan    = false;
  bool     autoBleScan     = false;

  // User preference: 2 keeps NRF3 electrically idle; 3 means ALL.  The
  // supervisor silently caps ALL to the physically detected radio count.
  uint8_t  activeNrfCount = 3;

  uint16_t touchXMin = TOUCH_X_MIN;
  uint16_t touchXMax = TOUCH_X_MAX;
  uint16_t touchYMin = TOUCH_Y_MIN;
  uint16_t touchYMax = TOUCH_Y_MAX;

};

struct AccentOption {
  const char* name;
  uint16_t color565;
};

constexpr uint8_t ACCENT_PRESET_COUNT = 8;
AppSettings& settings();
uint8_t accentPresetClamp(uint8_t preset);
uint16_t accentColor565(uint8_t preset);
const char* accentPresetName(uint8_t preset);
uint8_t activeNrfCountClamp(uint8_t count);
/** True only when ALL is selected and NRF3 was physically detected. */
bool thirdNrfEnabled();
/** Apply the persisted radio selection without talking to any nRF24. */
void nrfApplyActiveRadioSelection();
/** Load/save the 2/ALL preference from internal NVS. NRF Tuning deliberately
 *  avoids the shared microSD/SPI bus. */
bool nrfLoadActiveRadioSelection();
bool nrfSaveActiveRadioSelection();

/** Load/save the language shadow used before the SD-backed settings file is
 * available.  The NFC boot gate runs before SD initialisation, so it must read
 * this single byte from internal NVS to render in the selected language. */
bool settingsLoadBootLanguage();
bool settingsSaveBootLanguage();
/** Load the early language/theme/touch snapshot without mounting the SD card.
 * Used by the PIN keypad before the remaining peripherals are initialised. */
bool settingsLoadEarlyUi();

const char* settingsBoardProfileId();
void settingsApplyBoardTouchDefaults();
bool settingsLoad();
bool settingsSave();
