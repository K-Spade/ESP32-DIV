#include "PinLock.h"

#include <Arduino.h>
#include <Preferences.h>
#include <esp_system.h>
#include <mbedtls/sha256.h>

#include "Language.h"
#include "SettingsStore.h"
#include "Touchscreen.h"
#include "config.h"
#include "shared.h"
#include "utils.h"

namespace {

constexpr const char* PIN_NAMESPACE = "mk-pin-lock";
constexpr const char* PIN_KEY = "record";
constexpr uint32_t PIN_MAGIC = 0x4D4B5031u;  // "MKP1"
constexpr uint8_t PIN_MIN_LENGTH = 4;
constexpr uint8_t PIN_MAX_LENGTH = 8;

constexpr int KEY_COLS = 3;
constexpr int KEY_ROWS = 4;
constexpr int KEY_X = 16;
constexpr int KEY_Y = 104;
constexpr int KEY_W = 64;
constexpr int KEY_H = 44;
constexpr int KEY_GAP_X = 8;
constexpr int KEY_GAP_Y = 5;

struct __attribute__((packed)) PinRecord {
  uint32_t magic;
  uint8_t length;
  uint8_t salt[8];
  uint8_t digest[32];
  uint32_t checksum;
};

struct PinEntry {
  char digits[PIN_MAX_LENGTH + 1] = {0};
  uint8_t length = 0;
};

enum class EntryResult : uint8_t { Confirmed, Cancelled };

static uint32_t recordChecksum(const PinRecord& record) {
  uint32_t hash = 2166136261u;
  const uint8_t* bytes = reinterpret_cast<const uint8_t*>(&record);
  const size_t length = offsetof(PinRecord, checksum);
  for (size_t i = 0; i < length; ++i) {
    hash ^= bytes[i];
    hash *= 16777619u;
  }
  return hash;
}

static void digestPin(const char* pin, uint8_t length, const uint8_t salt[8],
                      uint8_t output[32]) {
  uint8_t input[8 + PIN_MAX_LENGTH] = {0};
  memcpy(input, salt, 8);
  memcpy(input + 8, pin, length);
  mbedtls_sha256_ret(input, 8u + length, output, 0);
  memset(input, 0, sizeof(input));
}

static bool loadRecord(PinRecord& record) {
  memset(&record, 0, sizeof(record));
  Preferences prefs;
  if (!prefs.begin(PIN_NAMESPACE, true)) return false;
  const size_t got = prefs.getBytes(PIN_KEY, &record, sizeof(record));
  prefs.end();
  return got == sizeof(record) && record.magic == PIN_MAGIC &&
         record.length >= PIN_MIN_LENGTH && record.length <= PIN_MAX_LENGTH &&
         record.checksum == recordChecksum(record);
}

static bool saveRecord(const PinEntry& entry) {
  if (entry.length < PIN_MIN_LENGTH || entry.length > PIN_MAX_LENGTH) return false;
  PinRecord record{};
  record.magic = PIN_MAGIC;
  record.length = entry.length;
  esp_fill_random(record.salt, sizeof(record.salt));
  digestPin(entry.digits, entry.length, record.salt, record.digest);
  record.checksum = recordChecksum(record);

  Preferences prefs;
  if (!prefs.begin(PIN_NAMESPACE, false)) return false;
  const size_t written = prefs.putBytes(PIN_KEY, &record, sizeof(record));
  prefs.end();
  memset(&record, 0, sizeof(record));
  return written == sizeof(PinRecord);
}

static bool eraseRecord() {
  Preferences prefs;
  if (!prefs.begin(PIN_NAMESPACE, false)) return false;
  const bool existed = prefs.isKey(PIN_KEY);
  const bool removed = !existed || prefs.remove(PIN_KEY);
  prefs.end();
  return removed;
}

static void eraseLegacyNfcLock() {
  Preferences prefs;
  if (!prefs.begin("mk-nfc-lock", false)) return;
  if (prefs.isKey("uid")) prefs.remove("uid");
  prefs.end();
}

static bool pinMatches(const PinEntry& entry, const PinRecord& record) {
  if (entry.length != record.length) return false;
  uint8_t digest[32] = {0};
  digestPin(entry.digits, entry.length, record.salt, digest);
  uint8_t difference = 0;
  for (size_t i = 0; i < sizeof(digest); ++i) {
    difference |= digest[i] ^ record.digest[i];
  }
  memset(digest, 0, sizeof(digest));
  return difference == 0;
}

static const char* keyLabel(uint8_t index) {
  static const char* const labels[KEY_ROWS * KEY_COLS] = {
      "1", "2", "3", "4", "5", "6", "7", "8", "9", "<", "0", "OK"};
  return index < KEY_ROWS * KEY_COLS ? labels[index] : "";
}

static int keyDigit(uint8_t index) {
  if (index <= 8u) return (int)index + 1;
  if (index == 10u) return 0;
  return -1;
}

enum class KeyVisual : uint8_t { Normal, Focused, Pressed };

static void drawKey(uint8_t index, KeyVisual visual) {
  const int row = index / KEY_COLS;
  const int col = index % KEY_COLS;
  const int x = KEY_X + col * (KEY_W + KEY_GAP_X);
  const int y = KEY_Y + row * (KEY_H + KEY_GAP_Y);
  const uint16_t accent = uiUniversalColor();
  const bool focused = visual == KeyVisual::Focused;
  const bool pressed = visual == KeyVisual::Pressed;
  const int shift = pressed ? 2 : 0;

  // Clear the complete plate (including its drop shadow) before changing
  // state. This prevents the old focused/pressed outline from remaining lit.
  tft.fillRect(x, y, KEY_W + 5, KEY_H + 5, UI_BG);
  drawArmoredMenuPanel(x, y, KEY_W, KEY_H, focused || pressed, pressed);
  if (pressed) {
    tft.fillRoundRect(x + shift + 4, y + shift + 4, KEY_W - 8,
                      KEY_H - 8, 3, accent);
  }

  const uint16_t fill = pressed ? accent : UI_FG;
  const uint16_t text = pressed ? TFT_BLACK : (focused ? accent : UI_TEXT);
  tft.setTextDatum(MC_DATUM);
  tft.setTextFont(index == 11u ? 2 : 4);
  tft.setTextColor(text, fill);
  tft.drawString(keyLabel(index), x + shift + KEY_W / 2,
                 y + shift + KEY_H / 2 + 1);
  tft.setTextDatum(TL_DATUM);
}

static void pulseKey(uint8_t index, bool restoreFocus) {
  drawKey(index, KeyVisual::Pressed);
  delay(70);
  drawKey(index, restoreFocus ? KeyVisual::Focused : KeyVisual::Normal);
}

static void drawEntryDots(const PinEntry& entry, uint16_t color) {
  tft.fillRect(12, 72, TFT_WIDTH - 24, 25, UI_BG);
  const int spacing = 20;
  const int total = PIN_MAX_LENGTH * spacing;
  const int startX = (TFT_WIDTH - total) / 2 + spacing / 2;
  for (uint8_t i = 0; i < PIN_MAX_LENGTH; ++i) {
    const int x = startX + i * spacing;
    if (i < entry.length) {
      tft.fillCircle(x, 84, 5, color);
    } else {
      tft.drawCircle(x, 84, 4, UI_DIM_TEXT);
    }
  }
}

static void drawStatus(const char* status, uint16_t color) {
  tft.fillRect(8, 45, TFT_WIDTH - 16, 22, UI_BG);
  tft.setTextDatum(MC_DATUM);
  tft.setTextFont(1);
  tft.setTextColor(color, UI_BG);
  tft.drawString(status ? status : "", TFT_WIDTH / 2, 56);
  tft.setTextDatum(TL_DATUM);
}

static void drawPinPad(const char* title, const char* status, const PinEntry& entry,
                       int8_t focus, bool allowCancel) {
  drawGhostFleetWallpaper();
  drawArmoredMenuPanel(5, 3, TFT_WIDTH - 10, 34, false, false);
  tft.setTextDatum(MC_DATUM);
  tft.setTextFont(2);
  tft.setTextColor(UI_TEXT, UI_FG);
  tft.drawString(title, TFT_WIDTH / 2, 20);
  if (allowCancel) {
    tft.setTextDatum(ML_DATUM);
    tft.setTextColor(UI_WARN, UI_FG);
    tft.drawString("X", 14, 20);
  }
  tft.setTextDatum(TL_DATUM);
  tft.fillRoundRect(8, 43, TFT_WIDTH - 16, 57, 6, UI_BG);
  tft.drawRoundRect(8, 43, TFT_WIDTH - 16, 57, 6, UI_LINE);
  drawStatus(status, UI_DIM_TEXT);
  drawEntryDots(entry, uiUniversalColor());
  for (uint8_t i = 0; i < KEY_ROWS * KEY_COLS; ++i) {
    drawKey(i, focus >= 0 && i == (uint8_t)focus
                   ? KeyVisual::Focused
                   : KeyVisual::Normal);
  }
}

static int touchedKey(int x, int y) {
  for (int index = 0; index < KEY_ROWS * KEY_COLS; ++index) {
    const int row = index / KEY_COLS;
    const int col = index % KEY_COLS;
    const int bx = KEY_X + col * (KEY_W + KEY_GAP_X);
    const int by = KEY_Y + row * (KEY_H + KEY_GAP_Y);
    if (x >= bx && x < bx + KEY_W && y >= by && y < by + KEY_H) return index;
  }
  return -1;
}

static void clearEntry(PinEntry& entry) {
  memset(entry.digits, 0, sizeof(entry.digits));
  entry.length = 0;
}

static EntryResult collectPin(const char* title, const char* initialStatus,
                              PinEntry& entry, bool allowCancel,
                              const PinRecord* autoVerify) {
  clearEntry(entry);
  int8_t focus = -1;
  const bool usePhysicalButtons = allowCancel;
  bool touchHeld = isTouchDownDismiss();
  bool prevUp = usePhysicalButtons && isPhysicalButtonPressed(BTN_UP);
  bool prevDown = usePhysicalButtons && isPhysicalButtonPressed(BTN_DOWN);
  bool prevLeft = usePhysicalButtons && isPhysicalButtonPressed(BTN_LEFT);
  bool prevRight = usePhysicalButtons && isPhysicalButtonPressed(BTN_RIGHT);
  bool prevSelect = usePhysicalButtons && isPhysicalButtonPressed(BTN_SELECT);
  drawPinPad(title, initialStatus, entry, focus, allowCancel);

  for (;;) {
    int selected = -1;
    int tx = 0, ty = 0;
    const bool touching = readTouchXYDismiss(tx, ty);
    if (touching && !touchHeld) {
      if (allowCancel && ty < 40 && tx < 48) return EntryResult::Cancelled;
      selected = touchedKey(tx, ty);
      if (selected >= 0) {
        if (focus >= 0) drawKey((uint8_t)focus, KeyVisual::Normal);
        focus = -1;
        pulseKey((uint8_t)selected, false);
      }
    }
    touchHeld = touching;

    const bool up = usePhysicalButtons && isPhysicalButtonPressed(BTN_UP);
    const bool down = usePhysicalButtons && isPhysicalButtonPressed(BTN_DOWN);
    const bool left = usePhysicalButtons && isPhysicalButtonPressed(BTN_LEFT);
    const bool right = usePhysicalButtons && isPhysicalButtonPressed(BTN_RIGHT);
    const bool select = usePhysicalButtons && isPhysicalButtonPressed(BTN_SELECT);
    if (up && !prevUp) {
      if (focus >= 0) drawKey((uint8_t)focus, KeyVisual::Normal);
      focus = focus < 0 ? 0 :
          (int8_t)((focus + KEY_ROWS * KEY_COLS - KEY_COLS) % (KEY_ROWS * KEY_COLS));
      drawKey((uint8_t)focus, KeyVisual::Focused);
    } else if (down && !prevDown) {
      if (focus >= 0) drawKey((uint8_t)focus, KeyVisual::Normal);
      focus = focus < 0 ? 0 :
          (int8_t)((focus + KEY_COLS) % (KEY_ROWS * KEY_COLS));
      drawKey((uint8_t)focus, KeyVisual::Focused);
    } else if (left && !prevLeft) {
      if (focus >= 0) drawKey((uint8_t)focus, KeyVisual::Normal);
      focus = focus < 0 ? 0 :
          (int8_t)((focus + KEY_ROWS * KEY_COLS - 1) % (KEY_ROWS * KEY_COLS));
      drawKey((uint8_t)focus, KeyVisual::Focused);
    } else if (right && !prevRight) {
      if (focus >= 0) drawKey((uint8_t)focus, KeyVisual::Normal);
      focus = focus < 0 ? 0 :
          (int8_t)((focus + 1) % (KEY_ROWS * KEY_COLS));
      drawKey((uint8_t)focus, KeyVisual::Focused);
    } else if (select && !prevSelect && focus >= 0) {
      selected = (uint8_t)focus;
      pulseKey((uint8_t)focus, true);
    }
    prevUp = up;
    prevDown = down;
    prevLeft = left;
    prevRight = right;
    prevSelect = select;

    if (selected >= 0) {
      const int digit = keyDigit((uint8_t)selected);
      if (digit >= 0 && entry.length < PIN_MAX_LENGTH) {
        entry.digits[entry.length++] = (char)('0' + digit);
        entry.digits[entry.length] = '\0';
        drawEntryDots(entry, uiUniversalColor());
        if (autoVerify && entry.length == autoVerify->length) {
          if (pinMatches(entry, *autoVerify)) return EntryResult::Confirmed;
          drawStatus(tr("Incorrect PIN"), UI_WARN);
          delay(350);
          clearEntry(entry);
          drawEntryDots(entry, uiUniversalColor());
          drawStatus(tr("Enter PIN"), UI_DIM_TEXT);
        }
      } else if (selected == 9) {
        if (entry.length > 0) {
          entry.digits[--entry.length] = '\0';
          drawEntryDots(entry, uiUniversalColor());
        } else if (allowCancel) {
          return EntryResult::Cancelled;
        }
      } else if (selected == 11) {
        if (autoVerify) {
          if (entry.length == autoVerify->length && pinMatches(entry, *autoVerify)) {
            return EntryResult::Confirmed;
          }
          drawStatus(tr("Incorrect PIN"), UI_WARN);
          delay(350);
          clearEntry(entry);
          drawEntryDots(entry, uiUniversalColor());
          drawStatus(tr("Enter PIN"), UI_DIM_TEXT);
        } else if (entry.length >= PIN_MIN_LENGTH) {
          return EntryResult::Confirmed;
        } else {
          drawStatus(tr("PIN must contain 4-8 digits"), UI_WARN);
        }
      }
    }
    delay(8);
  }
}

static void drawResult(const char* title, const char* message, uint16_t color) {
  tft.fillScreen(UI_BG);
  tft.fillRoundRect(18, 72, TFT_WIDTH - 36, 152, 10, UI_FG);
  tft.drawRoundRect(18, 72, TFT_WIDTH - 36, 152, 10, color);
  tft.fillCircle(TFT_WIDTH / 2, 118, 25, color);
  tft.setTextDatum(MC_DATUM);
  tft.setTextFont(4);
  tft.setTextColor(UI_BG, color);
  tft.drawString(color == UI_OK ? "OK" : "!", TFT_WIDTH / 2, 118);
  tft.setTextFont(2);
  tft.setTextColor(color, UI_FG);
  tft.drawString(title, TFT_WIDTH / 2, 166);
  tft.setTextFont(1);
  tft.setTextColor(UI_TEXT, UI_FG);
  tft.drawString(message, TFT_WIDTH / 2, 195);
  tft.setTextDatum(TL_DATUM);
}

static bool waitManageAction(bool currentlyEnabled) {
  drawGhostFleetWallpaper();
  drawArmoredMenuPanel(5, 3, TFT_WIDTH - 10, 38, false, false);
  tft.setTextDatum(MC_DATUM);
  tft.setTextFont(2);
  tft.setTextColor(UI_TEXT, UI_FG);
  tft.drawString(tr("PIN Device Lock"), TFT_WIDTH / 2, 22);
  drawArmoredMenuPanel(18, 68, TFT_WIDTH - 36, 72, false, false);
  tft.setTextFont(2);
  tft.setTextColor(currentlyEnabled ? UI_OK : UI_DIM_TEXT, UI_FG);
  tft.drawString(currentlyEnabled ? tr("Lock is enabled") : tr("Lock is disabled"),
                 TFT_WIDTH / 2, 104);

  const int y = 220;
  drawArmoredMenuPanel(14, y, 96, 52, false, false);
  drawArmoredMenuPanel(130, y, 96, 52, true, false);
  tft.setTextFont(2);
  tft.setTextColor(UI_TEXT, UI_FG);
  tft.drawString(tr("Exit"), 62, y + 26);
  tft.setTextColor(uiUniversalColor(), UI_FG);
  tft.drawString(currentlyEnabled ? tr("Disable") : tr("Enable"), 178, y + 26);
  tft.setTextDatum(TL_DATUM);

  bool touchHeld = isTouchDownDismiss();
  bool prevLeft = isPhysicalButtonPressed(BTN_LEFT);
  bool prevSelect = isPhysicalButtonPressed(BTN_SELECT);
  for (;;) {
    int x = 0, yTouch = 0;
    const bool touching = readTouchXYDismiss(x, yTouch);
    if (touching && !touchHeld && yTouch >= y && yTouch < y + 52) {
      if (x >= 14 && x < 110) return false;
      if (x >= 130 && x < 226) return true;
    }
    touchHeld = touching;
    const bool left = isPhysicalButtonPressed(BTN_LEFT);
    const bool select = isPhysicalButtonPressed(BTN_SELECT);
    if (left && !prevLeft) return false;
    if (select && !prevSelect) return true;
    prevLeft = left;
    prevSelect = select;
    delay(8);
  }
}

}  // namespace

namespace PinLock {

bool enabled() {
  PinRecord record{};
  const bool result = loadRecord(record);
  memset(&record, 0, sizeof(record));
  return result;
}

void bootGate() {
  eraseLegacyNfcLock();
  PinRecord record{};
  if (!loadRecord(record)) return;

  PinEntry entry{};
  collectPin(tr("PIN DEVICE LOCK"), tr("Enter PIN"), entry, false, &record);
  clearEntry(entry);
  memset(&record, 0, sizeof(record));
  drawResult(tr("DEVICE UNLOCKED"), tr("Access granted"), UI_OK);
  Serial.println(F("[pin-lock] boot PIN accepted"));
  delay(350);
}

void manage() {
  eraseLegacyNfcLock();
  const bool wasEnabled = enabled();
  if (!waitManageAction(wasEnabled)) return;

  if (wasEnabled) {
    const bool erased = eraseRecord();
    drawResult(erased ? tr("PIN UNLOCK DISABLED") : tr("NVS ERROR"),
               erased ? tr("Stored PIN erased") : tr("PIN could not be erased"),
               erased ? UI_OK : TFT_RED);
    Serial.printf("[pin-lock] disable %s\n", erased ? "ok" : "failed");
    delay(900);
    return;
  }

  for (;;) {
    PinEntry first{};
    if (collectPin(tr("SET NEW PIN"), tr("PIN must contain 4-8 digits"), first,
                   true, nullptr) == EntryResult::Cancelled) {
      clearEntry(first);
      return;
    }

    PinEntry confirm{};
    if (collectPin(tr("CONFIRM PIN"), tr("Enter PIN again"), confirm, true,
                   nullptr) == EntryResult::Cancelled) {
      clearEntry(first);
      clearEntry(confirm);
      return;
    }

    const bool same = first.length == confirm.length &&
                      memcmp(first.digits, confirm.digits, first.length) == 0;
    if (!same) {
      drawResult(tr("PIN MISMATCH"), tr("PINs do not match - try again"), UI_WARN);
      clearEntry(first);
      clearEntry(confirm);
      delay(950);
      continue;
    }

    const bool saved = saveRecord(first);
    clearEntry(first);
    clearEntry(confirm);
    drawResult(saved ? tr("PIN UNLOCK CONFIGURED") : tr("NVS ERROR"),
               saved ? tr("PIN lock active on reboot") : tr("PIN could not be stored"),
               saved ? UI_OK : TFT_RED);
    Serial.printf("[pin-lock] enrol %s\n", saved ? "ok" : "failed");
    delay(1000);
    return;
  }
}

}  // namespace PinLock
