#pragma once

#include <Arduino.h>

/** Probe the physical nRF24 population and isolate any unselected radio. */
void nrfSupervisorInit(bool restoreSd = true);

uint8_t nrfDetectedHardwareCount();
/** True only when the VS1838B output electrically holds the shared RX pin
 *  HIGH against a pulldown. This is independent of the number of nRF radios. */
bool nrfIrHardwareAvailable();
/** Isolate/power down NRF3, when present, before IR reuses GPIO14/GPIO21. */
bool nrfPrepareIrHardwareForUse();
