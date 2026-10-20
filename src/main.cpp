#include <Arduino.h>
#include <Wire.h>
#include <U8g2lib.h>

// OLED desk companion: big animated clock, expressive eyes, and a Claude mode.
//
// tools/pc_link.py on the PC sends:
//   "T<unix seconds>"                                   local time, every 10 s
//   "C<working>,<5h %>,<week %>,<5h reset min>,<week reset min>"   every 2 s
//   (<working> = Claude Code sessions busy on a reply right now)
//
// Claude not working:  eyes -> time -> eyes -> time ...
// Claude working:      Claude animation -> usage -> Claude animation -> time ...
//
// Wiring (Arduino Uno / Nano):
//   OLED:  VCC->5V, GND->GND, SDA->A4, SCL->A5
//
// If the image is shifted or garbled, use the SSD1306 constructor instead:
//   U8G2_SSD1306_128X64_NONAME_2_HW_I2C display(U8G2_R0, U8X8_PIN_NONE);
//
// Page mode ("_2"): the screen is drawn in 4 strips through a 256-byte buffer
// instead of a full 1 KB one, which leaves the Uno enough RAM to run safely.
// So each frame's logic runs once in update(), and render() only draws.
U8G2_SH1106_128X64_NONAME_2_HW_I2C display(U8G2_R0, U8X8_PIN_NONE);

// 400 kHz gives smooth animation. If the screen flickers or blanks out
// (loose wiring), drop this to 100000.
const uint32_t I2C_CLOCK = 400000;

const uint16_t ROLL_MS = 450;          // digit roll animation length
const uint16_t LID_MS = 300;           // screen "blink" transition length
const uint16_t CLOCK_MIN_S = 10;       // show the time for 10 seconds
const uint16_t CLOCK_MAX_S = 10;
const uint16_t EYES_MIN_S = 10;        // eye animations for 10..20 seconds
const uint16_t EYES_MAX_S = 20;
const uint16_t CLAUDE_MS = 6000;       // one Claude animation
const uint16_t USAGE_MS = 5000;        // the usage report page
const uint16_t CLAUDE_TIMEOUT_MS = 15000;  // no Claude status for this long = not working

#define TIME_FONT u8g2_font_logisoso32_tn
#define MID_FONT u8g2_font_logisoso16_tr   // seconds, usage numbers, shimmer word
#define SMALL_FONT u8g2_font_5x7_tr

float clamp01(float v) { return v < 0 ? 0 : v > 1 ? 1 : v; }
int16_t rnd(float v) { return v < 0 ? v - 0.5 : v + 0.5; }
float easeOut(float p) { return 1 - (1 - p) * (1 - p); }

// ---------------------------------------------------------------------------
// Timekeeping
// ---------------------------------------------------------------------------
uint32_t syncUnix = 0;        // local time (seconds since 1970) at last sync
unsigned long syncMillis = 0; // millis() at last sync
bool synced = false;

// millis() captured once per frame, so every strip of a frame shows the same moment
unsigned long frameNow = 0;

uint32_t nowUnix() { return syncUnix + (frameNow - syncMillis) / 1000; }
uint16_t nowMs() { return (frameNow - syncMillis) % 1000; }

struct Date { uint16_t year; uint8_t month, day, weekday; };

Date toDate(uint32_t t) {
  int32_t days = t / 86400;
  Date d;
  d.weekday = (days + 4) % 7;  // 1970-01-01 was a Thursday; 0 = Sunday

  // Days since 1970 -> calendar date (Howard Hinnant's algorithm)
  int32_t z = days + 719468;
  int32_t era = z / 146097;
  uint32_t doe = z - era * 146097;
  uint32_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
  uint32_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
  uint32_t mp = (5 * doy + 2) / 153;
  d.day = doy - (153 * mp + 2) / 5 + 1;
  d.month = mp < 10 ? mp + 3 : mp - 9;
  d.year = yoe + era * 400 + (d.month <= 2);
  return d;
}

// ---------------------------------------------------------------------------
// Serial from the PC
// ---------------------------------------------------------------------------
uint8_t claudeBusy = 0;  // Claude Code sessions busy on a reply
uint8_t fivePct = 0xFF, weekPct = 0xFF;  // 0xFF = not known yet
uint16_t fiveResetMin = 0, weekResetMin = 0;
unsigned long claudeMsgAt = 0;

bool claudeWorking() { return claudeBusy > 0 && millis() - claudeMsgAt < CLAUDE_TIMEOUT_MS; }

char line[24];
uint8_t lineLen = 0;

// Reads digits up to the next comma (or the end) and moves past it
uint32_t nextNumber(char *&p) {
  uint32_t v = 0;
  while (*p >= '0' && *p <= '9') v = v * 10 + (*p++ - '0');
  if (*p == ',') p++;
  return v;
}

void handleLine() {
  char *p = line + 1;
  if (line[0] == 'T') {
    syncUnix = nextNumber(p);
    syncMillis = millis();
    synced = true;
  } else if (line[0] == 'C') {
    claudeBusy = nextNumber(p);
    fivePct = nextNumber(p);
    weekPct = nextNumber(p);
    fiveResetMin = nextNumber(p);
    weekResetMin = nextNumber(p);
    claudeMsgAt = millis();
  }
}

void readSerial() {
  while (Serial.available()) {
    char c = Serial.read();
    if (c == '\n') {
      line[lineLen] = 0;
      handleLine();
      lineLen = 0;
    } else if (c != '\r' && lineLen < sizeof(line) - 1) {
      line[lineLen++] = c;
    }
  }
}

// ---------------------------------------------------------------------------
// Clock page: big HH:MM with small seconds beside the minutes, all rolling,
// and "SAT 17 OCT 2026" cut out of a solid bar along the bottom
// ---------------------------------------------------------------------------
const uint8_t CLOCK_TOP = 6;  // y of the top of the big digits

const char DAY_NAMES[] PROGMEM = "SUNMONTUEWEDTHUFRISAT";
const char MONTH_NAMES[] PROGMEM = "JANFEBMARAPRMAYJUNJULAUGSEPOCTNOVDEC";

// Six digit slots: HH MM SS. 0xFF = blank, so digits roll in on first sync.
struct Slot { uint8_t val, prev; unsigned long changed; };
Slot slots[6] = {{0xFF, 0xFF, 0}, {0xFF, 0xFF, 0}, {0xFF, 0xFF, 0},
                 {0xFF, 0xFF, 0}, {0xFF, 0xFF, 0}, {0xFF, 0xFF, 0}};

void setSlot(uint8_t i, uint8_t v) {
  if (slots[i].val == v) return;
  slots[i].prev = slots[i].val;
  slots[i].val = v;
  slots[i].changed = frameNow;
}

void drawDigitAt(uint8_t x, int16_t baseline, uint8_t w, uint8_t v) {
  if (v > 9) return;
  char s[2] = {(char)('0' + v), 0};
  uint8_t sw = display.getStrWidth(s);  // "1" is wider than "0" in Logisoso
  display.drawStr(sw < w ? x + (w - sw) / 2 : x, baseline, s);
}

// Old digit slides up and out while the new one glides in from below
void drawSlot(uint8_t i, uint8_t x, uint8_t baseline, uint8_t w, uint8_t h) {
  Slot &s = slots[i];
  unsigned long t = frameNow - s.changed;
  // Clip only vertically: digits roll up and down, never sideways
  display.setClipWindow(0, baseline - h - 1, 128, baseline + 1);
  if (t >= ROLL_MS) {
    drawDigitAt(x, baseline, w, s.val);
  } else {
    float q = (float)t / ROLL_MS - 1;
    float eased = 1 + q * q * q;  // ease-out: fast start, gentle stop, no bounce
    int16_t dy = (h + 4) * eased;
    drawDigitAt(x, baseline - dy, w, s.prev);
    drawDigitAt(x, baseline + h + 4 - dy, w, s.val);
  }
  display.setMaxClipWindow();
}

// Layout, measured once in setup()
uint8_t bigW, bigH, secW, secH, colonW, clockX, secX, timeBase, secBase;

void measureClock() {
  display.setFont(MID_FONT);
  secW = display.getStrWidth("0");
  secH = display.getAscent();
  display.setFont(TIME_FONT);
  bigW = display.getStrWidth("0");
  bigH = display.getAscent();
  colonW = display.getStrWidth(":") + 2;

  // Centre HH:MM + seconds as one block; seconds top-aligned with the big digits
  uint8_t timeW = 4 * bigW + colonW;
  clockX = (128 - (timeW + 3 + 2 * secW)) / 2;
  timeBase = CLOCK_TOP + bigH;
  secX = clockX + timeW + 3;
  secBase = timeBase - bigH + secH;
}

// Per-frame clock state, worked out once in prepareClock()
bool colonOn, pm;
char dateText[16];
uint8_t dateX;

void prepareClock() {
  uint32_t t = nowUnix();
  uint8_t h24 = (t / 3600) % 24, m = (t / 60) % 60, s = t % 60;
  pm = h24 >= 12;
  uint8_t h = h24 % 12 ? h24 % 12 : 12;  // 12-hour clock: 12, 1, 2 ... 11
  setSlot(0, h >= 10 ? 1 : 0xFF);        // no leading zero: " 9:05", not "09:05"
  setSlot(1, h % 10);
  setSlot(2, m / 10);
  setSlot(3, m % 10);
  setSlot(4, s / 10);
  setSlot(5, s % 10);
  colonOn = nowMs() < 500;  // blinking colon

  // e.g. "SAT 17 OCT 2026"
  Date d = toDate(t);
  memcpy_P(dateText, DAY_NAMES + d.weekday * 3, 3);
  dateText[3] = ' ';
  utoa(d.day, dateText + 4, 10);
  strcat_P(dateText, PSTR(" "));
  char *mon = dateText + strlen(dateText);
  memcpy_P(mon, MONTH_NAMES + (d.month - 1) * 3, 3);
  mon[3] = ' ';
  utoa(d.year, mon + 4, 10);
  display.setFont(SMALL_FONT);
  dateX = (128 - display.getStrWidth(dateText)) / 2;
}

void drawClock() {
  // HH:MM, each digit rolls on its own when it changes
  display.setFont(TIME_FONT);
  uint8_t x = clockX;
  for (uint8_t i = 0; i < 4; i++) {
    drawSlot(i, x, timeBase, bigW, bigH);
    x += bigW;
    if (i == 1) {
      if (colonOn) display.drawStr(x + 1, timeBase, ":");
      x += colonW;
    }
  }

  // Seconds in the smaller font
  display.setFont(MID_FONT);
  drawSlot(4, secX, secBase, secW, secH);
  drawSlot(5, secX + secW, secBase, secW, secH);

  // AM / PM under the seconds, level with the bottom of the big digits
  display.setFont(SMALL_FONT);
  const char *ampm = pm ? "PM" : "AM";
  display.drawStr(secX + secW - display.getStrWidth(ampm) / 2, timeBase, ampm);

  // Date cut out of a solid bar along the bottom
  display.drawRBox(0, 52, 128, 11, 1);
  display.setDrawColor(0);
  display.setFont(SMALL_FONT);
  display.setFontPosTop();
  display.drawStr(dateX, 54, dateText);
  display.setFontPosBaseline();
  display.setDrawColor(1);
}

// ---------------------------------------------------------------------------
// Eyes: short "acts" made of keyframes the eyes glide between
// ---------------------------------------------------------------------------
enum Style : uint8_t {
  NORMAL, HAPPY, WINK_L, ROUND, ANGRY, X_EYES, STAR, CONFUSED, SLEEP, THINK, CROSS};
struct Key { int8_t x, y; uint8_t h; uint8_t style; uint16_t ms; };

const Key ACT_LOOK[] PROGMEM = {
  {0, 0, 28, NORMAL, 700}, {-14, 0, 28, NORMAL, 800}, {14, 0, 28, NORMAL, 800},
  {0, 0, 2, NORMAL, 120},  {0, 0, 28, NORMAL, 600},   {0, 0, 0, 0, 0}};
const Key ACT_HAPPY[] PROGMEM = {
  {0, 0, 28, NORMAL, 600}, {0, 0, 28, HAPPY, 1400}, {0, 0, 28, WINK_L, 500},
  {0, 0, 28, HAPPY, 900},  {0, 0, 0, 0, 0}};
const Key ACT_SLEEPY[] PROGMEM = {
  {0, 4, 14, NORMAL, 900}, {0, 6, 2, NORMAL, 700},  {0, 4, 12, NORMAL, 500},
  {0, 6, 2, NORMAL, 1300}, {0, -4, 30, NORMAL, 700}, {0, 0, 0, 0, 0}};
const Key ACT_SURPRISED[] PROGMEM = {
  {0, 0, 28, NORMAL, 500}, {0, -2, 32, ROUND, 1400}, {0, 0, 2, NORMAL, 120},
  {0, -2, 32, ROUND, 700}, {0, 0, 28, NORMAL, 500},  {0, 0, 0, 0, 0}};
const Key ACT_SUSPICIOUS[] PROGMEM = {
  {0, 0, 10, NORMAL, 500}, {-14, 2, 10, NORMAL, 1000}, {14, 2, 10, NORMAL, 1000},
  {0, 0, 10, NORMAL, 400}, {0, 0, 28, NORMAL, 600},    {0, 0, 0, 0, 0}};
const Key ACT_ANGRY[] PROGMEM = {
  {0, 0, 28, NORMAL, 400}, {0, 2, 24, ANGRY, 1500}, {-4, 2, 24, ANGRY, 150},
  {4, 2, 24, ANGRY, 150},  {-4, 2, 24, ANGRY, 150}, {4, 2, 24, ANGRY, 150},
  {0, 2, 24, ANGRY, 600},  {0, 0, 28, NORMAL, 500}, {0, 0, 0, 0, 0}};
const Key ACT_DIZZY[] PROGMEM = {
  {-12, 0, 28, NORMAL, 120}, {12, 0, 28, NORMAL, 120}, {-12, 0, 28, NORMAL, 120},
  {12, 0, 28, NORMAL, 120},  {-12, 0, 28, NORMAL, 120}, {12, 0, 28, NORMAL, 120},
  {0, 0, 28, X_EYES, 1800},  {0, 0, 2, NORMAL, 150},    {0, 0, 28, NORMAL, 500},
  {0, 0, 0, 0, 0}};
const Key ACT_STARS[] PROGMEM = {
  {0, 0, 28, NORMAL, 400}, {0, -2, 32, ROUND, 600}, {0, 0, 28, STAR, 2200},
  {0, 0, 28, HAPPY, 800},  {0, 0, 28, NORMAL, 400}, {0, 0, 0, 0, 0}};
const Key ACT_CONFUSED[] PROGMEM = {
  {0, 0, 28, NORMAL, 400}, {6, -2, 28, CONFUSED, 1800}, {-6, -2, 28, CONFUSED, 900},
  {0, 0, 28, NORMAL, 500}, {0, 0, 0, 0, 0}};
const Key ACT_THINK[] PROGMEM = {
  {0, 0, 28, NORMAL, 400}, {10, -7, 24, THINK, 2600}, {0, 0, 28, NORMAL, 200},
  {0, -2, 32, ROUND, 400}, {0, 0, 28, HAPPY, 900},    {0, 0, 28, NORMAL, 400},
  {0, 0, 0, 0, 0}};
const Key ACT_LAUGH[] PROGMEM = {
  {0, 0, 28, HAPPY, 300}, {0, -4, 28, HAPPY, 150}, {0, 2, 28, HAPPY, 150},
  {0, -4, 28, HAPPY, 150}, {0, 2, 28, HAPPY, 150}, {0, -4, 28, HAPPY, 150},
  {0, 2, 28, HAPPY, 150}, {0, -4, 28, HAPPY, 150}, {0, 2, 28, HAPPY, 300},
  {0, 0, 28, NORMAL, 500}, {0, 0, 0, 0, 0}};
const Key ACT_EYEROLL[] PROGMEM = {
  {0, 0, 28, NORMAL, 400}, {-12, -6, 20, NORMAL, 300}, {0, -8, 20, NORMAL, 300},
  {12, -6, 20, NORMAL, 300}, {0, 2, 12, NORMAL, 1000}, {0, 0, 28, NORMAL, 500},
  {0, 0, 0, 0, 0}};
const Key ACT_CROSS[] PROGMEM = {
  {0, 0, 28, NORMAL, 400}, {0, 0, 24, CROSS, 1600}, {0, 0, 2, NORMAL, 150},
  {0, 0, 28, NORMAL, 500}, {0, 0, 0, 0, 0}};
const Key ACT_NAP[] PROGMEM = {
  {0, 0, 28, NORMAL, 300}, {0, 4, 10, NORMAL, 700}, {0, 6, 3, SLEEP, 3200},
  {0, -4, 32, ROUND, 600}, {0, 0, 28, NORMAL, 500}, {0, 0, 0, 0, 0}};
const Key ACT_SCAN[] PROGMEM = {
  {-14, 0, 20, NORMAL, 250}, {14, 0, 20, NORMAL, 250}, {-14, 0, 20, NORMAL, 250},
  {14, 0, 20, NORMAL, 250},  {-14, 0, 20, NORMAL, 250}, {14, 0, 20, NORMAL, 250},
  {0, 0, 28, NORMAL, 500},   {0, 0, 0, 0, 0}};

const Key *const ACTS[] PROGMEM = {
  ACT_LOOK, ACT_HAPPY, ACT_SLEEPY, ACT_SURPRISED, ACT_SUSPICIOUS, ACT_ANGRY,
  ACT_DIZZY, ACT_STARS, ACT_CONFUSED, ACT_THINK, ACT_LAUGH, ACT_EYEROLL,
  ACT_CROSS, ACT_NAP, ACT_SCAN};
const uint8_t NUM_ACTS = sizeof(ACTS) / sizeof(ACTS[0]);

const Key *act;
uint8_t keyIdx;
Key key;
unsigned long keyStart;
int16_t eyeX, eyeY, eyeH;  // in 1/4 px, eased towards the current keyframe
uint8_t currentAct = 0xFF;

void loadKey() {
  memcpy_P(&key, &act[keyIdx], sizeof(Key));
  keyStart = millis();
}

void startAct() {
  uint8_t i;
  do i = random(NUM_ACTS); while (i == currentAct);  // never the same one twice in a row
  currentAct = i;
  act = (const Key *)pgm_read_ptr(&ACTS[i]);
  keyIdx = 0;
  loadKey();
}

// Advances through the keyframes; returns false when the act is over
bool updateAct() {
  if (millis() - keyStart >= key.ms) {
    keyIdx++;
    loadKey();
  }
  return key.ms != 0;
}

// Glide the eyes one step towards a target position and height (once per frame)
void easeEyes(int8_t tx, int8_t ty, uint8_t th) {
  eyeX += (tx * 4 - eyeX) / 3;
  eyeY += (ty * 4 - eyeY) / 3;
  eyeH += (th * 4 - eyeH) / 3;
}

void drawArcEye(uint8_t ex, uint8_t ey) {  // closed "^" eye
  for (uint8_t t = 0; t < 3; t++) {
    display.drawLine(ex - 15, ey + 4 + t, ex, ey - 5 + t);
    display.drawLine(ex, ey - 5 + t, ex + 15, ey + 4 + t);
  }
}

void drawZ(uint8_t x, uint8_t y, uint8_t s) {  // a "Z" drawn with lines, any size
  display.drawHLine(x, y, s);
  display.drawLine(x + s - 1, y, x, y + s - 1);
  display.drawHLine(x, y + s - 1, s);
}

void drawEyes(uint8_t style) {
  uint8_t h = eyeH / 4;
  unsigned long now = frameNow;

  for (uint8_t i = 0; i < 2; i++) {
    uint8_t ex = 64 + (i ? 24 : -24) + eyeX / 4;
    uint8_t ey = 30 + eyeY / 4;
    uint8_t eh = h;
    if (style == CROSS) ex += i ? -9 : 9;                  // both look inward
    if (style == CONFUSED && i == 1) { eh = h / 2; ey -= 4; }  // one squinting eye

    if (style == HAPPY || (style == WINK_L && i == 0)) {
      drawArcEye(ex, ey);
    } else if (style == ROUND) {
      uint8_t r = eh / 2;
      display.drawDisc(ex, ey, r);
      if (r >= 6) {  // shiny highlight
        display.setDrawColor(0);
        display.drawDisc(ex + r / 3, ey - r / 3, r / 4);
        display.setDrawColor(1);
      }
    } else if (style == X_EYES) {
      for (int8_t t = -1; t <= 1; t++) {
        display.drawLine(ex - 10 + t, ey - 10, ex + 10 + t, ey + 10);
        display.drawLine(ex - 10 + t, ey + 10, ex + 10 + t, ey - 10);
      }
    } else if (style == STAR) {
      uint8_t arm = 11 + (now / 90 + i * 2) % 5;  // twinkling 4-point star
      display.drawTriangle(ex - 4, ey, ex + 4, ey, ex, ey - arm);
      display.drawTriangle(ex - 4, ey, ex + 4, ey, ex, ey + arm);
      display.drawTriangle(ex, ey - 4, ex, ey + 4, ex - arm, ey);
      display.drawTriangle(ex, ey - 4, ex, ey + 4, ex + arm, ey);
    } else if (style == SLEEP) {
      display.drawBox(ex - 15, ey, 30, 3);
    } else {
      uint8_t top = ey - eh / 2;
      if (eh >= 12) display.drawRBox(ex - 15, top, 30, eh, 5);
      else if (eh >= 4) display.drawRBox(ex - 15, top, 30, eh, 1);
      else if (eh >= 1) display.drawBox(ex - 15, top, 30, eh);

      // Angry: eyelids slant down towards the nose
      if (style == ANGRY) {
        uint8_t cx = i == 0 ? ex + 16 : ex - 16;
        display.setDrawColor(0);
        display.drawTriangle(ex - 16, top - 1, ex + 16, top - 1, cx, top + eh / 2);
        display.setDrawColor(1);
      }
    }
  }

  // Extras drawn once over both eyes
  if (style == CONFUSED) {
    display.setFont(SMALL_FONT);
    display.drawGlyph(117, 12, '?');
  } else if (style == THINK) {
    for (uint8_t j = 0; j < (now / 400) % 4; j++) display.drawDisc(108 + j * 7, 4, 2);
  } else if (style == SLEEP) {
    for (uint8_t j = 0; j < 3; j++) {  // three Z's drifting up and growing
      uint8_t phase = (now / 40 + j * 33) % 100;
      drawZ(100 + phase / 6, 24 - phase / 5, 3 + phase / 20);
    }
  }
}

// ---------------------------------------------------------------------------
// Claude's spark and other shapes, built from tapered "rays"
// ---------------------------------------------------------------------------
const uint8_t NUM_RAYS = 12;

// Hand-drawn feel: each ray has a slightly uneven angle and length
const int8_t RAY_ANGLE_JITTER[NUM_RAYS] PROGMEM = {0, 6, -4, 3, -7, 2, 5, -3, 8, -5, 1, -2};  // degrees
const uint8_t RAY_LENGTH[NUM_RAYS] PROGMEM = {100, 82, 95, 78, 90, 100, 85, 93, 80, 97, 88, 84};  // percent

float rayAngle(uint8_t i, float rot) {
  return rot + (i * 30 + (int8_t)pgm_read_byte(&RAY_ANGLE_JITTER[i])) * 0.01745;
}
float rayLength(uint8_t i) { return pgm_read_byte(&RAY_LENGTH[i]) / 100.0; }

// A tapered shape from distance r0 to r1 out from the centre, half-width hw0 at
// the inner end and hw1 at the outer end, with an optional round tip.
// Worked out once per frame (prepare...) and drawn for every strip (drawRays).
struct Ray { int16_t bx, by, tx, ty; int8_t px, py, qx, qy; uint8_t tipR; };
const uint8_t MAX_RAYS = 16;
Ray rays[MAX_RAYS];
uint8_t rayCount;

void addRay(float cx, float cy, float a, float r0, float r1, float hw0, float hw1, uint8_t tipR) {
  if (rayCount >= MAX_RAYS) return;
  float dx = cos(a), dy = sin(a);
  Ray &r = rays[rayCount++];
  r.bx = rnd(cx + dx * r0);
  r.by = rnd(cy + dy * r0);
  r.tx = rnd(cx + dx * r1);
  r.ty = rnd(cy + dy * r1);
  r.px = rnd(-dy * hw0);
  r.py = rnd(dx * hw0);
  r.qx = rnd(-dy * hw1);
  r.qy = rnd(dx * hw1);
  r.tipR = tipR;
}

// One ray of Claude's spark; frac scales its length (for growing, breathing, waves)
void addSparkRay(uint8_t i, float cx, float cy, float r, float rot, float frac) {
  float len = r * rayLength(i) * frac;
  if (len < 2.5) return;
  float hw = r * 0.09 < 1.1 ? 1.1 : r * 0.09;
  addRay(cx, cy, rayAngle(i, rot), 1.5, len, hw, hw * 0.72, 1);
}

void drawRays() {
  for (uint8_t i = 0; i < rayCount; i++) {
    Ray &r = rays[i];
    if (r.bx != r.tx || r.by != r.ty) {
      display.drawTriangle(r.bx + r.px, r.by + r.py, r.bx - r.px, r.by - r.py, r.tx + r.qx, r.ty + r.qy);
      display.drawTriangle(r.bx - r.px, r.by - r.py, r.tx - r.qx, r.ty - r.qy, r.tx + r.qx, r.ty + r.qy);
    }
    if (r.tipR && r.tx >= 0 && r.tx < 128 && r.ty >= 0 && r.ty < 64) display.drawDisc(r.tx, r.ty, r.tipR);
  }
}

uint8_t hash8(uint16_t n) { return (uint32_t)(n * 2654435761UL) >> 24; }

// ---------------------------------------------------------------------------
// Claude animations (shown while Claude Code is working)
// ---------------------------------------------------------------------------
enum ClaudeAnim : uint8_t { SPINNER, WAVE, WRITING, SHIMMER, ASSEMBLE, NUM_CLAUDE_ANIMS };
uint8_t claudeAnim = 0xFF;
uint8_t claudeSeed;     // picks the verb and the "code" for this run
char claudeText[14];
uint8_t claudeTextX;
int16_t shimmerBand;

// Whimsical verbs, like Claude Code's spinner
const char VERBS[] PROGMEM =
  "THINKING\0PONDERING\0NOODLING\0CLAUDING\0BREWING\0MUSING\0COGITATING\0CONJURING\0";
const uint8_t NUM_VERBS = 8;

void loadVerb(uint8_t i, char *out) {
  const char *p = VERBS;
  while (i--) p += strlen_P(p) + 1;
  strcpy_P(out, p);
}

void startClaudeAnim() {
  uint8_t i;
  do i = random(NUM_CLAUDE_ANIMS); while (i == claudeAnim);  // never the same twice in a row
  claudeAnim = i;
  claudeSeed = random(256);
}

// Claude Code's thinking spinner shapes:  ·  ✢  ✳  ✶  ✻  ✽
const uint8_t SPINNER_SEQ[10] PROGMEM = {0, 1, 2, 3, 4, 5, 4, 3, 2, 1};
const uint8_t SPINNER_PETAL_W[6] PROGMEM = {0, 30, 13, 0, 30, 44};  // petal width, % of size

void addSpinnerGlyph(float cx, float cy, float s, uint8_t k, float rot) {
  if (k == 0) {  // ·
    addRay(cx, cy, 0, 0, 0, 0, 0, 3);
  } else if (k == 3) {  // ✶ six spikes
    for (uint8_t j = 0; j < 6; j++) addRay(cx, cy, rot + j * 1.0472, 0, s, s * 0.22, 0, 0);
  } else {  // ✢ ✳ ✻ ✽: 4 or 8 leaf-shaped petals, each made of two rays
    uint8_t n = k == 1 ? 4 : 8;
    float w = s * pgm_read_byte(&SPINNER_PETAL_W[k]) / 200.0;
    for (uint8_t j = 0; j < n; j++) {
      float a = rot + j * 6.2832 / n - 1.5708;
      addRay(cx, cy, a, 0, s * 0.5, 0, w, 0);
      addRay(cx, cy, a, s * 0.5, s, w, 0, 0);
    }
  }
}

void prepareClaude(long ms) {
  float t = ms / 1000.0;
  rayCount = 0;
  claudeText[0] = 0;

  switch (claudeAnim) {
    case SPINNER: {
      uint8_t k = pgm_read_byte(&SPINNER_SEQ[(ms / 143) % 10]);
      addSpinnerGlyph(64, 25, 21, k, t * 0.8);
      loadVerb(claudeSeed % NUM_VERBS, claudeText);
      for (uint8_t d = (ms / 400) % 4; d; d--) strcat_P(claudeText, PSTR("."));
      display.setFont(SMALL_FONT);
      claudeTextX = (128 - display.getStrWidth(claudeText)) / 2 + 9;
      break;
    }

    case WAVE: {  // a wave runs round the spark, each ray stretching in turn
      float phase = t * 4;
      for (uint8_t i = 0; i < NUM_RAYS; i++) {
        float d = cos(rayAngle(i, 0) - phase);
        addSparkRay(i, 64, 32, 27, t * 0.25, 0.5 + 0.5 * (d > 0 ? d * d * d : 0));
      }
      break;
    }

    case WRITING:  // small busy spark; the lines of "code" are drawn in drawCodeLines()
      for (uint8_t i = 0; i < NUM_RAYS; i++)
        addSparkRay(i, 7, 7, 6, t * 2.2, 0.8 + 0.2 * sin(t * 6 + i * 0.9));
      strcpy_P(claudeText, PSTR("WRITING"));
      break;

    case SHIMMER: {  // a big verb with a bright band sweeping across it
      for (uint8_t i = 0; i < NUM_RAYS; i++)
        addSparkRay(i, 64, 11, 9, t * 1.2, 0.85 + 0.15 * sin(t * 4 + i * 0.9));
      loadVerb((claudeSeed + 3) % NUM_VERBS, claudeText);
      display.setFont(MID_FONT);
      claudeTextX = (128 - display.getStrWidth(claudeText)) / 2;
      shimmerBand = -20 + (ms % 1800) * 170L / 1800;
      break;
    }

    case ASSEMBLE: {  // rays fly in, snap into the spark, spin, then burst out
      const float inT = 1.3, outT = CLAUDE_MS / 1000.0 - 1.1;
      float rot = t < outT ? t * 0.7 : outT * 0.7 + (t - outT) * 3;
      for (uint8_t i = 0; i < NUM_RAYS; i++) {
        float off;
        if (t < outT) off = 75 * (1 - easeOut(clamp01((t - i * 0.05) / inT)));
        else {
          float p = clamp01((t - outT - (11 - i) * 0.03) / 0.9);
          off = 75 * p * p;
        }
        float len = 26 * rayLength(i);
        addRay(64, 32, rayAngle(i, rot), 1.5 + off, len + off, 2.3, 1.7, 1);
      }
      break;
    }
  }
}

// Lines of "code" being typed out and scrolling up (the WRITING animation)
void drawCodeLines(long ms) {
  const uint8_t INDENTS[4] = {0, 6, 12, 6};
  int16_t scroll = ms > 2640 ? (ms - 2640) * 8 / 600 : 0;
  display.setClipWindow(0, 17, 128, 64);
  for (uint8_t k = 0; k < 20; k++) {
    int16_t y = 21 + k * 8 - scroll;
    if (y > 63) break;
    long lt = ms - (long)k * 600;
    if (y < 14 || lt <= 0) continue;
    uint8_t typed = lt >= 600 ? 255 : lt * 255 / 600;
    uint8_t indent = INDENTS[hash8(claudeSeed + k * 7) & 3];
    int16_t x = 3 + indent;
    int16_t reach = x + (40 + hash8(claudeSeed + k) * 70 / 255) * typed / 255;
    for (uint8_t wd = 0; x < reach && wd < 8; wd++) {
      uint8_t len = 4 + hash8(claudeSeed + k * 13 + wd) % 14;
      display.drawBox(x, y, len < reach - x ? len : reach - x, 3);
      x += len + 3;
    }
    if (typed < 255 && (ms / 166) % 2) display.drawBox((reach < 122 ? reach : 122) + 2, y - 1, 2, 5);
  }
  display.setMaxClipWindow();
}

// Dim the shimmer word with a checkerboard, except in the sweeping band.
// Works straight on the strip buffer, so it costs almost nothing.
void shimmerMask() {
  uint8_t *buf = display.getBufferPtr();
  uint8_t firstTile = u8g2_GetBufferCurrTileRow(display.getU8g2());
  for (uint8_t r = 0; r < 2; r++) {  // each strip holds 2 rows of 8 pixels
    uint8_t tile = firstTile + r;
    if (tile < 3 || tile > 5) continue;  // only the word's rows (y 24..47)
    int16_t yc = tile * 8 + 4;
    uint8_t *row = buf + r * 128;
    for (uint8_t x = 0; x < 128; x++)
      if (abs(x - yc * 2 / 5 - shimmerBand) > 9) row[x] &= (x & 1) ? 0x55 : 0xAA;
  }
}

void drawClaude(long ms) {
  drawRays();
  switch (claudeAnim) {
    case SPINNER:
      display.setFont(SMALL_FONT);
      display.setFontPosTop();
      display.drawStr(claudeTextX, 54, claudeText);
      display.setFontPosBaseline();
      break;
    case WRITING:
      display.setFont(SMALL_FONT);
      display.setFontPosTop();
      display.drawStr(17, 4, claudeText);
      display.setFontPosBaseline();
      drawCodeLines(ms);
      break;
    case SHIMMER:
      display.setFont(MID_FONT);
      display.drawStr(claudeTextX, 44, claudeText);
      shimmerMask();
      break;
  }
}

// ---------------------------------------------------------------------------
// Usage page: two rings, 5-hour on the left and weekly on the right.
// The numbers count up; the labels swap with "resets in" times.
// ---------------------------------------------------------------------------
uint8_t shownFive, shownWeek;
char fiveLabel[8], weekLabel[8];

// e.g. "2H31M" or "45M"
void formatLeft(char *out, uint16_t minutes) {
  if (minutes >= 60) {
    utoa(minutes / 60, out, 10);
    out += strlen(out);
    *out++ = 'H';
    *out++ = '0' + (minutes % 60) / 10;
  }
  utoa(minutes >= 60 ? minutes % 10 : minutes, out, 10);
  strcat_P(out, PSTR("M"));
}

void prepareUsage(long ms) {
  float f = easeOut(clamp01(ms / 1000.0));
  shownFive = fivePct > 100 ? 0xFF : fivePct * f + 0.5;
  shownWeek = weekPct > 100 ? 0xFF : weekPct * f + 0.5;
  if ((ms / 2000) % 2) {
    formatLeft(fiveLabel, fiveResetMin);
    formatLeft(weekLabel, weekResetMin);
  } else {
    strcpy_P(fiveLabel, PSTR("5 HOUR"));
    strcpy_P(weekLabel, PSTR("WEEKLY"));
  }
}

// Sine in 2-degree steps (0..90 degrees), scaled to -127..127, for fast ring drawing
const int8_t SIN2[46] PROGMEM = {
  0, 4, 9, 13, 18, 22, 26, 31, 35, 39, 43, 48, 52, 56, 60, 63, 67, 71, 75, 78, 82, 85, 88,
  91, 94, 97, 100, 103, 105, 108, 110, 112, 114, 116, 118, 119, 121, 122, 123, 124, 125,
  126, 126, 127, 127, 127};

int8_t sin2(uint8_t k) {  // k = 0..179 -> sin(k * 2 degrees)
  if (k <= 45) return pgm_read_byte(&SIN2[k]);
  if (k <= 90) return pgm_read_byte(&SIN2[90 - k]);
  if (k <= 135) return -(int8_t)pgm_read_byte(&SIN2[k - 90]);
  return -(int8_t)pgm_read_byte(&SIN2[180 - k]);
}

// Point k (0..179) on a circle of radius r, clockwise from the top
void ringPixel(uint8_t cx, uint8_t cy, uint8_t k, uint8_t r) {
  // >> 7 is a fast divide by 128 (close enough to the table's 127 scale)
  display.drawPixel(cx + ((sin2(k) * r) >> 7), cy - ((sin2((k + 45) % 180) * r) >> 7));
}

void drawRing(uint8_t cx, uint8_t cy, uint8_t pct, const char *label) {
  for (uint8_t k = 0; k < 180; k += 6) ringPixel(cx, cy, k, 21);  // dotted track
  if (pct <= 100)  // progress arc, 3 px thick
    for (uint8_t k = 0; k < (uint16_t)pct * 180 / 100; k++)
      for (uint8_t r = 20; r <= 22; r++) ringPixel(cx, cy, k, r);

  char num[4];
  if (pct > 100) strcpy_P(num, PSTR("--"));
  else utoa(pct, num, 10);
  display.setFont(MID_FONT);
  display.drawStr(cx - display.getStrWidth(num) / 2, cy + 8, num);

  display.setFont(SMALL_FONT);
  display.setFontPosTop();
  display.drawStr(cx - display.getStrWidth(label) / 2, 54, label);
  display.setFontPosBaseline();
}

void drawUsage() {
  drawRing(32, 26, shownFive, fiveLabel);
  drawRing(96, 26, shownWeek, weekLabel);
}

// ---------------------------------------------------------------------------
// Page sequencing, with the screen "blinking" shut and open between pages
// ---------------------------------------------------------------------------
enum Page : uint8_t { PG_CLOCK, PG_EYES, PG_CLAUDE, PG_USAGE };
enum Trans : uint8_t { T_NONE, T_CLOSING, T_OPENING };
Page page;
Trans trans = T_NONE;
unsigned long pageStart, transStart, clockUntil, eyesUntil, eyesCloseStart;
bool eyesClosing, usageNext;

// Not working:  eyes -> clock -> eyes ...
// Working:      Claude -> usage -> Claude -> clock -> Claude ...
Page nextPage() {
  bool working = claudeWorking();
  switch (page) {
    case PG_CLOCK: return working ? PG_CLAUDE : PG_EYES;
    case PG_EYES: return PG_CLOCK;
    case PG_CLAUDE:
      if (!working) return PG_CLOCK;
      usageNext = !usageNext;
      return usageNext ? PG_USAGE : PG_CLOCK;
    default: return working ? PG_CLAUDE : PG_CLOCK;
  }
}

void showPage(Page p) {
  page = p;
  pageStart = frameNow;
  trans = T_OPENING;  // lids open onto the new page
  transStart = frameNow;
  if (p == PG_CLOCK) clockUntil = frameNow + random(CLOCK_MIN_S, CLOCK_MAX_S + 1) * 1000UL;
  if (p == PG_CLAUDE) startClaudeAnim();
  if (p == PG_EYES) {
    trans = T_NONE;  // the eyes open by themselves
    eyeX = eyeY = eyeH = 0;
    eyesUntil = frameNow + random(EYES_MIN_S, EYES_MAX_S + 1) * 1000UL;
    eyesClosing = false;
    startAct();
  }
}

void closePage() {
  trans = T_CLOSING;
  transStart = frameNow;
}

void drawLids(uint8_t lid) {  // black bars closing in from top and bottom
  display.setDrawColor(0);
  display.drawBox(0, 0, 128, lid);
  display.drawBox(0, 64 - lid, 128, lid);
  display.setDrawColor(1);
}

// Advance timers, animations and page changes: runs once per frame
void update() {
  long pt = frameNow - pageStart;
  unsigned long tt = frameNow - transStart;

  if (trans == T_CLOSING && tt >= LID_MS) showPage(nextPage());
  else if (trans == T_OPENING && tt >= LID_MS) trans = T_NONE;
  pt = frameNow - pageStart;

  switch (page) {
    case PG_CLOCK:
      if (trans == T_NONE && frameNow >= clockUntil) closePage();
      break;

    case PG_EYES:
      if (claudeWorking() && synced) eyesUntil = 0;  // Claude started: wrap up soon
      if (eyesClosing) {
        easeEyes(0, 0, 0);
        if (frameNow - eyesCloseStart >= LID_MS) showPage(nextPage());
      } else {
        // Play acts back to back; once the eye time is up, finish the
        // current act and close the eyes (stay here until the time is synced)
        if (!updateAct()) {
          if (synced && frameNow >= eyesUntil) {
            eyesClosing = true;
            eyesCloseStart = frameNow;
          } else {
            startAct();
          }
        }
        if (!eyesClosing) easeEyes(key.x, key.y, key.h);
      }
      break;

    case PG_CLAUDE:
      if (trans == T_NONE && pt >= CLAUDE_MS) closePage();
      break;

    case PG_USAGE:
      if (trans == T_NONE && pt >= USAGE_MS) closePage();
      break;
  }

  // Work out this frame's picture once, so render() only has to draw it
  if (page == PG_CLOCK) prepareClock();
  else if (page == PG_CLAUDE) prepareClaude(pt);
  else if (page == PG_USAGE) prepareUsage(pt);
}

// Draw the current frame: called once for each strip of the screen
void render() {
  switch (page) {
    case PG_CLOCK: drawClock(); break;
    case PG_CLAUDE: drawClaude(frameNow - pageStart); break;
    case PG_USAGE: drawUsage(); break;
    case PG_EYES:
      drawEyes(eyesClosing ? NORMAL : key.style);
      if (!synced) {
        display.setFont(SMALL_FONT);
        display.setCursor(14, 63);
        display.print(F("waiting for PC time"));
      }
      break;
  }

  unsigned long tt = frameNow - transStart;
  if (tt > LID_MS) tt = LID_MS;
  if (trans == T_CLOSING) drawLids(tt * 32 / LID_MS);
  else if (trans == T_OPENING) drawLids(32 - tt * 32 / LID_MS);
}

void setup() {
  Serial.begin(115200);
  randomSeed(analogRead(A0));

  // Most modules use address 0x3C. U8g2 expects the 8-bit form (0x3C << 1).
  display.setI2CAddress(0x3C << 1);
  display.setBusClock(I2C_CLOCK);
  display.begin();
  display.setFontMode(1);
  measureClock();

  showPage(PG_EYES);
}

void loop() {
  readSerial();
  frameNow = millis();

  update();
  display.firstPage();
  do {
    render();
  } while (display.nextPage());
  display.setPowerSave(0);  // re-send "display on" in case an I2C glitch turned it off
}
