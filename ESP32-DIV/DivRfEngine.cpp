#include "DivRfEngine.h"

#include "NrfSupervisor.h"
#include "SettingsStore.h"
#include "shared.h"
#include "utils.h"

#include <RF24.h>
#include <SPI.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

// Declared in bluetooth.cpp/config.h. Keeping the narrow declaration here
// avoids coupling the engine to the complete feature registry in config.h.
namespace BleScan {
bool releaseControllerForWifi();
}

namespace DivRfEngine {
namespace {

// V1.81 definitive shared RF engine: balanced shuffled traversal at 80 us,
// 10 MHz SPI, 2 Mbps and PA MAX, with a complete stop/start fence.
constexpr uint32_t kModernSpiHz = 10000000;
constexpr uint32_t kHopDwellUs = 80;
constexpr uint32_t kHealthSampleMs = 500;
constexpr uint32_t kWorkerStopTimeoutMs = 1000;
constexpr uint32_t kRestartGuardUs = 5000;
constexpr uint32_t kSchedulerDelayPeriodUs = 3000;
constexpr uint32_t kWorkerStackWords = 4096;
constexpr UBaseType_t kWorkerPriority = 3;
constexpr BaseType_t kWorkerCore = 0;
constexpr uint8_t kMaximumPlanChannels = 32;

constexpr uint8_t kBleChannels[] = {2, 26, 80};
constexpr uint8_t kBluetoothChannels[] = {
    32, 34, 46, 48, 50, 52, 0, 1, 2, 4, 6,
    8, 22, 24, 26, 28, 30, 74, 76, 78, 80};
// IEEE 802.11 channels 1..13 expressed as nRF RF_CH (MHz - 2400).
constexpr uint8_t kWiFiChannels[] = {
    12, 17, 22, 27, 32, 37, 42, 47, 52, 57, 62, 67, 72};
constexpr uint8_t kVideoChannels[] = {70, 75, 80};
constexpr uint8_t kRcChannels[] = {1, 3, 5, 7};
constexpr uint8_t kUsbWirelessChannels[] = {40, 50, 60};
// IEEE 802.15.4 channels 11, 15, 20 and 25 expressed as nRF RF_CH.
constexpr uint8_t kZigbeeChannels[] = {5, 25, 50, 75};
constexpr uint8_t kNrf24Channels[] = {76, 78, 79};

struct ChannelPlan {
  const uint8_t* channels;
  uint8_t count;
};

template <size_t N>
constexpr ChannelPlan makePlan(const uint8_t (&channels)[N]) {
  return {channels, static_cast<uint8_t>(N)};
}

ChannelPlan channelPlan(Profile profile) {
  switch (profile) {
    case Profile::Ble: return makePlan(kBleChannels);
    case Profile::Bluetooth: return makePlan(kBluetoothChannels);
    case Profile::WiFi: return makePlan(kWiFiChannels);
    case Profile::Video: return makePlan(kVideoChannels);
    case Profile::Rc: return makePlan(kRcChannels);
    case Profile::UsbWireless: return makePlan(kUsbWirelessChannels);
    case Profile::Zigbee: return makePlan(kZigbeeChannels);
    case Profile::Nrf24:
    default: return makePlan(kNrf24Channels);
  }
}

RF24 radio1(CE_PIN_1, CSN_PIN_1, kModernSpiHz);
RF24 radio2(CE_PIN_2, CSN_PIN_2, kModernSpiHz);
RF24 radio3(CE_PIN_3, CSN_PIN_3, kModernSpiHz);
RF24* const radios[3] = {&radio1, &radio2, &radio3};
constexpr uint8_t kCePins[3] = {CE_PIN_1, CE_PIN_2, CE_PIN_3};
constexpr uint8_t kCsnPins[3] = {CSN_PIN_1, CSN_PIN_2, CSN_PIN_3};
constexpr char kRadioNames[3] = {'1', '2', '3'};

Snapshot engine = {};

bool busOwned = false;
uint8_t hopCursor = 0;
uint32_t activeStartedAt = 0;
uint32_t lastHopAtUs = 0;
uint32_t lastServiceAtUs = 0;
uint32_t lastHealthAtMs = 0;
uint32_t lastRadioWriteAtUs[3] = {0, 0, 0};
uint32_t lastSchedulerDelayAtUs = 0;
uint8_t channelOrder[kMaximumPlanChannels] = {};
uint8_t channelOrderCount = 0;
uint8_t healthRadioCursor = 0;
TaskHandle_t workerTask = nullptr;
volatile bool workerRunning = false;
volatile bool workerStopRequested = false;
volatile StopReason workerStopReason = StopReason::None;
volatile bool workerCleanupComplete = true;
uint32_t lastShutdownAtUs = 0;

void serviceHardwareOnce();
void finalizeStop(StopReason reason);
void workerMain(void*);
bool waitForQuiescentHardware(uint32_t timeoutMs);

uint8_t selectedRadioCount() {
  return thirdNrfEnabled() ? 3u : 2u;
}

uint32_t selectedSpiHz() { return kModernSpiHz; }

uint32_t selectedDwellUs() { return kHopDwellUs; }

rf24_datarate_e selectedDataRate() {
  return RF24_2MBPS;
}

const char* selectedDataRateName() {
  switch (selectedDataRate()) {
    case RF24_250KBPS: return "250K";
    case RF24_1MBPS: return "1M";
    case RF24_2MBPS:
    default: return "2M";
  }
}

void loadChannelOrder(const ChannelPlan& plan) {
  channelOrderCount = plan.count > kMaximumPlanChannels
                          ? kMaximumPlanChannels
                          : plan.count;
  for (uint8_t i = 0; i < channelOrderCount; ++i) {
    channelOrder[i] = plan.channels[i];
  }
}

void shuffleChannelOrder(uint8_t avoidFirst = 0xFF) {
  if (channelOrderCount < 2) return;
  for (uint8_t i = channelOrderCount - 1; i > 0; --i) {
    const uint8_t j = static_cast<uint8_t>(random(0, i + 1u));
    const uint8_t temporary = channelOrder[i];
    channelOrder[i] = channelOrder[j];
    channelOrder[j] = temporary;
  }
  if (channelOrder[0] == avoidFirst) {
    const uint8_t temporary = channelOrder[0];
    channelOrder[0] = channelOrder[1];
    channelOrder[1] = temporary;
  }
}

void setMandatoryPinsIdle() {
  pinMode(CE_PIN_1, OUTPUT);
  pinMode(CE_PIN_2, OUTPUT);
  pinMode(CSN_PIN_1, OUTPUT);
  pinMode(CSN_PIN_2, OUTPUT);
  digitalWrite(CE_PIN_1, LOW);
  digitalWrite(CE_PIN_2, LOW);
  digitalWrite(CSN_PIN_1, HIGH);
  digitalWrite(CSN_PIN_2, HIGH);
}

void setSelectedPinsIdle() {
  const uint8_t count = selectedRadioCount();
  for (uint8_t i = 0; i < count; ++i) {
    pinMode(kCePins[i], OUTPUT);
    pinMode(kCsnPins[i], OUTPUT);
    digitalWrite(kCePins[i], LOW);
    digitalWrite(kCsnPins[i], HIGH);
  }
  setMandatoryPinsIdle();
}

void waitForRestartGuard() {
  if (lastShutdownAtUs == 0) return;
  while (static_cast<uint32_t>(micros() - lastShutdownAtUs) <
         kRestartGuardUs) {
    delayMicroseconds(100);
  }
}

bool waitForQuiescentHardware(uint32_t timeoutMs) {
  const uint32_t waitStarted = millis();
  while (millis() - waitStarted < timeoutMs) {
    __sync_synchronize();
    if (!workerRunning && workerTask == nullptr && workerCleanupComplete &&
        !busOwned && engine.state != State::Active) {
      setSelectedPinsIdle();
      waitForRestartGuard();
      return true;
    }
    delay(1);
  }
  Serial.printf("[div-rf] quiescent wait timeout worker=%u task=%p cleanup=%u "
                "bus=%u state=%u\n",
                workerRunning ? 1u : 0u, workerTask,
                workerCleanupComplete ? 1u : 0u, busOwned ? 1u : 0u,
                static_cast<unsigned>(engine.state));
  return false;
}

void resetSnapshot(Profile profile) {
  engine.state = State::Off;
  engine.profile = profile;
  engine.stopReason = StopReason::None;
  engine.selectedRadios = selectedRadioCount();
  memset(engine.ready, 0, sizeof(engine.ready));
  memset(engine.attempts, 0, sizeof(engine.attempts));
  memset(engine.maximumPowerVerified, 0, sizeof(engine.maximumPowerVerified));
  memset(engine.activeChannels, 0xFF, sizeof(engine.activeChannels));
  engine.channelChanges = 0;
  engine.changesPerSecond = 0;
  engine.completeSweeps = 0;
  engine.sweepsPerSecond = 0;
  engine.serviceTicks = 0;
  engine.activeMilliseconds = 0;
  engine.maximumServiceGapUs = 0;
  engine.maximumRadioGapUs = 0;
  engine.healthChecks = 0;
  engine.healthFailures = 0;
  engine.schedulerYields = 0;
  engine.channelBatchSamples = 0;
  engine.channelBatchMicrosTotal = 0;
  engine.maximumChannelBatchMicros = 0;
  engine.workerStackMinimumWords = 0;
  engine.shutdownMicros = 0;
  engine.minimumFreeHeap = ESP.getFreeHeap();
  hopCursor = 0;
  lastSchedulerDelayAtUs = 0;
  channelOrderCount = 0;
  healthRadioCursor = 0;
  memset(lastRadioWriteAtUs, 0, sizeof(lastRadioWriteAtUs));
  lastHopAtUs = 0;
  lastServiceAtUs = 0;
  lastHealthAtMs = millis();
}

bool acquireBus() {
  neopixelClear();
  nrfApplyActiveRadioSelection();
  // This is the single foreground-entry fence. It parks SD, pauses every
  // background scanner, quiesces Wi-Fi and binds the shared pins to SPI.
  prepareNrfSharedSpiBus();
  busOwned = true;

  // The S3 controller is not part of the external nRF engine. Releasing it
  // here avoids an internal 2.4 GHz producer competing with the selected
  // profile. BLE features recreate the controller through ensureBleStackReady().
  if (!BleScan::releaseControllerForWifi()) {
    Serial.println(F("[div-rf] BLE controller did not quiesce; start refused"));
    releaseSharedSpiWithoutSdMount();
    busOwned = false;
    nrfApplyActiveRadioSelection();
    return false;
  }
  return true;
}

bool beginRadioReliable(uint8_t index) {
  if (index >= engine.selectedRadios) return false;
  RF24& radio = *radios[index];
  radio.setSPISpeed(selectedSpiHz());
  for (uint8_t attempt = 1; attempt <= 4; ++attempt) {
    engine.attempts[index] = attempt;
    const bool rawBusReady = verifyNrfSharedSpiDevice(
        kCePins[index], kCsnPins[index], 2);
    const bool began = rawBusReady && radio.begin();
    const bool connected = began && radio.isChipConnected();
    Serial.printf("[div-rf] NRF%c attempt=%u raw=%u begin=%u connected=%u\n",
                  kRadioNames[index], attempt, rawBusReady, began, connected);
    if (connected) return true;
    digitalWrite(kCePins[index], LOW);
    digitalWrite(kCsnPins[index], HIGH);
    if (attempt < 4) {
      prepareNrfSharedSpiBus();
      delay(attempt * 8u);
    }
  }
  return false;
}

bool beginSelectedRadios() {
  bool ok = true;
  for (uint8_t i = 0; i < engine.selectedRadios; ++i) {
    engine.ready[i] = beginRadioReliable(i);
    ok = ok && engine.ready[i];
  }
  return ok;
}

bool configureCarrier(uint8_t index, uint8_t channel) {
  RF24& radio = *radios[index];
  const rf24_datarate_e dataRate = selectedDataRate();
  radio.setAutoAck(false);
  radio.stopListening();
  radio.setRetries(0, 0);
  radio.setPALevel(RF24_PA_MAX, true);
  const bool rateSet = radio.setDataRate(dataRate);
  radio.setCRCLength(RF24_CRC_DISABLED);
  const uint8_t verifiedChannel = channel;
  // Start the carrier once; runtime changes only RF_CH.
  radio.startConstCarrier(RF24_PA_MAX, verifiedChannel);
  const bool verified = rateSet && radio.isChipConnected() &&
                        radio.getPALevel() == RF24_PA_MAX &&
                        radio.getDataRate() == dataRate &&
                         radio.getChannel() == verifiedChannel &&
                         radio.getSPISpeed() == selectedSpiHz();
  engine.maximumPowerVerified[index] = verified;
  engine.activeChannels[index] = verifiedChannel;
  Serial.printf("[div-rf] NRF%c PA=MAX rate=%s SPI=%luMHz CH=%u verify=%s\n",
                kRadioNames[index], selectedDataRateName(),
                static_cast<unsigned long>(selectedSpiHz() / 1000000u),
                verifiedChannel,
                verified ? "PASS" : "FAIL");
  return verified;
}

void releaseBusAndRestoreTopology() {
  if (busOwned) {
    releaseSharedSpiWithoutSdMount();
    busOwned = false;
  }
  // Critical on original two-radio hardware: restores GPIO21 to IR input.
  nrfApplyActiveRadioSelection();
}

void haltHardware() {
  if (busOwned) {
    for (uint8_t i = 0; i < engine.selectedRadios; ++i) {
      digitalWrite(kCePins[i], LOW);
      if (engine.ready[i]) {
        radios[i]->stopConstCarrier();
        radios[i]->stopListening();
        radios[i]->powerDown();
      }
      digitalWrite(kCePins[i], LOW);
      digitalWrite(kCsnPins[i], HIGH);
    }
  }
  setSelectedPinsIdle();
  releaseBusAndRestoreTopology();
}

bool initializeForTest(Profile profile) {
  if (engine.state == State::Active) stop(StopReason::Reinitialize);
  resetSnapshot(profile);
  if (!acquireBus()) {
    engine.state = State::Error;
    engine.stopReason = StopReason::BusFailure;
    setMandatoryPinsIdle();
    nrfApplyActiveRadioSelection();
    return false;
  }
  // Match RF-Clown's proven bring-up sequence: let the GPIO matrix, SPI
  // peripheral and radio supplies settle before the first RF24 transaction.
  delay(6);
  if (!beginSelectedRadios()) {
    engine.state = State::Error;
    engine.stopReason = StopReason::RadioFailure;
    haltHardware();
    return false;
  }
  return true;
}

void configureReceiver(RF24& radio, uint8_t channel) {
  radio.stopConstCarrier();
  radio.stopListening();
  radio.setAutoAck(false);
  radio.setRetries(0, 0);
  radio.setPALevel(RF24_PA_MIN, true);
  radio.setDataRate(RF24_2MBPS);
  radio.setCRCLength(RF24_CRC_DISABLED);
  radio.setChannel(channel);
  radio.startListening();
}

struct LinkPacket {
  uint8_t marker;
  uint8_t transmitter;
  uint8_t receiver;
  uint8_t sequence;
  uint32_t check;
};

void configurePacketRadio(RF24& radio, uint8_t channel, bool autoAck) {
  radio.stopConstCarrier();
  radio.stopListening();
  radio.setChannel(channel);
  radio.setDataRate(RF24_250KBPS);
  radio.setPALevel(RF24_PA_MIN, true);
  radio.setCRCLength(RF24_CRC_16);
  radio.setAutoAck(autoAck);
  radio.setRetries(autoAck ? 5 : 0, autoAck ? 15 : 0);
  radio.setPayloadSize(sizeof(LinkPacket));
  radio.flush_rx();
  radio.flush_tx();
  radio.powerUp();
}

void finishTest(bool passed, const char* tag) {
  haltHardware();
  engine.state = passed ? State::Off : State::Error;
  engine.stopReason = passed ? StopReason::User : StopReason::RadioFailure;
  Serial.printf("[%s] result=%s radios=%u carriers=OFF\n", tag,
                passed ? "PASS" : "FAIL", engine.selectedRadios);
}

}  // namespace

bool start(Profile profile) {
  // State::Active alone is not sufficient here: a fast UI stop/start can see
  // State::Off while the previous worker is still publishing its final state.
  // Drain every possible owner before touching the shared SPI bus again.
  if (engine.state == State::Active || busOwned || workerRunning ||
      workerTask != nullptr || !workerCleanupComplete) {
    stop(StopReason::Reinitialize);
  }
  if (!waitForQuiescentHardware(kWorkerStopTimeoutMs)) {
    engine.state = State::Error;
    engine.stopReason = StopReason::BusFailure;
    return false;
  }
  if (!initializeForTest(profile)) return false;

  const ChannelPlan plan = channelPlan(profile);
  loadChannelOrder(plan);
  shuffleChannelOrder();
  // Match the recovered four-radio bring-up: explicitly wake every selected
  // transceiver, then allow the supplies/oscillators to settle before CE is
  // asserted by startConstCarrier().
  for (uint8_t i = 0; i < engine.selectedRadios; ++i) {
    radios[i]->powerUp();
  }
  delay(5);
  bool verified = true;
  for (uint8_t i = 0; i < engine.selectedRadios; ++i) {
    verified = configureCarrier(i, channelOrder[0]) && verified;
  }
  if (!verified) {
    engine.state = State::Error;
    engine.stopReason = StopReason::RadioFailure;
    haltHardware();
    return false;
  }

  engine.state = State::Active;
  activeStartedAt = millis();
  lastHopAtUs = micros();
  lastServiceAtUs = lastHopAtUs;
  lastSchedulerDelayAtUs = lastHopAtUs;
  lastHealthAtMs = activeStartedAt;
  for (uint8_t i = 0; i < engine.selectedRadios; ++i) {
    lastRadioWriteAtUs[i] = lastHopAtUs;
  }
  hopCursor = 1u % channelOrderCount;
  workerStopRequested = false;
  workerStopReason = StopReason::None;
  workerCleanupComplete = false;
  workerRunning = true;
  __sync_synchronize();
  const BaseType_t taskCreated = xTaskCreatePinnedToCore(
      workerMain, "div-rf-worker", kWorkerStackWords, nullptr,
      kWorkerPriority, &workerTask, kWorkerCore);
  if (taskCreated != pdPASS) {
    workerRunning = false;
    workerTask = nullptr;
    workerCleanupComplete = true;
    __sync_synchronize();
    engine.state = State::Error;
    engine.stopReason = StopReason::BusFailure;
    haltHardware();
    Serial.println(F("[div-rf] worker creation failed"));
    return false;
  }

  Serial.printf("[div-rf] ACTIVE profile=%s radios=%u SPI=%luMHz PA=MAX "
                "rate=2M dwell_us=%lu scheduler=3ms\n",
                profileName(profile), engine.selectedRadios,
                static_cast<unsigned long>(selectedSpiHz() / 1000000u),
                static_cast<unsigned long>(selectedDwellUs()));
  return true;
}

namespace {

void serviceHardwareOnce() {
  if (engine.state != State::Active || !busOwned) return;

  const uint32_t nowUs = micros();
  const uint32_t gapUs = nowUs - lastServiceAtUs;
  lastServiceAtUs = nowUs;
  if (gapUs > engine.maximumServiceGapUs) engine.maximumServiceGapUs = gapUs;

  const uint32_t nowMs = millis();
  if (nowMs - lastHealthAtMs >= kHealthSampleMs) {
    const uint32_t heap = ESP.getFreeHeap();
    if (heap < engine.minimumFreeHeap) engine.minimumFreeHeap = heap;
    const uint32_t stackWords = uxTaskGetStackHighWaterMark(nullptr);
    if (engine.workerStackMinimumWords == 0 ||
        stackWords < engine.workerStackMinimumWords) {
      engine.workerStackMinimumWords = stackWords;
    }
    ++engine.healthChecks;
    // Probe one radio per sample. This preserves supervision without creating
    // a burst of six SPI reads every 500 ms in the RF hot path.
    const uint8_t i = healthRadioCursor;
    const bool healthy = radios[i]->isChipConnected() &&
                         radios[i]->getChannel() == engine.activeChannels[i];
    healthRadioCursor = static_cast<uint8_t>(
        (healthRadioCursor + 1u) % engine.selectedRadios);
    if (!healthy) {
      ++engine.healthFailures;
      workerStopReason = StopReason::BusFailure;
      workerStopRequested = true;
      Serial.println(F("[div-rf] 500ms RF24 health check failed"));
    }
    lastHealthAtMs = nowMs;
  }
  if (workerStopRequested) return;

  if (nowUs - lastHopAtUs < selectedDwellUs()) return;
  lastHopAtUs = nowUs;

  uint8_t changes = 0;
  const uint8_t channel = channelOrder[hopCursor];
  const uint32_t channelBatchStartedUs = micros();
  for (uint8_t i = 0; i < engine.selectedRadios; ++i) {
    radios[i]->setChannel(channel);
    engine.activeChannels[i] = channel;
    const uint32_t radioGapUs = nowUs - lastRadioWriteAtUs[i];
    lastRadioWriteAtUs[i] = nowUs;
    if (radioGapUs > engine.maximumRadioGapUs) {
      engine.maximumRadioGapUs = radioGapUs;
    }
    ++changes;
  }
  const uint32_t channelBatchMicros = micros() - channelBatchStartedUs;
  ++engine.channelBatchSamples;
  engine.channelBatchMicrosTotal += channelBatchMicros;
  if (channelBatchMicros > engine.maximumChannelBatchMicros) {
    engine.maximumChannelBatchMicros = channelBatchMicros;
  }
  if (++hopCursor >= channelOrderCount) {
    ++engine.completeSweeps;
    shuffleChannelOrder(channel);
    hopCursor = 0;
  }
  engine.channelChanges += changes;
  ++engine.serviceTicks;
}

void workerMain(void*) {
  while (!workerStopRequested && engine.state == State::Active) {
    serviceHardwareOnce();
    const uint32_t nowUs = micros();
    if (nowUs - lastSchedulerDelayAtUs >= kSchedulerDelayPeriodUs) {
      lastSchedulerDelayAtUs = nowUs;
      ++engine.schedulerYields;
      vTaskDelay(1);
    }
  }

  StopReason reason = workerStopReason;
  if (reason == StopReason::None) reason = StopReason::User;
  finalizeStop(reason);
  workerTask = nullptr;
  workerCleanupComplete = true;
  __sync_synchronize();
  workerRunning = false;
  __sync_synchronize();
  vTaskDelete(nullptr);
}

void finalizeStop(StopReason reason) {
  const uint32_t stopStartedUs = micros();
  if (engine.state == State::Active) {
    engine.activeMilliseconds = millis() - activeStartedAt;
    if (engine.activeMilliseconds != 0) {
      engine.changesPerSecond = static_cast<uint32_t>(
          (static_cast<uint64_t>(engine.channelChanges) * 1000ull) /
          engine.activeMilliseconds);
      engine.sweepsPerSecond = static_cast<uint32_t>(
          (static_cast<uint64_t>(engine.completeSweeps) * 1000ull) /
          engine.activeMilliseconds);
    }
  }
  haltHardware();
  lastShutdownAtUs = micros();
  engine.shutdownMicros = micros() - stopStartedUs;
  engine.stopReason = reason;
  engine.state = (reason == StopReason::RadioFailure ||
                  reason == StopReason::BusFailure)
                     ? State::Error
                     : State::Off;
  const uint32_t averageChannelBatchMicros = engine.channelBatchSamples != 0
      ? engine.channelBatchMicrosTotal / engine.channelBatchSamples
      : 0;
  Serial.printf("[div-rf] STOP reason=%s radios=%u writes=%lu writes_s=%lu "
                "sweeps=%lu sweeps_s=%lu gap_us=%lu radio_gap_us=%lu "
                "health=%lu/%lu yields=%lu stack_words=%lu active_ms=%lu "
                "stop_us=%lu dwell_us=%lu "
                "spi_batch_avg_us=%lu spi_batch_max_us=%lu "
                "final_ch=%u/%u/%u "
                "carriers=OFF\n",
                stopReasonName(reason),
                engine.selectedRadios,
                static_cast<unsigned long>(engine.channelChanges),
                static_cast<unsigned long>(engine.changesPerSecond),
                static_cast<unsigned long>(engine.completeSweeps),
                static_cast<unsigned long>(engine.sweepsPerSecond),
                static_cast<unsigned long>(engine.maximumServiceGapUs),
                static_cast<unsigned long>(engine.maximumRadioGapUs),
                static_cast<unsigned long>(engine.healthChecks),
                static_cast<unsigned long>(engine.healthFailures),
                static_cast<unsigned long>(engine.schedulerYields),
                static_cast<unsigned long>(engine.workerStackMinimumWords),
                static_cast<unsigned long>(engine.activeMilliseconds),
                static_cast<unsigned long>(engine.shutdownMicros),
                static_cast<unsigned long>(selectedDwellUs()),
                static_cast<unsigned long>(averageChannelBatchMicros),
                static_cast<unsigned long>(engine.maximumChannelBatchMicros),
                engine.activeChannels[0], engine.activeChannels[1],
                engine.activeChannels[2]);
}

}  // namespace

void service() {
  // RF24 register access is owned exclusively by workerMain() while active.
  // The foreground call remains as a cooperative yield for the existing UI.
  if (workerRunning) delay(0);
}

void stop(StopReason reason) {
  if (workerRunning && workerTask != nullptr &&
      xTaskGetCurrentTaskHandle() != workerTask) {
    workerStopReason = reason;
    workerStopRequested = true;
    const uint32_t waitStarted = millis();
    while ((workerRunning || workerTask != nullptr ||
            !workerCleanupComplete) &&
           millis() - waitStarted < kWorkerStopTimeoutMs) {
      delay(1);
    }
    if (!workerRunning && workerTask == nullptr && workerCleanupComplete) {
      if (!waitForQuiescentHardware(kWorkerStopTimeoutMs)) {
        engine.state = State::Error;
        engine.stopReason = StopReason::BusFailure;
      }
      return;
    }

    // A wedged worker must not keep CE asserted or retain the shared bus.
    Serial.println(F("[div-rf] worker stop timeout; forcing RF shutdown"));
    TaskHandle_t stuckWorker = workerTask;
    workerTask = nullptr;
    workerRunning = false;
    workerCleanupComplete = false;
    __sync_synchronize();
    if (stuckWorker != nullptr) vTaskDelete(stuckWorker);
    finalizeStop(StopReason::BusFailure);
    workerCleanupComplete = true;
    __sync_synchronize();
    waitForQuiescentHardware(kWorkerStopTimeoutMs);
    return;
  }

  if (engine.state == State::Active || busOwned) {
    finalizeStop(reason);
  } else {
    engine.stopReason = reason;
  }
  workerTask = nullptr;
  workerRunning = false;
  workerCleanupComplete = true;
  __sync_synchronize();
  waitForQuiescentHardware(kWorkerStopTimeoutMs);
}

bool isActive() { return engine.state == State::Active; }

const Snapshot& snapshot() { return engine; }

bool registerSelfTest() {
  Serial.println(F("[div-rf-reg] start"));
  if (!initializeForTest(Profile::Nrf24)) return false;
  bool passed = true;
  for (uint8_t i = 0; i < engine.selectedRadios; ++i) {
    RF24& radio = *radios[i];
    const uint8_t requestedChannel = 31u + i * 17u;
    radio.setChannel(requestedChannel);
    const bool rateSet = radio.setDataRate(RF24_1MBPS);
    radio.setPALevel(RF24_PA_MAX, true);
    radio.setCRCLength(RF24_CRC_16);
    const bool onePassed = radio.isChipConnected() &&
        radio.getChannel() == requestedChannel && rateSet &&
        radio.getDataRate() == RF24_1MBPS &&
        radio.getPALevel() == RF24_PA_MAX &&
        radio.getCRCLength() == RF24_CRC_16 && !radio.failureDetected;
    passed = passed && onePassed;
    Serial.printf("[div-rf-reg] NRF%c CH=%u PA=%u result=%s\n",
                  kRadioNames[i], radio.getChannel(), radio.getPALevel(),
                  onePassed ? "PASS" : "FAIL");
    radio.failureDetected = false;
  }
  finishTest(passed, "div-rf-reg");
  return passed;
}

bool maximumPowerSelfTest(uint16_t dwellMs) {
  if (!start(Profile::Nrf24)) return false;
  delay(dwellMs);
  bool passed = true;
  for (uint8_t i = 0; i < engine.selectedRadios; ++i) {
    passed = passed && engine.maximumPowerVerified[i];
  }
  stop(passed ? StopReason::User : StopReason::RadioFailure);
  return passed;
}

bool carrierCouplingSelfTest(uint8_t channel, uint16_t dwellMs) {
  Serial.printf("[div-rf-rpd] start CH=%u dwell=%u\n", channel, dwellMs);
  if (!initializeForTest(Profile::Nrf24)) return false;
  bool observed[3][3] = {};
  for (uint8_t transmitter = 0; transmitter < engine.selectedRadios;
       ++transmitter) {
    for (uint8_t i = 0; i < engine.selectedRadios; ++i) {
      radios[i]->stopConstCarrier();
      radios[i]->stopListening();
      digitalWrite(kCePins[i], LOW);
    }
    for (uint8_t receiver = 0; receiver < engine.selectedRadios; ++receiver) {
      if (receiver != transmitter) configureReceiver(*radios[receiver], channel);
    }
    radios[transmitter]->powerUp();
    radios[transmitter]->startConstCarrier(RF24_PA_MAX, channel);
    delay(dwellMs);
    for (uint8_t receiver = 0; receiver < engine.selectedRadios; ++receiver) {
      if (receiver != transmitter) {
        observed[transmitter][receiver] = radios[receiver]->testRPD();
      }
    }
    radios[transmitter]->stopConstCarrier();
    for (uint8_t receiver = 0; receiver < engine.selectedRadios; ++receiver) {
      if (receiver != transmitter) {
        Serial.printf("[div-rf-rpd] NRF%c->NRF%c observed=%u\n",
                      kRadioNames[transmitter], kRadioNames[receiver],
                      observed[transmitter][receiver]);
      }
    }
  }
  bool passed = true;
  for (uint8_t transmitter = 0; transmitter < engine.selectedRadios;
       ++transmitter) {
    bool seenByAnother = false;
    for (uint8_t receiver = 0; receiver < engine.selectedRadios; ++receiver) {
      if (receiver != transmitter) {
        seenByAnother = seenByAnother || observed[transmitter][receiver];
      }
    }
    passed = passed && seenByAnother;
  }
  finishTest(passed, "div-rf-rpd");
  return passed;
}

bool packetLinkSelfTest(uint8_t channel) {
  Serial.printf("[div-rf-link] start CH=%u ACK=ON\n", channel);
  if (!initializeForTest(Profile::Nrf24)) return false;
  const uint8_t address[6] = {'M', 'K', 'D', 'I', 'V', 0};
  uint8_t sequence = 0;
  bool passed = true;
  for (uint8_t txIndex = 0; txIndex < engine.selectedRadios; ++txIndex) {
    for (uint8_t rxIndex = 0; rxIndex < engine.selectedRadios; ++rxIndex) {
      if (txIndex == rxIndex) continue;
      for (uint8_t i = 0; i < engine.selectedRadios; ++i) {
        configurePacketRadio(*radios[i], channel, true);
        radios[i]->powerDown();
      }
      RF24& tx = *radios[txIndex];
      RF24& rx = *radios[rxIndex];
      configurePacketRadio(tx, channel, true);
      configurePacketRadio(rx, channel, true);
      rx.openReadingPipe(1, address);
      tx.openWritingPipe(address);
      rx.startListening();
      delay(5);
      const LinkPacket sent = {
          0xA5, txIndex, rxIndex, sequence++, 0x4D4B4449ul};
      const bool acknowledged = tx.write(&sent, sizeof(sent));
      delay(3);
      const bool available = rx.available();
      LinkPacket received = {};
      if (available) rx.read(&received, sizeof(received));
      const bool payloadOk = available &&
          memcmp(&sent, &received, sizeof(sent)) == 0;
      const bool onePassed = acknowledged && payloadOk;
      passed = passed && onePassed;
      Serial.printf("[div-rf-link] NRF%c->NRF%c ack=%u payload=%u %s\n",
                    kRadioNames[txIndex], kRadioNames[rxIndex], acknowledged,
                    payloadOk, onePassed ? "PASS" : "FAIL");
      tx.stopListening();
      rx.stopListening();
      tx.powerDown();
      rx.powerDown();
      delay(8);
    }
  }
  finishTest(passed, "div-rf-link");
  return passed;
}

bool noAckPacketSelfTest(uint8_t channel, uint8_t packetsPerLink) {
  if (packetsPerLink == 0) packetsPerLink = 1;
  Serial.printf("[div-rf-noack] start CH=%u packets=%u\n", channel,
                packetsPerLink);
  if (!initializeForTest(Profile::Nrf24)) return false;
  const uint8_t address[6] = {'M', 'K', 'N', 'A', 'K', 0};
  uint8_t sequence = 0;
  bool passed = true;
  for (uint8_t txIndex = 0; txIndex < engine.selectedRadios; ++txIndex) {
    for (uint8_t rxIndex = 0; rxIndex < engine.selectedRadios; ++rxIndex) {
      if (txIndex == rxIndex) continue;
      RF24& tx = *radios[txIndex];
      RF24& rx = *radios[rxIndex];
      for (uint8_t i = 0; i < engine.selectedRadios; ++i) {
        configurePacketRadio(*radios[i], channel, false);
        radios[i]->powerDown();
      }
      configurePacketRadio(tx, channel, false);
      configurePacketRadio(rx, channel, false);
      rx.openReadingPipe(1, address);
      tx.openWritingPipe(address);
      rx.startListening();
      delay(5);
      uint8_t txComplete = 0;
      uint8_t valid = 0;
      for (uint8_t packet = 0; packet < packetsPerLink; ++packet) {
        const LinkPacket sent = {
            0x5A, txIndex, rxIndex, sequence++, 0x4E414B31ul};
        if (tx.write(&sent, sizeof(sent))) ++txComplete;
        delay(2);
        while (rx.available()) {
          LinkPacket received = {};
          rx.read(&received, sizeof(received));
          if (memcmp(&sent, &received, sizeof(sent)) == 0) ++valid;
        }
      }
      const bool onePassed = txComplete == packetsPerLink &&
                             valid == packetsPerLink;
      passed = passed && onePassed;
      Serial.printf("[div-rf-noack] NRF%c->NRF%c tx=%u/%u valid=%u %s\n",
                    kRadioNames[txIndex], kRadioNames[rxIndex], txComplete,
                    packetsPerLink, valid, onePassed ? "PASS" : "FAIL");
      tx.stopListening();
      rx.stopListening();
      tx.powerDown();
      rx.powerDown();
      delay(8);
    }
  }
  finishTest(passed, "div-rf-noack");
  return passed;
}

bool transitionSoakSelfTest(uint16_t cycles) {
  if (cycles == 0) cycles = 1;
  static const Profile profiles[] = {
      Profile::WiFi, Profile::Video, Profile::Rc, Profile::Ble,
      Profile::Bluetooth, Profile::UsbWireless, Profile::Zigbee,
      Profile::Nrf24};
  bool passed = true;
  uint32_t worstStopUs = 0;
  for (uint16_t cycle = 0; cycle < cycles && passed; ++cycle) {
    const Profile profile = profiles[cycle % 8u];
    passed = start(profile);
    if (!passed) break;
    const uint32_t dwellStart = millis();
    while (millis() - dwellStart < 3u) service();
    stop(StopReason::User);
    if (engine.shutdownMicros > worstStopUs) worstStopUs = engine.shutdownMicros;
    const bool mandatoryIdle = digitalRead(CE_PIN_1) == LOW &&
                               digitalRead(CE_PIN_2) == LOW &&
                               digitalRead(CSN_PIN_1) == HIGH &&
                               digitalRead(CSN_PIN_2) == HIGH;
    passed = engine.state == State::Off && mandatoryIdle && !busOwned;
    if (((cycle + 1u) % 10u) == 0u || cycle + 1u == cycles) {
      Serial.printf("[div-rf-soak] %u/%u heap=%u %s\n", cycle + 1u,
                    cycles, ESP.getFreeHeap(), passed ? "PASS" : "FAIL");
    }
  }
  Serial.printf("[div-rf-soak] complete worst_stop_us=%lu result=%s\n",
                static_cast<unsigned long>(worstStopUs),
                passed ? "PASS" : "FAIL");
  return passed;
}

void printDiagnosticReport() {
  const uint32_t averageChannelBatchMicros = engine.channelBatchSamples != 0
      ? engine.channelBatchMicrosTotal / engine.channelBatchSamples
      : 0;
  Serial.printf(
      "[div-rf-health] state=%s profile=%s radios=%u ready=%u/%u/%u "
      "max=%u/%u/%u ch=%u/%u/%u changes=%lu changes_s=%lu ticks=%lu "
      "sweeps=%lu sweeps_s=%lu gap_us=%lu radio_gap_us=%lu "
      "health=%lu/%lu yields=%lu worker_stack=%lu stop_us=%lu heap=%u "
      "min_heap=%lu bus=%u dwell_us=%lu scheduler_us=%lu "
      "spi_batch_avg_us=%lu spi_batch_max_us=%lu\n",
      stateName(engine.state), profileName(engine.profile),
      engine.selectedRadios, engine.ready[0], engine.ready[1], engine.ready[2],
      engine.maximumPowerVerified[0], engine.maximumPowerVerified[1],
      engine.maximumPowerVerified[2], engine.activeChannels[0],
      engine.activeChannels[1], engine.activeChannels[2],
      static_cast<unsigned long>(engine.channelChanges),
      static_cast<unsigned long>(engine.changesPerSecond),
      static_cast<unsigned long>(engine.serviceTicks),
      static_cast<unsigned long>(engine.completeSweeps),
      static_cast<unsigned long>(engine.sweepsPerSecond),
      static_cast<unsigned long>(engine.maximumServiceGapUs),
      static_cast<unsigned long>(engine.maximumRadioGapUs),
      static_cast<unsigned long>(engine.healthChecks),
      static_cast<unsigned long>(engine.healthFailures),
      static_cast<unsigned long>(engine.schedulerYields),
      static_cast<unsigned long>(engine.workerStackMinimumWords),
      static_cast<unsigned long>(engine.shutdownMicros), ESP.getFreeHeap(),
      static_cast<unsigned long>(engine.minimumFreeHeap), busOwned,
      static_cast<unsigned long>(selectedDwellUs()),
      static_cast<unsigned long>(kSchedulerDelayPeriodUs),
      static_cast<unsigned long>(averageChannelBatchMicros),
      static_cast<unsigned long>(engine.maximumChannelBatchMicros));
}

const char* profileName(Profile profile) {
  switch (profile) {
    case Profile::Ble: return "BLE";
    case Profile::Bluetooth: return "BLUETOOTH";
    case Profile::WiFi: return "WIFI";
    case Profile::Video: return "VIDEO";
    case Profile::Rc: return "RC";
    case Profile::UsbWireless: return "USB";
    case Profile::Zigbee: return "ZIGBEE";
    case Profile::Nrf24: return "NRF24";
    default: return "?";
  }
}

const char* stateName(State state) {
  switch (state) {
    case State::Active: return "ACTIVE";
    case State::Error: return "ERROR";
    case State::Off:
    default: return "OFF";
  }
}

const char* stopReasonName(StopReason reason) {
  switch (reason) {
    case StopReason::User: return "USER";
    case StopReason::Exit: return "EXIT";
    case StopReason::ModeChange: return "MODE";
    case StopReason::RadioFailure: return "RADIO";
    case StopReason::BusFailure: return "BUS";
    case StopReason::Reinitialize: return "REINIT";
    case StopReason::None:
    default: return "NONE";
  }
}

}  // namespace DivRfEngine
