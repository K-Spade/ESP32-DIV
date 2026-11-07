MK SIGNAL DIV V2.1 - V1.81 Stable
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

  dist\ESP32-DIV-1.7.2-stable-by_MK_SIGNAL_v1.81-DIV-V2.1-full.bin

FLASH THE COMPLETE IMAGE
------------------------

The following commands perform a clean installation on COM4. Change COM4 if
the device uses a different serial port. Erasing flash removes settings stored
in internal flash but does not erase the microSD card.

  & "$env:USERPROFILE\.platformio\penv\Scripts\python.exe" "$env:USERPROFILE\.platformio\packages\tool-esptoolpy\esptool.py" --chip esp32s3 --port COM4 --baud 460800 erase_flash

  & "$env:USERPROFILE\.platformio\penv\Scripts\python.exe" "$env:USERPROFILE\.platformio\packages\tool-esptoolpy\esptool.py" --chip esp32s3 --port COM4 --baud 921600 write_flash 0x0 ".\dist\ESP32-DIV-1.7.2-stable-by_MK_SIGNAL_v1.81-DIV-V2.1-full.bin"

SUPPLIED IMAGE CHECKSUM
-----------------------

SHA-256:
1A8CA5FA6071A03AB7CB5A3F7133BB77429FB02083D0B93A29C5A41028FCB24E

Verify it with:

  Get-FileHash -Algorithm SHA256 .\dist\ESP32-DIV-1.7.2-stable-by_MK_SIGNAL_v1.81-DIV-V2.1-full.bin
