#include <RF24.h>
#include <SPI.h>

#include "NrfSupervisor.h"
#include "SettingsStore.h"
#include "shared.h"
#include "utils.h"

namespace {

uint8_t s_detectedCount = 2;
bool s_irReceiverDetected = false;
constexpr uint32_t kNrfSpiHz = 10000000;

static bool probeIrReceiverElectrical() {
  // The original DIV V2.1 population leaves two independent electrical
  // fingerprints on the NRF3 nets:
  //   GPIO14 -> Q1 base resistor / base-emitter junction
  //   GPIO21 -> actively driven idle-high VS1838B output
  // Test both before RF24 is allowed to select NRF3. The previous pulldown
  // test could overpower a weak receiver output and falsely classify an IR
  // board as a valid third-radio board.

  pinMode(CE_PIN_3, INPUT_PULLUP);
  delay(2);
  uint8_t q1LowSamples = 0;
  constexpr uint8_t kQ1Samples = 32;
  for (uint8_t i = 0; i < kQ1Samples; ++i) {
    if (digitalRead(CE_PIN_3) == LOW) ++q1LowSamples;
    delayMicroseconds(50);
  }
  const bool q1Present = q1LowSamples >= 24u;
  pinMode(CE_PIN_3, OUTPUT);
  digitalWrite(CE_PIN_3, LOW);  // Q1 off and NRF3 CE inactive.

  // Discharge GPIO21 briefly using open drain, then completely release it.
  // A fitted VS1838B restores the line HIGH on every pass. A bare nRF CSN
  // input is high impedance and cannot restore the discharged node.
  uint8_t receiverRiseRounds = 0;
  constexpr uint8_t kRiseRounds = 8;
  for (uint8_t round = 0; round < kRiseRounds; ++round) {
    pinMode(IR_RX_PIN, OUTPUT_OPEN_DRAIN);
    digitalWrite(IR_RX_PIN, LOW);
    delayMicroseconds(80);
    pinMode(IR_RX_PIN, INPUT);
    delayMicroseconds(25);
    const bool earlyHigh = digitalRead(IR_RX_PIN) == HIGH;
    delayMicroseconds(175);
    const bool settledHigh = digitalRead(IR_RX_PIN) == HIGH;
    if (earlyHigh && settledHigh) ++receiverRiseRounds;
  }
  const bool receiverPresent = receiverRiseRounds >= 6u;
  const bool detected = q1Present || receiverPresent;

  Serial.printf(
      "[ir-detect] Q1 low=%u/%u VS1838 rise=%u/%u -> %s\n",
      (unsigned)q1LowSamples, (unsigned)kQ1Samples,
      (unsigned)receiverRiseRounds, (unsigned)kRiseRounds,
      detected ? "IR topology" : "NRF3 topology");
  return detected;
}

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

  RF24 probe(ce, csn, kNrfSpiHz);
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

static bool configureIrFromBoardTopology(bool thirdRadioPresent,
                                         bool receiverElectricallyPresent) {
  // DIV V2.1 has two supported, mutually exclusive populations on the shared
  // GPIO14/GPIO21 nets: the original board fits Q1 + VS1838B and therefore
  // only two nRF radios; the three-radio modification removes those IR parts.
  // The electrical receiver probe has priority. This prevents a floating or
  // contended SPI read on an original IR board from being accepted as NRF3.
  digitalWrite(CE_PIN_3, LOW);
  if (receiverElectricallyPresent) {
    pinMode(IR_RX_PIN, INPUT_PULLUP);
    Serial.println(F("[ir-detect] VS1838B present -> NRF3 disabled"));
    return true;
  }
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
  Serial.println(F("[ir-detect] two-radio fallback -> IR hardware enabled"));
  return true;
}

}  // namespace

void nrfSupervisorInit(bool restoreSd) {
  pinMode(CE_PIN_1, OUTPUT);  digitalWrite(CE_PIN_1, LOW);
  pinMode(CE_PIN_2, OUTPUT);  digitalWrite(CE_PIN_2, LOW);
  pinMode(CE_PIN_3, OUTPUT);  digitalWrite(CE_PIN_3, LOW);
  pinMode(CSN_PIN_1, OUTPUT); digitalWrite(CSN_PIN_1, HIGH);
  pinMode(CSN_PIN_2, OUTPUT); digitalWrite(CSN_PIN_2, HIGH);

  // This must be the first operation on GPIO21. Once CSN3 has been driven by
  // the MCU it is no longer possible to distinguish the VS1838B idle output
  // from the level written by firmware.
  // Once an original IR population has been identified, keep that topology
  // latched for the whole boot. A remote may be transmitting while the bus is
  // rebuilt after NFC; re-probing its active-low output at that instant must
  // not reclassify the board as a three-radio unit.
  const bool irElectricalPresent =
      s_irReceiverDetected || probeIrReceiverElectrical();
  s_irReceiverDetected = irElectricalPresent;
  if (irElectricalPresent) {
    pinMode(CSN_PIN_3, INPUT_PULLUP);
  } else {
    pinMode(CSN_PIN_3, OUTPUT);
    digitalWrite(CSN_PIN_3, HIGH);
  }

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
    // Never toggle GPIO21 as CSN when the receiver is populated. NRF3 is
    // mutually exclusive with IR on the supported DIV V2.1 PCB topology.
    third = irElectricalPresent ? false : probeRadio(CE_PIN_3, CSN_PIN_3);
    if (first && second) break;

    Serial.printf("[nrf-detect] shared bus recovery round %u/3\n",
                  (unsigned)round);
    digitalWrite(CE_PIN_1, LOW);
    digitalWrite(CE_PIN_2, LOW);
    digitalWrite(CE_PIN_3, LOW);
    digitalWrite(CSN_PIN_1, HIGH);
    digitalWrite(CSN_PIN_2, HIGH);
    if (irElectricalPresent) {
      pinMode(CSN_PIN_3, INPUT_PULLUP);
    } else {
      digitalWrite(CSN_PIN_3, HIGH);
    }
    delay((unsigned long)round * 12UL);
  }
  s_detectedCount = third ? 3u : 2u;
  s_irReceiverDetected = configureIrFromBoardTopology(
      third, irElectricalPresent);

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
    if (s_irReceiverDetected) {
      pinMode(CSN_PIN_3, INPUT_PULLUP);
    } else {
      digitalWrite(CSN_PIN_3, HIGH);
    }
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
    RF24 third(CE_PIN_3, CSN_PIN_3, kNrfSpiHz);
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
