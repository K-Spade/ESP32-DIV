#ifndef SUBCONFIG_H
#define SUBCONFIG_H

#include <EEPROM.h>
#include <ELECHOUSE_CC1101_SRC_DRV.h>
#include <PCF8574.h>
#include <RCSwitch.h>
#include <SPI.h>
#include <TFT_eSPI.h>
#include <Wire.h>
#include <cstddef>
#include <stdio.h>
#include <string>
#include "arduinoFFT.h"
#include "shared.h"
#include "utils.h"

#include "LocalizedTFT.h"
extern LocalizedTFT tft;
extern PCF8574 pcf;

namespace replayat {
  void ReplayAttackSetup();
  void ReplayAttackLoop();
}

namespace SavedProfile {
  void saveSetup();
  void saveLoop();
}

namespace subjammer {
  void subjammerSetup();
  void subjammerLoop();
}

namespace SubBrute {
  void subBruteSetup();
  void subBruteLoop();
}

namespace jammingdetector {
  void Setup();
  void Loop();
}

namespace SubGhzLifecycle {
  /** Stop RMT/ISR/CC1101 activity and optionally restore the SD bus. */
  void shutdown(bool restoreSd = true);
}

#endif
