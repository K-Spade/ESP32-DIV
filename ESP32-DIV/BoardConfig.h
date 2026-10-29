#pragma once

// Select the hardware target.
// Leave all lines commented to use the ESP32-DIV V2 wiring.

// #define BOARD_CYD
// #define BOARD_ESP32_DIV_V1
#define BOARD_ESP32_DIV_V2

// Set to 0 to hide the on-screen touch nav bar (5 footer buttons).
// Touch button input will still work when this is disabled.
#define TOUCH_BUTTON_CUE_ENABLED 1

// Optional fixed PCF8574 I2C address (0x20-0x27). Leave commented for auto-detect.
//#define pcf_ADDR 0x20

// Factory calibration captured from this DIV V2.1 production panel. A valid
// /config/settings.json on SD can still override it, but touch remains usable
// when the card is absent or temporarily unmounted.
#define TOUCH_X_MIN 613
#define TOUCH_X_MAX 3603
#define TOUCH_Y_MIN 3719
#define TOUCH_Y_MAX 627

#if !defined(BOARD_ESP32_DIV_V2) && !defined(BOARD_CYD) && !defined(BOARD_ESP32_DIV_V1)
#define BOARD_ESP32_DIV_V2
#endif
