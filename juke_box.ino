/*
 * MIT License
 *
 * Copyright (c) 2026 Frank Jennings
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in all
 * copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

// =======================================================
// ESP32-C3 SuperMini Smooth Buzzer Jukebox  v2
// Passive Buzzer (+) -> GPIO 10 | (-) -> GND
// Prev Song Button   -> GPIO 7  | (-) -> GND
// Next Song Button   -> GPIO 4  | (-) -> GND
// Pause/Resume Button -> GPIO 3  | (-) -> GND
// OLED 0.96" SSD1306 -> SDA GPIO 8 | SCL GPIO 9 | VCC 3.3V | GND
//
// Requires: Arduino-ESP32 core 3.x (ledcAttach / ledcChangeFrequency)
//           NO external libraries - the OLED driver is built in (uses Wire only)
//
// CONTROLS
//   Next  (short press) : next song
//   Prev  (short press) : restart song if >3 s in, otherwise previous song
//   Next  (long press)  : cycle volume (5 levels, rising blip)
//   Prev  (long press)  : toggle shuffle (rising blip = on, falling = off)
//   Pause (short press) : pause / resume the current tune
//   Startup             : restores last song and always waits paused
//   Persistent storage  : ESP32 Preferences (NVS flash)
// =======================================================

#include <esp_random.h>
#include <Preferences.h>
#include <Wire.h>

// ---------- Pins ----------
const int BUZZER_PIN    = 10;
const int PREV_BTN_PIN  = 7;   // Left button
const int NEXT_BTN_PIN  = 4;   // Right button
const int PAUSE_BTN_PIN = 3;   // Pause / Resume button

// ---------- OLED (SSD1306 128x64, I2C) ----------
const int      OLED_SDA_PIN    = 8;
const int      OLED_SCL_PIN    = 9;
const uint8_t  OLED_ADDR       = 0x3C;     // 0x3D on some modules
const uint8_t  OLED_COL_OFFSET = 0;        // set to 2 for SH1106 (1.3") displays
const uint32_t OLED_I2C_HZ     = 400000;

// ---------- PWM ----------
const uint8_t  PWM_BITS      = 8;
const uint32_t PWM_BASE_FREQ = 2000;

// ---------- Tone character ----------
// Duty out of 255 (8-bit). 128 = 50% = loudest for a passive buzzer.
const uint8_t VOLUME_LEVELS[] = { 12, 30, 55, 90, 128 };
const uint8_t VOLUME_STEPS    = sizeof(VOLUME_LEVELS) / sizeof(VOLUME_LEVELS[0]);
uint8_t volumeIndex           = 2;     // start at 55 (your original setting)

const int   ATTACK_MS           = 18;       // fade-in
const int   RELEASE_MS          = 45;       // fade-out
const bool  USE_VIBRATO         = true;
const int   VIBRATO_MIN_NOTE_MS = 250;      // only on long notes
const int   VIBRATO_DELAY_MS    = 120;      // let the note "settle" first
const float VIBRATO_DEPTH       = 0.006f;   // +/-0.6% of the note frequency
const float VIBRATO_RAD_PER_MS  = 0.0345f;  // ~5.5 Hz

// ---------- Buttons / behaviour ----------
const unsigned long DEBOUNCE_MS       = 30;
const unsigned long LONG_PRESS_MS     = 700;
const unsigned long SONG_GAP_MS       = 1500;  // pause between songs
const unsigned long RESTART_WINDOW_MS = 3000;  // Prev restarts if song is older than this

// ---------- Musical playback enhancements ----------
// Play every arrangement twice, at a slightly more relaxed tempo.
// 125% note length is approximately 20% slower in perceived tempo.
const uint8_t  NOTE_LENGTH_PERCENT  = 125;
const uint8_t  SONG_PLAYTHROUGHS    = 2;
const uint8_t  CADENCE_HOLD_PERCENT = 125; // sustain the final pitched note of each pass
const uint16_t REPEAT_BREAK_MS      = 500; // short breath between complete passes
const uint16_t ENDING_SILENCE_MS    = 350; // let the final note ring out before the next track

// ---------- Note frequencies (Hz) ----------
#define REST     0
#define NOTE_E3  165
#define NOTE_F3  175
#define NOTE_G3  196
#define NOTE_GS3 208
#define NOTE_A3  220
#define NOTE_AS3 233
#define NOTE_B3  247
#define NOTE_C4  262
#define NOTE_CS4 277
#define NOTE_D4  294
#define NOTE_DS4 311
#define NOTE_E4  330
#define NOTE_F4  349
#define NOTE_FS4 370
#define NOTE_G4  392
#define NOTE_GS4 415
#define NOTE_A4  440
#define NOTE_AS4 466
#define NOTE_B4  494
#define NOTE_C5  523
#define NOTE_CS5 554
#define NOTE_D5  587
#define NOTE_DS5 622
#define NOTE_E5  659
#define NOTE_F5  698
#define NOTE_FS5 740
#define NOTE_G5  784
#define NOTE_A5  880
#define NOTE_B5  988
#define NOTE_C6  1047

// ---------- Song data ----------
// Each note is {frequency, duration}. Duration: 1=whole, 2=half, 4=quarter,
// 8=eighth, 16=sixteenth. Negative = dotted (e.g. -4 = dotted quarter).
// Keeping pitch and length together means they can never fall out of sync.
struct Note { uint16_t f; int8_t d; };

// 1. Ode to Joy (Beethoven)
const Note odeToJoy[] = {
  {NOTE_E4,4},{NOTE_E4,4},{NOTE_F4,4},{NOTE_G4,4},
  {NOTE_G4,4},{NOTE_F4,4},{NOTE_E4,4},{NOTE_D4,4},
  {NOTE_C4,4},{NOTE_C4,4},{NOTE_D4,4},{NOTE_E4,4},
  {NOTE_E4,-4},{NOTE_D4,8},{NOTE_D4,2},
  {NOTE_E4,4},{NOTE_E4,4},{NOTE_F4,4},{NOTE_G4,4},
  {NOTE_G4,4},{NOTE_F4,4},{NOTE_E4,4},{NOTE_D4,4},
  {NOTE_C4,4},{NOTE_C4,4},{NOTE_D4,4},{NOTE_E4,4},
  {NOTE_D4,-4},{NOTE_C4,8},{NOTE_C4,2}
};

// 2. Minuet in G Major (Petzold, long attributed to Bach)
const Note minuet[] = {
  {NOTE_D5,4},{NOTE_G4,8},{NOTE_A4,8},{NOTE_B4,8},{NOTE_C5,8},
  {NOTE_D5,4},{NOTE_G4,4},{NOTE_G4,4},
  {NOTE_E5,4},{NOTE_C5,8},{NOTE_D5,8},{NOTE_E5,8},{NOTE_FS5,8},
  {NOTE_G5,4},{NOTE_G4,4},{NOTE_G4,4},
  {NOTE_C5,4},{NOTE_D5,8},{NOTE_C5,8},{NOTE_B4,8},{NOTE_A4,8},
  {NOTE_B4,4},{NOTE_C5,8},{NOTE_B4,8},{NOTE_A4,8},{NOTE_G4,8},
  {NOTE_FS4,4},{NOTE_G4,8},{NOTE_A4,8},{NOTE_B4,8},{NOTE_G4,8},
  {NOTE_A4,-2}
};

// 3. Twinkle Twinkle Little Star
const Note twinkle[] = {
  {NOTE_C4,4},{NOTE_C4,4},{NOTE_G4,4},{NOTE_G4,4},{NOTE_A4,4},{NOTE_A4,4},{NOTE_G4,2},
  {NOTE_F4,4},{NOTE_F4,4},{NOTE_E4,4},{NOTE_E4,4},{NOTE_D4,4},{NOTE_D4,4},{NOTE_C4,2},
  {NOTE_G4,4},{NOTE_G4,4},{NOTE_F4,4},{NOTE_F4,4},{NOTE_E4,4},{NOTE_E4,4},{NOTE_D4,2},
  {NOTE_G4,4},{NOTE_G4,4},{NOTE_F4,4},{NOTE_F4,4},{NOTE_E4,4},{NOTE_E4,4},{NOTE_D4,2},
  {NOTE_C4,4},{NOTE_C4,4},{NOTE_G4,4},{NOTE_G4,4},{NOTE_A4,4},{NOTE_A4,4},{NOTE_G4,2},
  {NOTE_F4,4},{NOTE_F4,4},{NOTE_E4,4},{NOTE_E4,4},{NOTE_D4,4},{NOTE_D4,4},{NOTE_C4,2}
};

// 4. Fur Elise (Beethoven) - rhythm rewritten in 3/8 so the rests land correctly
const Note furElise[] = {
  {NOTE_E5,16},{NOTE_DS5,16},{NOTE_E5,16},{NOTE_DS5,16},{NOTE_E5,16},{NOTE_B4,16},{NOTE_D5,16},{NOTE_C5,16},
  {NOTE_A4,8},{REST,16},{NOTE_C4,16},{NOTE_E4,16},{NOTE_A4,16},
  {NOTE_B4,8},{REST,16},{NOTE_E4,16},{NOTE_GS4,16},{NOTE_B4,16},
  {NOTE_C5,8},{REST,16},{NOTE_E4,16},{NOTE_E5,16},{NOTE_DS5,16},
  {NOTE_E5,16},{NOTE_DS5,16},{NOTE_E5,16},{NOTE_B4,16},{NOTE_D5,16},{NOTE_C5,16},
  {NOTE_A4,8},{REST,16},{NOTE_C4,16},{NOTE_E4,16},{NOTE_A4,16},
  {NOTE_B4,8},{REST,16},{NOTE_E4,16},{NOTE_C5,16},{NOTE_B4,16},
  {NOTE_A4,-4}
};

// 5. Greensleeves (Traditional English)
const Note greensleeves[] = {
  {NOTE_A4,4},{NOTE_C5,2},{NOTE_D5,4},{NOTE_E5,-4},{NOTE_F5,8},{NOTE_E5,4},{NOTE_D5,2},{NOTE_B4,4},
  {NOTE_G4,-4},{NOTE_A4,8},{NOTE_B4,4},{NOTE_C5,2},{NOTE_A4,4},
  {NOTE_A4,-4},{NOTE_GS4,8},{NOTE_A4,4},{NOTE_B4,2},{NOTE_GS4,4},{NOTE_E4,-2},
  {NOTE_A4,4},{NOTE_C5,2},{NOTE_D5,4},{NOTE_E5,-4},{NOTE_F5,8},{NOTE_E5,4},{NOTE_D5,2},{NOTE_B4,4},
  {NOTE_G4,-4},{NOTE_A4,8},{NOTE_B4,4},{NOTE_C5,-4},{NOTE_B4,8},{NOTE_A4,4},
  {NOTE_GS4,-4},{NOTE_FS4,8},{NOTE_GS4,4},{NOTE_A4,-2}
};

// 6. Canon in D (Pachelbel)
const Note canon[] = {
  {NOTE_FS5,2},{NOTE_E5,2},{NOTE_D5,2},{NOTE_CS5,2},{NOTE_B4,2},{NOTE_A4,2},{NOTE_B4,2},{NOTE_CS5,2},
  {NOTE_D5,2},{NOTE_CS5,2},{NOTE_B4,2},{NOTE_A4,2},{NOTE_G4,2},{NOTE_FS4,2},{NOTE_G4,2},{NOTE_E4,2},
  {NOTE_D4,4},{NOTE_FS4,4},{NOTE_A4,4},{NOTE_G4,4},{NOTE_FS4,4},{NOTE_D4,4},{NOTE_FS4,4},{NOTE_E4,4},
  {NOTE_D4,4},{NOTE_B3,4},{NOTE_D4,4},{NOTE_A4,4},{NOTE_G4,4},{NOTE_B4,4},{NOTE_A4,4},{NOTE_G4,4},{NOTE_FS4,2}
};

// 7. Brahms' Lullaby
const Note lullaby[] = {
  {NOTE_E4,8},{NOTE_E4,8},{NOTE_G4,2},{NOTE_E4,8},{NOTE_E4,8},{NOTE_G4,2},
  {NOTE_E4,8},{NOTE_G4,8},{NOTE_C5,4},{NOTE_B4,-4},{NOTE_A4,8},{NOTE_A4,4},{NOTE_G4,4},
  {NOTE_D4,8},{NOTE_E4,8},{NOTE_F4,4},{NOTE_D4,4},{NOTE_D4,8},{NOTE_E4,8},{NOTE_F4,4},
  {NOTE_D4,8},{NOTE_F4,8},{NOTE_B4,8},{NOTE_A4,8},{NOTE_G4,4},{NOTE_B4,4},{NOTE_C5,2}
};

// 8. Korobeiniki / "Tetris" theme (Russian folk song, 1861)
const Note korobeiniki[] = {
  {NOTE_E5,4},{NOTE_B4,8},{NOTE_C5,8},{NOTE_D5,4},{NOTE_C5,8},{NOTE_B4,8},
  {NOTE_A4,4},{NOTE_A4,8},{NOTE_C5,8},{NOTE_E5,4},{NOTE_D5,8},{NOTE_C5,8},
  {NOTE_B4,-4},{NOTE_C5,8},{NOTE_D5,4},{NOTE_E5,4},
  {NOTE_C5,4},{NOTE_A4,4},{NOTE_A4,4},{REST,4},
  {NOTE_D5,-4},{NOTE_F5,8},{NOTE_A5,4},{NOTE_G5,8},{NOTE_F5,8},
  {NOTE_E5,-4},{NOTE_C5,8},{NOTE_E5,4},{NOTE_D5,8},{NOTE_C5,8},
  {NOTE_B4,4},{NOTE_B4,8},{NOTE_C5,8},{NOTE_D5,4},{NOTE_E5,4},
  {NOTE_C5,4},{NOTE_A4,4},{NOTE_A4,2}
};

// ---------------- NEW SONGS ----------------

// 9. Jingle Bells (Pierpont, 1857)
const Note jingleBells[] = {
  {NOTE_E5,4},{NOTE_E5,4},{NOTE_E5,2},
  {NOTE_E5,4},{NOTE_E5,4},{NOTE_E5,2},
  {NOTE_E5,4},{NOTE_G5,4},{NOTE_C5,-4},{NOTE_D5,8},{NOTE_E5,1},
  {NOTE_F5,4},{NOTE_F5,4},{NOTE_F5,-4},{NOTE_F5,8},
  {NOTE_F5,4},{NOTE_E5,4},{NOTE_E5,4},{NOTE_E5,8},{NOTE_E5,8},
  {NOTE_E5,4},{NOTE_D5,4},{NOTE_D5,4},{NOTE_E5,4},{NOTE_D5,2},{NOTE_G5,2},
  {NOTE_C5,1}
};

// 10. Silent Night (Gruber, 1818)
const Note silentNight[] = {
  {NOTE_G4,-4},{NOTE_A4,8},{NOTE_G4,4},{NOTE_E4,-2},
  {NOTE_G4,-4},{NOTE_A4,8},{NOTE_G4,4},{NOTE_E4,-2},
  {NOTE_D5,2},{NOTE_D5,4},{NOTE_B4,-2},
  {NOTE_C5,2},{NOTE_C5,4},{NOTE_G4,-2},
  {NOTE_A4,2},{NOTE_A4,4},{NOTE_C5,-4},{NOTE_B4,8},{NOTE_A4,4},
  {NOTE_G4,-4},{NOTE_A4,8},{NOTE_G4,4},{NOTE_E4,-2},
  {NOTE_A4,2},{NOTE_A4,4},{NOTE_C5,-4},{NOTE_B4,8},{NOTE_A4,4},
  {NOTE_G4,-4},{NOTE_A4,8},{NOTE_G4,4},{NOTE_E4,-2},
  {NOTE_D5,2},{NOTE_D5,4},{NOTE_F5,-4},{NOTE_D5,8},{NOTE_B4,4},
  {NOTE_C5,-2},{NOTE_E5,-2},
  {NOTE_C5,4},{NOTE_G4,4},{NOTE_E4,4},
  {NOTE_G4,-4},{NOTE_F4,8},{NOTE_D4,4},{NOTE_C4,-2}
};

// 11. We Wish You a Merry Christmas (Traditional English)
const Note merryChristmas[] = {
  {NOTE_B3,4},
  {NOTE_E4,4},{NOTE_E4,8},{NOTE_FS4,8},{NOTE_E4,8},{NOTE_DS4,8},
  {NOTE_CS4,4},{NOTE_CS4,4},{NOTE_CS4,4},
  {NOTE_FS4,4},{NOTE_FS4,8},{NOTE_GS4,8},{NOTE_FS4,8},{NOTE_E4,8},
  {NOTE_DS4,4},{NOTE_B3,4},{NOTE_B3,4},
  {NOTE_GS4,4},{NOTE_GS4,8},{NOTE_A4,8},{NOTE_GS4,8},{NOTE_FS4,8},
  {NOTE_E4,4},{NOTE_CS4,4},{NOTE_B3,8},{NOTE_B3,8},
  {NOTE_CS4,4},{NOTE_FS4,4},{NOTE_DS4,4},
  {NOTE_E4,2}
};

// 12. Beethoven's 5th - the famous opening motif
const Note beethoven5[] = {
  {REST,8},{NOTE_G4,8},{NOTE_G4,8},{NOTE_G4,8},{NOTE_DS4,1},
  {REST,2},
  {REST,8},{NOTE_F4,8},{NOTE_F4,8},{NOTE_F4,8},{NOTE_D4,1}
};

// 13. In the Hall of the Mountain King (Grieg, 1875)
const Note mountainKing[] = {
  {NOTE_B3,8},{NOTE_CS4,8},{NOTE_D4,8},{NOTE_E4,8},{NOTE_FS4,8},{NOTE_D4,8},{NOTE_FS4,4},
  {NOTE_F4,8},{NOTE_CS4,8},{NOTE_F4,4},
  {NOTE_E4,8},{NOTE_C4,8},{NOTE_E4,4},
  {NOTE_B3,8},{NOTE_CS4,8},{NOTE_D4,8},{NOTE_E4,8},{NOTE_FS4,8},{NOTE_D4,8},{NOTE_FS4,4},
  {NOTE_B4,8},{NOTE_A4,8},{NOTE_FS4,8},{NOTE_D4,8},{NOTE_FS4,8},{NOTE_A4,2}
};

// 14. Happy Birthday (Hill sisters, 1893)
const Note happyBirthday[] = {
  {NOTE_G4,-8},{NOTE_G4,16},{NOTE_A4,4},{NOTE_G4,4},{NOTE_C5,4},{NOTE_B4,-2},
  {NOTE_G4,-8},{NOTE_G4,16},{NOTE_A4,4},{NOTE_G4,4},{NOTE_D5,4},{NOTE_C5,-2},
  {NOTE_G4,-8},{NOTE_G4,16},{NOTE_G5,4},{NOTE_E5,4},{NOTE_C5,4},{NOTE_B4,4},{NOTE_A4,2},
  {NOTE_F5,-8},{NOTE_F5,16},{NOTE_E5,4},{NOTE_C5,4},{NOTE_D5,4},{NOTE_C5,-2}
};

// 15. London Bridge Is Falling Down (Traditional)
const Note londonBridge[] = {
  {NOTE_G4,-4},{NOTE_A4,8},{NOTE_G4,4},{NOTE_F4,4},
  {NOTE_E4,4},{NOTE_F4,4},{NOTE_G4,2},
  {NOTE_D4,4},{NOTE_E4,4},{NOTE_F4,2},
  {NOTE_E4,4},{NOTE_F4,4},{NOTE_G4,2},
  {NOTE_G4,-4},{NOTE_A4,8},{NOTE_G4,4},{NOTE_F4,4},
  {NOTE_E4,4},{NOTE_F4,4},{NOTE_G4,2},
  {NOTE_D4,4},{NOTE_G4,4},{NOTE_E4,4},{NOTE_C4,2}
};

// 16. Row, Row, Row Your Boat (Traditional)
const Note rowBoat[] = {
  {NOTE_C4,4},{NOTE_C4,4},{NOTE_C4,-4},{NOTE_D4,8},{NOTE_E4,4},
  {NOTE_E4,-4},{NOTE_D4,8},{NOTE_E4,-4},{NOTE_F4,8},{NOTE_G4,2},
  {NOTE_C5,8},{NOTE_C5,8},{NOTE_C5,8},{NOTE_G4,8},{NOTE_G4,8},{NOTE_G4,8},
  {NOTE_E4,8},{NOTE_E4,8},{NOTE_E4,8},{NOTE_C4,8},{NOTE_C4,8},{NOTE_C4,8},
  {NOTE_G4,-4},{NOTE_F4,8},{NOTE_E4,-4},{NOTE_D4,8},{NOTE_C4,2}
};

// 17. Turkish March / Rondo Alla Turca (Mozart)
const Note turkishMarch[] = {
  {NOTE_B4,8},{NOTE_A4,8},{NOTE_GS4,8},{NOTE_A4,8},{NOTE_C5,8},{NOTE_D5,8},{NOTE_E5,8},{NOTE_F5,8},
  {NOTE_E5,8},{NOTE_D5,8},{NOTE_C5,8},{NOTE_B4,8},{NOTE_A4,8},{NOTE_GS4,8},{NOTE_A4,4},
  {NOTE_C5,8},{NOTE_D5,8},{NOTE_E5,8},{NOTE_F5,8},{NOTE_G5,8},{NOTE_F5,8},{NOTE_E5,8},{NOTE_D5,8},
  {NOTE_C5,8},{NOTE_B4,8},{NOTE_A4,8},{NOTE_GS4,8},{NOTE_A4,2}
};

// 18. Frere Jacques (traditional round)
const Note frereJacques[] = {
  {NOTE_C4,4},{NOTE_D4,4},{NOTE_E4,4},{NOTE_C4,4},
  {NOTE_C4,4},{NOTE_D4,4},{NOTE_E4,4},{NOTE_C4,4},
  {NOTE_E4,4},{NOTE_F4,4},{NOTE_G4,2},
  {NOTE_E4,4},{NOTE_F4,4},{NOTE_G4,2},
  {NOTE_G4,8},{NOTE_A4,8},{NOTE_G4,8},{NOTE_F4,8},{NOTE_E4,4},{NOTE_C4,4},
  {NOTE_G4,8},{NOTE_A4,8},{NOTE_G4,8},{NOTE_F4,8},{NOTE_E4,4},{NOTE_C4,4},
  {NOTE_C4,4},{NOTE_G3,4},{NOTE_C4,2},
  {NOTE_C4,4},{NOTE_G3,4},{NOTE_C4,2}
};

// 19. Yankee Doodle (traditional American song)
const Note yankeeDoodle[] = {
  {NOTE_C4,4},{NOTE_C4,4},{NOTE_D4,4},{NOTE_E4,4},
  {NOTE_C4,4},{NOTE_E4,4},{NOTE_D4,2},
  {NOTE_C4,4},{NOTE_C4,4},{NOTE_D4,4},{NOTE_E4,4},
  {NOTE_C4,4},{NOTE_B3,4},{NOTE_C4,2},
  {NOTE_C4,4},{NOTE_C4,4},{NOTE_D4,4},{NOTE_E4,4},
  {NOTE_F4,4},{NOTE_E4,4},{NOTE_D4,4},{NOTE_C4,4},
  {NOTE_B3,4},{NOTE_G3,4},{NOTE_A3,4},{NOTE_B3,4},{NOTE_C4,2}
};

// 20. Oh! Susanna (Stephen Foster, 1848)
const Note ohSusanna[] = {
  {NOTE_C4,4},{NOTE_D4,4},{NOTE_E4,4},{NOTE_G4,4},
  {NOTE_G4,4},{NOTE_A4,4},{NOTE_G4,2},
  {NOTE_E4,4},{NOTE_C4,4},{NOTE_D4,4},{NOTE_E4,4},
  {NOTE_E4,4},{NOTE_D4,4},{NOTE_C4,2},
  {NOTE_C4,4},{NOTE_D4,4},{NOTE_E4,4},{NOTE_G4,4},
  {NOTE_G4,4},{NOTE_A4,4},{NOTE_G4,2},
  {NOTE_E4,4},{NOTE_C4,4},{NOTE_D4,4},{NOTE_E4,4},
  {NOTE_D4,4},{NOTE_C4,4},{NOTE_C4,2}
};

// 21. When the Saints Go Marching In (traditional)
const Note whenTheSaints[] = {
  {NOTE_C4,4},{NOTE_E4,4},{NOTE_F4,4},{NOTE_G4,2},
  {NOTE_C4,4},{NOTE_E4,4},{NOTE_F4,4},{NOTE_G4,2},
  {NOTE_C4,4},{NOTE_E4,4},{NOTE_F4,4},{NOTE_G4,4},
  {NOTE_E4,4},{NOTE_C4,4},{NOTE_E4,2},
  {NOTE_D4,4},{NOTE_D4,4},{NOTE_C4,4},{NOTE_D4,4},
  {NOTE_E4,2},{NOTE_F4,2},
  {NOTE_E4,4},{NOTE_C4,4},{NOTE_D4,4},{NOTE_E4,4},
  {NOTE_C4,2},{NOTE_D4,2},{NOTE_C4,1}
};

// 22. Deck the Halls (traditional Welsh melody)
const Note deckTheHalls[] = {
  {NOTE_G4,8},{NOTE_F4,8},{NOTE_E4,8},{NOTE_D4,8},{NOTE_C4,8},{NOTE_D4,8},{NOTE_E4,8},{NOTE_C4,8},
  {NOTE_D4,8},{NOTE_E4,8},{NOTE_F4,8},{NOTE_D4,8},{NOTE_E4,8},{NOTE_F4,8},{NOTE_G4,8},{NOTE_A4,8},
  {NOTE_G4,8},{NOTE_F4,8},{NOTE_E4,8},{NOTE_D4,8},{NOTE_C4,8},{NOTE_D4,8},{NOTE_E4,8},{NOTE_C4,8},
  {NOTE_D4,8},{NOTE_E4,8},{NOTE_F4,8},{NOTE_D4,8},{NOTE_C4,4},{NOTE_D4,4},{NOTE_E4,2}
};

// 23. Joy to the World (Handel)
const Note joyToTheWorld[] = {
  {NOTE_C5,4},{NOTE_B4,8},{NOTE_A4,8},{NOTE_G4,4},{NOTE_F4,4},
  {NOTE_E4,4},{NOTE_D4,4},{NOTE_C4,2},
  {NOTE_G4,4},{NOTE_A4,8},{NOTE_A4,16},{NOTE_B4,16},{NOTE_B4,8},{NOTE_C5,8},{NOTE_C5,8},{NOTE_C5,8},
  {NOTE_C5,4},{NOTE_D5,8},{NOTE_E5,8},{NOTE_E5,4},{NOTE_F5,4},
  {NOTE_F5,8},{NOTE_G5,8},{NOTE_G5,4},{NOTE_A5,4},{NOTE_G5,2}
};

// 24. Scarborough Fair (traditional English ballad - simplified melody)
const Note scarboroughFair[] = {
  {NOTE_A4,4},{NOTE_D5,4},{NOTE_A4,4},{NOTE_A4,4},
  {NOTE_E5,4},{NOTE_D5,4},{NOTE_C5,4},{NOTE_B4,4},
  {NOTE_A4,2},{NOTE_C5,4},{NOTE_D5,4},
  {NOTE_E5,4},{NOTE_D5,4},{NOTE_C5,4},{NOTE_B4,4},
  {NOTE_A4,2},{NOTE_A4,4},{NOTE_C5,4},
  {NOTE_D5,4},{NOTE_C5,4},{NOTE_B4,4},{NOTE_A4,4},
  {NOTE_G4,2},{NOTE_A4,2}
};

// 25. Auld Lang Syne (traditional Scottish song)
const Note auldLangSyne[] = {
  {NOTE_G4,4},{NOTE_C5,4},{NOTE_C5,4},{NOTE_C5,8},{NOTE_E5,8},
  {NOTE_D5,4},{NOTE_C5,4},{NOTE_D5,4},{NOTE_E5,4},
  {NOTE_C5,4},{NOTE_C5,4},{NOTE_E5,4},{NOTE_G5,4},
  {NOTE_A5,2},{NOTE_G5,4},{NOTE_E5,4},
  {NOTE_D5,4},{NOTE_C5,4},{NOTE_D5,4},{NOTE_E5,4},
  {NOTE_C5,2},{NOTE_A4,4},{NOTE_A4,4},
  {NOTE_G4,4},{NOTE_C5,4},{NOTE_C5,4},{NOTE_E5,4},
  {NOTE_D5,2},{NOTE_C5,2}
};

// 26. Pop Goes the Weasel (traditional English nursery rhyme)
const Note popGoesTheWeasel[] = {
  {NOTE_C5,4},{NOTE_C5,8},{NOTE_D5,8},{NOTE_E5,4},{NOTE_C5,4},
  {NOTE_A4,4},{NOTE_A4,8},{NOTE_B4,8},{NOTE_C5,4},{NOTE_A4,4},
  {NOTE_G4,4},{NOTE_G4,8},{NOTE_A4,8},{NOTE_B4,4},{NOTE_G4,4},
  {NOTE_E4,4},{NOTE_E4,8},{NOTE_F4,8},{NOTE_G4,4},{NOTE_E4,4},
  {NOTE_C5,4},{NOTE_C5,8},{NOTE_D5,8},{NOTE_E5,4},{NOTE_C5,4},
  {NOTE_A4,4},{NOTE_A4,8},{NOTE_B4,8},{NOTE_C5,4},{NOTE_A4,4},
  {NOTE_G4,4},{NOTE_E4,4},{NOTE_G4,4},{NOTE_C5,2}
};

// ---------------- MORE PUBLIC-DOMAIN CLASSICS ----------------

// 27. William Tell Overture (Rossini, 1829) - opening gallop motif
const Note williamTell[] = {
  {NOTE_E5,8},{NOTE_E5,8},{NOTE_E5,8},{NOTE_E5,8},{NOTE_E5,8},{NOTE_E5,8},
  {NOTE_E5,8},{NOTE_G5,8},{NOTE_C5,8},{NOTE_E5,8},{NOTE_G5,4},{NOTE_C5,4},
  {NOTE_C5,8},{NOTE_G4,8},{NOTE_E4,8},{NOTE_C4,8},{NOTE_E4,8},{NOTE_G4,8},
  {NOTE_C5,4},{NOTE_G4,4},{NOTE_E4,4},{NOTE_C4,2}
};

// 28. Eine kleine Nachtmusik (Mozart, 1787) - opening theme
const Note kleineNachtmusik[] = {
  {NOTE_G4,4},{NOTE_D5,4},{NOTE_G4,4},{NOTE_D5,4},
  {NOTE_G4,4},{NOTE_B4,4},{NOTE_D5,4},{NOTE_G5,4},
  {NOTE_FS5,8},{NOTE_E5,8},{NOTE_D5,8},{NOTE_C5,8},{NOTE_B4,4},{NOTE_A4,4},
  {NOTE_G4,4},{NOTE_D5,4},{NOTE_G4,2}
};

// 29. Spring (Vivaldi, 1725) - opening ritornello motif
const Note vivaldiSpring[] = {
  {NOTE_E5,8},{NOTE_E5,8},{NOTE_E5,8},{NOTE_E5,8},
  {NOTE_E5,8},{NOTE_D5,8},{NOTE_C5,8},{NOTE_D5,8},
  {NOTE_E5,4},{NOTE_E5,4},{NOTE_E5,4},
  {NOTE_D5,8},{NOTE_D5,8},{NOTE_D5,8},{NOTE_D5,8},
  {NOTE_D5,8},{NOTE_C5,8},{NOTE_B4,8},{NOTE_C5,8},
  {NOTE_D5,4},{NOTE_D5,4},{NOTE_D5,4}
};

// 30. Habanera from Carmen (Bizet, 1875) - simplified main motif
const Note habanera[] = {
  {NOTE_D5,4},{NOTE_CS5,8},{NOTE_C5,8},{NOTE_B4,4},{NOTE_B4,4},
  {NOTE_A4,4},{NOTE_B4,8},{NOTE_C5,8},{NOTE_CS5,4},{NOTE_CS5,4},
  {NOTE_D5,4},{NOTE_CS5,8},{NOTE_C5,8},{NOTE_B4,4},{NOTE_B4,4},
  {NOTE_A4,4},{NOTE_B4,8},{NOTE_C5,8},{NOTE_CS5,4},{NOTE_D5,2}
};

// 31. The Entertainer (Joplin, 1902) - opening ragtime strain, simplified
const Note entertainer[] = {
  {NOTE_D5,8},{NOTE_DS5,8},{NOTE_E5,8},{NOTE_C5,8},{NOTE_E5,8},{NOTE_C5,8},
  {NOTE_E5,8},{NOTE_C5,8},{NOTE_E5,8},{NOTE_C5,8},{NOTE_E5,8},{NOTE_C5,8},
  {NOTE_B4,8},{NOTE_C5,8},{NOTE_D5,8},{NOTE_E5,8},{NOTE_C5,8},{NOTE_A4,8},
  {NOTE_FS4,8},{NOTE_G4,8},{NOTE_A4,8},{NOTE_B4,8},{NOTE_C5,4}
};

// 32. Amazing Grace (traditional hymn tune, New Britain)
const Note amazingGrace[] = {
  {NOTE_G4,2},{NOTE_C5,4},{NOTE_E5,2},{NOTE_C5,4},
  {NOTE_E5,2},{NOTE_D5,4},{NOTE_C5,2},{NOTE_A4,4},
  {NOTE_G4,2},{NOTE_G4,4},{NOTE_C5,2},{NOTE_E5,4},
  {NOTE_G5,2},{NOTE_E5,4},{NOTE_C5,2}
};

// 33. Danny Boy / Londonderry Air (traditional Irish melody)
const Note dannyBoy[] = {
  {NOTE_G4,4},{NOTE_C5,4},{NOTE_D5,4},{NOTE_E5,4},
  {NOTE_D5,4},{NOTE_C5,4},{NOTE_A4,2},
  {NOTE_G4,4},{NOTE_A4,4},{NOTE_C5,4},{NOTE_D5,4},
  {NOTE_E5,2},{NOTE_D5,4},{NOTE_C5,2}
};

// 34. Hungarian Dance No. 5 (Brahms, 1869) - recognizable opening phrase
const Note hungarianDance[] = {
  {NOTE_FS4,4},{NOTE_A4,4},{NOTE_C5,4},{NOTE_E5,4},
  {NOTE_D5,4},{NOTE_C5,4},{NOTE_B4,4},{NOTE_A4,4},
  {NOTE_G4,4},{NOTE_B4,4},{NOTE_D5,4},{NOTE_G5,4},
  {NOTE_FS5,4},{NOTE_E5,4},{NOTE_D5,4},{NOTE_C5,4},
  {NOTE_B4,2},{NOTE_A4,2}
};

// 35. Swan Lake (Tchaikovsky, 1876) - main theme, simplified
const Note swanLake[] = {
  {NOTE_B4,4},{NOTE_E5,2},{NOTE_D5,4},{NOTE_C5,2},
  {NOTE_B4,4},{NOTE_A4,2},{NOTE_G4,4},{NOTE_FS4,2},
  {NOTE_E4,4},{NOTE_G4,4},{NOTE_B4,4},{NOTE_E5,2},
  {NOTE_D5,4},{NOTE_C5,2},{NOTE_B4,4},{NOTE_A4,2}
};

// 36. House of the Rising Sun (traditional folk melody; simplified)
const Note risingSun[] = {
  {NOTE_A4,4},{NOTE_C5,4},{NOTE_E5,4},{NOTE_A5,4},
  {NOTE_C5,4},{NOTE_E5,4},{NOTE_A5,2},
  {NOTE_C5,4},{NOTE_E5,4},{NOTE_G5,4},{NOTE_C5,4},
  {NOTE_E5,4},{NOTE_G5,4},{NOTE_E5,2},
  {NOTE_A4,4},{NOTE_C5,4},{NOTE_E5,4},{NOTE_A5,2}
};

// 37. The Blue Danube (Johann Strauss II, 1866) - opening waltz phrase
const Note blueDanube[] = {
  {NOTE_FS4,4},{NOTE_A4,4},{NOTE_FS4,4},{NOTE_D4,4},
  {NOTE_D4,4},{NOTE_D4,4},{NOTE_CS4,4},{NOTE_E4,4},
  {NOTE_E4,4},{NOTE_E4,4},{NOTE_B4,4},{NOTE_D5,4},
  {NOTE_D5,4},{NOTE_D5,4},{NOTE_CS5,4},{NOTE_E5,4},
  {NOTE_E5,4},{NOTE_E5,4},{NOTE_A4,4},{NOTE_CS5,4},
  {NOTE_B4,2},{NOTE_B4,2}
};

// 38. Radetzky March (Johann Strauss I, 1848) - recognizable opening idea
const Note radetzkyMarch[] = {
  {NOTE_D5,4},{NOTE_A4,4},{NOTE_D5,4},{NOTE_FS5,4},
  {NOTE_A5,4},{NOTE_FS5,4},{NOTE_D5,4},{NOTE_A4,4},
  {NOTE_D5,4},{NOTE_A4,4},{NOTE_D5,4},{NOTE_FS5,4},
  {NOTE_A5,2},{NOTE_A5,4},{NOTE_A5,4},
  {NOTE_G5,4},{NOTE_FS5,4},{NOTE_E5,4},{NOTE_D5,4},
  {NOTE_CS5,4},{NOTE_B4,4},{NOTE_A4,2}
};

// 39. Bridal Chorus (Richard Wagner, 1850) - wedding march theme
const Note bridalChorus[] = {
  {NOTE_FS4,4},{NOTE_FS4,4},{NOTE_FS4,4},{NOTE_A4,2},
  {NOTE_FS4,4},{NOTE_E4,4},{NOTE_D4,4},{NOTE_D4,4},
  {NOTE_E4,4},{NOTE_FS4,4},{NOTE_G4,4},{NOTE_E4,2},
  {NOTE_E4,4},{NOTE_FS4,4},{NOTE_G4,4},{NOTE_E4,4},
  {NOTE_D4,4},{NOTE_CS4,4},{NOTE_D4,2}
};

// 40. La donna e mobile (Giuseppe Verdi, 1851) - opera aria motif
const Note laDonnaEMobile[] = {
  {NOTE_A4,4},{NOTE_D5,4},{NOTE_D5,4},{NOTE_D5,4},
  {NOTE_C5,4},{NOTE_B4,4},{NOTE_A4,4},{NOTE_G4,4},
  {NOTE_FS4,4},{NOTE_FS4,4},{NOTE_FS4,4},{NOTE_FS4,4},
  {NOTE_E4,4},{NOTE_A4,4},{NOTE_A4,2},
  {NOTE_A4,4},{NOTE_D5,4},{NOTE_D5,4},{NOTE_D5,4},
  {NOTE_C5,4},{NOTE_B4,4},{NOTE_A4,2}
};

// 41. Toreador Song (Georges Bizet, 1875) - opening march-like motif
const Note toreadorSong[] = {
  {NOTE_G4,4},{NOTE_C5,4},{NOTE_C5,4},{NOTE_C5,4},
  {NOTE_D5,4},{NOTE_E5,4},{NOTE_D5,4},{NOTE_C5,4},
  {NOTE_B4,4},{NOTE_G4,4},{NOTE_B4,4},{NOTE_D5,4},
  {NOTE_C5,2},{NOTE_G4,4},{NOTE_C5,4},
  {NOTE_E5,4},{NOTE_D5,4},{NOTE_C5,4},{NOTE_B4,4},
  {NOTE_C5,2}
};

// 42. Jesu, Joy of Man's Desiring (J.S. Bach, 1723) - simplified chorale line
const Note jesuJoy[] = {
  {NOTE_G4,4},{NOTE_A4,4},{NOTE_B4,4},{NOTE_D5,4},
  {NOTE_C5,4},{NOTE_B4,4},{NOTE_A4,4},{NOTE_G4,4},
  {NOTE_FS4,4},{NOTE_G4,4},{NOTE_A4,4},{NOTE_B4,4},
  {NOTE_A4,4},{NOTE_G4,4},{NOTE_FS4,2},
  {NOTE_G4,4},{NOTE_A4,4},{NOTE_B4,4},{NOTE_D5,4},
  {NOTE_C5,4},{NOTE_B4,4},{NOTE_A4,4},{NOTE_G4,2}
};

// 43. Morning Mood (Edvard Grieg, 1875) - opening flute melody, simplified
const Note morningMood[] = {
  {NOTE_E4,4},{NOTE_G4,4},{NOTE_A4,4},{NOTE_B4,4},
  {NOTE_C5,2},{NOTE_B4,4},{NOTE_G4,4},
  {NOTE_A4,4},{NOTE_B4,4},{NOTE_C5,4},{NOTE_D5,4},
  {NOTE_E5,2},{NOTE_D5,4},{NOTE_C5,4},
  {NOTE_B4,4},{NOTE_A4,4},{NOTE_G4,2}
};

// 44. O Christmas Tree (traditional German carol)
const Note oChristmasTree[] = {
  {NOTE_G4,4},{NOTE_C5,4},{NOTE_C5,4},{NOTE_C5,2},
  {NOTE_D5,4},{NOTE_E5,4},{NOTE_E5,4},{NOTE_E5,2},
  {NOTE_E5,4},{NOTE_D5,4},{NOTE_E5,4},{NOTE_C5,4},
  {NOTE_D5,4},{NOTE_A4,4},{NOTE_B4,4},{NOTE_C5,2}
};

// 45. Mary Had a Little Lamb (traditional nursery rhyme)
const Note maryHadALittleLamb[] = {
  {NOTE_E4,4},{NOTE_D4,4},{NOTE_C4,4},{NOTE_D4,4},
  {NOTE_E4,4},{NOTE_E4,4},{NOTE_E4,2},
  {NOTE_D4,4},{NOTE_D4,4},{NOTE_D4,2},
  {NOTE_E4,4},{NOTE_G4,4},{NOTE_G4,2},
  {NOTE_E4,4},{NOTE_D4,4},{NOTE_C4,4},{NOTE_D4,4},
  {NOTE_E4,4},{NOTE_E4,4},{NOTE_E4,4},{NOTE_E4,4},
  {NOTE_D4,4},{NOTE_D4,4},{NOTE_E4,4},{NOTE_D4,4},{NOTE_C4,2}
};

// 46. Old MacDonald Had a Farm (traditional children's song)
const Note oldMacDonald[] = {
  {NOTE_C4,4},{NOTE_C4,4},{NOTE_C4,4},{NOTE_G4,4},
  {NOTE_A4,4},{NOTE_A4,4},{NOTE_G4,2},
  {NOTE_E4,4},{NOTE_E4,4},{NOTE_D4,4},{NOTE_D4,4},
  {NOTE_C4,2},{NOTE_G4,4},{NOTE_G4,4},
  {NOTE_C4,4},{NOTE_C4,4},{NOTE_C4,4},{NOTE_G4,4},
  {NOTE_A4,4},{NOTE_A4,4},{NOTE_G4,2}
};

// 47. My Bonnie Lies Over the Ocean (traditional Scottish/folk song)
const Note myBonnie[] = {
  {NOTE_G4,4},{NOTE_E4,4},{NOTE_E4,4},{NOTE_F4,4},
  {NOTE_G4,2},{NOTE_G4,4},{NOTE_G4,4},
  {NOTE_A4,4},{NOTE_B4,4},{NOTE_C5,2},
  {NOTE_B4,4},{NOTE_A4,4},{NOTE_G4,2},
  {NOTE_E4,4},{NOTE_G4,4},{NOTE_A4,4},{NOTE_B4,4},
  {NOTE_C5,2},{NOTE_B4,4},{NOTE_A4,4},{NOTE_G4,2}
};

// 48. Shenandoah (traditional American folk song)
const Note shenandoah[] = {
  {NOTE_G4,2},{NOTE_C5,4},{NOTE_D5,4},{NOTE_E5,2},
  {NOTE_D5,4},{NOTE_C5,4},{NOTE_A4,2},
  {NOTE_G4,4},{NOTE_A4,4},{NOTE_C5,2},
  {NOTE_D5,4},{NOTE_C5,4},{NOTE_A4,2},
  {NOTE_G4,2},{NOTE_E4,4},{NOTE_G4,4},{NOTE_A4,2},
  {NOTE_G4,1}
};

// 49. Kumbaya (traditional spiritual)
const Note kumbaya[] = {
  {NOTE_G4,4},{NOTE_C5,4},{NOTE_C5,2},
  {NOTE_E5,4},{NOTE_D5,4},{NOTE_C5,2},
  {NOTE_A4,4},{NOTE_G4,4},{NOTE_E4,2},
  {NOTE_G4,4},{NOTE_A4,4},{NOTE_G4,2},
  {NOTE_G4,4},{NOTE_C5,4},{NOTE_C5,2},
  {NOTE_A4,4},{NOTE_G4,4},{NOTE_C5,1}
};

// 50. Camptown Races (Stephen Foster, 1850)
const Note camptownRaces[] = {
  {NOTE_C5,4},{NOTE_C5,4},{NOTE_C5,4},{NOTE_E5,4},
  {NOTE_G5,2},{NOTE_G5,2},
  {NOTE_A4,4},{NOTE_A4,4},{NOTE_A4,4},{NOTE_C5,4},
  {NOTE_E5,2},{NOTE_E5,2},
  {NOTE_G4,4},{NOTE_A4,4},{NOTE_G4,4},{NOTE_E4,4},
  {NOTE_C4,2},{NOTE_C4,2}
};

// ------------------------------------------
// Playlist
// ------------------------------------------
struct Song {
  const char*   title;
  const Note*   notes;
  uint16_t      length;
  uint8_t       bpm;
};

#define SONG(title, arr, bpm) { title, arr, (uint16_t)(sizeof(arr) / sizeof(Note)), bpm }

const Song playlist[] = {
  SONG("Ode to Joy",               odeToJoy,         115),
  SONG("Minuet in G Major",        minuet,           125),
  SONG("Twinkle Twinkle",          twinkle,          110),
  SONG("Fur Elise",                furElise,         125),
  SONG("Greensleeves",             greensleeves,     115),
  SONG("Canon in D",               canon,            100),
  SONG("Brahms Lullaby",           lullaby,           90),
  SONG("Korobeiniki",              korobeiniki,      145),
  SONG("Jingle Bells",             jingleBells,      130),
  SONG("Silent Night",             silentNight,       80),
  SONG("We Wish You a Merry Xmas", merryChristmas,   140),
  SONG("Beethoven's 5th",          beethoven5,       108),
  SONG("Hall of the Mountain King",mountainKing,     120),
  SONG("Happy Birthday",           happyBirthday,    100),
  SONG("London Bridge",            londonBridge,     130),
  SONG("Row Row Row Your Boat",    rowBoat,          120),
  SONG("Turkish March",            turkishMarch,     138),
  SONG("Frere Jacques",            frereJacques,     112),
  SONG("Yankee Doodle",            yankeeDoodle,     120),
  SONG("Oh Susanna",               ohSusanna,        118),
  SONG("When the Saints",          whenTheSaints,    116),
  SONG("Deck the Halls",           deckTheHalls,     132),
  SONG("Joy to the World",         joyToTheWorld,    120),
  SONG("Scarborough Fair",         scarboroughFair,   96),
  SONG("Auld Lang Syne",           auldLangSyne,     108),
  SONG("Pop Goes the Weasel",      popGoesTheWeasel, 132),
  SONG("William Tell Overture",    williamTell,      150),
  SONG("Eine Kleine Nachtmusik",   kleineNachtmusik, 132),
  SONG("Vivaldi: Spring",          vivaldiSpring,    140),
  SONG("Carmen: Habanera",         habanera,         110),
  SONG("The Entertainer",          entertainer,      130),
  SONG("Amazing Grace",            amazingGrace,      90),
  SONG("Danny Boy",                dannyBoy,          88),
  SONG("Hungarian Dance No. 5",    hungarianDance,   120),
  SONG("Swan Lake Theme",          swanLake,          92),
  SONG("House of the Rising Sun",  risingSun,         96),
  SONG("The Blue Danube",          blueDanube,       150),
  SONG("Radetzky March",           radetzkyMarch,    120),
  SONG("Bridal Chorus",            bridalChorus,      96),
  SONG("La Donna e Mobile",        laDonnaEMobile,   120),
  SONG("Toreador Song",            toreadorSong,     126),
  SONG("Jesu Joy of Man's Desiring",jesuJoy,          96),
  SONG("Morning Mood",             morningMood,       88),
  SONG("O Christmas Tree",         oChristmasTree,   112),
  SONG("Mary Had a Little Lamb",   maryHadALittleLamb,120),
  SONG("Old MacDonald",            oldMacDonald,     116),
  SONG("My Bonnie",                myBonnie,          96),
  SONG("Shenandoah",               shenandoah,        84),
  SONG("Kumbaya",                  kumbaya,           92),
  SONG("Camptown Races",           camptownRaces,    132)
};

const int TOTAL_SONGS = sizeof(playlist) / sizeof(playlist[0]);

// Curated display notes, in the same order as the playlist above.
// Keep this table aligned with playlist[] when adding or reordering songs.
struct SongInfo { const char* composer; const char* period; const char* fact; };
const SongInfo songInfo[] = {
  {"Ludwig van Beethoven", "Classical | 1824", "Inspired by Schiller's poem"},
  {"Christian Petzold", "Baroque | c.1725", "Once attributed to J.S. Bach"},
  {"Traditional", "Nursery rhyme", "A familiar lullaby across generations"},
  {"Ludwig van Beethoven", "Classical | c.1810", "One of the best-known piano themes"},
  {"Traditional English", "Folk song", "A melody with a long, varied history"},
  {"Johann Pachelbel", "Baroque | c.1680", "Famous repeating harmonic progression"},
  {"Johannes Brahms", "Romantic | 1868", "Also known as the Cradle Song"},
  {"Russian folk tune", "Folk | 1861", "The melody behind the Tetris theme"},
  {"James Lord Pierpont", "1857", "Originally published as One Horse Open Sleigh"},
  {"Franz Xaver Gruber", "Christmas carol | 1818", "First performed in Oberndorf, Austria"},
  {"Traditional English", "Christmas carol", "A well-known festive wassailing song"},
  {"Ludwig van Beethoven", "Classical | 1808", "The opening motif uses four notes"},
  {"Edvard Grieg", "Romantic | 1875", "Builds from quiet to a dramatic climax"},
  {"Traditional", "Celebration song", "Often sung at birthday gatherings"},
  {"Traditional English", "Folk song", "A nursery rhyme about a London bridge"},
  {"Traditional", "Children's song", "A rowing song with a round-like structure"},
  {"Wolfgang Amadeus Mozart", "Classical | 1783", "Also called Rondo alla Turca"},
  {"Traditional French", "Nursery song", "Its title means Brother John"},
  {"Traditional American", "Folk song", "A song associated with early America"},
  {"Stephen Foster", "American folk | 1848", "A popular minstrel-era song"},
  {"Traditional Gospel", "Spiritual", "A joyful call-and-response standard"},
  {"Traditional Welsh", "Christmas carol", "A lively carol celebrating the holly"},
  {"George Frideric Handel", "Baroque | 1741", "From the Messiah oratorio"},
  {"Traditional English", "Folk ballad", "A ballad about a mysterious fair"},
  {"Traditional Scottish", "Folk song", "A song of remembrance and friendship"},
  {"Traditional English", "Nursery rhyme", "A tune about a weasel and a pop"},
  {"Gioachino Rossini", "Romantic | 1829", "The famous finale's galloping energy"},
  {"Wolfgang Amadeus Mozart", "Classical | 1787", "A serenade in G major, K. 525"},
  {"Antonio Vivaldi", "Baroque | 1725", "The first concerto in The Four Seasons"},
  {"Georges Bizet", "Romantic | 1875", "The seductive aria from the opera Carmen"},
  {"Scott Joplin", "Ragtime | 1902", "A landmark ragtime piano composition"},
  {"Traditional hymn", "Folk hymn", "Set to the tune New Britain"},
  {"Traditional Irish", "Folk air", "Also known as the Londonderry Air"},
  {"Johannes Brahms", "Romantic | 1869", "A lively dance based on Hungarian themes"},
  {"Pyotr Ilyich Tchaikovsky", "Romantic | 1876", "From the ballet of the same name"},
  {"Traditional folk", "Folk song", "A song with many regional versions"},
  {"Johann Strauss II", "Romantic | 1866", "One of the best-known Viennese waltzes"},
  {"Johann Strauss I", "Romantic | 1848", "Famous for its lively finale"},
  {"Richard Wagner", "Romantic | 1850", "Often played at wedding ceremonies"},
  {"Giuseppe Verdi", "Opera | 1851", "A celebrated aria from Rigoletto"},
  {"Georges Bizet", "Opera | 1875", "The bold march from Carmen"},
  {"Johann Sebastian Bach", "Baroque | 1723", "A chorale from Cantata BWV 147"},
  {"Edvard Grieg", "Romantic | 1875", "The opening of Peer Gynt Suite No. 1"},
  {"Traditional German", "Christmas carol", "Known in German as O Tannenbaum"},
  {"Traditional", "Nursery rhyme", "A classic counting and singing rhyme"},
  {"Traditional", "Children's song", "An animal song with many verses"},
  {"Traditional Scottish", "Folk song", "A song of longing and homecoming"},
  {"Traditional American", "Folk song", "A river song of distance and yearning"},
  {"Traditional spiritual", "Spiritual", "A song of comfort and togetherness"},
  {"Stephen Foster", "American song | 1850", "A lively song about the races"}
};
static_assert((sizeof(songInfo) / sizeof(songInfo[0])) ==
              (sizeof(playlist) / sizeof(playlist[0])),
              "songInfo must have one entry for every playlist song");

// Short genre tags sized for the OLED's 32-pixel top-right header area.
const char* const songGenres[] = {
  "CLASS", "BAROQ", "NURS", "CLASS", "FOLK", "BAROQ", "LULL", "FOLK",
  "CAROL", "CAROL", "CAROL", "CLASS", "CLASS", "NURS", "NURS", "NURS",
  "CLASS", "NURS", "FOLK", "FOLK", "GOSPL", "CAROL", "BAROQ", "FOLK",
  "FOLK", "NURS", "OPERA", "CLASS", "CLASS", "OPERA", "RAG", "HYMN",
  "FOLK", "DANCE", "BALLET", "FOLK", "WALTZ", "MARCH", "MARCH", "OPERA",
  "OPERA", "BAROQ", "CLASS", "CAROL", "NURS", "NURS", "FOLK", "FOLK",
  "GOSPL", "FOLK"
};
static_assert((sizeof(songGenres) / sizeof(songGenres[0])) ==
              (sizeof(playlist) / sizeof(playlist[0])),
              "songGenres must have one entry for every playlist song");

// ------------------------------------------
// State
// ------------------------------------------
enum NavAction   { NAV_NONE, NAV_PREV, NAV_NEXT };
enum ButtonEvent { EV_NONE, EV_PREV_SHORT, EV_NEXT_SHORT, EV_PREV_LONG, EV_NEXT_LONG, EV_PAUSE };

int           currentSongIndex = 0;
bool          shuffleOn        = false;
bool          playbackPaused   = false;
Preferences   jukeboxPrefs;
const char*   PREF_NAMESPACE   = "jukebox";
const char*   PREF_LAST_SONG   = "lastSong";
unsigned long songStartMs      = 0;
uint32_t      activeFreq       = 0;

const int HISTORY_SIZE = 16;
int historyStack[HISTORY_SIZE];
int historyCount = 0;

void saveLastSong() {
  jukeboxPrefs.putInt(PREF_LAST_SONG, currentSongIndex);
}

// ------------------------------------------
// Debounced buttons with short / long press
// Short press fires on RELEASE, long press fires while still held.
// Holding a button no longer machine-guns through the playlist.
// ------------------------------------------
struct Button {
  uint8_t       pin;
  bool          stableDown;
  bool          rawLast;
  unsigned long lastChange;
  unsigned long pressedAt;
  bool          longFired;
};

Button prevBtn  = { (uint8_t)PREV_BTN_PIN,  false, false, 0, 0, false };
Button nextBtn  = { (uint8_t)NEXT_BTN_PIN,  false, false, 0, 0, false };
Button pauseBtn = { (uint8_t)PAUSE_BTN_PIN, false, false, 0, 0, false };

// Explicit prototype: Arduino's auto-prototype generator can otherwise place
// this declaration before the custom Button / ButtonEvent types are known.
ButtonEvent updateButton(Button &b, ButtonEvent shortEv, ButtonEvent longEv);

ButtonEvent updateButton(Button &b, ButtonEvent shortEv, ButtonEvent longEv) {
  const unsigned long now = millis();
  const bool raw = (digitalRead(b.pin) == LOW);

  if (raw != b.rawLast) {
    b.rawLast = raw;
    b.lastChange = now;
  }

  if (raw != b.stableDown && (now - b.lastChange) >= DEBOUNCE_MS) {
    b.stableDown = raw;
    if (raw) {
      b.pressedAt = now;
      b.longFired = false;
    } else if (!b.longFired) {
      return shortEv;
    }
  }

  if (b.stableDown && !b.longFired && (now - b.pressedAt) >= LONG_PRESS_MS) {
    b.longFired = true;
    return longEv;
  }
  return EV_NONE;
}

// ------------------------------------------
// Tiny built-in OLED driver (SSD1306 / SH1106, 128x64, I2C) - no library
// Framebuffer is page-major: oledBuf[page * 128 + x], 8 vertical pixels/byte.
// ------------------------------------------
uint8_t oledBuf[128 * 8];

// Classic 5x7 font, ASCII 32..126 (5 column bytes per glyph, LSB = top row)
const uint8_t FONT5X7[] = {
  0x00,0x00,0x00,0x00,0x00,  // ' '
  0x00,0x00,0x5F,0x00,0x00,  // '!'
  0x00,0x03,0x00,0x03,0x00,  // '"'
  0x14,0x7F,0x14,0x7F,0x14,  // '#'
  0x24,0x2A,0x7F,0x2A,0x12,  // '$'
  0x23,0x13,0x08,0x64,0x62,  // '%'
  0x36,0x49,0x55,0x22,0x50,  // '&'
  0x00,0x05,0x03,0x00,0x00,  // '''
  0x00,0x1C,0x22,0x41,0x00,  // '('
  0x00,0x41,0x22,0x1C,0x00,  // ')'
  0x14,0x08,0x3E,0x08,0x14,  // '*'
  0x08,0x08,0x3E,0x08,0x08,  // '+'
  0x00,0x50,0x30,0x00,0x00,  // ','
  0x08,0x08,0x08,0x08,0x08,  // '-'
  0x00,0x60,0x60,0x00,0x00,  // '.'
  0x20,0x10,0x08,0x04,0x02,  // '/'
  0x3E,0x51,0x49,0x45,0x3E,  // '0'
  0x00,0x42,0x7F,0x40,0x00,  // '1'
  0x42,0x61,0x51,0x49,0x46,  // '2'
  0x21,0x41,0x45,0x4B,0x31,  // '3'
  0x18,0x14,0x12,0x7F,0x10,  // '4'
  0x27,0x45,0x45,0x45,0x39,  // '5'
  0x3C,0x4A,0x49,0x49,0x30,  // '6'
  0x01,0x71,0x09,0x05,0x03,  // '7'
  0x36,0x49,0x49,0x49,0x36,  // '8'
  0x06,0x49,0x49,0x29,0x1E,  // '9'
  0x00,0x36,0x36,0x00,0x00,  // ':'
  0x00,0x56,0x36,0x00,0x00,  // ';'
  0x08,0x14,0x22,0x41,0x00,  // '<'
  0x14,0x14,0x14,0x14,0x14,  // '='
  0x00,0x41,0x22,0x14,0x08,  // '>'
  0x02,0x01,0x51,0x09,0x06,  // '?'
  0x32,0x49,0x79,0x41,0x3E,  // '@'
  0x7E,0x11,0x11,0x11,0x7E,  // 'A'
  0x7F,0x49,0x49,0x49,0x36,  // 'B'
  0x3E,0x41,0x41,0x41,0x22,  // 'C'
  0x7F,0x41,0x41,0x22,0x1C,  // 'D'
  0x7F,0x49,0x49,0x49,0x41,  // 'E'
  0x7F,0x09,0x09,0x09,0x01,  // 'F'
  0x3E,0x41,0x49,0x49,0x7A,  // 'G'
  0x7F,0x08,0x08,0x08,0x7F,  // 'H'
  0x00,0x41,0x7F,0x41,0x00,  // 'I'
  0x20,0x40,0x41,0x3F,0x01,  // 'J'
  0x7F,0x08,0x14,0x22,0x41,  // 'K'
  0x7F,0x40,0x40,0x40,0x40,  // 'L'
  0x7F,0x02,0x0C,0x02,0x7F,  // 'M'
  0x7F,0x04,0x08,0x10,0x7F,  // 'N'
  0x3E,0x41,0x41,0x41,0x3E,  // 'O'
  0x7F,0x09,0x09,0x09,0x06,  // 'P'
  0x3E,0x41,0x51,0x21,0x5E,  // 'Q'
  0x7F,0x09,0x19,0x29,0x46,  // 'R'
  0x46,0x49,0x49,0x49,0x31,  // 'S'
  0x01,0x01,0x7F,0x01,0x01,  // 'T'
  0x3F,0x40,0x40,0x40,0x3F,  // 'U'
  0x1F,0x20,0x40,0x20,0x1F,  // 'V'
  0x3F,0x40,0x38,0x40,0x3F,  // 'W'
  0x63,0x14,0x08,0x14,0x63,  // 'X'
  0x07,0x08,0x70,0x08,0x07,  // 'Y'
  0x61,0x51,0x49,0x45,0x43,  // 'Z'
  0x00,0x7F,0x41,0x41,0x00,  // '['
  0x02,0x04,0x08,0x10,0x20,  // 'backslash'
  0x00,0x41,0x41,0x7F,0x00,  // ']'
  0x04,0x02,0x01,0x02,0x04,  // '^'
  0x40,0x40,0x40,0x40,0x40,  // '_'
  0x00,0x01,0x02,0x04,0x00,  // '`'
  0x20,0x54,0x54,0x54,0x78,  // 'a'
  0x7F,0x48,0x44,0x44,0x38,  // 'b'
  0x38,0x44,0x44,0x44,0x20,  // 'c'
  0x38,0x44,0x44,0x48,0x7F,  // 'd'
  0x38,0x54,0x54,0x54,0x18,  // 'e'
  0x08,0x7E,0x09,0x01,0x02,  // 'f'
  0x0C,0x52,0x52,0x52,0x3E,  // 'g'
  0x7F,0x08,0x04,0x04,0x78,  // 'h'
  0x00,0x44,0x7D,0x40,0x00,  // 'i'
  0x20,0x40,0x44,0x3D,0x00,  // 'j'
  0x7F,0x10,0x28,0x44,0x00,  // 'k'
  0x00,0x41,0x7F,0x40,0x00,  // 'l'
  0x7C,0x04,0x18,0x04,0x78,  // 'm'
  0x7C,0x08,0x04,0x04,0x78,  // 'n'
  0x38,0x44,0x44,0x44,0x38,  // 'o'
  0x7C,0x14,0x14,0x14,0x08,  // 'p'
  0x08,0x14,0x14,0x18,0x7C,  // 'q'
  0x7C,0x08,0x04,0x04,0x08,  // 'r'
  0x48,0x54,0x54,0x54,0x20,  // 's'
  0x04,0x3F,0x44,0x40,0x20,  // 't'
  0x3C,0x40,0x40,0x20,0x7C,  // 'u'
  0x1C,0x20,0x40,0x20,0x1C,  // 'v'
  0x3C,0x40,0x30,0x40,0x3C,  // 'w'
  0x44,0x28,0x10,0x28,0x44,  // 'x'
  0x0C,0x50,0x50,0x50,0x3C,  // 'y'
  0x44,0x64,0x54,0x4C,0x44,  // 'z'
  0x00,0x08,0x36,0x41,0x00,  // '{'
  0x00,0x00,0x7F,0x00,0x00,  // '|'
  0x00,0x41,0x36,0x08,0x00,  // '}'
  0x10,0x08,0x08,0x10,0x08,  // '~'
};

void oledCmd(uint8_t c) {
  Wire.beginTransmission(OLED_ADDR);
  Wire.write((uint8_t)0x00);
  Wire.write(c);
  Wire.endTransmission();
}

void oledInit() {
  static const uint8_t seq[] = {
    0xAE,             // display off
    0xD5, 0x80,       // clock
    0xA8, 0x3F,       // multiplex 64
    0xD3, 0x00,       // display offset
    0x40,             // start line
    0x8D, 0x14,       // charge pump on
    0x20, 0x02,       // page addressing mode
    0xA1, 0xC8,       // flip so it is upright
    0xDA, 0x12,       // COM pins
    0x81, 0xCF,       // contrast
    0xD9, 0xF1,       // pre-charge
    0xDB, 0x40,       // VCOM detect
    0xA4, 0xA6,       // resume RAM, normal (not inverted)
    0xAF              // display on
  };
  for (uint8_t i = 0; i < sizeof(seq); i++) oledCmd(seq[i]);
}

// Send pages [firstPage, firstPage+pageCount) to the display (~3 ms per page)
void oledFlush(uint8_t firstPage, uint8_t pageCount) {
  for (uint8_t p = firstPage; p < firstPage + pageCount && p < 8; p++) {
    Wire.beginTransmission(OLED_ADDR);
    Wire.write((uint8_t)0x00);
    Wire.write((uint8_t)(0xB0 | p));
    Wire.write((uint8_t)(0x00 | (OLED_COL_OFFSET & 0x0F)));
    Wire.write((uint8_t)(0x10 | (OLED_COL_OFFSET >> 4)));
    Wire.endTransmission();
    for (int x = 0; x < 128; x += 32) {
      Wire.beginTransmission(OLED_ADDR);
      Wire.write((uint8_t)0x40);
      Wire.write(&oledBuf[p * 128 + x], 32);
      Wire.endTransmission();
    }
  }
}

void oledFillRect(int x, int y, int w, int h, bool on) {
  for (int yy = y; yy < y + h; yy++) {
    if (yy < 0 || yy >= 64) continue;
    for (int xx = x; xx < x + w; xx++) {
      if (xx < 0 || xx >= 128) continue;
      uint8_t &b = oledBuf[(yy >> 3) * 128 + xx];
      if (on) b |=  (1 << (yy & 7));
      else    b &= ~(1 << (yy & 7));
    }
  }
}

void oledFrame(int x, int y, int w, int h) {
  oledFillRect(x, y, w, 1, true);
  oledFillRect(x, y + h - 1, w, 1, true);
  oledFillRect(x, y, 1, h, true);
  oledFillRect(x + w - 1, y, 1, h, true);
}

int oledTextWidth(const char* s, int scale, bool bold) {
  const int n = strlen(s);
  return n ? (n * 6 - 1) * scale + (bold ? 1 : 0) : 0;
}

void oledChar(int x, int y, char c, int scale) {
  if (c < 32 || c > 126) c = '?';
  const uint8_t* g = &FONT5X7[(c - 32) * 5];
  for (int col = 0; col < 5; col++)
    for (int row = 0; row < 7; row++)
      if (g[col] & (1 << row)) oledFillRect(x + col * scale, y + row * scale, scale, scale, true);
}

// (x, y) = top-left corner. scale 1 = 5x7 px glyphs, 2 = 10x14, ...
void oledText(int x, int y, const char* s, int scale, bool bold) {
  for (; *s; s++) {
    oledChar(x, y, *s, scale);
    if (bold) oledChar(x + 1, y, *s, scale);   // fake-bold: draw twice, 1 px apart
    x += 6 * scale;
  }
}

// Convenience overloads (instead of default arguments, which break the
// auto-generated prototypes in ArduinoDroid / Arduino IDE)
int  oledTextWidth(const char* s, int scale)          { return oledTextWidth(s, scale, false); }
void oledText(int x, int y, const char* s, int scale) { oledText(x, y, s, scale, false); }
void oledText(int x, int y, const char* s)            { oledText(x, y, s, 1, false); }

// ------------------------------------------
// OLED UI
// Unified layout (no page flipping):
//   y  0..13 : header (song number, shuffle/pause, genre label) + line
//   y 15..29 : song title (tighter top spacing, 1 or 2 lines)
//   y 31..35 : progress bar directly under song title
//   y 38..44 : BPM | elapsed / total
//   y 47..53 : current note + Hz | note #
//   y 56..62 : continuous scrolling ticker (composer | period | fact)
// ------------------------------------------
bool          oledOK    = false;
const Song*   uiSong    = nullptr;
uint32_t      uiTotalMs = 0;
uint16_t      uiNoteIdx = 0;
uint16_t      uiFreq    = 0;
unsigned long lastUiMs  = 0;

// Horizontal marquee state for the continuous song ticker.
int           tickerOffset = 0;
unsigned long lastTickerMs = 0;
char          tickerText[180];

// Find the final pitched note so each pass gets a slightly longer cadence.
int lastPitchedNoteIndex(const Song &s) {
  for (int i = (int)s.length - 1; i >= 0; --i) {
    if (s.notes[i].d != 0 && s.notes[i].f != REST) return i;
  }
  return -1;
}

int scaledNoteDurationMs(const Song &s, uint16_t i, int wholeNoteMs,
                         int lastPitchedIndex) {
  const int8_t d = s.notes[i].d;
  if (d == 0) return 0;
  int duration = (d > 0) ? (wholeNoteMs / d)
                         : (wholeNoteMs / -d) * 3 / 2;
  duration = duration * NOTE_LENGTH_PERCENT / 100;
  if ((int)i == lastPitchedIndex)
    duration += duration * (CADENCE_HOLD_PERCENT - 100) / 100;
  return duration;
}

uint32_t songDurationMs(const Song &s) {
  const int wholeNoteMs = 240000 / s.bpm;
  const int lastPitched = lastPitchedNoteIndex(s);
  uint32_t onePassMs = 0;
  for (uint16_t i = 0; i < s.length; i++)
    onePassMs += scaledNoteDurationMs(s, i, wholeNoteMs, lastPitched);

  uint32_t total = onePassMs * SONG_PLAYTHROUGHS;
  if (SONG_PLAYTHROUGHS > 1)
    total += (uint32_t)(SONG_PLAYTHROUGHS - 1) * REPEAT_BREAK_MS;
  total += ENDING_SILENCE_MS;
  return total;
}

void noteLabel(uint16_t f, char* out, size_t n) {
  static const char* names[12] = { "C","C#","D","D#","E","F","F#","G","G#","A","A#","B" };
  if (f == 0) { snprintf(out, n, "rest"); return; }
  int midi = (int)lroundf(69.0f + 12.0f * log2f(f / 440.0f));
  snprintf(out, n, "%s%d %uHz", names[midi % 12], midi / 12 - 1, (unsigned)f);
}

void uiDrawHeader() {
  oledFillRect(0, 0, 128, 14, false);
  char buf[16];
  snprintf(buf, sizeof(buf), "%d/%d", currentSongIndex + 1, TOTAL_SONGS);
  oledText(0, 2, buf);
  if (shuffleOn) oledText(36, 2, "SHUF");
  if (playbackPaused) oledText(64, 2, "PAUSE");
  oledText(94, 2, songGenres[currentSongIndex]);
  oledFillRect(0, 12, 128, 1, true);
}

void uiDrawTitle(const char* title) {
  // Compact 15px zone (y=15..29) right below the header line
  oledFillRect(0, 14, 128, 16, false);

  // Single line bold if it fits (centered vertically at y=19)
  int w = oledTextWidth(title, 1, true);
  if (w <= 128) {
    oledText((128 - w) / 2, 19, title, 1, true);
    return;
  }

  // Two compact lines (y=15 and y=23) split at the space nearest the middle
  char buf[40];
  strncpy(buf, title, sizeof(buf) - 1);
  buf[sizeof(buf) - 1] = '\0';
  const int len = strlen(buf);
  int split = -1;
  for (int i = 0; i < len; i++) {
    if (buf[i] == ' ' && (split < 0 || abs(i - len / 2) < abs(split - len / 2))) split = i;
  }
  if (split < 0) {
    oledText(0, 19, buf, 1, true);
    return;
  }
  buf[split] = '\0';
  const char* l1 = buf;
  const char* l2 = buf + split + 1;
  oledText((128 - oledTextWidth(l1, 1, true)) / 2, 15, l1, 1, true);
  oledText((128 - oledTextWidth(l2, 1, true)) / 2, 23, l2, 1, true);
}

void uiDrawLive() {
  // Covers y=31..54 (progress bar + BPM/time + note info)
  oledFillRect(0, 31, 128, 24, false);
  if (!uiSong) return;
  char buf[24];

  uint32_t el = millis() - songStartMs;
  if (el > uiTotalMs) el = uiTotalMs;

  // 1. Progress bar directly under the song title (y=31..35)
  oledFrame(0, 31, 128, 5);
  const uint32_t w = uiTotalMs ? (el * 126UL) / uiTotalMs : 0;
  if (w) oledFillRect(1, 32, (int)w, 3, true);

  // 2. BPM (left) | elapsed / total (right) at y=38
  const unsigned playbackBpm = ((unsigned)uiSong->bpm * 100U + NOTE_LENGTH_PERCENT / 2)
                              / NOTE_LENGTH_PERCENT;
  snprintf(buf, sizeof(buf), "%u BPM", playbackBpm);
  oledText(0, 38, buf);
  snprintf(buf, sizeof(buf), "%u:%02u / %u:%02u",
           (unsigned)(el / 60000), (unsigned)((el / 1000) % 60),
           (unsigned)(uiTotalMs / 60000), (unsigned)((uiTotalMs / 1000) % 60));
  oledText(128 - oledTextWidth(buf, 1), 38, buf);

  // 3. Current note + frequency (left) | note counter (right) at y=47
  noteLabel(uiFreq, buf, sizeof(buf));
  oledText(0, 47, buf);
  snprintf(buf, sizeof(buf), "%u/%u", (unsigned)(uiNoteIdx + 1), (unsigned)uiSong->length);
  oledText(128 - oledTextWidth(buf, 1), 47, buf);
}

void uiInitTicker() {
  const SongInfo &info = songInfo[currentSongIndex];
  snprintf(tickerText, sizeof(tickerText), "%s | %s - %s",
           info.composer, info.period, info.fact);
  tickerOffset = 0;
  lastTickerMs = millis();
  oledFillRect(0, 56, 128, 8, false);
  oledText(0, 56, tickerText, 1);
}

// Faster continuous marquee on the bottom row (page 7, y=56..62)
void uiUpdateDetailsTicker() {
  if (!oledOK || !uiSong) return;
  const unsigned long now = millis();
  if (now - lastTickerMs < 40) return; // Faster scroll (was 75 ms)
  lastTickerMs = now;

  const int textWidth = oledTextWidth(tickerText, 1);
  if (textWidth <= 128) return;

  tickerOffset++;
  if (tickerOffset > textWidth + 28) tickerOffset = 0;

  oledFillRect(0, 56, 128, 8, false);
  oledText(-tickerOffset, 56, tickerText, 1);
  // y=56..62 sits cleanly inside OLED page 7, so only 1 page needs flushing
  oledFlush(7, 1);
}

// Full redraw + full send (called once at the start of each song)
void uiDrawSong(const Song &s) {
  if (!oledOK) return;
  oledFillRect(0, 0, 128, 64, false);
  uiDrawHeader();
  uiDrawTitle(s.title);
  uiDrawLive();
  uiInitTicker();
  oledFlush(0, 8);
  lastUiMs = millis();
}

// Header only (volume / shuffle / pause changes)
void uiRefreshHeader() {
  if (!oledOK) return;
  uiDrawHeader();
  oledFlush(0, 2);
}

// Live info only, rate-limited (pages 3..6 cover y=24..55)
void uiUpdateLive(unsigned long minIntervalMs) {
  if (!oledOK || !uiSong) return;
  const unsigned long now = millis();
  if (now - lastUiMs < minIntervalMs) return;
  lastUiMs = now;
  uiDrawLive();
  oledFlush(3, 4);
}

void uiSplash() {
  if (!oledOK) return;
  oledFillRect(0, 0, 128, 64, false);
  oledText((128 - oledTextWidth("BUZZER", 2)) / 2, 8, "BUZZER", 2);
  oledText((128 - oledTextWidth("JUKEBOX", 2)) / 2, 28, "JUKEBOX", 2);
  char buf[24];
  snprintf(buf, sizeof(buf), "%d songs ready", TOTAL_SONGS);
  oledText((128 - oledTextWidth(buf, 1)) / 2, 52, buf);
  oledFlush(0, 8);
}

// ------------------------------------------
// Low-level audio helpers
// ------------------------------------------
inline void silence() { ledcWrite(BUZZER_PIN, 0); }

// Only touches the PWM timer when the pitch actually changes.
void setTone(uint32_t f) {
  if (f != activeFreq) {
    ledcChangeFrequency(BUZZER_PIN, f, PWM_BITS);
    activeFreq = f;
  }
}

// Short blocking beep for UI feedback.
void blip(uint32_t f, int ms) {
  setTone(f);
  ledcWrite(BUZZER_PIN, VOLUME_LEVELS[volumeIndex]);
  delay(ms);
  silence();
}

void cycleVolume() {
  volumeIndex = (volumeIndex + 1) % VOLUME_STEPS;
  Serial.printf("Volume: %u/%u\n", volumeIndex + 1, VOLUME_STEPS);
  uiRefreshHeader();
  blip(600 + volumeIndex * 150, 70);
}

void toggleShuffle() {
  shuffleOn = !shuffleOn;
  historyCount = 0;
  Serial.println(shuffleOn ? "Shuffle: ON" : "Shuffle: OFF");
  uiRefreshHeader();
  if (shuffleOn) { blip(NOTE_G5, 60); delay(30); blip(NOTE_C6, 80); }
  else           { blip(NOTE_C6, 60); delay(30); blip(NOTE_G5, 80); }
}

// ------------------------------------------
// Input polling: skips are returned, volume/shuffle are handled in place
// ------------------------------------------
NavAction pollInput() {
  ButtonEvent e = updateButton(pauseBtn, EV_PAUSE, EV_NONE);
  if (e == EV_PAUSE) {
    playbackPaused = !playbackPaused;
    if (playbackPaused) silence();
    uiRefreshHeader();
    Serial.println(playbackPaused ? "Playback: PAUSED" : "Playback: RESUMED");
  }
  if (e == EV_NONE) e = updateButton(prevBtn, EV_PREV_SHORT, EV_PREV_LONG);
  if (e == EV_NONE) e = updateButton(nextBtn, EV_NEXT_SHORT, EV_NEXT_LONG);

  switch (e) {
    case EV_PREV_SHORT: return NAV_PREV;
    case EV_NEXT_SHORT: return NAV_NEXT;
    case EV_NEXT_LONG:  cycleVolume();   break;
    case EV_PREV_LONG:  toggleShuffle(); break;
    default: break;
  }
  return NAV_NONE;
}

// Interruptible delay
// Freeze a note/rest timer while paused. Navigation buttons remain active.
NavAction waitWhilePaused(unsigned long &timerStart) {
  if (!playbackPaused) return NAV_NONE;
  silence();
  const unsigned long pauseStart = millis();
  while (playbackPaused) {
    NavAction a = pollInput();
    if (a != NAV_NONE) return a;
    uiUpdateDetailsTicker();
    delay(2);
  }
  timerStart += millis() - pauseStart;  // exclude paused time from playback timing
  return NAV_NONE;
}

NavAction smartDelay(unsigned long ms) {
  unsigned long start = millis();
  while (millis() - start < ms) {
    NavAction a = pollInput();
    if (a != NAV_NONE) return a;
    uiUpdateDetailsTicker();
    a = waitWhilePaused(start);
    if (a != NAV_NONE) return a;
    delay(2);
  }
  return NAV_NONE;
}

// ------------------------------------------
// Smooth envelope note player (interruptible)
// ------------------------------------------
NavAction playSmoothNote(int freq, int durationMs) {
  if (freq == REST || durationMs <= 0) {
    silence();
    return smartDelay(durationMs > 0 ? durationMs : 0);
  }

  const int attack  = min(ATTACK_MS,  durationMs / 3);
  const int release = min(RELEASE_MS, durationMs / 2);

  setTone(freq);
  silence();

  unsigned long start = millis();
  unsigned long lastFreqUpdate = 0;
  unsigned long elapsed;

  while ((elapsed = millis() - start) < (unsigned long)durationMs) {
    NavAction a = pollInput();
    if (a != NAV_NONE) { silence(); return a; }
    a = waitWhilePaused(start);
    if (a != NAV_NONE) { silence(); return a; }
    elapsed = millis() - start;  // recalculate after any pause

    const int vol = VOLUME_LEVELS[volumeIndex];
    int duty = vol;
    if (elapsed < (unsigned long)attack) {
      duty = map(elapsed, 0, attack, 0, vol);
    } else if (elapsed > (unsigned long)(durationMs - release)) {
      duty = map(elapsed, durationMs - release, durationMs, vol, 0);
    }

    // Pitch: base note (+ gentle, fading-in vibrato). Rate-limited to 8 ms.
    if (elapsed - lastFreqUpdate >= 8 || lastFreqUpdate == 0) {
      lastFreqUpdate = elapsed ? elapsed : 1;
      int f = freq;
      if (USE_VIBRATO && durationMs > VIBRATO_MIN_NOTE_MS && elapsed > (unsigned long)VIBRATO_DELAY_MS) {
        float ramp = min(1.0f, (elapsed - VIBRATO_DELAY_MS) / 200.0f);
        f += (int)lroundf(sinf(elapsed * VIBRATO_RAD_PER_MS) * freq * VIBRATO_DEPTH * ramp);
      }
      setTone(f);   // also restores pitch if a UI blip changed it
    }

    ledcWrite(BUZZER_PIN, constrain(duty, 0, 128));

    // Long notes: keep the clock/progress bar moving, only during the sustain
    if (durationMs > 400 && elapsed > (unsigned long)(attack + 20) &&
        elapsed + release + 30 < (unsigned long)durationMs) {
      uiUpdateLive(250);
    }
    uiUpdateDetailsTicker();
    delay(2);
  }

  silence();
  return NAV_NONE;
}

// ------------------------------------------
// Melody sequencer
// ------------------------------------------
NavAction playSong(const Song &s) {
  saveLastSong();
  Serial.printf("Ready [%d/%d]: %s%s\n", currentSongIndex + 1, TOTAL_SONGS,
                s.title, shuffleOn ? "  (shuffle)" : "");

  const int wholeNoteMs = 240000 / s.bpm;
  const int lastPitched = lastPitchedNoteIndex(s);

  uiSong    = &s;
  uiTotalMs = songDurationMs(s);
  uiNoteIdx = 0;
  uiFreq    = 0;
  uiDrawSong(s);                 // ~25 ms full redraw, before the clock starts
  songStartMs = millis();

  // On boot the jukebox deliberately waits here in its paused state.
  // The pause button resumes; Prev/Next remain responsive while waiting.
  unsigned long pauseClock = millis();
  NavAction startupAction = waitWhilePaused(pauseClock);
  if (startupAction != NAV_NONE) return startupAction;
  songStartMs += millis() - pauseClock;

  // A complete arrangement is played more than once so short tunes feel like
  // proper performances. All waits remain interruptible by the existing buttons.
  for (uint8_t pass = 0; pass < SONG_PLAYTHROUGHS; ++pass) {
    for (uint16_t i = 0; i < s.length; i++) {
      if (s.notes[i].d == 0) continue;
      int noteMs = scaledNoteDurationMs(s, i, wholeNoteMs, lastPitched);

      // Update the display at each note start, and take the I2C time out of
      // this note so the overall tempo stays accurate.
      const unsigned long t0 = millis();
      uiNoteIdx = i;
      uiFreq    = s.notes[i].f;
      uiUpdateLive(100);
      const int spent = (int)(millis() - t0);
      noteMs = max(noteMs - spent, 10);

      const int playMs = noteMs * 92 / 100;
      const int gapMs  = noteMs - playMs;

      NavAction a = playSmoothNote(s.notes[i].f, playMs);
      if (a != NAV_NONE) return a;

      a = smartDelay(gapMs);
      if (a != NAV_NONE) return a;
    }

    if (pass + 1 < SONG_PLAYTHROUGHS) {
      NavAction a = smartDelay(REPEAT_BREAK_MS);
      if (a != NAV_NONE) return a;
    }
  }

  // A short, responsive tail keeps the ending from feeling abrupt.
  return smartDelay(ENDING_SILENCE_MS);
}

// ------------------------------------------
// Playlist navigation
// ------------------------------------------
void pushHistory(int idx) {
  if (historyCount == HISTORY_SIZE) {
    memmove(historyStack, historyStack + 1, (HISTORY_SIZE - 1) * sizeof(int));
    historyCount--;
  }
  historyStack[historyCount++] = idx;
}

int randomOtherSong() {
  if (TOTAL_SONGS < 2) return currentSongIndex;
  int n;
  do { n = esp_random() % TOTAL_SONGS; } while (n == currentSongIndex);
  return n;
}

void goNext() {
  if (shuffleOn) {
    pushHistory(currentSongIndex);
    currentSongIndex = randomOtherSong();
  } else {
    currentSongIndex = (currentSongIndex + 1) % TOTAL_SONGS;
  }
  saveLastSong();
}

void goPrevious() {
  if (shuffleOn) {
    currentSongIndex = (historyCount > 0) ? historyStack[--historyCount]
                                          : randomOtherSong();
  } else {
    currentSongIndex = (currentSongIndex - 1 + TOTAL_SONGS) % TOTAL_SONGS;
  }
  saveLastSong();
}

// ------------------------------------------
// Arduino entry points
// ------------------------------------------
void setup() {
  Serial.begin(115200);

  jukeboxPrefs.begin(PREF_NAMESPACE, false);
  currentSongIndex = jukeboxPrefs.getInt(PREF_LAST_SONG, 0);
  if (currentSongIndex < 0 || currentSongIndex >= TOTAL_SONGS) currentSongIndex = 0;
  playbackPaused = true;  // Always boot silently and wait for Continue.

  pinMode(PREV_BTN_PIN,  INPUT_PULLUP);
  pinMode(NEXT_BTN_PIN,  INPUT_PULLUP);
  pinMode(PAUSE_BTN_PIN, INPUT_PULLUP);

  ledcAttach(BUZZER_PIN, PWM_BASE_FREQ, PWM_BITS);
  activeFreq = PWM_BASE_FREQ;
  silence();

  // OLED on GPIO 8 (SDA) / 9 (SCL)
  Wire.begin(OLED_SDA_PIN, OLED_SCL_PIN);
  Wire.setClock(OLED_I2C_HZ);
  Wire.beginTransmission(OLED_ADDR);
  oledOK = (Wire.endTransmission() == 0);
  if (oledOK) oledInit();
  Serial.println(oledOK ? "OLED found" : "OLED NOT found - running without display");
  uiSplash();

  // No startup chime: boot always remains paused until the user presses GPIO 3.
}

void loop() {
  NavAction action = playSong(playlist[currentSongIndex]);

  if (action == NAV_NONE) {
    action = smartDelay(SONG_GAP_MS);   // gap between songs, still listening
  }

  if (action == NAV_PREV) {
    if (millis() - songStartMs > RESTART_WINDOW_MS) {
      Serial.println("<- Restart song");        // same index, plays again
    } else {
      Serial.println("<- Previous song");
      goPrevious();
    }
  } else {
    if (action == NAV_NEXT) Serial.println("-> Next song");
    goNext();                                    // NAV_NEXT or natural end
  }
}
    