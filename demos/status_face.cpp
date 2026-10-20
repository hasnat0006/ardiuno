#include <Arduino.h>
#include <Wire.h>
#include <U8g2lib.h>

// PC activity status display.
//
// tools/status_sender.py runs on the PC and sends one line ~10 times a second:
//   <state>,<keyCount>,<keysPerMin>,<mouseX%>,<mouseY%>,<idleSec>,
//   <leftClicks>,<rightClicks>,<HH:MM>
// state: T = typing, A = using the mouse, I = idle, Z = away
//
// Wiring (Arduino Uno / Nano): VCC->5V, GND->GND, SDA->A4, SCL->A5
// If the image is shifted or garbled, use the SSD1306 constructor instead:
//   U8G2_SSD1306_128X64_NONAME_F_HW_I2C display(U8G2_R0, U8X8_PIN_NONE);
U8G2_SH1106_128X64_NONAME_F_HW_I2C display(U8G2_R0, U8X8_PIN_NONE);

const unsigned long OFFLINE_MS = 3000;  // no data for this long = PC not connected
const unsigned long WINK_MS = 300;      // how long an eye stays shut after a click

// ---------------------------------------------------------------------------
// Latest status received from the PC
// ---------------------------------------------------------------------------
char state = 0;
uint16_t kpm = 0, idleSec = 0;
uint8_t mouseX = 50, mouseY = 50;
char clockStr[6] = "--:--";
unsigned long lastMsg = 0;

// Click counters from the PC; a change means a click happened
uint8_t leftClicks, rightClicks;
bool haveClicks = false;
unsigned long winkUntil[2];  // [0] = left eye, [1] = right eye

char line[48];
uint8_t lineLen = 0;

// ---------------------------------------------------------------------------
// Serial protocol
// ---------------------------------------------------------------------------
void parseLine() {
  char st, clk[6];
  unsigned int keys, k, mx, my, idle, lc, rc;
  if (sscanf_P(line, PSTR("%c,%u,%u,%u,%u,%u,%u,%u,%5s"),
               &st, &keys, &k, &mx, &my, &idle, &lc, &rc, clk) != 9) return;

  if (haveClicks) {
    if (lc != leftClicks) winkUntil[0] = millis() + WINK_MS;
    if (rc != rightClicks) winkUntil[1] = millis() + WINK_MS;
  }
  leftClicks = lc;
  rightClicks = rc;
  haveClicks = true;

  state = st;
  kpm = k;
  mouseX = min(mx, 100u);
  mouseY = min(my, 100u);
  idleSec = idle;
  strcpy(clockStr, clk);
  lastMsg = millis();
}

void readSerial() {
  while (Serial.available()) {
    char c = Serial.read();
    if (c == '\n') {
      line[lineLen] = 0;
      parseLine();
      lineLen = 0;
    } else if (c != '\r' && lineLen < sizeof(line) - 1) {
      line[lineLen++] = c;
    }
  }
}

// ---------------------------------------------------------------------------
// Drawing helpers
// ---------------------------------------------------------------------------
uint8_t currentContrast = 0;
void setContrast(uint8_t c) {
  if (c != currentContrast) display.setContrast(c);
  currentContrast = c;
}

// Eye offset in 1/4 px so it can glide smoothly towards its target
int16_t eyeX = 0, eyeY = 0;
unsigned long nextBlink = 0, blinkUntil = 0;

uint8_t eyeCenterX(uint8_t i) { return 64 + (i ? 24 : -24) + eyeX / 4; }

void drawEyes(uint8_t cy, uint8_t w, uint8_t h, int8_t lookX, int8_t lookY) {
  eyeX += (lookX * 4 - eyeX) / 3;
  eyeY += (lookY * 4 - eyeY) / 3;

  unsigned long now = millis();
  if (now > nextBlink) {
    blinkUntil = now + 120;
    nextBlink = now + random(2000, 6000);
  }
  bool blinking = now < blinkUntil;

  for (uint8_t i = 0; i < 2; i++) {
    uint8_t ex = eyeCenterX(i), ey = cy + eyeY / 4;
    if (now < winkUntil[i]) {
      // Winking: closed "^" shaped eye
      for (uint8_t t = 0; t < 3; t++) {
        display.drawLine(ex - w / 2, ey + 3 + t, ex, ey - 3 + t);
        display.drawLine(ex, ey - 3 + t, ex + w / 2, ey + 3 + t);
      }
      continue;
    }
    uint8_t eh = blinking ? 2 : h;
    uint8_t x = ex - w / 2, y = ey - eh / 2;
    if (eh >= 12) display.drawRBox(x, y, w, eh, 5);
    else if (eh >= 4) display.drawRBox(x, y, w, eh, 1);
    else display.drawBox(x, y, w, eh);
  }
}

void formatDuration(char *out, uint16_t sec) {
  if (sec < 60) sprintf_P(out, PSTR("%us"), sec);
  else if (sec < 3600) sprintf_P(out, PSTR("%um"), sec / 60);
  else sprintf_P(out, PSTR("%uh%02um"), sec / 3600, (sec / 60) % 60);
}

void drawFooter(const char *label) {
  display.setFont(u8g2_font_6x10_tf);
  display.drawStr(0, 63, label);
  display.drawStr(128 - display.getStrWidth(clockStr), 63, clockStr);
}

// ---------------------------------------------------------------------------
// Screens
// ---------------------------------------------------------------------------
void drawTyping() {
  setContrast(255);

  // Narrowed eyes looking down at the work, with determined eyebrows
  const uint8_t cy = 28, h = 16;
  drawEyes(cy, 30, h, 0, 2);
  uint8_t browY = cy - h / 2 + eyeY / 4 - 6;
  for (uint8_t t = 0; t < 2; t++) {
    display.drawLine(eyeCenterX(0) - 14, browY - 4 + t, eyeCenterX(0) + 12, browY + t);
    display.drawLine(eyeCenterX(1) + 14, browY - 4 + t, eyeCenterX(1) - 12, browY + t);
  }

  // Typing fast: a sweat drop slides down beside the right eye
  if (kpm > 200) {
    uint8_t x = 113, y = 10 + (millis() / 80) % 16;
    display.drawTriangle(x - 3, y, x + 3, y, x, y - 5);
    display.drawDisc(x, y, 3);
  }

  char label[16];
  strcpy_P(label, PSTR("typing "));
  utoa(kpm, label + 7, 10);
  strcat_P(label, PSTR("/min"));
  drawFooter(label);
}

unsigned long nextWander = 0;
int8_t wanderX = 0, wanderY = 0;

void drawAwake(bool usingMouse) {
  setContrast(255);
  char label[16];

  if (usingMouse) {
    // Eyes follow your mouse pointer around the screen
    drawEyes(26, 30, 28, ((int16_t)mouseX - 50) * 14 / 50, ((int16_t)mouseY - 50) * 8 / 50);
    strcpy_P(label, PSTR("mouse"));
  } else {
    // Nothing happening: eyes wander around on their own
    if (millis() > nextWander) {
      wanderX = random(-14, 15);
      wanderY = random(-8, 9);
      nextWander = millis() + random(800, 3000);
    }
    drawEyes(26, 30, 28, wanderX, wanderY);
    strcpy_P(label, PSTR("idle "));
    formatDuration(label + 5, idleSec);
  }
  drawFooter(label);
}

void drawAway() {
  // Closed eyes and slowly "breathing" brightness
  for (int8_t side = -1; side <= 1; side += 2) display.drawBox(64 + side * 24 - 15, 30, 30, 3);
  uint8_t p = (millis() / 20) & 255;
  setContrast(10 + (p < 128 ? p : 255 - p));

  // Three Z's drifting up and growing
  for (uint8_t i = 0; i < 3; i++) {
    uint8_t phase = (millis() / 40 + i * 33) % 100;
    display.setFont(phase < 33 ? u8g2_font_5x7_tf : phase < 66 ? u8g2_font_6x10_tf : u8g2_font_7x13B_tf);
    display.drawGlyph(96 + phase / 6, 30 - phase / 5, 'Z');
  }

  char label[16];
  strcpy_P(label, PSTR("away "));
  formatDuration(label + 5, idleSec);
  drawFooter(label);
}

void drawOffline() {
  setContrast(255);
  display.setFont(u8g2_font_7x13B_tf);
  display.setCursor(6, 26);
  display.print(F("Waiting for PC"));
  for (uint8_t i = 0; i < 3; i++)
    if ((millis() / 300) % 4 > i) display.drawDisc(54 + i * 10, 38, 2);
  display.setFont(u8g2_font_5x7_tf);
  display.setCursor(4, 60);
  display.print(F("run status_sender.py"));
}

// ---------------------------------------------------------------------------
void setup() {
  Serial.begin(115200);
  Serial.println(F("boot"));
  randomSeed(analogRead(A0));

  // Most modules use address 0x3C. U8g2 expects the 8-bit form (0x3C << 1).
  display.setI2CAddress(0x3C << 1);
  display.setBusClock(100000);  // slower I2C is much more tolerant of loose/long wires
  display.begin();
  display.setFontMode(1);
}

void loop() {
  readSerial();

  display.clearBuffer();
  if (lastMsg == 0 || millis() - lastMsg > OFFLINE_MS) drawOffline();
  else if (state == 'T') drawTyping();
  else if (state == 'A') drawAwake(true);
  else if (state == 'I') drawAwake(false);
  else drawAway();
  display.sendBuffer();
  display.setPowerSave(0);  // re-send "display on" in case an I2C glitch turned it off
}
