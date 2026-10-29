#include <algorithm>
#include <vector>
#include "driver/rmt.h"
#include "freertos/ringbuf.h"
#include "KeyboardUI.h"
#include "Touchscreen.h"
#include "config.h"
#include "icon.h"
#include "shared.h"


namespace {
  static constexpr const char* SUBGHZ_DIR = "/subghz";
  static constexpr const char* SUBGHZ_CAPTURE_DIR = "/subghz/captures";
  static constexpr const char* SUBGHZ_EXPORT_PREFIX = "/subghz/profiles_";
  static constexpr const char* SUBGHZ_CURRENT_PATH = "/subghz/profiles_current.bin";
  static constexpr uint32_t SUBGHZ_EXPORT_MAGIC = 0x315A4753;

  struct __attribute__((packed)) SubGhzProfile {
    uint32_t frequency;
    uint32_t value;
    uint16_t bitLength;
    uint16_t protocol;
    char     name[16];
  };

  static constexpr uint16_t MAX_NAME_LENGTH = 16;
  static constexpr uint16_t PROFILE_SIZE = sizeof(SubGhzProfile);

  static constexpr uint16_t ADDR_VALUE = 1280;
  static constexpr uint16_t ADDR_BITLEN = 1284;
  static constexpr uint16_t ADDR_PROTO = 1286;
  static constexpr uint16_t ADDR_FREQ = 1288;
  static constexpr uint16_t ADDR_PROFILE_COUNT = 1296;
  static constexpr uint16_t ADDR_PROFILE_START = 1300;
  static constexpr uint16_t MAX_PROFILES = 5;
  static constexpr uint16_t SUBGHZ_EEPROM_SIZE = 4096;
  static constexpr uint16_t RAW_PROFILE_PROTOCOL = 0xFFFE;
  static constexpr uint32_t RAW_PROFILE_MAGIC = 0x31574152;  // RAW1
  // Five 256-pulse RAW slots still fit in the 4096-byte EEPROM partition
  // (last byte used: 4059). This also matches the RMT hardware capture depth.
  static constexpr uint16_t RAW_MAX_CHANGES = 256;
  static constexpr uint16_t ADDR_RAW_PROFILE_START = 1440;

  struct __attribute__((packed)) SubGhzRawProfile {
    uint32_t magic;
    uint32_t frequency;
    uint16_t count;
    uint8_t startLevel;
    uint8_t repeats;
    uint16_t timings[RAW_MAX_CHANGES];
  };

  static constexpr uint16_t RAW_PROFILE_SIZE = sizeof(SubGhzRawProfile);

  static bool subghzIsRawProfile(const SubGhzProfile& p) {
    return p.protocol == RAW_PROFILE_PROTOCOL && p.value >= 1u &&
           p.value <= MAX_PROFILES && p.bitLength >= 8u &&
           p.bitLength <= RAW_MAX_CHANGES;
  }

  static int subghzRawProfileAddress(uint16_t slot) {
    return ADDR_RAW_PROFILE_START + (slot * RAW_PROFILE_SIZE);
  }

  static void subghzClearRawSlot(uint16_t slot) {
    SubGhzRawProfile empty{};
    EEPROM.put(subghzRawProfileAddress(slot), empty);
  }

  static bool subghzReadRawSlot(uint16_t slot, SubGhzRawProfile& out) {
    if (slot >= MAX_PROFILES) return false;
    EEPROM.get(subghzRawProfileAddress(slot), out);
    return out.magic == RAW_PROFILE_MAGIC && out.count >= 8u &&
           out.count <= RAW_MAX_CHANGES && out.startLevel <= 1u &&
           out.frequency >= 300000000UL && out.frequency <= 928000000UL;
  }

  struct __attribute__((packed)) SubGhzExportHeader {
    uint32_t magic;
    uint16_t version;
    uint16_t count;
    uint16_t profileSize;
    uint16_t reserved;
  };

  static bool subghz_sd_mounted = false;
  static bool subghzMountSD() {
#if defined(CC1101_CS)
    pinMode(CC1101_CS, OUTPUT);
    digitalWrite(CC1101_CS, HIGH);
#endif

    restoreSdAfterSharedSpi();
    subghz_sd_mounted = isSDCardAvailable();
    return subghz_sd_mounted;
  }

  /** Create a directory after the caller has mounted SD.  Keeping mount and
   * directory creation separate is important on DIV V2.1: mounting once per
   * operation avoids two consecutive SD.end()/SD.begin() cycles while the
   * CC1101 is temporarily idle. */
  static bool subghzEnsureDirMounted(const char* dirPath) {
    if (SD.exists(dirPath)) return true;
    if (SD.mkdir(dirPath)) return true;
    if (dirPath && dirPath[0] == '/') return SD.mkdir(dirPath + 1);
    return false;
  }

  static void clearProfilesInEeprom() {

    uint16_t zero = 0;
    EEPROM.put(ADDR_PROFILE_COUNT, zero);
    SubGhzProfile empty{};
    for (uint16_t i = 0; i < MAX_PROFILES; i++) {
      EEPROM.put(ADDR_PROFILE_START + (i * PROFILE_SIZE), empty);
      subghzClearRawSlot(i);
    }
    EEPROM.commit();
  }

  static bool makeNextExportPath(String& outPath) {

    for (uint16_t i = 0; i < 10000; i++) {
      char buf[48];
      snprintf(buf, sizeof(buf), "%s%04u.bin", SUBGHZ_EXPORT_PREFIX, (unsigned)i);
      if (!SD.exists(buf)) { outPath = String(buf); return true; }
    }
    return false;
  }

  static String subghzSafeFileStem(const char* name) {
    String stem;
    if (name) {
      for (uint8_t i = 0; name[i] != '\0' && i < MAX_NAME_LENGTH; ++i) {
        const char c = name[i];
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
            (c >= '0' && c <= '9') || c == '-' || c == '_') {
          stem += c;
        } else if (c == ' ' && stem.length() > 0 && !stem.endsWith("_")) {
          stem += '_';
        }
      }
    }
    if (stem.isEmpty()) stem = "capture";
    return stem;
  }

  static bool makeNextCapturePath(const SubGhzProfile& profile, String& outPath) {
    const String stem = subghzSafeFileStem(profile.name);
    for (uint16_t i = 0; i < 10000; ++i) {
      char suffix[12];
      snprintf(suffix, sizeof(suffix), "_%04u.sub", (unsigned)i);
      outPath = String(SUBGHZ_CAPTURE_DIR) + "/" + stem + suffix;
      if (!SD.exists(outPath.c_str())) return true;
    }
    return false;
  }

  /** Export one EEPROM profile as a self-contained, readable .sub file.
   * RAW captures include their complete signed timing sequence; decoded
   * RCSwitch profiles retain the protocol, bit count and value. */
  static bool exportProfileToSD(uint16_t slot, const SubGhzProfile& profile,
                                String& outPath, String* errOut = nullptr) {
    SdFileIoGuard io(1500);
    if (!io.locked()) { if (errOut) *errOut = "SD bus busy"; return false; }
    if (slot >= MAX_PROFILES) {
      if (errOut) *errOut = "Index out of range";
      return false;
    }
    if (!subghzMountSD() || !subghzEnsureDirMounted(SUBGHZ_DIR) ||
        !subghzEnsureDirMounted(SUBGHZ_CAPTURE_DIR)) {
      if (errOut) *errOut = "SD not mounted";
      return false;
    }
    if (!makeNextCapturePath(profile, outPath)) {
      if (errOut) *errOut = "No free filename";
      return false;
    }

    SubGhzRawProfile raw{};
    const bool isRaw = subghzIsRawProfile(profile);
    const uint16_t rawSlot = isRaw ? (uint16_t)(profile.value - 1u) : slot;
    if (isRaw && !subghzReadRawSlot(rawSlot, raw)) {
      if (errOut) *errOut = "RAW data missing";
      return false;
    }

    File f = SD.open(outPath.c_str(), FILE_WRITE);
    if (!f) {
      if (errOut) *errOut = "Open failed";
      return false;
    }

    f.println(isRaw ? "Filetype: ESP32-DIV SubGHz RAW File"
                    : "Filetype: ESP32-DIV SubGHz Profile");
    f.println("Version: 1");
    f.printf("Name: %s\n", profile.name);
    f.printf("Frequency: %lu\n", (unsigned long)profile.frequency);
    f.println("Preset: FuriHalSubGhzPresetOok650Async");
    if (isRaw) {
      f.println("Protocol: RAW");
      f.printf("Start_Level: %u\n", (unsigned)raw.startLevel);
      f.printf("Repeat: %u\n", (unsigned)raw.repeats);
      f.print("RAW_Data:");
      for (uint16_t i = 0; i < raw.count; ++i) {
        const bool high = ((i & 1u) == 0u) ? (raw.startLevel != 0u)
                                           : (raw.startLevel == 0u);
        const int32_t signedTiming = high ? (int32_t)raw.timings[i]
                                          : -(int32_t)raw.timings[i];
        f.printf(" %ld", (long)signedTiming);
      }
      f.println();
    } else {
      f.println("Protocol: RCSwitch");
      f.printf("RCSwitch_Protocol: %u\n", (unsigned)profile.protocol);
      f.printf("Bit: %u\n", (unsigned)profile.bitLength);
      f.printf("Key: %lu\n", (unsigned long)profile.value);
    }
    f.flush();
    const bool ok = f.getWriteError() == 0;
    f.close();
    if (!ok) {
      SD.remove(outPath.c_str());
      if (errOut) *errOut = "Write failed";
      return false;
    }
    Serial.printf("[subghz] exported EEPROM slot=%u type=%s path=%s\n",
                  (unsigned)slot, isRaw ? "raw" : "decoded",
                  outPath.c_str());
    return true;
  }

  /** Write a live Replay capture directly to SD. EEPROM is not part of the
   * capture path, so a failed/removed card cannot corrupt persistent settings. */
  static bool writeCaptureToSD(const SubGhzProfile& profile,
                               const SubGhzRawProfile* raw,
                               String& outPath,
                               String* errOut = nullptr) {
    SdFileIoGuard io(1800);
    if (!io.locked()) { if (errOut) *errOut = "SD bus busy"; return false; }
    if (!subghzMountSD() || !subghzEnsureDirMounted(SUBGHZ_DIR) ||
        !subghzEnsureDirMounted(SUBGHZ_CAPTURE_DIR)) {
      if (errOut) *errOut = "SD not mounted";
      return false;
    }
    if (!makeNextCapturePath(profile, outPath)) {
      if (errOut) *errOut = "No free filename";
      return false;
    }

    File f = SD.open(outPath.c_str(), FILE_WRITE);
    if (!f) { if (errOut) *errOut = "Open failed"; return false; }
    const bool isRaw = raw && raw->count >= 8u && raw->count <= RAW_MAX_CHANGES;
    f.println(isRaw ? "Filetype: ESP32-DIV SubGHz RAW File"
                    : "Filetype: ESP32-DIV SubGHz Profile");
    f.println("Version: 1");
    f.printf("Name: %s\n", profile.name);
    f.printf("Frequency: %lu\n", (unsigned long)profile.frequency);
    f.println("Preset: FuriHalSubGhzPresetOok650Async");
    if (isRaw) {
      f.println("Protocol: RAW");
      f.printf("Start_Level: %u\n", (unsigned)raw->startLevel);
      f.printf("Repeat: %u\n", (unsigned)raw->repeats);
      f.print("RAW_Data:");
      for (uint16_t i = 0; i < raw->count; ++i) {
        const bool high = ((i & 1u) == 0u) ? raw->startLevel != 0u
                                           : raw->startLevel == 0u;
        f.printf(" %ld", high ? (long)raw->timings[i]
                               : -(long)raw->timings[i]);
      }
      f.println();
    } else {
      f.println("Protocol: RCSwitch");
      f.printf("RCSwitch_Protocol: %u\n",
               (unsigned)(profile.protocol ? profile.protocol : 1u));
      f.printf("Bit: %u\n",
               (unsigned)(profile.bitLength ? profile.bitLength : 24u));
      f.printf("Key: %lu\n", (unsigned long)profile.value);
    }
    f.flush();
    const bool ok = f.getWriteError() == 0;
    f.close();
    if (!ok) {
      SD.remove(outPath.c_str());
      if (errOut) *errOut = "Write failed";
      return false;
    }
    Serial.printf("[subghz] capture saved directly to %s (%s)\n",
                  outPath.c_str(), isRaw ? "raw" : "decoded");
    return true;
  }

  static bool findLatestExportPath(String& outPath) {

    for (int i = 9999; i >= 0; i--) {
      char buf[48];
      snprintf(buf, sizeof(buf), "%s%04u.bin", SUBGHZ_EXPORT_PREFIX, (unsigned)i);
      if (SD.exists(buf)) { outPath = String(buf); return true; }
    }
    return false;
  }

  static bool exportProfilesToSD(String& outPath, String* errOut = nullptr) {
    SdFileIoGuard io(1500);
    if (!io.locked()) { if (errOut) *errOut = "SD bus busy"; return false; }
    if (!subghzMountSD() || !subghzEnsureDirMounted(SUBGHZ_DIR)) {
      if (errOut) *errOut = "SD not mounted";
      return false;
    }

    uint16_t count = 0;
    EEPROM.get(ADDR_PROFILE_COUNT, count);
    if (count > MAX_PROFILES) count = MAX_PROFILES;

    if (!makeNextExportPath(outPath)) {
      if (errOut) *errOut = "No free filename";
      return false;
    }

    File f = SD.open(outPath.c_str(), FILE_WRITE);
    if (!f) {
      if (errOut) *errOut = "Open failed";
      return false;
    }

    SubGhzExportHeader h{};
    h.magic = SUBGHZ_EXPORT_MAGIC;
    h.version = 1;
    h.count = count;
    h.profileSize = PROFILE_SIZE;
    h.reserved = 0;

    bool ok = (f.write((const uint8_t*)&h, sizeof(h)) == sizeof(h));
    for (uint16_t i = 0; ok && i < count; i++) {
      SubGhzProfile p{};
      int addr = ADDR_PROFILE_START + (i * PROFILE_SIZE);
      EEPROM.get(addr, p);
      ok = (f.write((const uint8_t*)&p, sizeof(p)) == sizeof(p));
    }
    f.close();

    if (!ok && errOut) *errOut = "Write failed";
    return ok;
  }

  static bool syncCurrentProfilesToSD(String* errOut = nullptr) {
    SdFileIoGuard io(1500);
    if (!io.locked()) { if (errOut) *errOut = "SD bus busy"; return false; }

    if (!subghzMountSD() || !subghzEnsureDirMounted(SUBGHZ_DIR)) {
      if (errOut) *errOut = "SD not mounted";
      return false;
    }

    uint16_t count = 0;
    EEPROM.get(ADDR_PROFILE_COUNT, count);
    if (count > MAX_PROFILES) count = MAX_PROFILES;

    if (SD.exists(SUBGHZ_CURRENT_PATH)) SD.remove(SUBGHZ_CURRENT_PATH);
    File f = SD.open(SUBGHZ_CURRENT_PATH, FILE_WRITE);
    if (!f) {
      if (errOut) *errOut = "Open failed";
      return false;
    }

    SubGhzExportHeader h{};
    h.magic = SUBGHZ_EXPORT_MAGIC;
    h.version = 1;
    h.count = count;
    h.profileSize = PROFILE_SIZE;
    h.reserved = 0;

    bool ok = (f.write((const uint8_t*)&h, sizeof(h)) == sizeof(h));
    for (uint16_t i = 0; ok && i < count; i++) {
      SubGhzProfile p{};
      int addr = ADDR_PROFILE_START + (i * PROFILE_SIZE);
      EEPROM.get(addr, p);
      ok = (f.write((const uint8_t*)&p, sizeof(p)) == sizeof(p));
    }
    f.close();
    if (!ok && errOut) *errOut = "Write failed";
    return ok;
  }

  static bool importProfilesFromSD(const String& path, String* errOut = nullptr) {
    SdFileIoGuard io(1500);
    if (!io.locked()) { if (errOut) *errOut = "SD bus busy"; return false; }
    if (!subghzMountSD()) {
      if (errOut) *errOut = "SD not mounted";
      return false;
    }
    if (path.isEmpty() || !SD.exists(path.c_str())) {
      if (errOut) *errOut = "File not found";
      return false;
    }

    File f = SD.open(path.c_str(), FILE_READ);
    if (!f) {
      if (errOut) *errOut = "Open failed";
      return false;
    }

    SubGhzExportHeader h{};
    if (f.read((uint8_t*)&h, sizeof(h)) != sizeof(h)) { f.close(); if (errOut) *errOut="Bad header"; return false; }
    if (h.magic != SUBGHZ_EXPORT_MAGIC || h.version != 1) { f.close(); if (errOut) *errOut="Wrong file"; return false; }
    if (h.profileSize != PROFILE_SIZE) { f.close(); if (errOut) *errOut="Size mismatch"; return false; }

    uint16_t count = h.count;
    if (count > MAX_PROFILES) count = MAX_PROFILES;

    clearProfilesInEeprom();
    for (uint16_t i = 0; i < count; i++) {
      SubGhzProfile p{};
      if (f.read((uint8_t*)&p, sizeof(p)) != sizeof(p)) { f.close(); if (errOut) *errOut="Read failed"; return false; }
      p.name[MAX_NAME_LENGTH - 1] = '\0';
      EEPROM.put(ADDR_PROFILE_START + (i * PROFILE_SIZE), p);
    }
    EEPROM.put(ADDR_PROFILE_COUNT, count);
    EEPROM.commit();
    f.close();
    return true;
  }

  struct SubGhzFileEntry {
    String path;
    uint16_t count = 0;
    bool isCurrent = false;
  };

  static bool readExportHeader(File& f, SubGhzExportHeader& out, String* errOut = nullptr) {
    if (f.read((uint8_t*)&out, sizeof(out)) != sizeof(out)) { if (errOut) *errOut="Bad header"; return false; }
    if (out.magic != SUBGHZ_EXPORT_MAGIC || out.version != 1) { if (errOut) *errOut="Wrong file"; return false; }
    if (out.profileSize != PROFILE_SIZE) { if (errOut) *errOut="Size mismatch"; return false; }
    return true;
  }

  static bool readProfileAt(const String& path, uint16_t localIndex, SubGhzProfile& out, String* errOut = nullptr) {
    SdFileIoGuard io(1500);
    if (!io.locked()) { if (errOut) *errOut = "SD bus busy"; return false; }
    if (!subghzMountSD()) { if (errOut) *errOut="SD not mounted"; return false; }
    File f = SD.open(path.c_str(), FILE_READ);
    if (!f) { if (errOut) *errOut="Open failed"; return false; }
    SubGhzExportHeader h{};
    if (!readExportHeader(f, h, errOut)) { f.close(); return false; }
    uint16_t count = h.count; if (count > MAX_PROFILES) count = MAX_PROFILES;
    if (localIndex >= count) { f.close(); if (errOut) *errOut="Index OOR"; return false; }
    uint32_t off = (uint32_t)sizeof(SubGhzExportHeader) + (uint32_t)localIndex * (uint32_t)PROFILE_SIZE;
    if (!f.seek(off)) { f.close(); if (errOut) *errOut="Seek failed"; return false; }
    if (f.read((uint8_t*)&out, sizeof(out)) != sizeof(out)) { f.close(); if (errOut) *errOut="Read failed"; return false; }
    out.name[MAX_NAME_LENGTH - 1] = '\0';
    f.close();
    return true;
  }

  static bool listAllProfileFiles(std::vector<SubGhzFileEntry>& out, String* errOut = nullptr) {
    SdFileIoGuard io(1500);
    if (!io.locked()) { if (errOut) *errOut = "SD bus busy"; return false; }
    out.clear();
    if (!subghzMountSD()) { if (errOut) *errOut="SD not mounted"; return false; }
    if (!SD.exists(SUBGHZ_DIR)) { if (errOut) *errOut="No /subghz"; return false; }
    File d = SD.open(SUBGHZ_DIR);
    if (!d) { if (errOut) *errOut="Open dir failed"; return false; }

    for (;;) {
      File f = d.openNextFile();
      if (!f) break;
      if (f.isDirectory()) { f.close(); continue; }
      String name = String(f.name());

      String fullPath = String(SUBGHZ_DIR) + "/" + name;

      bool isCurrent = (name == "profiles_current.bin");
      bool isArchive = name.startsWith("profiles_") && name.endsWith(".bin") && !isCurrent;
      if (!isCurrent && !isArchive) { f.close(); continue; }

      SubGhzExportHeader h{};
      String herr;
      bool ok = readExportHeader(f, h, &herr);
      f.close();
      if (!ok) continue;

      uint16_t cnt = h.count;
      if (cnt > MAX_PROFILES) cnt = MAX_PROFILES;
      if (cnt == 0) continue;

      SubGhzFileEntry e;
      e.path = fullPath;
      e.count = cnt;
      e.isCurrent = isCurrent;
      out.push_back(e);
    }
    d.close();

    std::sort(out.begin(), out.end(), [](const SubGhzFileEntry& a, const SubGhzFileEntry& b) {
      if (a.isCurrent != b.isCurrent) return a.isCurrent > b.isCurrent;
      return a.path > b.path;
    });
    return true;
  }

  static uint16_t totalProfilesInIndex(const std::vector<SubGhzFileEntry>& files) {
    uint32_t total = 0;
    for (auto& f : files) total += f.count;
    if (total > 65535) total = 65535;
    return (uint16_t)total;
  }

  static bool locateGlobalIndex(const std::vector<SubGhzFileEntry>& files, uint16_t globalIndex,
                                String& outPath, uint16_t& outLocalIdx) {
    uint32_t idx = globalIndex;
    for (auto& fe : files) {
      if (idx < fe.count) {
        outPath = fe.path;
        outLocalIdx = (uint16_t)idx;
        return true;
      }
      idx -= fe.count;
    }
    return false;
  }

  static bool deleteProfileFromFile(const String& path, uint16_t localIndex, String* errOut = nullptr) {
    SdFileIoGuard io(1500);
    if (!io.locked()) { if (errOut) *errOut = "SD bus busy"; return false; }
    if (!subghzMountSD()) { if (errOut) *errOut="SD not mounted"; return false; }
    File f = SD.open(path.c_str(), FILE_READ);
    if (!f) { if (errOut) *errOut="Open failed"; return false; }
    SubGhzExportHeader h{};
    if (!readExportHeader(f, h, errOut)) { f.close(); return false; }
    uint16_t count = h.count; if (count > MAX_PROFILES) count = MAX_PROFILES;
    if (localIndex >= count) { f.close(); if (errOut) *errOut="Index OOR"; return false; }

    SubGhzProfile buf[MAX_PROFILES]{};
    for (uint16_t i = 0; i < count; i++) {
      if (f.read((uint8_t*)&buf[i], sizeof(SubGhzProfile)) != sizeof(SubGhzProfile)) { f.close(); if (errOut) *errOut="Read failed"; return false; }
      buf[i].name[MAX_NAME_LENGTH - 1] = '\0';
    }
    f.close();

    for (uint16_t i = localIndex; i + 1 < count; i++) buf[i] = buf[i + 1];
    count--;

    if (SD.exists(path.c_str())) SD.remove(path.c_str());
    File w = SD.open(path.c_str(), FILE_WRITE);
    if (!w) { if (errOut) *errOut="Open write failed"; return false; }

    SubGhzExportHeader nh{};
    nh.magic = SUBGHZ_EXPORT_MAGIC;
    nh.version = 1;
    nh.count = count;
    nh.profileSize = PROFILE_SIZE;
    nh.reserved = 0;
    bool ok = (w.write((const uint8_t*)&nh, sizeof(nh)) == sizeof(nh));
    for (uint16_t i = 0; ok && i < count; i++) {
      ok = (w.write((const uint8_t*)&buf[i], sizeof(SubGhzProfile)) == sizeof(SubGhzProfile));
    }
    w.close();
    if (!ok && errOut) *errOut="Write failed";

    if (ok && path.endsWith("profiles_current.bin")) {
      importProfilesFromSD(path, nullptr);
    }
    return ok;
  }
}

#ifdef TFT_BLACK
#undef TFT_BLACK
#endif
#define TFT_BLACK FEATURE_BG

#ifndef FEATURE_TEXT
#define FEATURE_TEXT ORANGE
#endif
#ifndef FEATURE_WHITE
#define FEATURE_WHITE 0xFFFF
#endif

#ifdef TFT_WHITE
#undef TFT_WHITE
#endif
#define TFT_WHITE FEATURE_TEXT

#ifdef WHITE
#undef WHITE
#endif
#define WHITE FEATURE_WHITE

#ifdef DARK_GRAY
#undef DARK_GRAY
#endif
#define DARK_GRAY UI_FG

static constexpr int kSubghzScreenH = 320;

static int subghzContentBottom() {
  return featureHasTouchNavBar() ? touchNavContentBottomY() : kSubghzScreenH;
}

static void subghzClearBody(uint16_t color = TFT_BLACK) {
  if (featureHasTouchNavBar()) {
    featureClearContent(color);
  } else {
    tft.fillScreen(color);
  }
}

static constexpr unsigned long kSubghzNavDebounceMs = 200;

static void subghzWaitNavRelease(int pin) {
  while (isTouchNavButtonPressed(pin)) {
    delay(10);
  }
  delay(kSubghzNavDebounceMs);
}

static void subghzDrainButtons() {
  const uint32_t started = millis();
  while ((isPhysicalButtonPressed(BTN_UP) ||
          isPhysicalButtonPressed(BTN_DOWN) ||
          isPhysicalButtonPressed(BTN_LEFT) ||
          isPhysicalButtonPressed(BTN_RIGHT) ||
          isPhysicalButtonPressed(BTN_SELECT)) &&
         (uint32_t)(millis() - started) < 1500u) {
    delay(8);
  }
  resetTouchNavHeldState();
}

static void subghzRedrawNavChrome() {
  if (!featureHasTouchNavBar()) {
    return;
  }
  invalidateTouchButtonCue();
  redrawTouchButtonBar();
  maintainTouchNavBar();
}

static bool subghzWaitWithNav(uint32_t ms) {
  const uint32_t until = millis() + ms;
  while ((int32_t)(millis() - until) < 0) {
    if (feature_exit_requested || featureExitButtonPressed()) {
      return false;
    }
    if (featureHasTouchNavBar()) {
      maintainTouchNavBar();
    }
    delay(50);
  }
  return true;
}

static bool subghzValidateRcswitchFrame(uint32_t frequencyHz,
                                       uint16_t bitLength,
                                       uint16_t protocol,
                                       char* reason,
                                       size_t reasonSize) {
  if (bitLength < 1 || bitLength > 32) {
    snprintf(reason, reasonSize, "Bad bits: %u (1..32)", (unsigned)bitLength);
    return false;
  }
  if (protocol < 1 || protocol > 12) {
    snprintf(reason, reasonSize, "Bad proto: %u (1..12)", (unsigned)protocol);
    return false;
  }
  if (frequencyHz != 0 && (frequencyHz < 300000000UL || frequencyHz > 928000000UL)) {
    snprintf(reason, reasonSize, "Bad freq: %lu", (unsigned long)frequencyHz);
    return false;
  }
  if (reasonSize) reason[0] = '\0';
  return true;
}

struct SubGhzParsedCapture {
  SubGhzProfile profile{};
  SubGhzRawProfile raw{};
  bool hasRaw = false;
};

static bool subghzParseUnsigned(const String& text, uint32_t& out) {
  String value = text;
  value.trim();
  if (value.isEmpty()) return false;
  char* end = nullptr;
  // Treat ordinary values as decimal even when a hand-edited capture has
  // leading zeroes. Only an explicit 0x prefix selects hexadecimal.
  const int base = value.startsWith("0x") || value.startsWith("0X") ? 16 : 10;
  const unsigned long parsed = strtoul(value.c_str(), &end, base);
  while (end && *end == ' ') ++end;
  if (!end || *end != '\0') return false;
  out = (uint32_t)parsed;
  return true;
}

static void subghzCaptureNameFromPath(const String& path, char* out,
                                      size_t outSize) {
  String name = path;
  const int slash = name.lastIndexOf('/');
  if (slash >= 0) name = name.substring(slash + 1);
  if (name.endsWith(".sub")) name.remove(name.length() - 4);
  if (name.length() >= outSize) name.remove(outSize - 1);
  snprintf(out, outSize, "%s", name.c_str());
}

static bool subghzParseRawDataLine(const String& values,
                                   SubGhzRawProfile& raw,
                                   String* errOut) {
  int pos = 0;
  while (pos < (int)values.length()) {
    while (pos < (int)values.length() && values[pos] == ' ') ++pos;
    if (pos >= (int)values.length()) break;
    int end = values.indexOf(' ', pos);
    if (end < 0) end = values.length();
    String token = values.substring(pos, end);
    token.trim();
    char* tail = nullptr;
    const long signedTiming = strtol(token.c_str(), &tail, 10);
    if (!tail || *tail != '\0' || signedTiming == 0 ||
        labs(signedTiming) > 65535L) {
      if (errOut) *errOut = "Invalid RAW timing";
      return false;
    }
    if (raw.count >= RAW_MAX_CHANGES) {
      if (errOut) *errOut = "RAW capture too long";
      return false;
    }
    if (raw.count == 0) raw.startLevel = signedTiming > 0 ? 1u : 0u;
    raw.timings[raw.count++] = (uint16_t)labs(signedTiming);
    pos = end + 1;
  }
  return true;
}

/** Read the portable text format written by exportProfileToSD(). Multiple
 * RAW_Data lines are accepted so files can also be hand-wrapped safely. */
static bool subghzParseCaptureFileMounted(const String& path,
                                          SubGhzParsedCapture& out,
                                          String* errOut = nullptr) {
  out = SubGhzParsedCapture{};
  out.raw.magic = RAW_PROFILE_MAGIC;
  out.raw.repeats = 4;
  subghzCaptureNameFromPath(path, out.profile.name, sizeof(out.profile.name));

  File f = SD.open(path.c_str(), FILE_READ);
  if (!f) {
    if (errOut) *errOut = "Open failed";
    return false;
  }

  String protocol;
  bool sawFrequency = false;
  while (f.available()) {
    String line = f.readStringUntil('\n');
    line.trim();
    if (line.isEmpty() || line.startsWith("#")) continue;
    const int colon = line.indexOf(':');
    if (colon <= 0) continue;
    String key = line.substring(0, colon);
    String value = line.substring(colon + 1);
    key.trim();
    value.trim();

    uint32_t parsed = 0;
    if (key.equalsIgnoreCase("Name")) {
      if (value.length() >= sizeof(out.profile.name)) {
        value.remove(sizeof(out.profile.name) - 1);
      }
      snprintf(out.profile.name, sizeof(out.profile.name), "%s", value.c_str());
    } else if (key.equalsIgnoreCase("Frequency")) {
      if (!subghzParseUnsigned(value, parsed)) {
        f.close(); if (errOut) *errOut = "Invalid frequency"; return false;
      }
      out.profile.frequency = parsed;
      out.raw.frequency = parsed;
      sawFrequency = true;
    } else if (key.equalsIgnoreCase("Protocol")) {
      protocol = value;
    } else if (key.equalsIgnoreCase("Start_Level")) {
      if (subghzParseUnsigned(value, parsed) && parsed <= 1u) {
        out.raw.startLevel = (uint8_t)parsed;
      }
    } else if (key.equalsIgnoreCase("Repeat")) {
      if (subghzParseUnsigned(value, parsed)) {
        out.raw.repeats = (uint8_t)constrain(parsed, 1u, 8u);
      }
    } else if (key.equalsIgnoreCase("RAW_Data")) {
      out.hasRaw = true;
      if (!subghzParseRawDataLine(value, out.raw, errOut)) {
        f.close(); return false;
      }
    } else if (key.equalsIgnoreCase("RCSwitch_Protocol")) {
      if (subghzParseUnsigned(value, parsed)) out.profile.protocol = parsed;
    } else if (key.equalsIgnoreCase("Bit")) {
      if (subghzParseUnsigned(value, parsed)) out.profile.bitLength = parsed;
    } else if (key.equalsIgnoreCase("Key")) {
      if (subghzParseUnsigned(value, parsed)) out.profile.value = parsed;
    }
  }
  f.close();

  if (!sawFrequency || out.profile.frequency < 300000000UL ||
      out.profile.frequency > 928000000UL) {
    if (errOut) *errOut = "Frequency out of range";
    return false;
  }
  if (out.hasRaw || protocol.equalsIgnoreCase("RAW")) {
    out.hasRaw = true;
    if (out.raw.count < 8u || out.raw.count > RAW_MAX_CHANGES) {
      if (errOut) *errOut = "RAW pulse count invalid";
      return false;
    }
    out.profile.protocol = RAW_PROFILE_PROTOCOL;
    out.profile.bitLength = out.raw.count;
    return true;
  }

  // Portable decoded files use safe defaults when optional metadata is
  // absent; no analyser-style qualification blocks loading or TX.
  if (out.profile.bitLength < 1u || out.profile.bitLength > 32u) {
    out.profile.bitLength = 24u;
  }
  if (out.profile.protocol < 1u || out.profile.protocol > 12u) {
    out.profile.protocol = 1u;
  }
  return true;
}

static bool subghzParseCaptureFile(const String& path,
                                   SubGhzParsedCapture& out,
                                   String* errOut = nullptr) {
  SdFileIoGuard io(1500);
  if (!io.locked()) { if (errOut) *errOut = "SD bus busy"; return false; }
  if (!subghzMountSD()) { if (errOut) *errOut = "SD not mounted"; return false; }
  return subghzParseCaptureFileMounted(path, out, errOut);
}

static bool subghzListCaptureFilesMounted(std::vector<String>& out,
                                          String* errOut = nullptr) {
  out.clear();
  if (!SD.exists(SUBGHZ_CAPTURE_DIR)) {
    if (errOut) *errOut = "No /subghz/captures";
    return false;
  }
  File dir = SD.open(SUBGHZ_CAPTURE_DIR);
  if (!dir || !dir.isDirectory()) {
    if (errOut) *errOut = "Capture folder failed";
    return false;
  }
  while (out.size() < 64u) {
    File f = dir.openNextFile();
    if (!f) break;
    if (!f.isDirectory()) {
      String path = f.name();
      if (!path.startsWith("/")) path = String(SUBGHZ_CAPTURE_DIR) + "/" + path;
      if (path.endsWith(".sub")) out.push_back(path);
    }
    f.close();
  }
  dir.close();
  std::sort(out.begin(), out.end(), [](const String& a, const String& b) {
    return a > b;
  });
  if (out.empty() && errOut) *errOut = "No .sub captures";
  return !out.empty();
}

static bool subghzListCaptureFiles(std::vector<String>& out,
                                   String* errOut = nullptr) {
  SdFileIoGuard io(1500);
  if (!io.locked()) { if (errOut) *errOut = "SD bus busy"; return false; }
  if (!subghzMountSD()) {
    if (errOut) *errOut = "SD not mounted";
    return false;
  }
  return subghzListCaptureFilesMounted(out, errOut);
}

static bool subghzTransmitRawTimings(uint8_t txPin, uint8_t startLevel,
                                     const uint16_t* timings, uint16_t count,
                                     uint8_t repeats) {
  if (!timings || count < 8u || count > RAW_MAX_CHANGES || startLevel > 1u) {
    return false;
  }
  // An ISR/RMT capture can start while the receiver is already in the idle
  // LOW interval. That interval is not part of the command and replaying it
  // before every burst changes the receiver's framing. Start at the first
  // carrier edge instead.
  if (startLevel == LOW && timings[0] >= 3500u && count > 8u) {
    ++timings;
    --count;
    startLevel = HIGH;
  }
  repeats = constrain(repeats, 1u, 8u);
  pinMode(txPin, OUTPUT);
  for (uint8_t repeat = 0; repeat < repeats; ++repeat) {
    uint8_t level = startLevel;
    for (uint16_t i = 0; i < count; ++i) {
      digitalWrite(txPin, level);
      // The first duration is the inter-frame gap.  Limiting only that gap
      // prevents a damaged/noisy capture from making the UI appear frozen.
      const uint32_t duration = i == 0u ? std::min<uint32_t>(timings[i], 30000u)
                                        : timings[i];
      delayMicroseconds(duration);
      level = level == LOW ? HIGH : LOW;
    }
    const uint8_t finalLevel = startLevel ^ ((count - 1u) & 1u);
    const bool capturedGap = finalLevel == LOW && timings[count - 1u] >= 3500u;
    if (!capturedGap) {
      digitalWrite(txPin, LOW);
      delayMicroseconds(10000);
    }
  }
  digitalWrite(txPin, LOW);
  return true;
}

/** Hardware-timed OOK output.  RMT removes scheduler/interrupt jitter from
 * Replay and Saved Profiles. Long imported timings are split safely because a
 * legacy RMT duration field is 15 bits. */
static bool subghzTransmitRawRmt(uint8_t txPin, uint8_t startLevel,
                                const uint16_t* timings, uint16_t count,
                                uint8_t repeats, uint16_t interFrameGapUs) {
  if (!timings || count < 8u || count > RAW_MAX_CHANGES || startLevel > 1u) {
    return false;
  }
  if (startLevel == LOW && timings[0] >= 3500u && count > 8u) {
    ++timings;
    --count;
    startLevel = HIGH;
  }

  // A hardware RX frame normally contains the terminal LOW interval used to
  // close the RMT frame. Do not append another arbitrary 10 ms gap on top of
  // the measured one. ISR fallback captures do not contain it and retain the
  // requested safety gap.
  const uint8_t finalLevel = startLevel ^ ((count - 1u) & 1u);
  if (finalLevel == LOW && timings[count - 1u] >= 3500u) {
    interFrameGapUs = 0u;
  }

  struct Segment { uint16_t duration; uint8_t level; };
  // These buffers used to be local (~3 KiB). TX is launched from the Arduino
  // loop task, whose stack is already busy with the SubGHz UI and SD profile
  // parser; long RAW captures could therefore reboot the S3 at the moment TX
  // started. Only one SubGHz transmitter can own RMT, so static storage is the
  // safe and deterministic choice here.
  static Segment segments[(RAW_MAX_CHANGES * 2u) + 2u];
  static rmt_item32_t items[(RAW_MAX_CHANGES + 1u)];
  memset(segments, 0, sizeof(segments));
  memset(items, 0, sizeof(items));
  uint16_t segmentCount = 0;
  uint8_t level = startLevel;
  for (uint16_t i = 0; i < count; ++i) {
    uint32_t remaining = timings[i];
    if (remaining == 0u) return false;
    while (remaining > 0u && segmentCount < (RAW_MAX_CHANGES * 2u)) {
      const uint16_t part = (uint16_t)std::min<uint32_t>(remaining, 30000u);
      segments[segmentCount++] = Segment{part, level};
      remaining -= part;
    }
    if (remaining != 0u) return false;
    level = level == LOW ? HIGH : LOW;
  }
  if (interFrameGapUs > 0u) {
    segments[segmentCount++] = Segment{interFrameGapUs, LOW};
  }

  uint16_t itemCount = 0;
  for (uint16_t i = 0; i < segmentCount; i += 2u) {
    rmt_item32_t& item = items[itemCount++];
    item.level0 = segments[i].level;
    item.duration0 = segments[i].duration;
    if (i + 1u < segmentCount) {
      item.level1 = segments[i + 1u].level;
      item.duration1 = segments[i + 1u].duration;
    } else {
      item.level1 = LOW;
      item.duration1 = 1u;
    }
  }

  constexpr rmt_channel_t kTxChannel = RMT_CHANNEL_0;
  rmt_driver_uninstall(kTxChannel);  // Harmless if no previous owner exists.
  rmt_config_t config = RMT_DEFAULT_CONFIG_TX((gpio_num_t)txPin, kTxChannel);
  config.clk_div = 80;  // 1 us resolution.
  config.mem_block_num = 1;
  config.tx_config.idle_level = RMT_IDLE_LEVEL_LOW;
  config.tx_config.idle_output_en = true;

  esp_err_t err = rmt_config(&config);
  if (err == ESP_OK) err = rmt_driver_install(kTxChannel, 0, 0);
  if (err != ESP_OK) {
    Serial.printf("[subghz] RMT TX init failed err=%d\n", (int)err);
    rmt_driver_uninstall(kTxChannel);
    return false;
  }

  repeats = constrain(repeats, 1u, 12u);
  bool ok = true;
  for (uint8_t repeat = 0; repeat < repeats; ++repeat) {
    err = rmt_write_items(kTxChannel, items, itemCount, true);
    if (err != ESP_OK || rmt_wait_tx_done(kTxChannel, pdMS_TO_TICKS(500)) != ESP_OK) {
      ok = false;
      break;
    }
    yield();
  }
  rmt_driver_uninstall(kTxChannel);
  pinMode(txPin, INPUT);
  return ok;
}

static uint8_t subghzSafeRawRepeats(const uint16_t* timings, uint16_t count,
                                    uint8_t requested) {
  if (!timings || count == 0u) return 1u;
  uint32_t totalUs = 0u;
  for (uint16_t i = 0; i < count; ++i) {
    totalUs += timings[i];
    if (totalUs >= 180000u) break;
  }
  // The 24 ms RMT idle threshold captures the remote's complete burst, often
  // including its own repetitions. Repeating that large burst eight times was
  // both inaccurate and long enough to trigger watchdog/brownout failures.
  if (totalUs >= 120000u) return 1u;
  if (totalUs >= 60000u) return (uint8_t)std::min<uint8_t>(requested, 2u);
  return (uint8_t)constrain(requested, 1u, 4u);
}

static void subghzSetReplayNavLabels() {
  setTouchNavLabels("Freq-", "Save", "Exit", "Send", "Freq+");
}

static void subghzSetJammerNavLabels() {
  setTouchNavLabels("Freq-", "Auto", "Exit", "Toggle", "Freq+");
}

static void subghzSetProfileNavLabels() {
  setTouchNavLabels("SD", "Next", "Exit", "Prev", "TX");
}

static void subghzSetBruteNavLabels() {
  setTouchNavLabels("Prev", "Sel", "Exit", "Go", "Next");
}

enum class SubGhzOwner : uint8_t {
  None,
  Replay,
  SavedProfiles,
  Jammer,
  Brute,
  Detector,
};

static bool s_subghzLifecycleActive = false;
static SubGhzOwner s_subghzOwner = SubGhzOwner::None;

/**
 * Initialise the CC1101 only after the SD/PN532 bus owner has been released,
 * and verify the silicon status register.  A card that briefly holds MISO can
 * otherwise make Init() appear to succeed while every later register access
 * goes to the wrong slave.
 */
static bool subghzInitCc1101Reliable() {
  for (uint8_t attempt = 1; attempt <= 3; ++attempt) {
    reclaimSharedSpiBus();
#if defined(SD_CS)
    pinMode(SD_CS, OUTPUT);
    digitalWrite(SD_CS, HIGH);
#endif
#if defined(CC1101_CS)
    pinMode(CC1101_CS, OUTPUT);
    digitalWrite(CC1101_CS, HIGH);
#endif
    ELECHOUSE_cc1101.setSpiPin(CC1101_SCK, CC1101_MISO,
                               CC1101_MOSI, CC1101_CS);
    ELECHOUSE_cc1101.setGDO(CC1101_GDO0, CC1101_GDO2);
    ELECHOUSE_cc1101.Init();
    const uint8_t version = ELECHOUSE_cc1101.SpiReadStatus(CC1101_VERSION);
    if (version != 0x00u && version != 0xFFu) {
      s_subghzLifecycleActive = true;
      Serial.printf("[subghz] CC1101 ready attempt=%u version=0x%02X\n",
                    (unsigned)attempt, (unsigned)version);
      return true;
    }
    Serial.printf("[subghz] CC1101 bus retry %u/3 version=0x%02X\n",
                  (unsigned)attempt, (unsigned)version);
    digitalWrite(CC1101_CS, HIGH);
    delay((unsigned long)attempt * 8UL);
  }
  Serial.println(F("[subghz] CC1101 unavailable after bus recovery"));
  return false;
}

struct SubGhzTxDiagnostics {
  uint8_t marcState = 0xFF;
  uint8_t pktCtrl0 = 0xFF;
  uint8_t mdmCfg2 = 0xFF;
  uint8_t ioCfg0 = 0xFF;
};

/** Put the CC1101 into a known asynchronous OOK TX state.
 *
 * A bare SetTx() only sends the STX strobe; it does not repair registers left
 * behind by another SubGHz feature. Replay used to report "Done" even when
 * the radio never reached MARCSTATE=TX. This helper makes both live Replay and
 * Saved Profiles use the same complete configuration and verifies the radio
 * state before RMT drives GDO0.
 */
static bool subghzPrepareOokTx(uint32_t frequencyHz,
                               SubGhzTxDiagnostics* diag = nullptr) {
  reclaimSharedSpiBus();
#if defined(SD_CS)
  pinMode(SD_CS, OUTPUT);
  digitalWrite(SD_CS, HIGH);
#endif
#if defined(CC1101_CS)
  pinMode(CC1101_CS, OUTPUT);
  digitalWrite(CC1101_CS, HIGH);
#endif
  ELECHOUSE_cc1101.setSpiPin(CC1101_SCK, CC1101_MISO,
                             CC1101_MOSI, CC1101_CS);
  ELECHOUSE_cc1101.setGDO(CC1101_GDO0, CC1101_GDO2);
  ELECHOUSE_cc1101.setSidle();
  ELECHOUSE_cc1101.SpiStrobe(CC1101_SFRX);
  ELECHOUSE_cc1101.SpiStrobe(CC1101_SFTX);
  ELECHOUSE_cc1101.setMHZ(frequencyHz / 1000000.0f);
  ELECHOUSE_cc1101.setCCMode(0);       // PKTCTRL0=0x32, async serial.
  ELECHOUSE_cc1101.setModulation(2);   // ASK/OOK.
  ELECHOUSE_cc1101.setDRate(3.79f);
  ELECHOUSE_cc1101.SpiWriteReg(CC1101_PKTCTRL0, 0x32);
  ELECHOUSE_cc1101.SpiWriteReg(CC1101_MDMCFG2, 0x30);
  ELECHOUSE_cc1101.SpiWriteReg(CC1101_IOCFG0, 0x0D);
  ELECHOUSE_cc1101.SpiWriteReg(CC1101_IOCFG2, 0x0D);
  ELECHOUSE_cc1101.setPA(12);

  // GDO0 is the asynchronous serial data input in TX. Hold it LOW before STX
  // so entering TX cannot create an unintended carrier pulse.
  pinMode(CC1101_GDO0, OUTPUT);
  digitalWrite(CC1101_GDO0, LOW);
  ELECHOUSE_cc1101.SetTx();

  uint8_t marcState = 0xFF;
  const uint32_t deadline = millis() + 25u;
  do {
    marcState = ELECHOUSE_cc1101.SpiReadStatus(CC1101_MARCSTATE) & 0x1Fu;
    if (marcState == 0x13u) break;  // TX
    if (marcState == 0x16u) {       // TXFIFO_UNDERFLOW
      ELECHOUSE_cc1101.setSidle();
      ELECHOUSE_cc1101.SpiStrobe(CC1101_SFTX);
      ELECHOUSE_cc1101.SetTx();
    }
    delay(1);
  } while ((int32_t)(deadline - millis()) > 0);

  SubGhzTxDiagnostics local;
  local.marcState = marcState;
  local.pktCtrl0 = ELECHOUSE_cc1101.SpiReadReg(CC1101_PKTCTRL0);
  local.mdmCfg2 = ELECHOUSE_cc1101.SpiReadReg(CC1101_MDMCFG2);
  local.ioCfg0 = ELECHOUSE_cc1101.SpiReadReg(CC1101_IOCFG0);
  if (diag) *diag = local;

  const bool ready = local.marcState == 0x13u &&
                     local.pktCtrl0 == 0x32u &&
                     local.mdmCfg2 == 0x30u;
  Serial.printf("[subghz] TX prepare %s freq=%lu MARC=%02X PKT=%02X MDM=%02X IO0=%02X\n",
                ready ? "OK" : "FAIL", (unsigned long)frequencyHz,
                local.marcState, local.pktCtrl0, local.mdmCfg2, local.ioCfg0);
  return ready;
}

static void subghzBeginSession(SubGhzOwner owner) {
  SubGhzLifecycle::shutdown(false);
  pauseBackgroundRadioTasks();
  quiesceWifiSubsystem(true);
  neopixelClear();
  holdSdInactiveOnSharedSpi();
  reclaimSharedSpiBus();
  s_subghzOwner = owner;
  s_subghzLifecycleActive = true;
}

namespace replayat { void replayHandleNavButtons(); }
namespace subjammer { void subjammerHandleNavButtons(); }
namespace SavedProfile { void profileHandleNavButtons(); }
namespace SubBrute { void bruteHandleNavButtons(); }

namespace replayat {

#define EEPROM_SIZE SUBGHZ_EEPROM_SIZE

#define ADDR_VALUE         1280
#define ADDR_BITLEN        1284
#define ADDR_PROTO         1286
#define ADDR_FREQ          1288
#define ADDR_PROFILE_COUNT 1296
#define ADDR_PROFILE_START 1300
#define MAX_PROFILES       5

#define SCREEN_WIDTH  240
#define SCREENHEIGHT 320
#define SCREEN_HEIGHT 320

static bool uiDrawn = false;

void runUI();
void sendSignal();
void saveProfile(bool promptForName);
void updateDisplay();
static void replayInvalidateDisplay();
static void replayRestoreStatusPanel();

#define MAX_NAME_LENGTH 16

const char* randomNames[] = {
  "Signal", "Remote", "KeyFob", "GateOpener", "DoorLock",
  "RFTest", "Profile", "Control", "Switch", "Beacon"
};
const uint8_t numRandomNames = 10;

struct __attribute__((packed)) Profile {
    uint32_t frequency;
    uint32_t value;
    uint16_t bitLength;
    uint16_t protocol;
    char name[MAX_NAME_LENGTH];
};

#define PROFILE_SIZE sizeof(Profile)

uint16_t profileCount = 0;

RCSwitch mySwitch = RCSwitch();
arduinoFFT FFTSUB = arduinoFFT();

const uint16_t samplesSUB = ESP32DIV_FFT_SAMPLES;
const double FrequencySUB = 5000;

double attenuation_num = 10;

unsigned int sampling_period;
unsigned long micro_s;

double vRealSUB[samplesSUB];
double vImagSUB[samplesSUB];

byte red[ESP32DIV_FFT_PALETTE_SIZE], green[ESP32DIV_FFT_PALETTE_SIZE],
     blue[ESP32DIV_FFT_PALETTE_SIZE];

unsigned int epochSUB = 0;
unsigned int colorcursor = 2016;

int rssi;

static constexpr uint8_t REPLAY_RX_PIN = SUBGHZ_RX_PIN;
static constexpr uint8_t REPLAY_TX_PIN = SUBGHZ_TX_PIN;

// RCSwitch only exposes a frame after it has already matched one of its known
// protocols.  Its stock ISR also requires two frame gaps to differ by <200 us,
// which is too strict for a marginal OOK signal.  Keep RCSwitch for TX, but use
// a small double-buffered edge recorder for Replay RX so the UI can distinguish
// real pulse activity from RSSI and retry decoding with a measured tolerance.
static constexpr uint16_t REPLAY_RAW_MAX_CHANGES = RAW_MAX_CHANGES;
static constexpr uint16_t REPLAY_FRAME_GAP_US = 3500;

struct ReplayRawFrame {
  uint16_t count = 0;
  uint8_t startLevel = LOW;
  bool truncated = false;
  uint16_t timings[REPLAY_RAW_MAX_CHANGES]{};
};

static portMUX_TYPE s_replayPulseMux = portMUX_INITIALIZER_UNLOCKED;
static volatile uint32_t s_replayLastEdgeUs = 0;
static volatile uint16_t s_replayActiveCount = 0;
static volatile uint8_t s_replayActiveStartLevel = LOW;
static volatile uint16_t s_replayActiveTimings[REPLAY_RAW_MAX_CHANGES]{};
static volatile uint16_t s_replayReadyCount = 0;
static volatile uint8_t s_replayReadyStartLevel = LOW;
static volatile uint16_t s_replayReadyTimings[REPLAY_RAW_MAX_CHANGES]{};
static volatile bool s_replayRawReady = false;
static volatile bool s_replayActiveOverflow = false;
static volatile bool s_replayReadyOverflow = false;

#if BOARD_HAS_ESP32S3
// ESP32-S3 channels 4..7 are RX-capable. Channel 4 is unused while SubGHz
// owns the device; NeoPixels are explicitly off before Replay starts.
static constexpr rmt_channel_t REPLAY_RMT_RX_CHANNEL = RMT_CHANNEL_4;
#else
static constexpr rmt_channel_t REPLAY_RMT_RX_CHANNEL = RMT_CHANNEL_1;
#endif
static RingbufHandle_t s_replayRmtRing = nullptr;
static bool s_replayRmtArmed = false;

static void IRAM_ATTR replayPulseEdgeIsr() {
  const uint32_t now = micros();
  const uint32_t previous = s_replayLastEdgeUs;
  s_replayLastEdgeUs = now;
  if (previous == 0) return;

  const uint32_t elapsed = now - previous;
  const uint16_t duration = elapsed > 65535u ? 65535u : (uint16_t)elapsed;
  const uint8_t levelBeforeEdge = digitalRead(REPLAY_RX_PIN) ? LOW : HIGH;

  portENTER_CRITICAL_ISR(&s_replayPulseMux);
  if (elapsed > REPLAY_FRAME_GAP_US && s_replayActiveCount >= 8u) {
    if (!s_replayRawReady) {
      s_replayReadyCount = s_replayActiveCount;
      s_replayReadyStartLevel = s_replayActiveStartLevel;
      s_replayReadyOverflow = s_replayActiveOverflow;
      for (uint16_t i = 0; i < s_replayActiveCount; ++i) {
        s_replayReadyTimings[i] = s_replayActiveTimings[i];
      }
      s_replayRawReady = true;
    }
    s_replayActiveCount = 0;
    s_replayActiveOverflow = false;
  }

  if (s_replayActiveCount < REPLAY_RAW_MAX_CHANGES) {
    if (s_replayActiveCount == 0u) {
      s_replayActiveStartLevel = levelBeforeEdge;
    }
    s_replayActiveTimings[s_replayActiveCount++] = duration;
  } else {
    // Preserve the prefix for diagnostics, but never treat an incomplete
    // frame as saveable or replayable.
    s_replayActiveOverflow = true;
  }
  portEXIT_CRITICAL_ISR(&s_replayPulseMux);
}

static void replayResetPulseCapture() {
  // The stable V1.20 receiver uses the same RCSwitch ISR for both decoded
  // packets and bounded RAW frames. Clear its pending frame together with the
  // legacy buffers so a frequency/menu change can never reuse an old burst.
  mySwitch.resetRawAvailable();
  portENTER_CRITICAL(&s_replayPulseMux);
  s_replayLastEdgeUs = 0;
  s_replayActiveCount = 0;
  s_replayReadyCount = 0;
  s_replayActiveStartLevel = LOW;
  s_replayReadyStartLevel = LOW;
  s_replayRawReady = false;
  s_replayActiveOverflow = false;
  s_replayReadyOverflow = false;
  portEXIT_CRITICAL(&s_replayPulseMux);
}

static bool replayTakeRawFrame(ReplayRawFrame& out) {
  // Primary V1.20 path: RCSwitch owns GPIO3 and publishes the complete edge
  // sequence even when none of its protocol decoders recognise the remote.
  // This avoids running a second ISR or allocating four RMT RX blocks.
  if (mySwitch.rawAvailable()) {
    out = ReplayRawFrame{};
    unsigned int rawCount = mySwitch.getReceivedRawCount();
    unsigned int* raw = mySwitch.getReceivedRawCapture();
    out.startLevel = mySwitch.getReceivedRawStartLevel() ? HIGH : LOW;
    if (rawCount > REPLAY_RAW_MAX_CHANGES) {
      rawCount = REPLAY_RAW_MAX_CHANGES;
      out.truncated = true;
    }
    out.count = (uint16_t)rawCount;
    for (uint16_t i = 0; i < out.count; ++i) {
      out.timings[i] = (uint16_t)std::min<unsigned int>(raw[i], 65535u);
    }
    mySwitch.resetRawAvailable();
    return out.count >= 8u;
  }

  if (s_replayRmtArmed && s_replayRmtRing) {
    size_t bytes = 0;
    rmt_item32_t* items = static_cast<rmt_item32_t*>(
        xRingbufferReceive(s_replayRmtRing, &bytes, 0));
    if (items) {
      out = ReplayRawFrame{};
      const size_t itemCount = bytes / sizeof(rmt_item32_t);
      bool haveLevel = false;
      for (size_t i = 0; i < itemCount; ++i) {
        const rmt_item32_t& item = items[i];
        if (item.duration0 > 0) {
          if (!haveLevel) {
            out.startLevel = item.level0 ? HIGH : LOW;
            haveLevel = true;
          }
          if (out.count < REPLAY_RAW_MAX_CHANGES) out.timings[out.count++] = item.duration0;
          else out.truncated = true;
        }
        if (item.duration1 > 0) {
          if (!haveLevel) {
            out.startLevel = item.level1 ? HIGH : LOW;
            haveLevel = true;
          }
          if (out.count < REPLAY_RAW_MAX_CHANGES) out.timings[out.count++] = item.duration1;
          else out.truncated = true;
        }
      }
      vRingbufferReturnItem(s_replayRmtRing, items);
      if (out.count >= 8u) return true;
    }
  }

  bool ready = false;
  portENTER_CRITICAL(&s_replayPulseMux);
  if (s_replayRawReady) {
    out.count = s_replayReadyCount;
    out.startLevel = s_replayReadyStartLevel;
    out.truncated = s_replayReadyOverflow;
    for (uint16_t i = 0; i < out.count; ++i) {
      out.timings[i] = s_replayReadyTimings[i];
    }
    s_replayRawReady = false;
    s_replayReadyOverflow = false;
    ready = true;
  }
  portEXIT_CRITICAL(&s_replayPulseMux);
  return ready;
}

/** RCSwitch::disableReceive() errors if no ISR was ever attached — track arm state. */
static bool s_replayRxArmed = false;

static bool replayStartRmtReceive() {
  if (s_replayRmtArmed) return true;
  rmt_config_t config = {};
  config.rmt_mode = RMT_MODE_RX;
  config.channel = REPLAY_RMT_RX_CHANNEL;
  config.gpio_num = (gpio_num_t)REPLAY_RX_PIN;
  config.clk_div = 80;  // 80 MHz / 80 = 1 us resolution.
  // Four blocks provide 256 hardware items. One block truncated longer remote
  // frames before software ever saw them.
  config.mem_block_num = 4;
  config.flags = 0;
  // Fan/ceiling-controller remotes often encode one command as several short
  // packets separated by 5-15 ms. A 5 ms threshold captured only one fragment
  // and Replay then repeated that fragment, which looks active on-screen but
  // is rejected by the appliance. Keep the complete command burst together.
  config.rx_config.idle_threshold = 24000;  // 24 ms silence closes a burst.
  config.rx_config.filter_en = true;
  // Keep short pulses from inexpensive remotes. The calibrated RSSI gate and
  // repeated-frame matcher perform the noise rejection in software.
  config.rx_config.filter_ticks_thresh = 40;

  esp_err_t err = rmt_config(&config);
  if (err == ESP_OK) err = rmt_driver_install(REPLAY_RMT_RX_CHANNEL, 4096, 0);
  if (err == ESP_OK) err = rmt_get_ringbuf_handle(REPLAY_RMT_RX_CHANNEL,
                                                  &s_replayRmtRing);
  if (err == ESP_OK && s_replayRmtRing) {
    err = rmt_rx_start(REPLAY_RMT_RX_CHANNEL, true);
  }
  if (err != ESP_OK || !s_replayRmtRing) {
    rmt_driver_uninstall(REPLAY_RMT_RX_CHANNEL);
    s_replayRmtRing = nullptr;
    Serial.printf("[subghz] RMT RX unavailable err=%d; ISR fallback\n", (int)err);
    return false;
  }
  s_replayRmtArmed = true;
  Serial.printf("[subghz] RMT RX GPIO%u channel=%u blocks=4 idle=24ms filter=40us\n",
                (unsigned)REPLAY_RX_PIN, (unsigned)REPLAY_RMT_RX_CHANNEL);
  return true;
}

static void replayStopRmtReceive() {
  if (!s_replayRmtArmed) return;
  rmt_rx_stop(REPLAY_RMT_RX_CHANNEL);
  rmt_driver_uninstall(REPLAY_RMT_RX_CHANNEL);
  s_replayRmtRing = nullptr;
  s_replayRmtArmed = false;
}

static void replayArmReceive() {
  pinMode(REPLAY_RX_PIN, INPUT);
  pinMode(REPLAY_TX_PIN, INPUT);
  // DIV V2.1 official path: RCSwitch owns GDO2/RX (GPIO3).  The later RAW
  // RMT receiver consumed four RMT blocks and was the source of long-frame
  // timing variation and intermittent resets when switching back to TX.
  mySwitch.resetAvailable();
  mySwitch.resetRawAvailable();
  mySwitch.enableReceive(REPLAY_RX_PIN);
  s_replayRxArmed = true;
}

static void replayDisarmReceive() {
  if (!s_replayRxArmed) {
    return;
  }
  mySwitch.disableReceive();
  s_replayRxArmed = false;
}

uint32_t receivedValue = 0;
uint16_t receivedBitLength = 0;
uint16_t receivedProtocol = 0;
const int rssi_threshold = -75;

static const uint32_t subghz_frequency_list[] = {
    300000000, 303875000, 304250000, 310000000, 314000000, 315000000,
    318000000, 390000000, 418000000, 433075000, 433420000, 433920000,
    434420000, 434775000, 438900000, 868350000, 915000000, 925000000
};
static constexpr uint16_t REPLAY_FREQ_COUNT =
    (uint16_t)(sizeof(subghz_frequency_list) /
               sizeof(subghz_frequency_list[0]));

// Replay opens on the common 433.92 MHz preset. Runtime state is not written
// to flash; every session starts from this known-good configuration.
uint16_t currentFrequencyIndex = 11;
// Leave a real gap below the 20..40 px icon toolbar. At y=40 the toolbar
// separator crossed the first Replay text row and clipped its header glyphs.
int yshift = 23;

static bool autoScanEnabled = false;
static uint16_t scanIndex = 0;
static uint32_t lastHopMs = 0;
static uint32_t lockUntilMs = 0;
// AUTO favours complete frames over headline sweep speed. The old 220 ms
// dwell left barely three RSSI samples after tuning and could hop before the
// RCSwitch ISR was consumed. A complete 18-frequency pass remains under
// roughly six seconds while each channel now gets a useful receive window.
static constexpr uint32_t SCAN_DWELL_MS = 300;
static constexpr uint32_t SCAN_DWELL_LOW_MS = 420;
static constexpr uint32_t SCAN_SETTLE_MS = 70;
static constexpr uint32_t SCAN_SETTLE_LOW_MS = 100;
static constexpr uint32_t SCAN_SETTLE_BAND_MS = 170;
static constexpr uint32_t LOCK_HOLD_MS  = 2500;
static constexpr uint32_t RSSI_LOCK_MS  = 1700;
static constexpr int      RSSI_DETECT_THRESHOLD = -90;
static constexpr int      RSSI_DETECT_THRESHOLD_LOW = -92;
static constexpr int      RSSI_CLEAR_THRESHOLD  = -96;
static constexpr int      RSSI_CLEAR_THRESHOLD_LOW = -98;
static constexpr int      RSSI_DECODE_THRESHOLD = -92;
static constexpr int      RSSI_DECODE_THRESHOLD_LOW = -94;
static constexpr uint32_t RSSI_SAMPLE_MS = 35;
static constexpr uint8_t  RSSI_DETECT_HITS = 2;
static constexpr uint8_t  RSSI_DETECT_HITS_LOW = 2;
static constexpr uint32_t DECODE_MIN_DWELL_MS = 80;
static constexpr uint32_t DECODE_MIN_DWELL_LOW_MS = 110;
static constexpr uint32_t UI_SCAN_UPDATE_MS = 250;
// 270 kHz tolerates the frequency error of inexpensive 315/433 MHz remotes
// while the AGC and pulse decoder still reject broadband noise.
static constexpr float    REPLAY_RX_BW_KHZ = 270.0f;
static constexpr uint8_t  REPLAY_RX_TOLERANCE = 70;
static uint32_t lastUiScanUpdateMs = 0;
static uint32_t scanSettledAtMs = 0;
static uint32_t lastRssiSampleMs = 0;
static uint8_t  rssiDetectStreak = 0;
static bool     rssiHot = false;
static int16_t  scanNoiseFloor[REPLAY_FREQ_COUNT];
static bool     scanNoiseReady[REPLAY_FREQ_COUNT];
static uint32_t lastUndecodedPulseMs = 0;
static uint32_t lastUndecodedLogMs = 0;
static uint32_t pendingDecodedValue = 0;
static uint16_t pendingDecodedBits = 0;
static uint16_t pendingDecodedProtocol = 0;
static uint32_t pendingDecodedAtMs = 0;
static ReplayRawFrame latestRawFrame;
static bool latestRawValid = false;
static uint32_t latestRawAtMs = 0;
static uint32_t lastDecodedCaptureMs = 0;
static ReplayRawFrame pendingRawFrame;
static uint8_t pendingRawRepeats = 0;
static uint32_t pendingRawAtMs = 0;
static uint16_t lastRawCandidateCount = 0;
static bool lastRawCandidateWasNoise = false;

static constexpr uint16_t REPLAY_RAW_MIN_CHANGES = 10;
static constexpr uint16_t REPLAY_RAW_LONG_FRAME_CHANGES = 20;
static constexpr uint32_t REPLAY_RAW_CONFIRM_WINDOW_MS = 2500;

static constexpr uint32_t REPLAY_CALIBRATION_MS = 15000;
static constexpr uint32_t REPLAY_CAL_SAMPLE_MS = 10;
static constexpr int REPLAY_RSSI_MIN = -120;
static constexpr int REPLAY_RSSI_MAX = -20;
static constexpr size_t REPLAY_RSSI_BINS =
    (size_t)(REPLAY_RSSI_MAX - REPLAY_RSSI_MIN + 1);
static bool s_replayCalibrating = false;
static uint32_t s_replayCalibrationStartedMs = 0;
static uint32_t s_replayCalibrationEndsMs = 0;
static uint32_t s_replayCalibrationLastSampleMs = 0;
static uint32_t s_replayCalibrationSamples = 0;
static uint32_t s_replayRssiHistogram[REPLAY_RSSI_BINS]{};
static int s_replayNoiseFloor = -100;
static int s_replaySignalThreshold = -94;
static int s_replayRecentPeakRssi = REPLAY_RSSI_MIN;
static uint32_t s_replayRecentPeakAtMs = 0;
static int s_replayActivityPeakRssi = REPLAY_RSSI_MIN;
static uint32_t s_replayRfActivityUntilMs = 0;

static bool replayRecentSignalStrong(uint32_t now, int currentRssi);

/** Compare two RMT frames while allowing a one-edge alignment difference and
 * normal component/timing tolerance. Real remotes repeat almost the same pulse
 * train; random OOK noise does not. */
static bool replayRawFramesSimilar(const ReplayRawFrame& a,
                                   const ReplayRawFrame& b) {
  if (a.count < REPLAY_RAW_MIN_CHANGES || b.count < REPLAY_RAW_MIN_CHANGES) {
    return false;
  }
  const int countDelta = (int)a.count - (int)b.count;
  const int allowedCountDelta = std::max<int>(
      4, (int)std::min(a.count, b.count) * 30 / 100);
  if (abs(countDelta) > allowedCountDelta) return false;

  int bestPercent = 0;
  for (int shift = -3; shift <= 3; ++shift) {
    uint16_t matches = 0;
    uint16_t compared = 0;
    for (int ai = 0; ai < (int)a.count; ++ai) {
      const int bi = ai + shift;
      if (bi < 0 || bi >= (int)b.count) continue;
      const uint32_t av = a.timings[ai];
      const uint32_t bv = b.timings[bi];
      if (av == 0u || bv == 0u) continue;
      const uint32_t tolerance = std::max<uint32_t>(140u,
          std::max(av, bv) * 45u / 100u);
      const uint32_t diff = av > bv ? av - bv : bv - av;
      if (diff <= tolerance) matches++;
      compared++;
    }
    if (compared >= REPLAY_RAW_MIN_CHANGES) {
      bestPercent = std::max(bestPercent, (int)(matches * 100u / compared));
    }
  }
  return bestPercent >= 65;
}

static bool replayRecentSignalStrong(uint32_t now, int currentRssi);

/** A RAW frame becomes actionable only after a matching repeat. This prevents
 * short noise bursts (such as the observed 15-pulse/−93 dBm frame) from
 * becoming R-SAVE and later being transmitted as if they were a real remote. */
static bool replayConsiderRawFrame(const ReplayRawFrame& frame,
                                   uint32_t now, int frameRssi) {
  lastRawCandidateCount = frame.count;
  if (frame.truncated || frame.count < REPLAY_RAW_MIN_CHANGES ||
      frame.count > RAW_MAX_CHANGES) {
    lastRawCandidateWasNoise = true;
    pendingRawRepeats = 0;
    pendingRawAtMs = now;
    Serial.printf("[subghz] RAW ignored: incomplete frame (%u changes)\n",
                  (unsigned)frame.count);
    return false;
  }

  const bool activityHeld = s_replayRfActivityUntilMs != 0u &&
      (int32_t)(now - s_replayRfActivityUntilMs) < 0;
  // AUTO can receive a complete long burst between two 35 ms RSSI samples.
  // Once the tuner has settled, the structure of that frame is sufficient
  // evidence to keep it as a candidate; short frames still need RSSI evidence
  // and a matching repeat.  This avoids missing fast remotes without accepting
  // the 10-15 edge noise bursts observed on this board.
  const bool autoStructuredFrame = autoScanEnabled &&
      now >= scanSettledAtMs &&
      frame.count >= REPLAY_RAW_LONG_FRAME_CHANGES;
  if (!autoStructuredFrame && !activityHeld &&
      !replayRecentSignalStrong(now, frameRssi)) {
    lastRawCandidateWasNoise = true;
    pendingRawRepeats = 0;
    pendingRawAtMs = now;
    Serial.printf("[subghz] RAW ignored: weak frame (%u changes rssi=%d)\n",
                  (unsigned)frame.count, frameRssi);
    return false;
  }

  const bool repeated = pendingRawRepeats > 0u &&
      (uint32_t)(now - pendingRawAtMs) <= REPLAY_RAW_CONFIRM_WINDOW_MS &&
      replayRawFramesSimilar(pendingRawFrame, frame);
  if (repeated) {
    pendingRawRepeats = (uint8_t)std::min<unsigned int>(
        (unsigned int)pendingRawRepeats + 1u, 255u);
  } else {
    pendingRawFrame = frame;
    pendingRawRepeats = 1u;
  }
  pendingRawAtMs = now;

  // Long frames carry enough structure to be useful on their own. Shorter
  // frames must repeat once, which rejects the 10-15 edge noise bursts seen
  // on the DIV V2.1 while still accepting ordinary remote controls quickly.
  const bool accepted = frame.count >= REPLAY_RAW_LONG_FRAME_CHANGES ||
                        pendingRawRepeats >= 2u;
  if (!accepted) {
    lastRawCandidateWasNoise = false;
    Serial.printf("[subghz] RAW candidate changes=%u; waiting repeat\n",
                  (unsigned)frame.count);
    return false;
  }

  latestRawFrame = frame;
  latestRawValid = true;
  latestRawAtMs = now;
  lastRawCandidateWasNoise = false;
  Serial.printf("[subghz] RAW captured changes=%u repeats=%u rssi=%d\n",
                (unsigned)frame.count, (unsigned)pendingRawRepeats, frameRssi);
  return true;
}

/** Stable OOK asynchronous receiver preset adapted from the useful parts of
 * HaleHound's CC1101 path. Both GDO pins are configured because this DIV V2.1
 * captures on GDO2/GPIO3, not HaleHound's GDO0. */
static void replayConfigureCc1101Rx() {
  ELECHOUSE_cc1101.setCCMode(0);
  ELECHOUSE_cc1101.setModulation(2);
  ELECHOUSE_cc1101.setRxBW(REPLAY_RX_BW_KHZ);
  ELECHOUSE_cc1101.setDRate(3.79f);
  ELECHOUSE_cc1101.SpiWriteReg(CC1101_MDMCFG2, 0x30);  // OOK, async, no sync.
  ELECHOUSE_cc1101.SpiWriteReg(CC1101_AGCCTRL2, 0x03);
  ELECHOUSE_cc1101.SpiWriteReg(CC1101_AGCCTRL1, 0x00);
  ELECHOUSE_cc1101.SpiWriteReg(CC1101_AGCCTRL0, 0x40);
  ELECHOUSE_cc1101.SpiWriteReg(CC1101_FREND1, 0xB6);
  ELECHOUSE_cc1101.SpiWriteReg(CC1101_FREND0, 0x11);
  ELECHOUSE_cc1101.SpiWriteReg(CC1101_IOCFG2, 0x0D);
  ELECHOUSE_cc1101.SpiWriteReg(CC1101_IOCFG0, 0x0D);
}

/** Exact receive preset used by the original ESP32-DIV replay path.  Keep it
 * separate from the analyser preset above: the extra async/RMT register writes
 * were useful for raw diagnostics, but they are not part of the stable DIV
 * RCSwitch receive/transmit lifecycle. */
static void replayInitOfficialDivRadio() {
  ELECHOUSE_cc1101.Init();
  ELECHOUSE_cc1101.setCCMode(0);
  ELECHOUSE_cc1101.setModulation(2);
  ELECHOUSE_cc1101.setRxBW(500.0);
}

static void replayLogCc1101RxConfig() {
  Serial.printf("[subghz] RX cfg BW=270k DR=3.79 IOCFG2=%02X IOCFG0=%02X MDMCFG2=%02X AGC=%02X/%02X/%02X\n",
                ELECHOUSE_cc1101.SpiReadReg(CC1101_IOCFG2),
                ELECHOUSE_cc1101.SpiReadReg(CC1101_IOCFG0),
                ELECHOUSE_cc1101.SpiReadReg(CC1101_MDMCFG2),
                ELECHOUSE_cc1101.SpiReadReg(CC1101_AGCCTRL2),
                ELECHOUSE_cc1101.SpiReadReg(CC1101_AGCCTRL1),
                ELECHOUSE_cc1101.SpiReadReg(CC1101_AGCCTRL0));
}

static void replayResetDecodeTracking() {
  replayResetPulseCapture();
  lastUndecodedPulseMs = 0;
  lastUndecodedLogMs = 0;
  pendingDecodedValue = 0;
  pendingDecodedBits = 0;
  pendingDecodedProtocol = 0;
  pendingDecodedAtMs = 0;
  latestRawFrame = ReplayRawFrame{};
  latestRawValid = false;
  latestRawAtMs = 0;
  lastDecodedCaptureMs = 0;
  pendingRawFrame = ReplayRawFrame{};
  pendingRawRepeats = 0;
  pendingRawAtMs = 0;
  lastRawCandidateCount = 0;
  lastRawCandidateWasNoise = false;
  s_replayRecentPeakRssi = REPLAY_RSSI_MIN;
  s_replayRecentPeakAtMs = 0;
  s_replayActivityPeakRssi = REPLAY_RSSI_MIN;
  s_replayRfActivityUntilMs = 0;
}

static int replayClampRssi(int rssi) {
  return constrain(rssi, REPLAY_RSSI_MIN, REPLAY_RSSI_MAX);
}

/** Feed both calibration and the short peak-hold used to associate a burst of
 * RF energy with the RMT frame that closes a few milliseconds later. */
static void replayObserveRssi(int rssi) {
  const uint32_t now = millis();
  rssi = replayClampRssi(rssi);
  if (s_replayCalibrating) {
    if (s_replayCalibrationLastSampleMs != 0u &&
        (uint32_t)(now - s_replayCalibrationLastSampleMs) <
            REPLAY_CAL_SAMPLE_MS) {
      return;
    }
    s_replayCalibrationLastSampleMs = now;
    s_replayRssiHistogram[(size_t)(rssi - REPLAY_RSSI_MIN)]++;
    s_replayCalibrationSamples++;
    s_replayNoiseFloor = s_replayCalibrationSamples == 1u
        ? rssi : (s_replayNoiseFloor * 7 + rssi) / 8;
    s_replaySignalThreshold = constrain(s_replayNoiseFloor + 4, -105, -45);
    return;
  }

  // RCSwitch only reports protocols it can decode.  Preserve the official
  // decoder while exposing RF energy as a diagnostic state, so an unsupported
  // remote is distinguishable from a silent receiver.
  const int activityThreshold = constrain(
      std::max(s_replayNoiseFloor + 10, -88), -88, -50);
  s_replaySignalThreshold = activityThreshold;
  if (rssi >= activityThreshold) {
    if ((int32_t)(now - s_replayRfActivityUntilMs) >= 0) {
      s_replayActivityPeakRssi = rssi;
    } else {
      s_replayActivityPeakRssi = std::max(s_replayActivityPeakRssi, rssi);
    }
    s_replayRfActivityUntilMs = now + 450u;
  } else if ((int32_t)(now - s_replayRfActivityUntilMs) >= 0) {
    // Follow idle noise slowly, but never train the floor on the active burst.
    s_replayNoiseFloor = (s_replayNoiseFloor * 31 + rssi) / 32;
  }

  if (s_replayRecentPeakAtMs == 0u ||
      (uint32_t)(now - s_replayRecentPeakAtMs) > 350u ||
      rssi > s_replayRecentPeakRssi) {
    s_replayRecentPeakRssi = rssi;
    s_replayRecentPeakAtMs = now;
  }
}

static bool replayRecentSignalStrong(uint32_t now, int currentRssi) {
  const bool recent = s_replayRecentPeakAtMs != 0u &&
      (uint32_t)(now - s_replayRecentPeakAtMs) <= 350u;
  const int observed = recent ? std::max(currentRssi, s_replayRecentPeakRssi)
                              : currentRssi;
  // Two dB of grace accounts for the RSSI decay between the last modulation
  // edge and the 5 ms RMT idle interval that closes the frame.
  return observed >= s_replaySignalThreshold - 2;
}

static uint8_t replayCalibrationSecondsRemaining() {
  if (!s_replayCalibrating) return 0;
  const uint32_t now = millis();
  if ((int32_t)(s_replayCalibrationEndsMs - now) <= 0) return 0;
  return (uint8_t)std::min<uint32_t>(
      15u, (s_replayCalibrationEndsMs - now + 999u) / 1000u);
}

static void replayBeginCalibration() {
  ReplayRawFrame discarded;
  while (replayTakeRawFrame(discarded)) {}
  replayResetDecodeTracking();
  // A calibration belongs to the current frequency. Do not let a previously
  // decoded packet appear saveable after changing bands or re-entering Replay.
  receivedValue = 0;
  receivedBitLength = 0;
  receivedProtocol = 0;
  s_replayCalibrating = false;
  Serial.printf("[subghz] replay ready %.2f MHz (no calibration gate)\n",
                subghz_frequency_list[currentFrequencyIndex] / 1000000.0f);
  replayInvalidateDisplay();
}

static void replayFinishCalibration() {
  if (!s_replayCalibrating) return;
  s_replayCalibrating = false;

  if (s_replayCalibrationSamples > 0u) {
    // The 85th percentile ignores isolated remote presses during calibration
    // while following a persistently elevated local noise floor.
    const uint32_t target =
        (s_replayCalibrationSamples * 85u + 99u) / 100u;
    uint32_t cumulative = 0;
    int percentile = REPLAY_RSSI_MIN;
    for (size_t i = 0; i < REPLAY_RSSI_BINS; ++i) {
      cumulative += s_replayRssiHistogram[i];
      if (cumulative >= target) {
        percentile = REPLAY_RSSI_MIN + (int)i;
        break;
      }
    }
    s_replayNoiseFloor = percentile;
  } else {
    s_replayNoiseFloor = replayClampRssi(ELECHOUSE_cc1101.getRssi());
  }
  s_replaySignalThreshold = constrain(s_replayNoiseFloor + 4, -105, -45);

  ReplayRawFrame discarded;
  while (replayTakeRawFrame(discarded)) {}
  replayResetDecodeTracking();
  Serial.printf("[subghz] calibration complete samples=%lu noise=%d threshold=%d\n",
                (unsigned long)s_replayCalibrationSamples,
                s_replayNoiseFloor, s_replaySignalThreshold);
  replayInvalidateDisplay();
}

static void replayCalibrationPoll() {
  s_replayCalibrating = false;
}

static bool replayLooksLikeRealDecode(uint32_t value, uint16_t bits,
                                      uint16_t proto);

static bool replayRawPreferred() {
  if (!latestRawValid || latestRawFrame.truncated ||
      latestRawFrame.count < REPLAY_RAW_MIN_CHANGES ||
      latestRawFrame.count > RAW_MAX_CHANGES) {
    return false;
  }

  // If RCSwitch decoded the same burst, keep the original DIV protocol TX.
  // RAW is selected only for an otherwise unsupported capture.
  const bool decodedSameBurst = replayLooksLikeRealDecode(
      receivedValue, receivedBitLength, receivedProtocol) &&
      lastDecodedCaptureMs != 0u &&
      (uint32_t)abs((int32_t)(latestRawAtMs - lastDecodedCaptureMs)) <= 900u;
  return !decodedSameBurst;
}

static uint32_t lastDetectAlertMs = 0;
static uint16_t lastDetectAlertFreq = 0xFFFF;
static uint32_t notifHideAtMs = 0;
static bool notifActive = false;

static void replayBeep(uint16_t hz = 2200, uint16_t ms = 60) {
  soundBeep(hz, ms);
}

static void replayBeepPoll() {
  soundPoll();
}

static void replayShowDetectNotice(const String& reason, int rssi = 0) {
  uint32_t now = millis();

  if (now - lastDetectAlertMs < 1200 && lastDetectAlertFreq == currentFrequencyIndex) return;
  lastDetectAlertMs = now;
  lastDetectAlertFreq = currentFrequencyIndex;

  char msg[96];
  float mhz = subghz_frequency_list[currentFrequencyIndex] / 1000000.0f;

  snprintf(msg, sizeof(msg), "%s @ %.2f MHz | RSSI %d", reason.c_str(), mhz, rssi);
  showNotificationActions("SubGHz Detected", msg, true);
  replayBeep(reason == "DECODE" ? 2600 : 2000, 70);
  notifActive = true;
  notifHideAtMs = 0;
}

static inline uint16_t freqCount() {
  return REPLAY_FREQ_COUNT;
}

static bool replayFreqIsLowBand(uint16_t idx) {
  return subghz_frequency_list[idx % freqCount()] < 350000000UL;
}

static uint8_t replayFreqBandId(uint16_t idx) {
  const uint32_t hz = subghz_frequency_list[idx % freqCount()];
  if (hz < 350000000UL) {
    return 0;
  }
  if (hz < 500000000UL) {
    return 1;
  }
  return 2;
}

static bool replayFreqBandChanged(uint16_t prevIdx, uint16_t newIdx) {
  return replayFreqBandId(prevIdx) != replayFreqBandId(newIdx);
}

static bool s_freqPersistPending = false;
static uint32_t s_freqPersistAtMs = 0;

static void replayPersistFrequencyNow() {
  s_freqPersistPending = false;
}

static void replayPersistFrequencyPoll() {
  if (s_freqPersistPending &&
      (int32_t)(millis() - s_freqPersistAtMs) >= 0) {
    replayPersistFrequencyNow();
  }
}

static int replayRssiDetectThreshold() {
  const uint16_t idx = currentFrequencyIndex % REPLAY_FREQ_COUNT;
  const int absoluteFloor = replayFreqIsLowBand(idx)
      ? RSSI_DETECT_THRESHOLD_LOW : RSSI_DETECT_THRESHOLD;
  if (!autoScanEnabled || !scanNoiseReady[idx]) return absoluteFloor;
  // Per-frequency noise tracking prevents a noisy band from locking the
  // entire sweep, while the absolute floor still catches the -85/-90 dBm
  // remotes that the previous fixed -58 dBm threshold missed completely.
  return constrain(std::max<int>(absoluteFloor, scanNoiseFloor[idx] + 8),
                   absoluteFloor, -48);
}

static int replayRssiClearThreshold() {
  if (autoScanEnabled) return replayRssiDetectThreshold() - 5;
  return replayFreqIsLowBand(currentFrequencyIndex)
      ? RSSI_CLEAR_THRESHOLD_LOW : RSSI_CLEAR_THRESHOLD;
}

static int replayRssiDecodeThreshold() {
  if (autoScanEnabled) return replayRssiDetectThreshold() - 2;
  return replayFreqIsLowBand(currentFrequencyIndex)
      ? RSSI_DECODE_THRESHOLD_LOW : RSSI_DECODE_THRESHOLD;
}

static uint32_t replayScanDwellMs() {
  return replayFreqIsLowBand(currentFrequencyIndex) ? SCAN_DWELL_LOW_MS : SCAN_DWELL_MS;
}

static uint32_t replayDecodeMinDwellMs() {
  return replayFreqIsLowBand(currentFrequencyIndex) ? DECODE_MIN_DWELL_LOW_MS
                                                    : DECODE_MIN_DWELL_MS;
}

static void replayDeselectSharedSpiSlaves() {
#if defined(SD_CS)
  pinMode(SD_CS, OUTPUT);
  digitalWrite(SD_CS, HIGH);
#endif
#if defined(PN532_SS)
  pinMode(PN532_SS, OUTPUT);
  digitalWrite(PN532_SS, HIGH);
#endif
#if defined(CSN_PIN_1)
  pinMode(CSN_PIN_1, OUTPUT);
  digitalWrite(CSN_PIN_1, HIGH);
#endif
#if defined(CSN_PIN_2)
  pinMode(CSN_PIN_2, OUTPUT);
  digitalWrite(CSN_PIN_2, HIGH);
#endif
#if defined(CSN_PIN_3)
  pinMode(CSN_PIN_3, OUTPUT);
  digitalWrite(CSN_PIN_3, HIGH);
#endif
#if defined(CC1101_CS)
  pinMode(CC1101_CS, OUTPUT);
  digitalWrite(CC1101_CS, HIGH);
#endif
}

static void tuneToIndex(uint16_t idx, bool persist = true) {
  const uint16_t previousIndex = currentFrequencyIndex;
  currentFrequencyIndex = idx % freqCount();
  const bool receiverWasArmed = s_replayRxArmed;
  if (receiverWasArmed) replayDisarmReceive();
  // AUTO never mounts or probes the card. Keep every other shared-SPI slave
  // explicitly deselected before the CC1101 driver repeatedly begin()/end()s
  // the host while calculating and calibrating a new frequency.
  replayDeselectSharedSpiSlaves();
  ELECHOUSE_cc1101.setSidle();
  delay(3);
  ELECHOUSE_cc1101.setMHZ(subghz_frequency_list[currentFrequencyIndex] / 1000000.0);
  delay(replayFreqBandChanged(previousIndex, currentFrequencyIndex) ? 10 : 4);
  ELECHOUSE_cc1101.SetRx();
  const uint32_t rxDeadline = millis() +
      (replayFreqBandChanged(previousIndex, currentFrequencyIndex) ? 24u : 14u);
  uint8_t marcState = 0xFFu;
  do {
    marcState = ELECHOUSE_cc1101.SpiReadStatus(CC1101_MARCSTATE) & 0x1Fu;
    if (marcState == 0x0Du) break;  // RX
    delay(1);
  } while ((int32_t)(rxDeadline - millis()) > 0);
  if (marcState != 0x0Du) {
    // One bounded retry is enough; never spin indefinitely inside AUTO.
    ELECHOUSE_cc1101.setSidle();
    delay(2);
    ELECHOUSE_cc1101.SetRx();
    delay(4);
    marcState = ELECHOUSE_cc1101.SpiReadStatus(CC1101_MARCSTATE) & 0x1Fu;
    Serial.printf("[subghz] tune RX retry %.2f MHz MARC=%02X\n",
                  subghz_frequency_list[currentFrequencyIndex] / 1000000.0f,
                  (unsigned)marcState);
  }
  if (receiverWasArmed) replayArmReceive();
  (void)persist;
}

/** SD and CC1101 share the board SPI bus.  Saving a profile mounts SD, which
 * changes the controller/clock and leaves the radio unable to RX/TX unless its
 * complete setup is replayed. */
static void replayRestoreRadioAfterSd() {
  replayDisarmReceive();
  reclaimSharedSpiBus();
#if defined(SD_CS)
  pinMode(SD_CS, OUTPUT);
  digitalWrite(SD_CS, HIGH);
#endif
#if defined(CC1101_CS)
  pinMode(CC1101_CS, OUTPUT);
  digitalWrite(CC1101_CS, HIGH);
#endif
  ELECHOUSE_cc1101.setSpiPin(CC1101_SCK, CC1101_MISO, CC1101_MOSI, CC1101_CS);
  ELECHOUSE_cc1101.setGDO(CC1101_GDO0, CC1101_GDO2);
  replayInitOfficialDivRadio();
  tuneToIndex(currentFrequencyIndex, false);
  mySwitch.setReceiveTolerance(replayFreqIsLowBand(currentFrequencyIndex) ? 50 : 40);
  pinMode(REPLAY_RX_PIN, INPUT);
  pinMode(REPLAY_TX_PIN, INPUT);
  delay(40);
  replayArmReceive();
  Serial.println(F("[subghz] CC1101 restored after SD access"));
}

static void replayClearScanLock() {
  lockUntilMs = 0;
  rssiHot = false;
  rssiDetectStreak = 0;
}

static bool replayLooksLikeRealDecode(uint32_t value, uint16_t bits, uint16_t proto) {
  if (value == 0) {
    return false;
  }
  if (bits < 8 || bits > 64) {
    return false;
  }
  if (proto < 1 || proto > 12) {
    return false;
  }
  return true;
}

struct ReplayProtocolShape {
  uint16_t nominalPulse;
  uint8_t syncHigh;
  uint8_t syncLow;
  uint8_t zeroHigh;
  uint8_t zeroLow;
  uint8_t oneHigh;
  uint8_t oneLow;
  bool inverted;
};

// Same twelve encodings used by RCSwitch.  The decoder below differs only in
// how it estimates the base pulse and in requiring two equal decoded frames.
static const ReplayProtocolShape kReplayProtocols[] = {
  {350, 1, 31, 1, 3, 3, 1, false},
  {650, 1, 10, 1, 2, 2, 1, false},
  {100, 30, 71, 4, 11, 9, 6, false},
  {380, 1, 6, 1, 3, 3, 1, false},
  {500, 6, 14, 1, 2, 2, 1, false},
  {450, 23, 1, 1, 2, 2, 1, true},
  {150, 2, 62, 1, 6, 6, 1, false},
  {200, 3, 130, 7, 16, 3, 16, false},
  {200, 130, 7, 16, 7, 16, 3, true},
  {365, 18, 1, 3, 1, 1, 3, true},
  {270, 36, 1, 1, 2, 2, 1, true},
  {320, 36, 1, 1, 2, 2, 1, true},
};

/** Encode a decoded RCSwitch packet into the same hardware-timed RMT path used
 * by RAW replay. This preserves the protocol's sync gap while removing task
 * scheduling jitter from digitalWrite()/delayMicroseconds(). */
static bool replayTransmitDecodedRmt(uint8_t txPin, uint32_t value,
                                     uint16_t bits, uint16_t protocol,
                                     uint8_t repeats) {
  if (value == 0u || bits < 8u || bits > 32u ||
      protocol == 0u || protocol >
          (sizeof(kReplayProtocols) / sizeof(kReplayProtocols[0]))) {
    return false;
  }

  const ReplayProtocolShape& shape = kReplayProtocols[protocol - 1u];
  uint16_t timings[68]{};
  uint16_t count = 0;
  uint8_t startLevel = shape.inverted ? LOW : HIGH;
  const uint16_t pulse = shape.nominalPulse;

  for (int bit = bits - 1; bit >= 0; --bit) {
    const bool one = ((value >> bit) & 1u) != 0u;
    const uint8_t highMul = one ? shape.oneHigh : shape.zeroHigh;
    const uint8_t lowMul = one ? shape.oneLow : shape.zeroLow;
    timings[count++] = (uint16_t)(pulse * highMul);
    timings[count++] = (uint16_t)(pulse * lowMul);
  }
  timings[count++] = (uint16_t)std::min<uint32_t>(65535u,
      (uint32_t)pulse * shape.syncHigh);
  timings[count++] = (uint16_t)std::min<uint32_t>(65535u,
      (uint32_t)pulse * shape.syncLow);
  return subghzTransmitRawRmt(txPin, startLevel, timings, count, repeats, 0u);
}

static uint32_t replayPulseDiff(uint32_t a, uint32_t b) {
  return a > b ? a - b : b - a;
}

static bool replayDecodeRawAtBase(const ReplayRawFrame& frame,
                                  const ReplayProtocolShape& shape,
                                  uint16_t basePulse,
                                  uint32_t& valueOut,
                                  uint16_t& bitsOut,
                                  uint32_t& scoreOut) {
  if (basePulse < 35u || basePulse > 2000u) return false;
  const uint16_t first = shape.inverted ? 2u : 1u;
  if (frame.count <= first + 15u) return false;

  const uint16_t bits = (frame.count - first) / 2u;
  if (bits < 8u || bits > 32u) return false;

  const uint32_t tolerance = std::max<uint32_t>(80u,
      (uint32_t)basePulse * REPLAY_RX_TOLERANCE / 100u);
  uint32_t value = 0;
  uint32_t score = 0;

  for (uint16_t bit = 0, i = first; bit < bits; ++bit, i += 2u) {
    const uint32_t high = frame.timings[i];
    const uint32_t low = frame.timings[i + 1u];
    const uint32_t zHigh = (uint32_t)basePulse * shape.zeroHigh;
    const uint32_t zLow = (uint32_t)basePulse * shape.zeroLow;
    const uint32_t oHigh = (uint32_t)basePulse * shape.oneHigh;
    const uint32_t oLow = (uint32_t)basePulse * shape.oneLow;
    const uint32_t ze1 = replayPulseDiff(high, zHigh);
    const uint32_t ze2 = replayPulseDiff(low, zLow);
    const uint32_t oe1 = replayPulseDiff(high, oHigh);
    const uint32_t oe2 = replayPulseDiff(low, oLow);
    const bool zeroOk = ze1 <= tolerance && ze2 <= tolerance;
    const bool oneOk = oe1 <= tolerance && oe2 <= tolerance;
    if (!zeroOk && !oneOk) return false;

    value <<= 1u;
    if (oneOk && (!zeroOk || (oe1 + oe2) < (ze1 + ze2))) {
      value |= 1u;
      score += oe1 + oe2;
    } else {
      score += ze1 + ze2;
    }
  }

  valueOut = value;
  bitsOut = bits;
  scoreOut = score;
  return value != 0u;
}

static bool replayDecodeRawFrame(const ReplayRawFrame& frame,
                                 uint32_t& valueOut,
                                 uint16_t& bitsOut,
                                 uint16_t& protocolOut,
                                 uint16_t& pulseOut) {
  bool found = false;
  uint32_t bestScore = UINT32_MAX;

  for (uint16_t p = 0; p < (sizeof(kReplayProtocols) / sizeof(kReplayProtocols[0])); ++p) {
    const ReplayProtocolShape& shape = kReplayProtocols[p];
    uint16_t candidates[20]{};
    uint8_t candidateCount = 0;
    const uint8_t syncPulses = std::max(shape.syncHigh, shape.syncLow);
    candidates[candidateCount++] = syncPulses ? frame.timings[0] / syncPulses : 0u;
    candidates[candidateCount++] = shape.nominalPulse;

    const uint8_t factors[] = {shape.zeroHigh, shape.zeroLow,
                               shape.oneHigh, shape.oneLow};
    const uint16_t first = shape.inverted ? 2u : 1u;
    for (uint16_t i = first; i < frame.count && i < first + 4u; ++i) {
      for (uint8_t f = 0; f < 4u && candidateCount < 20u; ++f) {
        if (factors[f] != 0u) {
          candidates[candidateCount++] = frame.timings[i] / factors[f];
        }
      }
    }

    for (uint8_t c = 0; c < candidateCount; ++c) {
      uint32_t candidateValue = 0;
      uint16_t candidateBits = 0;
      uint32_t candidateScore = 0;
      if (!replayDecodeRawAtBase(frame, shape, candidates[c], candidateValue,
                                 candidateBits, candidateScore)) {
        continue;
      }
      if (!found || candidateScore < bestScore) {
        found = true;
        bestScore = candidateScore;
        valueOut = candidateValue;
        bitsOut = candidateBits;
        protocolOut = p + 1u;
        pulseOut = candidates[c];
      }
    }
  }
  return found;
}

static void replayScanHopTo(uint16_t idx, uint16_t fromIdx) {
  tuneToIndex(idx, false);
  replayResetDecodeTracking();

  uint32_t settleMs = SCAN_SETTLE_MS;
  if (replayFreqBandChanged(fromIdx, idx)) {
    settleMs = SCAN_SETTLE_BAND_MS;
  } else if (replayFreqIsLowBand(idx)) {
    settleMs = SCAN_SETTLE_LOW_MS;
  }
  scanSettledAtMs = millis() + settleMs;
  rssiDetectStreak = 0;
  lastRssiSampleMs = 0;
}

static bool replayPrepareAutoScanBus() {
  // An AUTO session owns the shared SPI wires exclusively. Release any stale
  // SD/PN532 transaction once, then leave SD untouched until Save or feature
  // exit explicitly transfers ownership back through the guarded lifecycle.
  replayDisarmReceive();
  holdSdInactiveOnSharedSpi();
  reclaimSharedSpiBus();
  replayDeselectSharedSpiSlaves();
  ELECHOUSE_cc1101.setSpiPin(CC1101_SCK, CC1101_MISO,
                             CC1101_MOSI, CC1101_CS);
  ELECHOUSE_cc1101.setGDO(CC1101_GDO0, CC1101_GDO2);
  replayInitOfficialDivRadio();
  const uint8_t version = ELECHOUSE_cc1101.SpiReadStatus(CC1101_VERSION);
  if (version == 0x00u || version == 0xFFu) {
    Serial.printf("[subghz] AUTO bus unavailable VERSION=%02X\n",
                  (unsigned)version);
    return false;
  }
  tuneToIndex(currentFrequencyIndex, false);
  mySwitch.setReceiveTolerance(
      replayFreqIsLowBand(currentFrequencyIndex) ? 50 : 40);
  replayArmReceive();
  Serial.printf("[subghz] AUTO bus ready VERSION=%02X; SD isolated\n",
                (unsigned)version);
  return true;
}

static bool replayBeginAutoScan() {
  if (!replayPrepareAutoScanBus()) return false;
  for (uint16_t i = 0; i < REPLAY_FREQ_COUNT; ++i) {
    scanNoiseFloor[i] = -100;
    scanNoiseReady[i] = false;
  }
  scanIndex = currentFrequencyIndex;
  // Listen on the currently displayed frequency first instead of immediately
  // skipping it. Replay normally opens at 433.92 MHz, the most useful preset.
  lastHopMs = millis();
  scanSettledAtMs = lastHopMs +
      (replayFreqIsLowBand(currentFrequencyIndex) ? SCAN_SETTLE_LOW_MS
                                                   : SCAN_SETTLE_MS);
  lastRssiSampleMs = 0;
  replayClearScanLock();
  lastUiScanUpdateMs = 0;
  replayResetDecodeTracking();
  Serial.printf("[subghz] AUTO start %.2f MHz, %u frequencies\n",
                subghz_frequency_list[currentFrequencyIndex] / 1000000.0f,
                (unsigned)REPLAY_FREQ_COUNT);
  return true;
}

static bool replayAutoScanReadyForDecode(uint32_t now) {
  if (lastHopMs == 0 || (now - lastHopMs) < replayDecodeMinDwellMs()) {
    return false;
  }
  if (now < scanSettledAtMs) {
    return false;
  }
  // A complete decoded/RAW frame is itself stronger evidence than a single
  // RSSI register read taken after the burst has ended. Signal qualification
  // is performed by replayConsiderRawFrame and the adaptive RSSI sampler.
  return true;
}

static void replaySampleRssiForScan(uint32_t now) {
  if (now < scanSettledAtMs) {
    return;
  }
  if (lastRssiSampleMs != 0 && (now - lastRssiSampleMs) < RSSI_SAMPLE_MS) {
    return;
  }

  const int rssi = ELECHOUSE_cc1101.getRssi();
  const uint16_t idx = currentFrequencyIndex % REPLAY_FREQ_COUNT;
  const int detectThreshold = replayRssiDetectThreshold();
  s_replaySignalThreshold = detectThreshold;
  const uint8_t detectHits = replayFreqIsLowBand(currentFrequencyIndex) ? RSSI_DETECT_HITS_LOW
                                                                        : RSSI_DETECT_HITS;
  if (rssi > detectThreshold) {
    if (s_replayRecentPeakAtMs == 0u ||
        (uint32_t)(now - s_replayRecentPeakAtMs) > 450u ||
        rssi > s_replayRecentPeakRssi) {
      s_replayRecentPeakRssi = rssi;
      s_replayRecentPeakAtMs = now;
    }
    s_replayActivityPeakRssi = rssiHot
        ? std::max(s_replayActivityPeakRssi, rssi) : rssi;
    s_replayRfActivityUntilMs = now + 550u;
    if (rssiDetectStreak < 255) {
      rssiDetectStreak++;
    }
    if (!rssiHot && rssiDetectStreak >= detectHits) {
      rssiHot = true;
      lockUntilMs = now + RSSI_LOCK_MS;
      // RSSI alone identifies a likely frequency, not a saveable packet.  A
      // modal notice here used to return from ReplayAttackLoop before the RMT
      // queue was consumed: AUTO appeared to find the remote, while Save had
      // no frame and silently did nothing.  Hold this frequency and keep RX
      // running; the notice is raised only after a complete frame is latched.
      Serial.printf("[subghz] AUTO RSSI lock %.2f MHz rssi=%d; waiting frame\n",
                    subghz_frequency_list[currentFrequencyIndex] / 1000000.0f,
                    rssi);
    }
  } else {
    rssiDetectStreak = 0;
    if (rssiHot && rssi < replayRssiClearThreshold()) {
      rssiHot = false;
    }
    // Learn only quiet samples. Never let an active remote raise its own
    // threshold and disappear from the next sweep.
    if (!rssiHot) {
      if (!scanNoiseReady[idx]) {
        scanNoiseFloor[idx] = constrain(rssi, REPLAY_RSSI_MIN, -55);
        scanNoiseReady[idx] = true;
      } else {
        scanNoiseFloor[idx] = (int16_t)(
            (scanNoiseFloor[idx] * 7 + rssi) / 8);
      }
      s_replayNoiseFloor = scanNoiseFloor[idx];
    }
  }
  lastRssiSampleMs = now;
}

static void replayFreqNext() {
  autoScanEnabled = false;
  replayClearScanLock();
  replayResetDecodeTracking();
  tuneToIndex((uint16_t)((currentFrequencyIndex + 1) % freqCount()), true);
  replayBeginCalibration();
  updateDisplay();
}

static void replayFreqPrev() {
  autoScanEnabled = false;
  replayClearScanLock();
  replayResetDecodeTracking();
  tuneToIndex((uint16_t)((currentFrequencyIndex + freqCount() - 1) % freqCount()), true);
  replayBeginCalibration();
  updateDisplay();
}

static void replayToggleAuto() {
  autoScanEnabled = !autoScanEnabled;
  if (autoScanEnabled) {
    s_replayCalibrating = false;
    ReplayRawFrame discarded;
    while (replayTakeRawFrame(discarded)) {}
    if (!replayBeginAutoScan()) {
      autoScanEnabled = false;
      replayClearScanLock();
    }
  } else {
    replayClearScanLock();
    replayBeginCalibration();
  }
  updateDisplay();
}

static bool replayRequireValidFrame(const char* action) {
  if (replayRawPreferred()) {
    return true;
  }
  if (receivedValue != 0u && receivedValue != UINT32_MAX) {
    return true;
  }

  Serial.printf("[subghz] %s rejected: no complete frame raw=%u candidate=%u\n",
                action ? action : "Action", latestRawValid ? 1u : 0u,
                (unsigned)lastRawCandidateCount);
  tft.fillRect(0, 40, 240, 46, TFT_BLACK);
  tft.setTextColor(TFT_RED, TFT_BLACK);
  tft.setCursor(10, 30 + yshift);
  tft.printf("Nothing to %s", action ? action : "save");
  tft.setTextColor(UI_WARN, TFT_BLACK);
  tft.setCursor(10, 45 + yshift);
  tft.print("Wait for SAVE / R-SAVE");
  subghzWaitWithNav(1300);
  replayRestoreStatusPanel();
  return false;
}

static void replayTrySave(bool promptForName) {
  if (!replayRequireValidFrame("Save")) return;
  autoScanEnabled = false;
  replayClearScanLock();
  saveProfile(promptForName);
}

static bool s_replayStaticDrawn = false;

struct ReplayDisplayCache {
  uint16_t freqIndex = 0xFFFF;
  uint8_t modeState = 0xFF;
  uint16_t bitLength = 0xFFFF;
  int16_t rssi = -9999;
  uint16_t protocol = 0xFFFF;
  uint32_t value = 0xFFFFFFFF;
  uint16_t rawCount = 0xFFFF;
  uint8_t rawRepeats = 0xFF;
  uint8_t calibrationSeconds = 0xFF;
  int16_t signalThreshold = INT16_MIN;
  int16_t activityPeak = INT16_MIN;
  uint8_t captureState = 0xFF;
  bool valid = false;
};

static ReplayDisplayCache s_replayDisp;

static void replayInvalidateDisplay() {
  s_replayStaticDrawn = false;
  s_replayDisp = ReplayDisplayCache{};
}

static void replayRestoreStatusPanel() {
  replayInvalidateDisplay();
  updateDisplay();
}

static uint8_t replayModeState() {
  const bool locked = (autoScanEnabled && lockUntilMs != 0 &&
                       (int32_t)(millis() - lockUntilMs) < 0);
  if (locked) {
    return 2;
  }
  return autoScanEnabled ? 1 : 0;
}

static uint8_t replayCaptureState() {
  if (replayRawPreferred()) {
    return 3;  // A complete raw edge capture can be saved/replayed.
  }
  if (receivedValue != 0 && receivedValue != UINT32_MAX) {
    return 2;  // A decoded frame is available for Save/TX.
  }
  if (s_replayRfActivityUntilMs != 0u &&
      (int32_t)(millis() - s_replayRfActivityUntilMs) < 0) {
    return 6;  // RF energy present, but no supported RCSwitch decode yet.
  }
  if (lastUndecodedPulseMs != 0 &&
      (uint32_t)(millis() - lastUndecodedPulseMs) < 1600u) {
    return lastRawCandidateWasNoise ? 1 : 4;  // Rejected noise or candidate.
  }
  return 0;    // RSSI activity alone is not a capturable frame.
}

static constexpr int kReplayStatusLineY = 86;
static constexpr int kReplayValueLineH = 11;

static void replayDrawStatusSeparator() {
  tft.drawFastHLine(0, kReplayStatusLineY, 240, UI_LINE);
}

static void replayDrawValueCell(int x, int y, int w, int h, const String& text, uint16_t color) {
  const int maxH = kReplayStatusLineY - y;
  if (maxH <= 0) {
    return;
  }
  const int clipH = min(h, maxH);
  tft.fillRect(x, y, w, clipH, TFT_BLACK);
  tft.setTextSize(1);
  tft.setTextColor(color, TFT_BLACK);
  tft.setCursor(x, y);
  tft.print(text);
}

static void replayDrawStaticChrome() {
  if (s_replayStaticDrawn) {
    return;
  }

  const int bodyBottom = subghzContentBottom();
  const int infoH = min(kReplayStatusLineY - 40, bodyBottom - 40);
  if (infoH > 0) {
    tft.fillRect(0, 40, 240, infoH, TFT_BLACK);
  }
  replayDrawStatusSeparator();

  tft.setTextSize(1);
  tft.setTextColor(UI_TEXT, TFT_BLACK);
  tft.setCursor(5, 20 + yshift);
  tft.print("Freq:");
  tft.setCursor(5, 35 + yshift);
  tft.print("Bit:");
  tft.setCursor(130, 35 + yshift);
  tft.print("RSSI:");
  tft.setCursor(130, 20 + yshift);
  tft.print("Ptc:");
  tft.setCursor(5, 50 + yshift);
  tft.print("Data:");
  tft.setCursor(145, 50 + yshift);
  tft.print("RX:");

  s_replayStaticDrawn = true;
}

void replayHandleNavButtons() {
  if (!featureHasTouchNavBar()) {
    return;
  }

  if (isTouchNavButtonPressedEdge(BTN_LEFT)) {
    replayFreqPrev();
    subghzWaitNavRelease(BTN_LEFT);
  }
  if (isTouchNavButtonPressedEdge(BTN_RIGHT)) {
    replayFreqNext();
    subghzWaitNavRelease(BTN_RIGHT);
  }
  if (isTouchNavButtonPressedEdge(BTN_UP)) {
    autoScanEnabled = false;
    replayClearScanLock();
    sendSignal();
    subghzWaitNavRelease(BTN_UP);
  }
  if (isTouchNavButtonPressedEdge(BTN_DOWN)) {
    // Save must remain usable even with a marginal touch panel.  Use an
    // automatic name; profiles can still be managed from Saved Profiles.
    replayTrySave(false);
    subghzWaitNavRelease(BTN_DOWN);
  }
}

void updateDisplay() {
    replayDrawStaticChrome();

    const uint8_t modeState = replayModeState();
    const int16_t rssi = ELECHOUSE_cc1101.getRssi();
    char freqBuf[16];
    char modeBuf[8];
    char bitBuf[8];
    char rssiBuf[8];
    char ptcBuf[8];
    char valBuf[16];
    char captureBuf[8];
    const uint8_t calibrationSeconds = replayCalibrationSecondsRemaining();

    snprintf(freqBuf, sizeof(freqBuf), "%.2f MHz",
             subghz_frequency_list[currentFrequencyIndex] / 1000000.0);
    if (modeState == 2) {
      snprintf(modeBuf, sizeof(modeBuf), "LOCK");
    } else {
      snprintf(modeBuf, sizeof(modeBuf), "%s", modeState == 1 ? "AUTO" : "MAN ");
    }
    snprintf(rssiBuf, sizeof(rssiBuf), "%d", rssi);
    const uint8_t captureState = replayCaptureState();
    if (captureState == 5) {
      snprintf(bitBuf, sizeof(bitBuf), "--");
      snprintf(ptcBuf, sizeof(ptcBuf), "CAL");
      snprintf(valBuf, sizeof(valBuf), "N%d T%d",
               s_replayNoiseFloor, s_replaySignalThreshold);
    } else if (captureState == 3) {
      snprintf(bitBuf, sizeof(bitBuf), "--");
      snprintf(ptcBuf, sizeof(ptcBuf), "RAW");
      snprintf(valBuf, sizeof(valBuf), "%u pulses", (unsigned)latestRawFrame.count);
    } else if (captureState == 4) {
      snprintf(bitBuf, sizeof(bitBuf), "--");
      snprintf(ptcBuf, sizeof(ptcBuf), "RAW");
      snprintf(valBuf, sizeof(valBuf), "%up x%u",
               (unsigned)lastRawCandidateCount,
               (unsigned)pendingRawRepeats);
    } else if (captureState == 1) {
      snprintf(bitBuf, sizeof(bitBuf), "--");
      snprintf(ptcBuf, sizeof(ptcBuf), "NOISE");
      snprintf(valBuf, sizeof(valBuf), "%up reject",
               (unsigned)lastRawCandidateCount);
    } else if (captureState == 6) {
      snprintf(bitBuf, sizeof(bitBuf), "--");
      snprintf(ptcBuf, sizeof(ptcBuf), "RF");
      snprintf(valBuf, sizeof(valBuf), "%d dBm", s_replayActivityPeakRssi);
    } else if (captureState == 0 && receivedValue == 0u) {
      snprintf(bitBuf, sizeof(bitBuf), "--");
      snprintf(ptcBuf, sizeof(ptcBuf), "--");
      snprintf(valBuf, sizeof(valBuf), "Listening");
    } else {
      snprintf(bitBuf, sizeof(bitBuf), "%d", receivedBitLength);
      snprintf(ptcBuf, sizeof(ptcBuf), "%d", receivedProtocol);
      snprintf(valBuf, sizeof(valBuf), "%lu", (unsigned long)receivedValue);
    }
    if (captureState == 5) {
      snprintf(captureBuf, sizeof(captureBuf), "CAL %us",
               (unsigned)calibrationSeconds);
    } else {
      snprintf(captureBuf, sizeof(captureBuf), "%s", captureState == 6 ? "SIGNAL" :
               (captureState == 3 ? "R-SAVE" :
               (captureState == 2 ? "SAVE" : (captureState == 4 ? "CAND" :
               (captureState == 1 ? "NOISE" : "LISTEN")))));
    }

    const bool fullRedraw = !s_replayDisp.valid;
    const bool calibrationTickChanged =
        s_replayDisp.calibrationSeconds != calibrationSeconds;
    if (fullRedraw || s_replayDisp.freqIndex != currentFrequencyIndex) {
      replayDrawValueCell(50, 20 + yshift, 72, kReplayValueLineH, freqBuf, UI_WARN);
      s_replayDisp.freqIndex = currentFrequencyIndex;
    }
    if (fullRedraw || s_replayDisp.modeState != modeState) {
      replayDrawValueCell(207, 20 + yshift, 31, kReplayValueLineH, modeBuf, UI_WARN);
      s_replayDisp.modeState = modeState;
    }
    if (fullRedraw || s_replayDisp.bitLength != receivedBitLength ||
        s_replayDisp.captureState != captureState) {
      replayDrawValueCell(50, 35 + yshift, 40, kReplayValueLineH, bitBuf, UI_WARN);
      s_replayDisp.bitLength = receivedBitLength;
    }
    if (fullRedraw || s_replayDisp.rssi != rssi) {
      replayDrawValueCell(170, 35 + yshift, 48, kReplayValueLineH, rssiBuf, UI_WARN);
      s_replayDisp.rssi = rssi;
    }
    if (fullRedraw || s_replayDisp.protocol != receivedProtocol ||
        s_replayDisp.captureState != captureState) {
      replayDrawValueCell(170, 20 + yshift, 40, kReplayValueLineH, ptcBuf, UI_WARN);
      s_replayDisp.protocol = receivedProtocol;
    }
    if (fullRedraw || s_replayDisp.value != receivedValue ||
        s_replayDisp.rawCount != lastRawCandidateCount ||
        s_replayDisp.rawRepeats != pendingRawRepeats ||
        s_replayDisp.calibrationSeconds != calibrationSeconds ||
        s_replayDisp.signalThreshold != s_replaySignalThreshold ||
        s_replayDisp.activityPeak != s_replayActivityPeakRssi ||
        s_replayDisp.captureState != captureState) {
      replayDrawValueCell(50, 50 + yshift, 90, kReplayValueLineH, valBuf, UI_WARN);
      s_replayDisp.value = receivedValue;
      s_replayDisp.rawCount = lastRawCandidateCount;
      s_replayDisp.rawRepeats = pendingRawRepeats;
      s_replayDisp.calibrationSeconds = calibrationSeconds;
      s_replayDisp.signalThreshold = s_replaySignalThreshold;
      s_replayDisp.activityPeak = s_replayActivityPeakRssi;
    }
    if (fullRedraw || s_replayDisp.captureState != captureState ||
        calibrationTickChanged) {
      const uint16_t stateColor = (captureState == 2 || captureState == 3) ? TFT_GREEN
                                  : (captureState == 6 ? TFT_ORANGE
                                  : (captureState == 4 ? TFT_ORANGE
                                  : (captureState == 5 ? TFT_CYAN : TFT_RED)));
      replayDrawValueCell(165, 50 + yshift, 70, kReplayValueLineH,
                          captureBuf, stateColor);
      s_replayDisp.captureState = captureState;
    }

    replayDrawStatusSeparator();

    s_replayDisp.valid = true;
    /* Do NOT idle/retune here — that drops RCSwitch pulse timing mid-receive. */
}

String getUserInputName() {
  OnScreenKeyboardConfig cfg;
  cfg.titleLine1     = "[!] Set a name for the saved profile.";
  cfg.titleLine2     = "(max 15 chars, ^ caps, # sym)";
  osKeyboardUseStandardLayout(cfg);
  cfg.maxLen         = MAX_NAME_LENGTH - 1;
  cfg.shuffleNames   = randomNames;
  cfg.shuffleCount   = numRandomNames;
  cfg.buttonsY       = 195;
  cfg.backLabel      = "Back";
  cfg.middleLabel    = "Shuffle";
  cfg.okLabel        = "OK";
  cfg.enableShuffle  = true;
  cfg.requireNonEmpty = true;
  cfg.emptyErrorMsg  = "Name cannot be empty!";

  OnScreenKeyboardResult r = showOnScreenKeyboard(cfg, "");

  if (!r.accepted) {

    subghzClearBody(TFT_BLACK);
    uiDrawn = false;
    replayRestoreStatusPanel();
    runUI();
    subghzRedrawNavChrome();
  }
  return r.text;
}

void sendSignal() {
    const uint32_t replayHz = subghz_frequency_list[currentFrequencyIndex % freqCount()];
    const bool useRaw = replayRawPreferred();
    const bool decodedValid = replayLooksLikeRealDecode(
        receivedValue, receivedBitLength, receivedProtocol);
    if (!useRaw && !decodedValid) {
      tft.fillRect(0, 40, 240, kReplayStatusLineY - 40, TFT_BLACK);
      tft.setTextColor(TFT_RED, TFT_BLACK);
      tft.setCursor(10, 30 + yshift);
      tft.print("Nothing captured");
      tft.setCursor(10, 45 + yshift);
      tft.print("Capture a signal first");
      subghzWaitWithNav(1200);
      replayRestoreStatusPanel();
      return;
    }

    replayDisarmReceive();
    tft.fillRect(0, 40, 240, kReplayStatusLineY - 40, TFT_BLACK);
    tft.setTextColor(UI_TEXT, TFT_BLACK);
    tft.setCursor(10, 30 + yshift);
    tft.print(useRaw ? "Sending RAW..." : "Sending...");
    tft.setCursor(10, 40 + yshift);
    if (useRaw) {
      tft.printf("%u pulses", (unsigned)latestRawFrame.count);
    } else {
      tft.print(receivedValue);
    }

    const uint32_t txStartedMs = millis();
    bool txOk = false;
    if (useRaw) {
      delay(20);
      SubGhzTxDiagnostics diag;
      const bool radioReady = subghzPrepareOokTx(replayHz, &diag);
      const uint8_t repeats = subghzSafeRawRepeats(
          latestRawFrame.timings, latestRawFrame.count, 4u);
      txOk = radioReady && subghzTransmitRawRmt(
          REPLAY_TX_PIN, latestRawFrame.startLevel, latestRawFrame.timings,
          latestRawFrame.count, repeats, 10000u);
      Serial.printf("[subghz] RAW TX %s in %lums freq=%lu pulses=%u repeats=%u\n",
                    txOk ? "OK" : "FAIL",
                    (unsigned long)(millis() - txStartedMs),
                    (unsigned long)replayHz,
                    (unsigned)latestRawFrame.count, (unsigned)repeats);
    } else {
      delay(100);
      ELECHOUSE_cc1101.setSidle();
      ELECHOUSE_cc1101.setMHZ(replayHz / 1000000.0f);
      pinMode(REPLAY_TX_PIN, OUTPUT);
      mySwitch.enableTransmit(REPLAY_TX_PIN);
      ELECHOUSE_cc1101.SetTx();
      mySwitch.setProtocol(receivedProtocol);
      mySwitch.setRepeatTransmit(8);
      mySwitch.send(receivedValue, receivedBitLength);
      yield();
      txOk = true;
      Serial.printf("[subghz] official DIV TX in %lums freq=%lu bits=%u proto=%u\n",
                    (unsigned long)(millis() - txStartedMs),
                    (unsigned long)replayHz, receivedBitLength, receivedProtocol);
    }

    subghzWaitWithNav(350);
    tft.fillRect(0, 40, 240, kReplayStatusLineY - 40, TFT_BLACK);
    tft.setCursor(10, 30 + yshift);
    tft.setTextColor(txOk ? TFT_GREEN : TFT_RED, TFT_BLACK);
    tft.print(txOk ? "Done!" : "RAW TX failed");

    mySwitch.disableTransmit();
    pinMode(REPLAY_TX_PIN, INPUT);
    pinMode(REPLAY_RX_PIN, INPUT);
    replayRestoreRadioAfterSd();

    subghzWaitWithNav(350);
    replayRestoreStatusPanel();
}

void do_sampling() {
  constexpr unsigned int kGraphYOffset = 87;
  const int plotY = (int)epochSUB + (int)kGraphYOffset;
  if (plotY >= subghzContentBottom()) {
    return;
  }

  micro_s = micros();

  #define ALPHA 0.2
  float ewmaRSSI = -50;

for (int i = 0; i < samplesSUB; i++) {
    int rssi = ELECHOUSE_cc1101.getRssi();
    replayObserveRssi(rssi);
    rssi += 100;

    ewmaRSSI = (ALPHA * rssi) + ((1 - ALPHA) * ewmaRSSI);

    vRealSUB[i] = ewmaRSSI * 2;
    vImagSUB[i] = 1;

    while (micros() < micro_s + sampling_period);
    micro_s += sampling_period;
}

  double mean = 0;

  for (uint16_t i = 0; i < samplesSUB; i++)
        mean += vRealSUB[i];
        mean /= samplesSUB;
  for (uint16_t i = 0; i < samplesSUB; i++)
        vRealSUB[i] -= mean;

  micro_s = micros();

  FFTSUB.Windowing(vRealSUB, samplesSUB, FFT_WIN_TYP_HAMMING, FFT_FORWARD);
  FFTSUB.Compute(vRealSUB, vImagSUB, samplesSUB, FFT_FORWARD);
  FFTSUB.ComplexToMagnitude(vRealSUB, vImagSUB, samplesSUB);

unsigned int left_x = 120;
unsigned int graph_y_offset = kGraphYOffset;
int max_k = 0;

for (int j = 0; j < samplesSUB >> 1; j++) {
    int k = vRealSUB[j] / attenuation_num;
    if (k > max_k)
        max_k = k;
    if (k > 127) k = 127;

    unsigned int color = red[k] << 11 | green[k] << 5 | blue[k];
    unsigned int vertical_x = left_x + j;

    tft.drawPixel(vertical_x, epochSUB + graph_y_offset, color);
}

for (int j = 0; j < samplesSUB >> 1; j++) {
    int k = vRealSUB[j] / attenuation_num;
    if (k > max_k)
        max_k = k;
    if (k > 127) k = 127;

    unsigned int color = red[k] << 11 | green[k] << 5 | blue[k];
    unsigned int mirrored_x = left_x - j;
    tft.drawPixel(mirrored_x, epochSUB + graph_y_offset, color);
}

  double tattenuation = max_k / 127.0;

  if (tattenuation > attenuation_num)
    attenuation_num = tattenuation;

    delay(10);
}

void readProfileCount() {
    EEPROM.get(ADDR_PROFILE_COUNT, profileCount);
    if (profileCount > MAX_PROFILES) profileCount = 0;
}

void saveProfile(bool promptForName) {
    String customName;
    if (promptForName) {
      customName = getUserInputName();
      if (customName.isEmpty()) {
        replayRestoreStatusPanel();
        return;
      }
    } else {
      char autoName[MAX_NAME_LENGTH];
      snprintf(autoName, sizeof(autoName), "Signal%lu",
               (unsigned long)(millis() % 100000UL));
      customName = autoName;
    }

    const bool savingRaw = replayRawPreferred();
    SubGhzProfile profile{};
    profile.frequency = subghz_frequency_list[currentFrequencyIndex];
    profile.value = receivedValue;
    profile.bitLength = receivedBitLength ? receivedBitLength : 24u;
    profile.protocol = receivedProtocol ? receivedProtocol : 1u;
    strncpy(profile.name, customName.c_str(), MAX_NAME_LENGTH - 1);
    profile.name[MAX_NAME_LENGTH - 1] = '\0';

    SubGhzRawProfile raw{};
    const SubGhzRawProfile* rawPtr = nullptr;
    if (savingRaw) {
      raw.magic = RAW_PROFILE_MAGIC;
      raw.frequency = profile.frequency;
      raw.count = latestRawFrame.count;
      raw.startLevel = latestRawFrame.startLevel;
      raw.repeats = 4u;
      memcpy(raw.timings, latestRawFrame.timings,
             raw.count * sizeof(raw.timings[0]));
      profile.protocol = RAW_PROFILE_PROTOCOL;
      profile.bitLength = raw.count;
      rawPtr = &raw;
    }

    // SD and CC1101 share SPI. Stop RMT/RX first, perform one guarded write,
    // then rebuild the complete CC1101 receive state.
    replayDisarmReceive();
    ELECHOUSE_cc1101.setSidle();
    String path;
    String err;
    const bool ok = writeCaptureToSD(profile, rawPtr, path, &err);
    replayRestoreRadioAfterSd();

    subghzClearBody(TFT_BLACK);
    tft.setTextSize(1);
    tft.setTextColor(ok ? TFT_GREEN : TFT_RED, TFT_BLACK);
    tft.setCursor(10, 30 + yshift);
    tft.print(ok ? "Saved to SD" : "SD save failed");
    tft.setTextColor(UI_TEXT, TFT_BLACK);
    tft.setCursor(10, 45 + yshift);
    const String detail = ok ? path : err;
    tft.print(detail.substring(0, 34));
    if (detail.length() > 34) {
      tft.setCursor(10, 60 + yshift);
      tft.print(detail.substring(34, 68));
    }

    uiDrawn = false;
    runUI();
    subghzRedrawNavChrome();
    if (!subghzWaitWithNav(ok ? 1200 : 1800)) {
      return;
    }
    replayRestoreStatusPanel();
    float currentBatteryVoltage = readBatteryVoltage();
    drawStatusBar(currentBatteryVoltage, true);
    uiDrawn = false;
    runUI();
    subghzRedrawNavChrome();
}

void loadProfileCount() {

    readProfileCount();
}

void runUI() {

    #define STATUS_BAR_Y_OFFSET 20
    #define STATUS_BAR_HEIGHT 20
    #define ICON_SIZE 16
    #define ICON_NUM 6

    static int iconX[ICON_NUM] = {90, 130, 170, 210, 50, 10};
    static int iconY = STATUS_BAR_Y_OFFSET;

    static const unsigned char* icons[ICON_NUM] = {
        bitmap_icon_sort_up_plus,
        bitmap_icon_sort_down_minus,
        bitmap_icon_antenna,
        bitmap_icon_floppy,
        bitmap_icon_random,
        bitmap_icon_go_back
    };

    if (!uiDrawn) {
        tft.fillRect(0, STATUS_BAR_Y_OFFSET, SCREEN_WIDTH, STATUS_BAR_HEIGHT, DARK_GRAY);

        for (int i = 0; i < ICON_NUM; i++) {
            if (icons[i] != NULL) {
                drawToolbarIcon(iconX[i], iconY, icons[i], UI_ICON);
            }
        }
        tft.drawFastHLine(0, 19, 240, UI_LINE);
        tft.drawFastHLine(0, STATUS_BAR_Y_OFFSET + STATUS_BAR_HEIGHT, 240, UI_LINE);
        uiDrawn = true;
    }

    static unsigned long lastAnimationTime = 0;
    static int animationState = 0;
    static int activeIcon = -1;

    if (animationState > 0 && millis() - lastAnimationTime >= TOOLBAR_ANIMATION_MS) {
        if (animationState == 1) {
            drawToolbarIcon(iconX[activeIcon], iconY, icons[activeIcon], UI_ICON);
            animationState = 2;

            switch (activeIcon) {
                case 0:
                    replayFreqNext();
                    break;
                case 1:
                    replayFreqPrev();
                    break;
                case 2:
                    autoScanEnabled = false;
                    replayClearScanLock();
                    sendSignal();
                    break;
                case 3:
                    // Use the guarded Save path: it stops RX/AUTO, transfers
                    // the shared SPI bus to SD, writes once, then restores the
                    // CC1101 session.  Never mount/probe SD inside the sweep.
                    replayTrySave(false);
                    break;
                case 4:
                    replayToggleAuto();
                    break;
            }
        } else if (animationState == 2) {
            animationState = 0;
            activeIcon = -1;
        }
        lastAnimationTime = millis();
    }

    static unsigned long lastTouchCheck = 0;
    const unsigned long touchCheckInterval = 50;

    if (millis() - lastTouchCheck >= touchCheckInterval) {
        int x, y;
        if (feature_active && readTouchXY(x, y)) {
            if (toolbarIconHitY(y, iconY)) {
                for (int i = 0; i < ICON_NUM; i++) {
                    if (toolbarIconHitX(x, iconX[i])) {
                        if (icons[i] != NULL && animationState == 0) {

                            if (i == 5) {
                                feature_exit_requested = true;
                            } else {

                                drawToolbarIcon(iconX[i], iconY, icons[i], TFT_BLACK);
                                animationState = 1;
                                activeIcon = i;
                                lastAnimationTime = millis();
                            }
                        }
                        break;
                    }
                }
            }
        }
        lastTouchCheck = millis();
    }
}

void ReplayAttackSetup() {
  subghzBeginSession(SubGhzOwner::Replay);
  // WS2812.show() masks interrupts long enough to damage short OOK pulses.
  // Capture reliability takes priority while Replay is listening.
  neopixelClear();
  setTouchButtonInputEnabled(true);
  subghzSetReplayNavLabels();

  replayDisarmReceive();

#if defined(SD_CS)
  pinMode(SD_CS, OUTPUT);
  digitalWrite(SD_CS, HIGH);
#endif
#if defined(CC1101_CS)
  pinMode(CC1101_CS, OUTPUT);
  digitalWrite(CC1101_CS, HIGH);
#endif

  ELECHOUSE_cc1101.setSpiPin(CC1101_SCK, CC1101_MISO, CC1101_MOSI, CC1101_CS);
  ELECHOUSE_cc1101.setGDO(CC1101_GDO0, CC1101_GDO2);

  receivedValue = 0;
  receivedBitLength = 0;
  receivedProtocol = 0;
  latestRawValid = false;
  latestRawFrame = ReplayRawFrame{};
  currentFrequencyIndex = 11;  // 433.92 MHz

  autoScanEnabled = false;
  replayClearScanLock();
  replayResetDecodeTracking();

  subghzClearBody(TFT_BLACK);
  tft.setRotation(TFT_ROTATION);

  drawStatusBar(readBatteryVoltage(), true);
  subghzRedrawNavChrome();
  setupTouchscreen();

#if HAS_PCF8574_BUTTONS
  pcf.pinMode(BTN_LEFT, INPUT_PULLUP);
  pcf.pinMode(BTN_RIGHT, INPUT_PULLUP);
  pcf.pinMode(BTN_UP, INPUT_PULLUP);
  pcf.pinMode(BTN_DOWN, INPUT_PULLUP);
  pcf.pinMode(BTN_SELECT, INPUT_PULLUP);
#endif

  sampling_period = round(1000000*(1.0/FrequencySUB));

  for (int i = 0; i < 32; i++) {
    red[i] = i / 2;
    green[i] = 0;
    blue[i] = i;
  }
  for (int i = 32; i < 64; i++) {
    red[i] = i / 2;
    green[i] = 0;
    blue[i] = 63 - i;
  }
#if ESP32DIV_FFT_PALETTE_SIZE > 64
  for (int i = 64; i < 96; i++) {
    red[i] = 31;
    green[i] = (i - 64) * 2;
    blue[i] = 0;
  }
  for (int i = 96; i < 128; i++) {
    red[i] = 31;
    green[i] = 63;
    blue[i] = i - 96;
  }
#endif

  replayInvalidateDisplay();
  updateDisplay();
  uiDrawn = false;
  subghzRedrawNavChrome();

  /* Bring radio up after UI/SPI activity so first entry RX matches re-entry. */
  replayInitOfficialDivRadio();

  pinMode(REPLAY_RX_PIN, INPUT);
  pinMode(REPLAY_TX_PIN, INPUT);

  tuneToIndex(currentFrequencyIndex, false);
  mySwitch.setReceiveTolerance(replayFreqIsLowBand(currentFrequencyIndex) ? 50 : 40);
  mySwitch.setRepeatTransmit(8);

  delay(50);
  replayArmReceive();
  s_replayCalibrating = false;
}

void ReplayAttackLoop() {

    if (feature_active && (feature_exit_requested || featureExitButtonPressed())) {
        replayDisarmReceive();
        replayPersistFrequencyNow();
        feature_exit_requested = true;
        return;
    }

    maintainTouchNavBar();
    runUI();
    if (uiDrawn) {
      tft.drawFastHLine(0, 19, 240, UI_LINE);
      tft.drawFastHLine(0, 40, 240, UI_LINE);
      if (s_replayDisp.valid) {
        replayDrawStatusSeparator();
      }
    }
    replayHandleNavButtons();

    static unsigned long lastDebounceTime = 0;
    const unsigned long debounceDelay = 200;

    static bool prevLeft = false, prevRight = false, prevUp = false, prevDown = false;
    const bool leftPressed  = isPhysicalButtonPressed(BTN_LEFT);
    const bool rightPressed = isPhysicalButtonPressed(BTN_RIGHT);
    const bool upPressed    = isPhysicalButtonPressed(BTN_UP);
    const bool downPressed  = isPhysicalButtonPressed(BTN_DOWN);

    replayBeepPoll();
    replayPersistFrequencyPoll();
    replayCalibrationPoll();

    if (notifActive && isNotificationVisible()) {
      int x, y;
      if (readTouchXY(x, y)) {
        NotificationAction act = notificationHandleTouch(x, y);
        if (act == NotificationAction::Save) {
          notifActive = false;

          subghzClearBody(TFT_BLACK);
          uiDrawn = false;
          replayInvalidateDisplay();
          float v = readBatteryVoltage();
          drawStatusBar(v, true);
          runUI();
          updateDisplay();
          subghzRedrawNavChrome();

          autoScanEnabled = false;
          replayTrySave(false);

          subghzClearBody(TFT_BLACK);
          uiDrawn = false;
          replayInvalidateDisplay();
          v = readBatteryVoltage();
          drawStatusBar(v, true);
          runUI();
          updateDisplay();
          subghzRedrawNavChrome();
        } else if (act == NotificationAction::Ok || act == NotificationAction::Close) {
          notifActive = false;

          lastDetectAlertMs = millis();
          lastDetectAlertFreq = currentFrequencyIndex;
          lockUntilMs = millis() + 1500;
          rssiHot = true;

          subghzClearBody(TFT_BLACK);
          uiDrawn = false;
          replayInvalidateDisplay();
          float v = readBatteryVoltage();
          drawStatusBar(v, true);
          runUI();
          updateDisplay();
          subghzRedrawNavChrome();
        }
      }

      return;
    } else if (notifActive && !isNotificationVisible()) {

      notifActive = false;
      subghzClearBody(TFT_BLACK);
      uiDrawn = false;
      replayInvalidateDisplay();
      float v = readBatteryVoltage();
      drawStatusBar(v, true);
      runUI();
      updateDisplay();
      subghzRedrawNavChrome();
    }

    if (rightPressed && !prevRight && millis() - lastDebounceTime > debounceDelay) {
        replayFreqNext();
        lastDebounceTime = millis();
    }
    if (leftPressed && !prevLeft && millis() - lastDebounceTime > debounceDelay) {
        replayFreqPrev();
        lastDebounceTime = millis();
    }
    if (upPressed && !prevUp && millis() - lastDebounceTime > debounceDelay) {
        autoScanEnabled = false;
        replayClearScanLock();
        sendSignal();
        lastDebounceTime = millis();
    }
    if (downPressed && !prevDown && millis() - lastDebounceTime > debounceDelay) {
        // Physical-only fallback: save immediately with Signal1..Signal5 so a
        // marginal touch panel cannot make captured frames impossible to keep.
        replayTrySave(false);
        lastDebounceTime = millis();
    }

    prevLeft = leftPressed;
    prevRight = rightPressed;
    prevUp = upPressed;
    prevDown = downPressed;

    if (autoScanEnabled) {
      const uint32_t now = millis();
      const bool scanLocked = (lockUntilMs != 0 && (int32_t)(now - lockUntilMs) < 0);

      // Consume a completed decoded/RAW frame before changing frequency.  A
      // pending RCSwitch frame belongs to the current channel; hopping first
      // would clear it in tuneToIndex() and make AUTO appear intermittent.
      const bool rxFramePending = mySwitch.rawAvailable() ||
                                  mySwitch.available();
      if (!scanLocked && !rxFramePending &&
          (now - lastHopMs) >= replayScanDwellMs()) {
        const uint16_t fromIdx = currentFrequencyIndex;
        scanIndex = (uint16_t)((scanIndex + 1) % freqCount());
        replayScanHopTo(scanIndex, fromIdx);
        lastHopMs = now;
        rssiHot = false;
      }

      replaySampleRssiForScan(now);

      if (lastUiScanUpdateMs == 0 || (now - lastUiScanUpdateMs) >= UI_SCAN_UPDATE_MS) {
        updateDisplay();
        lastUiScanUpdateMs = now;
      }
    }

    if (!autoScanEnabled) {
      do_sampling();
      const uint32_t now = millis();
      if (lastUiScanUpdateMs == 0 ||
          (uint32_t)(now - lastUiScanUpdateMs) >= UI_SCAN_UPDATE_MS) {
        updateDisplay();
        lastUiScanUpdateMs = now;
      }
    }
    delay(10);
    epochSUB++;

    if (epochSUB >= tft.width())
      epochSUB = 0;

    // RCSwitch owns the only RX ISR. Besides its normal protocol result it now
    // publishes a bounded edge frame, allowing unsupported remotes to reach
    // R-SAVE without reintroducing the unstable parallel RMT receiver.
    ReplayRawFrame rawFrame;
    if (replayTakeRawFrame(rawFrame)) {
      const uint32_t rawNow = millis();
      const bool frequencyStable = !autoScanEnabled ||
                                   replayAutoScanReadyForDecode(rawNow);
      lastUndecodedPulseMs = rawNow;
      if (frequencyStable) {
        const int frameRssi = ELECHOUSE_cc1101.getRssi();
        const bool rawAccepted = replayConsiderRawFrame(
            rawFrame, rawNow, frameRssi);
        // A short but plausible first frame needs time for its repeat. Keep
        // AUTO on this frequency for a bounded interval instead of hopping
        // away at the end of the current dwell.
        if (autoScanEnabled && !rawAccepted && pendingRawRepeats > 0u) {
          lockUntilMs = rawNow +
              std::min<uint32_t>(REPLAY_RAW_CONFIRM_WINDOW_MS, 1700u);
          scanIndex = currentFrequencyIndex;
        }
        if (rawAccepted) {
          updateDisplay();
          if (autoScanEnabled) {
            lockUntilMs = rawNow + LOCK_HOLD_MS;
            scanIndex = currentFrequencyIndex;
            rssiHot = false;
            rssiDetectStreak = 0;
            replayShowDetectNotice("RAW", frameRssi);
          }
        }
      }
    }

    // Original ESP32-DIV decoded receive path remains authoritative whenever
    // the waveform matches one of RCSwitch's known protocols.
    if (mySwitch.available()) {
      const uint32_t val = mySwitch.getReceivedValue();
      const uint16_t bits = mySwitch.getReceivedBitlength();
      const uint16_t proto = mySwitch.getReceivedProtocol();
      mySwitch.resetAvailable();

      const uint32_t now = millis();
      const bool validDecode = replayLooksLikeRealDecode(val, bits, proto) &&
          (!autoScanEnabled || replayAutoScanReadyForDecode(now));
      if (validDecode) {
        receivedValue = val;
        receivedBitLength = bits;
        receivedProtocol = proto;
        lastDecodedCaptureMs = now;
        latestRawValid = false;

        Serial.printf("[subghz] DIV decode value=%lu bits=%u proto=%u rssi=%d\n",
                      (unsigned long)val, (unsigned)bits, (unsigned)proto,
                      ELECHOUSE_cc1101.getRssi());
        updateDisplay();

        if (autoScanEnabled) {
          lockUntilMs = now + LOCK_HOLD_MS;
          scanIndex = currentFrequencyIndex;
          rssiHot = false;
          rssiDetectStreak = 0;
          replayShowDetectNotice("DECODE", ELECHOUSE_cc1101.getRssi());
        }
      }
    }

  }

  void lifecycleShutdown() {
    replayPersistFrequencyNow();
    replayDisarmReceive();
    // Defensive cleanup for an interrupted RMT setup.  The normal path above
    // owns the channel flags; these assignments prevent a stale ring handle
    // from being consumed after another feature takes the peripheral.
    s_replayRmtRing = nullptr;
    s_replayRmtArmed = false;
    s_replayRxArmed = false;
    replayResetPulseCapture();
    mySwitch.disableTransmit();
    pinMode(REPLAY_RX_PIN, INPUT);
    pinMode(REPLAY_TX_PIN, INPUT);
  }
}

namespace SavedProfile {

void updateDisplay();
void runUI();
void transmitProfile(int index);
void deleteProfile(int index);
void exportProfileToSd(int index);
void importProfileFromSd();
static void profileRestoreRadioAfterSd();

static bool uiDrawn = false;

#define EEPROM_SIZE SUBGHZ_EEPROM_SIZE

#define ADDR_PROFILE_COUNT 1296
#define ADDR_PROFILE_START 1300
#define MAX_PROFILES       5
#define MAX_NAME_LENGTH    16

#define SCREEN_WIDTH 240
#define SCREEN_HEIGHT 320

RCSwitch mySwitch = RCSwitch();
struct __attribute__((packed)) Profile {
    uint32_t frequency;
    uint32_t value;
    uint16_t bitLength;
    uint16_t protocol;
    char name[MAX_NAME_LENGTH];
};

#define PROFILE_SIZE sizeof(Profile)

uint16_t profileCount = 0;
uint16_t currentProfileIndex = 0;

int yshift = 16;

static std::vector<String> sdFiles;
static std::vector<SubGhzParsedCapture> sdCaptures;
static std::vector<uint16_t> sdLocalIndices;
static std::vector<bool> sdBinarySources;
static uint16_t sdTotalProfiles = 0;
static String sdLastErr = "";
static String selectedPath = "";
static uint16_t selectedLocalIdx = 0;
static Profile selectedProfile{};
static SubGhzParsedCapture selectedCapture{};
static bool selectedValid = false;

static constexpr uint8_t ITEMS_PER_PAGE = 7;
static constexpr int PROFILE_PAD_X = 10;
static constexpr int LIST_X = PROFILE_PAD_X;
static constexpr int LIST_W = 220;

static constexpr int PROFILE_HEADER_Y = 50;
static constexpr int PROFILE_HEADER_H = 14;
static constexpr int LIST_Y = PROFILE_HEADER_Y + PROFILE_HEADER_H + 2;
static constexpr int ROW_H  = 18;
static constexpr int PROFILE_LINE_H = 14;
static constexpr int PROFILE_LINE_GAP = 3;
static constexpr int PROFILE_LINE_STEP = PROFILE_LINE_H + PROFILE_LINE_GAP;
static constexpr int PROFILE_INFO_LINES = 4;
static constexpr int PROFILE_INFO_CONTENT_H =
    PROFILE_LINE_STEP * (PROFILE_INFO_LINES - 1) + PROFILE_LINE_H;
static constexpr int PROFILE_LABEL_X = PROFILE_PAD_X;
static constexpr int PROFILE_VALUE_X = 50;
static constexpr int PROFILE_COL2_LABEL_X = 130;
static constexpr int PROFILE_COL2_VALUE_X = 165;

static constexpr int UI_GAP_Y = 6;

static int profileBottomY() {
  return subghzContentBottom();
}

static int profileListBottom() {
  return LIST_Y + (ITEMS_PER_PAGE * ROW_H);
}

static int profileDetailsY() {
  const int areaTop = profileListBottom();
  const int areaBottom = profileBottomY();
  const int areaH = areaBottom - areaTop;
  if (areaH <= PROFILE_INFO_CONTENT_H) {
    return areaTop + UI_GAP_Y;
  }
  return areaTop + (areaH - PROFILE_INFO_CONTENT_H) / 2;
}

static void profileClearContentArea(uint16_t color = TFT_BLACK) {
  const int top = 40;
  const int h = subghzContentBottom() - top;
  if (h > 0) {
    tft.fillRect(0, top, 240, h, color);
  }
}

static void profileRestoreChrome() {
  drawStatusBar(readBatteryVoltage(), true);
  uiDrawn = false;
  updateDisplay();
  runUI();
  subghzRedrawNavChrome();
}

static void updateSelectionUI(uint16_t oldIndex, bool forceListRedraw = false);

static uint16_t cachedPageStart = 0xFFFF;
static SubGhzProfile cachedPage[ITEMS_PER_PAGE]{};
static bool cachedOk[ITEMS_PER_PAGE]{};
static bool cacheDirty = true;

static bool deleteArmed = false;
static uint32_t deleteArmUntilMs = 0;

static bool sameCatalogProfile(const SubGhzParsedCapture& a,
                               const SubGhzParsedCapture& b) {
  return a.profile.frequency == b.profile.frequency &&
         a.profile.value == b.profile.value &&
         a.profile.bitLength == b.profile.bitLength &&
         a.profile.protocol == b.profile.protocol &&
         strncmp(a.profile.name, b.profile.name, MAX_NAME_LENGTH) == 0;
}

static void appendCatalogEntry(const String& path,
                               const SubGhzParsedCapture& parsed,
                               uint16_t localIndex, bool binarySource) {
  for (const auto& existing : sdCaptures) {
    if (sameCatalogProfile(existing, parsed)) return;
  }
  sdFiles.push_back(path);
  sdCaptures.push_back(parsed);
  sdLocalIndices.push_back(localIndex);
  sdBinarySources.push_back(binarySource);
}

static void refreshSdIndex(bool keepSelection = true) {
    uint16_t oldIdx = currentProfileIndex;
    sdFiles.clear();
    sdCaptures.clear();
    sdLocalIndices.clear();
    sdBinarySources.clear();
    sdLastErr = "";
    const uint32_t scanStarted = millis();
    String err;
    SdFileIoGuard io(2400);
    if (!io.locked()) {
      sdLastErr = "SD bus busy";
    } else if (!subghzMountSD()) {
      sdLastErr = "SD not mounted";
    } else {
      std::vector<String> paths;
      subghzListCaptureFilesMounted(paths, &err);
      for (const String& path : paths) {
        SubGhzParsedCapture parsed{};
        String parseErr;
        const bool parsedOk =
            subghzParseCaptureFileMounted(path, parsed, &parseErr);
        if (parsedOk) {
          appendCatalogEntry(path, parsed, 0u, false);
        } else {
          Serial.printf("[subghz] skip invalid capture %s: %s\n",
                        path.c_str(), parseErr.c_str());
        }
      }

      if (SD.exists(SUBGHZ_DIR)) {
        File dir = SD.open(SUBGHZ_DIR);
        if (dir && dir.isDirectory()) {
          for (;;) {
            File f = dir.openNextFile();
            if (!f) break;
            if (f.isDirectory()) { f.close(); continue; }
            String name = String(f.name());
            if (name.startsWith("/")) {
              const int slash = name.lastIndexOf('/');
              if (slash >= 0) name = name.substring(slash + 1);
            }
            const bool profileBin = name.startsWith("profiles_") &&
                                    name.endsWith(".bin");
            if (!profileBin) { f.close(); continue; }
            SubGhzExportHeader header{};
            String headerErr;
            if (!readExportHeader(f, header, &headerErr)) {
              Serial.printf("[subghz] skip legacy %s: %s\n",
                            name.c_str(), headerErr.c_str());
              f.close();
              continue;
            }
            uint16_t count = std::min<uint16_t>(header.count, MAX_PROFILES);
            const String fullPath = String(SUBGHZ_DIR) + "/" + name;
            for (uint16_t i = 0; i < count; ++i) {
              SubGhzProfile profile{};
              if (f.read((uint8_t*)&profile, sizeof(profile)) != sizeof(profile)) {
                Serial.printf("[subghz] short legacy read %s slot=%u\n",
                              name.c_str(), (unsigned)i);
                break;
              }
              profile.name[MAX_NAME_LENGTH - 1] = '\0';
              if (profile.protocol == RAW_PROFILE_PROTOCOL) continue;
              SubGhzParsedCapture parsed{};
              parsed.profile = profile;
              appendCatalogEntry(fullPath, parsed, i, true);
            }
            f.close();
          }
          dir.close();
        }
      }
    }
    sdTotalProfiles = (uint16_t)sdFiles.size();
    profileCount = sdTotalProfiles;
    if (sdTotalProfiles == 0) {
        currentProfileIndex = 0;
        selectedValid = false;
        if (sdLastErr.isEmpty()) sdLastErr = "No captures on SD";
        cacheDirty = true;
        return;
    }
    if (keepSelection) currentProfileIndex = oldIdx;
    if (currentProfileIndex >= sdTotalProfiles) currentProfileIndex = (uint16_t)(sdTotalProfiles - 1);
    selectedValid = false;
    cacheDirty = true;
    Serial.printf("[subghz] Saved Profiles catalog=%u load=%lums\n",
                  (unsigned)sdTotalProfiles,
                  (unsigned long)(millis() - scanStarted));
}

static bool loadSelectedFromSd(String* errOut = nullptr) {
    if (sdTotalProfiles == 0) { selectedValid = false; return false; }
    if (currentProfileIndex >= sdTotalProfiles) {
      selectedValid = false; if (errOut) *errOut="Index OOR"; return false;
    }
    const SubGhzParsedCapture& parsed = sdCaptures[currentProfileIndex];
    const SubGhzProfile& p = parsed.profile;
    selectedPath = sdFiles[currentProfileIndex];
    selectedLocalIdx = sdLocalIndices[currentProfileIndex];
    selectedCapture = parsed;

    selectedProfile.frequency = p.frequency;
    selectedProfile.value = p.value;
    selectedProfile.bitLength = p.bitLength;
    selectedProfile.protocol = p.protocol;
    memcpy(selectedProfile.name, p.name, MAX_NAME_LENGTH);
    selectedProfile.name[MAX_NAME_LENGTH - 1] = '\0';
    selectedValid = true;
    return true;
}

static uint16_t pageStartForIndex(uint16_t idx) {
  return (uint16_t)((idx / ITEMS_PER_PAGE) * ITEMS_PER_PAGE);
}

static void ensurePageCache() {
  if (sdTotalProfiles == 0) return;
  uint16_t start = pageStartForIndex(currentProfileIndex);
  if (!cacheDirty && cachedPageStart == start) return;
  cachedPageStart = start;
  for (uint8_t i = 0; i < ITEMS_PER_PAGE; i++) {
    cachedOk[i] = false;
    uint16_t globalIdx = (uint16_t)(start + i);
    if (globalIdx >= sdTotalProfiles) continue;
    cachedOk[i] = globalIdx < sdCaptures.size();
    if (cachedOk[i]) cachedPage[i] = sdCaptures[globalIdx].profile;
    if (!cachedOk[i]) memset(&cachedPage[i], 0, sizeof(SubGhzProfile));
  }
  cacheDirty = false;
}

static void drawHeaderLine() {
  const int hy = PROFILE_HEADER_Y;
  tft.fillRect(LIST_X, hy, LIST_W, PROFILE_HEADER_H, TFT_BLACK);
  tft.setTextColor(UI_WARN, TFT_BLACK);
  tft.setCursor(LIST_X, hy);
  tft.printf("Profile %d/%d", (int)currentProfileIndex + 1, (int)sdTotalProfiles);
}

static void profileSelectNext() {
  if (sdTotalProfiles == 0) {
    return;
  }
  uint16_t oldIdx = currentProfileIndex;
  currentProfileIndex = (uint16_t)((currentProfileIndex + 1) % sdTotalProfiles);
  selectedValid = false;
  updateSelectionUI(oldIdx, false);
}

static void profileSelectPrev() {
  if (sdTotalProfiles == 0) {
    return;
  }
  uint16_t oldIdx = currentProfileIndex;
  currentProfileIndex = (uint16_t)((currentProfileIndex + sdTotalProfiles - 1) % sdTotalProfiles);
  selectedValid = false;
  updateSelectionUI(oldIdx, false);
}

static void profileRefreshSd() {
  refreshSdIndex(true);
  selectedValid = false;
  cacheDirty = true;
  deleteArmed = false;
  updateDisplay();
}

void profileHandleNavButtons() {
  if (!featureHasTouchNavBar()) {
    return;
  }

  if (isTouchNavButtonPressedEdge(BTN_SELECT)) {
    feature_exit_requested = true;
    return;
  }
  if (isTouchNavButtonPressedEdge(BTN_UP)) {
    profileSelectPrev();
    subghzWaitNavRelease(BTN_UP);
  }
  if (isTouchNavButtonPressedEdge(BTN_DOWN)) {
    profileSelectNext();
    subghzWaitNavRelease(BTN_DOWN);
  }
  if (isTouchNavButtonPressedEdge(BTN_RIGHT)) {
    if (sdTotalProfiles > 0) {
      transmitProfile(currentProfileIndex);
    }
    subghzWaitNavRelease(BTN_RIGHT);
  }
  if (isTouchNavButtonPressedEdge(BTN_LEFT)) {
    if (sdTotalProfiles > 0) {
      exportProfileToSd(currentProfileIndex);
    }
    subghzWaitNavRelease(BTN_LEFT);
  }
}

static void drawRow(uint16_t pageStart, uint8_t row) {
  uint16_t globalIdx = (uint16_t)(pageStart + row);
  if (globalIdx >= sdTotalProfiles) return;

  bool isSel = (globalIdx == currentProfileIndex);
  int y = LIST_Y + (row * ROW_H);

  uint16_t bg = isSel ? DARK_GRAY : TFT_BLACK;
  uint16_t fg = isSel ? UI_WARN : UI_DIM_TEXT;
  tft.fillRect(LIST_X, y, LIST_W, ROW_H - 1, bg);
  tft.setTextColor(fg, bg);
  tft.setCursor(LIST_X + 2, y + 4);
  tft.printf("%2d.", (int)globalIdx + 1);
  tft.setCursor(LIST_X + 34, y + 4);

  if (cachedOk[row]) {

    char nameBuf[17];
    memcpy(nameBuf, cachedPage[row].name, 16);
    nameBuf[16] = '\0';
    String nm = String(nameBuf);
    if (nm.length() > 10) nm = nm.substring(0, 10);
    tft.print(nm);

    char fbuf[16];
    snprintf(fbuf, sizeof(fbuf), "%.2f", cachedPage[row].frequency / 1000000.0);
    int tw = tft.textWidth(fbuf, 1);
    tft.setCursor(LIST_X + LIST_W - 4 - tw, y + 4);
    tft.print(fbuf);
  } else {
    tft.print("<?>");
  }
}

static void drawListPage(uint16_t pageStart) {
  ensurePageCache();

  tft.fillRect(LIST_X, LIST_Y, LIST_W, (ITEMS_PER_PAGE * ROW_H), TFT_BLACK);
  for (uint8_t row = 0; row < ITEMS_PER_PAGE; row++) {
    if ((uint16_t)(pageStart + row) >= sdTotalProfiles) break;
    drawRow(pageStart, row);
  }
}

static void drawDetails() {
  const int detailsY = profileDetailsY();
  const int gapTop = profileListBottom();
  const int gapH = profileBottomY() - gapTop;
  if (gapH > 0) {
    tft.fillRect(LIST_X, gapTop, LIST_W, gapH, TFT_BLACK);
  }
  tft.drawFastHLine(LIST_X, profileListBottom(), LIST_W, UI_LINE);

  String err;
  if (!selectedValid) {
    loadSelectedFromSd(&err);
  }

  tft.setTextSize(1);
  tft.setTextColor(UI_TEXT, TFT_BLACK);
  if (!selectedValid) {
    tft.setCursor(PROFILE_LABEL_X, detailsY);
    tft.print("Read failed:");
    tft.setCursor(PROFILE_VALUE_X, detailsY);
    tft.print(err);
    return;
  }

  tft.setCursor(PROFILE_LABEL_X, detailsY);
  tft.print("Name:");
  tft.setCursor(PROFILE_VALUE_X, detailsY);
  tft.print(selectedProfile.name);

  tft.setCursor(PROFILE_LABEL_X, detailsY + PROFILE_LINE_STEP);
  tft.print("Freq:");
  tft.setCursor(PROFILE_VALUE_X, detailsY + PROFILE_LINE_STEP);
  tft.printf("%.2f MHz", selectedProfile.frequency / 1000000.0);
  tft.setCursor(PROFILE_COL2_LABEL_X, detailsY + PROFILE_LINE_STEP);
  tft.print("Ptc:");
  tft.setCursor(PROFILE_COL2_VALUE_X, detailsY + PROFILE_LINE_STEP);
  if (selectedProfile.protocol == RAW_PROFILE_PROTOCOL) tft.print("RAW");
  else tft.print(selectedProfile.protocol);

  tft.setCursor(PROFILE_LABEL_X, detailsY + (PROFILE_LINE_STEP * 2));
  tft.print("Val:");
  tft.setCursor(PROFILE_VALUE_X, detailsY + (PROFILE_LINE_STEP * 2));
  if (selectedProfile.protocol == RAW_PROFILE_PROTOCOL) tft.print("raw timing");
  else tft.print((unsigned long)selectedProfile.value);
  tft.setCursor(PROFILE_COL2_LABEL_X, detailsY + (PROFILE_LINE_STEP * 2));
  tft.print("Bit:");
  tft.setCursor(PROFILE_COL2_VALUE_X, detailsY + (PROFILE_LINE_STEP * 2));
  tft.print(selectedProfile.bitLength);
  if (selectedProfile.protocol == RAW_PROFILE_PROTOCOL) tft.print("p");

  tft.setTextColor(UI_DIM_TEXT, TFT_BLACK);
  tft.setCursor(PROFILE_LABEL_X, detailsY + (PROFILE_LINE_STEP * 3));
  tft.print("SRC:");
  tft.setCursor(PROFILE_VALUE_X, detailsY + (PROFILE_LINE_STEP * 3));
  tft.print(sdBinarySources[currentProfileIndex] ? "SD BIN" : "SD");

  if (deleteArmed && (int32_t)(millis() - deleteArmUntilMs) < 0) {
    int hintY = detailsY + (PROFILE_LINE_STEP * 4);
    if (hintY >= profileBottomY() - 12) {
      hintY = profileBottomY() - 12;
    }
    tft.setCursor(PROFILE_LABEL_X, hintY);
    tft.setTextColor(UI_WARN, TFT_BLACK);
    tft.print("Press Delete again to confirm");
  }
}

static void updateSelectionUI(uint16_t oldIndex, bool forceListRedraw) {
  if (sdTotalProfiles == 0) return;
  uint16_t oldPage = pageStartForIndex(oldIndex);
  uint16_t newPage = pageStartForIndex(currentProfileIndex);

  tft.startWrite();
  drawHeaderLine();

  if (forceListRedraw || oldPage != newPage) {
    drawListPage(newPage);
  } else {

    uint8_t oldRow = (uint8_t)(oldIndex - oldPage);
    uint8_t newRow = (uint8_t)(currentProfileIndex - newPage);
    ensurePageCache();

    drawRow(newPage, oldRow);
    drawRow(newPage, newRow);
  }

  drawDetails();
  tft.endWrite();
}

void updateDisplay() {

    tft.startWrite();
    const int bodyH = subghzContentBottom() - 40;
    if (bodyH > 0) {
      tft.fillRect(0, 40, 240, bodyH, TFT_BLACK);
    }

    if (sdTotalProfiles == 0) {
        tft.setTextSize(1);
        tft.setCursor(PROFILE_LABEL_X, PROFILE_HEADER_Y + PROFILE_LINE_H);
        tft.setTextColor(UI_TEXT, TFT_BLACK);
        tft.print("No saved profiles.");
        if (sdLastErr.length()) {
          tft.setCursor(PROFILE_LABEL_X, PROFILE_HEADER_Y + (PROFILE_LINE_H * 2));
          tft.setTextColor(UI_DIM_TEXT, TFT_BLACK);
          tft.print(sdLastErr);
        }
        tft.endWrite();
        return;
    }

    drawHeaderLine();
    drawListPage(pageStartForIndex(currentProfileIndex));
    drawDetails();
    tft.endWrite();
}

void transmitProfile(int index) {
    (void)index;
    String err;
    loadSelectedFromSd(&err);
    if (!selectedValid) return;
    Profile profileToSend = selectedProfile;
    const bool isRaw = selectedCapture.hasRaw ||
                       profileToSend.protocol == RAW_PROFILE_PROTOCOL;
    const bool invalidRaw = isRaw && (!selectedCapture.hasRaw ||
        selectedCapture.raw.count < 8u ||
        selectedCapture.raw.count > RAW_MAX_CHANGES);
    const bool invalidDecoded = !isRaw &&
        (profileToSend.value == 0u || profileToSend.bitLength == 0u ||
         profileToSend.protocol == 0u);
    if (invalidRaw || invalidDecoded) {
      profileClearContentArea(TFT_BLACK);
      tft.setTextColor(TFT_RED, TFT_BLACK);
      tft.setCursor(10, 30 + yshift);
      tft.print("Invalid capture");
      tft.setTextColor(UI_WARN, TFT_BLACK);
      tft.setCursor(10, 50 + yshift);
      tft.print("Capture/import it again");
      subghzWaitWithNav(1400);
      profileRestoreChrome();
      return;
    }

    mySwitch.disableReceive();
    profileClearContentArea(TFT_BLACK);
    tft.setCursor(10, 30 + yshift);
    tft.setTextColor(TFT_WHITE);
    tft.print(isRaw ? "Sending RAW " : "Sending ");
    tft.print(profileToSend.name);
    tft.print("...");
    tft.setCursor(10, 50 + yshift);
    if (isRaw) {
      tft.printf("Pulses: %u", (unsigned)selectedCapture.raw.count);
    } else {
      tft.print("Value: ");
      tft.print(profileToSend.value);
    }

    const uint32_t txStartedMs = millis();
    bool txOk = false;
    if (isRaw) {
      delay(20);
      SubGhzTxDiagnostics diag;
      const uint8_t requestedRepeats = selectedCapture.raw.repeats == 0u
          ? 4u : selectedCapture.raw.repeats;
      const uint8_t repeats = subghzSafeRawRepeats(
          selectedCapture.raw.timings, selectedCapture.raw.count,
          requestedRepeats);
      txOk = subghzPrepareOokTx(profileToSend.frequency, &diag) &&
          subghzTransmitRawRmt(
              SUBGHZ_TX_PIN, selectedCapture.raw.startLevel,
              selectedCapture.raw.timings, selectedCapture.raw.count,
              repeats, 10000u);
      Serial.printf("[subghz] RAW profile TX %s in %lums name=%s freq=%lu pulses=%u repeats=%u\n",
                    txOk ? "OK" : "FAIL",
                    (unsigned long)(millis() - txStartedMs),
                    profileToSend.name,
                    (unsigned long)profileToSend.frequency,
                    (unsigned)selectedCapture.raw.count,
                    (unsigned)repeats);
    } else {
      delay(100);
      ELECHOUSE_cc1101.setSidle();
      ELECHOUSE_cc1101.setMHZ(profileToSend.frequency / 1000000.0f);
      pinMode(SUBGHZ_TX_PIN, OUTPUT);
      mySwitch.enableTransmit(SUBGHZ_TX_PIN);
      ELECHOUSE_cc1101.SetTx();
      mySwitch.setProtocol(profileToSend.protocol);
      mySwitch.setRepeatTransmit(8);
      mySwitch.send(profileToSend.value, profileToSend.bitLength);
      yield();
      txOk = true;
      Serial.printf("[subghz] official DIV profile TX in %lums name=%s freq=%lu bits=%u proto=%u\n",
                    (unsigned long)(millis() - txStartedMs), profileToSend.name,
                    (unsigned long)profileToSend.frequency,
                    profileToSend.bitLength, profileToSend.protocol);
    }

    subghzWaitWithNav(350);
    profileClearContentArea(TFT_BLACK);
    tft.setCursor(10, 30 + yshift);
    tft.setTextColor(txOk ? TFT_GREEN : TFT_RED, TFT_BLACK);
    tft.print(txOk ? "Done!" : "TX failed");

    mySwitch.disableTransmit();
    pinMode(SUBGHZ_TX_PIN, INPUT);
    pinMode(SUBGHZ_RX_PIN, INPUT);
    profileRestoreRadioAfterSd();

    subghzWaitWithNav(350);
    profileRestoreChrome();
}

static void profileRestoreRadioAfterSd() {
    mySwitch.disableReceive();
    mySwitch.disableTransmit();
    reclaimSharedSpiBus();
#if defined(SD_CS)
    pinMode(SD_CS, OUTPUT);
    digitalWrite(SD_CS, HIGH);
#endif
#if defined(CC1101_CS)
    pinMode(CC1101_CS, OUTPUT);
    digitalWrite(CC1101_CS, HIGH);
#endif
    ELECHOUSE_cc1101.setSpiPin(CC1101_SCK, CC1101_MISO, CC1101_MOSI, CC1101_CS);
    ELECHOUSE_cc1101.setGDO(CC1101_GDO0, CC1101_GDO2);
    subghzInitCc1101Reliable();
    replayat::replayConfigureCc1101Rx();
    if (selectedValid && selectedProfile.frequency >= 300000000UL &&
        selectedProfile.frequency <= 928000000UL) {
      ELECHOUSE_cc1101.setMHZ(selectedProfile.frequency / 1000000.0);
    }
    pinMode(SUBGHZ_RX_PIN, INPUT);
    pinMode(SUBGHZ_TX_PIN, INPUT);
    ELECHOUSE_cc1101.SetRx();
    delay(40);
    mySwitch.enableReceive(SUBGHZ_RX_PIN);
    Serial.println(F("[subghz] Saved Profiles radio restored after SD export"));
}

static bool importCaptureToEeprom(const String& path, String* errOut) {
    SubGhzParsedCapture capture{};
    if (!subghzParseCaptureFile(path, capture, errOut)) return false;

    uint16_t count = 0;
    EEPROM.get(ADDR_PROFILE_COUNT, count);
    if (count > MAX_PROFILES) count = 0;
    if (count >= MAX_PROFILES) {
      if (errOut) *errOut = "EEPROM full: delete one";
      return false;
    }

    if (capture.hasRaw) {
      capture.profile.value = count + 1u;
      capture.profile.bitLength = capture.raw.count;
      capture.profile.protocol = RAW_PROFILE_PROTOCOL;
      capture.raw.magic = RAW_PROFILE_MAGIC;
      capture.raw.frequency = capture.profile.frequency;
      EEPROM.put(subghzRawProfileAddress(count), capture.raw);
    } else {
      subghzClearRawSlot(count);
    }
    EEPROM.put(ADDR_PROFILE_START + (count * PROFILE_SIZE), capture.profile);
    const uint16_t newCount = count + 1u;
    EEPROM.put(ADDR_PROFILE_COUNT, newCount);
    if (!EEPROM.commit()) {
      if (errOut) *errOut = "EEPROM commit failed";
      return false;
    }

    SubGhzProfile verify{};
    uint16_t verifyCount = 0;
    EEPROM.get(ADDR_PROFILE_COUNT, verifyCount);
    EEPROM.get(ADDR_PROFILE_START + (count * PROFILE_SIZE), verify);
    if (verifyCount != newCount ||
        verify.frequency != capture.profile.frequency ||
        verify.protocol != capture.profile.protocol) {
      if (errOut) *errOut = "EEPROM verify failed";
      return false;
    }
    Serial.printf("[subghz] imported path=%s slot=%u type=%s\n",
                  path.c_str(), (unsigned)count,
                  capture.hasRaw ? "raw" : "decoded");
    return true;
}

static String importDisplayName(const String& path) {
    String name = path;
    const int slash = name.lastIndexOf('/');
    if (slash >= 0) name = name.substring(slash + 1);
    if (name.length() > 28) name = name.substring(0, 25) + "...";
    return name;
}

static void drawImportBrowser(const std::vector<String>& files,
                              uint16_t selected) {
    profileClearContentArea(TFT_BLACK);
    tft.setTextSize(1);
    tft.setTextColor(UI_WARN, TFT_BLACK);
    tft.setCursor(10, 44);
    tft.printf("Import SD capture %u/%u", (unsigned)selected + 1u,
               (unsigned)files.size());
    const uint16_t page = (uint16_t)((selected / 8u) * 8u);
    for (uint8_t row = 0; row < 8u; ++row) {
      const uint16_t idx = page + row;
      if (idx >= files.size()) break;
      const int y = 65 + row * 20;
      const bool active = idx == selected;
      tft.fillRect(8, y - 2, 224, 18, active ? DARK_GRAY : TFT_BLACK);
      tft.setTextColor(active ? UI_WARN : UI_TEXT,
                       active ? DARK_GRAY : TFT_BLACK);
      tft.setCursor(12, y + 2);
      tft.print(importDisplayName(files[idx]));
    }
    tft.setTextColor(UI_DIM_TEXT, TFT_BLACK);
    tft.setCursor(10, min(profileBottomY() - 14, 238));
    tft.print("RIGHT: import   LEFT/SELECT: cancel");
}

void importProfileFromSd() {
    mySwitch.disableReceive();
    mySwitch.disableTransmit();
    ELECHOUSE_cc1101.setSidle();
    refreshSdIndex(true);
    profileRestoreRadioAfterSd();
    subghzSetProfileNavLabels();
    redrawTouchButtonBar();
    selectedValid = false;
    cacheDirty = true;
    profileClearContentArea(TFT_BLACK);
    tft.setTextColor(sdTotalProfiles > 0 ? TFT_GREEN : TFT_RED, TFT_BLACK);
    tft.setCursor(10, 50);
    tft.print(sdTotalProfiles > 0 ? "SD captures refreshed" : "No SD captures");
    if (!sdLastErr.isEmpty()) {
      tft.setCursor(10, 68);
      tft.print(sdLastErr);
    }
    subghzWaitWithNav(1200);
    profileRestoreChrome();
}

void exportProfileToSd(int index) {
    (void)index;
    if (sdTotalProfiles == 0) return;
    profileClearContentArea(TFT_BLACK);
    tft.setTextColor(TFT_GREEN, TFT_BLACK);
    tft.setCursor(10, 30 + yshift);
    tft.print("Already stored on SD");
    tft.setTextColor(TFT_WHITE, TFT_BLACK);
    const String detail = sdFiles[currentProfileIndex];
    tft.setCursor(10, 48 + yshift);
    if (detail.length() <= 34) {
      tft.print(detail);
    } else {
      tft.print(detail.substring(0, 34));
      tft.setCursor(10, 62 + yshift);
      tft.print(detail.substring(34, 68));
    }
    subghzWaitWithNav(1400);
    profileRestoreChrome();
}

void loadProfileCount() {

    refreshSdIndex(true);
}

void printProfiles() {
    refreshSdIndex(false);
}

void deleteProfile(int index) {
    (void)index;
    if (sdTotalProfiles == 0) return;
    String err;
    loadSelectedFromSd(&err);
    if (!selectedValid) return;

    uint32_t now = millis();
    if (!deleteArmed || (int32_t)(now - deleteArmUntilMs) >= 0) {
      deleteArmed = true;
      deleteArmUntilMs = now + 3000;
      updateDisplay();
      return;
    }
    deleteArmed = false;

    mySwitch.disableReceive();
    mySwitch.disableTransmit();
    ELECHOUSE_cc1101.setSidle();
    bool removed = false;
    if (sdBinarySources[currentProfileIndex]) {
      removed = deleteProfileFromFile(selectedPath, selectedLocalIdx, &err);
    } else {
      SdFileIoGuard io(1500);
      if (io.locked() && subghzMountSD()) {
        removed = SD.remove(selectedPath.c_str());
      }
    }
    if (!removed) {
      profileRestoreRadioAfterSd();
      profileClearContentArea(TFT_BLACK);
      tft.setCursor(10, 30 + yshift);
      tft.setTextColor(UI_WARN);
      tft.print("Delete FAILED");
      tft.setCursor(10, 45 + yshift);
      tft.print("SD remove failed");
      delay(1200);
      profileRestoreChrome();
      return;
    }

    refreshSdIndex(false);
    profileRestoreRadioAfterSd();
    if (sdTotalProfiles == 0) currentProfileIndex = 0;
    else if (currentProfileIndex >= sdTotalProfiles) currentProfileIndex = (uint16_t)(sdTotalProfiles - 1);
    selectedValid = false;
    cacheDirty = true;
    updateDisplay();
}

void runUI() {
    #define STATUS_BAR_Y_OFFSET 20
    #define STATUS_BAR_HEIGHT 20
    #define ICON_SIZE 16
    #define ICON_NUM 5

    static int iconX[ICON_NUM] = {90, 120, 150, 180, 10};
    static int iconY = STATUS_BAR_Y_OFFSET;

    static const unsigned char* icons[ICON_NUM] = {
        bitmap_icon_save,
        bitmap_icon_antenna,
        bitmap_icon_recycle,
        bitmap_icon_sdcard,
        bitmap_icon_go_back
    };

    if (!uiDrawn) {
        tft.fillRect(0, STATUS_BAR_Y_OFFSET, SCREEN_WIDTH, STATUS_BAR_HEIGHT, DARK_GRAY);

        for (int i = 0; i < ICON_NUM; i++) {
            if (icons[i] != NULL) {
                drawToolbarIcon(iconX[i], iconY, icons[i], UI_ICON);
            }
        }
        tft.drawFastHLine(0, 19, 240, UI_LINE);
        tft.drawFastHLine(0, STATUS_BAR_Y_OFFSET + STATUS_BAR_HEIGHT, 240, UI_LINE);
        uiDrawn = true;
    }

    static unsigned long lastAnimationTime = 0;
    static int animationState = 0;
    static int activeIcon = -1;

    if (animationState > 0 && millis() - lastAnimationTime >= TOOLBAR_ANIMATION_MS) {
        if (animationState == 1) {
            drawToolbarIcon(iconX[activeIcon], iconY, icons[activeIcon], UI_ICON);
            animationState = 2;

            switch (activeIcon) {
                case 0:
                    if (sdTotalProfiles > 0) {
                        exportProfileToSd(currentProfileIndex);
                    }
                    break;
                case 1:
                    if (sdTotalProfiles > 0) {
                        transmitProfile(currentProfileIndex);
                    }
                    break;
                case 2:
                    if (sdTotalProfiles > 0) {
                        deleteProfile(currentProfileIndex);
                    }
                    break;
                case 3: {
                    importProfileFromSd();
                    break;
                }
            }
        } else if (animationState == 2) {
            animationState = 0;
            activeIcon = -1;
        }
        lastAnimationTime = millis();
    }

    static unsigned long lastTouchCheck = 0;
    const unsigned long touchCheckInterval = 50;

    if (millis() - lastTouchCheck >= touchCheckInterval) {
        int x, y;
        if (feature_active && readTouchXY(x, y)) {
            if (y >= LIST_Y && y < (LIST_Y + (ITEMS_PER_PAGE * ROW_H)) && x >= LIST_X && x < (LIST_X + LIST_W)) {
              uint8_t row = (uint8_t)((y - LIST_Y) / ROW_H);
              uint16_t oldIdx = currentProfileIndex;
              uint16_t start = pageStartForIndex(currentProfileIndex);
              uint16_t idx = (uint16_t)(start + row);
              if (idx < sdTotalProfiles) {
                currentProfileIndex = idx;
                selectedValid = false;
                cacheDirty = true;
                deleteArmed = false;
                updateSelectionUI(oldIdx, false);
              }
            }
            if (toolbarIconHitY(y, iconY)) {
                for (int i = 0; i < ICON_NUM; i++) {
                    if (toolbarIconHitX(x, iconX[i])) {
                        if (icons[i] != NULL && animationState == 0) {

                            if (i == 4) {
                                feature_exit_requested = true;
                            } else {

                                drawToolbarIcon(iconX[i], iconY, icons[i], TFT_BLACK);
                                animationState = 1;
                                activeIcon = i;
                                lastAnimationTime = millis();
                            }
                        }
                        break;
                    }
                }
            }
        }
        lastTouchCheck = millis();
    }
}

void saveSetup() {
    Serial.begin(115200);
    subghzBeginSession(SubGhzOwner::SavedProfiles);
    setTouchButtonInputEnabled(true);
    subghzSetProfileNavLabels();

    ELECHOUSE_cc1101.setSpiPin(CC1101_SCK, CC1101_MISO, CC1101_MOSI, CC1101_CS);
    ELECHOUSE_cc1101.setGDO(CC1101_GDO0, CC1101_GDO2);

    refreshSdIndex(false);

    // All directory/File handles are closed now. Perform exactly one explicit
    // SD->CC1101 hand-off before the receiver is initialised; no SD operation
    // is allowed after this point until a user requests one.
    reclaimSharedSpiBus();

#if HAS_PCF8574_BUTTONS
    pcf.pinMode(BTN_UP, INPUT_PULLUP);
    pcf.pinMode(BTN_DOWN, INPUT_PULLUP);
    pcf.pinMode(BTN_LEFT, INPUT_PULLUP);
    pcf.pinMode(BTN_RIGHT, INPUT_PULLUP);
    pcf.pinMode(BTN_SELECT, INPUT_PULLUP);
#endif

    subghzClearBody(TFT_BLACK);
    tft.setTextColor(UI_TEXT);

    setupTouchscreen();

    float currentBatteryVoltage = readBatteryVoltage();
    drawStatusBar(currentBatteryVoltage, true);
    subghzRedrawNavChrome();
    uiDrawn = false;

    subghzInitCc1101Reliable();
    replayat::replayConfigureCc1101Rx();
    pinMode(SUBGHZ_RX_PIN, INPUT);
    pinMode(SUBGHZ_TX_PIN, INPUT);
    ELECHOUSE_cc1101.SetRx();

    mySwitch.enableReceive(SUBGHZ_RX_PIN);
    mySwitch.setRepeatTransmit(8);

    cacheDirty = true;
    deleteArmed = false;
    updateDisplay();
    uiDrawn = false;
    runUI();
    subghzRedrawNavChrome();
}

void saveLoop() {

    if (feature_active && (feature_exit_requested || featureExitButtonPressed())) {
        feature_exit_requested = true;
        return;
    }

    maintainTouchNavBar();
    runUI();
    profileHandleNavButtons();

    static unsigned long lastDebounceTime = 0;
    const unsigned long debounceDelay = 200;

    static bool prevUp = false;
    static bool prevDown = false;
    static bool prevRight = false;
    static bool prevLeft = false;
    const bool prevPressed    = isPhysicalButtonPressed(BTN_UP);
    const bool nextPressed    = isPhysicalButtonPressed(BTN_DOWN);
    const bool txPressed      = isPhysicalButtonPressed(BTN_RIGHT);
    const bool deletePressed = isPhysicalButtonPressed(BTN_LEFT);

    if (sdTotalProfiles > 0) {

        if (nextPressed && !prevDown && millis() - lastDebounceTime > debounceDelay) {
            profileSelectNext();
            lastDebounceTime = millis();
        }

        if (prevPressed && !prevUp && millis() - lastDebounceTime > debounceDelay) {
            profileSelectPrev();
            lastDebounceTime = millis();
        }

        if (txPressed && !prevRight && millis() - lastDebounceTime > debounceDelay) {
            transmitProfile(currentProfileIndex);
            lastDebounceTime = millis();
        }

        if (deletePressed && !prevLeft && millis() - lastDebounceTime > debounceDelay) {
            deleteProfile(currentProfileIndex);
            lastDebounceTime = millis();
        }
    }

    prevUp = prevPressed;
    prevDown = nextPressed;
    prevRight = txPressed;
    prevLeft = deletePressed;
}

void lifecycleShutdown() {
    mySwitch.disableReceive();
    mySwitch.disableTransmit();
    pinMode(SUBGHZ_RX_PIN, INPUT);
    pinMode(SUBGHZ_TX_PIN, INPUT);
}

}

namespace subjammer {

void updateDisplay();

static bool uiDrawn = false;

static unsigned long lastDebounceTime = 0;
const unsigned long debounceDelay = 200;

#define SCREEN_WIDTH 240
#define SCREEN_HEIGHT 64

static constexpr uint8_t JAM_BTN_LEFT  = 4;
static constexpr uint8_t JAM_BTN_RIGHT = 5;
static constexpr uint8_t JAM_BTN_DOWN  = 3;
static constexpr uint8_t JAM_BTN_UP    = 6;

bool jammingRunning = false;
bool continuousMode = true;
bool autoMode = false;
unsigned long lastSweepTime = 0;
const unsigned long sweepInterval = 1000;

static const uint32_t subghz_frequency_list[] = {
    300000000, 303875000, 304250000, 310000000, 314000000, 315000000,
    318000000, 390000000, 418000000, 433075000, 433420000, 433920000,
    434420000, 434775000, 438900000, 868350000, 915000000, 925000000
};
const int numFrequencies = sizeof(subghz_frequency_list) / sizeof(subghz_frequency_list[0]);
int currentFrequencyIndex = 5;
float targetFrequency = subghz_frequency_list[currentFrequencyIndex] / 1000000.0;

static constexpr int kJammerStatusLineY = 79;
static constexpr int kJammerYSHIFT = 20;
static constexpr int kJammerValueLineH = 11;
static constexpr int kJammerProgressY = 60 + kJammerYSHIFT;

static bool s_jammerStaticDrawn = false;

struct JammerDisplayCache {
  bool valid = false;
  int freqMHz100 = -1;
  bool autoMode = false;
  bool continuousMode = false;
  bool jammingRunning = false;
  int progress = -1;
  bool blinkOn = false;
};

static JammerDisplayCache s_jammerDisp;

static void jammerInvalidateDisplay() {
  s_jammerStaticDrawn = false;
  s_jammerDisp = JammerDisplayCache{};
}

static void subjammerToggleJam() {
  jammingRunning = !jammingRunning;
  if (jammingRunning) {
    Serial.println("Jamming started");
    ELECHOUSE_cc1101.setMHZ(targetFrequency);
    ELECHOUSE_cc1101.SetTx();
  } else {
    Serial.println("Jamming stopped");
    ELECHOUSE_cc1101.setSidle();
    digitalWrite(TX_PIN, LOW);
  }
  updateDisplay();
  lastDebounceTime = millis();
}

static void subjammerFreqNext() {
  if (autoMode) {
    return;
  }
  currentFrequencyIndex = (currentFrequencyIndex + 1) % numFrequencies;
  targetFrequency = subghz_frequency_list[currentFrequencyIndex] / 1000000.0;
  ELECHOUSE_cc1101.setMHZ(targetFrequency);
  updateDisplay();
  lastDebounceTime = millis();
}

static void subjammerFreqPrev() {
  if (autoMode) {
    return;
  }
  currentFrequencyIndex = (currentFrequencyIndex - 1 + numFrequencies) % numFrequencies;
  targetFrequency = subghz_frequency_list[currentFrequencyIndex] / 1000000.0;
  ELECHOUSE_cc1101.setMHZ(targetFrequency);
  updateDisplay();
  lastDebounceTime = millis();
}

static void subjammerApplyFrequency() {
  ELECHOUSE_cc1101.setMHZ(targetFrequency);
  if (jammingRunning) {
    ELECHOUSE_cc1101.SetTx();
  } else {
    ELECHOUSE_cc1101.setSidle();
    digitalWrite(TX_PIN, LOW);
  }
}

static void subjammerAutoSweepIfDue() {
  if (!autoMode || millis() - lastSweepTime < sweepInterval) {
    return;
  }

  currentFrequencyIndex = (currentFrequencyIndex + 1) % numFrequencies;
  targetFrequency = subghz_frequency_list[currentFrequencyIndex] / 1000000.0;
  subjammerApplyFrequency();
  updateDisplay();
  lastSweepTime = millis();
}

static void subjammerToggleAuto() {
  autoMode = !autoMode;
  Serial.print("Frequency mode: ");
  Serial.println(autoMode ? "Automatic" : "Manual");
  if (autoMode) {
    currentFrequencyIndex = 0;
    targetFrequency = subghz_frequency_list[currentFrequencyIndex] / 1000000.0;
    lastSweepTime = millis();
    subjammerApplyFrequency();
    s_jammerDisp.freqMHz100 = -1;
  }
  updateDisplay();
  lastDebounceTime = millis();
}

void subjammerHandleNavButtons() {
  if (!featureHasTouchNavBar()) {
    return;
  }

  if (isTouchNavButtonPressedEdge(BTN_UP)) {
    subjammerToggleJam();
    subghzWaitNavRelease(BTN_UP);
  }
  if (isTouchNavButtonPressedEdge(BTN_LEFT)) {
    subjammerFreqPrev();
    subghzWaitNavRelease(BTN_LEFT);
  }
  if (isTouchNavButtonPressedEdge(BTN_RIGHT)) {
    subjammerFreqNext();
    subghzWaitNavRelease(BTN_RIGHT);
  }
  if (isTouchNavButtonPressedEdge(BTN_DOWN)) {
    subjammerToggleAuto();
    subghzWaitNavRelease(BTN_DOWN);
  }
}

static void jammerDrawStatusSeparator() {
  tft.drawFastHLine(0, kJammerStatusLineY, 240, UI_LINE);
}

static void jammerDrawValueCell(int x, int y, int w, int h, const String& text, uint16_t color) {
  const int maxH = kJammerStatusLineY - y;
  if (maxH <= 0) {
    return;
  }
  const int clipH = min(h, maxH);
  tft.fillRect(x, y, w, clipH, TFT_BLACK);
  tft.setTextSize(1);
  tft.setTextColor(color, TFT_BLACK);
  tft.setCursor(x, y);
  tft.print(text);
}

static void jammerDrawStaticChrome() {
  if (s_jammerStaticDrawn) {
    return;
  }

  const int bodyBottom = subghzContentBottom();
  const int bodyH = min(kJammerStatusLineY - 40, bodyBottom - 40);
  if (bodyH > 0) {
    tft.fillRect(0, 40, 240, bodyH, TFT_BLACK);
  }
  jammerDrawStatusSeparator();

  tft.setTextSize(1);
  tft.setTextColor(UI_TEXT, TFT_BLACK);
  tft.setCursor(5, 22 + kJammerYSHIFT);
  tft.print("Freq:");
  tft.setCursor(130, 22 + kJammerYSHIFT);
  tft.print("Mode:");
  tft.setCursor(5, 42 + kJammerYSHIFT);
  tft.print("Status:");

  s_jammerStaticDrawn = true;
}

static void jammerDrawProgressBar(int progress) {
  tft.fillRect(0, kJammerProgressY, 240, 4, TFT_BLACK);
  if (progress > 0) {
    tft.fillRect(0, kJammerProgressY, progress, 4, UI_WARN);
  }
}

static void jammerDrawBlinkDot(bool on) {
  const int cx = 220;
  const int cy = 22 + kJammerYSHIFT;
  const int r = 2;
  if (on) {
    tft.fillCircle(cx, cy, r, UI_WARN);
  } else {
    tft.fillRect(cx - r, cy - r, r * 2 + 1, r * 2 + 1, TFT_BLACK);
  }
}

static void jammerPollBlinkIndicator() {
  const bool wantBlink = autoMode && jammingRunning;
  const bool blinkOn = wantBlink && ((millis() % 1000) < 500);
  if (!s_jammerDisp.valid) {
    return;
  }
  if (blinkOn != s_jammerDisp.blinkOn) {
    jammerDrawBlinkDot(blinkOn);
    s_jammerDisp.blinkOn = blinkOn;
  }
}

void updateDisplay() {
    jammerDrawStaticChrome();

    char freqBuf[20];
    char modeBuf[8];
    char statusBuf[8];

    if (autoMode) {
      snprintf(freqBuf, sizeof(freqBuf), "Auto:%.1f", targetFrequency);
    } else {
      snprintf(freqBuf, sizeof(freqBuf), "%.2f MHz", targetFrequency);
    }
    snprintf(modeBuf, sizeof(modeBuf), "%s", continuousMode ? "Cont" : "Noise");
    snprintf(statusBuf, sizeof(statusBuf), "%s", jammingRunning ? "Jamming" : "Idle   ");

    const int freqKey = (int)(targetFrequency * 100.0f + 0.5f);
    const bool fullRedraw = !s_jammerDisp.valid;
    if (fullRedraw || s_jammerDisp.freqMHz100 != freqKey ||
        s_jammerDisp.autoMode != autoMode) {
      jammerDrawValueCell(40, 22 + kJammerYSHIFT, 96, kJammerValueLineH, freqBuf,
                          autoMode ? UI_WARN : UI_TEXT);
      s_jammerDisp.freqMHz100 = freqKey;
      s_jammerDisp.autoMode = autoMode;
    }

    if (fullRedraw || s_jammerDisp.continuousMode != continuousMode) {
      jammerDrawValueCell(165, 22 + kJammerYSHIFT, 40, kJammerValueLineH, modeBuf,
                          continuousMode ? UI_WARN : UI_TEXT);
      s_jammerDisp.continuousMode = continuousMode;
    }

    if (fullRedraw || s_jammerDisp.jammingRunning != jammingRunning) {
      jammerDrawValueCell(50, 42 + kJammerYSHIFT, 72, kJammerValueLineH, statusBuf,
                          jammingRunning ? UI_WARN : UI_TEXT);
      s_jammerDisp.jammingRunning = jammingRunning;
    }

    if (autoMode) {
      const int progress = ::map(currentFrequencyIndex, 0, numFrequencies - 1, 0, 240);
      if (fullRedraw || s_jammerDisp.progress != progress) {
        jammerDrawProgressBar(progress);
        s_jammerDisp.progress = progress;
      }
    } else if (s_jammerDisp.progress != -1) {
      jammerDrawProgressBar(0);
      s_jammerDisp.progress = -1;
      if (s_jammerDisp.blinkOn) {
        jammerDrawBlinkDot(false);
        s_jammerDisp.blinkOn = false;
      }
    }

    jammerDrawStatusSeparator();
    s_jammerDisp.valid = true;
}

void runUI() {
    #define SCREEN_WIDTH  240
    #define SCREENHEIGHT 320
    #define STATUS_BAR_Y_OFFSET 20
    #define STATUS_BAR_HEIGHT 20
    #define ICON_SIZE 16
    #define ICON_NUM 6

    static int iconX[ICON_NUM] = {50, 90, 130, 170, 210, 10};
    static int iconY = STATUS_BAR_Y_OFFSET;

    static const unsigned char* icons[ICON_NUM] = {
        bitmap_icon_power,
        bitmap_icon_antenna,
        bitmap_icon_random,
        bitmap_icon_sort_down_minus,
        bitmap_icon_sort_up_plus,
        bitmap_icon_go_back
    };

    if (!uiDrawn) {
        tft.fillRect(0, STATUS_BAR_Y_OFFSET, SCREEN_WIDTH, STATUS_BAR_HEIGHT, DARK_GRAY);

        for (int i = 0; i < ICON_NUM; i++) {
            if (icons[i] != NULL) {
                drawToolbarIcon(iconX[i], iconY, icons[i], UI_ICON);
            }
        }
        tft.drawFastHLine(0, 19, 240, UI_LINE);
        tft.drawFastHLine(0, STATUS_BAR_Y_OFFSET + STATUS_BAR_HEIGHT, 240, UI_LINE);
        uiDrawn = true;
    }

    static unsigned long lastAnimationTime = 0;
    static int animationState = 0;
    static int activeIcon = -1;

    if (animationState > 0 && millis() - lastAnimationTime >= TOOLBAR_ANIMATION_MS) {
        if (animationState == 1) {
            drawToolbarIcon(iconX[activeIcon], iconY, icons[activeIcon], UI_ICON);
            animationState = 2;

            switch (activeIcon) {
                case 0:
                  jammingRunning = !jammingRunning;
                    if (jammingRunning) {
                        Serial.println("Jamming started");
                        ELECHOUSE_cc1101.setMHZ(targetFrequency);
                        ELECHOUSE_cc1101.SetTx();
                    } else {
                        Serial.println("Jamming stopped");
                        ELECHOUSE_cc1101.setSidle();
                        digitalWrite(TX_PIN, LOW);
                    }
                    updateDisplay();
                    lastDebounceTime = millis();
                    break;
                case 1:
                 continuousMode = !continuousMode;
                  Serial.print("Jamming mode: ");
                  Serial.println(continuousMode ? "Continuous Carrier" : "Noise");
                  updateDisplay();
                  lastDebounceTime = millis();
                    break;
                case 2:
                  autoMode = !autoMode;
                  Serial.print("Frequency mode: ");
                  Serial.println(autoMode ? "Automatic" : "Manual");
                  if (autoMode) {
                      currentFrequencyIndex = 0;
                      targetFrequency = subghz_frequency_list[currentFrequencyIndex] / 1000000.0;
                      lastSweepTime = millis();
                      subjammerApplyFrequency();
                      s_jammerDisp.freqMHz100 = -1;
                  }
                  updateDisplay();
                  lastDebounceTime = millis();
                    break;
                case 3:
                  currentFrequencyIndex = (currentFrequencyIndex - 1 + numFrequencies) % numFrequencies;
                  targetFrequency = subghz_frequency_list[currentFrequencyIndex] / 1000000.0;
                  ELECHOUSE_cc1101.setMHZ(targetFrequency);
                  Serial.print("Switched to: ");
                  Serial.print(targetFrequency);
                  Serial.println(" MHz");
                  updateDisplay();
                  lastDebounceTime = millis();
                    break;
                 case 4:
                  currentFrequencyIndex = (currentFrequencyIndex + 1) % numFrequencies;
                  targetFrequency = subghz_frequency_list[currentFrequencyIndex] / 1000000.0;
                  ELECHOUSE_cc1101.setMHZ(targetFrequency);
                  Serial.print("Switched to: ");
                  Serial.print(targetFrequency);
                  Serial.println(" MHz");
                  updateDisplay();
                  lastDebounceTime = millis();
                    break;
                case 5:
                    feature_exit_requested = true;
                    break;
            }
        } else if (animationState == 2) {
            animationState = 0;
            activeIcon = -1;
        }
        lastAnimationTime = millis();
    }

    static unsigned long lastTouchCheck = 0;
    const unsigned long touchCheckInterval = 50;

    if (millis() - lastTouchCheck >= touchCheckInterval) {
        int x, y;
        if (feature_active && readTouchXY(x, y)) {
            if (toolbarIconHitY(y, iconY)) {
                for (int i = 0; i < ICON_NUM; i++) {
                    if (toolbarIconHitX(x, iconX[i])) {
                        if (icons[i] != NULL && animationState == 0) {

                            if (i == 5) {
                                feature_exit_requested = true;
                            } else {

                                drawToolbarIcon(iconX[i], iconY, icons[i], TFT_BLACK);
                                animationState = 1;
                                activeIcon = i;
                                lastAnimationTime = millis();
                            }
                        }
                        break;
                    }
                }
            }
        }
        lastTouchCheck = millis();
    }
#undef SCREEN_WIDTH
#undef SCREENHEIGHT
#undef STATUS_BAR_Y_OFFSET
#undef STATUS_BAR_HEIGHT
#undef ICON_SIZE
#undef ICON_NUM
}

void subjammerSetup() {
    Serial.begin(115200);
    subghzBeginSession(SubGhzOwner::Jammer);
    setTouchButtonInputEnabled(true);
    subghzSetJammerNavLabels();
    subghzClearBody(TFT_BLACK);
    drawStatusBar(readBatteryVoltage(), true);
    subghzRedrawNavChrome();

    ELECHOUSE_cc1101.setSpiPin(CC1101_SCK, CC1101_MISO, CC1101_MOSI, CC1101_CS);

    subghzInitCc1101Reliable();
    ELECHOUSE_cc1101.setModulation(0);
    ELECHOUSE_cc1101.setRxBW(500.0);
    ELECHOUSE_cc1101.setPA(12);
    ELECHOUSE_cc1101.setMHZ(targetFrequency);
    ELECHOUSE_cc1101.SetTx();

    randomSeed(esp_random());

#if HAS_PCF8574_BUTTONS
    pcf.pinMode(BTN_LEFT, INPUT_PULLUP);
    pcf.pinMode(BTN_RIGHT, INPUT_PULLUP);
    pcf.pinMode(BTN_DOWN, INPUT_PULLUP);
    pcf.pinMode(BTN_UP, INPUT_PULLUP);
#endif
    delay(100);

    subghzClearBody(TFT_BLACK);
    drawStatusBar(readBatteryVoltage(), true);

    setupTouchscreen();

   jammerInvalidateDisplay();
   updateDisplay();
   uiDrawn = false;
   subghzRedrawNavChrome();
}

void subjammerLoop() {

    if (feature_active && (feature_exit_requested || featureExitButtonPressed())) {
        feature_exit_requested = true;
        return;
    }

    maintainTouchNavBar();
    runUI();
    if (uiDrawn) {
      tft.drawFastHLine(0, 19, 240, UI_LINE);
      tft.drawFastHLine(0, 40, 240, UI_LINE);
      if (s_jammerDisp.valid) {
        jammerDrawStatusSeparator();
      }
    }
    jammerPollBlinkIndicator();
    subjammerHandleNavButtons();

#if HAS_PCF8574_BUTTONS
    int btnLeftState = pcf.digitalRead(JAM_BTN_LEFT);
    int btnRightState = pcf.digitalRead(JAM_BTN_RIGHT);
    int btnUpState = pcf.digitalRead(JAM_BTN_UP);
    int btnDownState = pcf.digitalRead(JAM_BTN_DOWN);
#else
    int btnLeftState = isPhysicalButtonPressed(BTN_LEFT) ? LOW : HIGH;
    int btnRightState = isPhysicalButtonPressed(BTN_RIGHT) ? LOW : HIGH;
    int btnUpState = isPhysicalButtonPressed(BTN_UP) ? LOW : HIGH;
    int btnDownState = isPhysicalButtonPressed(BTN_DOWN) ? LOW : HIGH;
#endif

    if (btnUpState == LOW && millis() - lastDebounceTime > debounceDelay) {
        subjammerToggleJam();
    }

    if (btnRightState == LOW && !autoMode && millis() - lastDebounceTime > debounceDelay) {
        subjammerFreqNext();
    }

    if (btnLeftState == LOW && !autoMode && millis() - lastDebounceTime > debounceDelay) {
        subjammerFreqPrev();
    }

    if (btnDownState == LOW && millis() - lastDebounceTime > debounceDelay) {
        subjammerToggleAuto();
    }

    subjammerAutoSweepIfDue();

    if (jammingRunning) {
        ELECHOUSE_cc1101.SetTx();

        if (continuousMode) {
            ELECHOUSE_cc1101.SpiWriteReg(CC1101_TXFIFO, 0xFF);
            ELECHOUSE_cc1101.SpiStrobe(CC1101_STX);
            digitalWrite(TX_PIN, HIGH);
        } else {
            for (int i = 0; i < 10; i++) {
                uint32_t noise = random(16777216);
                ELECHOUSE_cc1101.SpiWriteReg(CC1101_TXFIFO, noise >> 16);
                ELECHOUSE_cc1101.SpiWriteReg(CC1101_TXFIFO, (noise >> 8) & 0xFF);
                ELECHOUSE_cc1101.SpiWriteReg(CC1101_TXFIFO, noise & 0xFF);
                ELECHOUSE_cc1101.SpiStrobe(CC1101_STX);
                delayMicroseconds(50);
              }
          }
      }
  }

void lifecycleShutdown() {
    jammingRunning = false;
    autoMode = false;
    digitalWrite(TX_PIN, LOW);
    pinMode(TX_PIN, INPUT);
}
}

namespace SubBrute {

static constexpr uint8_t BRUTE_TX_PIN = SUBGHZ_TX_PIN;

static const uint32_t kBruteFreqList[] = {
    300000000, 303875000, 304250000, 310000000, 314000000, 315000000,
    318000000, 390000000, 418000000, 433075000, 433420000, 433920000,
    434420000, 434775000, 438900000, 868350000, 915000000, 925000000
};
static constexpr int kBruteFreqCount =
    (int)(sizeof(kBruteFreqList) / sizeof(kBruteFreqList[0]));

static const uint8_t kBitsChoices[] = {8, 10, 12, 16, 18, 20, 24};
static constexpr int kBitsChoiceCount =
    (int)(sizeof(kBitsChoices) / sizeof(kBitsChoices[0]));

static const uint16_t kPulseChoicesUs[] = {250, 350, 400, 500};
static constexpr int kPulseChoiceCount =
    (int)(sizeof(kPulseChoicesUs) / sizeof(kPulseChoicesUs[0]));

static constexpr int kBarBottom = 36;
static constexpr int kPanelTop = 44;
static constexpr int kBoxHeaderH = 14;
static constexpr int kLineH = 14;
static constexpr int kPanelPadX = 4;
static constexpr int kPanelW = 240 - (kPanelPadX * 2);
static constexpr int kLabelX = 12;
static constexpr int kValueX = 58;
static constexpr int kValueW = 170;
static constexpr int kSettingsRows = 4;
static constexpr int kSettingsInnerH = kBoxHeaderH + (kSettingsRows * kLineH) + 4;
static constexpr int kProgressInnerH = kBoxHeaderH + (kLineH * 2) + 16;
static constexpr int kRadius = 3;

enum FocusRow : uint8_t { FOCUS_FREQ = 0, FOCUS_BITS, FOCUS_MODE, FOCUS_OPT, FOCUS_COUNT };
enum RunMode : uint8_t { MODE_DEBRUIJN = 0, MODE_BRUTE = 1 };
enum RunState : uint8_t { ST_IDLE = 0, ST_RUNNING, ST_DONE, ST_STOPPED };

static RCSwitch s_switch;
static bool s_uiDrawn = false;
static unsigned long s_lastDebounce = 0;
static constexpr unsigned long kDebounceMs = 200;

static int s_freqIndex = 11;  // 433.92 MHz
static int s_bitsIndex = 2;   // 12-bit
static int s_pulseIndex = 1;  // 350 us
static int s_protocol = 1;
static FocusRow s_focus = FOCUS_FREQ;
static RunMode s_mode = MODE_DEBRUIJN;
static RunState s_runState = ST_IDLE;
static bool s_stopRequested = false;
static bool s_running = false;

static uint32_t s_progressDone = 0;
static uint32_t s_progressTotal = 0;
static uint32_t s_lastUiProgress = 0xFFFFFFFFu;
static uint8_t s_lastUiPct = 255;
static RunState s_lastUiState = ST_IDLE;
static FocusRow s_lastUiFocus = FOCUS_COUNT;
static RunMode s_lastUiMode = MODE_BRUTE;
static int s_lastUiFreq = -1;
static int s_lastUiBits = -1;
static int s_lastUiPulse = -1;
static int s_lastUiProto = -1;
static bool s_chromeDrawn = false;

static float bruteFreqMHz() {
  return kBruteFreqList[s_freqIndex % kBruteFreqCount] / 1000000.0f;
}

static uint8_t bruteBits() {
  return kBitsChoices[s_bitsIndex % kBitsChoiceCount];
}

static uint16_t brutePulseUs() {
  return kPulseChoicesUs[s_pulseIndex % kPulseChoiceCount];
}

static int settingsPanelY() { return kPanelTop; }
static int settingsPanelH() { return kSettingsInnerH; }
static int progressPanelY() { return kPanelTop + kSettingsInnerH + 6; }
static int progressPanelH() { return kProgressInnerH; }
static int hintY0() { return progressPanelY() + kProgressInnerH + 6; }

static int rowTextY(int row) {
  return settingsPanelY() + kBoxHeaderH + 2 + row * kLineH;
}

// Fibonacci LFSR feedback: taps are 1-indexed from LSB.
static uint32_t bruteLfsrFeedback(uint8_t n, uint32_t state) {
  uint32_t fb = 0;
  switch (n) {
    case 8:
      fb ^= (state >> (8 - 1)) & 1u;
      fb ^= (state >> (6 - 1)) & 1u;
      fb ^= (state >> (5 - 1)) & 1u;
      fb ^= (state >> (4 - 1)) & 1u;
      break;
    case 10:
      fb ^= (state >> (10 - 1)) & 1u;
      fb ^= (state >> (7 - 1)) & 1u;
      break;
    case 12:
      fb ^= (state >> (12 - 1)) & 1u;
      fb ^= (state >> (11 - 1)) & 1u;
      fb ^= (state >> (8 - 1)) & 1u;
      fb ^= (state >> (6 - 1)) & 1u;
      break;
    case 16:
      fb ^= (state >> (16 - 1)) & 1u;
      fb ^= (state >> (14 - 1)) & 1u;
      fb ^= (state >> (13 - 1)) & 1u;
      fb ^= (state >> (11 - 1)) & 1u;
      break;
    case 18:
      fb ^= (state >> (18 - 1)) & 1u;
      fb ^= (state >> (11 - 1)) & 1u;
      break;
    case 20:
      fb ^= (state >> (20 - 1)) & 1u;
      fb ^= (state >> (17 - 1)) & 1u;
      break;
    case 24:
      fb ^= (state >> (24 - 1)) & 1u;
      fb ^= (state >> (23 - 1)) & 1u;
      fb ^= (state >> (22 - 1)) & 1u;
      fb ^= (state >> (17 - 1)) & 1u;
      break;
    default:
      fb = (state >> (n - 1)) & 1u;
      break;
  }
  (void)n;
  return fb & 1u;
}

static void bruteRadioIdle() {
  ELECHOUSE_cc1101.setSidle();
  digitalWrite(BRUTE_TX_PIN, LOW);
}

static void brutePrepareTx() {
  holdSdInactiveOnSharedSpi();
  reclaimSharedSpiBus();
#if defined(SD_CS)
  pinMode(SD_CS, OUTPUT);
  digitalWrite(SD_CS, HIGH);
#endif
#if defined(CC1101_CS)
  pinMode(CC1101_CS, OUTPUT);
  digitalWrite(CC1101_CS, HIGH);
#endif
  ELECHOUSE_cc1101.setSpiPin(CC1101_SCK, CC1101_MISO, CC1101_MOSI, CC1101_CS);
  ELECHOUSE_cc1101.setGDO(CC1101_GDO0, CC1101_GDO2);
  ELECHOUSE_cc1101.setSidle();
  ELECHOUSE_cc1101.setMHZ(bruteFreqMHz());
  ELECHOUSE_cc1101.setCCMode(0);
  ELECHOUSE_cc1101.setModulation(2);
  ELECHOUSE_cc1101.setPA(12);
  pinMode(BRUTE_TX_PIN, OUTPUT);
  digitalWrite(BRUTE_TX_PIN, LOW);
  ELECHOUSE_cc1101.SetTx();
}

static void bruteFinishTx() {
  s_switch.disableTransmit();
  s_switch.disableReceive();
  bruteRadioIdle();
  pinMode(BRUTE_TX_PIN, OUTPUT);
  digitalWrite(BRUTE_TX_PIN, LOW);
}

static bool bruteShouldAbort() {
  if (feature_exit_requested || featureExitButtonPressed()) {
    feature_exit_requested = true;
    s_stopRequested = true;
    return true;
  }
  if (s_stopRequested) {
    return true;
  }
  maintainTouchNavBar();
  if (isTouchNavButtonPressedEdge(BTN_UP)) {
    s_stopRequested = true;
    return true;
  }
  static bool prevPhysUp = false;
  const bool physUp = isPhysicalButtonPressed(BTN_UP);
  if (physUp && !prevPhysUp) {
    prevPhysUp = physUp;
    s_stopRequested = true;
    return true;
  }
  prevPhysUp = physUp;

  int x, y;
  if (readTouchXY(x, y)) {
    if (y > 20 && y < kBarBottom && x >= 10 && x < 26) {
      feature_exit_requested = true;
      s_stopRequested = true;
      return true;
    }
  }
  return false;
}

static void bruteWaitGoRelease() {
  const uint32_t t0 = millis();
  while ((isTouchNavButtonPressed(BTN_UP) || isPhysicalButtonPressed(BTN_UP)) &&
         (millis() - t0) < 800) {
    delay(5);
  }
  delay(30);
  (void)isTouchNavButtonPressedEdge(BTN_UP);
}

static void drawPanelFrame(int y, int h, const char* title) {
  tft.drawRoundRect(kPanelPadX, y, kPanelW, h, kRadius, UI_LINE);
  tft.setTextSize(1);
  tft.setTextColor(UI_DIM_TEXT, TFT_BLACK);
  tft.setCursor(kLabelX, y + 3);
  tft.print(title);
}

static void clearValueCell(int y) {
  tft.fillRect(kValueX, y, kValueW, kLineH - 1, TFT_BLACK);
}

static void drawSettingRow(int row, bool focused) {
  const int y = rowTextY(row);
  tft.fillRect(kLabelX, y, kValueX - kLabelX - 2, kLineH - 1, TFT_BLACK);
  clearValueCell(y);

  const char* label = "?";
  char value[28];
  value[0] = '\0';

  switch (row) {
    case FOCUS_FREQ:
      label = "Freq";
      snprintf(value, sizeof(value), "%.2f MHz", bruteFreqMHz());
      break;
    case FOCUS_BITS:
      label = "Bits";
      snprintf(value, sizeof(value), "%u", (unsigned)bruteBits());
      break;
    case FOCUS_MODE:
      label = "Mode";
      snprintf(value, sizeof(value), "%s",
               s_mode == MODE_DEBRUIJN ? "De Bruijn" : "Brute");
      break;
    case FOCUS_OPT:
      if (s_mode == MODE_DEBRUIJN) {
        label = "Pulse";
        snprintf(value, sizeof(value), "%u us", (unsigned)brutePulseUs());
      } else {
        label = "Proto";
        snprintf(value, sizeof(value), "%d", s_protocol);
      }
      break;
    default:
      break;
  }

  tft.setTextSize(1);
  tft.setTextColor(focused ? ORANGE : UI_DIM_TEXT, TFT_BLACK);
  tft.setCursor(kLabelX, y);
  tft.print(focused ? ">" : " ");
  tft.print(label);

  tft.setTextColor(focused ? ORANGE : UI_TEXT, TFT_BLACK);
  tft.setCursor(kValueX, y);
  tft.print(value);
}

static void drawProgressBody(bool force) {
  const int py = progressPanelY();
  const int statusY = py + kBoxHeaderH + 2;
  const int countY = statusY + kLineH;
  const int barX = kLabelX;
  const int barW = kPanelW - 16;
  const int barH = 6;

  uint8_t pct = 0;
  if (s_progressTotal > 0) {
    pct = (uint8_t)((s_progressDone * 100UL) / s_progressTotal);
    if (pct > 100) pct = 100;
  }

  const bool stateChanged = force || s_lastUiState != s_runState;
  const bool progChanged =
      force || s_lastUiProgress != s_progressDone || s_lastUiPct != pct;

  if (stateChanged) {
    tft.fillRect(kLabelX, statusY, kPanelW - 16, kLineH - 1, TFT_BLACK);
    tft.setTextSize(1);
    const char* st = "Idle";
    uint16_t col = UI_DIM_TEXT;
    if (s_runState == ST_RUNNING) {
      st = "Running";
      col = ORANGE;
    } else if (s_runState == ST_DONE) {
      st = "Done";
      col = UI_TEXT;
    } else if (s_runState == ST_STOPPED) {
      st = "Stopped";
      col = UI_WARN;
    }
    tft.setTextColor(UI_DIM_TEXT, TFT_BLACK);
    tft.setCursor(kLabelX, statusY);
    tft.print("Status");
    tft.setTextColor(col, TFT_BLACK);
    tft.setCursor(kValueX, statusY);
    tft.print(st);
    s_lastUiState = s_runState;
  }

  if (progChanged) {
    tft.fillRect(kLabelX, countY, kPanelW - 16, kLineH - 1, TFT_BLACK);
    char buf[40];
    if (s_progressTotal == 0) {
      snprintf(buf, sizeof(buf), "0 / 0");
    } else if (s_progressTotal >= 1000000UL) {
      snprintf(buf, sizeof(buf), "%lu/%lu %u%%",
               (unsigned long)s_progressDone,
               (unsigned long)s_progressTotal,
               (unsigned)pct);
    } else {
      snprintf(buf, sizeof(buf), "%lu / %lu  %u%%",
               (unsigned long)s_progressDone,
               (unsigned long)s_progressTotal,
               (unsigned)pct);
    }
    tft.setTextSize(1);
    tft.setTextColor(UI_DIM_TEXT, TFT_BLACK);
    tft.setCursor(kLabelX, countY);
    tft.print("Count");
    tft.setTextColor(UI_TEXT, TFT_BLACK);
    tft.setCursor(kValueX, countY);
    tft.print(buf);

    const int barY2 = countY + kLineH + 2;
    tft.fillRect(barX, barY2, barW, barH, DARK_GRAY);
    const int fill = (s_progressTotal > 0)
                         ? (int)((s_progressDone * (uint32_t)barW) / s_progressTotal)
                         : 0;
    if (fill > 0) {
      tft.fillRect(barX, barY2, min(fill, barW), barH, ORANGE);
    }
    s_lastUiProgress = s_progressDone;
    s_lastUiPct = pct;
  }
}

static void drawHints() {
  const int y0 = hintY0();
  const int bottom = subghzContentBottom();
  if (y0 + 20 >= bottom) {
    return;
  }
  tft.fillRect(0, y0, 240, min(28, bottom - y0), TFT_BLACK);
  tft.setTextSize(1);
  tft.setTextColor(UI_DIM_TEXT, TFT_BLACK);
  tft.setCursor(kLabelX, y0);
  tft.print("Sel focus   Prev/Next adjust");
  if (y0 + 12 < bottom) {
    tft.setCursor(kLabelX, y0 + 12);
    tft.print("Go start/stop");
  }
}

static void updateDisplay(bool force = false) {
  if (force || !s_chromeDrawn) {
    const int bodyBottom = subghzContentBottom();
    if (bodyBottom > kBarBottom) {
      tft.fillRect(0, kBarBottom + 1, 240, bodyBottom - kBarBottom - 1, TFT_BLACK);
    }
    tft.drawFastHLine(0, 19, 240, UI_LINE);
    tft.drawFastHLine(0, kBarBottom, 240, UI_LINE);

    drawPanelFrame(settingsPanelY(), settingsPanelH(), "Settings");
    drawPanelFrame(progressPanelY(), progressPanelH(), "Progress");
    drawHints();

    for (int r = 0; r < kSettingsRows; r++) {
      drawSettingRow(r, (FocusRow)r == s_focus);
    }
    s_lastUiFocus = s_focus;
    s_lastUiMode = s_mode;
    s_lastUiFreq = s_freqIndex;
    s_lastUiBits = s_bitsIndex;
    s_lastUiPulse = s_pulseIndex;
    s_lastUiProto = s_protocol;
    s_lastUiState = (RunState)255;
    s_lastUiProgress = 0xFFFFFFFFu;
    s_lastUiPct = 255;
    s_chromeDrawn = true;
    drawProgressBody(true);
    return;
  }

  const bool settingsDirty =
      s_lastUiFocus != s_focus || s_lastUiMode != s_mode ||
      s_lastUiFreq != s_freqIndex || s_lastUiBits != s_bitsIndex ||
      s_lastUiPulse != s_pulseIndex || s_lastUiProto != s_protocol;

  if (settingsDirty) {
    for (int r = 0; r < kSettingsRows; r++) {
      drawSettingRow(r, (FocusRow)r == s_focus);
    }
    s_lastUiFocus = s_focus;
    s_lastUiMode = s_mode;
    s_lastUiFreq = s_freqIndex;
    s_lastUiBits = s_bitsIndex;
    s_lastUiPulse = s_pulseIndex;
    s_lastUiProto = s_protocol;
  }

  drawProgressBody(false);
}

static void invalidateChrome() {
  s_chromeDrawn = false;
}

static void adjustFocused(int dir) {
  if (s_running) {
    return;
  }
  switch (s_focus) {
    case FOCUS_FREQ:
      s_freqIndex = (s_freqIndex + dir + kBruteFreqCount) % kBruteFreqCount;
      ELECHOUSE_cc1101.setMHZ(bruteFreqMHz());
      break;
    case FOCUS_BITS:
      s_bitsIndex = (s_bitsIndex + dir + kBitsChoiceCount) % kBitsChoiceCount;
      break;
    case FOCUS_MODE:
      s_mode = (s_mode == MODE_DEBRUIJN) ? MODE_BRUTE : MODE_DEBRUIJN;
      break;
    case FOCUS_OPT:
      if (s_mode == MODE_DEBRUIJN) {
        s_pulseIndex = (s_pulseIndex + dir + kPulseChoiceCount) % kPulseChoiceCount;
      } else {
        s_protocol += dir;
        if (s_protocol < 1) s_protocol = 12;
        if (s_protocol > 12) s_protocol = 1;
      }
      break;
    default:
      break;
  }
  updateDisplay();
  s_lastDebounce = millis();
}

static void cycleFocus() {
  if (s_running) {
    return;
  }
  s_focus = (FocusRow)((s_focus + 1) % FOCUS_COUNT);
  updateDisplay();
  s_lastDebounce = millis();
}

static void focusRowAtY(int y) {
  if (s_running) {
    return;
  }
  for (int r = 0; r < kSettingsRows; r++) {
    const int ry = rowTextY(r);
    if (y >= ry && y < ry + kLineH) {
      s_focus = (FocusRow)r;
      updateDisplay();
      s_lastDebounce = millis();
      return;
    }
  }
}

static void runDeBruijnStream() {
  const uint8_t n = bruteBits();
  const uint16_t pulse = brutePulseUs();
  const uint32_t mask = (n >= 32) ? 0xFFFFFFFFu : ((1UL << n) - 1UL);
  const uint32_t total = (n >= 32) ? 0xFFFFFFFFu : ((1UL << n) - 1UL);

  s_progressTotal = total;
  s_progressDone = 0;
  s_runState = ST_RUNNING;
  updateDisplay(true);

  brutePrepareTx();

  uint32_t state = 1u;
  for (uint32_t i = 0; i < total; i++) {
    if ((i & 0xFFu) == 0) {
      if (bruteShouldAbort()) {
        break;
      }
      yield();
      delay(0);
      s_progressDone = i;
      drawProgressBody(false);
    }

    const uint8_t bit = (uint8_t)(state & 1u);
    digitalWrite(BRUTE_TX_PIN, bit ? HIGH : LOW);
    delayMicroseconds(pulse);

    const uint32_t fb = bruteLfsrFeedback(n, state);
    state = ((state << 1) | fb) & mask;
  }

  digitalWrite(BRUTE_TX_PIN, LOW);
  bruteFinishTx();

  if (s_stopRequested || feature_exit_requested) {
    s_runState = ST_STOPPED;
  } else {
    s_progressDone = total;
    s_runState = ST_DONE;
  }
  s_running = false;
  s_stopRequested = false;
  updateDisplay(true);
}

static void runBruteForce() {
  const uint8_t bits = bruteBits();
  // Practical cap: framed RCSwitch brute beyond 12 bits is extremely slow.
  // Still allow larger sizes, but keep the loop responsive.
  const uint32_t total = (bits >= 31) ? 0x7FFFFFFFu : (1UL << bits);

  s_progressTotal = total;
  s_progressDone = 0;
  s_runState = ST_RUNNING;
  updateDisplay(true);

  brutePrepareTx();
  s_switch.disableReceive();
  s_switch.enableTransmit(BRUTE_TX_PIN);
  s_switch.setProtocol(s_protocol);
  s_switch.setPulseLength(brutePulseUs());
  // Default RCSwitch repeats (~10) make each code ~0.5–1s → looks frozen.
  s_switch.setRepeatTransmit(1);

  uint32_t lastUiMs = millis();
  for (uint32_t code = 0; code < total; code++) {
    // Poll stop often — send() itself blocks briefly per code.
    if ((code & 0x03u) == 0) {
      if (bruteShouldAbort()) {
        s_progressDone = code;
        break;
      }
      yield();
    }

    s_switch.send(code, bits);
    s_progressDone = code + 1;

    // Keep progress alive so the UI doesn't look hung.
    const uint32_t now = millis();
    if (now - lastUiMs >= 150) {
#if defined(CC1101_CS)
      digitalWrite(CC1101_CS, HIGH);
#endif
      drawProgressBody(false);
      maintainTouchNavBar();
      // TFT SPI can disturb CC1101 — re-enter TX for the next burst.
      ELECHOUSE_cc1101.SetTx();
      lastUiMs = now;
      yield();
    }
  }

  bruteFinishTx();

  if (s_stopRequested || feature_exit_requested) {
    s_runState = ST_STOPPED;
  } else {
    s_progressDone = total;
    s_runState = ST_DONE;
  }
  s_running = false;
  s_stopRequested = false;
  updateDisplay(true);
}

static void startOrStop() {
  if (s_running) {
    s_stopRequested = true;
    s_lastDebounce = millis();
    return;
  }

  bruteWaitGoRelease();
  s_stopRequested = false;
  s_running = true;
  s_runState = ST_RUNNING;
  s_progressDone = 0;
  s_progressTotal = 0;
  updateDisplay(true);

  if (s_mode == MODE_DEBRUIJN) {
    runDeBruijnStream();
  } else {
    runBruteForce();
  }
  s_lastDebounce = millis();
}

void bruteHandleNavButtons() {
  if (!featureHasTouchNavBar()) {
    return;
  }
  if (isTouchNavButtonPressedEdge(BTN_LEFT)) {
    adjustFocused(-1);
    subghzWaitNavRelease(BTN_LEFT);
  }
  if (isTouchNavButtonPressedEdge(BTN_RIGHT)) {
    adjustFocused(+1);
    subghzWaitNavRelease(BTN_RIGHT);
  }
  if (isTouchNavButtonPressedEdge(BTN_DOWN)) {
    cycleFocus();
    subghzWaitNavRelease(BTN_DOWN);
  }
  if (isTouchNavButtonPressedEdge(BTN_UP)) {
    startOrStop();
    subghzWaitNavRelease(BTN_UP);
  }
}

void runUI() {
  // Avoid jammer/replay macros (SCREEN_WIDTH, ICON_NUM, …) leaking into this scope.
  static constexpr int kBarY = 20;
  static constexpr int kBarH = 20;
  static constexpr int kIconSz = 16;
  static constexpr int kIconN = 6;
  static constexpr int kScreenW = 240;

  static int iconX[kIconN] = {50, 90, 130, 170, 210, 10};
  static int iconY = kBarY;

  static const unsigned char* icons[kIconN] = {
      bitmap_icon_power,
      bitmap_icon_antenna,
      bitmap_icon_random,
      bitmap_icon_sort_down_minus,
      bitmap_icon_sort_up_plus,
      bitmap_icon_go_back
  };

  if (!s_uiDrawn) {
    tft.fillRect(0, kBarY, kScreenW, kBarH, DARK_GRAY);
    for (int i = 0; i < kIconN; i++) {
      if (icons[i] != NULL) {
        drawToolbarIcon(iconX[i], iconY, icons[i], UI_ICON);
      }
    }
    tft.drawFastHLine(0, 19, 240, UI_LINE);
    tft.drawFastHLine(0, kBarY + kBarH, 240, UI_LINE);
    s_uiDrawn = true;
  }

  static unsigned long lastAnimationTime = 0;
  static int animationState = 0;
  static int activeIcon = -1;

  if (animationState > 0 && millis() - lastAnimationTime >= TOOLBAR_ANIMATION_MS) {
    if (animationState == 1) {
      drawToolbarIcon(iconX[activeIcon], iconY, icons[activeIcon], UI_ICON);
      animationState = 2;
      switch (activeIcon) {
        case 0:
          startOrStop();
          break;
        case 1:
          if (!s_running) {
            s_mode = (s_mode == MODE_DEBRUIJN) ? MODE_BRUTE : MODE_DEBRUIJN;
            updateDisplay();
          }
          break;
        case 2:
          cycleFocus();
          break;
        case 3:
          adjustFocused(-1);
          break;
        case 4:
          adjustFocused(+1);
          break;
        case 5:
          feature_exit_requested = true;
          s_stopRequested = true;
          break;
      }
    } else if (animationState == 2) {
      animationState = 0;
      activeIcon = -1;
    }
    lastAnimationTime = millis();
  }

  static unsigned long lastTouchCheck = 0;
  if (millis() - lastTouchCheck >= 50) {
    int x, y;
    if (feature_active && readTouchXY(x, y)) {
      if (toolbarIconHitY(y, iconY)) {
        for (int i = 0; i < kIconN; i++) {
          if (toolbarIconHitX(x, iconX[i])) {
            if (icons[i] != NULL && animationState == 0) {
              if (i == 5) {
                feature_exit_requested = true;
                s_stopRequested = true;
              } else {
                drawToolbarIcon(iconX[i], iconY, icons[i], TFT_BLACK);
                animationState = 1;
                activeIcon = i;
                lastAnimationTime = millis();
              }
            }
            break;
          }
        }
      } else if (!s_running && y >= settingsPanelY() &&
                 y < settingsPanelY() + settingsPanelH()) {
        focusRowAtY(y);
      }
    }
    lastTouchCheck = millis();
  }
}

void subBruteSetup() {
  Serial.begin(115200);
  subghzBeginSession(SubGhzOwner::Brute);
  setTouchButtonInputEnabled(true);
  subghzSetBruteNavLabels();
  subghzClearBody(TFT_BLACK);
  drawStatusBar(readBatteryVoltage(), true);
  subghzRedrawNavChrome();

#if defined(SD_CS)
  pinMode(SD_CS, OUTPUT);
  digitalWrite(SD_CS, HIGH);
#endif
#if defined(CC1101_CS)
  pinMode(CC1101_CS, OUTPUT);
  digitalWrite(CC1101_CS, HIGH);
#endif

  ELECHOUSE_cc1101.setSpiPin(CC1101_SCK, CC1101_MISO, CC1101_MOSI, CC1101_CS);
  ELECHOUSE_cc1101.setGDO(CC1101_GDO0, CC1101_GDO2);
  subghzInitCc1101Reliable();
  ELECHOUSE_cc1101.setCCMode(0);
  ELECHOUSE_cc1101.setModulation(2);
  ELECHOUSE_cc1101.setRxBW(500.0);
  ELECHOUSE_cc1101.setPA(12);
  ELECHOUSE_cc1101.setMHZ(bruteFreqMHz());
  ELECHOUSE_cc1101.setSidle();
  pinMode(BRUTE_TX_PIN, INPUT);

  s_freqIndex = 11;
  s_bitsIndex = 2;
  s_pulseIndex = 1;
  s_protocol = 1;
  s_focus = FOCUS_FREQ;
  s_mode = MODE_DEBRUIJN;
  s_runState = ST_IDLE;
  s_running = false;
  s_stopRequested = false;
  s_progressDone = 0;
  s_progressTotal = 0;

#if HAS_PCF8574_BUTTONS
  pcf.pinMode(BTN_LEFT, INPUT_PULLUP);
  pcf.pinMode(BTN_RIGHT, INPUT_PULLUP);
  pcf.pinMode(BTN_DOWN, INPUT_PULLUP);
  pcf.pinMode(BTN_UP, INPUT_PULLUP);
#endif
  delay(100);

  subghzClearBody(TFT_BLACK);
  drawStatusBar(readBatteryVoltage(), true);
  setupTouchscreen();

  invalidateChrome();
  s_uiDrawn = false;
  updateDisplay(true);
  subghzRedrawNavChrome();
}

void subBruteLoop() {
  if (feature_active && (feature_exit_requested || featureExitButtonPressed())) {
    feature_exit_requested = true;
    s_stopRequested = true;
    return;
  }

  maintainTouchNavBar();
  runUI();
  if (s_uiDrawn) {
    tft.drawFastHLine(0, 19, 240, UI_LINE);
    tft.drawFastHLine(0, kBarBottom, 240, UI_LINE);
  }
  bruteHandleNavButtons();

#if HAS_PCF8574_BUTTONS
  const int btnLeftState = pcf.digitalRead(BTN_LEFT);
  const int btnRightState = pcf.digitalRead(BTN_RIGHT);
  const int btnUpState = pcf.digitalRead(BTN_UP);
  const int btnDownState = pcf.digitalRead(BTN_DOWN);
#else
  const int btnLeftState = isPhysicalButtonPressed(BTN_LEFT) ? LOW : HIGH;
  const int btnRightState = isPhysicalButtonPressed(BTN_RIGHT) ? LOW : HIGH;
  const int btnUpState = isPhysicalButtonPressed(BTN_UP) ? LOW : HIGH;
  const int btnDownState = isPhysicalButtonPressed(BTN_DOWN) ? LOW : HIGH;
#endif

  if (btnLeftState == LOW && millis() - s_lastDebounce > kDebounceMs) {
    adjustFocused(-1);
  }
  if (btnRightState == LOW && millis() - s_lastDebounce > kDebounceMs) {
    adjustFocused(+1);
  }
  if (btnDownState == LOW && millis() - s_lastDebounce > kDebounceMs) {
    cycleFocus();
  }
  if (btnUpState == LOW && millis() - s_lastDebounce > kDebounceMs) {
    startOrStop();
  }
}

void lifecycleShutdown() {
  s_stopRequested = true;
  s_running = false;
  s_switch.disableReceive();
  s_switch.disableTransmit();
  digitalWrite(BRUTE_TX_PIN, LOW);
  pinMode(BRUTE_TX_PIN, INPUT);
}

}  // namespace SubBrute

namespace jammingdetector {

static constexpr uint16_t JD_SAMPLES = ESP32DIV_JD_RSSI_SAMPLES;
static constexpr double JD_SAMPLE_HZ = 5000.0;
static constexpr double JD_RXBW = 270.0;
static constexpr int JD_MARGIN_DB = 12;
static constexpr int JD_ABS_THRESH_DBM = -88;
static constexpr float JD_BUSY_WIN_DUTY = 0.90f;
static constexpr uint32_t JD_JAM_STREAK_MS = 2000;
static constexpr float JD_JAM_AVG_DUTY = 0.90f;
static constexpr float JD_ACTIVITY_DUTY = 0.10f;
static constexpr uint8_t JD_RING = 20;
static constexpr int JD_FLOOR_INIT_DBM = -95;
static constexpr uint32_t JD_CALIBRATION_MS = 5000;

static const uint32_t kFreqHz[] = {433920000UL, 434420000UL, 315000000UL, 868350000UL};
static const char* kFreqLabel[] = {"433.92", "434.42", "315.00", "868.35"};
static constexpr uint8_t kFreqCount = sizeof(kFreqHz) / sizeof(kFreqHz[0]);
static uint8_t freqIdx = 1;

static unsigned int samplingPeriod = 0;

static float noiseFloor = JD_FLOOR_INIT_DBM;
static uint32_t busyStreakMs = 0;
static float dutyRing[JD_RING];
static uint8_t dutyRingPos = 0;
static bool jamActive = false;
static uint32_t jamStartMs = 0;
static int jamPeakDbm = -127;
static uint32_t eventCount = 0;
static bool calibrating = true;
static uint32_t calibrationStartMs = 0;
static float calibrationFloorSum = 0.0f;
static uint16_t calibrationWindows = 0;

static bool logEnabled = false;
static bool logMounted = false;
struct PendingJdLog {
  uint32_t whenMs;
  uint32_t durMs;
  int peakDbm;
  int dutyPct;
  uint8_t freq;
};
static constexpr uint8_t kPendingJdLogs = 16;
static PendingJdLog s_pendingLogs[kPendingJdLogs];
static uint8_t s_pendingLogCount = 0;
static bool prevLeft = false, prevRight = false, prevUp = false, prevDown = false;

static constexpr int kJdBarBottom = 36;
static constexpr int kJdSectionGap = 6;
static constexpr int kJdPad = 4;
static constexpr int kJdPanelW = 240 - (kJdPad * 2);
static constexpr int kJdRadius = 3;
static constexpr int kJdLabelX = 10;
static constexpr int kJdCol2LabelX = 128;
static constexpr int kJdValueX = 44;
static constexpr int kJdCol2ValueX = 162;
static constexpr int kJdCol1ValueW = 78;
static constexpr int kJdCol2ValueW = 70;
static constexpr int kJdLineH = 12;
static constexpr int kJdPanelHeader = 14;
static constexpr int kJdInfoY = kJdBarBottom + 4;
static constexpr int kJdInfoH = 46;
static constexpr int kJdInfoRow1Y = kJdInfoY + kJdPanelHeader + 2;
static constexpr int kJdInfoRow2Y = kJdInfoRow1Y + kJdLineH + 2;
static constexpr int kJdStatusY = kJdInfoY + kJdInfoH + kJdSectionGap;
static constexpr int kJdStatusH = 30;
static constexpr int kJdWaveY = kJdStatusY + kJdStatusH + kJdSectionGap;
static constexpr int kJdWaveHeader = 14;

static constexpr int kWaveW = ESP32DIV_JD_WAVE_WIDTH;
static constexpr int kWaveRssiMin = -100;
static constexpr int kWaveRssiMax = -35;
static int8_t waveBuf[kWaveW];
static uint16_t waveWrite = 0;
static float waveSmooth = -95.0f;

static bool s_chromeDrawn = false;
static bool s_jdUiDrawn = false;
static bool s_waveHasPrev = false;
static int16_t s_wavePrevY[kWaveW];
static uint8_t s_statusState = 255;

struct JdDisp {
  bool valid = false;
  uint8_t freqIdx = 255;
  int rssi = -999;
  int floor = -999;
  int dutyPct = -1;
  uint32_t events = 0xFFFFFFFFu;
  bool logOn = false;
};
static JdDisp s_disp;

static void cc1101BeginRx() {
  ELECHOUSE_cc1101.setSpiPin(CC1101_SCK, CC1101_MISO, CC1101_MOSI, CC1101_CS);
  subghzInitCc1101Reliable();
  ELECHOUSE_cc1101.setModulation(2);
  ELECHOUSE_cc1101.setRxBW(JD_RXBW);
  ELECHOUSE_cc1101.setGDO(CC1101_GDO0, CC1101_GDO2);
  ELECHOUSE_cc1101.setMHZ(kFreqHz[freqIdx] / 1000000.0);
  ELECHOUSE_cc1101.SetRx();
}

static void tuneTo(uint8_t idx) {
  freqIdx = idx % kFreqCount;
  ELECHOUSE_cc1101.setSidle();
  ELECHOUSE_cc1101.setMHZ(kFreqHz[freqIdx] / 1000000.0);
  ELECHOUSE_cc1101.SetRx();
  calibrating = true;
  calibrationStartMs = millis();
  calibrationFloorSum = 0.0f;
  calibrationWindows = 0;
  busyStreakMs = 0;
  jamActive = false;
  for (uint8_t i = 0; i < JD_RING; i++) dutyRing[i] = 0;
  s_disp.freqIdx = 255;
}

static void logEvent(uint32_t whenMs, uint32_t durMs, int peakDbm, int dutyPct) {
  if (!logEnabled) return;
  // Never swap CC1101 -> SD -> CC1101 inside the detector sampling loop.
  // Queue a bounded record in RAM and flush once after CC1101 shutdown.
  if (s_pendingLogCount >= kPendingJdLogs) {
    memmove(&s_pendingLogs[0], &s_pendingLogs[1],
            sizeof(PendingJdLog) * (kPendingJdLogs - 1));
    s_pendingLogCount = kPendingJdLogs - 1;
  }
  s_pendingLogs[s_pendingLogCount++] =
      {whenMs, durMs, peakDbm, dutyPct, freqIdx};
}

static void flushPendingLogs() {
  if (s_pendingLogCount == 0) return;
  SdFileIoGuard io(2000);
  if (!io.locked()) return;
  restoreSdAfterSharedSpi();
  logMounted = isSDCardAvailable();
  if (!logMounted) return;
  if (!SD.exists(LOG_DIR) && !SD.mkdir(LOG_DIR)) return;
  File f = SD.open(LOG_DIR "/jamdet.csv", FILE_APPEND);
  if (!f) return;
  for (uint8_t i = 0; i < s_pendingLogCount; ++i) {
    const PendingJdLog& e = s_pendingLogs[i];
    f.printf("%lu,%s,RF_OCCUPANCY,%d,%lu,%d\n",
             (unsigned long)e.whenMs, kFreqLabel[e.freq], e.peakDbm,
             (unsigned long)e.durMs, e.dutyPct);
  }
  f.flush();
  f.close();
  s_pendingLogCount = 0;
}

static int jdWaveBottom() {
  return subghzContentBottom() - 2;
}

static int jdPlotTop() {
  return kJdWaveY + kJdWaveHeader + 4;
}

static int jdPlotHeight() {
  const int h = jdWaveBottom() - jdPlotTop() - 2;
  return h > 8 ? h : 8;
}

static int jdRssiToY(int dbm) {
  dbm = constrain(dbm, kWaveRssiMin, kWaveRssiMax);
  const int plotH = jdPlotHeight();
  return jdPlotTop() + plotH - 2 -
         ((dbm - kWaveRssiMin) * (plotH - 4) / (kWaveRssiMax - kWaveRssiMin));
}

static void jdDrawValueCell(int x, int y, int w, const char* text, uint16_t color) {
  tft.fillRect(x, y, w, kJdLineH, TFT_BLACK);
  tft.setTextSize(1);
  tft.setTextColor(color, TFT_BLACK);
  tft.setCursor(x, y);
  tft.print(text);
}

static void jdInvalidateContent() {
  s_chromeDrawn = false;
  s_statusState = 255;
  s_disp.valid = false;
  s_waveHasPrev = false;
  waveWrite = 0;
  waveSmooth = -95.0f;
  memset(waveBuf, kWaveRssiMin, sizeof(waveBuf));
}

static void jdInvalidateAll() {
  jdInvalidateContent();
  s_jdUiDrawn = false;
}

static void jdResetStats() {
  eventCount = 0;
  busyStreakMs = 0;
  jamActive = false;
  noiseFloor = JD_FLOOR_INIT_DBM;
  calibrating = true;
  calibrationStartMs = millis();
  calibrationFloorSum = 0.0f;
  calibrationWindows = 0;
  for (uint8_t i = 0; i < JD_RING; i++) dutyRing[i] = 0;
  jdInvalidateContent();
}

static void jdRunUI() {
  static constexpr int kBarY = 20;
  static constexpr int kBarH = 20;
  static constexpr int kIconSz = 16;
  static constexpr int kIconN = 5;
  static constexpr int kBackIdx = 4;

  static int iconX[kIconN] = {90, 130, 170, 210, 10};
  static int iconY = kBarY;

  static const unsigned char* icons[kIconN] = {
      bitmap_icon_sort_down_minus,
      bitmap_icon_floppy,
      bitmap_icon_undo,
      bitmap_icon_sort_up_plus,
      bitmap_icon_go_back,
  };

  if (!s_jdUiDrawn) {
    tft.fillRect(0, kBarY, 240, kBarH, DARK_GRAY);
    for (int i = 0; i < kIconN; i++) {
      drawToolbarIcon(iconX[i], iconY, icons[i], UI_ICON);
    }
    tft.drawFastHLine(0, 19, 240, UI_LINE);
    tft.drawFastHLine(0, kBarY + kBarH, 240, UI_LINE);
    s_jdUiDrawn = true;
  }

  static unsigned long lastAnimationTime = 0;
  static int animationState = 0;
  static int activeIcon = -1;

  if (animationState > 0 && millis() - lastAnimationTime >= TOOLBAR_ANIMATION_MS) {
    if (animationState == 1) {
      if (activeIcon >= 0 && activeIcon != kBackIdx) {
        drawToolbarIcon(iconX[activeIcon], iconY, icons[activeIcon], UI_ICON);
      }
      animationState = 2;
      switch (activeIcon) {
        case 0:
          tuneTo(freqIdx + kFreqCount - 1);
          s_disp.freqIdx = 255;
          break;
        case 1:
          logEnabled = !logEnabled;
          s_disp.logOn = !logEnabled;
          break;
        case 2:
          jdResetStats();
          break;
        case 3:
          tuneTo(freqIdx + 1);
          s_disp.freqIdx = 255;
          break;
        case kBackIdx:
          feature_exit_requested = true;
          break;
        default:
          break;
      }
    } else if (animationState == 2) {
      animationState = 0;
      activeIcon = -1;
    }
    lastAnimationTime = millis();
  }

  static unsigned long lastTouchCheck = 0;
  if (millis() - lastTouchCheck >= 50) {
    int x, y;
    if (feature_active && readTouchXY(x, y)) {
      if (toolbarIconHitY(y, iconY)) {
        for (int i = 0; i < kIconN; i++) {
          if (toolbarIconHitX(x, iconX[i])) {
            if (animationState == 0) {
              if (i == kBackIdx) {
                feature_exit_requested = true;
              } else {
                tft.fillRect(iconX[i], iconY, kIconSz, kIconSz, DARK_GRAY);
                animationState = 1;
                activeIcon = i;
                lastAnimationTime = millis();
              }
            }
            break;
          }
        }
      }
    }
    lastTouchCheck = millis();
  }
}

static void jdDrawPanelFrame(int y, int h, const char* title) {
  tft.drawRoundRect(kJdPad, y, kJdPanelW, h, kJdRadius, UI_LINE);
  tft.setTextSize(1);
  tft.setTextColor(UI_DIM_TEXT, TFT_BLACK);
  tft.setCursor(kJdLabelX, y + 3);
  tft.print(title);
}

static void jdDrawPlotGridLines() {
  const int plotX = kJdPad + 4;
  const int plotW = kJdPanelW - 8;
  const int plotTop = jdPlotTop();
  const int plotH = jdPlotHeight();
  if (plotH < 8) {
    return;
  }

  for (int g = 1; g < 4; g++) {
    const int gy = plotTop + (plotH * g) / 4;
    tft.drawFastHLine(plotX + 1, gy, plotW - 2, DARK_GRAY);
  }
  for (int g = 1; g < 4; g++) {
    const int gx = plotX + (plotW * g) / 4;
    tft.drawFastVLine(gx, plotTop + 1, plotH - 2, DARK_GRAY);
  }
}

static void jdDrawPlotBackground() {
  const int plotX = kJdPad + 4;
  const int plotW = kJdPanelW - 8;
  const int plotTop = jdPlotTop();
  const int plotH = jdPlotHeight();
  if (plotH < 8) {
    return;
  }

  tft.fillRect(plotX, plotTop, plotW, plotH, TFT_BLACK);
  jdDrawPlotGridLines();
}

static void jdDrawStaticChrome() {
  if (s_chromeDrawn) {
    return;
  }

  const int bodyBottom = jdWaveBottom();
  tft.fillRect(0, kJdBarBottom + 1, 240, bodyBottom - kJdBarBottom - 1, TFT_BLACK);

  jdDrawPanelFrame(kJdInfoY, kJdInfoH, "Monitor");
  tft.setTextSize(1);
  tft.setTextColor(UI_DIM_TEXT, TFT_BLACK);
  tft.setCursor(kJdLabelX, kJdInfoRow1Y);
  tft.print("Freq");
  tft.setCursor(kJdLabelX, kJdInfoRow2Y);
  tft.print("RSSI");
  tft.setCursor(kJdCol2LabelX, kJdInfoRow1Y);
  tft.print("Duty");
  tft.setCursor(kJdCol2LabelX, kJdInfoRow2Y);
  tft.print("Log");

  const int waveH = bodyBottom - kJdWaveY;
  if (waveH > kJdWaveHeader + 12) {
    jdDrawPanelFrame(kJdWaveY, waveH, "Signal");
    jdDrawPlotBackground();
  }

  s_chromeDrawn = true;
  s_disp.valid = false;
  s_statusState = 255;
}

static void jdDrawStatusBox(bool jam, bool activity, bool isCalibrating = false) {
  const uint8_t st = isCalibrating ? 3 : (jam ? 2 : (activity ? 1 : 0));
  if (st == s_statusState && s_disp.valid) {
    return;
  }
  s_statusState = st;

  uint16_t bg = isCalibrating ? UI_FG : (jam ? ORANGE : (activity ? UI_WARN : UI_OK));
  uint16_t fg = (jam || activity) ? TFT_BLACK : UI_TEXT;
  const char* s = isCalibrating ? "CALIBRATING" :
                  (jam ? "PERSISTENT RF" : (activity ? "BURST ACTIVITY" : "CLEAR"));

  tft.fillRoundRect(kJdPad, kJdStatusY, kJdPanelW, kJdStatusH, kJdRadius, bg);
  tft.drawRoundRect(kJdPad, kJdStatusY, kJdPanelW, kJdStatusH, kJdRadius, UI_LINE);
  tft.setTextColor(fg, bg);
  uint8_t sz = 2;
  tft.setTextSize(sz);
  if (tft.textWidth(s) > kJdPanelW - 8) { sz = 1; tft.setTextSize(sz); }
  const int16_t tw = tft.textWidth(s);
  const int16_t th = 8 * sz;
  tft.setCursor(kJdPad + (kJdPanelW - tw) / 2, kJdStatusY + (kJdStatusH - th) / 2);
  tft.print(s);
  tft.setTextSize(1);
}

static void jdUpdateInfo(int rssiNow, int dutyPct) {
  jdDrawStaticChrome();

  const bool full = !s_disp.valid;
  char buf[28];

  if (full || s_disp.freqIdx != freqIdx) {
    snprintf(buf, sizeof(buf), "%s MHz", kFreqLabel[freqIdx]);
    jdDrawValueCell(kJdValueX, kJdInfoRow1Y, kJdCol1ValueW, buf, UI_TEXT);
    s_disp.freqIdx = freqIdx;
  }

  if (full || abs(s_disp.rssi - rssiNow) >= 2 ||
      abs(s_disp.floor - (int)noiseFloor) >= 2) {
    snprintf(buf, sizeof(buf), "%d/%d dBm", rssiNow, (int)noiseFloor);
    jdDrawValueCell(kJdValueX, kJdInfoRow2Y, kJdCol1ValueW, buf, UI_TEXT);
    s_disp.rssi = rssiNow;
    s_disp.floor = (int)noiseFloor;
  }

  if (full || abs(s_disp.dutyPct - dutyPct) >= 5 || s_disp.events != eventCount) {
    snprintf(buf, sizeof(buf), "%d%% E:%lu", dutyPct, (unsigned long)eventCount);
    jdDrawValueCell(kJdCol2ValueX, kJdInfoRow1Y, kJdCol2ValueW, buf, UI_TEXT);
    s_disp.dutyPct = dutyPct;
    s_disp.events = eventCount;
  }

  if (full || s_disp.logOn != logEnabled) {
    jdDrawValueCell(kJdCol2ValueX, kJdInfoRow2Y, kJdCol2ValueW,
                    logEnabled ? "on" : "off", logEnabled ? UI_OK : UI_DIM_TEXT);
    s_disp.logOn = logEnabled;
  }

  s_disp.valid = true;
}

static void jdDrawWaveform(bool jam, bool activity) {
  const int plotX = kJdPad + 4;
  const int plotW = kJdPanelW - 8;
  if (jdPlotHeight() < 8 || plotW < 2 || kWaveW < 2) {
    return;
  }

  const uint16_t waveColor = jam ? ORANGE : (activity ? UI_WARN : UI_OK);

  if (s_waveHasPrev) {
    for (int x = 0; x < plotW - 1; x++) {
      const int px0 = (x * (kWaveW - 1)) / (plotW - 1);
      const int px1 = ((x + 1) * (kWaveW - 1)) / (plotW - 1);
      tft.drawLine(plotX + x, s_wavePrevY[px0], plotX + x + 1, s_wavePrevY[px1], TFT_BLACK);
    }
  }

  for (int i = 0; i < kWaveW; i++) {
    s_wavePrevY[i] = jdRssiToY(waveBuf[(waveWrite + i) % kWaveW]);
  }

  for (int x = 0; x < plotW - 1; x++) {
    const int i0 = (x * (kWaveW - 1)) / (plotW - 1);
    const int i1 = ((x + 1) * (kWaveW - 1)) / (plotW - 1);
    tft.drawLine(plotX + x, s_wavePrevY[i0], plotX + x + 1, s_wavePrevY[i1], waveColor);
  }

  s_waveHasPrev = true;
  jdDrawPlotGridLines();
}

struct WindowStat {
  int peakDbm;
  int minDbm;
  float avgDbm;
  float duty;
  uint32_t elapsedMs;
};

static WindowStat sampleWindow() {
  const int busyThresh = max(JD_ABS_THRESH_DBM, (int)(noiseFloor + JD_MARGIN_DB));
  int peak = -127, lo = 0;
  int32_t sum = 0;
  uint16_t busy = 0;
  const float kEwmaAlpha = 0.35f;

  const uint32_t t0 = millis();
  uint32_t micro_s = micros();
  for (int i = 0; i < JD_SAMPLES; i++) {
    const int dbm = ELECHOUSE_cc1101.getRssi();
    if (dbm > peak) peak = dbm;
    if (dbm < lo) lo = dbm;
    sum += dbm;
    if (dbm > busyThresh) busy++;

    waveSmooth = (kEwmaAlpha * dbm) + ((1.0f - kEwmaAlpha) * waveSmooth);
    if ((i & 1) == 0) {
      waveBuf[waveWrite] = (int8_t)constrain((int)lroundf(waveSmooth), kWaveRssiMin, kWaveRssiMax);
      waveWrite = (waveWrite + 1) % kWaveW;
    }

    while (micros() < micro_s + samplingPeriod) {}
    micro_s += samplingPeriod;
  }

  WindowStat st;
  st.peakDbm = peak;
  st.minDbm = lo;
  st.avgDbm = (float)sum / (float)JD_SAMPLES;
  st.duty = (float)busy / JD_SAMPLES;
  st.elapsedMs = millis() - t0;
  return st;
}

static void evaluate(const WindowStat& st) {
  if (st.duty < 0.2f) noiseFloor = 0.95f * noiseFloor + 0.05f * st.minDbm;

  dutyRing[dutyRingPos] = st.duty;
  dutyRingPos = (dutyRingPos + 1) % JD_RING;
  float avgDuty = 0;
  for (uint8_t i = 0; i < JD_RING; i++) avgDuty += dutyRing[i];
  avgDuty /= JD_RING;

  if (st.duty >= JD_BUSY_WIN_DUTY) busyStreakMs += st.elapsedMs;
  else busyStreakMs = 0;

  const bool jam = (busyStreakMs >= JD_JAM_STREAK_MS) || (avgDuty >= JD_JAM_AVG_DUTY);

  if (jam && !jamActive) {
    jamActive = true;
    jamStartMs = millis();
    jamPeakDbm = st.peakDbm;
    eventCount++;
    s_disp.events = 0xFFFFFFFFu;
  } else if (jam && jamActive) {
    if (st.peakDbm > jamPeakDbm) jamPeakDbm = st.peakDbm;
  } else if (!jam && jamActive) {
    jamActive = false;
    logEvent(jamStartMs, millis() - jamStartMs, jamPeakDbm, (int)(avgDuty * 100));
  }
}

static bool edge(int pin, bool& prev) {
  const bool now = isPhysicalButtonPressed(pin);
  const bool e = now && !prev;
  prev = now;
  return e;
}

static void handleInput() {
  const bool navFreqDown = featureHasTouchNavBar() && isTouchNavButtonPressedEdge(BTN_LEFT);
  const bool navFreqUp = featureHasTouchNavBar() && isTouchNavButtonPressedEdge(BTN_RIGHT);
  const bool navReset = featureHasTouchNavBar() && isTouchNavButtonPressedEdge(BTN_UP);
  const bool navLog = featureHasTouchNavBar() && isTouchNavButtonPressedEdge(BTN_DOWN);

  if (edge(BTN_LEFT, prevLeft) || navFreqDown) tuneTo(freqIdx + kFreqCount - 1);
  if (edge(BTN_RIGHT, prevRight) || navFreqUp) tuneTo(freqIdx + 1);
  if (edge(BTN_UP, prevUp) || navReset) {
    jdResetStats();
  }
  if (edge(BTN_DOWN, prevDown) || navLog) {
    logEnabled = !logEnabled;
    s_disp.logOn = !logEnabled;
  }
}

static void exitCleanup() {
  SubGhzLifecycle::shutdown(true);
}

void Setup() {
  subghzBeginSession(SubGhzOwner::Detector);
  setTouchButtonInputEnabled(true);
  setTouchNavLabels("Freq-", "Log", "Exit", "Reset", "Freq+");

#if defined(SD_CS)
  pinMode(SD_CS, OUTPUT);
  digitalWrite(SD_CS, HIGH);
#endif
#if defined(CC1101_CS)
  pinMode(CC1101_CS, OUTPUT);
  digitalWrite(CC1101_CS, HIGH);
#endif

  cc1101BeginRx();
  tuneTo(freqIdx);

  samplingPeriod = round(1000000.0 * (1.0 / JD_SAMPLE_HZ));

  noiseFloor = JD_FLOOR_INIT_DBM;
  busyStreakMs = 0;
  jamActive = false;
  logMounted = false;
  for (uint8_t i = 0; i < JD_RING; i++) dutyRing[i] = 0;
  prevLeft = prevRight = prevUp = prevDown = false;
  jdInvalidateAll();

#if HAS_PCF8574_BUTTONS
  pcf.pinMode(BTN_LEFT, INPUT_PULLUP);
  pcf.pinMode(BTN_RIGHT, INPUT_PULLUP);
  pcf.pinMode(BTN_UP, INPUT_PULLUP);
  pcf.pinMode(BTN_DOWN, INPUT_PULLUP);
  pcf.pinMode(BTN_SELECT, INPUT_PULLUP);
#endif

  tft.setRotation(TFT_ROTATION);
  subghzClearBody(TFT_BLACK);
  drawStatusBar(readBatteryVoltage(), true);
  subghzRedrawNavChrome();
  setupTouchscreen();
  jdRunUI();
  jdDrawStaticChrome();
  jdDrawStatusBox(false, false, true);
}

void Loop() {
  if (feature_active && (feature_exit_requested || featureExitButtonPressed())) {
    exitCleanup();
    feature_exit_requested = true;
    return;
  }

  maintainTouchNavBar();
  jdRunUI();
  handleInput();

  const WindowStat st = sampleWindow();
  if (calibrating) {
    calibrationFloorSum += st.avgDbm;
    calibrationWindows++;
    noiseFloor = calibrationFloorSum / max(1u, (unsigned)calibrationWindows);
    if ((uint32_t)(millis() - calibrationStartMs) >= JD_CALIBRATION_MS) {
      calibrating = false;
      busyStreakMs = 0;
      for (uint8_t i = 0; i < JD_RING; i++) dutyRing[i] = 0;
    }
  } else {
    evaluate(st);
  }

  const bool activity = !calibrating && !jamActive && (st.duty >= JD_ACTIVITY_DUTY);
  const int dutyPct = (int)(dutyRing[(dutyRingPos + JD_RING - 1) % JD_RING] * 100.0f);

  jdUpdateInfo(st.peakDbm, dutyPct);
  jdDrawStatusBox(jamActive, activity, calibrating);
  jdDrawWaveform(jamActive, activity);

  static uint32_t s_lastLedMs = 0;
  const uint32_t now = millis();
  if ((uint32_t)(now - s_lastLedMs) >= 150u) {
    if (jamActive) {
      static bool phase = false;
      phase = !phase;
      neopixelShowActivityColor(255, 0, 90, phase);
    } else {
      neopixelShowProgressColor((uint8_t)constrain(dutyPct, 0, 100), 180, 0, 200);
    }
    s_lastLedMs = now;
  }
}

void lifecycleShutdown() {
  logMounted = false;
  jamActive = false;
  calibrating = false;
  busyStreakMs = 0;
}

void lifecycleFlushLogs() {
  flushPendingLogs();
}

}  // namespace jammingdetector

namespace SubGhzLifecycle {

void shutdown(bool restoreSd) {
  // Every SubGHz entry point shares GPIO/SPI/RMT resources.  Teardown must be
  // idempotent because physical and touch exits take different menu paths.
  // Stop asynchronous feedback first: otherwise its moving head/tail frame
  // can overwrite the battery indication after this function has returned,
  // leaving exactly two LEDs apparently stuck.
  featureFeedbackStop();
  soundStop();
  const bool hadOwner = s_subghzLifecycleActive ||
                        s_subghzOwner != SubGhzOwner::None;

  // Put the radio idle while its SPI binding is still known-good. Owner
  // cleanup may change GDO pin directions, and SD restoration remaps SPI.
  if (s_subghzLifecycleActive) {
    ELECHOUSE_cc1101.setSidle();
  }
  const SubGhzOwner closingOwner = s_subghzOwner;
  switch (closingOwner) {
    case SubGhzOwner::Replay:
      replayat::lifecycleShutdown();
      break;
    case SubGhzOwner::SavedProfiles:
      SavedProfile::lifecycleShutdown();
      break;
    case SubGhzOwner::Jammer:
      subjammer::lifecycleShutdown();
      break;
    case SubGhzOwner::Brute:
      SubBrute::lifecycleShutdown();
      break;
    case SubGhzOwner::Detector:
      jammingdetector::lifecycleShutdown();
      break;
    case SubGhzOwner::None:
      break;
  }

  s_subghzLifecycleActive = false;
  s_subghzOwner = SubGhzOwner::None;

#if defined(CC1101_CS)
  pinMode(CC1101_CS, OUTPUT);
  digitalWrite(CC1101_CS, HIGH);
#endif
#if defined(SD_CS)
  pinMode(SD_CS, OUTPUT);
  digitalWrite(SD_CS, HIGH);
#endif
  pinMode(SUBGHZ_TX_PIN, INPUT);
  pinMode(SUBGHZ_RX_PIN, INPUT);

  if (restoreSd && closingOwner == SubGhzOwner::Detector) {
    jammingdetector::lifecycleFlushLogs();
  }

  if (restoreSd && hadOwner) {
    // A feature exit is not a storage operation.  Mounting the card here made
    // the UI wait inside SD.begin() and could trip the task watchdog after a
    // previous Wi-Fi/nRF owner left the shared bus in a marginal state.  Leave
    // every slave electrically idle and let SD File Manager/Wardriver mount on
    // demand instead.
    releaseSharedSpiWithoutSdMount();
    // Several callers repaint the submenu before invoking this common
    // teardown. Transition directly from the feature colour to the configured
    // battery indication: clearing first creates a visible off/on flash.
    neopixelShowAccentIdle();
  } else if (!settings().neopixelEnabled) {
    neopixelClear();
  } else if (restoreSd) {
    // A duplicated exit request must not remount/reset the shared bus again.
    // Only restore the idle visual state.
    neopixelShowAccentIdle();
  }
}

}  // namespace SubGhzLifecycle
