#pragma once

#include <Arduino.h>

/** Probe the physical nRF24 population and isolate any unselected radio. */
void nrfSupervisorInit(bool restoreSd = true);

uint8_t nrfDetectedHardwareCount();
/** True when the original two-radio/VS1838B topology is selected. The
 *  electrical GPIO21 probe has priority; a missing NRF3 is the safe fallback. */
bool nrfIrHardwareAvailable();
/** Isolate/power down NRF3, when present, before IR reuses GPIO14/GPIO21. */
bool nrfPrepareIrHardwareForUse();
