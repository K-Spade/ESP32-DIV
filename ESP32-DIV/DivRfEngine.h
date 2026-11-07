#pragma once

#include <Arduino.h>

// Adapted RF profile engine for ESP32 DIV V2.1.
//
// Integration contract:
// - NrfSupervisor/SettingsStore remain the hardware authority.
// - The engine uses exactly 2 radios, or 3 only when thirdNrfEnabled() is true.
// - The shared SPI bus is acquired/released exclusively through utils.cpp.
// - A dedicated worker is the sole RF24 owner while active; service() only
//   yields cooperatively to preserve the existing feature-loop contract.
namespace DivRfEngine {

enum class Profile : uint8_t {
  Ble,
  Bluetooth,
  WiFi,
  Video,
  Rc,
  UsbWireless,
  Zigbee,
  Nrf24
};

enum class State : uint8_t { Off, Active, Error };

enum class StopReason : uint8_t {
  None,
  User,
  Exit,
  ModeChange,
  RadioFailure,
  BusFailure,
  Reinitialize
};

struct Snapshot {
  State state;
  Profile profile;
  StopReason stopReason;
  uint8_t selectedRadios;
  bool ready[3];
  uint8_t attempts[3];
  bool maximumPowerVerified[3];
  uint8_t activeChannels[3];
  uint32_t channelChanges;
  uint32_t changesPerSecond;
  uint32_t completeSweeps;
  uint32_t sweepsPerSecond;
  uint32_t serviceTicks;
  uint32_t activeMilliseconds;
  uint32_t maximumServiceGapUs;
  uint32_t maximumRadioGapUs;
  uint32_t healthChecks;
  uint32_t healthFailures;
  uint32_t schedulerYields;
  uint32_t channelBatchSamples;
  uint32_t channelBatchMicrosTotal;
  uint32_t maximumChannelBatchMicros;
  uint32_t workerStackMinimumWords;
  uint32_t shutdownMicros;
  uint32_t minimumFreeHeap;
};

// Starts a PA=MAX constant-carrier profile at the selected test bitrate.
// Returns false with every CE low and every selected CSN high when the bus or
// a selected radio fails.
bool start(Profile profile);

// Non-blocking compatibility hook. RF work is performed by the worker.
void service();

// Idempotent and the only supported shutdown path.
void stop(StopReason reason = StopReason::User);

bool isActive();
const Snapshot& snapshot();
const char* profileName(Profile profile);
const char* stateName(State state);
const char* stopReasonName(StopReason reason);

// Controlled-bench diagnostics. They adapt automatically to the selected
// physical population: 2 directed links for two radios, 6 for three radios.
bool registerSelfTest();
bool maximumPowerSelfTest(uint16_t dwellMs = 30);
bool carrierCouplingSelfTest(uint8_t channel = 76, uint16_t dwellMs = 30);
bool packetLinkSelfTest(uint8_t channel = 76);
bool noAckPacketSelfTest(uint8_t channel = 76, uint8_t packetsPerLink = 16);
bool transitionSoakSelfTest(uint16_t cycles = 100);
void printDiagnosticReport();

}  // namespace DivRfEngine
