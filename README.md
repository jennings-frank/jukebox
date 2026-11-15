# ESP32-C3 SuperMini Smooth Buzzer Jukebox

A self-contained, library-free polyphonic-feel music player for the **ESP32-C3 SuperMini** featuring 50 built-in public-domain classics, PWM envelope shaping with vibrato, and a real-time 0.96" SSD1306/SH1106 OLED display.

## Overview

Most microcontroller buzzer sketches use blocking `tone()` calls that sound harsh and prevent responsive button controls. This project uses the ESP32 LEDC hardware PWM peripheral to shape note attacks and releases, apply subtle vibrato on sustained notes, and render a live, single-screen OLED dashboard without relying on any third-party display libraries.

## Features

- **50 Built-in Melodies:** Classical works, folk tunes, ragtime, opera motifs, hymns, and nursery rhymes—each paired with curated metadata (composer/origin, era, and historical trivia).
- **Smooth Audio Engine:**
  - Configurable attack (`18 ms`) and release (`45 ms`) duty-cycle envelopes to eliminate harsh clicks.
  - Gentle `~5.5 Hz` pitch vibrato on sustained notes.
  - 5-level software volume control (`12` to `128` 8-bit PWM duty).
  - Automatic two-pass playback with relaxed tempo scaling (`125%` note length) and sustained final cadence notes.
- **Zero-Library OLED Driver & Unified UI:**
  - Built-in I2C page-buffered driver for `128x64` SSD1306 (and SH1106 via column offset) displays using `Wire` only.
  - Page-level dirty flushing minimizes I2C overhead so audio timing stays rock-solid.
  - Single-screen layout displaying track index, shuffle/pause status, genre tag, song title, live progress bar, effective BPM, elapsed/total time, active pitch (`Note + Hz`), and a continuous bottom scrolling metadata marquee.
- **Responsive Controls & NVS Persistence:**
  - Non-blocking debounced short-press and long-press detection.
  - Remembers the last played track across reboots using ESP32 `Preferences` (NVS flash) and boots silently in a paused state until you press Play.

## Hardware & Wiring

### Components
- **Microcontroller:** ESP32-C3 SuperMini
- **Audio:** 1x Passive Buzzer (do not use an active buzzer)
- **Display:** 1x 0.96" `128x64` I2C OLED (`SSD1306`, address `0x3C`)
- **Controls:** 3x Tactile Push Buttons

### Pinout

| Component | Pin / Terminal | ESP32-C3 GPIO | Notes |
| :--- | :--- | :--- | :--- |
| **Passive Buzzer** | `+` (Signal) | `GPIO 10` | Driven via 8-bit LEDC PWM |
| **Passive Buzzer** | `-` (GND) | `GND` | |
| **OLED Display** | `SDA` | `GPIO 8` | I2C Data (`400 kHz`) |
| **OLED Display** | `SCL` | `GPIO 9` | I2C Clock |
| **OLED Display** | `VCC` / `GND` | `3.3V` / `GND` | |
| **Pause / Resume** | Button Pin 1 | `GPIO 3` | Internal `INPUT_PULLUP` enabled |
| **Next / Volume** | Button Pin 1 | `GPIO 4` | Internal `INPUT_PULLUP` enabled |
| **Prev / Shuffle** | Button Pin 1 | `GPIO 7` | Internal `INPUT_PULLUP` enabled |
| **All Buttons** | Button Pin 2 | `GND` | Active-low wiring |

## Controls

| Button | Action | Function |
| :--- | :--- | :--- |
| **Pause (`GPIO 3`)** | Short Press | Pause or resume playback (starts paused on boot) |
| **Next (`GPIO 4`)** | Short Press | Skip to the next song |
| **Next (`GPIO 4`)** | Long Press (`700 ms`) | Cycle through 5 volume levels (plays rising confirmation tone) |
| **Prev (`GPIO 7`)** | Short Press | Restart current song if `> 3 s` in; otherwise go to previous song |
| **Prev (`GPIO 7`)** | Long Press (`700 ms`) | Toggle Shuffle mode ON/OFF (2-tone audio indicator) |

## Getting Started

### Prerequisites
- **Arduino IDE** (2.x recommended) or **ArduinoDroid**
- **ESP32 Board Package:** `esp32` by Espressif Systems **v3.0.0 or newer** (required for the v3.x `ledcAttach` / `ledcChangeFrequency` API)
- **External Libraries:** None required (`Wire.h`, `Preferences.h`, and `esp_random.h` are included in the ESP32 core)

### Installation & Upload
1. Wire the passive buzzer, OLED display, and three push buttons according to the pinout table above.
2. Open the `.ino` sketch in the Arduino IDE.
3. Under **Tools > Board**, select **ESP32C3 Dev Module** (or **Nologo ESP32C3 Super Mini**).
4. Set **USB CDC On Boot** to **Enabled** if you want to view track and button logs in the Serial Monitor (`115200` baud).
5. Click **Upload**.
6. Once the splash screen clears, press the **Pause/Resume button (`GPIO 3`)** to start playback.

### Optional Configuration
You can customize hardware and playback settings near the top of the sketch:
- **1.3" SH1106 OLED:** Change `OLED_COL_OFFSET` from `0` to `2` if the display image appears shifted horizontally.
- **Alternate I2C Address:** Change `OLED_ADDR` from `0x3C` to `0x3D` if your OLED module uses the secondary address.
- **Tempo & Repeats:** Adjust `NOTE_LENGTH_PERCENT` (default `125`) and `SONG_PLAYTHROUGHS` (default `2`) to change playback speed and loop count per track.
- **Ticker Speed:** Adjust the `40` ms threshold inside `uiUpdateDetailsTicker()` to speed up or slow down the bottom metadata scroll.
