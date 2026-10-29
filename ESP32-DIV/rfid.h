#pragma once

#include <stdint.h>

/** PN532-based RFID/NFC (ported from NullTag-style flows). Install "Adafruit PN532". */
namespace RfidNfc {

/** Probe PN532 on SPI (shared bus). On failure, remounts SD; on success, hands SPI to SD pins until session exit. */
bool begin();

/** True after successful begin(). */
bool hardwareOk();

/** Clear cached clone source (optional). */
void resetCloneBuffer();

/** Clear session-retry flag before entering a feature loop. */
void clearSessionRetry();

/** @return true once if user pressed Ready on a failure result screen. */
bool consumeSessionRetry();

/** Start one guarded NFC menu operation. */
void beginGuardedSession();

/**
 * Do not return ownership of the shared peripherals until at least one valid
 * ISO14443A tag has been read, then perform the final PN532 bus teardown.
 */
void ensureTagReadBeforeExit();

void sessionCardReader();
void sessionClone();
void sessionErase();
void sessionDump();
void sessionDecodeAccess();
void sessionJamReader();
void sessionDisruptEmulate();
void sessionTagDisrupt();

} // namespace RfidNfc
