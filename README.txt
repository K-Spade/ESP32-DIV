MK SIGNAL DIV V2.1 - V1.90 Stable
=================================

PACKAGE CONTENTS
----------------

ESP32-DIV/     Firmware source code
lib/           Pinned application libraries
Libraries/     Board-specific display configuration
vendor/        Pinned board-specific display and CC1101 libraries
build-tools/   Reproducible build and complete-image generation scripts
dist/          Ready-to-flash complete firmware image
platformio.ini PlatformIO project configuration
LICENSE        Project license

TARGET HARDWARE
---------------

MK SIGNAL ESP32 DIV V2.1 based on ESP32-S3 with 16 MB flash.

CUSTOM EVIL PORTAL HTML
-----------------------

Place .html or .htm files (up to 4 MiB each) in /portals/ on the microSD. Large
pages are streamed in small chunks and do not need to fit in ESP32 RAM.
Select the page from WiFi > Evil Portal > HTML. Forms may submit URL-encoded
form data or JSON by POST to /login or another endpoint. Common identity field
names such as username, user, email, login, account and phone are supported;
password, pass, passwd, pwd and passphrase are accepted for the password. The
optional placeholders {{SSID}} and {{DEVICE_NAME}} are replaced when the portal
starts. JavaScript variables must still be transmitted with a form, fetch or
XHR request for the firmware to receive them.

BUILD REQUIREMENTS
------------------

- Windows 10 or Windows 11
- Python 3
- PlatformIO Core or the PlatformIO extension for Visual Studio Code
- Internet access on the first build to obtain the Espressif platform

BUILD THE COMPLETE IMAGE
------------------------

Open PowerShell in this directory and run:

  powershell -ExecutionPolicy Bypass -File .\build-tools\build-full.ps1

The complete image will be created at:

dist\ESP32-DIV-1.7.2-stable-by_MK_SIGNAL_v1.90-DIV-V2.1-full.bin

FLASH THE COMPLETE IMAGE
------------------------

The following commands perform a clean installation on COM4. Change COM4 if
the device uses a different serial port. Erasing flash removes settings stored
in internal flash but does not erase the microSD card.

  & "$env:USERPROFILE\.platformio\penv\Scripts\python.exe" "$env:USERPROFILE\.platformio\packages\tool-esptoolpy\esptool.py" --chip esp32s3 --port COM4 --baud 460800 erase_flash

  & "$env:USERPROFILE\.platformio\penv\Scripts\python.exe" "$env:USERPROFILE\.platformio\packages\tool-esptoolpy\esptool.py" --chip esp32s3 --port COM4 --baud 921600 write_flash 0x0 ".\dist\ESP32-DIV-1.7.2-stable-by_MK_SIGNAL_v1.90-DIV-V2.1-full.bin"

SUPPLIED IMAGE CHECKSUM
-----------------------

SHA-256:
AE311B55E29D128E98FC8737CCBFC993C446E2C100CCD718F9FC069CC51BBD4E

Verify it with:

Get-FileHash -Algorithm SHA256 .\dist\ESP32-DIV-1.7.2-stable-by_MK_SIGNAL_v1.90-DIV-V2.1-full.bin
