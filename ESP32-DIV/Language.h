#pragma once

#include <Arduino.h>

enum class UiLanguage : uint8_t {
  English = 0,
  Spanish = 1,
  Russian = 2,
  Portuguese = 3,
  Japanese = 4,
  Thai = 5,
  Korean = 6,
  Chinese = 7,
};

constexpr uint8_t UI_LANGUAGE_COUNT = 8;
void uiSetLanguage(UiLanguage language);
UiLanguage uiLanguage();
const char* uiLanguageName(UiLanguage language);
const char* tr(const char* english);
