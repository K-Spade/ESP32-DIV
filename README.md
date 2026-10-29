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
- Multiple built‑in mini‑games added (new **More → Games** menu, 10 titles)
- Expanded Bluetooth feature set (**BLE Spam All**, **Apple Juice**, **FindMy Attacks**)
- New device boot **PIN‑code protection** (**Tools → PIN Device Lock**)
- New **NRF Tuning** menu: choose 2‑radio or ALL‑radio operation for 2.4 GHz tools
- Settings gained **Sound on/off**, an **8‑preset accent colour picker** (MK Green default) and **8 UI languages**
- Wi‑Fi, Bluetooth and More menus are now **paged**, so every tool fits on screen

<!-- Features -->
## :dart: Features

> 📌 **What's new in this fork?** Every tool below that carries a 🆕 badge is **absent from the
> original cifertech/ESP32‑DIV 1.7.2** and appears on the menu only after this build is flashed.
> Items marked ✏️ changed behaviour or hardware support compared with the original.

<details open>
<summary><strong>🆕 Menu Changes at a Glance — original 1.7.2 → this build</strong></summary>

**Bluetooth menu (new features are at the top, where you reach them first)**

| Menu position | Original 1.7.2 | This build | What you get |
|---|---|---|---|
| 1 | BLE Jammer | BLE Jammer | unchanged |
| 2 | BLE Spoofer | BLE Spoofer | unchanged |
| 3 | — | 🆕 **BLE Spam All** | One‑tap rotating spam of 10 vendor payload families (Google Fast Pair, Samsung, Galaxy Buds 2 Pro, Microsoft, Apple action/device/setup, Flipper…) |
| 4 | — | 🆕 **Apple Juice** | Endless randomized Apple proximity pop‑ups (the classic "Apple Juice" nuisance, no SD card needed) |
| 5 | Sour Apple | Sour Apple | unchanged |
| 6 | AirTag Spoofer | AirTag Spoofer | unchanged |
| 7 | AirTag Sniffer | AirTag Sniffer | unchanged |
| 8 | — | 🆕 **FindMy Attacks** | Finds AirTag / FMNA / DULT trackers, then tests their GATT service or triggers the ringing routine |
| 13 (page 2) | — | 🆕 **Skimmer Detect** | moved here after the new entries; still scans for card‑skimmer BLE signatures |

**More menu (IR / RFID / GPS)**

| Menu position | Original 1.7.2 | This build | What you get |
|---|---|---|---|
| 4 | — | 🆕 **Games** | 10 built‑in games: Snake, Pong, Tetris, Breakout, Space Invaders, Missile Command, Connect Four, Blackjack, Five Card Draw, 8 Ball Pool |

**Tools menu**

| Menu position | Original 1.7.2 | This build | What you get |
|---|---|---|---|
| 2 | Update Firmware | — (removed from this menu) | Firmware flashing now lives in the Wi‑Fi → **Update Firmware** screen so the SD/SPI bus is free |
| 4 | — | 🆕 **NRF Tuning** | Choose whether 2.4 GHz features use **2 radios** (NRF1+NRF2, NRF3 electrically idle) or **ALL** detected radios |
| 5 | — | 🆕 **PIN Device Lock** | Sets / changes / removes a 4–8 digit boot PIN for the whole device |

**Settings screen**

| Setting | Original 1.7.2 | This build |
|---|---|---|
| Brightness | ✅ | ✅ |
| Theme | Dark / Light | Dark / Light |
| Accent | 7 presets, default Orange | 🆕 **8 presets, factory default MK Green**, hardware‑previewed swatches |
| NeoPixel | ✅ | ✅ |
| Sound | — | 🆕 **Sound on/off** (buzzer + feedback tones) |
| Auto Scan | on by default (Wi‑Fi + BLE) | 🆕 **off by default and merged into one switch** — saves battery, avoids SD/UART contention |
| Language | — | 🆕 **English, 中文, Español, Русский, Português, 日本語, ไทย, 한국어** |

**Sub ‑GHz menu** — same five tools, but 🆕 **Jamming Detector** is now a full receive‑only
analysis screen (target band, RSSI trend, event log) instead of a bare alarm, and
**Saved Profile** / **De Bruijn / Brute** gained on‑screen rename, delete and band selection.

**Hardware/bug‑fix bundle that changes what the menus can do**

- ✏️ **IR** now works on air‑conditioners and other long‑frame protocols (timing‑preserving capture & replay).
- ✏️ **2.4 GHz / NRF** startup no longer fails on interference, and the **Scanner** no longer refuses to start (both on the original's known‑bug list).
- ✏️ **SD card** is detected reliably, so every `Saved Profile` / PCAP / Wardriver / Ducky flow can actually write.
- ✏️ All Wi‑Fi/BLE menus are **paged** (8 + rest), with a footer page button, so no tool is hidden off‑screen.

</details>

<details>
<summary><strong>📡 Wi‑Fi</strong></summary>

| Tool | Description |
|------|-------------|
| Packet Monitor | Real‑time waterfall graph across all 14 channels; optional PCAP logging to SD |
| Beacon Spammer | Broadcasts fake SSIDs (custom or random) |
| WiFi Deauther | Sends deauthentication frames to disrupt client connections |
| Probe Request Flood | Floods probe requests to stress‑test APs |
| Deauth Detector | Monitors for incoming deauth attacks |
| WiFi Scanner | Lists nearby networks with extended details |
| Captive Portal | AP + DNS + web server; clone networks and force sign‑in pages |
| Hidden SSID Revealer | Forces hidden networks to expose their SSID |
| WPS Scanner | Detects access points with WPS enabled |
| ARP Scanner | Maps all devices on a network with IP and MAC after joining |
| Karma Attack | Listens for probe requests and impersonates saved networks to auto‑connect devices |

</details>

<details>
<summary><strong>🔵 Bluetooth</strong></summary>

| Tool | Description |
|------|-------------|
| BLE Jammer | Disrupts BLE and classic Bluetooth channels |
| BLE Spoofer | Broadcasts fake BLE advertisements with selectable Apple / device profiles |
| 🆕 BLE Spam All | Continuously cycles a 10‑family payload catalogue — Google Fast Pair, Samsung, Galaxy Buds 2 Pro, Microsoft / Custom Windows, Apple Action, Apple Device, Apple Setup and Flipper — with live payload name, burst and failure counters |
| 🆕 Apple Juice | Keeps firing randomized Apple proximity / action packets back‑to‑back to trigger connection pop‑ups (no SD card required) |
| Sour Apple | Spoof Apple BLE advertisements (e.g., AirDrop popups) |
| AirTag Spoofer | Broadcasts fake AirTag signals into the Find My network |
| AirTag Sniffer | Monitors for AirTags in range; select a hit to inspect it |
| 🆕 FindMy Attacks | Scans for AirTag, FMNA and DULT tracker advertisements, lists address / RSSI / hit count, then runs either a **connection + GATT service test** or the tracker’s **ring/sound routine** |
| Sniffer | Tracks MAC, RSSI, packet count, and last‑seen time |
| BLE Scanner | Discovers hidden and visible BLE devices |
| BLE Rubber Ducky | Acts as a BLE keyboard; executes scripts from `/ducky` on SD |
| Skimmer Detect | Scans for BLE signatures matching known card skimmer profiles |

</details>

<details>
<summary><strong>📶 2.4GHz / NRF24</strong></summary>

| Tool | Description |
|------|-------------|
| Scanner | Spectrum analyzer across 128 channels (Zigbee, custom RF, etc.); hardened start path so it no longer drops out |
| Proto Kill | Disrupts Zigbee, Wi‑Fi, and other 2.4GHz protocols |
| ESB Sniffer | Passively captures Enhanced ShockBurst NRF24 packets |
| ESB Replay | Replays captured ESB packets |
| MouseJack Scan | Detects vulnerable wireless mice and keyboards |
| MouseJack Inject | Injects keystrokes into vulnerable wireless receivers |
| 🆕 NRF Tuning (in Tools) | Switches 2.4 GHz tools between **2 radios** (keeps NRF3 idle) and **ALL** detected radios; the setting is stored in internal flash, never on the SD card |

> 🆕 All 2.4 GHz tools now pass through a radio supervisor that recovers from interference and
> only talks to the radios that were actually detected.

</details>

<details>
<summary><strong>📻 Sub‑GHz</strong></summary>

| Tool | Description |
|------|-------------|
| Replay Attack | Captures and replays Sub‑GHz commands (e.g., garage doors, remotes) |
| SubGHz Jammer | Disrupts Sub‑GHz communication across various bands |
| De Bruijn / Brute | Cycles through all possible fixed codes for Sub‑GHz remotes ✏️ with band selection and on‑screen profile naming |
| 🆕 Jamming Detector (upgraded) | Receive‑only monitor that detects Sub‑GHz jamming (e.g. car‑fob jamming) — now shows band, RSSI trend and an event log instead of only an alert |
| Saved Profile | Stores and manages captured signal profiles ✏️ rename / delete from the device |

</details>

<details>
<summary><strong>📺 Infrared (IR)</strong></summary>

| Tool | Description |
|------|-------------|
| Record (IR Replay) | Captures real IR presses, visualizes, replays, and saves to SD ✏️ fixed timing capture so **air‑conditioner and other long‑frame remotes now replay correctly** |
| Saved Profile | Browses IR captures; preserves signal and carrier frequency |
| Universal Controller | Built‑in profiles, SD imports, favorites, and remote‑style control |

</details>

<details>
<summary><strong>🧲 RFID / NFC</strong></summary>

| Tool | Description |
|------|-------------|
| Card Reader | Reads UID and tag identification |
| Card Clone | Copies supported writable tags |
| Erase | Wipes supported writable tags |
| Dump | Reads sectors/blocks when keys are available |
| Decode Access | Interprets access bits and ACL‑style fields from dumps |
| Jam Reader | Impedes another reader with RF patterns |
| Tag Disrupt | Advanced disruption flows for authorized physical tests |
| Disrupt Emulate | Disruption combined with emulation‑style flows |

</details>

<details>
<summary><strong>🛰️ GPS</strong></summary>

| Tool | Description |
|------|-------------|
| Wardriver | Logs GNSS position with Wi‑Fi/BLE observations to SD (WiGLE‑compatible upload file included) |
| Satellite Scanner | Shows satellites in view, signal strength, and fix diagnostics |

</details>

<details>
<summary><strong>🎮 Games — 🆕 new menu</strong></summary>

Reachable from **More → Games**. All games run full‑screen on the touch panel with the five
on‑screen buttons; TFT status bar and battery reading stay visible.

| Game | Description |
|------|-------------|
| Snake | Classic grow‑the‑tail, on‑screen direction pad |
| Pong | Two‑paddle rally against the CPU |
| Tetris | Falling blocks with rotate / drop controls |
| Breakout | Brick breaker with paddle control |
| Space Invaders | Wave shooter with hold‑to‑fire |
| Missile Command | Aim and intercept incoming missiles |
| Connect Four | Drop discs against the CPU |
| Blackjack | Hit / stand card game vs. dealer |
| Five Card Draw | Poker draw game with hold / draw |
| 8 Ball Pool | Aim‑and‑power billiards |

</details>

<details>
<summary><strong>🧰 Device & System</strong></summary>

| Tool | Description |
|------|-------------|
| Serial Monitor | Mirrors serial traffic on the TFT for field debugging |
| SD File Manager | Browses and manages files on the SD card |
| Update Firmware | Flashes new firmware from SD — ✏️ moved to the Wi‑Fi submenu in this build so the download/flash path does not fight with the shared SD/SPI bus |
| Touch Calibrate | Four‑corner XPT2046 touchscreen calibration |
| 🆕 NRF Tuning | Pick 2‑radio or ALL‑radio mode for 2.4 GHz tools; saved to internal flash |
| 🆕 PIN Device Lock | Set, change or remove a 4–8 digit boot PIN. The keypad appears **before** the SD card and menus are initialised, and the PIN is stored as a salted SHA‑256 digest, not as plain text |
| Settings | Brightness, Dark/Light theme, 8 accent colours (MK Green by default), NeoPixel, sound on/off, combined Auto Scan switch (off by default), and 8 UI languages |

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
