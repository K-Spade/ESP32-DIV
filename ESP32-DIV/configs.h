#pragma once

// Compatibility surface for the MIT-licensed MarauderEternal games.  The DIV
// owns the real display and input drivers; the games only need to know that a
// screen and five controls are available.
#include "shared.h"

#define HAS_SCREEN 1
#define HAS_BUTTONS 1
#define U_BTN BTN_UP
#define D_BTN BTN_DOWN
#define L_BTN BTN_LEFT
#define R_BTN BTN_RIGHT
#define C_BTN BTN_SELECT

