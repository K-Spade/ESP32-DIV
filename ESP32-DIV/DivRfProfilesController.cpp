#include "DivRfProfilesController.h"

namespace DivRfProfilesController {
namespace {

constexpr DivRfEngine::Profile kProfiles[kProfileCount] = {
    DivRfEngine::Profile::Ble,
    DivRfEngine::Profile::Bluetooth,
    DivRfEngine::Profile::WiFi,
    DivRfEngine::Profile::Video,
    DivRfEngine::Profile::Rc,
    DivRfEngine::Profile::UsbWireless,
    DivRfEngine::Profile::Zigbee,
    DivRfEngine::Profile::Nrf24};

uint8_t currentIndex = 0;

}  // namespace

void reset(uint8_t initialIndex) {
  DivRfEngine::stop(DivRfEngine::StopReason::Reinitialize);
  currentIndex = initialIndex < kProfileCount ? initialIndex : 0;
}

uint8_t selectedIndex() { return currentIndex; }

DivRfEngine::Profile selectedProfile() { return kProfiles[currentIndex]; }

const char* selectedName() {
  return DivRfEngine::profileName(selectedProfile());
}

bool selectIndex(uint8_t index) {
  if (index >= kProfileCount) return false;
  if (index == currentIndex) return true;
  const bool restart = DivRfEngine::isActive();
  if (restart) DivRfEngine::stop(DivRfEngine::StopReason::ModeChange);
  currentIndex = index;
  return !restart || DivRfEngine::start(selectedProfile());
}

bool selectNext() {
  return selectIndex((currentIndex + 1u) % kProfileCount);
}

bool selectPrevious() {
  return selectIndex((currentIndex + kProfileCount - 1u) % kProfileCount);
}

bool toggle() {
  if (DivRfEngine::isActive()) {
    DivRfEngine::stop(DivRfEngine::StopReason::User);
    return true;
  }
  return startSelected();
}

bool startSelected() { return DivRfEngine::start(selectedProfile()); }

void service() { DivRfEngine::service(); }

void exit() { DivRfEngine::stop(DivRfEngine::StopReason::Exit); }

}  // namespace DivRfProfilesController
