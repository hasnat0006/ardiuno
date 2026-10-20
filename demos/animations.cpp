#include <Arduino.h>
#include <Wire.h>
#include <U8g2lib.h>

// 1.3" 128x64 I2C OLED (SH1106). If the image is shifted or garbled, use:
//   U8G2_SSD1306_128X64_NONAME_F_HW_I2C display(U8G2_R0, U8X8_PIN_NONE);
//
// Wiring (Arduino Uno / Nano): VCC->5V, GND->GND, SDA->A4, SCL->A5
U8G2_SH1106_128X64_NONAME_F_HW_I2C display(U8G2_R0, U8X8_PIN_NONE);

const unsigned long EFFECT_MS = 8000;  // how long each effect runs

// ---------------------------------------------------------------------------
// Fast integer sine: angle 0..255 is one full turn, result is -127..127
// ---------------------------------------------------------------------------
const int8_t QUARTER_SINE[65] PROGMEM = {
  0, 3, 6, 9, 12, 16, 19, 22, 25, 28, 31, 34, 37, 40, 43, 46, 49, 51, 54,
  57, 60, 63, 65, 68, 71, 73, 76, 78, 81, 83, 85, 88, 90, 92, 94, 96, 98,
  100, 102, 104, 106, 107, 109, 111, 112, 113, 115, 116, 117, 118, 120, 121,
  122, 122, 123, 124, 125, 125, 126, 126, 126, 127, 127, 127, 127};

int8_t isin(uint8_t a) {
  uint8_t i = a & 63;
  int8_t v = pgm_read_byte(&QUARTER_SINE[(a & 64) ? 64 - i : i]);
  return (a & 128) ? -v : v;
}
int8_t icos(uint8_t a) { return isin(a + 64); }

// ---------------------------------------------------------------------------
// Effect state. Only one effect runs at a time, so they share memory
// (the Uno only has 2 KB of RAM and the screen buffer takes 1 KB of it).
// ---------------------------------------------------------------------------
const uint8_t NUM_STARS = 48;
const uint8_t NUM_SPARKS = 30;
const uint8_t RAIN_COLS = 21, RAIN_ROWS = 8, RAIN_TRAIL = 6;

union {
  struct { int8_t x, y; uint8_t z; } stars[NUM_STARS];
  struct { int8_t col[128]; int8_t row[64]; } plasma;
  struct { int16_t x, y; int8_t vx, vy; uint8_t life; } sparks[NUM_SPARKS];
  struct { int8_t head; uint8_t speed; } drops[RAIN_COLS];
} fx;

uint8_t nextSpark = 0;
uint8_t contrast = 255;

// ---------------------------------------------------------------------------
// 1. Warp-speed starfield
// ---------------------------------------------------------------------------
void respawnStar(uint8_t i, uint8_t z) {
  fx.stars[i].x = random(-127, 128);
  fx.stars[i].y = random(-63, 64);
  fx.stars[i].z = z;
}

void initStars() {
  for (uint8_t i = 0; i < NUM_STARS; i++) respawnStar(i, random(1, 256));
}

void drawStars(unsigned long t) {
  uint8_t speed = 2 + t / 1000;  // keeps accelerating into hyperspace
  for (uint8_t i = 0; i < NUM_STARS; i++) {
    auto &s = fx.stars[i];
    if (s.z <= speed) { respawnStar(i, 255); continue; }
    s.z -= speed;

    int16_t sx = 64 + (int16_t)s.x * 32 / s.z;
    int16_t sy = 32 + (int16_t)s.y * 32 / s.z;
    if (sx < 0 || sx > 127 || sy < 0 || sy > 63) { respawnStar(i, 255); continue; }

    // Streak from where the star was a moment ago
    uint8_t tz = min(255, s.z + speed * 4);
    int16_t tx = 64 + (int16_t)s.x * 32 / tz;
    int16_t ty = 32 + (int16_t)s.y * 32 / tz;
    display.drawLine(tx, ty, sx, sy);
  }
}

// ---------------------------------------------------------------------------
// 2. Plasma, dithered to fake 16 shades of grey
// ---------------------------------------------------------------------------
const uint8_t BAYER[4][4] PROGMEM = {
  {0, 8, 2, 10}, {12, 4, 14, 6}, {3, 11, 1, 9}, {15, 7, 13, 5}};

void drawPlasma(uint16_t f) {
  uint8_t t = f * 3;
  for (uint8_t x = 0; x < 128; x++) fx.plasma.col[x] = isin(x * 3 + t);
  for (uint8_t y = 0; y < 64; y++) fx.plasma.row[y] = isin(y * 6 - t * 2);

  // Write straight into the screen buffer: each byte is 8 vertical pixels
  uint8_t *buf = display.getBufferPtr();
  for (uint8_t page = 0; page < 8; page++) {
    for (uint8_t x = 0; x < 128; x++) {
      uint8_t bits = 0;
      for (uint8_t b = 0; b < 8; b++) {
        uint8_t y = page * 8 + b;
        int16_t v = fx.plasma.col[x] + fx.plasma.row[y] +
                    isin((x + y) * 2 + t) + isin(((x * y) >> 5) - t);
        uint8_t level = (v + 512) >> 6;  // 0..15
        if (level > pgm_read_byte(&BAYER[y & 3][x & 3])) bits |= 1 << b;
      }
      *buf++ = bits;
    }
  }
}

// ---------------------------------------------------------------------------
// 3. Two spinning 3D cubes, one inside the other
// ---------------------------------------------------------------------------
const int8_t CUBE_V[8][3] PROGMEM = {
  {-1, -1, -1}, {1, -1, -1}, {1, 1, -1}, {-1, 1, -1},
  {-1, -1, 1},  {1, -1, 1},  {1, 1, 1},  {-1, 1, 1}};
const uint8_t CUBE_E[12][2] PROGMEM = {
  {0, 1}, {1, 2}, {2, 3}, {3, 0}, {4, 5}, {5, 6},
  {6, 7}, {7, 4}, {0, 4}, {1, 5}, {2, 6}, {3, 7}};

void drawCube(float ax, float ay, float az, float size, bool dots) {
  float sx = sin(ax), cx = cos(ax);
  float sy = sin(ay), cy = cos(ay);
  float sz = sin(az), cz = cos(az);
  int16_t px[8], py[8];

  for (uint8_t i = 0; i < 8; i++) {
    float x = (int8_t)pgm_read_byte(&CUBE_V[i][0]);
    float y = (int8_t)pgm_read_byte(&CUBE_V[i][1]);
    float z = (int8_t)pgm_read_byte(&CUBE_V[i][2]);
    float y1 = y * cx - z * sx, z1 = y * sx + z * cx;    // rotate X
    float x2 = x * cy + z1 * sy, z2 = -x * sy + z1 * cy;  // rotate Y
    float x3 = x2 * cz - y1 * sz, y3 = x2 * sz + y1 * cz; // rotate Z
    float persp = size / (z2 + 4);
    px[i] = 64 + x3 * persp;
    py[i] = 32 + y3 * persp;
  }

  for (uint8_t e = 0; e < 12; e++) {
    uint8_t a = pgm_read_byte(&CUBE_E[e][0]), b = pgm_read_byte(&CUBE_E[e][1]);
    display.drawLine(px[a], py[a], px[b], py[b]);
  }
  if (dots)
    for (uint8_t i = 0; i < 8; i++) display.drawDisc(px[i], py[i], 2);
}

void drawCubes(uint16_t f) {
  float a = f * 0.06;
  float size = 30 + isin(f * 4) / 16;  // pulsing, 22..38 keeps it on screen
  drawCube(a, a * 0.7, a * 0.3, size, true);
  drawCube(-a * 1.3, a, -a * 0.5, size * 0.45, false);
}

// ---------------------------------------------------------------------------
// 4. Fireworks with brightness flashes
// ---------------------------------------------------------------------------
void initSparks() {
  for (uint8_t i = 0; i < NUM_SPARKS; i++) fx.sparks[i].life = 0;
}

void burst() {
  int16_t cx = random(24, 104) * 16, cy = random(10, 40) * 16;  // 1/16 px units
  for (uint8_t i = 0; i < 15; i++) {
    auto &p = fx.sparks[nextSpark];
    nextSpark = (nextSpark + 1) % NUM_SPARKS;
    uint8_t angle = i * 17 + random(8);
    uint8_t speed = random(10, 36);
    p.x = cx;
    p.y = cy;
    p.vx = (int16_t)icos(angle) * speed / 127;
    p.vy = (int16_t)isin(angle) * speed / 127 - 6;
    p.life = random(30, 60);
  }
  contrast = 255;  // flash on every explosion
}

void drawFireworks(uint16_t f) {
  if (f % 20 == 0) burst();

  for (uint8_t i = 0; i < NUM_SPARKS; i++) {
    auto &p = fx.sparks[i];
    if (p.life == 0) continue;
    p.x += p.vx;
    p.y += p.vy;
    if ((f & 1) && p.vy < 60) p.vy++;  // gravity
    p.life--;

    int16_t sx = p.x >> 4, sy = p.y >> 4;
    if (sx < 0 || sx > 126 || sy < 0 || sy > 62) { p.life = 0; continue; }
    if (p.life > 40) display.drawBox(sx, sy, 2, 2);
    else if (p.life > 15 || ((f + i) & 1)) display.drawPixel(sx, sy);  // dying sparks flicker
  }

  if (contrast > 60) contrast -= 10;
  display.setContrast(contrast);
}

// ---------------------------------------------------------------------------
// 5. Matrix rain
// ---------------------------------------------------------------------------
void initRain() {
  for (uint8_t c = 0; c < RAIN_COLS; c++) {
    fx.drops[c].head = -(int8_t)random(0, 12);
    fx.drops[c].speed = random(1, 4);
  }
}

void drawRain(uint16_t f) {
  display.setFont(u8g2_font_5x7_tf);
  for (uint8_t c = 0; c < RAIN_COLS; c++) {
    auto &d = fx.drops[c];
    if (f % d.speed == 0) d.head++;
    if (d.head - RAIN_TRAIL >= RAIN_ROWS) {
      d.head = -(int8_t)random(0, 6);
      d.speed = random(1, 4);
    }

    for (int8_t r = d.head - RAIN_TRAIL + 1; r <= d.head; r++) {
      if (r < 0 || r >= RAIN_ROWS) continue;
      uint8_t age = d.head - r;
      // Older characters show less often, so the trail looks like it fades
      if (age > 2 && (f + c + r) % (age - 1) != 0) continue;

      uint8_t ch = 33 + (c * 7 + r * 13 + (age == 0 ? f : f / 8) * 11) % 94;
      uint8_t x = c * 6, y = r * 8;
      if (age == 0) {  // bright inverted head
        display.drawBox(x, y, 6, 8);
        display.setDrawColor(0);
        display.drawGlyph(x, y + 7, ch);
        display.setDrawColor(1);
      } else {
        display.drawGlyph(x, y + 7, ch);
      }
    }
  }
}

// ---------------------------------------------------------------------------
// 6. Hypnotic tunnel with breathing brightness
// ---------------------------------------------------------------------------
void drawTunnel(uint16_t f) {
  // Biggest ring first, alternating white/black discs make the bands
  for (int8_t i = 7; i >= 0; i--) {
    uint8_t r = i * 10 + f % 10;
    if (r < 2) continue;
    uint8_t depth = 80 - r;  // small rings are "far away" and swing more
    int16_t cx = 64 + (int16_t)isin(f * 2 + r) * depth / 160;
    int16_t cy = 32 + (int16_t)icos(f * 3 + r) * depth / 320;
    display.setDrawColor((i + f / 10) & 1);
    display.drawDisc(cx, cy, r);
  }
  display.setDrawColor(1);
  display.setContrast(128 + isin(f * 4));
}

// ---------------------------------------------------------------------------
// Effect sequencing
// ---------------------------------------------------------------------------
enum Effect { STARS, PLASMA, CUBES, FIREWORKS, RAIN, TUNNEL, NUM_EFFECTS };
const char *const NAMES[NUM_EFFECTS] = {
  "WARP", "PLASMA", "CUBES", "BOOM!", "MATRIX", "TUNNEL"};

uint8_t effect = 0;
unsigned long effectStart = 0;
uint16_t frame = 0;

void showTitle(const char *name) {
  display.setContrast(255);
  for (uint8_t i = 0; i < 4; i++) {  // strobe
    display.clearBuffer();
    if (i & 1) display.drawBox(0, 0, 128, 64);
    display.sendBuffer();
    delay(60);
  }
  display.clearBuffer();
  display.drawBox(0, 0, 128, 64);
  display.setDrawColor(0);
  display.setFont(u8g2_font_logisoso22_tr);
  display.drawStr((128 - display.getStrWidth(name)) / 2, 43, name);
  display.setDrawColor(1);
  display.sendBuffer();
  delay(700);
}

void startEffect(uint8_t e) {
  effect = e;
  showTitle(NAMES[e]);
  if (e == STARS) initStars();
  if (e == FIREWORKS) initSparks();
  if (e == RAIN) initRain();
  frame = 0;
  effectStart = millis();
}

void setup() {
  randomSeed(analogRead(A0));

  // Most modules use address 0x3C. U8g2 expects the 8-bit form (0x3C << 1).
  // Change to 0x3D if your board has the address jumper set.
  display.setI2CAddress(0x3C << 1);
  display.begin();
  display.setFontMode(1);  // transparent text, needed for inverted text

  startEffect(STARS);
}

void loop() {
  if (millis() - effectStart > EFFECT_MS) startEffect((effect + 1) % NUM_EFFECTS);

  display.clearBuffer();
  switch (effect) {
    case STARS:     drawStars(millis() - effectStart); break;
    case PLASMA:    drawPlasma(frame); break;
    case CUBES:     drawCubes(frame); break;
    case FIREWORKS: drawFireworks(frame); break;
    case RAIN:      drawRain(frame); break;
    case TUNNEL:    drawTunnel(frame); break;
  }
  display.sendBuffer();
  frame++;
}
