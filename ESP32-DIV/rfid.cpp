#include "rfid.h"

#include <Adafruit_PN532.h>
#include <SPI.h>
#include <Arduino.h>
#include <cstddef>
#include <cstdio>
#include <cstring>

#include "SettingsStore.h"
#include "Language.h"
#include "NrfSupervisor.h"
#include "Touchscreen.h"
#include "icon.h"
#include "utils.h"
#include "shared.h"

namespace SubGhzLifecycle {
void shutdown(bool restoreSd);
}

#ifndef RFID_UID_CLONE
#define RFID_UID_CLONE 1
#endif

/** Passive listen max wait (ms). Old 60000 felt like “~1 minute” when no tag. Override in build flags if needed. */
#ifndef RFID_TAG_LISTEN_TIMEOUT_MS
#define RFID_TAG_LISTEN_TIMEOUT_MS 20000
#endif
/** Clone flow waits for source / blank tag (ms). */
#ifndef RFID_CLONE_PHASE_TIMEOUT_MS
#define RFID_CLONE_PHASE_TIMEOUT_MS 45000
#endif
/** Jam / disrupt: wait for external reader to poll target (ms). 0 = until Stop. */
#ifndef RFID_TARGET_WAIT_TIMEOUT_MS
#define RFID_TARGET_WAIT_TIMEOUT_MS 120000
#endif

static Adafruit_PN532 s_nfc(PN532_SCK, PN532_MISO, PN532_MOSI, PN532_SS);
static bool s_hwOk = false;
static char s_pn532VerStr[28] = "";
static bool s_rfidRetrySession = false;
static FeatureUI::Button s_rfFoot[2];

/* Adafruit_PN532 keeps the last response in this library-global buffer.  The
 * public API returns only the UID, even though InListPassiveTarget also gives
 * us ATQA and SAK.  Reading those two documented response bytes lets us
 * classify protected cards without guessing from a default-key login. */
extern byte pn532_packetbuffer[];

/* NullTag-style card family plus a non-destructive fallback for ISO-DEP cards. */
enum CardType {
  UNKNOWN,
  MIFARE_CLASSIC,
  MIFARE_ULTRALIGHT,
  NTAG,
  MIFARE_DESFIRE,
  ISO14443A_SECURE
};

static uint16_t s_lastAtqa = 0;
static uint8_t s_lastSak = 0;
static uint8_t s_lastUid[7] = {0};
static uint8_t s_lastUidLength = 0;
static bool s_lastTargetMetaValid = false;
static uint16_t s_lastClassicBlocks = 64;
static uint16_t s_lastType2UserPageEnd = 16;  // exclusive
static char s_lastVariant[40] = "ISO14443A";
static unsigned long s_lastPn532HealthMs = 0;
static bool s_pn532BusAttached = false;
static bool s_passiveCommandPending = false;
static bool s_tagScannedThisSession = false;

enum class RfidUiEvt { None, Back, Primary };

static void rfidPn532VerStr(uint32_t ver);

static bool rfidAttachBus() {
  // NFC bit-bangs the same physical pins used by CC1101/SD.  Stop any RMT,
  // ISR or transmitter left behind by an interrupted SubGHz screen before
  // changing pin direction; otherwise the RMT driver can flood the serial
  // console with "RX buffer too small" and starve the UI task.
  SubGhzLifecycle::shutdown(false);
  pauseBackgroundRadioTasks();
  quiesceWifiSubsystem(true);
  // Adafruit_PN532 is constructed for software SPI (bitbang). Do NOT call
  // SPI.begin(PN532_*) here: on DIV V2 the PN532 MOSI/MISO pins are swapped
  // vs the SD/CC1101 bus, and remapping hardware SPI that way leaves the SD
  // card dead until something else (e.g. SubGHz) re-inits the bus correctly.
  reclaimSharedSpiBus();
  s_pn532BusAttached = true;
  s_lastTargetMetaValid = false;

  for (uint8_t attempt = 1; attempt <= 3; attempt++) {
    if (!s_nfc.begin()) {
      Serial.printf("[nfc] SPI begin failed (%u/3)\n", (unsigned)attempt);
      delay(18);
      continue;
    }

    delay(12);
    const uint32_t ver = s_nfc.getFirmwareVersion();
    // One RF poll per host command. With retries > 0 the PN532 can still be
    // executing InListPassiveTarget after the ESP32 timeout has expired. The
    // next owner of GPIO11/12/13 (SD or nRF) then sees an active transaction.
    if (ver && s_nfc.SAMConfig() && s_nfc.setPassiveActivationRetries(0x00)) {
      rfidPn532VerStr(ver);
      s_hwOk = true;
      s_lastPn532HealthMs = millis();
      Serial.printf("[nfc] PN532 ready attempt=%u fw=0x%08lX\n", (unsigned)attempt,
                    (unsigned long)ver);
      return true;
    }

    Serial.printf("[nfc] PN532 init retry %u/3 fw=0x%08lX\n", (unsigned)attempt,
                  (unsigned long)ver);
    delay((unsigned long)attempt * 20UL);
  }

  s_hwOk = false;
  rfidPn532VerStr(0);
  Serial.println(F("[nfc] PN532 unavailable after 3 attempts"));
  return false;
}

/** Release PN532 bitbang pins without letting another SPI client run in the
 * middle of the hand-off.  The menu wrapper performs one deterministic
 * PN532 -> nRF -> SD transition after the NFC session has fully returned. */
static void rfidRestoreBus() {
  // Never return GPIO11/13 while an InListPassiveTarget command is pending.
  // A completed response is consumed; a genuinely pending command receives
  // the PN532 cancellation ACK before chip-select is parked high.
  if (s_pn532BusAttached && s_passiveCommandPending) {
    if (s_nfc.responseReady()) {
      uint8_t discardUid[7] = {0};
      uint8_t discardLen = 0;
      (void)s_nfc.readDetectedPassiveTargetID(discardUid, &discardLen);
      Serial.println(F("[nfc] pending response drained on exit"));
    } else {
      const bool aborted = s_nfc.abortCommand();
      Serial.printf("[nfc] pending command cancel %s\n",
                    aborted ? "ok" : "write-failed");
      // Give the controller a bounded interval to settle the cancelled
      // command. A cancellation is not itself a low-power transition; the
      // explicit PowerDown below is what finally stops the PN532 driving the
      // shared pins.
      const uint32_t cancelStarted = millis();
      while (!s_nfc.responseReady() &&
             (uint32_t)(millis() - cancelStarted) < 60u) {
        delay(3);
      }
      if (s_nfc.responseReady()) {
        uint8_t discardUid[7] = {0};
        uint8_t discardLen = 0;
        (void)s_nfc.readDetectedPassiveTargetID(discardUid, &discardLen);
        Serial.println(F("[nfc] cancelled response drained on exit"));
      } else {
        Serial.println(F("[nfc] cancel settled without response"));
      }
      delay(4);
    }
    s_passiveCommandPending = false;
  }
  if (s_pn532BusAttached) {
    if (!s_tagScannedThisSession) {
      // With an empty field the controller may still need an explicit stop.
      // This path never hands the bus to another feature: the guarded wrapper
      // requires a real tag read before it can leave NFC.
      bool poweredDown = s_nfc.powerDown(0x20);
      if (!poweredDown) {
        (void)s_nfc.abortCommand();
        delay(5);
        poweredDown = s_nfc.powerDown(0x20);
      }
      Serial.printf("[nfc] empty-field PowerDown %s\n",
                    poweredDown ? "ok" : "failed");
    } else {
      // A completed InListPassiveTarget read is the only transition verified
      // on this custom board to release the shared electrical state. Do not
      // follow it with PowerDown, RFConfiguration or another PN532 command.
      Serial.println(F("[nfc] real-tag recovery preserved; no post-read command"));
    }
    pinMode(PN532_SS, OUTPUT);
    digitalWrite(PN532_SS, HIGH);
    delay(4);
  }
  s_pn532BusAttached = false;

  // Do not mount SD here.  The former NFC -> SD -> nRF -> SD sequence was the
  // only path that failed when Card Reader was left with an empty field.  It
  // also allowed a slow SD card to retain MISO while the PN532 software-SPI
  // pins were still being released.  Park every shared pin now; the outer NFC
  // wrapper will probe nRF first and mount SD only after that probe completes.
  SPI.end();
#if BOARD_HAS_ESP32S3
  gpio_reset_pin((gpio_num_t)PN532_SCK);
  gpio_reset_pin((gpio_num_t)PN532_MISO);
  gpio_reset_pin((gpio_num_t)PN532_MOSI);
  gpio_reset_pin((gpio_num_t)PN532_SS);
#endif
  pinMode(PN532_SS, OUTPUT);
  digitalWrite(PN532_SS, HIGH);
  delay(3);
}

static void rfidPn532VerStr(uint32_t ver) {
  if (!ver) {
    s_pn532VerStr[0] = '\0';
    return;
  }
  snprintf(s_pn532VerStr, sizeof(s_pn532VerStr), "PN532 0x%06lX", (unsigned long)ver);
}

static void rfidReleaseNavButtons() {
  delay(12);
  for (int i = 0; i < 80; i++) {
    if (!isButtonPressed(BTN_SELECT) && !isButtonPressed(BTN_LEFT) &&
        !isButtonPressed(BTN_RIGHT) && !isButtonPressed(BTN_UP) &&
        !isButtonPressed(BTN_DOWN)) {
      break;
    }
    delay(5);
  }
}

static constexpr int kRfidScreenH = 320;
static constexpr int16_t kRfidToolbarY = 20;
static constexpr int16_t kRfidToolbarH = TOOLBAR_ICON_VISUAL_SIZE;
static constexpr int kRfidToolbarBottom = kRfidToolbarY + kRfidToolbarH;
static constexpr int kRfidIconBackX = 10;
static constexpr int kRfidIconSize = 16;
static bool s_rfidUiDrawn = false;

static int rfidContentBottom() {
  return featureHasTouchNavBar() ? touchNavContentBottomY() : kRfidScreenH;
}

static void rfidClearBody(uint16_t color = FEATURE_BG) {
  if (featureHasTouchNavBar()) {
    featureClearContent(color);
  } else {
    tft.fillScreen(color);
  }
}

static void rfidRedrawNavChrome() {
  redrawTouchButtonBar();
  maintainTouchNavBar();
}

static bool rfidSessionExitRequested() { return feature_exit_requested; }

static void rfidPollToolbar() {
  if (!feature_active) {
    return;
  }
  static unsigned long s_lastToolbarCheck = 0;
  if (millis() - s_lastToolbarCheck < 50) {
    return;
  }
  s_lastToolbarCheck = millis();
  int x = 0;
  int y = 0;
  if (!readTouchXY(x, y)) {
    return;
  }
  if (toolbarIconHit(x, y, kRfidIconBackX, kRfidToolbarY)) {
    feature_exit_requested = true;
  }
}

static bool rfidPumpSessionUi() {
  if (rfidSessionExitRequested()) {
    return true;
  }
  maintainTouchNavBar();
  rfidPollToolbar();
  if (s_rfidUiDrawn) {
    tft.drawFastHLine(0, kRfidToolbarY, TFT_WIDTH, UI_LINE);
    tft.drawFastHLine(0, kRfidToolbarBottom, TFT_WIDTH, UI_LINE);
  }
  return false;
}

static RfidUiEvt rfidPollFooter(FeatureUI::Button* btns, int n, bool mapSelectToPrimary) {
  if (isButtonPressed(BTN_LEFT)) {
    rfidReleaseNavButtons();
    return RfidUiEvt::Back;
  }
  if (n == 1 && isButtonPressed(BTN_SELECT)) {
    rfidReleaseNavButtons();
    return RfidUiEvt::Back;
  }
  if (mapSelectToPrimary && n >= 2 && isButtonPressed(BTN_SELECT)) {
    if (!btns[1].disabled) {
      rfidReleaseNavButtons();
      return RfidUiEvt::Primary;
    }
  }
  if (featureHasTouchNavBar()) {
    if (isTouchNavButtonPressedEdge(BTN_LEFT)) {
      rfidReleaseNavButtons();
      return RfidUiEvt::Back;
    }
    if (isTouchNavButtonPressedEdge(BTN_SELECT)) {
      if (mapSelectToPrimary && n >= 2 && !btns[1].disabled) {
        rfidReleaseNavButtons();
        return RfidUiEvt::Primary;
      }
      rfidReleaseNavButtons();
      return RfidUiEvt::Back;
    }
    return RfidUiEvt::None;
  }
  int x = 0, y = 0;
  if (!readTouchXYDismiss(x, y) && !readTouchXY(x, y)) {
    return RfidUiEvt::None;
  }
  int h = FeatureUI::hit(btns, n, x, y);
  if (h < 0) {
    return RfidUiEvt::None;
  }
  if (btns[h].disabled) {
    return RfidUiEvt::None;
  }
  rfidReleaseNavButtons();
  if (h == 0) {
    return RfidUiEvt::Back;
  }
  return RfidUiEvt::Primary;
}

static constexpr int RF_PAD_X = 10;
static constexpr int RF_RESULT_LINE_PX = 17;
static constexpr int RF_BODY_LINE_PX = 18;
static constexpr int RF_TAG_H = 14;
static constexpr int RF_TAG_GAP = 4;
/** Ignored — kept so session code need not change. */
static constexpr int RF_DYN_TOP_MAIN = 0;
static constexpr int RF_DYN_TOP_PROG = 0;

/** IR Record / 2.4GHz Scanner rhythm */
static constexpr int kRfidToolbarGap = 8;
static constexpr int kRfidBoxHeaderH = 15;
static constexpr int kRfidLineHeight = 12;
static constexpr int kRfidStatusY = kRfidToolbarBottom + kRfidToolbarGap;
static constexpr int kRfidStatusBoxH = 91;
static constexpr int kRfidLogGap = 4;
static constexpr int kRfidLogBoxH = 49;
static constexpr int kRfidLogBoxTop = kRfidStatusY + kRfidStatusBoxH + kRfidLogGap;
static constexpr int kRfidLogStartY = kRfidLogBoxTop + kRfidBoxHeaderH;
static constexpr int kRfidLogEndY = kRfidLogBoxTop + kRfidLogBoxH - 2;
static constexpr int kRfidDetailTop = kRfidLogBoxTop + kRfidLogBoxH + 4;
static constexpr int kRfidStatusLineCount = 6;
static constexpr int kRfidStatusTextY = kRfidStatusY + kRfidBoxHeaderH;
static constexpr int kRfidMaxLogLines = 48;
static constexpr int kRfidDetailPadX = RF_PAD_X;
static constexpr int kRfidDetailInnerW = 220;
static constexpr int kRfidDetailHeaderH = 18;

static int rfidHintY() {
  if (featureHasTouchNavBar()) {
    return rfidContentBottom() - 16;
  }
  return TFT_HEIGHT - FeatureUI::FOOTER_H - 16;
}

static int rfidDetailBottom() {
  if (featureHasTouchNavBar()) {
    return rfidContentBottom() - 8;
  }
  return rfidHintY() - 8;
}

static int rfidEffectiveDetailTop() {
  const int bottom = rfidDetailBottom();
  const int minDetailH = 80;
  int top = kRfidDetailTop;
  if (bottom - top < minDetailH) {
    top = bottom - minDetailH;
    const int logBottom = kRfidLogBoxTop + kRfidLogBoxH;
    if (top < logBottom + 2) {
      top = logBottom + 2;
    }
  }
  return top;
}

static int rfidDetailBodyTop() { return rfidEffectiveDetailTop() + kRfidBoxHeaderH + 2; }

static int rfidDetailLinesPerPage() {
  const int bodyBottom = rfidDetailBottom() - 4;
  const int n = (bodyBottom - rfidDetailBodyTop()) / kRfidLineHeight;
  return n < 1 ? 1 : n;
}

static bool s_rfidBoxesDrawn = false;
static bool s_rfidStatusStaticDrawn = false;
static bool s_rfidDetailBoxVisible = false;
static char s_featureInfoTitle[32];
static char s_featureInfoBody[640];
static bool s_featureInfoAvailable = false;
static char s_listenCenterLabel[24];
static String s_rfidLogBuffer[kRfidMaxLogLines];
static uint16_t s_rfidLogColor[kRfidMaxLogLines];
static int s_rfidLogIndex = 0;
static String s_rfidStatusLineText[kRfidStatusLineCount];
static uint16_t s_rfidStatusLineColor[kRfidStatusLineCount];

static void rfidPrintWrappedPageBg(int x, int y, int maxWidth, const char* text, int lineStepPx,
                                   uint16_t fg, uint16_t bg, int firstLine, int maxLines);
static int rfidCountWrappedLines(int maxWidth, const char* text);
static bool rfidDetailTagNeedsPill(const char* tagLabel);

static constexpr int kRfidInfoHeaderH = 18;
static constexpr int kRfidInfoTitleH = 15;
static constexpr int kRfidInfoPanelPad = 8;
static constexpr int kRfidInfoLinePx = 16;
/** Match Activity / log box chrome (drawRoundRect at x=4, w=TFT_WIDTH-8). */
static constexpr int kRfidInfoBoxX = 4;
static constexpr int kRfidInfoBoxW = TFT_WIDTH - 8;
static constexpr int kRfidInfoInnerX = 6;
static constexpr int kRfidInfoInnerW = TFT_WIDTH - 12;
static constexpr int kRfidInfoTextW = TFT_WIDTH - RF_PAD_X * 2;

static int rfidInfoPanelPageCountForLines(const char* body, int linesPerPage) {
  int total = rfidCountWrappedLines(kRfidInfoTextW, body);
  int pageCount = (total + linesPerPage - 1) / linesPerPage;
  return pageCount < 1 ? 1 : pageCount;
}

static int rfidDetailOverlayTop() { return kRfidStatusY; }

static int rfidInfoContentTop(bool withFeatureTitle) {
  int y = rfidDetailOverlayTop() + kRfidInfoHeaderH + 1 + 5;
  if (withFeatureTitle) {
    y += kRfidInfoTitleH + 4;
  }
  return y + kRfidInfoPanelPad;
}

static int rfidInfoContentBottom() { return rfidDetailBottom() - 6; }

static int rfidInfoOverlayLinesPerPage(bool withFeatureTitle) {
  const int panelTop = rfidInfoContentTop(withFeatureTitle) - kRfidInfoPanelPad;
  const int panelH = rfidInfoContentBottom() - panelTop;
  const int innerH = panelH - kRfidInfoPanelPad * 2;
  const int n = innerH / kRfidInfoLinePx;
  return n < 1 ? 1 : n;
}

static void rfidDrawInfoPill(int x, int y, int w, const char* label) {
  if (w < 28) {
    w = 28;
  }
  tft.fillRoundRect(x, y, w, RF_TAG_H, 7, DARK_GRAY);
  tft.setTextColor(UI_ICON, DARK_GRAY);
  tft.setCursor(x + 6, y + 3);
  tft.print(label);
}

static String rfidFitInfoTitle(const char* title) {
  if (!title || !title[0]) {
    return String();
  }
  String t = title;
  const int maxW = TFT_WIDTH - RF_PAD_X * 2;
  const bool needsEllipsis = tft.textWidth(t) > maxW;
  while (t.length() > 0 && tft.textWidth(needsEllipsis ? t + "..." : t) > maxW) {
    t.remove(t.length() - 1);
  }
  if (needsEllipsis && t.length() > 0) {
    t += "...";
  }
  return t;
}

static void rfidDrawInfoOverlay(const char* featureTitle, const char* body, int page, int pageCount,
                                int linesPerPage) {
  const int top = rfidDetailOverlayTop();
  const int bottom = rfidDetailBottom();
  const bool showTitle = featureTitle && featureTitle[0];

  tft.fillRect(0, top - 2, TFT_WIDTH, bottom - top + 2, TFT_BLACK);
  tft.drawFastHLine(0, 19, TFT_WIDTH, UI_LINE);

  tft.setTextFont(1);
  tft.setTextSize(1);

  const int headerY = top + 3;
  tft.setTextColor(UI_TEXT, TFT_BLACK);
  tft.drawString("Info", RF_PAD_X, headerY);

  if (pageCount > 1) {
    char pageLabel[12];
    snprintf(pageLabel, sizeof(pageLabel), "%d/%d", page + 1, pageCount);
    const int pw = (int)tft.textWidth(pageLabel) + 14;
    rfidDrawInfoPill(TFT_WIDTH - RF_PAD_X - pw, headerY, pw, pageLabel);
  }

  int y = top + kRfidInfoHeaderH;
  tft.drawFastHLine(kRfidInfoBoxX, y, kRfidInfoBoxW, UI_LINE);
  y += 5;

  if (showTitle) {
    tft.setTextColor(UI_TEXT, TFT_BLACK);
    tft.drawString(rfidFitInfoTitle(featureTitle), RF_PAD_X, y);
    y += kRfidInfoTitleH;
    tft.drawFastHLine(kRfidInfoBoxX, y - 2, kRfidInfoBoxW, UI_LINE);
    y += 4;
  }

  const int panelY = y;
  const int panelH = bottom - panelY - 4;
  if (panelH > 8) {
    tft.drawRoundRect(kRfidInfoBoxX, panelY, kRfidInfoBoxW, panelH, 3, UI_LINE);
    if (panelH > 4) {
      tft.fillRect(kRfidInfoInnerX, panelY + 2, kRfidInfoInnerW, panelH - 4, DARK_GRAY);
    }
  }

  const int textX = RF_PAD_X;
  const int textY = panelY + kRfidInfoPanelPad;
  const int textBottom = panelY + panelH - kRfidInfoPanelPad;
  const int maxLinesInPanel = (textBottom - textY) / kRfidInfoLinePx;
  const int linesToDraw = (linesPerPage < maxLinesInPanel) ? linesPerPage : maxLinesInPanel;

  rfidPrintWrappedPageBg(textX, textY, kRfidInfoTextW, body, kRfidInfoLinePx, UI_TEXT, DARK_GRAY,
                         page * linesPerPage, linesToDraw);

  if (pageCount > 1 && panelH > 20) {
    tft.setTextColor(UI_DIM_TEXT, DARK_GRAY);
    if (page > 0) {
      tft.setCursor(kRfidInfoInnerX + 4, panelY + panelH - 12);
      tft.print("< Prev");
    }
    if (page < pageCount - 1) {
      const char* nextHint = "Next >";
      const int nw = tft.textWidth(nextHint);
      tft.setCursor(kRfidInfoInnerX + kRfidInfoInnerW - nw - 4, panelY + panelH - 12);
      tft.print(nextHint);
    }
  }
}

static void rfidDynamicBand(int topY, const char* line1, const char* line2);
static void rfidTryShowInfo();
static void rfidWaitTwoBoxDismiss();
static void rfidRunToolbar(const char* title);
static void rfidDrawTextBoxes();
static void rfidResetUiState();
static void rfidPrint(const String& text, uint16_t color, bool extraSpace = false);
static void rfidDrawStatusLineIfChanged(int line, const String& text, uint16_t color);

/** Dim one-line caption for NFC Status (listen/progress); cleared in rfidFeatureSetup. */
static char s_rfidBandAux[72];

static void rfidFeatureSetup(const char* title) {
  applyThemeToPalette(settings().theme);
  rfidResetUiState();
  rfidClearBody(TFT_BLACK);
  drawStatusBar(readBatteryVoltage(), true);
  s_rfidBandAux[0] = '\0';
  rfidRunToolbar(title);
  rfidDrawTextBoxes();
  rfidRedrawNavChrome();
}

static void rfidSetDialogNavLabels(const char* primaryLabel) {
  setTouchNavLabels("Back", nullptr, primaryLabel, nullptr, nullptr);
  redrawTouchButtonBar();
}

static void rfidSetListenNavLabels(const char* actionLabel, bool withInfo = false) {
  setTouchNavLabels(nullptr, nullptr, actionLabel, nullptr, withInfo ? "Info" : nullptr);
  redrawTouchButtonBar();
}

static void rfidSetInfoOverlayNavLabels(int pageCount) {
  if (pageCount > 1) {
    setTouchNavLabels("Back", "Prev", "Back", "Next", nullptr);
  } else {
    setTouchNavLabels("Back", nullptr, "Back", nullptr, nullptr);
  }
  redrawTouchButtonBar();
}

static void rfidSetTimeoutNavLabels(bool withInfo) {
  setTouchNavLabels("Exit", nullptr, "Ready", nullptr, withInfo ? "Info" : nullptr);
  redrawTouchButtonBar();
}

static void rfidDrawShellFooter1(const char* actionLabel) {
  FeatureUI::layoutFooter1(s_rfFoot[0], actionLabel, FeatureUI::ButtonStyle::Secondary);
  if (actionLabel && actionLabel[0]) {
    strncpy(s_listenCenterLabel, actionLabel, sizeof(s_listenCenterLabel) - 1);
    s_listenCenterLabel[sizeof(s_listenCenterLabel) - 1] = '\0';
  } else {
    s_listenCenterLabel[0] = '\0';
  }
  if (featureHasTouchNavBar()) {
    rfidSetListenNavLabels(actionLabel, s_featureInfoAvailable);
  } else {
    FeatureUI::drawFooterBg();
    FeatureUI::drawButton(s_rfFoot[0]);
  }
}

static void rfidDrawShellFooter2(const char* primaryLabel, FeatureUI::ButtonStyle primaryStyle,
                                 bool primaryDisabled) {
  FeatureUI::layoutFooter2(s_rfFoot, "Back", FeatureUI::ButtonStyle::Secondary, primaryLabel,
                             primaryStyle, false, primaryDisabled);
  if (featureHasTouchNavBar()) {
    rfidSetDialogNavLabels(primaryLabel);
  } else {
    FeatureUI::drawFooterBg();
    FeatureUI::drawButton(s_rfFoot[0]);
    FeatureUI::drawButton(s_rfFoot[1]);
  }
}

static void rfidSetBandAux(const char* s) {
  if (!s || !s[0]) {
    s_rfidBandAux[0] = '\0';
    return;
  }
  strncpy(s_rfidBandAux, s, sizeof(s_rfidBandAux) - 1);
  s_rfidBandAux[sizeof(s_rfidBandAux) - 1] = '\0';
}

/** IR Record-style toolbar: DARK_GRAY bar, title left, back icon. */
static void rfidRunToolbar(const char* title) {
  if (s_rfidUiDrawn) {
    return;
  }
  tft.fillRect(0, kRfidToolbarY, TFT_WIDTH, kRfidToolbarH, DARK_GRAY);
  drawToolbarIcon(kRfidIconBackX, kRfidToolbarY, bitmap_icon_go_back, UI_ICON);
  tft.setTextColor(UI_TEXT, DARK_GRAY);
  tft.setTextSize(1);
  if (title && title[0]) {
    const int titleX = 35;
    const int maxTitleW = TFT_WIDTH - titleX - 8;
    String t = title;
    const bool needsEllipsis = tft.textWidth(t) > maxTitleW;
    while (t.length() > 0 && tft.textWidth(needsEllipsis ? t + "..." : t) > maxTitleW) {
      t.remove(t.length() - 1);
    }
    if (needsEllipsis) {
      t += "...";
    }
    tft.setCursor(titleX, kRfidToolbarY + 4);
    tft.print(t);
  }
  tft.drawFastHLine(0, 19, TFT_WIDTH, UI_LINE);
  tft.drawFastHLine(0, kRfidToolbarBottom, TFT_WIDTH, UI_LINE);
  s_rfidUiDrawn = true;
}

static void rfidUpdateToolbarTitle(const char* title) {
  s_rfidUiDrawn = false;
  rfidRunToolbar(title);
}

static void rfidPrintWrappedStepBg(int x, int y, int maxWidth, const char* text, int lineStepPx,
                                   uint16_t fg, uint16_t bg, int bottomY = TFT_HEIGHT) {
  String message = text ? text : "";
  int cursorY = y;
  bool clipped = false;

  while (message.length() > 0) {
    if (cursorY > bottomY - lineStepPx) {
      clipped = true;
      break;
    }

    int nl = message.indexOf('\n');
    String paragraph = (nl >= 0) ? message.substring(0, nl) : message;
    message = (nl >= 0) ? message.substring(nl + 1) : "";

    if (paragraph.length() == 0) {
      cursorY += lineStepPx;
      continue;
    }

    while (paragraph.length() > 0) {
      if (cursorY > bottomY - lineStepPx) {
        clipped = true;
        break;
      }

      int lineEnd = (int)paragraph.length();
      while (tft.textWidth(paragraph.substring(0, lineEnd)) > maxWidth && lineEnd > 1) {
        lineEnd--;
      }
      if (lineEnd < (int)paragraph.length()) {
        int lastSpace = paragraph.substring(0, lineEnd).lastIndexOf(' ');
        if (lastSpace > 0) {
          lineEnd = lastSpace;
        }
      }

      tft.setTextColor(fg, bg);
      tft.setCursor(x, cursorY);
      tft.print(paragraph.substring(0, lineEnd));
      paragraph = paragraph.substring(lineEnd);
      paragraph.trim();
      cursorY += lineStepPx;
    }
  }

  if (clipped && bottomY >= y + lineStepPx) {
    tft.setTextColor(UI_DIM_TEXT, bg);
    tft.setCursor(x, bottomY - lineStepPx);
    tft.print("...");
  }
}

static void rfidPrintWrappedStep(int x, int y, int maxWidth, const char* text, int lineStepPx) {
  rfidPrintWrappedStepBg(x, y, maxWidth, text, lineStepPx, UI_TEXT, FEATURE_BG);
}

static int rfidCountWrappedLines(int maxWidth, const char* text) {
  String message = text ? text : "";
  int total = 0;
  while (message.length() > 0) {
    int nl = message.indexOf('\n');
    String paragraph = (nl >= 0) ? message.substring(0, nl) : message;
    message = (nl >= 0) ? message.substring(nl + 1) : "";

    if (paragraph.length() == 0) {
      total++;
      continue;
    }

    while (paragraph.length() > 0) {
      int lineEnd = (int)paragraph.length();
      while (tft.textWidth(paragraph.substring(0, lineEnd)) > maxWidth && lineEnd > 1) {
        lineEnd--;
      }
      if (lineEnd < (int)paragraph.length()) {
        int lastSpace = paragraph.substring(0, lineEnd).lastIndexOf(' ');
        if (lastSpace > 0) {
          lineEnd = lastSpace;
        }
      }
      paragraph = paragraph.substring(lineEnd);
      paragraph.trim();
      total++;
    }
  }
  return total > 0 ? total : 1;
}

static void rfidPrintWrappedPageBg(int x, int y, int maxWidth, const char* text, int lineStepPx,
                                   uint16_t fg, uint16_t bg, int firstLine, int maxLines) {
  String message = text ? text : "";
  int lineNo = 0;
  int drawn = 0;
  while (message.length() > 0 && drawn < maxLines) {
    int nl = message.indexOf('\n');
    String paragraph = (nl >= 0) ? message.substring(0, nl) : message;
    message = (nl >= 0) ? message.substring(nl + 1) : "";

    if (paragraph.length() == 0) {
      if (lineNo >= firstLine) {
        drawn++;
      }
      lineNo++;
      continue;
    }

    while (paragraph.length() > 0 && drawn < maxLines) {
      int lineEnd = (int)paragraph.length();
      while (tft.textWidth(paragraph.substring(0, lineEnd)) > maxWidth && lineEnd > 1) {
        lineEnd--;
      }
      if (lineEnd < (int)paragraph.length()) {
        int lastSpace = paragraph.substring(0, lineEnd).lastIndexOf(' ');
        if (lastSpace > 0) {
          lineEnd = lastSpace;
        }
      }

      if (lineNo >= firstLine) {
        tft.setTextColor(fg, bg);
        tft.setCursor(x, y + drawn * lineStepPx);
        tft.print(paragraph.substring(0, lineEnd));
        drawn++;
      }
      paragraph = paragraph.substring(lineEnd);
      paragraph.trim();
      lineNo++;
    }
  }
}

static String rfidFitStatusText(const String& text) {
  const int maxWidth = TFT_WIDTH - 16;
  tft.setTextSize(1);
  if (tft.textWidth(text) <= maxWidth) {
    return text;
  }
  String out = text;
  while (out.length() > 1 && tft.textWidth(out + "...") > maxWidth) {
    out.remove(out.length() - 1);
  }
  if (out.length() > 0) {
    out += "...";
  }
  return out;
}

static constexpr int kRfidLogTextX = 10;
static constexpr int kRfidLogTextW = TFT_WIDTH - kRfidLogTextX * 2;

static String rfidFitLogText(const String& text) {
  tft.setTextSize(1);
  if (tft.textWidth(text) <= kRfidLogTextW) {
    return text;
  }
  String out = text;
  while (out.length() > 1 && tft.textWidth(out + "...") > kRfidLogTextW) {
    out.remove(out.length() - 1);
  }
  if (out.length() > 0) {
    out += "...";
  }
  return out;
}

static void rfidScrollLog() {
  for (int i = 0; i < kRfidMaxLogLines - 1; i++) {
    s_rfidLogBuffer[i] = s_rfidLogBuffer[i + 1];
    s_rfidLogColor[i] = s_rfidLogColor[i + 1];
  }
}

static int rfidLogAreaBottom() {
  if (s_rfidDetailBoxVisible) {
    return kRfidLogEndY;
  }
  const int logH = rfidDetailBottom() - kRfidLogBoxTop;
  return kRfidLogBoxTop + logH - 3;
}

static int rfidLogBoxDrawHeight() {
  if (s_rfidDetailBoxVisible) {
    return kRfidLogBoxH;
  }
  return rfidDetailBottom() - kRfidLogBoxTop;
}

static void rfidSetDetailBoxVisible(bool visible) {
  if (s_rfidDetailBoxVisible == visible) {
    return;
  }
  s_rfidDetailBoxVisible = visible;
  s_rfidBoxesDrawn = false;
}

static int rfidLogVisibleLines() {
  const int h = rfidLogAreaBottom() - kRfidLogStartY;
  if (h <= 0 || kRfidLineHeight <= 0) {
    return 1;
  }
  return h / kRfidLineHeight;
}

static void rfidRedrawActivityLog() {
  const int visible = rfidLogVisibleLines();
  const int logBottom = rfidLogAreaBottom();
  tft.fillRect(6, kRfidLogStartY, TFT_WIDTH - 12, logBottom - kRfidLogStartY, TFT_BLACK);
  if (visible <= 0 || s_rfidLogIndex <= 0) {
    return;
  }
  const int start = (s_rfidLogIndex > visible) ? (s_rfidLogIndex - visible) : 0;
  for (int row = 0; row < visible; row++) {
    const int bufIndex = start + row;
    if (bufIndex >= s_rfidLogIndex) {
      break;
    }
    if (s_rfidLogBuffer[bufIndex].length() == 0) {
      continue;
    }
    const int yPos = kRfidLogStartY + row * kRfidLineHeight;
    tft.setTextSize(1);
    tft.setTextColor(s_rfidLogColor[bufIndex], TFT_BLACK);
    tft.setCursor(kRfidLogTextX, yPos);
    tft.print(rfidFitLogText(s_rfidLogBuffer[bufIndex]));
  }
}

static void rfidAppendLogLine(const String& line, uint16_t color) {
  if (line.length() == 0) {
    return;
  }
  if (s_rfidLogIndex >= kRfidMaxLogLines) {
    rfidScrollLog();
    s_rfidLogIndex = kRfidMaxLogLines - 1;
  }
  s_rfidLogBuffer[s_rfidLogIndex] = rfidFitLogText(line);
  s_rfidLogColor[s_rfidLogIndex] = color;
  s_rfidLogIndex++;
}

static void rfidPrint(const String& text, uint16_t color, bool extraSpace) {
  tft.setTextSize(1);
  String remaining = text;
  while (remaining.length() > 0) {
    const int nl = remaining.indexOf('\n');
    if (nl >= 0) {
      rfidAppendLogLine(remaining.substring(0, nl), color);
      remaining = remaining.substring(nl + 1);
      continue;
    }
    if (tft.textWidth(remaining) <= kRfidLogTextW) {
      rfidAppendLogLine(remaining, color);
      break;
    }
    int end = (int)remaining.length();
    while (end > 1 && tft.textWidth(remaining.substring(0, end)) > kRfidLogTextW) {
      end--;
    }
    if (end < (int)remaining.length()) {
      const int sp = remaining.substring(0, end).lastIndexOf(' ');
      if (sp > 0) {
        end = sp;
      }
    }
    String line = remaining.substring(0, end);
    line.trim();
    remaining = remaining.substring(end);
    remaining.trim();
    if (line.length() == 0 && remaining.length() > 0) {
      line = rfidFitLogText(remaining);
      remaining = "";
    }
    rfidAppendLogLine(line, color);
  }
  if (extraSpace && s_rfidLogIndex < kRfidMaxLogLines) {
    s_rfidLogBuffer[s_rfidLogIndex] = "";
    s_rfidLogColor[s_rfidLogIndex] = TFT_WHITE;
    s_rfidLogIndex++;
  }
  rfidRedrawActivityLog();
}

static void rfidDrawStatusLine(int line, const String& text, uint16_t color) {
  const int y = kRfidStatusTextY + line * kRfidLineHeight;
  const String fitted = rfidFitStatusText(text);
  tft.fillRect(8, y, TFT_WIDTH - 16, kRfidLineHeight, TFT_BLACK);
  tft.setTextSize(1);
  tft.setTextColor(color, TFT_BLACK);
  tft.setCursor(8, y);
  tft.print(fitted);
}

static void rfidDrawStatusLineIfChanged(int line, const String& text, uint16_t color) {
  if (line < 0 || line >= kRfidStatusLineCount) {
    return;
  }
  if (s_rfidStatusLineText[line] == text && s_rfidStatusLineColor[line] == color) {
    return;
  }
  s_rfidStatusLineText[line] = text;
  s_rfidStatusLineColor[line] = color;
  rfidDrawStatusLine(line, text, color);
}

/** Repaint NFC Status lines after overlay/box chrome cleared the framebuffer. */
static void rfidRepaintStatusFromCache() {
  for (int i = 0; i < kRfidStatusLineCount; i++) {
    if (s_rfidStatusLineText[i].length() > 0) {
      rfidDrawStatusLine(i, s_rfidStatusLineText[i], s_rfidStatusLineColor[i]);
    }
  }
}

static void rfidDrawTextBoxes() {
  const int detailBottom = rfidDetailBottom();
  const int detailTop = rfidEffectiveDetailTop();
  const int logH = rfidLogBoxDrawHeight();
  tft.fillRect(0, kRfidStatusY - 2, TFT_WIDTH, detailBottom - kRfidStatusY + 2, TFT_BLACK);
  tft.drawFastHLine(0, 19, TFT_WIDTH, UI_LINE);
  tft.drawRoundRect(4, kRfidLogBoxTop, TFT_WIDTH - 8, logH, 3, UI_LINE);
  tft.setTextSize(1);
  tft.setTextColor(UI_DIM_TEXT, TFT_BLACK);
  tft.drawString("NFC Status", 8, kRfidStatusY + 3);
  tft.drawString("Activity", 8, kRfidLogBoxTop + 3);
  if (s_rfidDetailBoxVisible) {
    const int detailH = detailBottom - detailTop;
    if (detailH > 8) {
      tft.drawRoundRect(4, detailTop, TFT_WIDTH - 8, detailH, 3, UI_LINE);
    }
    tft.drawString("Tag Details", 8, detailTop + 3);
  } else {
    const int innerH = logH - kRfidBoxHeaderH - 2;
    if (innerH > 0) {
      tft.fillRect(6, kRfidLogBoxTop + kRfidBoxHeaderH, TFT_WIDTH - 12, innerH, TFT_BLACK);
    }
  }
  s_rfidBoxesDrawn = true;
}

static void rfidDrawStaticStatusLines() {
  if (s_rfidStatusStaticDrawn) {
    return;
  }
  rfidDrawStatusLineIfChanged(4, "Protocol: ISO14443A", UI_DIM_TEXT);
  if (s_pn532VerStr[0] != '\0') {
    rfidDrawStatusLineIfChanged(5, String("Reader: ") + s_pn532VerStr, UI_DIM_TEXT);
  } else {
    rfidDrawStatusLineIfChanged(5, "Reader: PN532", UI_DIM_TEXT);
  }
  s_rfidStatusStaticDrawn = true;
}

static void rfidResetUiState() {
  s_rfidUiDrawn = false;
  s_rfidBoxesDrawn = false;
  s_rfidStatusStaticDrawn = false;
  s_rfidDetailBoxVisible = false;
  s_featureInfoAvailable = false;
  s_featureInfoTitle[0] = '\0';
  s_featureInfoBody[0] = '\0';
  s_listenCenterLabel[0] = '\0';
  s_rfidLogIndex = 0;
  for (int i = 0; i < kRfidMaxLogLines; i++) {
    s_rfidLogBuffer[i] = "";
    s_rfidLogColor[i] = 0;
  }
  for (int i = 0; i < kRfidStatusLineCount; i++) {
    s_rfidStatusLineText[i] = "";
    s_rfidStatusLineColor[i] = 0;
  }
}

static char s_dynPrev1[72];
static char s_dynPrev2[72];
static char s_dynPrevAux[72];
static char s_dynPrevPill[16];
static char s_dynPillLabel[16] = "LIVE";
static uint16_t s_dynPillColor = UI_WARN;

static unsigned s_listenSec = 0xFFFFFFFFu;
static uint8_t s_listenSpin = 0;
static unsigned long s_listenSpinMs = 0;

static void rfidListenShellReset() {
  s_listenSec = 0xFFFFFFFFu;
  s_listenSpin = 0;
  s_listenSpinMs = 0;
}

static void rfidDynClearPrev() {
  s_dynPrev1[0] = '\0';
  s_dynPrev2[0] = '\0';
  s_dynPrevAux[0] = '\0';
  s_dynPrevPill[0] = '\0';
}

static void rfidSetDynamicPill(const char* label, uint16_t color) {
  if (!label) {
    return;
  }
  strncpy(s_dynPillLabel, label, sizeof(s_dynPillLabel) - 1);
  s_dynPillLabel[sizeof(s_dynPillLabel) - 1] = '\0';
  s_dynPillColor = color;
}

static void rfidSetFeatureInfo(const char* title, const char* body) {
  if (title && title[0]) {
    strncpy(s_featureInfoTitle, title, sizeof(s_featureInfoTitle) - 1);
    s_featureInfoTitle[sizeof(s_featureInfoTitle) - 1] = '\0';
  }
  if (!body) {
    body = "";
  }
  strncpy(s_featureInfoBody, body, sizeof(s_featureInfoBody) - 1);
  s_featureInfoBody[sizeof(s_featureInfoBody) - 1] = '\0';
  s_featureInfoAvailable = s_featureInfoBody[0] != '\0';
}

static int rfidInfoPanelPageCount(const char* body) {
  return rfidInfoPanelPageCountForLines(body, rfidDetailLinesPerPage());
}

static bool rfidDetailTagNeedsPill(const char* tagLabel) {
  if (!tagLabel || !tagLabel[0]) {
    return false;
  }
  return strcmp(tagLabel, "INFO") != 0 && strcmp(tagLabel, "PROGRESS") != 0 &&
         strcmp(tagLabel, "SCAN") != 0 && strcmp(tagLabel, "RESULT") != 0;
}

static void rfidDrawInfoPanel(const char* tagLabel, const char* body, int page, int pageCount,
                              int linesPerPage) {
  const int detailBottom = rfidDetailBottom();
  const int detailTop = rfidEffectiveDetailTop();
  const int bodyTop = rfidDetailBodyTop();
  tft.fillRect(5, detailTop + kRfidBoxHeaderH, TFT_WIDTH - 10, detailBottom - detailTop - kRfidBoxHeaderH - 1,
               TFT_BLACK);
  tft.setTextFont(1);
  const int tagTop = detailTop + kRfidBoxHeaderH + 2;
  int textTop = bodyTop;
  if (pageCount > 1) {
    tft.fillRoundRect(kRfidDetailPadX, tagTop, 62, RF_TAG_H, 7, DARK_GRAY);
    tft.setTextColor(UI_ICON, DARK_GRAY);
    tft.setCursor(kRfidDetailPadX + 6, tagTop + 3);
    tft.printf("%d/%d", page + 1, pageCount);
    textTop = tagTop + RF_TAG_H + RF_TAG_GAP;
  } else if (rfidDetailTagNeedsPill(tagLabel)) {
    int pillW = (int)tft.textWidth(tagLabel) + 14;
    if (pillW < 48) {
      pillW = 48;
    }
    tft.fillRoundRect(kRfidDetailPadX, tagTop, pillW, RF_TAG_H, 7, DARK_GRAY);
    tft.setTextColor(UI_ICON, DARK_GRAY);
    tft.setCursor(kRfidDetailPadX + 6, tagTop + 3);
    tft.print(tagLabel);
    textTop = tagTop + RF_TAG_H + RF_TAG_GAP;
  }
  rfidPrintWrappedPageBg(kRfidDetailPadX, textTop, kRfidDetailInnerW, body, kRfidLineHeight, UI_TEXT,
                         TFT_BLACK, page * linesPerPage, linesPerPage);
}

static void rfidRefreshInfoPanel(const char* tagLabel, const char* body) {
  (void)tagLabel;
  (void)body;
}

static void rfidDrawHint(bool twoActions, int padX = RF_PAD_X) {
  if (featureHasTouchNavBar()) {
    return;
  }
  const int hintY = rfidHintY();
  tft.fillRect(0, hintY - 3, TFT_WIDTH, 17, FEATURE_BG);
  tft.setTextFont(1);
  tft.setTextColor(UI_DIM_TEXT, FEATURE_BG);
  tft.setCursor(padX, hintY);
  tft.print(twoActions ? "LEFT: Back   SELECT: action" : "LEFT / SELECT: close");
}

static void rfidDynamicBand(int topY, const char* line1, const char* line2) {
  (void)topY;
  const char* l1 = line1 ? line1 : "";
  const char* l2 = line2 ? line2 : "";
  if (strcmp(s_dynPrev1, l1) == 0 && strcmp(s_dynPrev2, l2) == 0 &&
      strcmp(s_dynPrevAux, s_rfidBandAux) == 0 && strcmp(s_dynPrevPill, s_dynPillLabel) == 0) {
    return;
  }

  rfidDrawStaticStatusLines();
  const String modeLine = String("Mode: ") + s_dynPillLabel;
  rfidDrawStatusLineIfChanged(0, modeLine, s_dynPillColor);
  rfidDrawStatusLineIfChanged(1, l1, UI_TEXT);
  rfidDrawStatusLineIfChanged(2, l2, UI_DIM_TEXT);
  if (s_rfidBandAux[0]) {
    rfidDrawStatusLineIfChanged(3, s_rfidBandAux, UI_DIM_TEXT);
  }

  strncpy(s_dynPrev1, l1, sizeof(s_dynPrev1) - 1);
  s_dynPrev1[sizeof(s_dynPrev1) - 1] = '\0';
  strncpy(s_dynPrev2, l2, sizeof(s_dynPrev2) - 1);
  s_dynPrev2[sizeof(s_dynPrev2) - 1] = '\0';
  strncpy(s_dynPrevAux, s_rfidBandAux, sizeof(s_dynPrevAux) - 1);
  s_dynPrevAux[sizeof(s_dynPrevAux) - 1] = '\0';
  strncpy(s_dynPrevPill, s_dynPillLabel, sizeof(s_dynPrevPill) - 1);
  s_dynPrevPill[sizeof(s_dynPrevPill) - 1] = '\0';
}

static void rfidRedrawListenChrome() {
  rfidSetDetailBoxVisible(false);
  rfidDrawTextBoxes();
  rfidRepaintStatusFromCache();
  rfidRedrawActivityLog();
  if (featureHasTouchNavBar() && s_listenCenterLabel[0]) {
    rfidSetListenNavLabels(s_listenCenterLabel, s_featureInfoAvailable);
  }
}

/** Wait for Exit on 2-box chrome; feature info stays on Info nav. */
static void rfidWaitTwoBoxDismiss() {
  rfidSetDetailBoxVisible(false);
  rfidDrawShellFooter1("Exit");
  rfidDrawTextBoxes();
  rfidRepaintStatusFromCache();
  rfidRedrawActivityLog();
  if (featureHasTouchNavBar()) {
    rfidSetListenNavLabels("Exit", s_featureInfoAvailable);
  }

  for (;;) {
    if (rfidPumpSessionUi()) {
      return;
    }
    rfidTryShowInfo();
    if (rfidPollFooter(s_rfFoot, 1, false) != RfidUiEvt::None) {
      return;
    }
    delay(8);
  }
}

/** Timeout dismiss: Exit leaves, Ready retries. @return true when Ready pressed. */
static bool rfidWaitTimeoutDismiss() {
  rfidSetDetailBoxVisible(false);
  FeatureUI::layoutFooter2(s_rfFoot, "Exit", FeatureUI::ButtonStyle::Secondary, "Ready",
                           FeatureUI::ButtonStyle::Primary, false, false);
  rfidDrawTextBoxes();
  rfidRepaintStatusFromCache();
  rfidRedrawActivityLog();
  if (featureHasTouchNavBar()) {
    rfidSetTimeoutNavLabels(s_featureInfoAvailable);
  } else {
    FeatureUI::drawFooterBg();
    FeatureUI::drawButton(s_rfFoot[0]);
    FeatureUI::drawButton(s_rfFoot[1]);
  }

  for (;;) {
    if (rfidPumpSessionUi()) {
      return false;
    }
    rfidTryShowInfo();
    RfidUiEvt e = rfidPollFooter(s_rfFoot, 2, true);
    if (e == RfidUiEvt::Primary) {
      return true;
    }
    if (e == RfidUiEvt::Back) {
      return false;
    }
    delay(8);
  }
}

/** Timeout — stay on 2-box chrome; feature info on Info nav. @return true to retry. */
static bool rfidDismissTimeoutPanel(const char* statusLine, const char* message) {
  rfidSetDynamicPill("WARN", UI_WARN);
  rfidSetBandAux(nullptr);
  rfidDynClearPrev();
  rfidDynamicBand(0, statusLine, "No tag detected");

  String logLine = message;
  const int nl = logLine.indexOf('\n');
  if (nl >= 0) {
    logLine = logLine.substring(0, nl);
  }
  rfidPrint(String("[!] ") + logLine, UI_WARN, false);
  return rfidWaitTimeoutDismiss();
}

static void rfidShowInfoPanel() {
  const bool showTitle = s_featureInfoTitle[0] != '\0';
  const int linesPerPage = rfidInfoOverlayLinesPerPage(showTitle);
  const int pageCount = rfidInfoPanelPageCountForLines(s_featureInfoBody, linesPerPage);
  int page = 0;
  bool redraw = false;
  rfidSetInfoOverlayNavLabels(pageCount);
  rfidDrawInfoOverlay(s_featureInfoTitle, s_featureInfoBody, page, pageCount, linesPerPage);

  for (;;) {
    if (rfidPumpSessionUi()) {
      rfidRedrawListenChrome();
      return;
    }
    if (redraw) {
      rfidDrawInfoOverlay(s_featureInfoTitle, s_featureInfoBody, page, pageCount, linesPerPage);
      redraw = false;
    }

    if (isButtonPressed(BTN_LEFT)) {
      rfidReleaseNavButtons();
      break;
    }
    if (pageCount == 1 && isButtonPressed(BTN_SELECT)) {
      rfidReleaseNavButtons();
      break;
    }
    if (pageCount > 1 && isButtonPressed(BTN_UP) && page < pageCount - 1) {
      rfidReleaseNavButtons();
      page++;
      redraw = true;
      continue;
    }
    if (pageCount > 1 && isButtonPressed(BTN_DOWN) && page > 0) {
      rfidReleaseNavButtons();
      page--;
      redraw = true;
      continue;
    }

    if (featureHasTouchNavBar()) {
      if (isTouchNavButtonPressedEdge(BTN_LEFT) || isTouchNavButtonPressedEdge(BTN_SELECT)) {
        rfidReleaseNavButtons();
        break;
      }
      if (pageCount > 1 && isTouchNavButtonPressedEdge(BTN_UP) && page < pageCount - 1) {
        rfidReleaseNavButtons();
        page++;
        redraw = true;
        continue;
      }
      if (pageCount > 1 && isTouchNavButtonPressedEdge(BTN_DOWN) && page > 0) {
        rfidReleaseNavButtons();
        page--;
        redraw = true;
        continue;
      }
    }

    delay(8);
  }

  rfidRedrawListenChrome();
}

static void rfidTryShowInfo() {
  if (!s_featureInfoAvailable || !s_featureInfoBody[0]) {
    return;
  }

  bool open = false;
  if (featureHasTouchNavBar()) {
    if (isTouchNavButtonPressedEdge(BTN_RIGHT)) {
      rfidReleaseNavButtons();
      open = true;
    }
  } else if (isButtonPressed(BTN_RIGHT)) {
    rfidReleaseNavButtons();
    open = true;
  }
  if (!open) {
    return;
  }

  rfidShowInfoPanel();
}

static void rfidListenTick(const char* statusLine, unsigned long t0) {
  const char* spin = "|/-\\";
  unsigned sec = (unsigned)((millis() - t0) / 1000UL);
  char detail[40];
  if (sec != s_listenSec) {
    s_listenSec = sec;
    snprintf(detail, sizeof(detail), "%u s", sec);
  } else {
    if (millis() - s_listenSpinMs > 320) {
      s_listenSpin = (uint8_t)((s_listenSpin + 1) % 4);
      s_listenSpinMs = millis();
    }
    snprintf(detail, sizeof(detail), "%u s  %c", sec, spin[s_listenSpin]);
  }
  rfidDynamicBand(0, statusLine, detail);
}

/** Full chrome once — listening: NFC Status + Activity (info via nav button). */
static void rfidShellListen(const char* title, const char* footerLabel, const char* infoBody) {
  rfidListenShellReset();
  rfidDynClearPrev();
  rfidSetDynamicPill("SCAN", UI_WARN);
  rfidFeatureSetup(title);
  rfidSetDetailBoxVisible(false);
  const char* sub = (s_pn532VerStr[0] != '\0') ? s_pn532VerStr : nullptr;
  rfidSetBandAux(sub);
  const char* info = (infoBody && infoBody[0]) ? infoBody : "Waiting for tag...";
  rfidSetFeatureInfo(title, info);
  rfidDrawShellFooter1(footerLabel);
  rfidDrawHint(false);
  rfidDrawStaticStatusLines();
  rfidDynamicBand(0, "Ready", "");
  rfidPrint("[*] Waiting for tag...", UI_DIM_TEXT, false);
}

/** Full chrome once — progress: NFC Status + Activity (info via nav button). */
static void rfidShellProgress(const char* mainTitle, const char* stepLine, const char* footerLabel,
                              const char* infoBody = nullptr) {
  rfidListenShellReset();
  rfidDynClearPrev();
  rfidSetDynamicPill("WORK", UI_OK);
  rfidFeatureSetup(mainTitle);
  rfidSetDetailBoxVisible(false);
  rfidDrawTextBoxes();
  rfidSetBandAux(stepLine);
  const char* info = (infoBody && infoBody[0]) ? infoBody : stepLine;
  rfidSetFeatureInfo(mainTitle, info);
  rfidDrawShellFooter1(footerLabel);
  rfidDrawHint(false);
  rfidDrawStaticStatusLines();
  rfidDynamicBand(0, stepLine, "");
  rfidPrint(String("[!] ") + stepLine, UI_TEXT, false);
  rfidRedrawNavChrome();
}

/** Switch listen → progress without wiping chrome or activity log. */
static void rfidTransitionToProgress(const char* title, const char* stepLine, const char* footerLabel,
                                     const char* infoBody) {
  (void)infoBody;
  if (title && title[0]) {
    rfidUpdateToolbarTitle(title);
  }
  rfidListenShellReset();
  rfidDynClearPrev();
  rfidSetDynamicPill("WORK", UI_OK);
  rfidSetDetailBoxVisible(false);
  rfidSetBandAux(stepLine);
  rfidDrawShellFooter1(footerLabel);
  rfidDrawTextBoxes();
  rfidDrawStaticStatusLines();
  rfidDynamicBand(0, stepLine, "");
  rfidPrint(String("[!] ") + stepLine, UI_TEXT, false);
}

static void rfidLayoutFull(const char* title, const char* subtitleOrNull, const char* body,
                           bool twoButtons, const char* primaryLabel,
                           FeatureUI::ButtonStyle primaryStyle, bool primaryDisabled) {
  applyThemeToPalette(settings().theme);
  rfidResetUiState();
  rfidClearBody(TFT_BLACK);
  drawStatusBar(readBatteryVoltage(), true);
  s_rfidBandAux[0] = '\0';
  rfidRunToolbar(title);

  const int boxY = kRfidStatusY;
  const int boxBottom = rfidDetailBottom();
  const int boxH = boxBottom - boxY;
  if (boxH > 12) {
    tft.drawRoundRect(4, boxY, TFT_WIDTH - 8, boxH, 3, UI_LINE);
    tft.setTextSize(1);
    tft.setTextColor(UI_DIM_TEXT, TFT_BLACK);
    tft.drawString("Details", 8, boxY + 3);
    const int innerX = kRfidDetailPadX;
    const int innerW = kRfidDetailInnerW;
    const bool hasSub = subtitleOrNull && subtitleOrNull[0];
    int tagTop = boxY + kRfidBoxHeaderH + 4;
    int bodyTop = tagTop + RF_TAG_H + RF_TAG_GAP;
    if (hasSub) {
      tft.setTextFont(1);
      tft.setTextColor(UI_DIM_TEXT, TFT_BLACK);
      String sub = subtitleOrNull;
      const bool wasLong = tft.textWidth(sub) > innerW;
      while (sub.length() > 0 && tft.textWidth(wasLong ? sub + "..." : sub) > innerW) {
        sub.remove(sub.length() - 1);
      }
      if (wasLong) {
        sub += "...";
      }
      tft.setCursor(innerX, boxY + kRfidBoxHeaderH + 2);
      tft.print(sub);
      tagTop = boxY + kRfidBoxHeaderH + 16;
      bodyTop = tagTop + RF_TAG_H + RF_TAG_GAP;
    }
    const char* tag = (primaryStyle == FeatureUI::ButtonStyle::Danger) ? "CAUTION" : "DETAILS";
    tft.setTextFont(1);
    tft.fillRoundRect(innerX, tagTop, 52, RF_TAG_H, 7, DARK_GRAY);
    tft.setTextColor(UI_ICON, DARK_GRAY);
    tft.setCursor(innerX + 6, tagTop + 3);
    tft.print(tag);
    rfidPrintWrappedStepBg(innerX, bodyTop, innerW, body, RF_BODY_LINE_PX, UI_TEXT, TFT_BLACK,
                           boxBottom - 4);
  }

  rfidRedrawNavChrome();

  if (twoButtons && primaryLabel) {
    rfidDrawShellFooter2(primaryLabel, primaryStyle, primaryDisabled);
    rfidDrawHint(true);
  } else {
    rfidDrawShellFooter1("Back");
    rfidDrawHint(false);
  }
}

static bool rfidSubtitleIsFailure(const char* sub) {
  if (!sub || !sub[0]) {
    return false;
  }
  return strstr(sub, "fail") || strstr(sub, "Fail") || strstr(sub, "Timeout") ||
         strstr(sub, "Skipped") || strstr(sub, "No tag") || strstr(sub, "Unsupported") ||
         strstr(sub, "Aborted") || strstr(sub, "Stopped") || strstr(sub, "No write") ||
         strstr(sub, "lost") || strstr(sub, "Lost") || strstr(sub, "Incomplete");
}

/** Result dismiss: Exit only on success; Exit + Ready when failed. */
static void rfidWaitResultDismiss(bool offerRetry) {
  if (offerRetry) {
    if (rfidWaitTimeoutDismiss()) {
      s_rfidRetrySession = true;
    }
    return;
  }
  rfidWaitTwoBoxDismiss();
}

static void rfidPresentResult(const char* title, const char* sub, const char* body,
                              const char* detailTag = "RESULT", const char* pillLabel = nullptr) {
  (void)detailTag;
  if (!s_rfidBoxesDrawn) {
    rfidFeatureSetup(title ? title : "RFID");
  } else if (title && title[0]) {
    rfidUpdateToolbarTitle(title);
  }

  rfidListenShellReset();
  rfidDynClearPrev();

  const bool failed = rfidSubtitleIsFailure(sub);
  if (pillLabel && pillLabel[0]) {
    rfidSetDynamicPill(pillLabel, failed ? UI_WARN : UI_OK);
  } else {
    rfidSetDynamicPill(failed ? "WARN" : "DONE", failed ? UI_WARN : UI_OK);
  }
  rfidSetBandAux(nullptr);
  rfidDrawStaticStatusLines();
  rfidDynamicBand(0, title ? title : "Done", sub && sub[0] ? sub : "Complete");
  if (sub && sub[0]) {
    rfidPrint(String("[") + (failed ? "!" : "+") + "] " + sub, failed ? UI_WARN : UI_OK, false);
  }
  if (body && body[0]) {
    String remaining = body;
    int lines = 0;
    while (remaining.length() > 0 && lines < 8) {
      const int nl = remaining.indexOf('\n');
      String line = (nl >= 0) ? remaining.substring(0, nl) : remaining;
      remaining = (nl >= 0) ? remaining.substring(nl + 1) : "";
      line.trim();
      if (line.length() > 0) {
        rfidPrint(line, failed ? UI_WARN : UI_TEXT, false);
        lines++;
      }
    }
  }
  rfidWaitResultDismiss(failed);
}

static void rfidResultAndDismiss(const char* title, const char* sub, const char* body) {
  rfidPresentResult(title, sub, body);
}

/** @return true = primary (right) action, false = back */
static bool rfidRunTwoButtonDialog(const char* title, const char* sub, const char* body,
                                   const char* primaryLabel, FeatureUI::ButtonStyle primaryStyle) {
  rfidLayoutFull(title, sub, body, true, primaryLabel, primaryStyle, false);
  for (;;) {
    if (rfidPumpSessionUi()) {
      return false;
    }
    RfidUiEvt e = rfidPollFooter(s_rfFoot, 2, true);
    if (e == RfidUiEvt::Back) {
      return false;
    }
    if (e == RfidUiEvt::Primary) {
      return true;
    }
    delay(8);
  }
}

/** 2-box confirm: Back cancels, primary continues. */
static bool rfidRunTwoBoxConfirm(const char* title, const char* statusLine, const char* statusDetail,
                                 const char* activityMsg, const char* infoBody,
                                 const char* primaryLabel) {
  rfidListenShellReset();
  rfidDynClearPrev();
  rfidSetDynamicPill("READY", UI_OK);
  rfidFeatureSetup(title);
  rfidSetDetailBoxVisible(false);
  rfidSetFeatureInfo(title, infoBody ? infoBody : statusLine);
  rfidSetBandAux(nullptr);
  rfidDynamicBand(0, statusLine, statusDetail ? statusDetail : "");
  if (activityMsg && activityMsg[0]) {
    rfidPrint(activityMsg, UI_TEXT, false);
  }
  rfidDrawShellFooter2(primaryLabel, FeatureUI::ButtonStyle::Primary, false);
  rfidDrawHint(true);

  for (;;) {
    if (rfidPumpSessionUi()) {
      return false;
    }
    rfidTryShowInfo();
    RfidUiEvt e = rfidPollFooter(s_rfFoot, 2, true);
    if (e == RfidUiEvt::Back) {
      return false;
    }
    if (e == RfidUiEvt::Primary) {
      return true;
    }
    delay(8);
  }
}

static bool rfidPollListenUi() {
  if (rfidPumpSessionUi()) {
    return true;
  }
  rfidTryShowInfo();
  return rfidPollFooter(s_rfFoot, 1, false) != RfidUiEvt::None;
}

static bool rfidPollCancel() {
  return rfidPollListenUi();
}

static void rfidCaptureTargetMetadata(const uint8_t* uid, uint8_t uidLen) {
  s_lastTargetMetaValid = false;
  if (!uid || uidLen == 0 || uidLen > sizeof(s_lastUid) || pn532_packetbuffer[7] != 1 ||
      pn532_packetbuffer[12] != uidLen || memcmp(uid, pn532_packetbuffer + 13, uidLen) != 0) {
    return;
  }

  s_lastAtqa = (uint16_t)((uint16_t)pn532_packetbuffer[9] << 8) | pn532_packetbuffer[10];
  s_lastSak = pn532_packetbuffer[11];
  memcpy(s_lastUid, uid, uidLen);
  s_lastUidLength = uidLen;
  s_lastTargetMetaValid = true;
  Serial.printf("[nfc] target uidLen=%u ATQA=%04X SAK=%02X\n", (unsigned)uidLen,
                (unsigned)s_lastAtqa, (unsigned)s_lastSak);
}

/** Read one ISO14443A target and preserve ATQA/SAK before the next command
 * overwrites the PN532 response buffer. */
static bool rfidReadPassiveTarget(uint8_t* uid, uint8_t* uidLen, uint16_t timeoutMs) {
  // Preserve the real ISO14443A selection transaction that has been verified
  // to recover the shared bus on DIV V2.1. InAutoPoll can report the same UID,
  // but does not leave this particular PCB in the same electrical state.
  // Passive retries are configured to zero during attach, and the extended
  // host timeout lets the complete response be consumed before returning.
  const uint16_t commandTimeout = timeoutMs < 400 ? 400 : timeoutMs;
  s_passiveCommandPending = true;
  const bool found = s_nfc.readPassiveTargetID(
      PN532_MIFARE_ISO14443A, uid, uidLen, commandTimeout);
  s_passiveCommandPending = false;
  s_lastPn532HealthMs = millis();
  if (found) {
    s_tagScannedThisSession = true;
    rfidCaptureTargetMetadata(uid, *uidLen);
  }
  return found;
}

/** Adafruit_PN532::readPassiveTargetID() does not populate the library's
 * _inListedTag field.  Generic InDataExchange commands therefore fail unless
 * InListPassiveTarget is called once first. */
static bool rfidSelectExchangeTarget(const uint8_t* expectedUid, uint8_t expectedUidLen) {
  if (!s_nfc.inListPassiveTarget()) {
    return false;
  }
  if (pn532_packetbuffer[7] != 1 || pn532_packetbuffer[12] != expectedUidLen ||
      expectedUidLen == 0 || expectedUidLen > sizeof(s_lastUid) ||
      memcmp(expectedUid, pn532_packetbuffer + 13, expectedUidLen) != 0) {
    return false;
  }
  rfidCaptureTargetMetadata(expectedUid, expectedUidLen);
  return true;
}

static bool rfidInDataExchange(const uint8_t* expectedUid, uint8_t expectedUidLen, uint8_t* cmd,
                               uint8_t cmdLen, uint8_t* response, uint8_t* responseLength) {
  if (!rfidSelectExchangeTarget(expectedUid, expectedUidLen)) {
    return false;
  }
  return s_nfc.inDataExchange(cmd, cmdLen, response, responseLength);
}

static bool tryMagicBackdoor(const uint8_t* uid, uint8_t uidLen) {
  uint8_t commands[][2] = {{0x40, 0x00}, {0x43, 0x00}};
  uint8_t response[32];
  if (!rfidSelectExchangeTarget(uid, uidLen)) {
    return false;
  }
  for (int i = 0; i < 2; i++) {
    uint8_t responseLength = sizeof(response);
    if (s_nfc.inDataExchange(commands[i], sizeof(commands[i]), response, &responseLength)) {
      return true;
    }
  }
  return false;
}

/** True if the same ISO14443A tag is still on the coil (short poll). */
static bool rfidTagStillPresent(const uint8_t* uid, uint8_t uidLen) {
  uint8_t uidNow[7] = {0};
  uint8_t lenNow = 0;
  if (!rfidReadPassiveTarget(uidNow, &lenNow, 80)) {
    return false;
  }
  if (lenNow != uidLen) {
    return false;
  }
  return memcmp(uid, uidNow, uidLen) == 0;
}

static bool rfidDumpTagLost(const uint8_t* uid, uint8_t uidLen, unsigned blockOrPage) {
  if (rfidTagStillPresent(uid, uidLen)) {
    return false;
  }
  rfidRestoreBus();
  char body[160];
  snprintf(body, sizeof(body),
           "Tag left the field during dump\n(at block/page %u).\n\n"
           "Keep the tag on the coil until reading finishes.",
           blockOrPage);
  rfidResultAndDismiss("Dump", "Tag lost", body);
  return true;
}

struct RfidClassicKey {
  uint8_t value[6];
  const char* label;
};

/* Public factory/application defaults.  This is deliberately a short list:
 * it improves interoperability with blank, transport and NFC Forum-formatted
 * cards without turning a normal read into an unbounded key search. */
static const RfidClassicKey kRfidClassicKeys[] = {
    {{0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF}, "FF factory"},
    {{0x00, 0x00, 0x00, 0x00, 0x00, 0x00}, "00 transport"},
    {{0xD3, 0xF7, 0xD3, 0xF7, 0xD3, 0xF7}, "NFC Forum"},
    {{0xA0, 0xA1, 0xA2, 0xA3, 0xA4, 0xA5}, "MAD key A"},
    {{0xB0, 0xB1, 0xB2, 0xB3, 0xB4, 0xB5}, "MAD key B"},
};

static bool rfidAuthenticateClassicAny(uint8_t* uid, uint8_t uidLength, uint8_t block,
                                       uint8_t* keyOut, uint8_t* keyTypeOut,
                                       const char** keyLabelOut) {
  for (size_t ki = 0; ki < sizeof(kRfidClassicKeys) / sizeof(kRfidClassicKeys[0]); ki++) {
    for (uint8_t keyType = 0; keyType <= 1; keyType++) {
      uint8_t key[6];
      memcpy(key, kRfidClassicKeys[ki].value, sizeof(key));
      if (s_nfc.mifareclassic_AuthenticateBlock(uid, uidLength, block, keyType, key)) {
        if (keyOut) {
          memcpy(keyOut, key, sizeof(key));
        }
        if (keyTypeOut) {
          *keyTypeOut = keyType;
        }
        if (keyLabelOut) {
          *keyLabelOut = kRfidClassicKeys[ki].label;
        }
        return true;
      }

      // A failed Classic authentication may deselect the PICC. Re-list the
      // same UID before trying the next documented default.
      if (!rfidTagStillPresent(uid, uidLength)) {
        return false;
      }
    }
  }
  return false;
}

static uint8_t rfidClassicSectorForBlock(uint16_t block) {
  if (block < 128) {
    return (uint8_t)(block / 4u);
  }
  return (uint8_t)(32u + ((block - 128u) / 16u));
}

static void rfidSetVariant(const char* name) {
  strncpy(s_lastVariant, name ? name : "ISO14443A", sizeof(s_lastVariant) - 1);
  s_lastVariant[sizeof(s_lastVariant) - 1] = '\0';
}

static CardType rfidProbeType2(uint8_t* uid, uint8_t uidLength) {
  uint8_t data[4] = {0};
  if (!s_nfc.mifareultralight_ReadPage(4, data)) {
    return UNKNOWN;
  }

  s_lastType2UserPageEnd = 16;
  rfidSetVariant("MIFARE Ultralight-compatible");

  uint8_t cmd[] = {0x60};  // GET_VERSION, supported by EV1 and NTAG21x
  uint8_t response[32] = {0};
  uint8_t responseLength = sizeof(response);
  if (!rfidInDataExchange(uid, uidLength, cmd, sizeof(cmd), response, &responseLength) ||
      responseLength < 8 || response[0] != 0x00 || response[1] != 0x04) {
    return MIFARE_ULTRALIGHT;
  }

  const uint8_t productType = response[2];
  const uint8_t storageSize = response[6];
  Serial.printf("[nfc] GET_VERSION product=%02X subtype=%02X version=%02X storage=%02X\n",
                (unsigned)productType, (unsigned)response[3], (unsigned)response[4],
                (unsigned)storageSize);

  if (productType == 0x04) {
    switch (storageSize) {
      case 0x0F:
        s_lastType2UserPageEnd = 40;  // NTAG213: pages 4..39
        rfidSetVariant("NTAG213");
        break;
      case 0x11:
        s_lastType2UserPageEnd = 130;  // NTAG215: pages 4..129
        rfidSetVariant("NTAG215");
        break;
      case 0x13:
        s_lastType2UserPageEnd = 226;  // NTAG216: pages 4..225
        rfidSetVariant("NTAG216");
        break;
      default:
        s_lastType2UserPageEnd = 36;
        rfidSetVariant("NTAG21x");
        break;
    }
    return NTAG;
  }

  if (productType == 0x03) {
    if (storageSize == 0x0B) {
      s_lastType2UserPageEnd = 16;
      rfidSetVariant("MIFARE Ultralight EV1 48B");
    } else if (storageSize == 0x0E) {
      s_lastType2UserPageEnd = 36;
      rfidSetVariant("MIFARE Ultralight EV1 128B");
    } else {
      rfidSetVariant("MIFARE Ultralight EV1");
    }
  }
  return MIFARE_ULTRALIGHT;
}

static CardType detectCardType(uint8_t* uid, uint8_t uidLength) {
  s_lastClassicBlocks = 64;
  s_lastType2UserPageEnd = 16;
  rfidSetVariant("ISO14443A");

  if (uidLength == 0 || uidLength > 7) {
    return UNKNOWN;
  }

  const bool metadataMatches =
      s_lastTargetMetaValid && s_lastUidLength == uidLength && memcmp(s_lastUid, uid, uidLength) == 0;
  const uint8_t sak = metadataMatches ? s_lastSak : 0xFF;

  // SAK identifies the Classic capacity even when every sector key has been
  // changed.  Authentication is an access test, not a card-family test.
  if (sak == 0x09) {
    s_lastClassicBlocks = 20;
    rfidSetVariant("MIFARE Classic Mini");
    return MIFARE_CLASSIC;
  }
  if (sak == 0x08) {
    s_lastClassicBlocks = 64;
    rfidSetVariant("MIFARE Classic 1K / Plus SL1");
    return MIFARE_CLASSIC;
  }
  if (sak == 0x18) {
    s_lastClassicBlocks = 256;
    rfidSetVariant("MIFARE Classic 4K / Plus SL1");
    return MIFARE_CLASSIC;
  }

  // Type 2 tags normally report SAK 00.  Probe their memory command before
  // attempting ISO-DEP operations.
  if (sak == 0x00 || sak == 0xFF) {
    CardType type2 = rfidProbeType2(uid, uidLength);
    if (type2 != UNKNOWN) {
      return type2;
    }
  }

  if ((sak & 0x20u) != 0u || sak == 0xFF) {
    uint8_t cmd[] = {0x5A, 0x00, 0x00, 0x00};  // DESFire select master application
    uint8_t response[32] = {0};
    uint8_t responseLength = sizeof(response);
    if (rfidInDataExchange(uid, uidLength, cmd, sizeof(cmd), response, &responseLength) &&
        responseLength > 0) {
      rfidSetVariant("MIFARE DESFire");
      return MIFARE_DESFIRE;
    }
    rfidSetVariant("ISO14443-4 / protected");
    return ISO14443A_SECURE;
  }

  // Fallback for uncommon/clone cards with incomplete SAK reporting.
  uint8_t keyA[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
  if (s_nfc.mifareclassic_AuthenticateBlock(uid, uidLength, 0, 0, keyA)) {
    rfidSetVariant("MIFARE Classic-compatible");
    return MIFARE_CLASSIC;
  }
  rfidSetVariant("ISO14443A / protected");
  return ISO14443A_SECURE;
}

static const char* cardTypeStr(CardType t) {
  switch (t) {
    case MIFARE_CLASSIC:
      return "MIFARE Classic";
    case MIFARE_ULTRALIGHT:
      return "Ultralight";
    case NTAG:
      return "NTAG";
    case MIFARE_DESFIRE:
      return "DESFire";
    case ISO14443A_SECURE:
      return "ISO14443A protected";
    default:
      return "Unknown";
  }
}

static void uidToHex(char* out, size_t outLen, uint8_t* uid, uint8_t uidLen) {
  size_t pos = 0;
  out[0] = '\0';
  for (uint8_t i = 0; i < uidLen && i < 7u && pos + 3 < outLen; i++) {
    pos += (size_t)snprintf(out + pos, outLen - pos, "%02X", uid[i]);
  }
}

static void formatClassicBlock0(char* out, size_t outLen, const uint8_t* d) {
  snprintf(out, outLen,
           "Sector 0 block 0:\n"
           "%02X %02X %02X %02X %02X %02X %02X %02X\n"
           "%02X %02X %02X %02X %02X %02X %02X %02X",
           d[0], d[1], d[2], d[3], d[4], d[5], d[6], d[7], d[8], d[9], d[10], d[11], d[12], d[13],
           d[14], d[15]);
}

namespace RfidNfc {

bool begin() {
  // Each concrete session performs the sole PN532 probe. Avoid the previous
  // acquire/release/acquire cycle before Card Reader draws its first screen.
  return true;
}

bool hardwareOk() { return s_hwOk; }

/** Clear retry flag before starting a session (menu loop). */
void clearSessionRetry() { s_rfidRetrySession = false; }

/** @return true once if the user chose Ready on a failure result screen. */
bool consumeSessionRetry() {
  if (!s_rfidRetrySession) {
    return false;
  }
  s_rfidRetrySession = false;
  return true;
}

void beginGuardedSession() {
  s_tagScannedThisSession = false;
  feature_exit_requested = false;
  Serial.println(F("[nfc-guard] session armed; tag read required before hand-off"));
}

static void rfidGuardExitNotice() {
  feature_exit_requested = false;
  rfidSetDynamicPill("LOCK", UI_WARN);
  rfidSetBandAux(nullptr);
  rfidDynamicBand(0, tr("Exit blocked"), tr("Read tag or power off"));
  rfidPrint(String("[!] ") +
                tr("To exit, scan a tag. Otherwise power off the device."),
            UI_WARN, false);
  rfidReleaseNavButtons();
}

void ensureTagReadBeforeExit() {
  if (s_tagScannedThisSession) {
    // Some NFC operations release the PN532 before drawing their result
    // screen. Always repeat the idempotent teardown here: the result UI can
    // run for an arbitrary time and the final hand-off must occur immediately
    // before nRF/SD reinitialization, not merely when the card was detected.
    rfidRestoreBus();
    feature_exit_requested = false;
    Serial.println(F("[nfc-guard] satisfied; final PN532 teardown complete"));
    return;
  }

  Serial.println(F("[nfc-guard] blocking hand-off until a tag is read"));
  feature_exit_requested = false;

  for (;;) {
    if (!s_pn532BusAttached && !rfidAttachBus()) {
      rfidShellListen(tr("NFC protected exit"), tr("Read tag"),
                      tr("Present a tag to continue. Power off the device to leave."));
      rfidSetDynamicPill("WAIT", UI_WARN);
      rfidDynamicBand(0, tr("Exit blocked"), tr("Reader unavailable — power off"));
      rfidPrint(String("[!] ") + tr("Waiting for the NFC reader..."), UI_WARN,
                false);
      const uint32_t retryStarted = millis();
      while ((uint32_t)(millis() - retryStarted) < 900u) {
        maintainTouchNavBar();
        rfidPollToolbar();
        if (feature_exit_requested ||
            rfidPollFooter(s_rfFoot, 1, false) != RfidUiEvt::None) {
          rfidGuardExitNotice();
        }
        delay(12);
      }
      rfidRestoreBus();
      continue;
    }

    uint8_t uid[7] = {0};
    uint8_t uidLength = 0;
    rfidShellListen(tr("NFC protected exit"), tr("Read tag"),
                    tr("Present a tag to continue. Power off the device to leave."));
    rfidSetDynamicPill("LOCK", UI_WARN);
    rfidDynamicBand(0, tr("Exit blocked"), tr("Read tag or power off"));
    rfidPrint(String("[!] ") +
                  tr("Cancel cannot close this NFC session safely."),
              UI_WARN, false);
    rfidPrint(String("[*] ") + tr("Present an ISO14443A tag to exit."),
              UI_TEXT, false);

    while (!s_tagScannedThisSession) {
      maintainTouchNavBar();
      rfidPollToolbar();
      rfidTryShowInfo();
      if (feature_exit_requested ||
          rfidPollFooter(s_rfFoot, 1, false) != RfidUiEvt::None) {
        rfidGuardExitNotice();
      }
      rfidDynamicBand(0, tr("Exit blocked"), tr("Read tag or power off"));
      if (rfidReadPassiveTarget(uid, &uidLength, 150)) {
        char uidStr[24] = "";
        uidToHex(uidStr, sizeof(uidStr), uid, uidLength);
        rfidSetDynamicPill("OK", UI_OK);
        rfidDynamicBand(0, tr("Tag detected"), uidStr);
        rfidPrint(String("[+] ") + tr("Tag detected") + " " + uidStr,
                  UI_OK, false);
        delay(300);
        break;
      }
      delay(12);
    }

    if (s_tagScannedThisSession) {
      rfidRestoreBus();
      feature_exit_requested = false;
      Serial.println(F("[nfc-guard] satisfied by recovery tag read"));
      return;
    }
  }
}

void resetCloneBuffer() {}

static bool rfidTargetSendResponse(const uint8_t* payload, uint8_t payloadLen) {
  uint8_t tx[64];
  if (payloadLen + 1u > sizeof(tx)) {
    payloadLen = (uint8_t)(sizeof(tx) - 1u);
  }
  tx[0] = 0x8E;
  memcpy(tx + 1, payload, payloadLen);
  return s_nfc.setDataTarget(tx, (uint8_t)(payloadLen + 1u)) != 0;
}

/** Wait until an external reader polls us in ISO14443A target mode. */
static bool rfidTargetWaitReader(const char* tickMsg, unsigned long timeoutMs, uint8_t* rx,
                                 uint8_t* rxLen, bool showWaitLog) {
  for (;;) {
    unsigned long t0 = millis();
    if (showWaitLog) {
      rfidListenShellReset();
      rfidPrint("[*] Waiting for external reader...", UI_DIM_TEXT, false);
    }

    for (;;) {
      if (rfidPumpSessionUi()) {
        return false;
      }
      if (rfidPollCancel()) {
        return false;
      }
      rfidListenTick(tickMsg, t0);

      if (s_nfc.AsTarget()) {
        const uint8_t maxLen = *rxLen;
        *rxLen = maxLen;
        if (s_nfc.getDataTarget(rx, rxLen)) {
          return true;
        }
      }

      if (timeoutMs > 0 && (millis() - t0) >= timeoutMs) {
        char tmsg[128];
        snprintf(tmsg, sizeof(tmsg),
                 "No external reader in %lu s.\nTap Ready to wait again.",
                 timeoutMs / 1000UL);
        if (!rfidDismissTimeoutPanel("Timeout", tmsg)) {
          return false;
        }
        rfidPrint("[*] Ready — waiting for reader...", UI_DIM_TEXT, false);
        break;
      }
      delay(25);
    }
  }
}

static bool rfidListenIso14443a(const char* title, const char* footer, uint8_t* uid,
                                uint8_t* uidLenOut, unsigned long timeoutMs, const char* tickMsg,
                                const char* infoBody = nullptr) {
  const char* info = (infoBody && infoBody[0]) ? infoBody : tickMsg;
  for (;;) {
    unsigned long t0 = millis();
    rfidShellListen(title, footer, info);
    for (;;) {
      if (rfidPumpSessionUi()) {
        return false;
      }
      rfidListenTick(tickMsg, t0);
      if (rfidPollCancel()) {
        return false;
      }
      if (rfidReadPassiveTarget(uid, uidLenOut, 150)) {
        return true;
      }
      if (millis() - t0 > timeoutMs) {
        char tmsg[96];
        snprintf(tmsg, sizeof(tmsg),
                 "No ISO14443A tag in %lu s.\nTap Ready to listen again.",
                 (unsigned long)(timeoutMs / 1000UL));
        if (!rfidDismissTimeoutPanel("Timeout", tmsg)) {
          return false;
        }
        rfidPrint("[*] Ready — listening again...", UI_DIM_TEXT, false);
        break;
      }
      delay(12);
    }
  }
}

static bool rfidPrepareSession(const char* title) {
  if (rfidAttachBus()) {
    return true;
  }
  rfidRestoreBus();
  rfidResultAndDismiss(title, "PN532 unavailable",
                       "The NFC controller did not answer after 3 recovery attempts.\n"
                       "Leave the NFC field empty and tap Ready to retry.");
  return false;
}

void sessionErase() {
  static const char kIntro[] =
      "Clears MIFARE Classic data block 4 (16 bytes to 00) using documented default keys.\n\n"
      "Ultralight / NTAG are not erased by this mode.\n\n"
      "Hold a Classic tag on the coil.";

  if (!rfidPrepareSession("Erase")) {
    return;
  }
  uint8_t uid[7] = {0};
  uint8_t uidLength = 0;
  if (!rfidListenIso14443a("Erase", "Cancel", uid, &uidLength, (unsigned long)RFID_TAG_LISTEN_TIMEOUT_MS,
                           "Classic tag — block 4 erase", kIntro)) {
    rfidRestoreBus();
    return;
  }

  char uidStr[24] = "";
  uidToHex(uidStr, sizeof(uidStr), uid, uidLength);
  rfidSetDynamicPill("OK", UI_OK);
  rfidSetBandAux(nullptr);
  rfidDynamicBand(0, "Tag detected", uidStr);
  rfidPrint(String("[+] Tag detected ") + uidStr, UI_OK, false);

  CardType ct = detectCardType(uid, uidLength);
  if (ct != MIFARE_CLASSIC) {
    rfidRestoreBus();
    rfidPresentResult("Erase", "Skipped", "This erase only targets MIFARE Classic.");
    return;
  }

  uint8_t key[6] = {0};
  uint8_t keyType = 0;
  const char* keyLabel = nullptr;
  uint8_t emptyBlock[16] = {0};
  if (!rfidAuthenticateClassicAny(uid, uidLength, 4, key, &keyType, &keyLabel)) {
    rfidRestoreBus();
    rfidResultAndDismiss("Erase", "Auth failed",
                         "Could not authenticate block 4 with a documented default key.");
    return;
  }
  if (!s_nfc.mifareclassic_WriteDataBlock(4, emptyBlock)) {
    rfidRestoreBus();
    rfidResultAndDismiss("Erase", "Write failed", "Block 4 write was rejected.");
    return;
  }

  uint8_t verify[16] = {0xFF};
  bool verified = s_nfc.mifareclassic_ReadDataBlock(4, verify);
  for (uint8_t i = 0; verified && i < sizeof(verify); i++) {
    verified = verify[i] == 0;
  }
  if (!verified) {
    rfidRestoreBus();
    rfidResultAndDismiss("Erase", "Verify failed",
                         "The write was acknowledged, but block 4 did not read back as zero.");
    return;
  }

  rfidRestoreBus();
  char body[220];
  snprintf(body, sizeof(body),
           "UID %s\nBlock 4 cleared and verified (16 bytes zero).\nKey: %s (%c).", uidStr,
           keyLabel ? keyLabel : "default", keyType == 0 ? 'A' : 'B');
  rfidResultAndDismiss("Erase", "Done", body);
}

void sessionDump() {
  static const char kIntro[] =
      "Reads the detected Classic capacity or UL/NTAG user memory.\n\n"
      "Summary here; full hex prints on Serial Monitor.\n\n"
      "Present tag on the coil.";

  if (!rfidPrepareSession("Dump")) {
    return;
  }
  uint8_t uid[7] = {0};
  uint8_t uidLength = 0;
  if (!rfidListenIso14443a("Dump", "Cancel", uid, &uidLength, (unsigned long)RFID_TAG_LISTEN_TIMEOUT_MS,
                           "Present tag to dump", kIntro)) {
    rfidRestoreBus();
    return;
  }

  char uidStr[24] = "";
  uidToHex(uidStr, sizeof(uidStr), uid, uidLength);
  CardType ct = detectCardType(uid, uidLength);
  rfidSetDynamicPill("OK", UI_OK);
  rfidSetBandAux(nullptr);
  rfidDynamicBand(0, "Tag detected", uidStr);
  rfidPrint(String("[+] Tag detected ") + uidStr, UI_OK, false);

  uint8_t data[16];
  int blocksWithData = 0;
  int blocksRead = 0;
  int segmentsExpected = 0;

  if (ct == MIFARE_CLASSIC) {
    const uint16_t maxBlocks = s_lastClassicBlocks;
    segmentsExpected = (int)maxBlocks;
    rfidTransitionToProgress("Dump", "Reading Classic blocks", "Cancel",
                             "Reading detected MIFARE Classic capacity.\nFull hex on Serial Monitor.");
    Serial.println(F("\n--- RFID Dump Classic ---"));
    int currentSector = -1;
    bool sectorAuthenticated = false;
    uint8_t sectorKey[6] = {0};
    uint8_t sectorKeyType = 0;
    const char* sectorKeyLabel = nullptr;
    for (uint16_t blockIndex = 0; blockIndex < maxBlocks; blockIndex++) {
      const uint8_t block = (uint8_t)blockIndex;
      const uint8_t sector = rfidClassicSectorForBlock(blockIndex);
      char l2[48];
      snprintf(l2, sizeof(l2), "Block %u / %u", (unsigned)blockIndex,
               (unsigned)(maxBlocks - 1u));
      rfidDynamicBand(RF_DYN_TOP_PROG, "Dumping MIFARE Classic", l2);
      char infoLine[72];
      snprintf(infoLine, sizeof(infoLine), "Reading block %u of %u.\nFull hex on Serial Monitor.",
               (unsigned)blockIndex, (unsigned)(maxBlocks - 1u));
      rfidRefreshInfoPanel("PROGRESS", infoLine);
      if (rfidPollCancel()) {
        rfidRestoreBus();
        if (!rfidSessionExitRequested()) {
          rfidResultAndDismiss("Dump", "Stopped", "Dump aborted.");
        }
        return;
      }

      if ((int)sector != currentSector) {
        currentSector = sector;
        sectorAuthenticated = rfidAuthenticateClassicAny(
            uid, uidLength, block, sectorKey, &sectorKeyType, &sectorKeyLabel);
        if (sectorAuthenticated) {
          Serial.printf("S%u: auth %s key %c\n", (unsigned)sector,
                        sectorKeyLabel ? sectorKeyLabel : "default",
                        sectorKeyType == 0 ? 'A' : 'B');
        } else {
          Serial.printf("S%u: AUTH FAILED (known defaults)\n", (unsigned)sector);
        }
      }

      const bool ok = sectorAuthenticated && s_nfc.mifareclassic_ReadDataBlock(block, data);
      if (ok) {
        blocksRead++;
        bool nz = false;
        for (int i = 0; i < 16; i++) {
          if (data[i] != 0) {
            nz = true;
          }
        }
        if (nz) {
          blocksWithData++;
        }
        Serial.printf("B%u:", (unsigned)block);
        for (int i = 0; i < 16; i++) {
          Serial.printf(" %02X", data[i]);
        }
        Serial.println();
      } else if (sectorAuthenticated && rfidDumpTagLost(uid, uidLength, (unsigned)blockIndex)) {
        return;
      }
      delay(25);
    }
  } else if (ct == MIFARE_ULTRALIGHT || ct == NTAG) {
    const uint16_t maxPages = s_lastType2UserPageEnd;
    segmentsExpected = (int)maxPages - 4;
    rfidTransitionToProgress("Dump", "Reading user pages", "Cancel",
                             "Reading Ultralight / NTAG user pages.\nFull hex on Serial Monitor.");
    Serial.println(F("\n--- RFID Dump Ultralight / NTAG ---"));
    for (uint16_t pageIndex = 4; pageIndex < maxPages; pageIndex++) {
      const uint8_t page = (uint8_t)pageIndex;
      char l2[48];
      snprintf(l2, sizeof(l2), "Page %u / %u", (unsigned)pageIndex,
               (unsigned)(maxPages - 1u));
      rfidDynamicBand(RF_DYN_TOP_PROG, "Dumping pages", l2);
      char infoLine[72];
      snprintf(infoLine, sizeof(infoLine), "Reading page %u of %u.\nFull hex on Serial Monitor.",
               (unsigned)pageIndex, (unsigned)(maxPages - 1u));
      rfidRefreshInfoPanel("PROGRESS", infoLine);
      if (rfidPollCancel()) {
        rfidRestoreBus();
        if (!rfidSessionExitRequested()) {
          rfidResultAndDismiss("Dump", "Stopped", "Dump aborted.");
        }
        return;
      }
      uint8_t pg[4];
      if (s_nfc.mifareultralight_ReadPage(page, pg)) {
        blocksRead++;
        bool nz = false;
        for (int i = 0; i < 4; i++) {
          if (pg[i] != 0) {
            nz = true;
          }
        }
        if (nz) {
          blocksWithData++;
        }
        Serial.printf("P%u:", (unsigned)page);
        for (int i = 0; i < 4; i++) {
          Serial.printf(" %02X", pg[i]);
        }
        Serial.println();
      } else if (rfidDumpTagLost(uid, uidLength, (unsigned)pageIndex)) {
        return;
      }
      delay(18);
    }
  } else {
    rfidRestoreBus();
    rfidResultAndDismiss("Dump", "Unsupported",
                         "Dump targets Classic or Ultralight/NTAG.\nTag did not match.");
    return;
  }

  const bool tagGoneAtEnd = segmentsExpected > 0 && blocksRead > 0 && blocksRead < segmentsExpected &&
                            !rfidTagStillPresent(uid, uidLength);

  rfidRestoreBus();

  if (blocksRead == 0) {
    rfidResultAndDismiss("Dump", "Read failed", "No blocks/pages could be read.\nCheck tag position and keys.");
    return;
  }

  if (tagGoneAtEnd) {
    char body[160];
    snprintf(body, sizeof(body),
             "UID: %s\nType: %s\nVariant: %s\n\nOnly %d of %d segments read before tag left the field.",
             uidStr, cardTypeStr(ct), s_lastVariant, blocksRead, segmentsExpected);
    rfidResultAndDismiss("Dump", "Tag lost", body);
    return;
  }

  char body[420];
  const bool partial = segmentsExpected > 0 && blocksRead < segmentsExpected;
  snprintf(body, sizeof(body),
           "UID: %s\nType: %s\nVariant: %s\nATQA %04X / SAK %02X\n\n"
           "Segments read OK: %d%s\nBlocks/pages with any non-zero byte: %d\n\n"
           "%sOpen Serial Monitor for full hex.",
           uidStr, cardTypeStr(ct), s_lastVariant, (unsigned)s_lastAtqa, (unsigned)s_lastSak,
           blocksRead,
           partial ? " (partial — some blocks need other keys)" : "",
           blocksWithData, partial ? "Some blocks were skipped (auth/key).\n" : "");
  rfidResultAndDismiss("Dump", "Done", body);
}

void sessionDecodeAccess() {
  static const char kIntro[] =
      "Reads MIFARE Classic sector 1 trailer (block 7) and shows raw access bytes.\n"
      "Uses documented factory/NFC Forum keys A/B.\n\n"
      "Present a Classic tag on the coil.";

  if (!rfidPrepareSession("Decode Access")) {
    return;
  }
  uint8_t uid[7] = {0};
  uint8_t uidLength = 0;
  if (!rfidListenIso14443a("Decode Access", "Cancel", uid, &uidLength,
                           (unsigned long)RFID_TAG_LISTEN_TIMEOUT_MS, "Classic tag", kIntro)) {
    rfidRestoreBus();
    return;
  }

  char uidStr[24] = "";
  uidToHex(uidStr, sizeof(uidStr), uid, uidLength);
  rfidSetDynamicPill("OK", UI_OK);
  rfidSetBandAux(nullptr);
  rfidDynamicBand(0, "Tag detected", uidStr);
  rfidPrint(String("[+] Tag detected ") + uidStr, UI_OK, false);

  CardType ct = detectCardType(uid, uidLength);
  if (ct != MIFARE_CLASSIC) {
    rfidRestoreBus();
    rfidPresentResult("Decode Access", "Skipped", "Only MIFARE Classic sector trailers.");
    return;
  }

  uint8_t key[6] = {0};
  uint8_t keyType = 0;
  const char* keyLabel = nullptr;
  uint8_t blk[16] = {0};
  const bool authenticated =
      rfidAuthenticateClassicAny(uid, uidLength, 7, key, &keyType, &keyLabel);

  if (!authenticated || !s_nfc.mifareclassic_ReadDataBlock(7, blk)) {
    rfidRestoreBus();
    rfidResultAndDismiss("Decode Access", "Failed", "Could not read block 7 (sector 1 trailer).");
    return;
  }

  uint8_t a6 = blk[6], a7 = blk[7], a8 = blk[8];
  uint8_t c1 = (uint8_t)((a7 >> 5) & 0x01);
  uint8_t c2 = (uint8_t)((a8 >> 1) & 0x01);
  uint8_t c3 = (uint8_t)((a8 >> 5) & 0x01);
  uint8_t accessCond = (uint8_t)((c1 << 2) | (c2 << 1) | c3);

  rfidRestoreBus();

  char body[420];
  snprintf(body, sizeof(body),
           "UID %s\nSector 1 trailer (block 7):\n"
           "%02X %02X %02X %02X %02X %02X %02X %02X\n"
           "%02X %02X %02X %02X %02X %02X %02X %02X\n\n"
           "Access bytes [6..8]: %02X %02X %02X\n"
           "Condition bits C1..C3 (nibble-style): %u %u %u → code %u\n\n"
           "Authenticated with %s, key %c.\n"
           "(Interpret with MFC access-bit tables for your sector.)",
           uidStr, blk[0], blk[1], blk[2], blk[3], blk[4], blk[5], blk[6], blk[7], blk[8], blk[9],
           blk[10], blk[11], blk[12], blk[13], blk[14], blk[15], a6, a7, a8, (unsigned)c1,
           (unsigned)c2, (unsigned)c3, (unsigned)accessCond,
           keyLabel ? keyLabel : "default", keyType == 0 ? 'A' : 'B');
  rfidResultAndDismiss("Decode Access", "Block 7", body);
}

void sessionJamReader() {
  static const char kIntro[] =
      "Puts the PN532 into ISO14443A target mode and sends rapid replies toward "
      "an external reader.\n\nKeep antennas close to the reader coil.\n\n"
      "Stop ends the session.";

  if (!rfidPrepareSession("Jam Reader")) {
    return;
  }
  rfidShellProgress("Jam Reader", "Preparing target mode...", "Stop", kIntro);

  uint8_t rx[64];
  uint8_t rxLen = sizeof(rx);
  if (!rfidTargetWaitReader("Waiting for reader", RFID_TARGET_WAIT_TIMEOUT_MS, rx, &rxLen, true)) {
    rfidRestoreBus();
    return;
  }

  rfidSetDynamicPill("JAM", UI_WARN);
  rfidSetBandAux(nullptr);
  rfidDynamicBand(0, "Jam active", "Flooding external reader");
  rfidPrint("[+] Reader detected", UI_OK, false);

  static const uint8_t kJamResp[] = {0xFF, 0xFF, 0xFF, 0xFF};
  unsigned long lastUiMs = 0;
  bool pendingRx = true;

  for (;;) {
    if (rfidPumpSessionUi()) {
      break;
    }
    if (rfidPollCancel()) {
      break;
    }

    if (pendingRx) {
      if (!rfidTargetSendResponse(kJamResp, sizeof(kJamResp))) {
        rfidDynamicBand(0, "Jam active", "Waiting for reader...");
        pendingRx = rfidTargetWaitReader("Waiting for reader", 0, rx, &rxLen, false);
        if (!pendingRx) {
          break;
        }
        rfidPrint("[+] Reader reconnected", UI_OK, false);
        continue;
      }
      pendingRx = false;
    }

    rxLen = sizeof(rx);
    if (s_nfc.getDataTarget(rx, &rxLen)) {
      pendingRx = true;
      const unsigned long now = millis();
      if (now - lastUiMs >= 200) {
        rfidDynamicBand(0, "Jam active", "Reader RF collision / flood");
        lastUiMs = now;
      }
    } else {
      const unsigned long now = millis();
      if (now - lastUiMs >= 200) {
        rfidDynamicBand(0, "Jam active", "Waiting for reader...");
        lastUiMs = now;
      }
      pendingRx = rfidTargetWaitReader("Waiting for reader", 0, rx, &rxLen, false);
      if (!pendingRx) {
        break;
      }
      rfidPrint("[+] Reader reconnected", UI_OK, false);
    }
    delay(5);
  }

  s_nfc.SAMConfig();
  s_nfc.setPassiveActivationRetries(0x00);
  rfidRestoreBus();
  if (rfidSessionExitRequested()) {
    return;
  }
  rfidResultAndDismiss("Jam Reader", "Stopped", "SAM reconfigured; reader mode restored.");
}

void sessionDisruptEmulate() {
  static const char kIntro[] =
      "Target mode: cycles payloads (FF / 00 / random) on each exchange.\n"
      "Use only on gear you own or are permitted to test.\n\n"
      "Stop ends the session.";

  if (!rfidPrepareSession("Disrupt Emulate")) {
    return;
  }
  rfidShellProgress("Disrupt Emulate", "Preparing target mode...", "Stop", kIntro);

  uint8_t rx[64];
  uint8_t rxLen = sizeof(rx);
  if (!rfidTargetWaitReader("Waiting for reader", RFID_TARGET_WAIT_TIMEOUT_MS, rx, &rxLen, true)) {
    rfidRestoreBus();
    return;
  }

  rfidSetDynamicPill("TX", UI_WARN);
  rfidSetBandAux(nullptr);
  rfidDynamicBand(0, "Disrupt active", "FF / 00 / random payloads");
  rfidPrint("[+] Reader detected", UI_OK, false);

  uint8_t txPayload[16];
  uint8_t cycle = 0;
  unsigned long lastUiMs = 0;
  bool pendingRx = true;

  for (;;) {
    if (rfidPumpSessionUi()) {
      break;
    }
    if (rfidPollCancel()) {
      break;
    }

    if (pendingRx) {
      if (cycle % 3 == 0) {
        memset(txPayload, 0xFF, sizeof(txPayload));
      } else if (cycle % 3 == 1) {
        memset(txPayload, 0x00, sizeof(txPayload));
      } else {
        for (unsigned i = 0; i < sizeof(txPayload); i++) {
          txPayload[i] = (uint8_t)(random(256));
        }
      }
      cycle++;

      if (!rfidTargetSendResponse(txPayload, sizeof(txPayload))) {
        rfidDynamicBand(0, "Disrupt active", "Waiting for reader...");
        pendingRx = rfidTargetWaitReader("Waiting for reader", 0, rx, &rxLen, false);
        if (!pendingRx) {
          break;
        }
        rfidPrint("[+] Reader reconnected", UI_OK, false);
        continue;
      }
      pendingRx = false;
    }

    rxLen = sizeof(rx);
    if (s_nfc.getDataTarget(rx, &rxLen)) {
      pendingRx = true;
      const unsigned long now = millis();
      if (now - lastUiMs >= 200) {
        rfidDynamicBand(0, "Disrupt active", "FF / 00 / random payloads");
        lastUiMs = now;
      }
    } else {
      const unsigned long now = millis();
      if (now - lastUiMs >= 200) {
        rfidDynamicBand(0, "Disrupt active", "Waiting for reader...");
        lastUiMs = now;
      }
      pendingRx = rfidTargetWaitReader("Waiting for reader", 0, rx, &rxLen, false);
      if (!pendingRx) {
        break;
      }
      rfidPrint("[+] Reader reconnected", UI_OK, false);
    }
    delay(5);
  }

  s_nfc.SAMConfig();
  s_nfc.setPassiveActivationRetries(0x00);
  rfidRestoreBus();
  if (rfidSessionExitRequested()) {
    return;
  }
  rfidResultAndDismiss("Disrupt Emulate", "Stopped", "SAM reconfigured.");
}

void sessionCardReader() {
  static const char kIntro[] =
      "Passive ISO14443A read.\n\n"
      "Hold the tag flat on the coil. UID, tag type, and memory samples appear here after scan.";

  uint8_t uid[7] = {0};
  uint8_t uidLength = 0;
  if (!rfidPrepareSession("Card Reader")) {
    return;
  }

  if (!rfidListenIso14443a("Card Reader", "Cancel", uid, &uidLength,
                           (unsigned long)RFID_TAG_LISTEN_TIMEOUT_MS,
                           "Listening — present ISO14443A tag", kIntro)) {
    rfidRestoreBus();
    return;
  }

  char uidStr[24] = "";
  uidToHex(uidStr, sizeof(uidStr), uid, uidLength);
  CardType ct = detectCardType(uid, uidLength);

  char classicBlk[200] = "";
  if (ct == MIFARE_CLASSIC) {
    uint8_t key[6] = {0};
    uint8_t keyType = 0;
    const char* keyLabel = nullptr;
    uint8_t data[16];
    if (rfidAuthenticateClassicAny(uid, uidLength, 0, key, &keyType, &keyLabel) &&
        s_nfc.mifareclassic_ReadDataBlock(0, data)) {
      formatClassicBlock0(classicBlk, sizeof(classicBlk), data);
      size_t used = strlen(classicBlk);
      snprintf(classicBlk + used, sizeof(classicBlk) - used, "\nKey: %s (%c)",
               keyLabel ? keyLabel : "default", keyType == 0 ? 'A' : 'B');
    } else {
      snprintf(classicBlk, sizeof(classicBlk),
               "Classic family confirmed by SAK, but sector 0 is protected with a non-default key.");
    }
  }

  char ulSample[240] = "";
  if (ct == MIFARE_ULTRALIGHT || ct == NTAG) {
    uint8_t pg[4][4];
    bool ok = true;
    for (int pi = 0; pi < 4; pi++) {
      if (!s_nfc.mifareultralight_ReadPage((uint8_t)(4 + pi), pg[pi])) {
        ok = false;
      }
    }
    if (ok) {
      snprintf(ulSample, sizeof(ulSample),
               "User memory sample (pages 4–7, 4 bytes each):\n"
               "P4  %02X %02X %02X %02X\n"
               "P5  %02X %02X %02X %02X\n"
               "P6  %02X %02X %02X %02X\n"
               "P7  %02X %02X %02X %02X",
               pg[0][0], pg[0][1], pg[0][2], pg[0][3], pg[1][0], pg[1][1], pg[1][2], pg[1][3],
               pg[2][0], pg[2][1], pg[2][2], pg[2][3], pg[3][0], pg[3][1], pg[3][2], pg[3][3]);
    } else {
      snprintf(ulSample, sizeof(ulSample), "Pages 4–7 could not all be read.");
    }
  }

  const char* fw = s_pn532VerStr[0] ? s_pn532VerStr : "PN532 (version not cached)";
  char body[640];
  snprintf(body, sizeof(body),
           "Reader IC: %s\n\n"
           "UID (hex): %s\n"
           "UID length: %u bytes\n"
           "Detected type: %s\n"
           "Variant: %s\n"
           "ATQA: %04X   SAK: %02X\n",
           fw, uidStr, (unsigned)uidLength, cardTypeStr(ct), s_lastVariant,
           (unsigned)s_lastAtqa, (unsigned)s_lastSak);

  if (ct == MIFARE_CLASSIC) {
    size_t len = strlen(body);
    snprintf(body + len, sizeof(body) - len, "Capacity: %u blocks\n",
             (unsigned)s_lastClassicBlocks);
  } else if (ct == MIFARE_ULTRALIGHT || ct == NTAG) {
    size_t len = strlen(body);
    snprintf(body + len, sizeof(body) - len, "User memory: pages 4-%u\n",
             (unsigned)(s_lastType2UserPageEnd - 1u));
  }

  if (ct == MIFARE_CLASSIC && classicBlk[0]) {
    size_t len = strlen(body);
    snprintf(body + len, sizeof(body) - len, "\n%s", classicBlk);
  }
  if ((ct == MIFARE_ULTRALIGHT || ct == NTAG) && ulSample[0]) {
    size_t len = strlen(body);
    snprintf(body + len, sizeof(body) - len, "\n\n%s", ulSample);
  }
  if (ct == MIFARE_DESFIRE) {
    size_t len = strlen(body);
    snprintf(body + len, sizeof(body) - len, "\n\n"
             "DESFire / ISO-DEP-style tag detected.\n"
             "This tool only dumps UID here; full DESFire ops need other APDUs.");
  }
  if (ct == ISO14443A_SECURE) {
    size_t len = strlen(body);
    snprintf(body + len, sizeof(body) - len, "\n\n"
             "ISO-DEP/protected card detected. UID reading is supported; "
             "application data requires the card issuer's authorized keys/APDUs.");
  }
  if (ct == UNKNOWN) {
    size_t len = strlen(body);
    snprintf(body + len, sizeof(body) - len, "\n\n"
             "Family could not be determined from quick probes.");
  }

  rfidRestoreBus();

  rfidSetDynamicPill("OK", UI_OK);
  rfidSetBandAux(nullptr);
  rfidDynamicBand(0, "Tag detected", uidStr);
  rfidSetFeatureInfo("Tag details", body);
  rfidPrint(String("[+] ") + s_lastVariant + "  " + uidStr, UI_OK, false);
  rfidPrint("[*] Open Info for ATQA, SAK and memory details", UI_DIM_TEXT, false);
  if (ct == MIFARE_CLASSIC && classicBlk[0]) {
    rfidPrint(classicBlk, UI_DIM_TEXT, false);
  }
  rfidWaitTwoBoxDismiss();
}

static uint8_t s_srcData[16][3][16];
static uint8_t s_srcPages[ESP32DIV_RFID_SRC_PAGES][4];

void sessionClone() {
  uint8_t uid[7] = {0};
  uint8_t uidLength = 0;
  uint8_t keyA[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
  uint8_t keyB[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
  uint8_t keyZero[6] = {0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
  uint8_t data[16];
  uint8_t srcUidCopy[7];

#if RFID_UID_CLONE
  const char* introFoot =
      "Default keys (A/B) and zero.\nUID byte clone on Gen2 \"magic\" cards when detected.";
#else
  const char* introFoot = "Default keys (A/B) and zero. UID block may stay factory.";
#endif

  char introBody[220];
  snprintf(introBody, sizeof(introBody),
           "Two-tag flow: read source, confirm, then write a blank of the same type.\n\n%s\n\n"
           "Step 1/4: present the SOURCE tag on the coil.",
           introFoot);

  if (!rfidPrepareSession("Clone")) {
    return;
  }
  if (!rfidListenIso14443a("Clone", "Cancel", uid, &uidLength,
                           (unsigned long)RFID_CLONE_PHASE_TIMEOUT_MS,
                           "Step 1/4 — Present SOURCE tag", introBody)) {
    rfidRestoreBus();
    return;
  }

  memcpy(srcUidCopy, uid, uidLength);
  char uidStr[24] = "";
  uidToHex(uidStr, sizeof(uidStr), uid, uidLength);
  rfidSetDynamicPill("OK", UI_OK);
  rfidSetBandAux(nullptr);
  rfidDynamicBand(0, "Source tag detected", uidStr);
  rfidPrint(String("[+] Source ") + uidStr, UI_OK, false);

  CardType srcType = detectCardType(uid, uidLength);
  if (srcType != MIFARE_CLASSIC && srcType != MIFARE_ULTRALIGHT && srcType != NTAG) {
    rfidRestoreBus();
    rfidResultAndDismiss("Clone", "Unsupported",
                         "Clone supports writable Classic 1K, Ultralight and NTAG user memory.\n"
                         "Protected ISO-DEP/DESFire cards remain UID-read only.");
    return;
  }
  const uint16_t srcClassicBlocks = s_lastClassicBlocks;
  const uint16_t srcType2PageEnd = s_lastType2UserPageEnd;
  char srcVariant[sizeof(s_lastVariant)];
  strncpy(srcVariant, s_lastVariant, sizeof(srcVariant) - 1);
  srcVariant[sizeof(srcVariant) - 1] = '\0';

  if (srcType == MIFARE_CLASSIC && srcClassicBlocks != 64) {
    rfidRestoreBus();
    rfidResultAndDismiss("Clone", "Unsupported capacity",
                         "Safe Classic cloning currently supports 1K cards only.\n"
                         "Card Reader and Dump still support Mini/1K/4K.");
    return;
  }

  memset(s_srcData, 0, sizeof(s_srcData));
  memset(s_srcPages, 0, sizeof(s_srcPages));
  bool readOk = true;

  if (srcType == MIFARE_CLASSIC) {
    rfidTransitionToProgress("Clone", "Step 2/4 — Read source", "Cancel",
                             "Reading source Classic data blocks 0–47.");
    for (uint8_t sector = 0; sector < 16 && readOk; sector++) {
      char l1[48];
      char l2[48];
      snprintf(l1, sizeof(l1), "Classic — reading data blocks");
      snprintf(l2, sizeof(l2), "Sector %u / 15", (unsigned)sector);
      rfidDynamicBand(RF_DYN_TOP_PROG, l1, l2);
      char infoLine[64];
      snprintf(infoLine, sizeof(infoLine), "Reading sector %u of 15.", (unsigned)sector);
      rfidRefreshInfoPanel("PROGRESS", infoLine);
      if (rfidPollCancel()) {
        rfidRestoreBus();
        return;
      }
      for (uint8_t block = 0; block < 3; block++) {
        uint8_t blockNum = (uint8_t)(sector * 4 + block);
        bool auth =
            s_nfc.mifareclassic_AuthenticateBlock(uid, uidLength, blockNum, 0, keyA);
        if (!auth) {
          auth = s_nfc.mifareclassic_AuthenticateBlock(uid, uidLength, blockNum, 1, keyB);
        }
        if (auth) {
          if (s_nfc.mifareclassic_ReadDataBlock(blockNum, data)) {
            memcpy(s_srcData[sector][block], data, 16);
          } else {
            readOk = false;
          }
        } else {
          readOk = false;
        }
        delay(18);
      }
    }
  } else if (srcType == MIFARE_ULTRALIGHT || srcType == NTAG) {
    const uint16_t maxPages = srcType2PageEnd;
    rfidTransitionToProgress("Clone", "Step 2/4 — Read source", "Cancel",
                             "Reading source Ultralight / NTAG user pages.");
    for (uint16_t pageIndex = 4; pageIndex < maxPages; pageIndex++) {
      const uint8_t page = (uint8_t)pageIndex;
      char l2[40];
      snprintf(l2, sizeof(l2), "Page %u / %u", (unsigned)pageIndex,
               (unsigned)(maxPages - 1u));
      rfidDynamicBand(RF_DYN_TOP_PROG, "Ultralight / NTAG — user pages", l2);
      char infoLine[64];
      snprintf(infoLine, sizeof(infoLine), "Reading page %u of %u.", (unsigned)pageIndex,
               (unsigned)(maxPages - 1u));
      rfidRefreshInfoPanel("PROGRESS", infoLine);
      if (rfidPollCancel()) {
        rfidRestoreBus();
        return;
      }
      if (s_nfc.mifareultralight_ReadPage(page, data)) {
        memcpy(s_srcPages[page], data, 4);
      } else {
        readOk = false;
      }
      delay(12);
    }
  }

  rfidRestoreBus();

  if (!readOk) {
    rfidResultAndDismiss("Clone", "Read failed", "Some blocks/pages did not read.\nCheck keys or tag damage.");
    return;
  }

  static const char kConfirmInfo[] =
      "Source data is stored in device memory.\n\n"
      "Remove the source tag from the coil, then tap Clone to present a blank tag "
      "of the same type.\n\n"
      "Back cancels without writing anything.";

  char confirmDetail[56];
  snprintf(confirmDetail, sizeof(confirmDetail), "%s  %s", uidStr, cardTypeStr(srcType));
  char confirmLog[128];
  snprintf(confirmLog, sizeof(confirmLog),
           "[+] Source copied (%s, %s).\n[*] Remove source — tap Clone.",
           uidStr, cardTypeStr(srcType));

  if (!rfidRunTwoBoxConfirm("Clone", "Step 3/4 — Confirm clone", confirmDetail, confirmLog,
                            kConfirmInfo, "Clone")) {
    return;
  }

  static const char kBlankInfo[] =
      "Step 4/4: present a blank tag of the same type as the source.\n\n"
      "Writing starts automatically after the blank tag is detected.";

  if (!rfidPrepareSession("Clone")) {
    return;
  }
  if (!rfidListenIso14443a("Clone", "Cancel", uid, &uidLength,
                           (unsigned long)RFID_CLONE_PHASE_TIMEOUT_MS,
                           "Step 4/4 — Present BLANK tag", kBlankInfo)) {
    rfidRestoreBus();
    return;
  }

  char blankUidStr[24] = "";
  uidToHex(blankUidStr, sizeof(blankUidStr), uid, uidLength);
  rfidSetDynamicPill("OK", UI_OK);
  rfidSetBandAux(nullptr);
  rfidDynamicBand(0, "Blank tag detected", blankUidStr);
  rfidPrint(String("[+] Blank ") + blankUidStr, UI_OK, false);

  CardType blankType = detectCardType(uid, uidLength);
  if (blankType != srcType) {
    rfidRestoreBus();
    rfidResultAndDismiss("Clone", "Type mismatch", "Blank must match source family.");
    return;
  }
  if ((blankType == MIFARE_ULTRALIGHT || blankType == NTAG) &&
      s_lastType2UserPageEnd < srcType2PageEnd) {
    rfidRestoreBus();
    rfidResultAndDismiss("Clone", "Capacity mismatch",
                         "The destination tag has less user memory than the source.");
    return;
  }
  if (blankType == MIFARE_CLASSIC && s_lastClassicBlocks != srcClassicBlocks) {
    rfidRestoreBus();
    rfidResultAndDismiss("Clone", "Capacity mismatch",
                         "The destination Classic capacity differs from the source.");
    return;
  }

  bool cloneOk = true;
  int blocksDone = 0;
#if RFID_UID_CLONE
  bool gen2MagicDetected = false;
#endif

  if (blankType == MIFARE_CLASSIC) {
    bool magic = tryMagicBackdoor(uid, uidLength);
#if RFID_UID_CLONE
    gen2MagicDetected = magic;
#else
    (void)magic;
#endif
    bool uidWritten = false;
#if RFID_UID_CLONE
    if (magic) {
      bool ok = s_nfc.mifareclassic_AuthenticateBlock(uid, uidLength, 0, 0, keyA);
      if (!ok) {
        ok = s_nfc.mifareclassic_AuthenticateBlock(uid, uidLength, 0, 1, keyB);
      }
      if (!ok) {
        ok = s_nfc.mifareclassic_AuthenticateBlock(uid, uidLength, 0, 0, keyZero);
      }
      if (ok) {
        uint8_t block0Data[16];
        memcpy(block0Data, srcUidCopy, uidLength);
        block0Data[4] =
            (uint8_t)(block0Data[0] ^ block0Data[1] ^ block0Data[2] ^ block0Data[3]);
        memcpy(&block0Data[5], &s_srcData[0][0][5], 11);
        if (s_nfc.mifareclassic_WriteDataBlock(0, block0Data)) {
          blocksDone++;
          uidWritten = true;
        } else {
          cloneOk = false;
        }
      } else {
        cloneOk = false;
      }
    }
#else
    (void)magic;
    (void)uidWritten;
#endif
    rfidTransitionToProgress("Clone", "Writing — Classic", "Cancel",
                             "Writing copied Classic blocks to blank tag.");
    for (uint8_t sector = 0; sector < 16; sector++) {
      char l2[40];
      snprintf(l2, sizeof(l2), "Sector %u / 15", (unsigned)sector);
      rfidDynamicBand(RF_DYN_TOP_PROG, "Writing copied blocks", l2);
      char infoLine[64];
      snprintf(infoLine, sizeof(infoLine), "Writing sector %u of 15.", (unsigned)sector);
      rfidRefreshInfoPanel("PROGRESS", infoLine);
      if (rfidPollCancel()) {
        rfidRestoreBus();
        if (!rfidSessionExitRequested()) {
          rfidResultAndDismiss("Clone", "Stopped", "Clone aborted mid-write.");
        }
        return;
      }
      for (uint8_t block = 0; block < 3; block++) {
        uint8_t blockNum = (uint8_t)(sector * 4 + block);
#if RFID_UID_CLONE
        if (magic && uidWritten && sector == 0 && block == 0) {
          continue;
        }
#endif
        bool auth =
            s_nfc.mifareclassic_AuthenticateBlock(uid, uidLength, blockNum, 0, keyA);
        if (!auth) {
          auth = s_nfc.mifareclassic_AuthenticateBlock(uid, uidLength, blockNum, 1, keyB);
        }
        if (!auth) {
          auth = s_nfc.mifareclassic_AuthenticateBlock(uid, uidLength, blockNum, 0, keyZero);
        }
        if (auth) {
          if (s_nfc.mifareclassic_WriteDataBlock(blockNum, s_srcData[sector][block])) {
            uint8_t verify[16] = {0};
            if (s_nfc.mifareclassic_ReadDataBlock(blockNum, verify) &&
                memcmp(verify, s_srcData[sector][block], sizeof(verify)) == 0) {
              blocksDone++;
            } else {
              cloneOk = false;
            }
          } else {
            cloneOk = false;
          }
        } else {
          cloneOk = false;
        }
        delay(28);
      }
    }
  } else if (blankType == MIFARE_ULTRALIGHT || blankType == NTAG) {
    const uint16_t maxPages = srcType2PageEnd;
    rfidTransitionToProgress("Clone", "Writing — Ultralight / NTAG", "Cancel",
                             "Writing copied pages to blank tag.");
    for (uint16_t pageIndex = 4; pageIndex < maxPages; pageIndex++) {
      const uint8_t page = (uint8_t)pageIndex;
      char l2[40];
      snprintf(l2, sizeof(l2), "Page %u / %u", (unsigned)pageIndex,
               (unsigned)(maxPages - 1u));
      rfidDynamicBand(RF_DYN_TOP_PROG, "Writing copied pages", l2);
      char infoLine[64];
      snprintf(infoLine, sizeof(infoLine), "Writing page %u of %u.", (unsigned)pageIndex,
               (unsigned)(maxPages - 1u));
      rfidRefreshInfoPanel("PROGRESS", infoLine);
      if (rfidPollCancel()) {
        rfidRestoreBus();
        if (!rfidSessionExitRequested()) {
          rfidResultAndDismiss("Clone", "Stopped", "Clone aborted mid-write.");
        }
        return;
      }
      if (s_nfc.mifareultralight_WritePage(page, s_srcPages[page])) {
        uint8_t verify[4] = {0};
        if (s_nfc.mifareultralight_ReadPage(page, verify) &&
            memcmp(verify, s_srcPages[page], sizeof(verify)) == 0) {
          blocksDone++;
        } else {
          cloneOk = false;
        }
      } else {
        cloneOk = false;
      }
      delay(18);
    }
  }

  rfidRestoreBus();

  char summary[512];
  const char* fw = s_pn532VerStr[0] ? s_pn532VerStr : "PN532";
#if RFID_UID_CLONE
  snprintf(summary, sizeof(summary),
           "%s\n\n"
           "Source UID: %s\n"
           "Blank UID:    %s\n"
           "Tag type:     %s (%s)\n"
           "Reader IC:    %s\n\n"
           "Approx. write ops OK: %d\n"
           "Gen2 magic UID path: %s",
           cloneOk ? "Clone finished." : "Clone finished with some errors.",
           uidStr, blankUidStr, cardTypeStr(srcType), srcVariant, fw, blocksDone,
           gen2MagicDetected ? "detected (UID block attempted)" : "not detected");
#else
  snprintf(summary, sizeof(summary),
           "%s\n\n"
           "Source UID: %s\n"
           "Blank UID:    %s\n"
           "Tag type:     %s (%s)\n"
           "Reader IC:    %s\n\n"
           "Approx. write ops OK: %d",
           cloneOk ? "Clone finished." : "Clone finished with some errors.",
           uidStr, blankUidStr, cardTypeStr(srcType), srcVariant, fw, blocksDone);
#endif

  rfidResultAndDismiss("Clone", "Done", summary);
}

void sessionTagDisrupt() {
  static const char kIntro[] =
      "Writes sector trailer blocks on MIFARE Classic (default key A).\n"
      "Bad access bits can brick a tag.\n\n"
      "Only use tags you own or are allowed to modify.\n\n"
      "Present a Classic tag (~12 s window).";

  uint8_t uid[7] = {0};
  uint8_t uidLength = 0;
  uint8_t maliciousData[16];
  uint8_t key[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
  static const uint8_t sectorTrailers[] = {3,  7,  11, 15, 19, 23, 27, 31,
                                           35, 39, 43, 47, 51, 55, 59, 63};
  bool any = false;
  int trailersOk = 0;
  int authFail = 0;

  if (!rfidPrepareSession("Tag Disrupt")) {
    return;
  }
  if (!rfidListenIso14443a("Tag Disrupt", "Cancel", uid, &uidLength, 12000UL,
                           "Classic tag — hold on coil (~12 s window)", kIntro)) {
    rfidRestoreBus();
    return;
  }

  CardType ct = detectCardType(uid, uidLength);
  char uidStr[24] = "";
  uidToHex(uidStr, sizeof(uidStr), uid, uidLength);
  rfidSetDynamicPill("OK", UI_OK);
  rfidSetBandAux(nullptr);
  rfidDynamicBand(0, "Tag detected", uidStr);
  rfidPrint(String("[+] Tag detected ") + uidStr, UI_OK, false);

  if (ct != MIFARE_CLASSIC) {
    rfidRestoreBus();
    char msg[120];
    snprintf(msg, sizeof(msg), "Tag types as %s.\nDisrupt targets Classic trailers only.",
             cardTypeStr(ct));
    rfidResultAndDismiss("Tag Disrupt", "Skipped", msg);
    return;
  }

  rfidTransitionToProgress("Tag Disrupt", "Writing sector trailers", "Cancel",
                           "Rotating trailer write patterns.\n16 sector trailers targeted.");
  rfidSetDynamicPill("WRITE", UI_WARN);
  for (uint8_t i = 0; i < 16; i++) {
    uint8_t block = sectorTrailers[i];
    char l2[48];
    snprintf(l2, sizeof(l2), "Trailer block %u — sector %u", (unsigned)block, (unsigned)i);
    rfidDynamicBand(RF_DYN_TOP_PROG, "Rotating trailer pattern", l2);
    char infoLine[72];
    snprintf(infoLine, sizeof(infoLine), "Writing trailer block %u (sector %u).", (unsigned)block,
             (unsigned)i);
    rfidRefreshInfoPanel("PROGRESS", infoLine);
    if (rfidPollCancel()) {
      rfidRestoreBus();
      if (!rfidSessionExitRequested()) {
        rfidResultAndDismiss("Tag Disrupt", "Stopped", "Aborted mid-write.");
      }
      return;
    }

    if (i % 3 == 0) {
      memset(maliciousData, 0xFF, 16);
    } else if (i % 3 == 1) {
      memset(maliciousData, 0x00, 16);
    } else {
      for (uint8_t j = 0; j < 16; j++) {
        maliciousData[j] = (uint8_t)random(256);
      }
    }
    maliciousData[6] = 0x00;
    maliciousData[7] = 0x00;
    maliciousData[8] = 0x00;

    if (!s_nfc.mifareclassic_AuthenticateBlock(uid, uidLength, block, 0, key)) {
      authFail++;
    } else if (s_nfc.mifareclassic_WriteDataBlock(block, maliciousData)) {
      any = true;
      trailersOk++;
    }
    delay(35);
  }

  rfidRestoreBus();

  char msg[320];
  const char* fw = s_pn532VerStr[0] ? s_pn532VerStr : "PN532";
  snprintf(msg, sizeof(msg),
           "Tag UID: %s\n"
           "Reader IC: %s\n\n"
           "Sector trailers touched: %d / 16\n"
           "Key A auth failures:    %d\n\n"
           "%s\n\n"
           "Patterns alternate FF / 00 / random on trailers.\n"
           "Access bits may brick the tag.",
           uidStr, fw, trailersOk, authFail,
           any ? "At least one trailer write succeeded." : "No trailer writes succeeded.");

  rfidResultAndDismiss("Tag Disrupt", any ? "Finished" : "No writes", msg);
}

} // namespace RfidNfc
