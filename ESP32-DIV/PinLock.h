#pragma once

namespace PinLock {

/** True when a valid PIN record is stored in internal NVS. */
bool enabled();

/** Show the boot keypad when the lock is enabled. Returns only after the
 * configured PIN has been entered correctly. */
void bootGate();

/** Interactive Tools screen used to configure or disable the PIN lock. */
void manage();

}  // namespace PinLock
