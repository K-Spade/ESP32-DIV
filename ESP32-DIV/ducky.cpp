#include <Arduino.h>
#include <NimBLEDevice.h>
#include <NimBLEHIDDevice.h>
#include <PCF8574.h>
#include <Preferences.h>
#include <SD.h>
#include <SPI.h>
#include <TFT_eSPI.h>
#include <vector>
#include "Touchscreen.h"
#include "icon.h"
#include "SettingsStore.h"
#include "shared.h"
#include "utils.h"

extern PCF8574 pcf;

extern bool feature_exit_requested;

#include "LocalizedTFT.h"
extern LocalizedTFT tft;

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

namespace Ducky {

static const char* DUCKY_DIR = "/ducky";
static const char* DEV_NAME  = "ESP32S3 Ducky";

#define COL_BG     TFT_BLACK
#define COL_FG     TFT_WHITE
#define COL_ACCENT FEATURE_TEXT
#define COL_WARN   UI_WARN
#define COL_ROWSEL L_Dark

static const int HEADER_H    = 20;

static const int TEXT_HEADER_H = HEADER_H;
static const int FOOTER_H    = FeatureUI::FOOTER_H;
static const int BTN_H       = FeatureUI::BTN_H;
static const int BTN_GAP     = FeatureUI::GAP_X;

static const int LIST_ROW_H  = 40;

static const int LIST_ROW_GAP = 6;
static const int PADDING     = FeatureUI::PAD_X;

static const int TOOLBAR_Y      = 37;
static const int TOOLBAR_HEIGHT = HEADER_H;

static int duckyFooterReserve() {
  return featureHasTouchNavBar() ? touchNavReservedHeight() : FOOTER_H;
}

static int duckyToastTop() {
  return DISPLAY_HEIGHT - duckyFooterReserve() - 28;
}

static int duckyToastCenterY() {
  return DISPLAY_HEIGHT - duckyFooterReserve() - 24;
}

static bool sd_mounted = false;
static bool ui_inited  = false;
static bool ui_busy    = false;
static bool needsRedraw = false;

static bool ducky_active = false;

struct EdgeBtn {
  uint8_t pin;
  bool last;
  uint32_t tLast;
  EdgeBtn(): pin(0), last(true), tLast(0) {}
  explicit EdgeBtn(uint8_t p): pin(p), last(true), tLast(0) {}
};

static EdgeBtn ebUp{BTN_UP}, ebDown{BTN_DOWN}, ebLeft{BTN_LEFT}, ebRight{BTN_RIGHT}, ebSelect{BTN_SELECT};

static bool uiDrawn = false;
static unsigned long lastAnimationTime = 0;
static int animationState = 0;
static int activeIcon = -1;

static const int ICON_NUM = 2;
static int iconX[ICON_NUM] = {210, 10};
static int iconY = 20;

static const unsigned char* icons[ICON_NUM] = {
    bitmap_icon_list,
    bitmap_icon_go_back
};

static const int ICON_SIZE = 16;

static bool pcfPressedEdge(EdgeBtn& eb, uint16_t debounceMs=40) {
  bool cur = isButtonPressed(eb.pin) ? LOW : HIGH;
  uint32_t now = millis();
  bool pressedEvent = (!cur && eb.last && (now - eb.tLast) > debounceMs);
  if (pressedEvent) eb.tLast = now;
  eb.last = cur;
  return pressedEvent;
}

static NimBLEHIDDevice*      hid = nullptr;
static NimBLECharacteristic* inputChr = nullptr;
static NimBLECharacteristic* consumerInputChr = nullptr;
static NimBLECharacteristic* outputChr = nullptr;
static volatile bool         bleConnected = false;
static volatile bool         bleSubscribed = false;
static volatile bool         bleEncrypted = false;
static volatile bool         bleAdvertising = false;
// NimBLE removes a peer from its connection vector before invoking the
// application disconnect callback.  exit() must therefore wait for this
// counter as well as getConnectedCount()==0 before stopping the host.
static volatile uint32_t     bleDisconnectEvents = 0;
static uint32_t              hidStackGeneration = 0;

static bool duckyReady() {
  return bleConnected && bleSubscribed && inputChr != nullptr;
}

struct ScriptItem { String path, name, desc; uint32_t size = 0; };
static std::vector<ScriptItem> items;
static int  sel = 0;

enum class Page { List, Details, Settings };
static Page page = Page::List;

static void duckyUpdateNavLabels() {
  if (!featureHasTouchNavBar()) {
    return;
  }
  if (page == Page::List) {
    // Match the physical D-pad: UP/DOWN always navigate the script list.
    // Reload remains available on RIGHT and SELECT opens the highlighted item.
    setTouchNavLabels("Exit", "Next", "Open", "Prev", "Reload");
  } else if (page == Page::Details) {
    setTouchNavLabels("Back", "Delete", "Run", "List", "Run");
  } else if (page == Page::Settings) {
    setTouchNavLabels("Back", "Next", "Use", "Prev", "Next");
  }
  redrawTouchButtonBar();
}

enum class Dialog { None, ConfirmDelete };
static Dialog dialog = Dialog::None;

static uint32_t defaultDelay = 0;
static String   lastError;

static String trimCR(const String& s) {
  String x = s; if (x.endsWith("\r")) x.remove(x.length() - 1); return x;
}
static void clampSel() {
  int last = (int)items.size() - 1; if (last < 0) last = 0;
  if (sel > last) sel = last; if (sel < 0) sel = 0;
}
static String fitText(const String& s, int pixWidth, int font = 2) {
  String out = s;
  while (tft.textWidth(out, font) > pixWidth && out.length() > 1) {
    out.remove(out.length() - 1);
  }
  if (out.length() < s.length() && pixWidth >= tft.textWidth("…", font) + 4) out += "…";
  return out;
}

static int wrapLineNoEllipsis(const String& s, int pixWidth, int font, String& outLine) {
  if (s.length() == 0) { outLine = ""; return 0; }

  int nl = s.indexOf('\n');
  if (nl >= 0) {
    String pre = s.substring(0, nl);
    if (tft.textWidth(pre, font) <= pixWidth) {
      outLine = pre;
      outLine.trim();
      return nl + 1;
    }
  }

  int best = 0;
  int len = (int)s.length();
  for (int i = 1; i <= len; i++) {
    String sub = s.substring(0, i);
    if (tft.textWidth(sub, font) <= pixWidth) best = i;
    else break;
  }
  if (best <= 0) best = 1;

  int breakAt = best;
  int lastSpace = -1;
  for (int i = best - 1; i >= 0; i--) {
    if (s[i] == ' ') { lastSpace = i; break; }
  }
  if (lastSpace > 0) {
    const int tail = best - lastSpace;

    if (tail <= 12) breakAt = lastSpace;
  }
  if (breakAt <= 0) breakAt = best;

  outLine = s.substring(0, breakAt);
  outLine.trim();
  return breakAt;
}

struct Tap {
  bool has = false;
  int  x = 0, y = 0;
  unsigned long time = 0;
};

static bool touching = false;
static uint32_t touchDownMs = 0;
static int lastX = 0, lastY = 0;
static const uint32_t TOUCH_DEBOUNCE_MS = 120;

static Tap readTap() {
  Tap t{};
  int x, y;
  bool now = readTouchXY(x, y);
  uint32_t ms = millis();

  if (!touching && now) {
    touching = true; touchDownMs = ms; lastX = x; lastY = y;
  } else if (touching && now) {
    lastX = x; lastY = y;
  } else if (touching && !now) {
    touching = false;
    if (ms - touchDownMs >= 20 && ms - touchDownMs <= 800) {
      t.has = true; t.x = lastX; t.y = lastY; t.time = ms;
    }
  }
  return t;
}

// Track the logical script row instead of raw screen coordinates.  The first
// tap redraws the selection and that redraw can exceed the old 300 ms window;
// it also shifts a scrolling list, making coordinate-only matching unreliable.
static int lastScriptTapIndex = -1;
static uint32_t lastScriptTapTime = 0;
static int lastScriptTapX = 0;
static int lastScriptTapY = 0;
static const uint32_t SCRIPT_DOUBLE_TAP_MS = 700;

static int registerScriptRowTap(int index, const Tap& tap) {
  if (!tap.has || index < 0) return -1;
  const uint32_t elapsed = (uint32_t)(tap.time - lastScriptTapTime);
  const int distance = abs(tap.x - lastScriptTapX) + abs(tap.y - lastScriptTapY);
  // A selection redraw may recenter the visible list between both releases.
  // Accept either the same logical item or the same physical double-tap area,
  // and always execute the item selected by the first tap.
  const bool isDouble = lastScriptTapIndex >= 0 &&
      elapsed <= SCRIPT_DOUBLE_TAP_MS &&
      (lastScriptTapIndex == index || distance <= 70);
  if (isDouble) {
    const int target = lastScriptTapIndex;
    // Consume the gesture so a third tap starts a fresh pair.
    lastScriptTapIndex = -1;
    lastScriptTapTime = 0;
    return target;
  } else {
    lastScriptTapIndex = index;
    lastScriptTapTime = tap.time;
    lastScriptTapX = tap.x;
    lastScriptTapY = tap.y;
  }
  return -1;
}

static bool execFile(const String& path);
static void drawListPage(bool forceReload = false);
static void updateListSelection(int newSel);
static void drawDetailsPage();
static void drawSettingsPage();

static void drawIconHeader() {
    if (!uiDrawn) {
        tft.drawFastHLine(0, 19, 240, UI_LINE);
        tft.fillRect(0, 20, 240, 20, DARK_GRAY);

        for (int i = 0; i < ICON_NUM; i++) {
            if (icons[i] != NULL) {
                drawToolbarIcon(iconX[i], iconY, icons[i], TFT_WHITE);
            }
        }
        tft.drawFastHLine(0, 40, 240, UI_LINE);
        uiDrawn = true;
    }
}

static void handleIconTouch(int x, int y) {

    const int y0 = iconY - 4;
    const int y1 = iconY + ICON_SIZE + 4;
    if (y >= y0 && y <= y1) {
        for (int i = 0; i < ICON_NUM; i++) {
            const int x0 = iconX[i] - 6;
            const int x1 = iconX[i] + ICON_SIZE + 6;
            if (x >= x0 && x <= x1) {
                if (icons[i] != NULL && animationState == 0) {

                    if (i == 1) {
                        feature_exit_requested = true;
                        break;
                    }

                    if (i == 0) {
                        if (page != Page::Settings) {
                            page = Page::Settings;
                            drawSettingsPage();
                        } else {
                            page = Page::List;
                            drawListPage();
                        }

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

static void updateIconAnimation() {
    if (animationState > 0 && millis() - lastAnimationTime >= TOOLBAR_ANIMATION_MS) {
        if (animationState == 1) {
            drawToolbarIcon(iconX[activeIcon], iconY, icons[activeIcon], TFT_WHITE);
            animationState = 2;
        } else if (animationState == 2) {
            animationState = 0;
            activeIcon = -1;
        }
        lastAnimationTime = millis();
    }
}

static bool mountSD() {
  sd_mounted = isSDCardAvailable();
  return sd_mounted;
}
static bool ensureDuckyDir() {
  SdFileIoGuard io(1500);
  if (!io.locked()) return false;
  if (!mountSD()) return false;
  if (!SD.exists(DUCKY_DIR)) return SD.mkdir(DUCKY_DIR);
  return true;
}

static void parseMeta(File& f, ScriptItem& it) {
  it.desc = "";
  f.seek(0);
  for (int i = 0; i < 10 && f.available(); ++i) {
    String line = trimCR(f.readStringUntil('\n'));
    if (!line.startsWith("REM")) continue;
    String body = line.substring(3); body.trim();
    if (body.startsWith("Name:"))        { it.name = body.substring(5); it.name.trim(); }
    else if (body.startsWith("Description:")) { it.desc = body.substring(12); it.desc.trim(); }
  }
  if (!it.name.length()) {
    String fn = it.path.substring(it.path.lastIndexOf('/') + 1);
    int dot = fn.lastIndexOf('.'); if (dot > 0) fn.remove(dot);
    it.name = fn;
  }
}
static void reloadList() {
  SdFileIoGuard io(1500);
  if (!io.locked()) return;
  items.clear();
  if (!ensureDuckyDir()) return;
  File d = SD.open(DUCKY_DIR); if (!d) return;
  for (;;) {
    File f = d.openNextFile(); if (!f) break;
    if (f.isDirectory()) { f.close(); continue; }
    String p = String(DUCKY_DIR) + "/" + f.name();
    String ext = p.substring(p.lastIndexOf('.') + 1); ext.toLowerCase();
    if (!(ext == "duck" || ext == "txt")) { f.close(); continue; }
    ScriptItem it; it.path = p; it.size = f.size();
    parseMeta(f, it);
    items.push_back(it);
    f.close();
  }
  d.close();
  clampSel();
}

enum : uint8_t {
  KEY_A = 0x04, KEY_B, KEY_C, KEY_D, KEY_E, KEY_F, KEY_G, KEY_H, KEY_I, KEY_J, KEY_K, KEY_L, KEY_M,
  KEY_N, KEY_O, KEY_P, KEY_Q, KEY_R, KEY_S, KEY_T, KEY_U, KEY_V, KEY_W, KEY_X, KEY_Y, KEY_Z,
  KEY_1 = 0x1E, KEY_2, KEY_3, KEY_4, KEY_5, KEY_6, KEY_7, KEY_8, KEY_9, KEY_0,
  KEY_ENTER = 0x28, KEY_ESC = 0x29, KEY_BACKSPACE = 0x2A, KEY_TAB = 0x2B, KEY_SPACE = 0x2C,
  KEY_MINUS = 0x2D, KEY_EQUAL = 0x2E, KEY_LEFTBRACE = 0x2F, KEY_RIGHTBRACE = 0x30, KEY_BACKSLASH = 0x31,
  KEY_SEMICOLON = 0x33, KEY_APOSTROPHE = 0x34, KEY_GRAVE = 0x35, KEY_COMMA = 0x36, KEY_DOT = 0x37, KEY_SLASH = 0x38,
  KEY_NONUS_HASH = 0x32, KEY_CAPSLOCK = 0x39,
  KEY_F1 = 0x3A, KEY_F2, KEY_F3, KEY_F4, KEY_F5, KEY_F6, KEY_F7, KEY_F8, KEY_F9, KEY_F10, KEY_F11, KEY_F12,
  KEY_PRINTSCREEN = 0x46, KEY_SCROLLLOCK = 0x47, KEY_PAUSE = 0x48, KEY_INSERT = 0x49,
  KEY_HOME = 0x4A, KEY_PAGEUP = 0x4B, KEY_DELETE = 0x4C, KEY_END = 0x4D, KEY_PAGEDOWN = 0x4E,
  KEY_RIGHT = 0x4F, KEY_LEFT = 0x50, KEY_DOWN = 0x51, KEY_UP = 0x52,
  KEY_NUMLOCK = 0x53, KEY_KP_SLASH = 0x54, KEY_KP_ASTERISK = 0x55, KEY_KP_MINUS = 0x56,
  KEY_KP_PLUS = 0x57, KEY_KP_ENTER = 0x58, KEY_KP_1 = 0x59, KEY_KP_2 = 0x5A,
  KEY_KP_3 = 0x5B, KEY_KP_4 = 0x5C, KEY_KP_5 = 0x5D, KEY_KP_6 = 0x5E,
  KEY_KP_7 = 0x5F, KEY_KP_8 = 0x60, KEY_KP_9 = 0x61, KEY_KP_0 = 0x62,
  KEY_KP_DOT = 0x63, KEY_NONUS_BACKSLASH = 0x64, KEY_APP = 0x65, KEY_KP_EQUAL = 0x67,
  KEY_F13 = 0x68, KEY_F14, KEY_F15, KEY_F16, KEY_F17, KEY_F18,
  KEY_F19, KEY_F20, KEY_F21, KEY_F22, KEY_F23, KEY_F24
};
enum : uint8_t {
  MOD_CTRL=0x01, MOD_SHIFT=0x02, MOD_ALT=0x04, MOD_GUI=0x08,
  MOD_RCTRL=0x10, MOD_RSHIFT=0x20, MOD_RALT=0x40, MOD_RGUI=0x80
};
struct __attribute__((packed)) KbdReport { uint8_t modifiers; uint8_t reserved; uint8_t keys[6]; };
struct __attribute__((packed)) ConsumerReport { uint16_t usage; };

enum class KeyboardLayout : uint8_t {
  US, ES, LATAM, UK, DE, FR, IT, PT, Brazil, Custom, Count
};

struct KeyStroke {
  uint8_t usage;
  uint8_t mods;
  constexpr KeyStroke(uint8_t key=0, uint8_t modifiers=0)
      : usage(key), mods(modifiers) {}
};

static KeyboardLayout selectedLayout = KeyboardLayout::US;
static KeyboardLayout activeLayout = KeyboardLayout::US;
static KeyStroke customLayout[95];
static bool customLayoutLoaded = false;

static const char* layoutCode(KeyboardLayout layout) {
  switch (layout) {
    case KeyboardLayout::ES: return "ES";
    case KeyboardLayout::LATAM: return "LATAM";
    case KeyboardLayout::UK: return "UK";
    case KeyboardLayout::DE: return "DE";
    case KeyboardLayout::FR: return "FR";
    case KeyboardLayout::IT: return "IT";
    case KeyboardLayout::PT: return "PT";
    case KeyboardLayout::Brazil: return "BR";
    case KeyboardLayout::Custom: return "CUSTOM";
    default: return "US";
  }
}

static const char* layoutName(KeyboardLayout layout) {
  switch (layout) {
    case KeyboardLayout::ES: return "Spanish (Spain)";
    case KeyboardLayout::LATAM: return "Spanish (Latin America)";
    case KeyboardLayout::UK: return "English (United Kingdom)";
    case KeyboardLayout::DE: return "German";
    case KeyboardLayout::FR: return "French";
    case KeyboardLayout::IT: return "Italian";
    case KeyboardLayout::PT: return "Portuguese";
    case KeyboardLayout::Brazil: return "Portuguese (Brazil)";
    case KeyboardLayout::Custom: return "Custom SD layout";
    default: return "English (United States)";
  }
}

static bool parseLayoutCode(String code, KeyboardLayout& layout) {
  code.trim(); code.toUpperCase();
  if (code == "US" || code == "EN-US") layout = KeyboardLayout::US;
  else if (code == "ES" || code == "ES-ES") layout = KeyboardLayout::ES;
  else if (code == "LATAM" || code == "ES-LATAM" || code == "ES-MX") layout = KeyboardLayout::LATAM;
  else if (code == "UK" || code == "GB" || code == "EN-GB") layout = KeyboardLayout::UK;
  else if (code == "DE" || code == "DE-DE") layout = KeyboardLayout::DE;
  else if (code == "FR" || code == "FR-FR") layout = KeyboardLayout::FR;
  else if (code == "IT" || code == "IT-IT") layout = KeyboardLayout::IT;
  else if (code == "PT" || code == "PT-PT") layout = KeyboardLayout::PT;
  else if (code == "BR" || code == "PT-BR") layout = KeyboardLayout::Brazil;
  else if (code == "CUSTOM") layout = KeyboardLayout::Custom;
  else return false;
  return true;
}

static KeyboardLayout defaultLayoutForUiLanguage() {
  switch (settings().language) {
    case UiLanguage::Spanish: return KeyboardLayout::ES;
    case UiLanguage::Portuguese: return KeyboardLayout::PT;
    default: return KeyboardLayout::US;
  }
}

static void loadKeyboardLayoutPreference() {
  Preferences prefs;
  uint8_t saved = 0xFF;
  if (prefs.begin("mk-signal", true)) {
    saved = prefs.getUChar("duckyLayout", 0xFF);
    prefs.end();
  }
  selectedLayout = saved < (uint8_t)KeyboardLayout::Count
      ? (KeyboardLayout)saved : defaultLayoutForUiLanguage();
  if (selectedLayout == KeyboardLayout::Custom && !customLayoutLoaded) {
    selectedLayout = defaultLayoutForUiLanguage();
  }
  activeLayout = selectedLayout;
}

static void saveKeyboardLayoutPreference() {
  Preferences prefs;
  if (!prefs.begin("mk-signal", false)) return;
  prefs.putUChar("duckyLayout", (uint8_t)selectedLayout);
  prefs.end();
}

class ServerCB : public NimBLEServerCallbacks {
  void onConnect(NimBLEServer*) override {
    bleConnected = true;
    bleSubscribed = false;
    bleEncrypted = false;
    bleAdvertising = false;
    Serial.println(F("[ducky] BLE host connected; waiting for HID subscription"));
  }
  void onConnect(NimBLEServer* server, ble_gap_conn_desc* desc) override {
    if (server && desc) {
      server->updateConnParams(desc->conn_handle, 6, 12, 0, 300);
      Serial.printf("[ducky] peer=%s handle=%u\n",
                    NimBLEAddress(desc->peer_ota_addr).toString().c_str(),
                    (unsigned)desc->conn_handle);
    }
  }
  void onDisconnect(NimBLEServer*) override {
    bleConnected = false;
    bleSubscribed = false;
    bleEncrypted = false;

    if (ducky_active) {
      bleAdvertising = NimBLEDevice::startAdvertising();
      Serial.printf("[ducky] disconnected; advertising restart=%s\n",
                    bleAdvertising ? "OK" : "FAILED");
    }
    ++bleDisconnectEvents;
  }
  void onAuthenticationComplete(ble_gap_conn_desc* desc) override {
    bleEncrypted = desc && desc->sec_state.encrypted;
    Serial.printf("[ducky] pairing encrypted=%u authenticated=%u bonded=%u\n",
                  bleEncrypted ? 1u : 0u,
                  desc ? (unsigned)desc->sec_state.authenticated : 0u,
                  desc ? (unsigned)desc->sec_state.bonded : 0u);
    if (desc && !bleEncrypted) {
      NimBLEServer* server = NimBLEDevice::getServer();
      if (server) server->disconnect(desc->conn_handle);
    }
  }
};

class InputCB : public NimBLECharacteristicCallbacks {
  void onSubscribe(NimBLECharacteristic*, ble_gap_conn_desc* desc,
                   uint16_t subValue) override {
    bleSubscribed = (subValue & 0x0001u) != 0;
    Serial.printf("[ducky] HID notifications=%s handle=%u\n",
                  bleSubscribed ? "ON" : "OFF",
                  desc ? (unsigned)desc->conn_handle : 0u);
  }
};

class OutputCB : public NimBLECharacteristicCallbacks {
  void onWrite(NimBLECharacteristic* c) override { (void)c; }
};

static ServerCB s_serverCallbacks;
static InputCB s_inputCallbacks;
static OutputCB s_outputCallbacks;

static void sendKey(uint8_t usage, uint8_t mods = 0, uint16_t downMs = 10) {
  if (!duckyReady()) return;
  KbdReport idle{};
  inputChr->setValue((uint8_t*)&idle, sizeof(idle)); inputChr->notify(); delay(3);
  KbdReport rpt{}; rpt.modifiers = mods; rpt.keys[0]=usage;
  inputChr->setValue((uint8_t*)&rpt, sizeof(rpt)); inputChr->notify(); delay(downMs);
  inputChr->setValue((uint8_t*)&idle, sizeof(idle)); inputChr->notify(); delay(5);
}

static KeyStroke usKeyFromChar(char c) {
  if (c >= 'a' && c <= 'z') return {(uint8_t)(KEY_A + (c - 'a')), 0};
  if (c >= 'A' && c <= 'Z') return {(uint8_t)(KEY_A + (c - 'A')), MOD_SHIFT};
  if (c >= '1' && c <= '9') return {(uint8_t)(KEY_1 + (c - '1')), 0};
  if (c == '0') return {KEY_0, 0};
  switch (c) {
    case '!': return {KEY_1, MOD_SHIFT}; case '@': return {KEY_2, MOD_SHIFT};
    case '#': return {KEY_3, MOD_SHIFT}; case '$': return {KEY_4, MOD_SHIFT};
    case '%': return {KEY_5, MOD_SHIFT}; case '^': return {KEY_6, MOD_SHIFT};
    case '&': return {KEY_7, MOD_SHIFT}; case '*': return {KEY_8, MOD_SHIFT};
    case '(': return {KEY_9, MOD_SHIFT}; case ')': return {KEY_0, MOD_SHIFT};
    case ' ': return {KEY_SPACE, 0}; case '\n': case '\r': return {KEY_ENTER, 0};
    case '-': return {KEY_MINUS, 0}; case '_': return {KEY_MINUS, MOD_SHIFT};
    case '=': return {KEY_EQUAL, 0}; case '+': return {KEY_EQUAL, MOD_SHIFT};
    case '[': return {KEY_LEFTBRACE, 0}; case '{': return {KEY_LEFTBRACE, MOD_SHIFT};
    case ']': return {KEY_RIGHTBRACE, 0}; case '}': return {KEY_RIGHTBRACE, MOD_SHIFT};
    case '\\': return {KEY_BACKSLASH, 0}; case '|': return {KEY_BACKSLASH, MOD_SHIFT};
    case ';': return {KEY_SEMICOLON, 0}; case ':': return {KEY_SEMICOLON, MOD_SHIFT};
    case '\'': return {KEY_APOSTROPHE, 0}; case '"': return {KEY_APOSTROPHE, MOD_SHIFT};
    case ',': return {KEY_COMMA, 0}; case '<': return {KEY_COMMA, MOD_SHIFT};
    case '.': return {KEY_DOT, 0}; case '>': return {KEY_DOT, MOD_SHIFT};
    case '/': return {KEY_SLASH, 0}; case '?': return {KEY_SLASH, MOD_SHIFT};
    case '`': return {KEY_GRAVE, 0}; case '~': return {KEY_GRAVE, MOD_SHIFT};
    default: return {};
  }
}

static KeyStroke keyForLayout(char c, KeyboardLayout layout) {
  if (layout == KeyboardLayout::Custom && customLayoutLoaded && c >= 0x20 && c <= 0x7E) {
    return customLayout[(uint8_t)c - 0x20];
  }

  KeyStroke k = usKeyFromChar(c);
  const bool upper = c >= 'A' && c <= 'Z';
  char lower = upper ? (char)(c - 'A' + 'a') : c;

  if (layout == KeyboardLayout::DE) {
    if (lower == 'y') k = {KEY_Z, (uint8_t)(upper ? MOD_SHIFT : 0)};
    else if (lower == 'z') k = {KEY_Y, (uint8_t)(upper ? MOD_SHIFT : 0)};
    else switch (c) {
      case '@': k={KEY_Q,MOD_RALT}; break; case '#': k={KEY_BACKSLASH,0}; break;
      case '^': k={KEY_GRAVE,0}; break; case '&': k={KEY_6,MOD_SHIFT}; break;
      case '*': k={KEY_RIGHTBRACE,MOD_SHIFT}; break; case '(': k={KEY_8,MOD_SHIFT}; break;
      case ')': k={KEY_9,MOD_SHIFT}; break; case '-': k={KEY_SLASH,0}; break;
      case '_': k={KEY_SLASH,MOD_SHIFT}; break; case '=': k={KEY_0,MOD_SHIFT}; break;
      case '+': k={KEY_RIGHTBRACE,0}; break; case '[': k={KEY_8,MOD_RALT}; break;
      case ']': k={KEY_9,MOD_RALT}; break; case '\\': k={KEY_MINUS,MOD_RALT}; break;
      case '{': k={KEY_7,MOD_RALT}; break; case '}': k={KEY_0,MOD_RALT}; break;
      case ';': k={KEY_COMMA,MOD_SHIFT}; break; case ':': k={KEY_DOT,MOD_SHIFT}; break;
      case '\'': k={KEY_BACKSLASH,MOD_SHIFT}; break; case '"': k={KEY_2,MOD_SHIFT}; break;
      case '/': k={KEY_7,MOD_SHIFT}; break; case '?': k={KEY_MINUS,MOD_SHIFT}; break;
      case '<': k={KEY_NONUS_BACKSLASH,0}; break; case '>': k={KEY_NONUS_BACKSLASH,MOD_SHIFT}; break;
      case '|': k={KEY_NONUS_BACKSLASH,MOD_RALT}; break; default: break;
    }
  } else if (layout == KeyboardLayout::FR) {
    if (lower == 'a') k={KEY_Q,(uint8_t)(upper?MOD_SHIFT:0)};
    else if (lower == 'q') k={KEY_A,(uint8_t)(upper?MOD_SHIFT:0)};
    else if (lower == 'w') k={KEY_Z,(uint8_t)(upper?MOD_SHIFT:0)};
    else if (lower == 'z') k={KEY_W,(uint8_t)(upper?MOD_SHIFT:0)};
    else if (lower == 'm') k={KEY_SEMICOLON,(uint8_t)(upper?MOD_SHIFT:0)};
    else if (c >= '1' && c <= '9') k={(uint8_t)(KEY_1+(c-'1')),MOD_SHIFT};
    else if (c == '0') k={KEY_0,MOD_SHIFT};
    else switch (c) {
      case '&': k={KEY_1,0}; break; case '"': k={KEY_3,0}; break;
      case '\'': k={KEY_4,0}; break; case '(': k={KEY_5,0}; break;
      case '-': k={KEY_6,0}; break; case '_': k={KEY_8,0}; break;
      case ')': k={KEY_MINUS,0}; break; case '=': k={KEY_EQUAL,0}; break;
      case '@': k={KEY_0,MOD_RALT}; break; case '#': k={KEY_3,MOD_RALT}; break;
      case '$': k={KEY_RIGHTBRACE,0}; break; case '*': k={KEY_BACKSLASH,0}; break;
      case '[': k={KEY_5,MOD_RALT}; break; case ']': k={KEY_MINUS,MOD_RALT}; break;
      case '\\': k={KEY_8,MOD_RALT}; break; case '{': k={KEY_4,MOD_RALT}; break;
      case '}': k={KEY_EQUAL,MOD_RALT}; break; case ';': k={KEY_COMMA,0}; break;
      case ':': k={KEY_DOT,0}; break; case ',': k={KEY_M,0}; break;
      case '.': k={KEY_COMMA,MOD_SHIFT}; break; case '/': k={KEY_DOT,MOD_SHIFT}; break;
      case '?': k={KEY_M,MOD_SHIFT}; break; case '<': k={KEY_NONUS_BACKSLASH,0}; break;
      case '>': k={KEY_NONUS_BACKSLASH,MOD_SHIFT}; break; default: break;
    }
  } else if (layout == KeyboardLayout::ES || layout == KeyboardLayout::LATAM ||
             layout == KeyboardLayout::IT || layout == KeyboardLayout::PT ||
             layout == KeyboardLayout::Brazil) {
    const bool spanish = layout == KeyboardLayout::ES || layout == KeyboardLayout::LATAM;
    switch (c) {
      case '@': k={KEY_2,MOD_RALT}; break; case '#': k={KEY_3,MOD_RALT}; break;
      case '"': k={KEY_2,MOD_SHIFT}; break; case '&': k={KEY_6,MOD_SHIFT}; break;
      case '(': k={KEY_8,MOD_SHIFT}; break; case ')': k={KEY_9,MOD_SHIFT}; break;
      case '=': k={KEY_0,MOD_SHIFT}; break; case '[': k={KEY_LEFTBRACE,MOD_RALT}; break;
      case ']': k={KEY_RIGHTBRACE,MOD_RALT}; break; case '{': k={KEY_APOSTROPHE,MOD_RALT}; break;
      case '}': k={KEY_BACKSLASH,MOD_RALT}; break; case ';': k={KEY_COMMA,MOD_SHIFT}; break;
      case ':': k={KEY_DOT,MOD_SHIFT}; break; case '<': k={KEY_NONUS_BACKSLASH,0}; break;
      case '>': k={KEY_NONUS_BACKSLASH,MOD_SHIFT}; break;
      default: break;
    }
    if (spanish) {
      switch (c) {
        case '-': k={KEY_SLASH,0}; break; case '_': k={KEY_SLASH,MOD_SHIFT}; break;
        case '+': k={KEY_RIGHTBRACE,0}; break; case '*': k={KEY_RIGHTBRACE,MOD_SHIFT}; break;
        case '\\': k={KEY_GRAVE,MOD_RALT}; break; case '/': k={KEY_7,MOD_SHIFT}; break;
        case '?': k={KEY_MINUS,MOD_SHIFT}; break; case '\'': k={KEY_MINUS,0}; break;
        case '|': k={KEY_1,MOD_RALT}; break; default: break;
      }
    }
  } else if (layout == KeyboardLayout::UK) {
    switch (c) {
      case '@': k={KEY_APOSTROPHE,MOD_SHIFT}; break; case '"': k={KEY_2,MOD_SHIFT}; break;
      case '#': k={KEY_NONUS_HASH,0}; break; case '~': k={KEY_NONUS_HASH,MOD_SHIFT}; break;
      case '\\': k={KEY_NONUS_BACKSLASH,0}; break; case '|': k={KEY_NONUS_BACKSLASH,MOD_SHIFT}; break;
      default: break;
    }
  }
  return k;
}

static void typeASCII(const String& s, uint16_t charDown = 10) {
  for (size_t i=0;i<s.length();++i){ KeyStroke k=keyForLayout(s[i],activeLayout); if(k.usage) sendKey(k.usage,k.mods,charDown); }
}

static bool loadCustomKeyboardLayout() {
  customLayoutLoaded = false;
  for (uint8_t c=0x20; c<=0x7E; ++c) customLayout[c-0x20]=usKeyFromChar((char)c);
  if (!sd_mounted || !SD.exists("/ducky/layout.kmap")) return false;
  File f=SD.open("/ducky/layout.kmap",FILE_READ);
  if(!f) return false;
  uint16_t mapped=0;
  while(f.available()){
    String line=trimCR(f.readStringUntil('\n')); line.trim();
    if(!line.length() || line.startsWith("#") || line.startsWith("REM")) continue;
    int eq=line.indexOf('='); int comma=line.indexOf(',',eq+1);
    if(eq<=0 || comma<=eq) continue;
    String charHex=line.substring(0,eq); String usageHex=line.substring(eq+1,comma);
    String modsHex=line.substring(comma+1);
    charHex.trim(); usageHex.trim(); modsHex.trim();
    const uint32_t ch=strtoul(charHex.c_str(),nullptr,16);
    const uint32_t usage=strtoul(usageHex.c_str(),nullptr,16);
    const uint32_t mods=strtoul(modsHex.c_str(),nullptr,16);
    if(ch<0x20 || ch>0x7E || usage>0x73 || mods>0xFF) continue;
    customLayout[ch-0x20]={(uint8_t)usage,(uint8_t)mods};
    ++mapped;
  }
  f.close();
  customLayoutLoaded=mapped>0;
  Serial.printf("[ducky] custom keyboard layout entries=%u\n",(unsigned)mapped);
  return customLayoutLoaded;
}

static bool resolveKeyWord(const String& kw, uint8_t& usage, uint8_t& mods) {
  String k=kw; k.toUpperCase(); usage=0; mods=0;
  if (k=="ENTER"||k=="RETURN") usage=KEY_ENTER;
  else if (k=="ESC"||k=="ESCAPE") usage=KEY_ESC;
  else if (k=="TAB") usage=KEY_TAB;
  else if (k=="SPACE") usage=KEY_SPACE;
  else if (k=="BACKSPACE") usage=KEY_BACKSPACE;
  else if (k=="DELETE"||k=="DEL") usage=KEY_DELETE;
  else if (k=="INSERT"||k=="INS") usage=KEY_INSERT;
  else if (k=="PRINTSCREEN"||k=="PRTSC"||k=="PRINT_SCREEN") usage=KEY_PRINTSCREEN;
  else if (k=="PAUSE"||k=="BREAK") usage=KEY_PAUSE;
  else if (k=="MENU"||k=="APP"||k=="APPLICATION") usage=KEY_APP;
  else if (k=="CAPSLOCK"||k=="CAPS_LOCK") usage=KEY_CAPSLOCK;
  else if (k=="NUMLOCK"||k=="NUM_LOCK") usage=KEY_NUMLOCK;
  else if (k=="SCROLLLOCK"||k=="SCROLL_LOCK") usage=KEY_SCROLLLOCK;
  else if (k=="UP"||k=="UPARROW") usage=KEY_UP;
  else if (k=="DOWN"||k=="DOWNARROW") usage=KEY_DOWN;
  else if (k=="LEFT"||k=="LEFTARROW") usage=KEY_LEFT;
  else if (k=="RIGHT"||k=="RIGHTARROW") usage=KEY_RIGHT;
  else if (k=="HOME") usage=KEY_HOME;
  else if (k=="END") usage=KEY_END;
  else if (k=="PGUP"||k=="PAGEUP") usage=KEY_PAGEUP;
  else if (k=="PGDN"||k=="PAGEDOWN") usage=KEY_PAGEDOWN;
  else if (k=="KP_SLASH") usage=KEY_KP_SLASH;
  else if (k=="KP_ASTERISK") usage=KEY_KP_ASTERISK;
  else if (k=="KP_MINUS") usage=KEY_KP_MINUS;
  else if (k=="KP_PLUS") usage=KEY_KP_PLUS;
  else if (k=="KP_ENTER") usage=KEY_KP_ENTER;
  else if (k=="KP_DOT"||k=="KP_DECIMAL") usage=KEY_KP_DOT;
  else if (k=="KP_EQUAL"||k=="KP_EQUALS") usage=KEY_KP_EQUAL;
  else if (k.startsWith("KP_") && k.length()==4 && isDigit(k[3])) {
    int n=k.substring(3).toInt(); usage=(n==0)?KEY_KP_0:(KEY_KP_1+(n-1));
  }
  else if (k.startsWith("F")) {
    int n=k.substring(1).toInt();
    if(n>=1&&n<=12) usage = KEY_F1+(n-1);
    else if(n>=13&&n<=24) usage = KEY_F13+(n-13);
  }
  if (usage) return true;
  if (k.length()==1){
    char c=k[0];
    // A key name inside a shortcut describes the physical key, not a typed
    // character.  COMMAND Q must therefore be GUI+Q, never GUI+SHIFT+Q.
    if(c>='A' && c<='Z') { usage=KEY_A+(c-'A'); return true; }
    KeyStroke stroke=keyForLayout(c,activeLayout);
    if(stroke.usage){usage=stroke.usage; mods=stroke.mods; return true;}
  }
  return false;
}
static bool modifierFromToken(const String& token, uint8_t& modifier) {
  String t=token; t.toUpperCase(); modifier=0;
  if(t=="CTRL"||t=="CONTROL") modifier=MOD_CTRL;
  else if(t=="SHIFT") modifier=MOD_SHIFT;
  else if(t=="ALT"||t=="OPTION"||t=="OPT"||t=="OPTIOM") modifier=MOD_ALT;
  else if(t=="GUI"||t=="WIN"||t=="WINDOWS"||t=="CMD"||t=="COMMAND") modifier=MOD_GUI;
  else if(t=="RCTRL"||t=="RCONTROL") modifier=MOD_RCTRL;
  else if(t=="RSHIFT") modifier=MOD_RSHIFT;
  else if(t=="RALT"||t=="ROPTION") modifier=MOD_RALT;
  else if(t=="RGUI"||t=="RWINDOWS"||t=="RWIN"||t=="RCMD"||t=="RCOMMAND") modifier=MOD_RGUI;
  return modifier != 0;
}
static bool resolveConsumerWord(const String& kw, uint16_t& usage) {
  String k=kw; k.toUpperCase(); usage=0;
  if(k=="MK_VOLUP"||k=="VOLUMEUP"||k=="VOLUME_UP") usage=0x00E9;
  else if(k=="MK_VOLDOWN"||k=="VOLUMEDOWN"||k=="VOLUME_DOWN") usage=0x00EA;
  else if(k=="MK_MUTE"||k=="MUTE") usage=0x00E2;
  else if(k=="MK_PREV"||k=="MEDIA_PREV") usage=0x00B6;
  else if(k=="MK_NEXT"||k=="MEDIA_NEXT") usage=0x00B5;
  else if(k=="MK_PP"||k=="MEDIA_PLAY_PAUSE"||k=="PLAY_PAUSE") usage=0x00CD;
  else if(k=="MK_STOP"||k=="MEDIA_STOP") usage=0x00B7;
  return usage != 0;
}
static void pressChord(const uint8_t* usages, size_t usageCount, uint8_t mods, uint16_t downMs=50){
  if(!duckyReady()) return;
  KbdReport idle{}; inputChr->setValue((uint8_t*)&idle,sizeof(idle)); inputChr->notify(); delay(3);
  KbdReport rpt{}; rpt.modifiers=mods;
  const size_t count = usageCount > sizeof(rpt.keys) ? sizeof(rpt.keys) : usageCount;
  for(size_t i=0; i<count; ++i) rpt.keys[i]=usages[i];
  inputChr->setValue((uint8_t*)&rpt,sizeof(rpt)); inputChr->notify(); delay(downMs);
  inputChr->setValue((uint8_t*)&idle,sizeof(idle)); inputChr->notify(); delay(5);
}
static void pressCombo(uint8_t usage, uint8_t mods, uint16_t downMs=50){
  pressChord(&usage,1,mods,downMs);
}
static void pressModifiers(uint8_t mods, uint16_t downMs=50){
  if(!duckyReady()) return;
  KbdReport idle{}; inputChr->setValue((uint8_t*)&idle,sizeof(idle)); inputChr->notify(); delay(3);
  KbdReport rpt{}; rpt.modifiers=mods;
  inputChr->setValue((uint8_t*)&rpt,sizeof(rpt)); inputChr->notify(); delay(downMs);
  inputChr->setValue((uint8_t*)&idle,sizeof(idle)); inputChr->notify(); delay(5);
}
static void sendConsumer(uint16_t usage, uint16_t downMs=50){
  if(!duckyReady() || !consumerInputChr) return;
  ConsumerReport rpt{usage};
  consumerInputChr->setValue((uint8_t*)&rpt,sizeof(rpt)); consumerInputChr->notify(); delay(downMs);
  ConsumerReport idle{0};
  consumerInputChr->setValue((uint8_t*)&idle,sizeof(idle)); consumerInputChr->notify(); delay(5);
}
static void tokenize(const String& line, std::vector<String>& out){
  out.clear(); String tmp;
  for(size_t i=0;i<line.length();++i){
    char c=line[i];
    if(c==' '||c=='\t'){ if(tmp.length()){out.push_back(tmp); tmp="";} }
    else tmp+=c;
  }
  if(tmp.length()) out.push_back(tmp);
}
static bool parseUnsignedArg(const std::vector<String>& tok, uint32_t& value){
  if(tok.size()!=2 || !tok[1].length()) return false;
  uint64_t parsed=0;
  for(size_t i=0;i<tok[1].length();++i){
    if(!isDigit(tok[1][i])) return false;
    parsed=parsed*10u+(uint8_t)(tok[1][i]-'0');
    if(parsed>600000u) parsed=600000u;
  }
  value=(uint32_t)parsed;
  return true;
}
static bool execLine(const String& raw, uint32_t& interDelay, String& err,
                     String& previousCommand, bool repeated, bool& didExecute){
  didExecute=false;
  String line=raw; line.trim(); if(!line.length()) return true;
  if(line.startsWith("REM")) return true;

  std::vector<String> tok; tokenize(line,tok);
  if(tok.empty()) return true;

  String cmd=tok[0]; String rest=line.substring(cmd.length()); rest.trim();
  String ucmd=cmd; ucmd.toUpperCase();

  if(ucmd=="REPEAT"){
    if(repeated){ err="Nested REPEAT is not allowed"; return false; }
    uint32_t count=0;
    if(!parseUnsignedArg(tok,count)){ err="REPEAT requires one numeric count"; return false; }
    if(!previousCommand.length()){ err="REPEAT has no previous command"; return false; }
    const String replay=previousCommand;
    while(count--){
      bool replayExecuted=false;
      if(!execLine(replay,interDelay,err,previousCommand,true,replayExecuted)) return false;
      if(replayExecuted && defaultDelay) delay(defaultDelay);
    }
    return true;
  }
  if(ucmd=="DELAY"){
    uint32_t value=0; if(!parseUnsignedArg(tok,value)){err="DELAY requires one numeric value";return false;}
    interDelay=value; delay(interDelay); didExecute=true; if(!repeated) previousCommand=line; return true;
  }
  if(ucmd=="DEFAULT_DELAY"||ucmd=="DEFAULTDELAY"){
    uint32_t value=0; if(!parseUnsignedArg(tok,value)){err="DEFAULT_DELAY requires one numeric value";return false;}
    defaultDelay=value; return true;
  }
  if(ucmd=="DUCKY_LANG"||ucmd=="LANG"||ucmd=="KEYBOARD_LAYOUT"){
    if(tok.size()!=2){err="Keyboard layout requires one code";return false;}
    KeyboardLayout requested;
    if(!parseLayoutCode(tok[1],requested)){err=String("Unknown keyboard layout: ")+tok[1];return false;}
    if(requested==KeyboardLayout::Custom && !customLayoutLoaded){err="Custom layout.kmap not found";return false;}
    activeLayout=requested;
    return true;
  }
  if(ucmd=="WAIT_FOR"){
    uint32_t value=0; if(!parseUnsignedArg(tok,value)){err="WAIT_FOR requires one numeric value";return false;}
    delay(value); didExecute=true; if(!repeated) previousCommand=line; return true;
  }
  if(ucmd=="STRING"){ typeASCII(rest); didExecute=true; if(!repeated) previousCommand=line; return true; }

  // Parse the whole line as one HID chord.  A boot-keyboard report can carry
  // eight modifiers plus six simultaneous ordinary keys, so combinations such
  // as COMMAND OPTION M H and COMMAND CONTROL Q are sent atomically.
  uint8_t mods=0;
  uint8_t keys[6]={0,0,0,0,0,0};
  size_t keyCount=0;
  for(size_t i=0; i<tok.size(); ++i){
    uint8_t oneMod=0;
    if(modifierFromToken(tok[i],oneMod)) { mods|=oneMod; continue; }

    uint16_t consumer=0;
    if(resolveConsumerWord(tok[i],consumer)){
      if(tok.size()!=1){ err="Media keys cannot be used in a chord"; return false; }
      sendConsumer(consumer); didExecute=true; if(!repeated) previousCommand=line; return true;
    }

    uint8_t usage=0,keyMods=0;
    if(!resolveKeyWord(tok[i],usage,keyMods)){
      err=String("Unknown key: ")+tok[i]; return false;
    }
    mods|=keyMods;

    bool duplicate=false;
    for(size_t k=0; k<keyCount; ++k) if(keys[k]==usage){ duplicate=true; break; }
    if(duplicate) continue;
    if(keyCount>=sizeof(keys)){ err="Too many keys in chord (maximum 6)"; return false; }
    keys[keyCount++]=usage;
  }

  if(keyCount){
    pressChord(keys,keyCount,mods); didExecute=true; if(!repeated) previousCommand=line; return true;
  }
  if(mods){
    pressModifiers(mods); didExecute=true; if(!repeated) previousCommand=line; return true;
  }
  err=String("Unknown command: ")+cmd; return false;
}

struct ExecHistoryEntry {
  uint32_t number=0;
  String text;
};

static String executionCommandLabel(const String& raw){
  String line=raw; line.trim();
  std::vector<String> tok; tokenize(line,tok);
  if(tok.size()<2) return line;

  bool chord=true;
  for(size_t i=0; i<tok.size(); ++i){
    uint8_t modifier=0,usage=0,keyMods=0;
    if(modifierFromToken(tok[i],modifier)) continue;
    if(resolveKeyWord(tok[i],usage,keyMods)) continue;
    chord=false; break;
  }
  if(!chord) return line;

  String result;
  for(size_t i=0; i<tok.size(); ++i){
    String part=tok[i]; part.toUpperCase();
    if(result.length()) result += " + ";
    result += part;
  }
  return result;
}

static void drawExecutionFrame(const String& path, int top){
  tft.fillRect(0,top,DISPLAY_WIDTH,DISPLAY_HEIGHT-top,COL_BG);
  tft.setTextColor(COL_ACCENT,COL_BG);
  tft.drawString("LIVE EXECUTION",PADDING,top+3,2);
  tft.setTextColor(UI_DIM_TEXT,COL_BG);
  int slash=path.lastIndexOf('/');
  String name=(slash>=0)?path.substring(slash+1):path;
  tft.drawString(fitText(name,DISPLAY_WIDTH-2*PADDING,1),PADDING,top+23,1);
  tft.drawFastHLine(PADDING,top+36,DISPLAY_WIDTH-2*PADDING,UI_LINE);
  tft.setTextColor(UI_DIM_TEXT,COL_BG);
  tft.drawString("CURRENT COMMAND",PADDING,top+55,1);
  tft.drawString("RECENT ACTIVITY",PADDING,top+113,1);
}

static void drawExecutionProgress(int top, uint8_t percent, uint32_t commandNo){
  const int x=PADDING, y=top+40, w=DISPLAY_WIDTH-2*PADDING, h=8;
  tft.fillRoundRect(x,y,w,h,3,COL_ROWSEL);
  const int fill=(int)((uint32_t)(w-2)*percent/100u);
  if(fill>0) tft.fillRoundRect(x+1,y+1,fill,h-2,2,COL_ACCENT);
  tft.fillRect(DISPLAY_WIDTH-78,top+2,78,15,COL_BG);
  tft.setTextColor(COL_FG,COL_BG);
  tft.setTextDatum(TR_DATUM);
  tft.drawString(String(percent)+"%  #"+String(commandNo),DISPLAY_WIDTH-PADDING,top+3,1);
  tft.setTextDatum(TL_DATUM);
}

static void drawExecutionCurrent(int top, const String& command, const String& state, uint16_t color){
  const int x=PADDING, y=top+68, w=DISPLAY_WIDTH-2*PADDING, h=39;
  tft.fillRoundRect(x,y,w,h,6,COL_ROWSEL);
  tft.drawRoundRect(x,y,w,h,6,color);
  tft.fillRoundRect(x+5,y+7,34,24,5,color);
  tft.setTextColor(COL_BG,color);
  tft.drawCentreString(state,x+22,y+12,1);
  tft.setTextColor(COL_FG,COL_ROWSEL);
  tft.drawString(fitText(command,w-49,2),x+44,y+11,2);
}

static void drawExecutionHistory(int top, const ExecHistoryEntry* history, size_t count){
  const int x=PADDING, startY=top+127, w=DISPLAY_WIDTH-2*PADDING, rowH=23;
  tft.fillRect(x,startY,w,rowH*4,COL_BG);
  for(size_t i=0; i<count && i<4; ++i){
    const int y=startY+(int)i*rowH;
    tft.fillRoundRect(x,y,w,rowH-3,4,COL_ROWSEL);
    tft.setTextColor(UI_OK,COL_ROWSEL);
    tft.drawString(String(history[i].number),x+5,y+6,1);
    tft.setTextColor(UI_TEXT,COL_ROWSEL);
    tft.drawString(fitText(history[i].text,w-35,1),x+29,y+6,1);
  }
}

static void pushExecutionHistory(ExecHistoryEntry* history, size_t& count, uint32_t number, const String& text){
  if(count<4) ++count;
  for(size_t i=count-1; i>0; --i) history[i]=history[i-1];
  history[0].number=number;
  history[0].text=text.length()>80?text.substring(0,80):text;
}

static void drawExecutionResult(int top, const String& label, uint16_t color){
  const int y=DISPLAY_HEIGHT-25;
  tft.fillRoundRect(PADDING,y,DISPLAY_WIDTH-2*PADDING,20,5,color);
  tft.setTextColor(COL_BG,color);
  tft.drawCentreString(label,DISPLAY_WIDTH/2,y+5,1);
}

static bool anyDuckyPhysicalButtonPressed() {
  return isPhysicalButtonPressed(BTN_UP) || isPhysicalButtonPressed(BTN_DOWN) ||
         isPhysicalButtonPressed(BTN_LEFT) || isPhysicalButtonPressed(BTN_RIGHT) ||
         isPhysicalButtonPressed(BTN_SELECT);
}

static void waitForDuckyPhysicalButton(int top) {
  drawExecutionCurrent(top,"Press any physical button","WAIT",UI_WARN);
  drawExecutionResult(top,"SCRIPT PAUSED",UI_WARN);
  const int y=top+108;
  tft.fillRect(PADDING,y,DISPLAY_WIDTH-2*PADDING,18,COL_BG);
  tft.setTextColor(UI_TEXT,COL_BG);
  tft.drawCentreString("Waiting for a new button press",DISPLAY_WIDTH/2,y+4,1);

  // Audible confirmation is emitted once when the interpreter reaches the
  // wait instruction. soundBeep() honours the user's Sound setting and always
  // returns GPIO2 to input mode, which is required by the battery divider.
  soundBeep(2450, 58);
  delay(42);
  soundBeep(3150, 68);

  // Do not consume the button that launched the script.  The instruction
  // resumes only after all controls have first been released and a fresh
  // physical PCF8574 button press is observed.
  while(anyDuckyPhysicalButtonPressed()){ delay(10); yield(); }
  delay(35);
  while(!anyDuckyPhysicalButtonPressed()){ delay(10); yield(); }
  while(anyDuckyPhysicalButtonPressed()){ delay(10); yield(); }

  const uint32_t now=millis();
  ebUp.last=ebDown.last=ebLeft.last=ebRight.last=ebSelect.last=true;
  ebUp.tLast=ebDown.tLast=ebLeft.tLast=ebRight.tLast=ebSelect.tLast=now;
  drawExecutionResult(top,"SCRIPT RESUMED",UI_OK);
  delay(180);
}

static bool execFile(const String& path) {
  SdFileIoGuard io(1500);
  if (!io.locked()) { lastError="SD bus busy"; return false; }
  lastError = "";
  if (!duckyReady()) { lastError="BLE keyboard not ready"; return false; }
  if (!mountSD())    { lastError="SD not mounted";   return false; }
  File f = SD.open(path, FILE_READ); if(!f){ lastError="Open failed"; return false; }

  defaultDelay = 0; uint32_t interDelay = 0; String previousCommand;
  activeLayout=selectedLayout;

  const int execTop = TOOLBAR_Y + HEADER_H + HEADER_H + 1;
  drawExecutionFrame(path,execTop);
  drawExecutionProgress(execTop,0,0);
  drawExecutionCurrent(execTop,"Waiting for first command","READY",UI_ACCENT);

  if (duckyReady()) { KbdReport idle{}; inputChr->setValue((uint8_t*)&idle,sizeof(idle)); inputChr->notify(); delay(10); }

  ExecHistoryEntry history[4];
  size_t historyCount=0;
  uint32_t commandNo=0;
  const uint32_t fileSize=(uint32_t)f.size();

  ui_busy = true;

  while (f.available()) {
    String line = trimCR(f.readStringUntil('\n'));
    String trimmed=line; trimmed.trim();
    if(!trimmed.length() || trimmed.startsWith("REM")) { yield(); continue; }
    ++commandNo;
    const String commandLabel=executionCommandLabel(trimmed);
    const uint8_t beforePct=fileSize?(uint8_t)(((uint64_t)f.position()*100u)/fileSize):0;
    drawExecutionProgress(execTop,beforePct,commandNo);
    drawExecutionCurrent(execTop,commandLabel,"RUN",COL_ACCENT);

    String upperCommand=trimmed; upperCommand.toUpperCase();
    if(upperCommand=="WAIT_FOR_BUTTON_PRESS" || upperCommand=="WAIT_FOR_BUTTON" ||
       upperCommand=="WAIT_BUTTON"){
      const uint32_t resumePosition=(uint32_t)f.position();
      f.close();
      io.release();
      waitForDuckyPhysicalButton(execTop);
      if(!io.acquire(1500) || !mountSD()){
        activeLayout=selectedLayout;
        lastError="SD unavailable after pause";
        drawExecutionCurrent(execTop,lastError,"ERR",COL_WARN);
        drawExecutionResult(execTop,"SCRIPT ERROR",COL_WARN);
        ui_busy=false;
        delay(900);
        return false;
      }
      f=SD.open(path,FILE_READ);
      if(!f || !f.seek(resumePosition)){
        if(f) f.close();
        activeLayout=selectedLayout;
        lastError="Cannot resume script file";
        drawExecutionCurrent(execTop,lastError,"ERR",COL_WARN);
        drawExecutionResult(execTop,"SCRIPT ERROR",COL_WARN);
        ui_busy=false;
        delay(900);
        return false;
      }
      pushExecutionHistory(history,historyCount,commandNo,"WAIT_FOR_BUTTON_PRESS");
      drawExecutionHistory(execTop,history,historyCount);
      drawExecutionCurrent(execTop,"Physical button received","OK",UI_OK);
      continue;
    }

    String err;
    bool didExecute=false;
    if (!execLine(line, interDelay, err, previousCommand, false, didExecute)) {
      lastError = err; f.close(); activeLayout=selectedLayout;
      if (duckyReady()) { KbdReport idle{}; inputChr->setValue((uint8_t*)&idle,sizeof(idle)); inputChr->notify(); delay(10); }
      drawExecutionCurrent(execTop,fitText(err,DISPLAY_WIDTH-58,1),"ERR",COL_WARN);
      drawExecutionResult(execTop,"SCRIPT ERROR",COL_WARN);
      ui_busy = false;
      delay(900);
      return false;
    }
    if (didExecute && defaultDelay) delay(defaultDelay);
    pushExecutionHistory(history,historyCount,commandNo,commandLabel);
    drawExecutionHistory(execTop,history,historyCount);
    const uint8_t afterPct=fileSize?(uint8_t)(((uint64_t)f.position()*100u)/fileSize):100;
    drawExecutionProgress(execTop,afterPct,commandNo);
    yield();
  }
  f.close();
  activeLayout=selectedLayout;

  if (duckyReady()) { KbdReport idle{}; inputChr->setValue((uint8_t*)&idle,sizeof(idle)); inputChr->notify(); delay(20); }

  drawExecutionProgress(execTop,100,commandNo);
  drawExecutionCurrent(execTop,commandNo?history[0].text:"No executable commands","DONE",UI_OK);
  drawExecutionResult(execTop,"SCRIPT COMPLETED",UI_OK);

  ui_busy = false;
  delay(700);
  return true;
}

static FeatureUI::Button listBtns[3];
static FeatureUI::Button detailsBtns[3];
static FeatureUI::Button confirmBtns[2];
static FeatureUI::Button infoBtn;

enum class HeaderType { TextOnly, IconBar, TextWithInfo };

static void drawHeader(HeaderType type, const String& title = "", bool drawFooterBg = true) {
  int currentY = TOOLBAR_Y;

  if (type == HeaderType::IconBar) {
    drawIconHeader();

    currentY = TOOLBAR_Y;
  }

  if (type == HeaderType::TextOnly || type == HeaderType::TextWithInfo) {

    tft.fillRect(0, currentY, DISPLAY_WIDTH, TEXT_HEADER_H, COL_BG);

    if (type == HeaderType::TextOnly && !title.isEmpty()) {

      tft.setTextColor(COL_FG, COL_BG);
      tft.setTextFont(2);
      tft.drawString(title, PADDING, currentY + 1, 2);
    }

    if (type == HeaderType::TextWithInfo) {

      int infoY = currentY + 6;
      tft.setTextFont(1);

      const bool ready = duckyReady();
      const char* bleTxt = ready ? "BLE: Ready" :
                           (bleConnected ? "BLE: Pairing" :
                            (bleAdvertising ? "BLE: Pair keyboard" : "BLE: Offline"));
      tft.setTextColor(ready ? UI_OK : (bleAdvertising ? UI_ACCENT : UI_WARN), COL_BG);
      tft.drawString(bleTxt, PADDING, infoY, 1);

      tft.setTextColor(sd_mounted ? UI_OK : UI_WARN, COL_BG);
      const char* sdTxt = sd_mounted ? "SD: OK" : "SD: Not mounted";
      int sdTw = tft.textWidth(sdTxt, 1);
      tft.drawString(sdTxt, DISPLAY_WIDTH - PADDING - sdTw, infoY, 1);
    }

    tft.drawLine(
      0,
      currentY + TEXT_HEADER_H,
      DISPLAY_WIDTH,
      currentY + TEXT_HEADER_H,
      UI_LINE
    );
  }

  if (drawFooterBg && type != HeaderType::IconBar && !featureHasTouchNavBar()) {
    FeatureUI::drawFooterBg();
  }
}

static uint32_t lastStatusPollMs = 0;
static uint8_t lastHeaderBleState = 0xFF;
static bool lastHeaderSd  = false;

static uint8_t currentHeaderBleState() {
  if (duckyReady()) return 3;
  if (bleConnected) return 2;
  if (bleAdvertising) return 1;
  return 0;
}

static void refreshHeaderStatusIfNeeded(bool force = false) {
  auto showAndExitNoSD = [&](const char* title, const char* subtitle) {
    sd_mounted = false;

    tft.fillRect(0, TOOLBAR_Y - 1, DISPLAY_WIDTH, DISPLAY_HEIGHT - (TOOLBAR_Y - 1), COL_BG);
    tft.setTextColor(UI_WARN, COL_BG);
    tft.setTextFont(2);
    tft.drawCentreString(title, DISPLAY_WIDTH/2, DISPLAY_HEIGHT/2 - 14, 2);
    tft.setTextFont(1);
    tft.setTextColor(UI_TEXT, COL_BG);
    tft.drawCentreString(subtitle, DISPLAY_WIDTH/2, DISPLAY_HEIGHT/2 + 10, 1);
    delay(900);

    ducky_active = false;
    feature_exit_requested = true;
  };

  static bool lastCardPresent = true;
  const uint32_t now = millis();
  if (force || (now - lastStatusPollMs) > 500) {
    lastStatusPollMs = now;
#ifdef SD_CD
    pinMode(SD_CD, INPUT_PULLUP);
    bool cardPresent = !digitalRead(SD_CD);
#else
    bool cardPresent = true;
#endif

    if (lastCardPresent && !cardPresent) {
      showAndExitNoSD("SD card removed", "Returning...");
      lastCardPresent = cardPresent;
      return;
    }
    lastCardPresent = cardPresent;
    if (!cardPresent) {
      sd_mounted = false;
    } else if (cardPresent && !sd_mounted) {

      mountSD();
    }
  }

  const uint8_t bleState = currentHeaderBleState();
  if (force || lastHeaderBleState != bleState || lastHeaderSd != sd_mounted) {
    uiDrawn = false;
    drawIconHeader();

    drawHeader(HeaderType::TextWithInfo, "", false);
    lastHeaderBleState = bleState;
    lastHeaderSd  = sd_mounted;
  }
}

static bool listUiValid = false;
static bool listLoadedOnce = false;
static int  lastListCount = -1;
static int  lastListStartItem = -1;
static int  lastListMaxVisible = -1;
static bool lastListHasScrollbar = false;
static int  lastListHeaderBottom = 0;
static int  lastListHintY = 0;
static int  lastListTop = 0;
static int  lastListRowStride = 0;
static int  lastListRw = 0;
static int  lastListRh = 0;

static void calcListLayout(int& headerBottom, int& hintY, int& listTop,
                           int& rowStride, int& maxVisibleItems,
                           bool& hasScrollbar, int& rw, int& rh) {
  const int barY = TOOLBAR_Y;
  headerBottom = barY + TEXT_HEADER_H + 1;
  rowStride = LIST_ROW_H + LIST_ROW_GAP;

  const int contentBottom = DISPLAY_HEIGHT - duckyFooterReserve();
  const int hintBandH = 18;
  hintY = headerBottom + 6;
  const int listAreaTop = headerBottom + hintBandH;
  const int listAreaH = contentBottom - listAreaTop - 4;

  maxVisibleItems = listAreaH / rowStride;
  if (maxVisibleItems < 1) maxVisibleItems = 1;

  const int itemCount = (int)items.size();
  const int viewportH = maxVisibleItems * rowStride - LIST_ROW_GAP;
  if (itemCount > 0 && itemCount < maxVisibleItems) {
    const int blockH = itemCount * rowStride - LIST_ROW_GAP;
    listTop = listAreaTop + (listAreaH - blockH) / 2;
  } else {
    listTop = listAreaTop + (listAreaH - viewportH) / 2;
  }

  hasScrollbar = (itemCount > maxVisibleItems);
  rw = DISPLAY_WIDTH - 2*PADDING - (hasScrollbar ? 12 : 0);
  rh = LIST_ROW_H - 2;
}

static void calcListWindow(int maxVisibleItems, int& startItem, int& endItem) {
  startItem = max(0, sel - maxVisibleItems / 2);
  endItem = min((int)items.size(), startItem + maxVisibleItems);
}

static void drawListHints(int hintY) {

  tft.fillRect(0, hintY - 1, DISPLAY_WIDTH, 14, COL_BG);
  if (items.empty()) return;

  tft.setTextFont(1);
  tft.setTextColor(UI_TEXT, COL_BG);

  char counter[20];
  snprintf(counter, sizeof(counter), "%d/%d", sel + 1, (int)items.size());
  int counterWidth = tft.textWidth(counter, 1);
  tft.drawString(counter, DISPLAY_WIDTH - counterWidth - PADDING, hintY, 1);

  tft.drawString("Double-tap to execute", PADDING, hintY, 1);
}

static void drawListScrollbar(int listTop, int rowStride, int maxVisibleItems, int startItem) {
  if ((int)items.size() <= maxVisibleItems) return;
  int scrollBarX = DISPLAY_WIDTH - 8;
  const int viewportH = maxVisibleItems * rowStride - LIST_ROW_GAP;
  int scrollBarHeight = viewportH - 8;
  int scrollBarY = listTop + 4;
  int indicatorHeight = (maxVisibleItems * scrollBarHeight) / (int)items.size();
  int indicatorY = scrollBarY + (startItem * (scrollBarHeight - indicatorHeight)) / (int)items.size();

  tft.fillRect(scrollBarX, scrollBarY, 4, scrollBarHeight, UI_LINE);
  tft.fillRect(scrollBarX, indicatorY, 4, indicatorHeight, COL_ACCENT);
}

static void drawListRow(int i, int startItem, int listTop, int rowStride,
                        int rx, int rw, int rh) {
  const int maxVisibleItems = lastListMaxVisible;
  const int endItem = min((int)items.size(), startItem + maxVisibleItems);
  if (i < startItem || i >= endItem) return;

  int y = listTop + (i - startItem) * rowStride;
  bool selected = (i == sel);
  uint16_t body = selected ? COL_ROWSEL : COL_BG;
  uint16_t border = selected ? COL_ACCENT : UI_LINE;

  int wedgeX = rx - 8; if (wedgeX < 0) wedgeX = 0;
  tft.fillRect(wedgeX, y, rx - wedgeX, rh, COL_BG);

  tft.fillRoundRect(rx, y, rw, rh, 6, body);
  tft.drawRoundRect(rx, y, rw, rh, 6, border);

  if (selected) {
    tft.fillTriangle(rx - 6, y + rh/2 - 4, rx - 6, y + rh/2 + 4, rx - 2, y + rh/2, COL_ACCENT);
  }

  tft.setTextColor(COL_FG, body);
  tft.setTextFont(2);
  String name = fitText(items[i].name, rw - 40, 2);
  tft.drawString(name, rx + 12, y + 6, 2);

  uint32_t kb = (items[i].size + 1023) / 1024;
  char sbuf[16];
  snprintf(sbuf, sizeof(sbuf), "%lukB", (unsigned long)kb);
  tft.setTextFont(1);
  int sx = rx + rw - tft.textWidth(sbuf, 1) - 8;
  tft.setTextColor(selected ? UI_OK : UI_TEXT, body);
  tft.drawString(sbuf, sx, y + 8, 1);

  tft.setTextFont(1);
  String desc = items[i].desc.length() ? items[i].desc : "(no description)";
  desc = fitText(desc, rw - 24, 1);
  tft.setTextColor(UI_TEXT, body);
  tft.drawString(desc, rx + 12, y + 26, 1);
}

static void drawListContentFull() {
  const int rx = PADDING;

  int headerBottom, hintY, listTop, rowStride, maxVisibleItems, rw, rh;
  bool hasScrollbar;
  calcListLayout(headerBottom, hintY, listTop, rowStride, maxVisibleItems, hasScrollbar, rw, rh);

  tft.fillRect(0, headerBottom, DISPLAY_WIDTH, DISPLAY_HEIGHT - headerBottom - duckyFooterReserve(), COL_BG);

  int startItem, endItem;
  calcListWindow(maxVisibleItems, startItem, endItem);

  drawListScrollbar(listTop, rowStride, maxVisibleItems, startItem);

  int y = listTop;
  for (int i = startItem; i < endItem; ++i) {
    bool selected = (i == sel);
    uint16_t body = selected ? COL_ROWSEL : COL_BG;
    uint16_t border = selected ? COL_ACCENT : UI_LINE;

    tft.fillRoundRect(rx, y, rw, rh, 6, body);
    tft.drawRoundRect(rx, y, rw, rh, 6, border);
    if (selected) {
      tft.fillTriangle(rx - 6, y + rh/2 - 4, rx - 6, y + rh/2 + 4, rx - 2, y + rh/2, COL_ACCENT);
    }

    tft.setTextColor(COL_FG, body);
    tft.setTextFont(2);
    String name = fitText(items[i].name, rw - 40, 2);
    tft.drawString(name, rx + 12, y + 6, 2);

    uint32_t kb = (items[i].size + 1023) / 1024;
    char sbuf[16];
    snprintf(sbuf, sizeof(sbuf), "%lukB", (unsigned long)kb);
    tft.setTextFont(1);
    int sx = rx + rw - tft.textWidth(sbuf, 1) - 8;
    tft.setTextColor(selected ? UI_OK : UI_TEXT, body);
    tft.drawString(sbuf, sx, y + 8, 1);

    tft.setTextFont(1);
    String desc = items[i].desc.length() ? items[i].desc : "(no description)";
    desc = fitText(desc, rw - 24, 1);
    tft.setTextColor(UI_TEXT, body);
    tft.drawString(desc, rx + 12, y + 26, 1);

    y += rowStride;
  }

  drawListHints(hintY);

  listUiValid = true;
  lastListCount = (int)items.size();
  lastListStartItem = startItem;
  lastListMaxVisible = maxVisibleItems;
  lastListHasScrollbar = hasScrollbar;
  lastListHeaderBottom = headerBottom;
  lastListHintY = hintY;
  lastListTop = listTop;
  lastListRowStride = rowStride;
  lastListRw = rw;
  lastListRh = rh;
}

static void updateListSelection(int newSel) {
  if (newSel == sel) return;
  int oldSel = sel;
  sel = newSel;
  clampSel();

  if (!sd_mounted || items.empty() || !listUiValid) {
    drawListPage(false);
    return;
  }

  int headerBottom, hintY, listTop, rowStride, maxVisibleItems, rw, rh;
  bool hasScrollbar;
  calcListLayout(headerBottom, hintY, listTop, rowStride, maxVisibleItems, hasScrollbar, rw, rh);

  int startItem, endItem;
  calcListWindow(maxVisibleItems, startItem, endItem);

  if ((int)items.size() != lastListCount ||
      startItem != lastListStartItem ||
      maxVisibleItems != lastListMaxVisible ||
      hasScrollbar != lastListHasScrollbar ||
      headerBottom != lastListHeaderBottom ||
      hintY != lastListHintY ||
      listTop != lastListTop ||
      rowStride != lastListRowStride ||
      rw != lastListRw ||
      rh != lastListRh) {
    drawListContentFull();
    return;
  }

  const int rx = PADDING;
  drawListRow(oldSel, startItem, listTop, rowStride, rx, rw, rh);
  drawListRow(sel,    startItem, listTop, rowStride, rx, rw, rh);
  drawListHints(hintY);
}

static void drawListPage(bool forceReload) {
  dialog = Dialog::None;

  mountSD();

  drawHeader(HeaderType::IconBar);
  drawHeader(HeaderType::TextWithInfo, "BLE Ducky");

  listUiValid = false;

  if (!sd_mounted) {
    listLoadedOnce = false;
    tft.setTextColor(COL_FG, COL_BG);
    tft.drawCentreString("SD not mounted", DISPLAY_WIDTH/2, DISPLAY_HEIGHT/2 - 8, 2);
  } else {
    if (forceReload || !listLoadedOnce) {
      reloadList();
      listLoadedOnce = true;
    }
    clampSel();
    drawListContentFull();
  }
  if (featureHasTouchNavBar()) {
    duckyUpdateNavLabels();
  } else {
    FeatureUI::layoutFooter3(
      listBtns,
      "Open",    FeatureUI::ButtonStyle::Primary,
      "Refresh", FeatureUI::ButtonStyle::Secondary,
      "Exit",    FeatureUI::ButtonStyle::Secondary
    );
    for (auto& b: listBtns) FeatureUI::drawButton(b);
  }
}

static void drawDetailsPage() {

  drawHeader(HeaderType::IconBar);
  drawHeader(HeaderType::TextWithInfo, "Script details");

  tft.setTextColor(COL_FG, COL_BG);
  const int barY = TOOLBAR_Y;
  const int baseY = barY + TEXT_HEADER_H + 1;

  tft.fillRect(0, baseY, DISPLAY_WIDTH, DISPLAY_HEIGHT - baseY - duckyFooterReserve(), COL_BG);

  if (items.empty()) {
    tft.drawCentreString("No script selected", DISPLAY_WIDTH/2, baseY+24, 2);
    tft.setTextFont(1);
    tft.setTextColor(UI_TEXT, COL_BG);
    tft.drawCentreString("Use 'Refresh' to scan for scripts", DISPLAY_WIDTH/2, baseY+48, 1);
  }
  else {
    auto& it = items[sel];
    int y = baseY + 8;

    tft.setTextFont(2);

    tft.setTextColor(UI_TEXT, COL_BG);
    String displayName = fitText(it.name, DISPLAY_WIDTH - 2*PADDING, 2);
    tft.drawString(displayName, PADDING, y, 2);
    y += 24;

    tft.drawLine(PADDING, y, DISPLAY_WIDTH - PADDING, y, UI_LINE);
    y += 8;

    tft.setTextFont(2);
    tft.setTextColor(COL_FG, COL_BG);

    tft.drawString("Size:", PADDING, y, 2);
    tft.drawNumber(it.size, 100, y, 2);
    tft.drawString("bytes", 140, y, 2);
    y += 28;

    tft.drawString("Path:", PADDING, y, 2);
    tft.setTextFont(1);
    String path = fitText(it.path, DISPLAY_WIDTH - 100 - PADDING, 1);
    tft.drawString(path, 100, y + 2, 1);
    y += 28;

    tft.setTextFont(2);
    tft.drawString("Description:", PADDING, y, 2);
    y += 24;

    tft.setTextFont(1);
    String desc = it.desc.length() ? it.desc : "No description available";

    int descY = y;
    int maxWidth = DISPLAY_WIDTH - 2*PADDING;
    String remaining = desc;
    const int bottomY = DISPLAY_HEIGHT - duckyFooterReserve() - 6;
    while (remaining.length() > 0 && descY < bottomY) {
      String line;
      int consumed = wrapLineNoEllipsis(remaining, maxWidth, 1, line);
      if (consumed <= 0) break;
      tft.drawString(line, PADDING, descY, 1);
      remaining = remaining.substring(consumed);
      remaining.trim();
      descY += 14;
    }

  }
  if (featureHasTouchNavBar()) {
    duckyUpdateNavLabels();
  } else {
    FeatureUI::layoutFooter3(
      detailsBtns,
      "Execute", FeatureUI::ButtonStyle::Primary,
      "Delete",  FeatureUI::ButtonStyle::Danger,
      "Back",    FeatureUI::ButtonStyle::Secondary
    );
    for (auto& b: detailsBtns) FeatureUI::drawButton(b);
  }
}

static void drawSettingsPage() {

  drawHeader(HeaderType::IconBar);
  drawHeader(HeaderType::TextWithInfo, "BLE Ducky setup");

  tft.setTextColor(COL_FG, COL_BG);
  const int barY = TOOLBAR_Y;
  const int baseY = barY + TEXT_HEADER_H + 1;

  tft.fillRect(0, baseY, DISPLAY_WIDTH, DISPLAY_HEIGHT - baseY - duckyFooterReserve(), COL_BG);

  const int x=PADDING, w=DISPLAY_WIDTH-2*PADDING;
  int y=baseY+7;
  const char* status=duckyReady()?"Connected":(bleConnected?"Pairing":(bleAdvertising?"Ready to pair":"Offline"));
  const uint16_t statusColor=duckyReady()?UI_OK:(bleAdvertising?UI_ACCENT:UI_WARN);

  tft.fillRoundRect(x,y,w,42,7,COL_ROWSEL);
  tft.drawRoundRect(x,y,w,42,7,statusColor);
  tft.fillCircle(x+17,y+21,6,statusColor);
  tft.setTextFont(1); tft.setTextColor(UI_DIM_TEXT,COL_ROWSEL);
  tft.drawString("BLE KEYBOARD",x+31,y+7,1);
  tft.setTextFont(2); tft.setTextColor(COL_FG,COL_ROWSEL);
  tft.drawString(status,x+31,y+20,2);
  y+=49;

  const int miniGap=6, miniW=(w-miniGap)/2;
  tft.fillRoundRect(x,y,miniW,39,6,COL_ROWSEL);
  tft.drawRoundRect(x,y,miniW,39,6,UI_LINE);
  tft.setTextFont(1); tft.setTextColor(UI_DIM_TEXT,COL_ROWSEL); tft.drawString("STORAGE",x+8,y+5,1);
  tft.setTextFont(2); tft.setTextColor(sd_mounted?UI_OK:UI_WARN,COL_ROWSEL);
  tft.drawString(sd_mounted?"SD ready":"No SD",x+8,y+18,2);
  const int x2=x+miniW+miniGap;
  tft.fillRoundRect(x2,y,miniW,39,6,COL_ROWSEL);
  tft.drawRoundRect(x2,y,miniW,39,6,UI_LINE);
  tft.setTextFont(1); tft.setTextColor(UI_DIM_TEXT,COL_ROWSEL); tft.drawString("SCRIPTS",x2+8,y+5,1);
  tft.setTextFont(2); tft.setTextColor(COL_FG,COL_ROWSEL); tft.drawNumber((int)items.size(),x2+8,y+18,2);
  y+=46;

  tft.fillRoundRect(x,y,w,55,7,COL_ROWSEL);
  tft.drawRoundRect(x,y,w,55,7,COL_ACCENT);
  tft.setTextFont(1); tft.setTextColor(UI_DIM_TEXT,COL_ROWSEL);
  tft.drawString("KEYBOARD LAYOUT",x+10,y+6,1);
  tft.setTextColor(COL_ACCENT,COL_ROWSEL);
  tft.drawString("<",x+10,y+29,2);
  tft.drawString(">",x+w-20,y+29,2);
  tft.setTextDatum(TC_DATUM); tft.setTextFont(2); tft.setTextColor(COL_FG,COL_ROWSEL);
  tft.drawString(layoutName(selectedLayout),x+w/2,y+24,2);
  tft.setTextDatum(TL_DATUM);
  y+=62;

  tft.fillRoundRect(x,y,w,36,6,COL_ROWSEL);
  tft.drawRoundRect(x,y,w,36,6,UI_LINE);
  tft.setTextFont(1); tft.setTextColor(UI_DIM_TEXT,COL_ROWSEL); tft.drawString("DEVICE",x+8,y+5,1);
  tft.setTextColor(COL_FG,COL_ROWSEL); tft.drawString(DEV_NAME,x+8,y+18,1);
  tft.setTextColor(UI_TEXT,COL_BG);
  tft.drawCentreString("UP/DOWN changes layout",DISPLAY_WIDTH/2,y+42,1);

  if (featureHasTouchNavBar()) {
    duckyUpdateNavLabels();
  } else {
    FeatureUI::layoutFooter1(infoBtn, "Back", FeatureUI::ButtonStyle::Secondary);
    FeatureUI::drawButton(infoBtn);
  }
}

static void cycleKeyboardLayout(int direction) {
  int value=(int)selectedLayout;
  const int count=(int)KeyboardLayout::Count;
  do {
    value=(value+direction+count)%count;
  } while(value==(int)KeyboardLayout::Custom && !customLayoutLoaded);
  selectedLayout=(KeyboardLayout)value;
  activeLayout=selectedLayout;
  saveKeyboardLayoutPreference();
  drawSettingsPage();
}

static void drawConfirmDelete() {

  const int w = DISPLAY_WIDTH - 2*PADDING;
  const int h = 96;
  const int x = PADDING;
  const int y = DISPLAY_HEIGHT/2 - h/2;

  tft.fillRoundRect(x, y, w, h, 8, UI_FG);
  tft.drawRoundRect(x, y, w, h, 8, UI_LINE);
  tft.setTextColor(UI_TEXT, UI_FG);
  tft.drawCentreString("Delete this script?", DISPLAY_WIDTH/2, y + 10, 2);
  tft.setTextColor(UI_TEXT, UI_FG);
  tft.drawCentreString("(This cannot be undone)", DISPLAY_WIDTH/2, y + 30, 2);

  int bw = (w - 3*PADDING)/2;
  int bx1 = x + PADDING;
  int bx2 = x + 2*PADDING + bw;
  int by  = y + h - PADDING - BTN_H;

  confirmBtns[0] = { (int16_t)bx1, (int16_t)by, (int16_t)bw, (int16_t)BTN_H, "Yes", FeatureUI::ButtonStyle::Danger, false };
  confirmBtns[1] = { (int16_t)bx2, (int16_t)by, (int16_t)bw, (int16_t)BTN_H, "No",  FeatureUI::ButtonStyle::Secondary, false };

  FeatureUI::drawButton(confirmBtns[0]);
  FeatureUI::drawButton(confirmBtns[1]);
}

static bool configureDuckyAdvertising(const char* devName) {
  NimBLEDevice::setDeviceName(devName);

  // Keep the 0x1812 HID service in the primary advertisement and the complete
  // name in the scan response.  The previous packet duplicated the name but
  // omitted 0x1812, so several Android/Windows hosts discovered the DIV as a
  // generic BLE peripheral and never completed keyboard pairing.
  NimBLEAdvertisementData advData;
  advData.setFlags(0x06);
  advData.setAppearance(0x03C1);
  advData.setCompleteServices(NimBLEUUID((uint16_t)0x1812));

  NimBLEAdvertisementData scanRsp;
  scanRsp.setName(devName);

  NimBLEAdvertising* adv = NimBLEDevice::getAdvertising();
  if (!adv) {
    bleAdvertising = false;
    Serial.println(F("[ducky] advertising object unavailable"));
    return false;
  }

  adv->stop();
  adv->reset();
  adv->setAdvertisementData(advData);
  adv->setScanResponseData(scanRsp);
  adv->setAdvertisementType(BLE_GAP_CONN_MODE_UND);
  adv->setMinInterval(0x0030);
  adv->setMaxInterval(0x0060);
  bleAdvertising = adv->start();
  Serial.printf("[ducky] HID advertise=%s name='%s' service=0x1812 generation=%u\n",
                bleAdvertising ? "OK" : "FAILED", devName,
                (unsigned)bleStackGeneration());
  return bleAdvertising;
}

static bool startBleKeyboard(const char* devName) {
  // The background autoscan supervisor deliberately parks NimBLE when no BLE
  // foreground feature owns the controller.  Calling setPower() while parked
  // reaches NimBLE's uninitialised mutex and asserts.  Ducky is the only BLE
  // feature with its own HID server, so it must explicitly reacquire the stack
  // before touching any NimBLE object.
  if (!ensureBleStackReady()) {
    Serial.println("[ducky] BLE stack unavailable; HID start aborted");
    return false;
  }

  Serial.printf("[ducky] BLE stack ready, heap=%u\n", (unsigned)ESP.getFreeHeap());
  NimBLEDevice::setPower(ESP_PWR_LVL_P9);
  NimBLEDevice::setDeviceName(devName);

  NimBLEServer* server = NimBLEDevice::createServer();
  if (!server) {
    Serial.println(F("[ducky] server creation failed"));
    return false;
  }
  server->setCallbacks(&s_serverCallbacks, false);

  hid = new NimBLEHIDDevice(server);
  if (!hid) {
    Serial.println(F("[ducky] HID allocation failed"));
    return false;
  }
  inputChr  = hid->inputReport(1);
  consumerInputChr = hid->inputReport(2);
  outputChr = hid->outputReport(1);
  inputChr->setCallbacks(&s_inputCallbacks);
  outputChr->setCallbacks(&s_outputCallbacks);

  static const uint8_t REPORT_MAP[] = {
    0x05,0x01, 0x09,0x06, 0xA1,0x01,
    0x85,0x01, 0x05,0x07, 0x19,0xE0, 0x29,0xE7,
    0x15,0x00, 0x25,0x01, 0x75,0x01, 0x95,0x08, 0x81,0x02,
    0x75,0x08, 0x95,0x01, 0x81,0x01,
    0x75,0x08, 0x95,0x06, 0x15,0x00, 0x25,0x73, 0x05,0x07, 0x19,0x00, 0x29,0x73, 0x81,0x00,
    0x05,0x08, 0x19,0x01, 0x29,0x05, 0x95,0x05, 0x75,0x01, 0x91,0x02,
    0x95,0x01, 0x75,0x03, 0x91,0x01,
    0xC0,
    0x05,0x0C, 0x09,0x01, 0xA1,0x01, 0x85,0x02,
    0x15,0x00, 0x26,0xFF,0x03, 0x19,0x00, 0x2A,0xFF,0x03,
    0x75,0x10, 0x95,0x01, 0x81,0x00, 0xC0
  };

  hid->manufacturer()->setValue("MK SIGNAL");
  hid->pnp(0x02, 0x303A, 0x4001, 0x0185);
  hid->hidInfo(0x00, 0x02);
  hid->reportMap((uint8_t*)REPORT_MAP, sizeof(REPORT_MAP));
  hid->startServices();
  hid->setBatteryLevel(100);

  NimBLEDevice::setSecurityAuth(true, false, true);
  NimBLEDevice::setSecurityIOCap(BLE_HS_IO_NO_INPUT_OUTPUT);

  hidStackGeneration = bleStackGeneration();
  bleConnected = false;
  bleSubscribed = false;
  bleEncrypted = false;
  return configureDuckyAdvertising(devName);
}

static bool resumeBleKeyboard(const char* devName) {
  if (!ensureBleStackReady()) return false;

  const uint32_t generation = bleStackGeneration();
  if (!hid) return startBleKeyboard(devName);

  NimBLEServer* server = NimBLEDevice::getServer();
  if (!server || !inputChr || !outputChr) {
    Serial.println(F("[ducky] retained HID state is incomplete"));
    return false;
  }

  server->setCallbacks(&s_serverCallbacks, false);
  inputChr->setCallbacks(&s_inputCallbacks);
  outputChr->setCallbacks(&s_outputCallbacks);

  if (hidStackGeneration != generation) {
    // deinit(false) intentionally retains C++ objects to save heap churn, but
    // a freshly started NimBLE host has an empty GATT registry.  Re-register
    // the retained HID services before advertising them again.
    Serial.printf("[ducky] restoring HID GATT generation %u -> %u\n",
                  (unsigned)hidStackGeneration, (unsigned)generation);
    bleConnected = false;
    bleSubscribed = false;
    bleEncrypted = false;
    server->serviceChanged();
    hidStackGeneration = generation;
  }

  NimBLEDevice::setPower(ESP_PWR_LVL_P9);
  return configureDuckyAdvertising(devName);
}

void enter() {

#ifdef SD_CD
  pinMode(SD_CD, INPUT_PULLUP);
  if (digitalRead(SD_CD)) {

    tft.fillRect(0, TOOLBAR_Y - 1, DISPLAY_WIDTH, DISPLAY_HEIGHT - (TOOLBAR_Y - 1), COL_BG);
    tft.setTextColor(UI_WARN, COL_BG);
    tft.setTextFont(2);
    tft.drawCentreString("SD card not detected", DISPLAY_WIDTH/2, DISPLAY_HEIGHT/2 - 14, 2);
    tft.setTextFont(1);
    tft.setTextColor(UI_TEXT, COL_BG);
    tft.drawCentreString("Returning...", DISPLAY_WIDTH/2, DISPLAY_HEIGHT/2 + 10, 1);
    delay(900);
    ducky_active = false;
    feature_exit_requested = true;
    return;
  }
#endif

  ducky_active = true;
  lastScriptTapIndex = -1;
  lastScriptTapTime = 0;
  setTouchButtonInputEnabled(true);
  ui_busy = false;
  dialog = Dialog::None;
  ui_inited = false;

  // Reacquire NimBLE before startBleKeyboard() or before reusing a retained
  // HID/advertising object after BLEDevice::deinit(false).  This is safe both
  // on a cold entry and after autoscan has parked the controller.
  if (!ensureBleStackReady()) {
    Serial.println("[ducky] entry cancelled: BLE stack could not be started");
    tft.fillRect(0, TOOLBAR_Y - 1, DISPLAY_WIDTH,
                 DISPLAY_HEIGHT - (TOOLBAR_Y - 1), COL_BG);
    tft.setTextColor(UI_WARN, COL_BG);
    tft.setTextFont(2);
    tft.drawCentreString("BLE unavailable", DISPLAY_WIDTH / 2,
                         DISPLAY_HEIGHT / 2 - 14, 2);
    tft.setTextFont(1);
    tft.setTextColor(UI_TEXT, COL_BG);
    tft.drawCentreString("Returning...", DISPLAY_WIDTH / 2,
                         DISPLAY_HEIGHT / 2 + 10, 1);
    delay(900);
    ducky_active = false;
    feature_exit_requested = true;
    return;
  }

  if (!resumeBleKeyboard(DEV_NAME)) {
    Serial.println(F("[ducky] entry cancelled: HID advertising failed"));
    tft.fillRect(0, TOOLBAR_Y - 1, DISPLAY_WIDTH,
                 DISPLAY_HEIGHT - (TOOLBAR_Y - 1), COL_BG);
    tft.setTextColor(UI_WARN, COL_BG);
    tft.setTextFont(2);
    tft.drawCentreString("BLE HID start failed", DISPLAY_WIDTH / 2,
                         DISPLAY_HEIGHT / 2 - 14, 2);
    tft.setTextFont(1);
    tft.setTextColor(UI_TEXT, COL_BG);
    tft.drawCentreString("Restart Bluetooth and retry", DISPLAY_WIDTH / 2,
                         DISPLAY_HEIGHT / 2 + 10, 1);
    delay(1200);
    ducky_active = false;
    feature_exit_requested = true;
    return;
  }

  if (hid && inputChr && duckyReady()) {
    KbdReport idle{}; inputChr->setValue((uint8_t*)&idle, sizeof(idle)); inputChr->notify();
  }

  if (!mountSD() || !ensureDuckyDir()) {
    tft.fillRect(0, TOOLBAR_Y - 1, DISPLAY_WIDTH,
                 DISPLAY_HEIGHT - (TOOLBAR_Y - 1) - duckyFooterReserve(), COL_BG);
    tft.setTextColor(UI_WARN, COL_BG);
    tft.setTextFont(2);
    tft.drawCentreString("SD mount failed", DISPLAY_WIDTH/2, DISPLAY_HEIGHT/2 - 14, 2);
    tft.setTextFont(1);
    tft.setTextColor(UI_TEXT, COL_BG);
    tft.drawCentreString("Returning...", DISPLAY_WIDTH/2, DISPLAY_HEIGHT/2 + 10, 1);
    delay(900);
    ducky_active = false;
    feature_exit_requested = true;
    return;
  }
  loadCustomKeyboardLayout();
  loadKeyboardLayoutPreference();
  Serial.printf("[ducky] keyboard layout=%s\n",layoutCode(selectedLayout));
  needsRedraw = true;
}

bool active() { return ducky_active; }

void exit() {

  ducky_active = false;
  setTouchButtonInputEnabled(false);
  setTouchNavLabels(nullptr, nullptr, nullptr, nullptr, nullptr);
  ui_busy = false;
  dialog = Dialog::None;
  uiDrawn = false;

  if (hid && inputChr && duckyReady()) {
    KbdReport idle{};
    inputChr->setValue((uint8_t*)&idle, sizeof(idle));
    inputChr->notify();
  }

  NimBLEAdvertising* adv = NimBLEDevice::getAdvertising();
  if (adv) {
    adv->stop();

    NimBLEAdvertisementData empty;
    adv->setAdvertisementData(empty);
    adv->setScanResponseData(empty);
  }
  bleAdvertising = false;

  // End any HID session owned by this foreground feature.  Leaving a host
  // connected while the supervisor later parks NimBLE produces a stale bond
  // and makes the next entry look connected without a subscribed input report.
  NimBLEServer* server = NimBLEDevice::getServer();
  bool gapShutdownClean = true;
  if (server && server->getConnectedCount() > 0) {
    // NimBLEServer normally restarts advertising after a disconnect.  That
    // happens after our callback returns and used to race deinit(false).
    server->advertiseOnDisconnect(false);
    const std::vector<uint16_t> peers = server->getPeerDevices();
    const uint32_t callbackBaseline = bleDisconnectEvents;
    uint32_t expectedCallbacks = 0;
    for (uint16_t handle : peers) {
      const int rc = server->disconnect(handle);
      Serial.printf("[ducky] disconnect handle=%u rc=%d\n",
                    (unsigned)handle, rc);
      if (rc == 0) ++expectedCallbacks;
    }

    // GAP termination is asynchronous.  getConnectedCount() becomes zero
    // before NimBLE finishes the application callback, so both conditions are
    // required before the host/controller can be stopped safely.
    const uint32_t deadline = millis() + 2500u;
    while ((server->getConnectedCount() > 0 ||
            (uint32_t)(bleDisconnectEvents - callbackBaseline) < expectedCallbacks) &&
           static_cast<int32_t>(deadline - millis()) > 0) {
      delay(10);
    }

    gapShutdownClean = server->getConnectedCount() == 0 &&
        (uint32_t)(bleDisconnectEvents - callbackBaseline) >= expectedCallbacks;
    if (!gapShutdownClean) {
      // Never tear down NimBLE underneath an unfinished GAP callback.  The
      // disconnect request remains active and will finish asynchronously.
      Serial.println(F("[ducky] GAP disconnect timeout; controller kept alive"));
    } else {
      Serial.println(F("[ducky] host disconnected"));
      // Let the host task return from BLE_GAP_EVENT_DISCONNECT completely.
      delay(90);
    }
  }

  // BLE HID is foreground-owned.  Stop the host/controller even when GAP
  // reported a clean disconnect so iOS, macOS and Windows cannot keep the DIV
  // shown as an actively connected keyboard after leaving this feature.
  // clearAll=false preserves the reusable NimBLE objects expected by the
  // firmware supervisor; resumeBleKeyboard() restores them on the next entry.
  if (gapShutdownClean && NimBLEDevice::getInitialized()) {
    // Advertising stop is also delivered through the NimBLE host task.  Give
    // it one scheduler window before stopping the port, even when no phone was
    // connected during this session.
    delay(40);
    NimBLEDevice::deinit(false);
    Serial.printf("[ducky] HID controller stopped=%s\n",
                  NimBLEDevice::getInitialized() ? "NO" : "YES");
  } else if (!gapShutdownClean) {
    Serial.println(F("[ducky] HID controller shutdown deferred"));
  }
  bleConnected = false;
  bleSubscribed = false;
  bleEncrypted = false;
}
void setup() {
  ducky_active = false;
  tft.setTextDatum(TL_DATUM);
  mountSD(); ensureDuckyDir();

  ui_inited = false; needsRedraw = true; ui_busy = false; dialog = Dialog::None; uiDrawn = false;
}

void loop() {

  if (!ui_inited) {

    tft.fillRect(0, TOOLBAR_Y - 1, DISPLAY_WIDTH,
                 DISPLAY_HEIGHT - (TOOLBAR_Y - 1) - duckyFooterReserve(), COL_BG);
    sel = 0;
    page = Page::List;
    uiDrawn = false;
    drawListPage();
    ui_inited = true;
    needsRedraw = false;
    refreshHeaderStatusIfNeeded(true);
  }

  updateIconAnimation();

  refreshHeaderStatusIfNeeded(false);
  if (feature_exit_requested) return;

  if (ui_busy) { delay(5); return; }

  if (dialog != Dialog::ConfirmDelete) {
    if (page == Page::List) {
      if (featureHasTouchNavBar()) {
        if (pcfPressedEdge(ebUp))   { if (sel > 0) { updateListSelection(sel - 1); } return; }
        if (pcfPressedEdge(ebDown)) { if (sel < (int)items.size() - 1) { updateListSelection(sel + 1); } return; }
        if (pcfPressedEdge(ebRight)) { drawListPage(true); return; }
        if (pcfPressedEdge(ebSelect)) {
          if (!items.empty()) { page = Page::Details; drawDetailsPage(); }
          return;
        }
        if (pcfPressedEdge(ebLeft)) {
          ducky_active = false;
          feature_exit_requested = true;
          return;
        }
      } else {
        if (pcfPressedEdge(ebUp))   { if (sel>0){ updateListSelection(sel - 1); } else {  } return; }
        if (pcfPressedEdge(ebDown)) { if (sel < (int)items.size()-1){ updateListSelection(sel + 1); } return; }
        if (pcfPressedEdge(ebRight) || pcfPressedEdge(ebSelect)) { if (!items.empty()) { page = Page::Details; drawDetailsPage(); } return; }
        if (pcfPressedEdge(ebLeft)) {
          ducky_active = false;
          feature_exit_requested = true;
          return;
        }
      }
    } else if (page == Page::Details) {
      if (featureHasTouchNavBar()) {
        if (pcfPressedEdge(ebUp)) { page = Page::List; drawListPage(false); return; }
        if (pcfPressedEdge(ebDown)) {
          dialog = Dialog::ConfirmDelete;
          drawConfirmDelete();
          return;
        }
        if (pcfPressedEdge(ebRight) || pcfPressedEdge(ebSelect)) {
          if (!duckyReady()) {
            tft.fillRoundRect(PADDING, duckyToastTop(), DISPLAY_WIDTH - 2*PADDING, 22, 6, COL_WARN);
            tft.setTextColor(COL_BG, COL_WARN);
            tft.drawCentreString("Pair keyboard first", DISPLAY_WIDTH/2, duckyToastCenterY(), 2);
          } else if (!items.empty()) {
            bool ok = execFile(items[sel].path);
            drawDetailsPage();
            if (!ok) {
              tft.fillRoundRect(PADDING, duckyToastTop(), DISPLAY_WIDTH - 2*PADDING, 22, 6, COL_WARN);
              tft.setTextColor(COL_BG, COL_WARN);
              tft.drawCentreString(String("Error: ") + lastError, DISPLAY_WIDTH/2, duckyToastCenterY(), 2);
            }
          }
          return;
        }
        if (pcfPressedEdge(ebLeft)) { page = Page::List; drawListPage(false); return; }
      } else {
        if (pcfPressedEdge(ebRight) || pcfPressedEdge(ebSelect)) {
          if (!duckyReady()) {
            tft.fillRoundRect(PADDING, duckyToastTop(), DISPLAY_WIDTH - 2*PADDING, 22, 6, COL_WARN);
            tft.setTextColor(COL_BG, COL_WARN);
            tft.drawCentreString("Pair keyboard first", DISPLAY_WIDTH/2, duckyToastCenterY(), 2);
          } else if (!items.empty()) {
            bool ok = execFile(items[sel].path);
            drawDetailsPage();
            if (!ok) {
              tft.fillRoundRect(PADDING, duckyToastTop(), DISPLAY_WIDTH - 2*PADDING, 22, 6, COL_WARN);
              tft.setTextColor(COL_BG, COL_WARN);
              tft.drawCentreString(String("Error: ") + lastError, DISPLAY_WIDTH/2, duckyToastCenterY(), 2);
            }
          }
          return;
        }
        if (pcfPressedEdge(ebLeft)) { page = Page::List; drawListPage(false); return; }
      }
    } else if (page == Page::Settings) {
      if (pcfPressedEdge(ebLeft)) { page = Page::List; drawListPage(false); return; }
      if (pcfPressedEdge(ebUp)) { cycleKeyboardLayout(-1); return; }
      if (pcfPressedEdge(ebDown) || pcfPressedEdge(ebRight) || pcfPressedEdge(ebSelect)) {
        cycleKeyboardLayout(1); return;
      }
    }
  }
Tap tap = readTap();
  if (!tap.has) { delay(5); return; }

  if (dialog == Dialog::ConfirmDelete) {
    int id = FeatureUI::hit(confirmBtns, 2, tap.x, tap.y);
    if (id == 0) {
      SdFileIoGuard io(1500);
      if (io.locked() && !items.empty() && mountSD() && SD.exists(items[sel].path)) {
        SD.remove(items[sel].path.c_str());
        reloadList();
        page = Page::List;
        dialog = Dialog::None;
        drawListPage(false);
      } else {
        dialog = Dialog::None;
        drawDetailsPage();
      }
    } else if (id == 1) {
      dialog = Dialog::None;
      drawDetailsPage();
    } else {

    }
    return;
  }

  handleIconTouch(tap.x, tap.y);

  if (page == Page::List) {
    bool handledFooter = false;
    if (!featureHasTouchNavBar()) {
      int id = FeatureUI::hit(listBtns, 3, tap.x, tap.y);
      if (id == 0) {
        if (!items.empty()) { page = Page::Details; drawDetailsPage(); }
        handledFooter = true;
      } else if (id == 1) {
        drawListPage(true);
        handledFooter = true;
      } else if (id == 2) {
        ducky_active = false;
        feature_exit_requested = true;
        return;
      }
    }

    if (!handledFooter) {
      int headerBottom, hintY, listTop, rowStride, maxVisibleItems, rw, rh;
      bool hasScrollbar;
      calcListLayout(headerBottom, hintY, listTop, rowStride, maxVisibleItems, hasScrollbar, rw, rh);

      int y = tap.y;
      if (y >= listTop && y < DISPLAY_HEIGHT - duckyFooterReserve()) {
        int startItem, endItem;
        calcListWindow(maxVisibleItems, startItem, endItem);
        int clickedRow = (y - listTop) / rowStride;
        int actualIdx = startItem + clickedRow;

        if (actualIdx >= 0 && actualIdx < (int)items.size()) {
          const int executeIdx = registerScriptRowTap(actualIdx, tap);
          if (executeIdx >= 0 && executeIdx < (int)items.size()) {
            if (!duckyReady()) {
              tft.fillRoundRect(PADDING, duckyToastTop(), DISPLAY_WIDTH - 2*PADDING, 22, 6, COL_WARN);
              tft.setTextColor(COL_BG, COL_WARN);
              tft.drawCentreString("Pair keyboard first", DISPLAY_WIDTH/2, duckyToastCenterY(), 2);
            } else {
              sel = executeIdx;
              bool ok = execFile(items[executeIdx].path);
              drawListPage(false);
              if (!ok) {
                tft.fillRoundRect(PADDING, duckyToastTop(), DISPLAY_WIDTH - 2*PADDING, 22, 6, COL_WARN);
                tft.setTextColor(COL_BG, COL_WARN);
                tft.drawCentreString(String("Error: ") + lastError, DISPLAY_WIDTH/2, duckyToastCenterY(), 2);
              }
            }
          } else {
            updateListSelection(actualIdx);
          }
        }
      }
    }

  } else if (page == Page::Details) {
    if (!featureHasTouchNavBar()) {
      int id = FeatureUI::hit(detailsBtns, 3, tap.x, tap.y);
      if (id == 0) {
        if (!duckyReady()) {
          tft.fillRoundRect(PADDING, duckyToastTop(), DISPLAY_WIDTH - 2*PADDING, 22, 6, COL_WARN);
          tft.setTextColor(COL_BG, COL_WARN);
          tft.drawCentreString("Pair keyboard first", DISPLAY_WIDTH/2, duckyToastCenterY(), 2);
        } else if (!items.empty()) {
          bool ok = execFile(items[sel].path);
          drawDetailsPage();
          if (!ok) {
            tft.fillRoundRect(PADDING, duckyToastTop(), DISPLAY_WIDTH - 2*PADDING, 22, 6, COL_WARN);
            tft.setTextColor(COL_BG, COL_WARN);
            tft.drawCentreString(String("Error: ") + lastError, DISPLAY_WIDTH/2, duckyToastCenterY(), 2);
          }
        }
      } else if (id == 1) {
        dialog = Dialog::ConfirmDelete;
        drawConfirmDelete();
      } else if (id == 2) {
        page = Page::List; drawListPage(false);
      }
    }
  } else if (page == Page::Settings) {
    if (!featureHasTouchNavBar()) {
      int id = FeatureUI::hit(&infoBtn, 1, tap.x, tap.y);
      if (id == 0) {
        page = Page::List; drawListPage(false);
      }
    }
  }

  delay(5);
}

}
