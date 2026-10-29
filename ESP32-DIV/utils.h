#ifndef UTILS_H
#define UTILS_H

#include <SD.h>
#include <SPI.h>
#include <TFT_eSPI.h>
#include <WiFi.h>
#include <XPT2046_Touchscreen.h>
#include "SettingsStore.h"
#include "Touchscreen.h"
#include "shared.h"

#ifdef TFT_GREEN
#undef TFT_GREEN
#endif
#define TFT_GREEN GREEN

#include "LocalizedTFT.h"
extern LocalizedTFT tft;

// Top toolbars use legacy 16x16 bitmaps. Render them at 20x20 and give each
// control a finger-sized hit area without changing the compact menu layout.
constexpr int16_t TOOLBAR_ICON_SOURCE_SIZE = 16;
constexpr int16_t TOOLBAR_ICON_VISUAL_SIZE = 20;
constexpr uint16_t TOOLBAR_ANIMATION_MS = 60;
void drawToolbarIcon(int16_t x, int16_t y, const uint8_t* bitmap,
                     uint16_t color);
bool toolbarIconHitX(int16_t touchX, int16_t iconX);
bool toolbarIconHitY(int16_t touchY, int16_t iconY);
bool toolbarIconHit(int16_t touchX, int16_t touchY,
                    int16_t iconX, int16_t iconY);

// Obfuscated-string helpers (XOR decode, then print).
void tftPrintObf(const uint8_t* data, size_t len, uint8_t key = k0());
void tftPrintlnObf(const uint8_t* data, size_t len, uint8_t key = k0());
void serialPrintObf(const uint8_t* data, size_t len, bool newline = false, uint8_t key = k0());

void updateStatusBar();
/** Draw the low-cost Ghost Fleet wallpaper used by menu grids. It performs no
 * allocations and touches only the TFT. */
void drawGhostFleetWallpaper();
/** Draw an accent-aware raised plate. The caller draws its icon/text over the
 * returned neutral face. */
void drawArmoredMenuPanel(int16_t x, int16_t y, int16_t w, int16_t h,
                          bool focused = false, bool pressed = false);
enum class MkMainMenuIcon : uint8_t {
  Wifi,
  Radio24,
  More,
  Settings,
  Bluetooth,
  SubGhz,
  Tools,
  About
};
/** Draw the MK SIGNAL 24 px main-menu icon set using TFT primitives. */
void drawMkMainMenuIcon(MkMainMenuIcon icon, int16_t centerX, int16_t topY,
                        uint16_t color);
/** Stop all in-flight background radio work before a foreground feature owns it. */
void pauseBackgroundRadioTasks();
/** Resume automatic scanners once per real main-menu residency. Repeated
 *  displayMenu() redraws are intentionally no-ops so they cannot repeatedly
 *  unmount FatFS or restart background workers. */
bool resumeBackgroundRadioTasksAtMainMenu();
/** Keep a known-good mounted SD session through the next main-menu handoff
 *  and foreground-entry fence. Used after memory-heavy internal-WiFi features
 *  so Auto Scan cannot force an immediate FAT/VFS unregister/register cycle
 *  on a fragmented heap. */
void preserveMountedSdAcrossNextUiHandoff();
/** Serialize automatic Wi-Fi/BLE scans. Both scanners are enabled by the
 *  single Auto Scan setting, but running them together exhausts contiguous
 *  heap and can make the next FatFS mount fail with ESP_ERR_NO_MEM. */
bool autoScanTryClaimWifi();
void autoScanReleaseWifi();
bool autoScanTryClaimBle();
void autoScanReleaseBle();
/** Close any inherited SD/FatFS session before a foreground feature changes
 *  Wi-Fi, GPIO or SPI ownership. The next storage consumer mounts afresh.
 *  Idempotent and safe to call from every feature entry fence. */
bool parkSdForFeatureTransition();
/** Detach every promiscuous callback, abort scans and optionally stop the
 *  internal Wi-Fi driver. This is the storage/peripheral transition fence;
 *  it is safe when Wi-Fi was never initialised. */
void quiesceWifiSubsystem(bool powerOff = true);
float readBatteryVoltage();
/** Initialise/read the DIV 2.1 IP5306 coarse battery gauge. */
bool ip5306Init();
int readBatteryPercent();
bool ip5306IsCharging();
float readInternalTemperature();
bool isSDCardAvailable();
/** Verify the existing SD mount after a file-intensive feature. If recovery is
 *  needed, remount without SPI.end() or GPIO resets so live RF24 objects stay
 *  valid. */
bool sdValidateOrSoftRemount();
/** Reserve the mounted SD filesystem for a long-running foreground writer.
 *  Calls from the same task remain recursive; background health probes wait
 *  instead of issuing SPI commands in the middle of a file transaction. */
bool sdBeginFileIoSession(uint32_t timeoutMs = 1000);
/** Release a reservation obtained with sdBeginFileIoSession(). */
void sdEndFileIoSession();
/** Scoped reservation for SD transactions. The recursive mutex makes nested
 *  helpers safe while guaranteeing every early return releases the bus. */
class SdFileIoGuard {
 public:
  explicit SdFileIoGuard(uint32_t timeoutMs = 1000)
      : locked_(sdBeginFileIoSession(timeoutMs)) {}
  ~SdFileIoGuard() { if (locked_) sdEndFileIoSession(); }
  bool locked() const { return locked_; }
  SdFileIoGuard(const SdFileIoGuard&) = delete;
  SdFileIoGuard& operator=(const SdFileIoGuard&) = delete;

 private:
  bool locked_;
};
/** After another SPI device, restore the SD pin binding and mount a fresh FAT
 *  session for a storage consumer. */
void restoreSdAfterSharedSpi();
/** Release a foreground SPI radio without mounting SD synchronously.  This is
 *  the normal feature-exit path: menu rendering resumes immediately and the
 *  next storage feature performs the (potentially slow) mount on demand. */
void releaseSharedSpiWithoutSdMount();
/** Reclaim shared SPI pins for CC1101/nRF. Any registered FAT volume is closed
 *  before the hardware peripheral is rebound. */
void reclaimSharedSpiBus();
/** Hold SD CS high and set a CC1101-safe SPI rate on the shared bus. */
void holdSdInactiveOnSharedSpi();
/** Park SD electrically and bind the shared SPI pins for nRF24 access. */
void prepareNrfSharedSpiBus();
/** Confirm that a selected nRF24 answers with plausible registers. Rebuilds
 *  the shared bus between retries so NFC/SD pin-state leaks are recoverable. */
bool verifyNrfSharedSpiDevice(uint8_t cePin, uint8_t csnPin, uint8_t attempts = 3);
void neopixelInit();
void neopixelClear();
void neopixelApplySettings();
/** Four-pixel red/amber/green RF-strength bar. */
void neopixelShowSignalStrength(uint8_t pct);
/** Low-power battery bar used while browsing menus. */
void neopixelShowBatteryStatus();
/** Low-brightness idle colour derived from Settings -> Accent. */
void neopixelShowAccentIdle();
void neopixelShowProgressColor(uint8_t pct, uint8_t r, uint8_t g, uint8_t b);
void neopixelShowActivityColor(uint8_t r, uint8_t g, uint8_t b, bool pulse);
enum class FeatureFeedbackKind : uint8_t {
  Radio24,
  WiFi,
  Bluetooth,
  SubGhz,
  Nfc,
  Ir,
  Gps,
};
/** One launch beep plus an ongoing low-priority LED heartbeat. Feature-specific
 *  LED updates temporarily override the generic heartbeat. */
void featureStartCue(FeatureFeedbackKind kind, uint8_t patternId = 0);
void featureFeedbackStop();
/** Drive the passive DIV V2.1 buzzer briefly. The pin is returned to high
 *  impedance afterwards because GPIO2 is shared with the battery divider. */
void soundBeep(uint16_t frequencyHz = 2700, uint16_t durationMs = 60);
/** Play the two-tone startup confirmation selected by Settings. */
void soundPlayStartup();
void soundStop();
void soundPoll();
void drawStatusBar(float batteryVoltage, bool forceUpdate = false, bool bottomSeparator = false);
void startStatusBarTask();
/** Request a status bar pass on the next update (e.g. after SD or ward state changes). */
void requestStatusBarRedraw();

/** Init NimBLE once (releases unused Classic BT RAM first). Safe to call repeatedly. */
bool ensureBleStackReady();

/** Allow another SD mount attempt (clears v1 boot "gave up" latch). */
void sdRetryMount();

extern bool feature_exit_requested;

extern void setBrightness(uint8_t value);
bool isButtonPressed(int buttonPin);
/** True while the PCF8574 button for this pin is held (no touch nav). */
bool isPhysicalButtonPressed(int buttonPin);
/** Debounced, cached PCF8574 read shared safely with the IP5306 I2C task. */
bool readPcfButtonPressedStable(int buttonPin);
/** True while a touch nav slot for this pin is held (ignores physical buttons). */
bool isTouchNavButtonPressed(int buttonPin);
/** True once per touch nav press (ignores physical buttons). */
bool isTouchNavButtonPressedEdge(int buttonPin);
/** True once per press (touch nav edge or PCF falling edge). */
bool isButtonPressedEdge(int buttonPin);
/** Exit/back: level-sensitive so slow feature loops still catch PCF + touch nav. */
bool featureExitButtonPressed();
void setTouchButtonInputEnabled(bool enabled);
void drawTouchButtonCue();
void invalidateTouchButtonCue();
void redrawTouchButtonBar();
/** Clear touch-nav edge state after a full-screen overlay (keyboard, etc.). */
void resetTouchNavHeldState();
/** Draw the touch nav bar once (no-op if already drawn). */
void maintainTouchNavBar();
/** Pixels reserved at the bottom for the touch nav bar (0 when hidden). */
int16_t touchNavReservedHeight();
/** Lowest Y coordinate features should draw into (exclusive of nav bar). */
int16_t touchNavContentBottomY();
/** Clear feature body only; leaves the touch nav bar intact. */
void featureClearContent(uint16_t color = BLACK);
/** True when the on-screen touch nav bar is active for the current feature. */
bool featureHasTouchNavBar();
/** Override nav slots with text labels (nullptr = default icon for that slot). */
void setTouchNavLabels(const char* left, const char* down, const char* center,
                       const char* up, const char* right);
bool initPcf8574Buttons();
uint8_t getPcf8574Address();
void sdSpiInit();
bool sdMountChipSelect(uint8_t cs);

extern float currentBatteryVoltage;

void initDisplay();
void showNotification(const char* title, const char* message);
/** Warning dialog with separate Cancel and Continue actions. */
void showNotificationConfirm(const char* title, const char* message);
void hideNotification();

enum class NotificationAction : uint8_t { None, Close, Ok, Save };
void showNotificationActions(const char* title, const char* message, bool showSave);
bool isNotificationVisible();
/** Repaint the modal after a full-screen or body redraw covered it (same cached title/message). */
void notificationRedrawIfVisible();
NotificationAction notificationHandleTouch(int x, int y);
void printWrappedText(int x, int y, int maxWidth, const char* text);
void loading(int frameDelay, uint16_t color, int16_t x, int16_t y, int repeats, bool center);
void displayLogo(uint16_t color, int displayTime);
void initSDCard();

namespace AppSettingsUI{ void setup(); void loop(); }
namespace NrfTuningUI{ void setup(); void loop(); }
namespace TouchCalib{ void setup(); void loop(); }

namespace Terminal {
  void terminalSetup();
  void terminalLoop();
}

namespace SdFileManager {
  void setup();
  void loop();
  void shutdown();
}

namespace FeatureUI {
  enum class ButtonStyle : uint8_t { Primary, Secondary, Danger };

  struct Button {
    int16_t x, y, w, h;
    const char* label;
    ButtonStyle style;
    bool disabled;
  };

  constexpr int16_t FOOTER_H = 34;
  constexpr int16_t BTN_H    = 26;
  constexpr int16_t PAD_X    = 8;
  constexpr int16_t GAP_X    = 8;

  void drawFooterBg();

  void drawButtonRect(int x, int y, int w, int h,
                      const char* label,
                      ButtonStyle style,
                      bool pressed = false,
                      bool disabled = false,
                      uint8_t font = 2);

  inline void drawButton(const Button& b, bool pressed = false) {
    drawButtonRect(b.x, b.y, b.w, b.h, b.label, b.style, pressed, b.disabled);
  }

  void layoutFooter3(Button (&btns)[3],
                     const char* l0, ButtonStyle s0,
                     const char* l1, ButtonStyle s1,
                     const char* l2, ButtonStyle s2,
                     bool d0=false, bool d1=false, bool d2=false);
  void layoutFooter2(Button (&btns)[2],
                     const char* l0, ButtonStyle s0,
                     const char* l1, ButtonStyle s1,
                     bool d0=false, bool d1=false);
  void layoutFooter4(Button (&btns)[4],
                     const char* l0, ButtonStyle s0,
                     const char* l1, ButtonStyle s1,
                     const char* l2, ButtonStyle s2,
                     const char* l3, ButtonStyle s3,
                     bool d0=false, bool d1=false, bool d2=false, bool d3=false);
  void layoutFooter1(Button& btn, const char* label, ButtonStyle style, bool disabled=false);
  void layoutFooter5(Button (&btns)[5],
                     const char* l0, ButtonStyle s0,
                     const char* l1, ButtonStyle s1,
                     const char* l2, ButtonStyle s2,
                     const char* l3, ButtonStyle s3,
                     const char* l4, ButtonStyle s4,
                     bool d0=false, bool d1=false, bool d2=false, bool d3=false, bool d4=false);

  int hit(const Button* btns, int n, int x, int y);
}

#endif
