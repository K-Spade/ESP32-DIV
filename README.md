<div align="center">
  <img src="https://github.com/user-attachments/assets/1f70e8ba-d8be-4889-959a-700294068a3e" alt="MK SIGNAL DIV Banner" width="100%" />
  <br />
  <br />
  <p align="center">
    <a href="https://github.com/K‑Spade/ESP32‑DIV"><img src="https://img.shields.io/static/v1?label=MK&message=SIGNAL‑DIV&color=orange&logo=github" /></a>
    <a href="https://github.com/K‑Spade/ESP32‑DIV"><img src="https://img.shields.io/github/stars/K‑Spade/ESP32‑DIV?style=social" /></a>
    <a href="https://github.com/K‑Spade/ESP32‑DIV"><img src="https://img.shields.io/github/forks/K‑Spade/ESP32‑DIV?style=social" /></a>
    <img src="https://img.shields.io/github/downloads/K‑Spade/esp32‑div/total?color=orange&label=downloads&logo=github" />
    <img src="https://img.shields.io/badge/ESP32‑S3‑Offensive%20%2B%20Defensive-orange?logo=espressif" />
    <img src="https://img.shields.io/badge/license‑MIT‑orange" />
    <img src="https://img.shields.io/badge/firmware‑V1.7.2‑green" />
  </p>
  <p align="center">
    <a href="https://github.com/K‑Spade/ESP32‑DIV/releases"><img src="https://img.shields.io/badge/Releases‑Download‑orange?logo=github" /></a>
  </p>
</div>
&nbsp;

## 📖 Wiki
> This repository is modified fork based on original [cifertech/ESP32‑DIV](https://github.com/cifertech/ESP32‑DIV).

> ⚡ **Skip the IDE** — Flash firmware binary from Releases.

<div>&nbsp;</div>

<!-- About the Project -->
## :star2: About the Project
**MK SIGNAL DIV V2.1** is an enhanced open‑source multi‑band wireless toolkit built on **ESP32‑S3**.
It covers Wi‑Fi, BLE, 2.4GHz(NRF24), Sub‑GHz, IR, RFID/NFC, GPS, with cyber‑mecha redesigned touch‑screen UI.
Hand‑held all‑in‑one platform for wireless analysis, signal test and RF research.

> [!WARNING]
> This project is intended for **educational and research purposes only**. Use only on networks and devices you own or have explicit written permission to test. Unauthorized use may violate local laws and regulations.

## :sparkles: Version Changes — V1.7.2 by MK
> This is complete full‑feature release based on MK SIGNAL DIV V2.1 hardware, integrated NFC + GPS.

### Bug Fixes
- Fixed original known NRF interference failure issue
- Fixed occasional SD‑card invalid / malfunction problem
- Fixed IR remote failure when controlling air‑conditioners and other IR appliances
- Fixed NRF‑SCAN function failure bug

### New Features & Improvements
- UI fully reset & redesigned to **cyber‑mecha style theme**
- Multiple built‑in mini‑games added
- Expanded partial Bluetooth feature set
- New device boot PIN‑code protection function

<!-- Features -->
## :dart: Features
<details>
<summary><strong>📡 Wi‑Fi</strong></summary>
  
| Tool | Description |
|------|-------------|
| Packet Monitor | Real‑time waterfall graph across all 14 channels; optional PCAP logging to SD |
| Wi‑Fi Scanner | Lists nearby networks with extended details |
| Beacon Spammer | Broadcasts fake SSIDs (custom or random) |
| Deauth Attack | Sends deauthentication frames to disrupt client connections |
| Deauth Detector | Monitors for incoming deauth attacks |
| Captive Portal | AP + DNS + web server; clone networks and force sign‑in pages |
| Probe Flood | Floods probe requests to stress‑test APs |
| Hidden SSID Revealer | Forces hidden networks to expose their SSID |
| WPS Scanner | Detects access points with WPS enabled |
| ARP Scanner | Maps all devices on a network with IP and MAC after joining |
| Karma Attack | Listens for probe requests and impersonates saved networks to auto‑connect devices |
</details>

<details>
<summary><strong>🔵 Bluetooth</strong></summary>
  
| Tool | Description |
|------|-------------|
| BLE Scanner | Discovers hidden and visible BLE devices |
| BLE Sniffer | Tracks MAC, RSSI, packet count, and last‑seen time |
| BLE Spoofer | Broadcasts fake BLE advertisements |
| Sour Apple | Spoof Apple BLE advertisements (e.g., AirDrop popups) |
| BLE Jammer | Disrupts BLE and classic Bluetooth channels |
| BLE Rubber Ducky | Acts as a BLE keyboard; executes scripts from `/ducky` on SD |
| AirTag Spoofer | Broadcasts fake AirTag signals into the Find My network |
| AirTag Sniffer | Monitors for AirTags in range |
| Skimmer Detect | Scans for BLE signatures matching known card skimmer profiles |
</details>

<details>
<summary><strong>📶 2.4GHz / NRF24</strong></summary>
  
| Tool | Description |
|------|-------------|
| 2.4GHz Scanner | Spectrum analyzer across 128 channels (Zigbee, custom RF, etc.) |
| Protokill | Disrupts Zigbee, Wi‑Fi, and other 2.4GHz protocols |
| ESB Sniffer | Passively captures Enhanced ShockBurst NRF24 packets |
| ESB Replay | Replays captured ESB packets |
| MouseJack Scan | Detects vulnerable wireless mice and keyboards |
| MouseJack Inject | Injects keystrokes into vulnerable wireless receivers |
</details>

<details>
<summary><strong>📻 Sub‑GHz</strong></summary>
  
| Tool | Description |
|------|-------------|
| Replay Attack | Captures and replays Sub‑GHz commands (e.g., garage doors, remotes) |
| Sub‑GHz Jammer | Disrupts Sub‑GHz communication across various bands |
| Saved Profiles | Stores and manages captured signal profiles |
| De Bruijn / Brute Force | Cycles through all possible fixed codes for Sub‑GHz remotes |
| Jamming Detector | Receive‑only monitor that detects Sub‑GHz jamming attacks (e.g. car‑fob jamming) |
</details>

<details>
<summary><strong>📺 Infrared (IR)</strong></summary>
  
| Tool | Description |
|------|-------------|
| IR Replay Attack | Captures real IR presses, visualizes, replays, and saves to SD |
| IR Saved Profiles | Browses IR captures; preserves signal and carrier frequency |
| Universal IR Controller | Built‑in profiles, SD imports, favorites, and remote‑style control |
</details>

<details>
<summary><strong>🧲 RFID / NFC</strong></summary>
  
| Tool | Description |
|------|-------------|
| Card Reader | Reads UID and tag identification |
| Card Clone | Copies supported writable tags |
| Dump | Reads sectors/blocks when keys are available |
| Decode Access | Interprets access bits and ACL‑style fields from dumps |
| Erase | Wipes supported writable tags |
| Jam Reader | Impedes another reader with RF patterns |
| Tag Disrupt | Advanced disruption flows for authorized physical tests |
| Disrupt Emulate | Disruption combined with emulation‑style flows |
</details>

<details>
<summary><strong>🛰️ GPS</strong></summary>
  
| Tool | Description |
|------|-------------|
| Wardriver | Logs GNSS position with Wi‑Fi/BLE observations to SD |
| Satellite Scanner | Shows satellites in view, signal strength, and fix diagnostics |
</details>

<details>
<summary><strong>🧰 Device & System</strong></summary>
  
| Tool | Description |
|------|-------------|
| Serial Monitor | Mirrors serial traffic on the TFT for field debugging |
| SD File Manager | Browses and manages files on the SD card |
| Update Firmware | Flashes new firmware from SD |
| Touch Calibrate | Four‑corner XPT2046 touchscreen calibration |
| Settings | Brightness, cyber‑mecha / dark theme, NeoPixel, boot PIN setting, background auto‑scan |
| Built‑in Games | Multiple mini‑games for entertainment |
</details>

<div>&nbsp;</div>

<!-- Hardware Overview -->
## 🔧 Hardware Overview
MK SIGNAL DIV V2.1 consists of main‑board and RF shield:
### 🧠 Main Board
- **ESP32‑S3** – Main microcontroller with Wi‑Fi and BLE
- **ILI9341 TFT Display** – 2.8" touch UI display
- **AMS1117** – 3.3V regulator
- **IP5306** – Lithium battery charging and protection
- **CP2102** – USB‑to‑serial for flashing
- **PCF8574** – I/O expander for buttons
- **SD Card Slot** – Stores logs and captured signals
- **Push Buttons** – Navigation and interaction
- **Antenna Connector** – External antenna support
- **WS2812 NeoPixels**‑ Status feedback
- **Buzzer**‑ Optional buzzer alert

### 🛡️ RF Shield
- **3x NRF24 Modules** – 2.4GHz jamming and spoofing
- **1x CC1101 Module** – Sub‑GHz jamming and replay
- **Multiple antennas**‑ Extended RF range
- **IR Transceiver**‑ Capture & replay IR remotes
- **NFC module**‑ RFID/NFC function
- **GPS module**‑ GNSS positioning

<div>&nbsp;</div>

<!-- License -->
## :warning: License
 
Distributed under the MIT License. See LICENSE.txt for more information.
> This project is modified from original cifertech/ESP32‑DIV, MIT license inherited.

<!-- Support & Contributions -->
## Support & Contributions
- 💬 Found a bug or have a feature request? Open an [Issue](https://github.com/K‑Spade/ESP32‑DIV/issues)
- ⭐ Like this modified firmware? Star the repo!
- 🛠 Want to contribute? Fork it and submit pull requests.

<!-- Contact -->
## Contact
Project Repo: [https://github.com/K‑Spade/ESP32‑DIV](https://github.com/K‑Spade/ESP32‑DIV)
