#include <RF24.h>
#include <SPI.h>

#include "NrfSupervisor.h"
#include "SettingsStore.h"
#include "shared.h"
#include "utils.h"

namespace {

uint8_t s_detectedCount = 2;
bool s_irReceiverDetected = false;

static bool probeRadio(uint8_t ce, uint8_t csn) {
  // Validate the electrical bus before RF24 mutates registers.  Besides
  // producing actionable STATUS/CONFIG/AW/CH diagnostics, this invokes the
  // GPIO-direction recovery path when PN532 software-SPI was the previous
  // owner of GPIO11/12/13.
  if (!verifyNrfSharedSpiDevice(ce, csn, 3)) {
    digitalWrite(ce, LOW);
    digitalWrite(csn, HIGH);
    return false;
  }

  RF24 probe(ce, csn, 4000000);
  bool connected = false;
  for (uint8_t attempt = 0; attempt < 6u && !connected; ++attempt) {
    connected = probe.begin() && probe.isChipConnected();
    if (!connected) delay(8);
  }
  if (connected) probe.powerDown();
  digitalWrite(ce, LOW);
  digitalWrite(csn, HIGH);
  return connected;
}

static bool configureIrFromBoardTopology(bool thirdRadioPresent) {
  // DIV V2.1 has two supported, mutually exclusive populations on the shared
  // GPIO14/GPIO21 nets: the original board fits Q1 + VS1838B and therefore
  // only two nRF radios; the three-radio modification removes those IR parts.
  // A logic-level probe is not reliable here because the VS1838B output can
  // be weakly pulled and looks identical to a floating CSN3 input. Use the
  // already verified NRF3 response as the authoritative hardware discriminator.
  digitalWrite(CE_PIN_3, LOW);
  if (thirdRadioPresent) {
    pinMode(CSN_PIN_3, OUTPUT);
    digitalWrite(CSN_PIN_3, HIGH);
    Serial.println(F("[ir-detect] NRF3 present -> IR hardware absent"));
    return false;
  }

  // Release CSN3/GPIO21 to the receiver immediately. Do not drive this net
  // HIGH: doing so masks every active-low VS1838B pulse on original boards.
  pinMode(IR_RX_PIN, INPUT_PULLUP);
  pinMode(CE_PIN_3, OUTPUT);
  digitalWrite(CE_PIN_3, LOW);
  Serial.println(F("[ir-detect] two-radio DIV topology -> IR hardware enabled"));
  return true;
}

}  // namespace

void nrfSupervisorInit(bool restoreSd) {
  pinMode(CE_PIN_1, OUTPUT);  digitalWrite(CE_PIN_1, LOW);
  pinMode(CE_PIN_2, OUTPUT);  digitalWrite(CE_PIN_2, LOW);
  pinMode(CE_PIN_3, OUTPUT);  digitalWrite(CE_PIN_3, LOW);
  pinMode(CSN_PIN_1, OUTPUT); digitalWrite(CSN_PIN_1, HIGH);
  pinMode(CSN_PIN_2, OUTPUT); digitalWrite(CSN_PIN_2, HIGH);
  pinMode(CSN_PIN_3, OUTPUT); digitalWrite(CSN_PIN_3, HIGH);

  bool first = false;
  bool second = false;
  bool third = false;

  // NFC software-SPI uses the shared data pins in the opposite direction.
  // If a Card Reader session is cancelled with no tag in the field, a single
  // SPI.begin() is not always sufficient to recover the S3 GPIO matrix.  Do a
  // complete bus rebuild between rounds and require both primary radios to
  // answer before accepting the hand-off.  NRF3 remains optional.
  for (uint8_t round = 1; round <= 3u; ++round) {
    prepareNrfSharedSpiBus();
    first = probeRadio(CE_PIN_1, CSN_PIN_1);
    second = probeRadio(CE_PIN_2, CSN_PIN_2);
    third = probeRadio(CE_PIN_3, CSN_PIN_3);
    if (first && second) break;

    Serial.printf("[nrf-detect] shared bus recovery round %u/3\n",
                  (unsigned)round);
    digitalWrite(CE_PIN_1, LOW);
    digitalWrite(CE_PIN_2, LOW);
    digitalWrite(CE_PIN_3, LOW);
    digitalWrite(CSN_PIN_1, HIGH);
    digitalWrite(CSN_PIN_2, HIGH);
    digitalWrite(CSN_PIN_3, HIGH);
    delay((unsigned long)round * 12UL);
  }
  s_detectedCount = third ? 3u : 2u;
  s_irReceiverDetected = configureIrFromBoardTopology(third);

  Serial.printf("[nrf-detect] NRF1=%s NRF2=%s NRF3=%s -> hardware=%u IR=%s\n",
                first ? "OK" : "MISS", second ? "OK" : "MISS",
                third ? "OK" : "MISS", (unsigned)s_detectedCount,
                s_irReceiverDetected ? "detected" : "absent");

  // Detection is independent from the saved 2/ALL preference. ALL remains
  // saved even on two-radio hardware; thirdNrfEnabled() silently resolves the
  // effective count and this call keeps an unused NRF3 off the bus.
  nrfApplyActiveRadioSelection();

  // RF24 remaps the global SPI object. Normal feature exits restore SD here;
  // boot detection runs before the first SD mount and deliberately leaves the
  // card untouched so a marginal card cannot mask the installed radios.
  if (restoreSd) {
    restoreSdAfterSharedSpi();
  } else {
    SPI.end();
    digitalWrite(CSN_PIN_1, HIGH);
    digitalWrite(CSN_PIN_2, HIGH);
    digitalWrite(CSN_PIN_3, HIGH);
    digitalWrite(CE_PIN_1, LOW);
    digitalWrite(CE_PIN_2, LOW);
    digitalWrite(CE_PIN_3, LOW);
  }
}

uint8_t nrfDetectedHardwareCount() { return s_detectedCount; }

bool nrfIrHardwareAvailable() { return s_irReceiverDetected; }

bool nrfPrepareIrHardwareForUse() {
  if (!s_irReceiverDetected) return false;

  // An unusual custom PCB may populate both devices on the shared nets. Make
  // that electrically safe: power NRF3 down while its CSN is still under MCU
  // control, then release GPIO21 to the receiver and GPIO14 to IR TX.
  if (s_detectedCount >= 3u) {
    prepareNrfSharedSpiBus();
    RF24 third(CE_PIN_3, CSN_PIN_3, 4000000);
    if (third.begin() && third.isChipConnected()) third.powerDown();
    digitalWrite(CE_PIN_3, LOW);
    digitalWrite(CSN_PIN_3, HIGH);
  }
  pinMode(IR_RX_PIN, INPUT_PULLUP);
  pinMode(IR_TX_PIN, OUTPUT);
  digitalWrite(IR_TX_PIN, LOW);
  Serial.printf("[ir] shared GPIO prepared; NRF3=%s\n",
                s_detectedCount >= 3u ? "isolated" : "not present");
  return true;
}
