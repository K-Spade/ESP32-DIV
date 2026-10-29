#include <ArduinoJson.h>
#include <Preferences.h>
#include <SD.h>
#include "NrfSupervisor.h"
#include "SettingsStore.h"
#include "utils.h"


static AppSettings g_settings;
AppSettings& settings() { return g_settings; }

static const AccentOption kAccentPresets[] = {
  {"Orange", 0xFBE4},
  {"Green",  0xB721},
  {"MK Green", MK_SIGNAL_GREEN},
  {"Red",    0xF800},
  {"Cyan",   0x07FF},
  {"Purple", 0xF81F},
  {"Yellow", 0xFFE0},
  {"White",  0xFFFF},
};

uint8_t accentPresetClamp(uint8_t preset) {
  if (preset >= ACCENT_PRESET_COUNT) return 0;
  return preset;
}

uint16_t accentColor565(uint8_t preset) {
  return kAccentPresets[accentPresetClamp(preset)].color565;
}

const char* accentPresetName(uint8_t preset) {
  return kAccentPresets[accentPresetClamp(preset)].name;
}

uint8_t activeNrfCountClamp(uint8_t count) {
  return count == 2 ? 2 : 3;
}

bool thirdNrfEnabled() {
  return activeNrfCountClamp(g_settings.activeNrfCount) == 3 &&
         nrfDetectedHardwareCount() >= 3u;
}

void nrfApplyActiveRadioSelection() {
  g_settings.activeNrfCount = activeNrfCountClamp(g_settings.activeNrfCount);
  // Setting the inactive radio's CE low and CSN high is sufficient to keep it
  // out of RX/TX and off the SPI bus. Do not call begin()/powerDown(): those
  // commands would still select and communicate with NRF3.
  if (!thirdNrfEnabled()) {
    pinMode(CE_PIN_3, OUTPUT);
    digitalWrite(CE_PIN_3, LOW);
    if (nrfDetectedHardwareCount() < 3u && nrfIrHardwareAvailable()) {
      // GPIO21 is VS1838B OUT on the original two-radio PCB. Driving it HIGH
      // masked every received pulse and produced the misleading "GPIO
      // reserved" behaviour. Leave only that net as an input; GPIO14 remains
      // LOW so the IR transistor is off until an IR feature starts.
      pinMode(CSN_PIN_3, INPUT_PULLUP);
    } else {
      pinMode(CSN_PIN_3, OUTPUT);
      digitalWrite(CSN_PIN_3, HIGH);
    }
  }
}

bool nrfLoadActiveRadioSelection() {
  Preferences prefs;
  if (!prefs.begin("mk-signal", true)) return false;
  const uint8_t saved = prefs.getUChar("nrfCount", 0xFFu);
  prefs.end();
  if (saved != 2u && saved != 3u) return false;
  g_settings.activeNrfCount = saved;
  nrfApplyActiveRadioSelection();
  return true;
}

bool nrfSaveActiveRadioSelection() {
  Preferences prefs;
  if (!prefs.begin("mk-signal", false)) return false;
  const uint8_t value = activeNrfCountClamp(g_settings.activeNrfCount);
  const size_t written = prefs.putUChar("nrfCount", value);
  prefs.end();
  return written == sizeof(uint8_t);
}

bool settingsLoadBootLanguage() {
  Preferences prefs;
  if (!prefs.begin("mk-signal", true)) return false;
  const uint8_t saved = prefs.getUChar("language", 0xFFu);
  prefs.end();
  if (saved >= UI_LANGUAGE_COUNT) return false;
  g_settings.language = (UiLanguage)saved;
  uiSetLanguage(g_settings.language);
  return true;
}

bool settingsSaveBootLanguage() {
  Preferences prefs;
  if (!prefs.begin("mk-signal", false)) return false;
  const uint8_t value = (uint8_t)g_settings.language;
  const size_t written = prefs.putUChar("language", value);
  prefs.end();
  return written == sizeof(uint8_t);
}

static bool touchCalibrationSane(const AppSettings& s);

// Keep the user-interface settings in internal NVS as the authoritative,
// bus-independent copy. The SD JSON remains a human-readable mirror, but a
// busy or marginal card must never make the Settings screen report a false
// save or discard a toggle on the next boot.
static bool settingsLoadNvsSnapshot() {
  Preferences prefs;
  if (!prefs.begin("mk-signal", true)) return false;
  const uint8_t version = prefs.getUChar("settingsV", 0u);
  if (version != 1u) {
    prefs.end();
    return false;
  }

  auto& s = g_settings;
  s.brightness = prefs.getUChar("bright", s.brightness);
  s.theme = (Theme)constrain((int)prefs.getUChar("theme", (uint8_t)s.theme), 0, 1);
  s.accentColor = accentPresetClamp(prefs.getUChar("accent", s.accentColor));
  s.language = (UiLanguage)constrain((int)prefs.getUChar("lang", (uint8_t)s.language),
                                    0, (int)UI_LANGUAGE_COUNT - 1);
  s.neopixelEnabled = prefs.getBool("pixels", s.neopixelEnabled);
  s.soundEnabled = prefs.getBool("sound", s.soundEnabled);
  const bool autoScan = prefs.getBool("autoScan", false);
  s.autoWifiScan = autoScan;
  s.autoBleScan = autoScan;
  s.touchXMin = prefs.getUShort("touchX0", s.touchXMin);
  s.touchXMax = prefs.getUShort("touchX1", s.touchXMax);
  s.touchYMin = prefs.getUShort("touchY0", s.touchYMin);
  s.touchYMax = prefs.getUShort("touchY1", s.touchYMax);
  prefs.end();

  if (!touchCalibrationSane(s)) settingsApplyBoardTouchDefaults();
  uiSetLanguage(s.language);
  return true;
}

bool settingsLoadEarlyUi() {
  settingsApplyBoardTouchDefaults();
  if (settingsLoadNvsSnapshot()) return true;
  return settingsLoadBootLanguage();
}

static bool settingsSaveNvsSnapshot() {
  Preferences prefs;
  if (!prefs.begin("mk-signal", false)) return false;
  const auto& s = g_settings;
  bool ok = true;
  ok &= prefs.putUChar("bright", s.brightness) == sizeof(uint8_t);
  ok &= prefs.putUChar("theme", (uint8_t)s.theme) == sizeof(uint8_t);
  ok &= prefs.putUChar("accent", accentPresetClamp(s.accentColor)) == sizeof(uint8_t);
  ok &= prefs.putUChar("lang", (uint8_t)s.language) == sizeof(uint8_t);
  ok &= prefs.putBool("pixels", s.neopixelEnabled) == sizeof(bool);
  ok &= prefs.putBool("sound", s.soundEnabled) == sizeof(bool);
  ok &= prefs.putBool("autoScan", s.autoWifiScan || s.autoBleScan) == sizeof(bool);
  ok &= prefs.putUShort("touchX0", s.touchXMin) == sizeof(uint16_t);
  ok &= prefs.putUShort("touchX1", s.touchXMax) == sizeof(uint16_t);
  ok &= prefs.putUShort("touchY0", s.touchYMin) == sizeof(uint16_t);
  ok &= prefs.putUShort("touchY1", s.touchYMax) == sizeof(uint16_t);
  ok &= prefs.putUChar("settingsV", 1u) == sizeof(uint8_t);
  prefs.end();
  return ok;
}

const char* settingsBoardProfileId() {
  return TOUCH_PROFILE_ID;
}

void settingsApplyBoardTouchDefaults() {
  auto& s = g_settings;
  s.touchXMin = TOUCH_X_MIN;
  s.touchXMax = TOUCH_X_MAX;
  s.touchYMin = TOUCH_Y_MIN;
  s.touchYMax = TOUCH_Y_MAX;
}

static bool settingsTouchSavedForBoard(const StaticJsonDocument<512>& doc) {
  JsonObjectConst touch = doc["touch"];
  if (touch.isNull()) {
    return false;
  }
  if (!touch["xMin"].is<uint16_t>() || !touch["xMax"].is<uint16_t>() ||
      !touch["yMin"].is<uint16_t>() || !touch["yMax"].is<uint16_t>()) {
    return false;
  }
  const char* savedBoard = doc["board"] | "";
  if (savedBoard[0] == '\0') {
    return true;
  }
  return strcmp(savedBoard, TOUCH_PROFILE_ID) == 0;
}

static bool touchCalibrationSane(const AppSettings& s) {
  const int xSpan = abs((int)s.touchXMax - (int)s.touchXMin);
  const int ySpan = abs((int)s.touchYMax - (int)s.touchYMin);
  return s.touchXMin <= 4095 && s.touchXMax <= 4095 &&
         s.touchYMin <= 4095 && s.touchYMax <= 4095 &&
         xSpan >= 1800 && ySpan >= 1800;
}

static bool sd_mounted = false;
static bool mountSD() {
  sd_mounted = isSDCardAvailable();
  return sd_mounted;
}

static bool ensureDir(const char* dirPath) {
  if (!mountSD()) return false;
  if (!SD.exists(dirPath)) {
    if (SD.mkdir(dirPath)) return true;

    if (dirPath && dirPath[0] == '/') {
      return SD.mkdir(dirPath + 1);
    }
    return false;
  }
  return true;
}

bool settingsLoad() {
  settingsApplyBoardTouchDefaults();
  // NRF Tuning is stored independently in internal NVS so changing 2/ALL
  // never needs to access the shared SD/radio SPI bus. Keep the JSON field as
  // a migration fallback for installations that predate this separation.
  const bool nrfLoadedFromNvs = nrfLoadActiveRadioSelection();
  const bool settingsLoadedFromNvs = settingsLoadNvsSnapshot();
  sdRetryMount();
  SdFileIoGuard io(1500);
  if (!io.locked()) return settingsLoadedFromNvs;
  // initSDCard() already mounted and validated the card immediately before
  // settingsLoad() during boot. Calling restoreSdAfterSharedSpi() here used
  // SPI.end() under that live FatFS mount, so the first mount reported OK and
  // the very next settings read poisoned diskio with no-token/select errors.
  // mountSD() preserves a healthy binding and performs recovery itself when
  // the first mount really failed.
  if (!mountSD()) {
    Serial.printf("[touch] source=factory board=%s X=%u..%u Y=%u..%u (SD unavailable)\n",
                  TOUCH_PROFILE_ID, g_settings.touchXMin, g_settings.touchXMax,
                  g_settings.touchYMin, g_settings.touchYMax);
    return settingsLoadedFromNvs;
  }
  const char* sourcePath = SETTINGS_PATH;
  if (!SD.exists(sourcePath) && SD.exists("/config/settings.bak")) {
    sourcePath = "/config/settings.bak";
  }
  if (!SD.exists(sourcePath)) {
    Serial.printf("[touch] source=factory board=%s X=%u..%u Y=%u..%u (no settings file)\n",
                  TOUCH_PROFILE_ID, g_settings.touchXMin, g_settings.touchXMax,
                  g_settings.touchYMin, g_settings.touchYMax);
    return true;
  }

  File f = SD.open(sourcePath, FILE_READ);
  if (!f) return settingsLoadedFromNvs;

  StaticJsonDocument<512> doc;
  DeserializationError err = deserializeJson(doc, f);
  f.close();
  if (err && strcmp(sourcePath, SETTINGS_PATH) == 0 &&
      SD.exists("/config/settings.bak")) {
    doc.clear();
    File backup = SD.open("/config/settings.bak", FILE_READ);
    if (backup) {
      err = deserializeJson(doc, backup);
      backup.close();
      if (!err) sourcePath = "/config/settings.bak";
    }
  }
  if (err) return settingsLoadedFromNvs;

  auto& s = g_settings;
  s.brightness      = doc["brightness"]      | s.brightness;
  s.theme           = (Theme)(uint8_t)(doc["theme"] | (uint8_t)s.theme);
  s.accentColor     = accentPresetClamp(doc["accentColor"] | s.accentColor);
  s.language        = (UiLanguage)constrain((int)(doc["language"] | 0),
                                           0, (int)UI_LANGUAGE_COUNT - 1);
  uiSetLanguage(s.language);
  // Migrate the SD setting into the early-boot language shadow used by the
  // PIN lock screen on the next restart.
  settingsSaveBootLanguage();
  s.neopixelEnabled = doc["neopixelEnabled"] | s.neopixelEnabled;
  s.soundEnabled    = doc["soundEnabled"]    | s.soundEnabled;

  // Old settings files enabled background scans by default. Require an
  // explicit opt-in marker written by this stable build.
  const bool backgroundOptIn = doc["backgroundScanOptIn"] | false;
  s.autoWifiScan = backgroundOptIn && (doc["autoWifiScan"] | false);
  s.autoBleScan  = backgroundOptIn && (doc["autoBleScan"] | false);
  if (!nrfLoadedFromNvs) {
    s.activeNrfCount = activeNrfCountClamp(
        doc["activeNrfCount"] | s.activeNrfCount);
    nrfSaveActiveRadioSelection();
  }
  nrfApplyActiveRadioSelection();

  if (s.autoWifiScan != s.autoBleScan) {
    bool en = (s.autoWifiScan || s.autoBleScan);
    s.autoWifiScan = en;
    s.autoBleScan  = en;
  }

  if (settingsTouchSavedForBoard(doc)) {
    JsonObjectConst touch = doc["touch"];
    s.touchXMin = touch["xMin"] | s.touchXMin;
    s.touchXMax = touch["xMax"] | s.touchXMax;
    s.touchYMin = touch["yMin"] | s.touchYMin;
    s.touchYMax = touch["yMax"] | s.touchYMax;
    if (!touchCalibrationSane(s)) {
      Serial.printf("[touch] rejected invalid calibration X=%u..%u Y=%u..%u\n",
                    s.touchXMin, s.touchXMax, s.touchYMin, s.touchYMax);
      settingsApplyBoardTouchDefaults();
    }
  } else {
    settingsApplyBoardTouchDefaults();
  }

  Serial.printf("[touch] source=%s board=%s X=%u..%u Y=%u..%u\n",
                settingsTouchSavedForBoard(doc) ? "SD" : "factory",
                TOUCH_PROFILE_ID, s.touchXMin, s.touchXMax,
                s.touchYMin, s.touchYMax);

  // The internal copy wins when present; otherwise migrate the successfully
  // parsed legacy SD configuration into NVS for future bus-independent boots.
  if (settingsLoadedFromNvs) {
    settingsLoadNvsSnapshot();
  } else {
    settingsSaveNvsSnapshot();
  }
  return true;
}

bool settingsSave() {
  // Persist this before touching SD.  Even if a marginal card rejects the
  // settings transaction, the pre-SD NFC gate must use the language selected
  // by the user instead of silently reverting to English.
  const bool nvsOk = settingsSaveNvsSnapshot();
  settingsSaveBootLanguage();
  sdRetryMount();
  // Settings are only saved from the foreground settings UI. Explicitly
  // return the shared bus to SD before beginning one atomic transaction.
  SdFileIoGuard io(1500);
  if (!io.locked()) {
    Serial.println("[settings] SD mirror skipped: bus busy");
    return nvsOk;
  }
  restoreSdAfterSharedSpi();

  if (!ensureDir("/config")) {
    sd_mounted = false;
    if (!ensureDir("/config")) {
      Serial.println("[settings] SD mirror skipped: mount/directory unavailable");
      return nvsOk;
    }
  }

  auto& s = g_settings;
  StaticJsonDocument<512> doc;
  doc["board"]           = TOUCH_PROFILE_ID;
  doc["brightness"]      = s.brightness;
  doc["theme"]           = (uint8_t)s.theme;
  doc["accentColor"]     = s.accentColor;
  doc["language"]        = (uint8_t)s.language;
  doc["neopixelEnabled"] = s.neopixelEnabled;
  doc["soundEnabled"]    = s.soundEnabled;

  doc["autoWifiScan"]    = s.autoWifiScan;
  doc["autoBleScan"]     = s.autoBleScan;
  doc["backgroundScanOptIn"] = (s.autoWifiScan || s.autoBleScan);
  doc["activeNrfCount"]  = activeNrfCountClamp(s.activeNrfCount);

  JsonObject t = doc.createNestedObject("touch");
  t["xMin"] = s.touchXMin;
  t["xMax"] = s.touchXMax;
  t["yMin"] = s.touchYMin;
  t["yMax"] = s.touchYMax;

  constexpr const char* kTmp = "/config/settings.tmp";
  constexpr const char* kBak = "/config/settings.bak";
  if (SD.exists(kTmp)) SD.remove(kTmp);
  File f = SD.open(kTmp, FILE_WRITE);
  if (!f) return nvsOk;
  const size_t written = serializeJson(doc, f);
  f.flush();
  const bool writeOk = written > 0 && !f.getWriteError();
  f.close();
  if (!writeOk) {
    if (SD.exists(kTmp)) SD.remove(kTmp);
    return nvsOk;
  }

  if (SD.exists(kBak)) SD.remove(kBak);
  if (SD.exists(SETTINGS_PATH) && !SD.rename(SETTINGS_PATH, kBak)) {
    SD.remove(kTmp);
    return nvsOk;
  }
  if (!SD.rename(kTmp, SETTINGS_PATH)) {
    if (SD.exists(kBak)) SD.rename(kBak, SETTINGS_PATH);
    SD.remove(kTmp);
    return nvsOk;
  }
  return true;
}
