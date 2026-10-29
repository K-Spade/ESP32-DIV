#pragma once

// Reserve the bottom 20 pixels for the DIV five-zone touch controller.  The
// original Mini V3 games used the whole 128x128 panel; on DIV the playable
// surface is 240x300 and the touch bar is drawn over the remaining area.
#undef TFT_HEIGHT
#define TFT_HEIGHT 300

// The shared game framebuffer is four bits per pixel. Keep game source using
// familiar TFT colour names while mapping them to its fixed 16-colour palette.
#undef TFT_BLACK
#undef TFT_NAVY
#undef TFT_DARKGREEN
#undef TFT_DARKCYAN
#undef TFT_MAROON
#undef TFT_PURPLE
#undef TFT_OLIVE
#undef TFT_LIGHTGREY
#undef TFT_DARKGREY
#undef TFT_BLUE
#undef TFT_GREEN
#undef TFT_CYAN
#undef TFT_RED
#undef TFT_MAGENTA
#undef TFT_YELLOW
#undef TFT_WHITE
#undef TFT_ORANGE

#define TFT_BLACK 0
#define TFT_NAVY 1
#define TFT_DARKGREEN 2
#define TFT_DARKCYAN 3
#define TFT_MAROON 4
#define TFT_PURPLE 5
#define TFT_ORANGE 6
#define TFT_LIGHTGREY 7
#define TFT_DARKGREY 8
#define TFT_BLUE 9
#define TFT_GREEN 10
#define TFT_CYAN 11
#define TFT_RED 12
#define TFT_MAGENTA 13
#define TFT_YELLOW 14
#define TFT_WHITE 15

