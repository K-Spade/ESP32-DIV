#include "shared.h"
#include "SettingsStore.h"


UiPalette UI = {BG_Dark, FG_Dark, ICON_Dark, TEXT_Dark, MK_SIGNAL_GREEN,
                LINE_Dark, L_Dark, MK_SIGNAL_GREEN, GREEN,
                HIGHLIGHT_Dark, SHADOW_Dark, MOTIF_Dark};

uint16_t uiUniversalColor() {
  return UI.warn;
}

void applyThemeToPalette(Theme t) {
  const uint16_t universal = accentColor565(settings().accentColor);

  if (t == Theme::Light) {
    UI.bg     =  BG_Light;
    UI.fg     =  FG_Light;
    UI.icon   =  universal;
    UI.text   =  TEXT_Light;
    UI.line   =  LINE_Light;
    UI.accent =  universal;
    UI.lable  =  L_Light;
    UI.warn   =  universal;
    UI.ok     =  GREEN;
    UI.highlight = HIGHLIGHT_Light;
    UI.shadow    = SHADOW_Light;
    UI.motif     = MOTIF_Light;

  } else {
    UI.bg     =  BG_Dark;
    UI.fg     =  FG_Dark;
    UI.icon   =  universal;
    UI.text   =  TEXT_Dark;
    UI.line   =  LINE_Dark;
    UI.accent =  universal;
    UI.lable  =  L_Dark;
    UI.warn   =  universal;
    UI.ok     =  GREEN;
    UI.highlight = HIGHLIGHT_Dark;
    UI.shadow    = SHADOW_Dark;
    UI.motif     = MOTIF_Dark;
  }
}
