#include "Display.h"
#include "Switches.h"
#include "shared.h"

Display display_obj(::tft);
Switches u_btn(BTN_UP, 1000);
Switches d_btn(BTN_DOWN, 1000);
Switches l_btn(BTN_LEFT, 1000);
Switches r_btn(BTN_RIGHT, 1000);
Switches c_btn(BTN_SELECT, 1000);

