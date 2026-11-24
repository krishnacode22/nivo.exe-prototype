/*
  C3 ARCADE - Handheld Console V1, multi-game firmware
  Board : ESP32C3 Dev Module  (USB CDC On Boot: Enabled)
  Lib   : U8g2 by olikraus   (Preferences is built into the ESP32 core)

  Games: DOOM-LITE (raycaster FPS), 3D MAZE, RACER (OutRun style),
         SNAKE, PONG, BREAKOUT, SKY RAID (3D flight)

  Menu : arrows move the cursor, A launches.
  In any game: hold A+B (no arrow pressed) for ~0.8 s to return to the menu.

  Add a game: write `void gameFoo() { startGame(); while (tick()) { ...; present(); } }`
  then add one line to the GAMES table and one case to drawIcon().

  Speed tips:
   - I2C_SPEED is the main FPS limit (a frame is ~1 KB over I2C). Lower it to
     800000 or 400000 if the screen shows garbage.
   - Wall/floor drawing writes whole bytes of the frame buffer, not pixels.
   - Snake only redraws when something changed.
*/

#include <Arduino.h>
#include <Wire.h>
#include <U8g2lib.h>
#include <Preferences.h>
#include <math.h>
#include <WiFi.h>
#include "c3_arcade_sprites.h"

// ================= PINS =================
#define BTN_UP     0
#define BTN_DOWN   1
#define BTN_LEFT   3
#define BTN_RIGHT  4
#define BTN_A      5
#define BTN_B      6
#define BUZZER     7
#define OLED_SDA   8
#define OLED_SCL   9

// ================= SETTINGS =================
#define BOOT_TEXT     "nivo.exe" // Boot screen text (change to "nevo.exe" if desired)
#define I2C_SPEED     1000000  // 1 MHz = max ESP32-C3 speed. Lower if the display glitches.
#define SHOW_FPS      1
#define SOUND_ON      1
#define PANEL_LINES   1        // dark seams at tile boundaries (3D games)

// Doom-lite / maze tuning
#define MOVE_SPEED    2.6f
#define STRAFE_SPEED  2.2f
#define TURN_SPEED    2.3f
#define FIRE_DELAY    320
#define START_HEALTH  100
#define START_AMMO    40
#define ENEMY_HP      3
#define ENEMY_SPEED   0.9f
#define ENEMY_WAKE    7.0f

// ================= CONSTANTS =================
#define SCR_W        128
#define SCR_H        64
#define HALF_H       32
#define MAP_W        16
#define MAP_H        16
#define MAX_ENEMIES  8
#define NGAMES       22

const float FOV_PLANE = 0.66f;
const float FOCAL     = 64.0f / FOV_PLANE;

// '#' wall   'X' bright wall   '.' floor   'P' player start   'E' monster
const char* const LEVEL[MAP_H] = {
  "################",
  "#P.....#.......#",
  "#......#...E...#",
  "#..XX..#.......#",
  "#..XX..........#",
  "#......#.......#",
  "####.#####.#####",
  "#.......#......#",
  "#.......#..E...#",
  "#...##.........#",
  "#...##..#......#",
  "#.......#......#",
  "####.#####.#####",
  "#.E............#",
  "#.........#..E.#",
  "################"
};

const char* const MONSTER[16] = {
  "##............##",
  "###..........###",
  ".###..####..###.",
  "..############..",
  "..##oo####oo##..",
  "..############..",
  "...#oo#oo#oo#...",
  "...##########...",
  "..############..",
  ".##############.",
  "###.########.###",
  "###..######..###",
  "..#..######..#..",
  "....########....",
  "...###....###...",
  "..####....####.."
};

const char* const MONSTER_B[16] = {
  ".##..........##.",
  "###..........###",
  ".###..####..###.",
  "..############..",
  "..##oo####oo##..",
  "..############..",
  "...#oooooooo#...",
  "...##oo##oo##...",
  "..############..",
  ".##############.",
  "####.######.####",
  ".###.######.###.",
  ".....######.....",
  "....########....",
  ".....##..##.....",
  "....###..###...."
};

const char* const CRAWLER[16] = {
  "................",
  "................",
  "................",
  "................",
  "#..............#",
  "##....####....##",
  ".##.########.##.",
  "..############..",
  "...##oo##oo##...",
  "..############..",
  "#.###oo##oo###.#",
  "###.########.###",
  "##...######...##",
  "#....##..##....#",
  "....###..###....",
  "...##......##..."
};

const char* const CRAWLER_B[16] = {
  "................",
  "................",
  "................",
  "................",
  "..#..........#..",
  ".##...####...##.",
  "###.########.###",
  ".#############..",
  "...##oo##oo##...",
  "..############..",
  ".####oo##oo####.",
  "#..##########..#",
  "#....######....#",
  "....###..###....",
  "...##......##...",
  "..##........##.."
};

const uint8_t BAYER[4][4] = {
  { 0,  8,  2, 10},
  {12,  4, 14,  6},
  { 3, 11,  1,  9},
  {15,  7, 13,  5}
};

// ================= COMMON STATE =================
U8G2_SSD1306_128X64_NONAME_F_HW_I2C u8g2(U8G2_R0, U8X8_PIN_NONE);
Preferences prefs;
uint8_t *fb;

enum { K_UP = 1, K_DOWN = 2, K_LEFT = 4, K_RIGHT = 8, K_A = 16, K_B = 32 };
const uint8_t pins[6] = {BTN_UP, BTN_DOWN, BTN_LEFT, BTN_RIGHT, BTN_A, BTN_B};
enum GState { PLAYING, DEAD, WON };

// Structs used as function parameters must be defined before the first function,
// because the Arduino IDE auto-generates prototypes at the top of the file.
struct Enemy {
  float    x, y;
  int8_t   hp;
  bool     awake;
  uint32_t hurtUntil;
  uint32_t nextAttack;
  uint8_t  type; // 0 = demon, 1 = crawler
};
struct Obj { float x, y, z; uint8_t type; };   // flight game: 0 pylon, 1 ring, 2 drone

uint8_t  btn = 0, btnPrev = 0;
uint32_t now = 0, lastMs = 0, fpsT = 0, comboSince = 0, sfxEnd = 0;
float    dt = 0.016f;
int      fps = 0, frames = 0;

static inline bool held(uint8_t k)    { return btn & k; }
static inline bool pressed(uint8_t k) { return (btn & ~btnPrev) & k; }
static inline int  imax(int a, int b) { return a > b ? a : b; }
static inline int  imin(int a, int b) { return a < b ? a : b; }
static inline float clampf(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }

// ================= SOUND =================
void buzzInit() {
#if ESP_ARDUINO_VERSION_MAJOR >= 3
  ledcAttach(BUZZER, 2000, 8);
#else
  ledcSetup(0, 2000, 8);
  ledcAttachPin(BUZZER, 0);
#endif
}

void buzz(uint16_t f) {
#if ESP_ARDUINO_VERSION_MAJOR >= 3
  ledcWriteTone(BUZZER, f);
#else
  ledcWriteTone(0, f);
#endif
}

void sfx(uint16_t f, uint16_t ms) {
#if SOUND_ON
  buzz(f);
  sfxEnd = millis() + ms;
#endif
}

void sfxUpdate() {
  if (sfxEnd && (int32_t)(now - sfxEnd) >= 0) { buzz(0); sfxEnd = 0; }
}

// ================= HIGH SCORES =================
uint32_t hiGet(const char* k) {
  prefs.begin("arcade", false);
  uint32_t v = prefs.getUInt(k, 0);
  prefs.end();
  return v;
}
void hiSet(const char* k, uint32_t v) {
  prefs.begin("arcade", false);
  prefs.putUInt(k, v);
  prefs.end();
}
void hiTime(const char* k, uint32_t t) {   // lower is better (tenths of a second)
  uint32_t o = hiGet(k);
  if (t < 1) t = 1;
  if (!o || t < o) hiSet(k, t);
}
void hiScore(const char* k, uint32_t s) { // higher is better
  if (s > hiGet(k)) hiSet(k, s);
}

// ================= FRAME LOOP HELPERS =================
void readButtons() {
  btnPrev = btn;
  uint8_t v = 0;
  for (int i = 0; i < 6; i++) if (digitalRead(pins[i]) == LOW) v |= (1 << i);
  btn = v;
}

void fxClear();

// Call at the start of every game / menu
void startGame() {
  readButtons(); readButtons();           // so a held button is not a fresh press
  comboSince = 0;
  lastMs = millis();
  fpsT = lastMs; frames = 0; fps = 0;
  randomSeed(micros());
  u8g2.setDrawColor(1);
  fxClear();
}

// Call at the top of every frame. Returns false when the player asked for the menu.
bool tick() {
  now = millis();
  dt = (now - lastMs) * 0.001f;
  lastMs = now;
  if (dt > 0.05f) dt = 0.05f;
  readButtons();
  sfxUpdate();
  if ((btn & 63) == (K_A | K_B)) {        // A+B only, no arrows
    if (!comboSince) comboSince = now;
    else if (now - comboSince > 800) { comboSince = 0; sfx(300, 80); return false; }
  } else comboSince = 0;
  return true;
}

// Call at the end of every frame
void present() {
  if (comboSince) {                        // exit-progress line on top row
    u8g2.setDrawColor(2);
    u8g2.drawHLine(0, 0, (int)((now - comboSince) * SCR_W / 800));
    u8g2.setDrawColor(1);
  }
  u8g2.sendBuffer();
  frames++;
  if (now - fpsT >= 1000) { fps = frames; frames = 0; fpsT = now; }
}

// ================= DRAW HELPERS & SAFE CLIPPED PRIMITIVES =================
static inline void setPx(int x, int y) {
  if (x >= 0 && x < SCR_W && y >= 0 && y < SCR_H) {
    fb[(y >> 3) * SCR_W + x] |=  (1 << (y & 7));
  }
}
static inline void clrPx(int x, int y) {
  if (x >= 0 && x < SCR_W && y >= 0 && y < SCR_H) {
    fb[(y >> 3) * SCR_W + x] &= ~(1 << (y & 7));
  }
}

// OR a vertical byte pattern into rows ya..yb of column x (whole bytes, no per-pixel loop)
static inline void colSpan(int x, int ya, int yb, uint8_t pat) {
  if (x < 0 || x >= SCR_W) return;
  if (ya < 0) ya = 0;
  if (yb >= SCR_H) yb = SCR_H - 1;
  if (ya > yb) return;
  int pa = ya >> 3, pb = yb >> 3;
  uint8_t *p = fb + pa * SCR_W + x;
  if (pa == pb) { *p |= pat & (0xFF << (ya & 7)) & (0xFF >> (7 - (yb & 7))); return; }
  *p |= pat & (0xFF << (ya & 7));
  p += SCR_W;
  for (int q = pa + 1; q < pb; q++, p += SCR_W) *p |= pat;
  *p |= pat & (0xFF >> (7 - (yb & 7)));
}

// Horizontal spans inside one buffer row (row = fb + page*128, m = 1 << (y&7))
static inline void hspan(uint8_t *row, uint8_t m, int a, int b) {
  if (a < 0) a = 0;
  if (b > SCR_W - 1) b = SCR_W - 1;
  if (a > b) return;
  for (int x = a; x <= b; x++) row[x] |= m;
}
static inline void hspanDither(uint8_t *row, uint8_t m, int a, int b) { // every 2nd pixel
  if (a < 0) a = 0;
  if (b > SCR_W - 1) b = SCR_W - 1;
  if (a > b) return;
  for (int x = (a + 1) & ~1; x <= b; x += 2) row[x] |= m;
}

// U8g2 takes uint8_t coordinates and does not clip signed negative values,
// which wraps negative coordinates around 255 and stretches lines across the entire display.
// The following safe primitives perform signed clipping strictly within the display window:
static inline bool clipLine(int &x0, int &y0, int &x1, int &y1) {
  const int INSIDE = 0, C_LEFT = 1, C_RIGHT = 2, C_BOTTOM = 4, C_TOP = 8;
  auto getCode = [](int x, int y) -> int {
    int c = INSIDE;
    if (x < 0) c |= C_LEFT;
    else if (x >= SCR_W) c |= C_RIGHT;
    if (y < 0) c |= C_TOP;
    else if (y >= SCR_H) c |= C_BOTTOM;
    return c;
  };

  int c0 = getCode(x0, y0);
  int c1 = getCode(x1, y1);

  while (true) {
    if (!(c0 | c1)) return true;
    if (c0 & c1) return false;

    int cOut = c0 ? c0 : c1;
    int x = 0, y = 0;

    if (cOut & C_TOP) {
      if (y1 == y0) return false;
      x = x0 + (int)((int64_t)(x1 - x0) * (0 - y0) / (y1 - y0));
      y = 0;
    } else if (cOut & C_BOTTOM) {
      if (y1 == y0) return false;
      x = x0 + (int)((int64_t)(x1 - x0) * (SCR_H - 1 - y0) / (y1 - y0));
      y = SCR_H - 1;
    } else if (cOut & C_RIGHT) {
      if (x1 == x0) return false;
      y = y0 + (int)((int64_t)(y1 - y0) * (SCR_W - 1 - x0) / (x1 - x0));
      x = SCR_W - 1;
    } else if (cOut & C_LEFT) {
      if (x1 == x0) return false;
      y = y0 + (int)((int64_t)(y1 - y0) * (0 - x0) / (x1 - x0));
      x = 0;
    }

    if (cOut == c0) {
      x0 = x; y0 = y;
      c0 = getCode(x0, y0);
    } else {
      x1 = x; y1 = y;
      c1 = getCode(x1, y1);
    }
  }
}

static inline void drawLineSafe(int x0, int y0, int x1, int y1) {
  if (clipLine(x0, y0, x1, y1)) {
    u8g2.drawLine(x0, y0, x1, y1);
  }
}

static inline void drawHLineSafe(int x, int y, int w) {
  if (y < 0 || y >= SCR_H || w <= 0) return;
  if (x < 0) { w += x; x = 0; }
  if (x + w > SCR_W) w = SCR_W - x;
  if (w > 0) u8g2.drawHLine(x, y, w);
}

static inline void drawVLineSafe(int x, int y, int h) {
  if (x < 0 || x >= SCR_W || h <= 0) return;
  if (y < 0) { h += y; y = 0; }
  if (y + h > SCR_H) h = SCR_H - y;
  if (h > 0) u8g2.drawVLine(x, y, h);
}

static inline void drawBoxSafe(int x, int y, int w, int h) {
  if (w <= 0 || h <= 0) return;
  if (x < 0) { w += x; x = 0; }
  if (y < 0) { h += y; y = 0; }
  if (x + w > SCR_W) w = SCR_W - x;
  if (y + h > SCR_H) h = SCR_H - y;
  if (w > 0 && h > 0) u8g2.drawBox(x, y, w, h);
}

static inline void drawFrameSafe(int x, int y, int w, int h) {
  drawHLineSafe(x, y, w);
  drawHLineSafe(x, y + h - 1, w);
  drawVLineSafe(x, y, h);
  drawVLineSafe(x + w - 1, y, h);
}

static inline void drawPixelSafe(int x, int y) {
  if (x >= 0 && x < SCR_W && y >= 0 && y < SCR_H) {
    u8g2.drawPixel(x, y);
  }
}

static inline void drawCircleSafe(int x, int y, int r) {
  if (x + r < 0 || x - r >= SCR_W || y + r < 0 || y - r >= SCR_H) return;
  u8g2.drawCircle(x, y, r, U8G2_DRAW_ALL);
}

static inline void drawDiscSafe(int x, int y, int r) {
  if (x + r < 0 || x - r >= SCR_W || y + r < 0 || y - r >= SCR_H) return;
  u8g2.drawDisc(x, y, r, U8G2_DRAW_ALL);
}

static inline void drawTriangleSafe(int x0, int y0, int x1, int y1, int x2, int y2) {
  if ((x0 < 0 && x1 < 0 && x2 < 0) || (x0 >= SCR_W && x1 >= SCR_W && x2 >= SCR_W) ||
      (y0 < 0 && y1 < 0 && y2 < 0) || (y0 >= SCR_H && y1 >= SCR_H && y2 >= SCR_H)) return;
  if (x0 >= 0 && x0 < SCR_W && x1 >= 0 && x1 < SCR_W && x2 >= 0 && x2 < SCR_W &&
      y0 >= 0 && y0 < SCR_H && y1 >= 0 && y1 < SCR_H && y2 >= 0 && y2 < SCR_H) {
    u8g2.drawTriangle(x0, y0, x1, y1, x2, y2);
  } else {
    drawLineSafe(x0, y0, x1, y1);
    drawLineSafe(x1, y1, x2, y2);
    drawLineSafe(x2, y2, x0, y0);
  }
}

void drawLabel(int x, int y, const char* s, bool rightAlign) {
  int w = u8g2.getStrWidth(s);
  if (rightAlign) x = SCR_W - w - 2 - x;
  u8g2.setDrawColor(0);
  u8g2.drawBox(x, y - 7, w + 3, 8);
  u8g2.setDrawColor(1);
  u8g2.drawStr(x + 1, y, s);
}

void drawMessage(const char* l1, const char* l2) {
  u8g2.setFont(u8g2_font_6x10_tr);
  u8g2.setDrawColor(0);
  u8g2.drawBox(10, 14, 108, 36);
  u8g2.setDrawColor(1);
  u8g2.drawFrame(10, 14, 108, 36);
  u8g2.drawStr((SCR_W - u8g2.getStrWidth(l1)) / 2, 29, l1);
  u8g2.drawStr((SCR_W - u8g2.getStrWidth(l2)) / 2, 44, l2);
}

// ================= COLLISION & DESTRUCTION PARTICLES =================
#define MAX_FX 40
struct FxPart {
  float x, y;
  float vx, vy;
  int8_t life;
  int8_t maxLife;
};
FxPart fxPool[MAX_FX];
int fxCount = 0;

void fxClear() {
  fxCount = 0;
}

// Spawns debris pieces scattering in random directions
void fxSpawn(float x, float y, int count, float minSpd, float maxSpd, float spreadX = 0, float spreadY = 0) {
  for (int i = 0; i < count; i++) {
    if (fxCount >= MAX_FX) {
      for (int j = 0; j < MAX_FX - 1; j++) fxPool[j] = fxPool[j + 1];
      fxCount = MAX_FX - 1;
    }
    float ang = (float)random(0, 628) * 0.01f;
    float spd = minSpd + (float)random(0, 100) * 0.01f * (maxSpd - minSpd);
    float px = x + (spreadX > 0 ? (float)random((int)-spreadX, (int)spreadX + 1) : 0);
    float py = y + (spreadY > 0 ? (float)random((int)-spreadY, (int)spreadY + 1) : 0);

    fxPool[fxCount].x = px;
    fxPool[fxCount].y = py;
    fxPool[fxCount].vx = cosf(ang) * spd;
    fxPool[fxCount].vy = sinf(ang) * spd;
    int8_t l = (int8_t)random(9, 16); // 150-250ms duration: snappy & clean
    fxPool[fxCount].life = l;
    fxPool[fxCount].maxLife = l;
    fxCount++;
  }
}

// Updates physics and renders particles (supports downward gravity)
void fxUpdateAndDraw(float dt, float gravity = 0.0f) {
  for (int i = 0; i < fxCount; i++) {
    fxPool[i].vy += gravity * dt;
    fxPool[i].x += fxPool[i].vx * dt;
    fxPool[i].y += fxPool[i].vy * dt;
    fxPool[i].life--;

    int ix = (int)fxPool[i].x;
    int iy = (int)fxPool[i].y;

    if (fxPool[i].life <= 0 || ix < 0 || ix >= SCR_W || iy < 0 || iy >= SCR_H) {
      fxPool[i] = fxPool[--fxCount];
      i--;
      continue;
    }

    // High clarity: single pixels, flickering when fading to avoid confusing with game objects
    if (fxPool[i].life > (fxPool[i].maxLife / 2)) {
      u8g2.drawPixel(ix, iy);
    } else {
      if (fxPool[i].life & 1) u8g2.drawPixel(ix, iy);
    }
  }
}

// ================= 3D ENGINE SHARED (DOOM + MAZE) =================
Enemy   enemies[MAX_ENEMIES];
int     enemyCount = 0, enemiesAlive = 0;
uint8_t world[MAP_H][MAP_W];
float   posX = 1.5f, posY = 1.5f, angle = 0.0f, dirX = 1.0f, dirY = 0.0f;
float   zbuf[SCR_W];
int8_t  floorB[SCR_H];
uint8_t wallPat[17][4];     // [brightness][x&3] -> vertical dither byte
uint8_t floorMask[8][4];    // [page][x&3]       -> floor dither byte
bool    doomActive = false;
bool    isDarkSector = false;
bool    isRestoredBanner = false;
bool    mazeActive = false;
uint32_t recoilSince = 0;
uint32_t flashUntil = 0;
int     doomWave = 1;
uint32_t waveBannerUntil = 0;

// Racer tables
#define HOR 22
int16_t rowHW[SCR_H];
float   rowD[SCR_H], rowQ[SCR_H];

void buildTables() {
  for (int y = 0; y < SCR_H; y++) {
    int8_t v = 0;
    if (y > HALF_H) {
      float rowDist = (0.5f * FOCAL) / (y - HALF_H);
      int b = (int)(9.0f - rowDist * 2.0f);
      if (b < 0) b = 0;
      if (b > 16) b = 16;
      v = b;
    }
    floorB[y] = v;
  }
  for (int b = 0; b <= 16; b++)
    for (int xm = 0; xm < 4; xm++) {
      uint8_t m = 0;
      for (int yy = 0; yy < 8; yy++) if (b > BAYER[yy & 3][xm]) m |= (1 << yy);
      wallPat[b][xm] = m;
    }
  for (int pg = 0; pg < 8; pg++)
    for (int xm = 0; xm < 4; xm++) {
      uint8_t m = 0;
      for (int yy = 0; yy < 8; yy++) {
        int y = pg * 8 + yy;
        if (floorB[y] > BAYER[y & 3][xm]) m |= (1 << yy);
      }
      floorMask[pg][xm] = m;
    }
  for (int y = HOR + 1; y < SCR_H; y++) {
    int t = y - HOR;
    float p = t / (float)(SCR_H - 1 - HOR);
    rowD[y]  = 10.0f / t;
    rowHW[y] = (int16_t)(p * 56.0f);
    rowQ[y]  = (1.0f - p) * (1.0f - p) * 50.0f;
  }
}

static inline uint8_t mapAt(int x, int y) {
  if (x < 0 || y < 0 || x >= MAP_W || y >= MAP_H) return 1;
  return world[y][x];
}

bool blocked(float x, float y, float r) {
  return mapAt((int)(x - r), (int)(y - r)) || mapAt((int)(x + r), (int)(y - r)) ||
         mapAt((int)(x - r), (int)(y + r)) || mapAt((int)(x + r), (int)(y + r));
}

bool lineOfSight(float x0, float y0, float x1, float y1) {
  float dx = x1 - x0, dy = y1 - y0;
  float d = sqrtf(dx * dx + dy * dy);
  int steps = (int)(d / 0.15f) + 1;
  for (int i = 1; i < steps; i++) {
    float t = (float)i / steps;
    if (mapAt((int)(x0 + dx * t), (int)(y0 + dy * t))) return false;
  }
  return true;
}

void tryMove(float dx, float dy) {
  if (!blocked(posX + dx, posY, 0.22f)) posX += dx;
  if (!blocked(posX, posY + dy, 0.22f)) posY += dy;
}

void renderWorld() {
  float planeX = -dirY * FOV_PLANE;
  float planeY =  dirX * FOV_PLANE;
  int prevKey = -1;
  bool flash = (doomActive && (int32_t)(now - flashUntil) < 0);

  for (int x = 0; x < SCR_W; x++) {
    float camX = (2.0f * x) / SCR_W - 1.0f;
    float rdx = dirX + planeX * camX;
    float rdy = dirY + planeY * camX;

    int mx = (int)posX, my = (int)posY;
    float ddx = (rdx == 0.0f) ? 1e30f : fabsf(1.0f / rdx);
    float ddy = (rdy == 0.0f) ? 1e30f : fabsf(1.0f / rdy);
    int stepX, stepY;
    float sdx, sdy;

    if (rdx < 0) { stepX = -1; sdx = (posX - mx) * ddx; }
    else         { stepX =  1; sdx = (mx + 1.0f - posX) * ddx; }
    if (rdy < 0) { stepY = -1; sdy = (posY - my) * ddy; }
    else         { stepY =  1; sdy = (my + 1.0f - posY) * ddy; }

    int side = 0;
    uint8_t tile = 0;
    for (int guard = 0; guard < 40; guard++) {
      if (sdx < sdy) { sdx += ddx; mx += stepX; side = 0; }
      else           { sdy += ddy; my += stepY; side = 1; }
      tile = mapAt(mx, my);
      if (tile) break;
    }

    float perp = (side == 0) ? (sdx - ddx) : (sdy - ddy);
    if (perp < 0.05f) perp = 0.05f;
    zbuf[x] = perp;

    int lh  = (int)(FOCAL / perp);
    int top = HALF_H - lh / 2;
    int bot = HALF_H + lh / 2;
    int y0  = top < 0 ? 0 : top;
    int y1  = bot >= SCR_H ? SCR_H - 1 : bot;

    int b = 17 - (int)(perp * 2.2f);
    if (side) b -= 4;
    if (tile == 2) b += 4;
    if (flash) b += 8; // Gun muzzle flash lights up the walls!
    if (b < 0) b = 0;
    if (b > 16) b = 16;

    int key = (my * MAP_W + mx) * 2 + side;
#if PANEL_LINES
    if (x > 0 && key != prevKey) b = 0;
#endif

    // In Doom Dark Sector: environment is completely dark; walls do NOT light up
    // unless the gun is fired (flash is active)! When flash fires, walls & floor fully illuminate!
    if (doomActive && isDarkSector && !flash) {
      if (top >= 0 && top < SCR_H) setPx(x, top);
      if (bot >= 0 && bot < SCR_H) setPx(x, bot);
      if (x > 0 && key != prevKey) drawVLineSafe(x, y0, y1 - y0 + 1);
    } else {
      // 3D Maze: Brick textures & Arched Exit Dungeon Door
      if (mazeActive && b > 0) {
        float wallX = (side == 0) ? (posY + perp * rdy) : (posX + perp * rdx);
        wallX -= floorf(wallX);
        if (tile == 2) {
          // Exit Door: stone jamb, vertical timber planks, iron reinforcement bands, and ring handle
          for (int y = y0; y <= y1; y++) {
            int texY = ((y - top) * 16) / (lh > 0 ? lh : 1);
            bool isLit = false;
            if (wallX < 0.12f || wallX > 0.88f || texY < 2) {
              isLit = (b > 4) && (((x + y) & 1) == 0); // stone arch
            } else {
              bool ironBand = (texY == 4 || texY == 5 || texY == 12 || texY == 13);
              bool plankSeam = (fabsf(wallX - 0.50f) < 0.035f);
              bool handle = (fabsf(wallX - 0.38f) < 0.05f && (texY == 8 || texY == 9));
              if (ironBand || handle) isLit = true;
              else if (plankSeam) isLit = false;
              else isLit = ((b > 2) && (((x ^ y) & 2) == 0));
            }
            if (isLit) setPx(x, y); else clrPx(x, y);
          }
        } else {
          // Brick masonry texture: staggered brick courses with mortar grooves
          int texX = (int)(wallX * 8.0f) & 7;
          for (int y = y0; y <= y1; y++) {
            int texY = ((y - top) * 16) / (lh > 0 ? lh : 1);
            bool isMortar = ((texY & 3) == 0);
            if (!isMortar) {
              bool alt = ((texY / 4) & 1);
              if (alt ? (texX == 4) : (texX == 0)) isMortar = true;
            }
            if (isMortar) {
              clrPx(x, y);
            } else {
              uint8_t pat = wallPat[b][x & 3];
              if (pat & (1 << (y & 7))) setPx(x, y); else clrPx(x, y);
            }
          }
        }
      } else {
        colSpan(x, y0, y1, wallPat[b][x & 3]);
      }

      if (top >= 0)    setPx(x, top);
      if (bot < SCR_H) setPx(x, bot);

      int yf = y1 + 1;                          // floor, written one byte at a time
      if (yf < SCR_H) {
        int pg = yf >> 3;
        uint8_t xm = x & 3;
        uint8_t *p = fb + pg * SCR_W + x;
        *p |= floorMask[pg][xm] & (uint8_t)(0xFF << (yf & 7));
        for (pg++; pg < 8; pg++) { p += SCR_W; *p |= floorMask[pg][xm]; }
      }
    }
    prevKey = key;
  }
}

// ================= GAME 1: DOOM-LITE =================
int      health, ammo;
bool     moving = false;
float    bobPhase = 0.0f;
uint32_t nextShot, hurtFlashUntil, startTime, finishTime, stateSince;
GState   ds = PLAYING;

void loadDoomLevel() {
  enemyCount = 0;
  for (int y = 0; y < MAP_H; y++)
    for (int x = 0; x < MAP_W; x++) {
      char c = LEVEL[y][x];
      uint8_t t = 0;
      if (c == '#') t = 1;
      else if (c == 'X') t = 2;
      else if (c == 'P') { posX = x + 0.5f; posY = y + 0.5f; }
      else if (c == 'E' && enemyCount < MAX_ENEMIES) {
        Enemy &e = enemies[enemyCount++];
        e.x = x + 0.5f; e.y = y + 0.5f;
        e.type = (enemyCount % 2 == 0 ? 0 : 1); // alternate demons and crawlers
        e.hp = (e.type == 1) ? 2 : ENEMY_HP;
        e.awake = false;
        e.hurtUntil = 0; e.nextAttack = 0;
      }
      world[y][x] = t;
    }
  angle = 0.5f;
}

void spawnDoomWave(int wave) {
  enemyCount = 0;
  int targetEnemies = imin(3 + wave, MAX_ENEMIES);
  struct Pt { uint8_t x, y; };
  Pt spots[64];
  int nSpots = 0;
  for (int y = 1; y < MAP_H - 1; y++) {
    for (int x = 1; x < MAP_W - 1; x++) {
      if (world[y][x] == 0) {
        float dx = (x + 0.5f) - posX, dy = (y + 0.5f) - posY;
        if (dx * dx + dy * dy >= 9.0f) {
          if (nSpots < 64) spots[nSpots++] = {(uint8_t)x, (uint8_t)y};
        }
      }
    }
  }
  for (int i = 0; i < targetEnemies && nSpots > 0; i++) {
    int pick = random(nSpots);
    Pt p = spots[pick];
    spots[pick] = spots[--nSpots];
    Enemy &e = enemies[enemyCount++];
    e.x = p.x + 0.5f; e.y = p.y + 0.5f;
    e.type = (random(2) == 0 ? 1 : 0);
    e.hp = (e.type == 1) ? (2 + (wave - 1) / 3) : (ENEMY_HP + (wave - 1) / 2);
    e.awake = false;
    e.hurtUntil = 0;
    e.nextAttack = 0;
  }
  enemiesAlive = enemyCount;
}

void doomReset() {
  doomWave = 1;
  isDarkSector = false;
  isRestoredBanner = false;
  waveBannerUntil = 0;
  loadDoomLevel();
  health = START_HEALTH; ammo = START_AMMO;
  enemiesAlive = enemyCount;
  ds = PLAYING;
  startTime = stateSince = now;
  nextShot = now + 300;
  flashUntil = hurtFlashUntil = recoilSince = 0;
  for (int i = 0; i < SCR_W; i++) zbuf[i] = 100.0f;
}

void wakeEnemy(Enemy &e) {
  if (!e.awake) { e.awake = true; e.nextAttack = now + 500; }
}

void shoot() {
  if (ammo <= 0) { sfx(200, 25); nextShot = now + FIRE_DELAY; return; }
  ammo--;
  nextShot = now + FIRE_DELAY;
  flashUntil = now + 120;
  recoilSince = now;
  sfx(950, 45);

  int best = -1;
  float bestF = zbuf[SCR_W / 2];
  for (int i = 0; i < enemyCount; i++) {
    Enemy &e = enemies[i];
    if (e.hp <= 0) continue;
    float rx = e.x - posX, ry = e.y - posY;
    float fwd = rx * dirX + ry * dirY;
    float lat = -rx * dirY + ry * dirX;
    float tol = (e.type == 1) ? 0.35f : 0.38f;
    if (fwd > 0.1f && fwd < bestF && fabsf(lat) < tol) { best = i; bestF = fwd; }
  }
  if (best >= 0) {
    Enemy &e = enemies[best];
    e.hp -= (int8_t)random(1, 3);
    e.hurtUntil = now + 120;
    wakeEnemy(e);
    if (e.hp <= 0) { ammo += 5; sfx(170, 160); }
    else sfx(330, 60);
  }
}

void updateEnemies() {
  int alive = 0;
  for (int i = 0; i < enemyCount; i++) {
    Enemy &e = enemies[i];
    if (e.hp <= 0) continue;
    alive++;
    float dx = posX - e.x, dy = posY - e.y;
    float d = sqrtf(dx * dx + dy * dy);

    if (!e.awake) {
      if (d < ENEMY_WAKE && lineOfSight(e.x, e.y, posX, posY)) wakeEnemy(e);
      else continue;
    }
    if (d > 0.85f) {
      float spd = (e.type == 1) ? 1.40f : ENEMY_SPEED;
      float s = spd * dt / d;
      float nx = e.x + dx * s, ny = e.y + dy * s;
      if (!blocked(nx, e.y, 0.25f)) e.x = nx;
      if (!blocked(e.x, ny, 0.25f)) e.y = ny;
    } else if ((int32_t)(now - e.nextAttack) >= 0) {
      e.nextAttack = now + (e.type == 1 ? 650 : 900);
      health -= (e.type == 1) ? (int)random(4, 9) : (int)random(6, 12);
      hurtFlashUntil = now + 150;
      sfx(110, 120);
    }
  }
  enemiesAlive = alive;
}

void renderSprites() {
  float d[MAX_ENEMIES];
  uint8_t ord[MAX_ENEMIES];
  int n = 0;
  bool flash = (doomActive && (int32_t)(now - flashUntil) < 0);

  for (int i = 0; i < enemyCount; i++) {
    if (enemies[i].hp <= 0) continue;
    float dx = enemies[i].x - posX, dy = enemies[i].y - posY;
    d[n] = dx * dx + dy * dy;
    ord[n] = i;
    n++;
  }
  for (int a = 1; a < n; a++) {             // far to near
    float kd = d[a]; uint8_t ko = ord[a];
    int b = a - 1;
    while (b >= 0 && d[b] < kd) { d[b + 1] = d[b]; ord[b + 1] = ord[b]; b--; }
    d[b + 1] = kd; ord[b + 1] = ko;
  }

  for (int k = 0; k < n; k++) {
    Enemy &e = enemies[ord[k]];
    float rx = e.x - posX, ry = e.y - posY;
    float fwd = rx * dirX + ry * dirY;
    float lat = -rx * dirY + ry * dirX;
    if (fwd < 0.25f) continue;

    // Dark mode visibility:
    // If dark mode and no muzzle flash: completely hidden beyond 5.0m;
    // between 1.4m and 5.0m, monsters smoothly fade/dither in using the Bayer matrix!
    int ditherThreshold = 16; // default 100% visible
    if (doomActive && isDarkSector && !flash) {
      if (fwd >= 5.0f) continue;
      if (fwd > 1.4f) {
        float vis = (5.0f - fwd) / 3.6f; // 0.0 at 5.0m, 1.0 at 1.4m
        ditherThreshold = (int)(vis * 16.0f);
        if (ditherThreshold < 1) ditherThreshold = 1;
      }
    }

    int sx = (int)((SCR_W / 2) * (1.0f + lat / (fwd * FOV_PLANE)));
    float heightScale = (e.type == 1) ? 0.55f : 0.7f;
    int size = (int)(FOCAL * heightScale / fwd);
    if (size < 2) continue;

    int bottom = HALF_H + (int)(FOCAL * 0.5f / fwd);
    int top    = bottom - size;
    int left   = sx - size / 2;
    bool hurt  = (int32_t)(now - e.hurtUntil) < 0;

    int xa = imax(left, 0), xb = imin(left + size, SCR_W);
    int ya = imax(top, 0),  yb = imin(bottom, SCR_H);
    if (xa >= xb || ya >= yb) continue;

    // Animation frame: 2-frame walking / scurrying
    bool animFrame = ((now / (e.type == 1 ? 140 : 220)) & 1);
    const char* const (*spr)[16];
    if (e.type == 1) {
      spr = (e.awake && animFrame) ? &CRAWLER_B : &CRAWLER;
    } else {
      spr = (e.awake && animFrame) ? &MONSTER_B : &MONSTER;
    }

    for (int x = xa; x < xb; x++) {
      if (fwd >= zbuf[x]) continue;
      int tx = ((x - left) * 16) / size;
      for (int y = ya; y < yb; y++) {
        if (ditherThreshold < 16) {
          if (BAYER[y & 3][x & 3] >= ditherThreshold) continue;
        }
        int ty = ((y - top) * 16) / size;
        char c = (*spr)[ty][tx];
        if (c == '.') continue;
        bool white = (c == '#');
        if (hurt) white = !white;
        if (white) setPx(x, y); else clrPx(x, y);
      }
    }
  }
}

void drawGun() {
  int oy = moving ? (int)(fabsf(sinf(bobPhase)) * 2.0f) : 0;
  bool flash = (int32_t)(now - flashUntil) < 0;

  // Recoil kick & gun barrel bending animation
  int kickY = 0;
  int muzzleRise = 0;
  uint32_t rTime = now - recoilSince;
  if (rTime < 220) {
    if (rTime < 50) {
      kickY = (int)(6.0f * (1.0f - rTime / 50.0f));
      muzzleRise = (int)(3.0f * (1.0f - rTime / 50.0f));
    } else {
      float t = (rTime - 50) / 170.0f;
      kickY = (int)(4.0f * (1.0f - t) * sinf(t * 3.14159f));
      muzzleRise = (int)(2.0f * (1.0f - t));
    }
  }
  oy += kickY;

  // Muzzle flash: multi-point blast star with blast core and sparks
  if (flash) {
    u8g2.setDrawColor(1);
    int fy = 38 + oy - muzzleRise;
    u8g2.drawDisc(64, fy, 8, U8G2_DRAW_ALL);
    u8g2.drawLine(64, fy - 10, 64, fy + 10);
    u8g2.drawLine(52, fy, 76, fy);
    u8g2.drawLine(55, fy - 6, 73, fy + 6);
    u8g2.drawLine(55, fy + 6, 73, fy - 6);
    u8g2.setDrawColor(0);
    u8g2.drawDisc(64, fy, 3, U8G2_DRAW_ALL);
    u8g2.setDrawColor(1);
    drawPixelSafe(58, fy - 7);
    drawPixelSafe(70, fy - 7);
    drawPixelSafe(64, fy - 11);
    drawPixelSafe(53, fy - 3);
    drawPixelSafe(75, fy - 3);
  }

  // Clear background silhouette under gun
  u8g2.setDrawColor(0);
  u8g2.drawBox(54, 45 + oy - muzzleRise, 20, 20);
  u8g2.setDrawColor(1);

  // Twin shotgun barrels with kick & muzzle rise
  int by = 47 + oy - muzzleRise;
  u8g2.drawBox(57 - muzzleRise / 2, by, 5 + muzzleRise / 2, 10 + muzzleRise);
  u8g2.drawBox(66, by, 5 + muzzleRise / 2, 10 + muzzleRise);

  // Twin muzzle bore openings at the tip
  u8g2.setDrawColor(0);
  u8g2.drawBox(58 - muzzleRise / 2, by, 3, 2);
  u8g2.drawBox(67, by, 3, 2);
  // Center rib divider between barrels
  u8g2.drawVLine(64, by, 10 + muzzleRise);
  u8g2.setDrawColor(1);

  // Vented rib cooling slots and front bead sight
  u8g2.drawPixel(64, by - 1); // Front brass bead sight
  u8g2.setDrawColor(0);
  u8g2.drawPixel(64, by + 3); // Vent port 1
  u8g2.drawPixel(64, by + 6); // Vent port 2
  u8g2.setDrawColor(1);

  // Receiver & breech chamber
  u8g2.drawBox(55, 56 + oy, 18, 8);
  u8g2.setDrawColor(0);
  u8g2.drawVLine(64, 56 + oy, 6);
  u8g2.drawHLine(57, 59 + oy, 14);
  u8g2.setDrawColor(1);

  // Stock grip with cross-hatch checkering
  u8g2.drawBox(57, 62 + oy, 14, 4);
  u8g2.setDrawColor(0);
  u8g2.drawPixel(59, 63 + oy);
  u8g2.drawPixel(62, 63 + oy);
  u8g2.drawPixel(65, 63 + oy);
  u8g2.drawPixel(68, 63 + oy);
  u8g2.setDrawColor(1);
}

void gameDoom() {
  startGame();
  doomActive = true;
  doomReset();
  while (tick()) {
    bool up = held(K_UP), down = held(K_DOWN), left = held(K_LEFT), right = held(K_RIGHT);
    bool a = held(K_A), b = held(K_B);

    if (ds == PLAYING) {
      float strafe = 0.0f;
      if (b) strafe = (right ? 1.0f : 0.0f) - (left ? 1.0f : 0.0f);
      else {
        float turn = (right ? 1.0f : 0.0f) - (left ? 1.0f : 0.0f);
        angle += turn * TURN_SPEED * dt;
        if (angle > TWO_PI) angle -= TWO_PI;
        else if (angle < 0) angle += TWO_PI;
      }
      dirX = cosf(angle);
      dirY = sinf(angle);

      float fwd = (up ? 1.0f : 0.0f) - (down ? 1.0f : 0.0f);
      moving = (fwd != 0.0f) || (strafe != 0.0f);
      if (moving) {
        bobPhase += dt * 9.0f;
        float mx = dirX * fwd * MOVE_SPEED + (-dirY) * strafe * STRAFE_SPEED;
        float my = dirY * fwd * MOVE_SPEED + ( dirX) * strafe * STRAFE_SPEED;
        tryMove(mx * dt, my * dt);
      }
      if (a && (int32_t)(now - nextShot) >= 0) shoot();
      updateEnemies();

      if (health <= 0) {
        ds = DEAD;
        stateSince = now;
        hiTime("doom", doomWave);
        sfx(90, 400);
      } else if (enemiesAlive == 0) {
        bool wasDark = isDarkSector;
        doomWave++;
        ammo = imin(START_AMMO + 10, ammo + 18);
        health = imin(START_HEALTH, health + 25);
        sfx(1400, 250);
        isDarkSector = (doomWave % 2 == 0);
        isRestoredBanner = wasDark && !isDarkSector;
        spawnDoomWave(doomWave);
        waveBannerUntil = now + 1200;
        hiTime("doom", doomWave);
      }
    } else if (a && (now - stateSince) > 800) doomReset();

    u8g2.clearBuffer();
    fb = u8g2.getBufferPtr();
    renderWorld();
    renderSprites();
    drawGun();

    if (ds == PLAYING) {
      u8g2.setDrawColor(2);
      u8g2.drawPixel(64, 29); u8g2.drawPixel(64, 35);
      u8g2.drawPixel(61, 32); u8g2.drawPixel(67, 32);
      u8g2.setDrawColor(1);
    }

    char buf[24];
    u8g2.setFont(u8g2_font_5x7_tr);
    snprintf(buf, sizeof(buf), "HP %d", health < 0 ? 0 : health);  drawLabel(0, 63, buf, false);
    snprintf(buf, sizeof(buf), "AM %d", ammo);                       drawLabel(0, 63, buf, true);
    snprintf(buf, sizeof(buf), isDarkSector ? "W:%d [DARK]" : "W:%d FOES %d", doomWave, enemiesAlive);
    drawLabel(0, 7, buf, false);
#if SHOW_FPS
    snprintf(buf, sizeof(buf), "%d fps", fps);                       drawLabel(0, 7, buf, true);
#endif

    if ((int32_t)(now - hurtFlashUntil) < 0) {
      u8g2.setDrawColor(2);
      u8g2.drawFrame(0, 0, SCR_W, SCR_H);
      u8g2.drawFrame(1, 1, SCR_W - 2, SCR_H - 2);
      u8g2.setDrawColor(1);
    }

    if (ds == DEAD) {
      snprintf(buf, sizeof(buf), "Wave %d reached", doomWave);
      drawMessage("YOU DIED", buf);
    } else if (ds == PLAYING && now < waveBannerUntil) {
      if (isDarkSector || isRestoredBanner) {
        // Plain centered text only, blink twice, NO box!
        uint32_t elapsed = 1200 - (waveBannerUntil - now);
        bool blinkOn = (elapsed < 350) || (elapsed >= 550 && elapsed < 900);
        if (blinkOn) {
          u8g2.setFont(u8g2_font_7x14_tf);
          const char* alert = isDarkSector ? "LIGHTS OUT" : "LIGHT RESTORED";
          int tw = u8g2.getStrWidth(alert);
          u8g2.drawStr((SCR_W - tw) / 2, 34, alert);
        }
      } else {
        char wbuf[20];
        snprintf(wbuf, sizeof(wbuf), "WAVE %d START", doomWave);
        drawMessage("WAVE CLEARED!", wbuf);
      }
    }
    present();
  }
  doomActive = false;
  isDarkSector = false;
  isRestoredBanner = false;
}

// ================= GAME 2: 3D MAZE =================
void mazeGenerate() {
  memset(world, 1, sizeof(world));
  static const int8_t DX[4] = {2, -2, 0, 0}, DY[4] = {0, 0, 2, -2};
  uint8_t stack[64];
  int sp = 0;
  world[1][1] = 0;
  stack[sp++] = 1 * 16 + 1;
  while (sp) {
    int cur = stack[sp - 1], cx = cur & 15, cy = cur >> 4;
    int dirs[4], n = 0;
    for (int d = 0; d < 4; d++) {
      int nx = cx + DX[d], ny = cy + DY[d];
      if (nx > 0 && nx < 14 && ny > 0 && ny < 14 && world[ny][nx] == 1) dirs[n++] = d;
    }
    if (!n) { sp--; continue; }
    int d = dirs[random(n)];
    world[cy + DY[d] / 2][cx + DX[d] / 2] = 0;
    world[cy + DY[d]][cx + DX[d]] = 0;
    stack[sp++] = (cy + DY[d]) * 16 + (cx + DX[d]);
  }
  world[13][14] = 2;                        // bright exit door next to the last cell
}

void mazeReset() {
  mazeGenerate();
  posX = 1.5f; posY = 1.5f;
  angle = (world[1][2] == 0) ? 0.0f : HALF_PI;
  dirX = cosf(angle); dirY = sinf(angle);
  for (int i = 0; i < SCR_W; i++) zbuf[i] = 100.0f;
}

void drawMiniMap() {
  const int ox = 94, oy = 10;
  u8g2.setDrawColor(0);
  u8g2.drawBox(ox - 1, oy - 1, 34, 34);
  u8g2.setDrawColor(1);
  u8g2.drawFrame(ox - 1, oy - 1, 34, 34);
  for (int y = 0; y < MAP_H; y++)
    for (int x = 0; x < MAP_W; x++) {
      uint8_t t = world[y][x];
      bool on = (t == 1) || (t == 2 && ((now >> 8) & 1));   // exit blinks
      if (!on) continue;
      int px = ox + x * 2, py = oy + y * 2;
      setPx(px, py); setPx(px + 1, py); setPx(px, py + 1); setPx(px + 1, py + 1);
    }
  int ppx = ox + (int)(posX * 2.0f), ppy = oy + (int)(posY * 2.0f);
  u8g2.setDrawColor(2);
  u8g2.drawBox(ppx - 1, ppy - 1, 3, 3);
  u8g2.drawPixel(ppx + (int)(dirX * 4.0f), ppy + (int)(dirY * 4.0f));
  u8g2.setDrawColor(1);
}

void gameMaze() {
  startGame();
  mazeActive = true;
  GState st = PLAYING;
  uint32_t t0, since = 0, finish = 0;
  mazeReset();
  t0 = now = millis();

  while (tick()) {
    if (st == PLAYING) {
      float turn = (held(K_RIGHT) ? 1.0f : 0.0f) - (held(K_LEFT) ? 1.0f : 0.0f);
      angle += turn * TURN_SPEED * dt;
      if (angle > TWO_PI) angle -= TWO_PI;
      else if (angle < 0) angle += TWO_PI;
      dirX = cosf(angle);
      dirY = sinf(angle);
      float fwd = (held(K_UP) ? 1.0f : 0.0f) - (held(K_DOWN) ? 1.0f : 0.0f);
      if (fwd != 0.0f) tryMove(dirX * fwd * MOVE_SPEED * dt, dirY * fwd * MOVE_SPEED * dt);

      if (posX >= 13.0f && posY >= 13.0f) {
        st = WON; since = now; finish = now - t0;
        hiTime("maze", finish / 100);
        sfx(784, 300);
      }
    } else if (pressed(K_A) && now - since > 700) {
      mazeReset(); t0 = now; st = PLAYING;
    }

    u8g2.clearBuffer();
    fb = u8g2.getBufferPtr();
    renderWorld();

    uint32_t el = (st == PLAYING) ? now - t0 : finish;
    char buf[20];
    u8g2.setFont(u8g2_font_5x7_tr);
    snprintf(buf, sizeof(buf), "T %lu.%lu", (unsigned long)(el / 1000), (unsigned long)((el / 100) % 10));
    drawLabel(0, 7, buf, false);
#if SHOW_FPS
    snprintf(buf, sizeof(buf), "%d fps", fps);
    drawLabel(0, 7, buf, true);
#endif
    if (st == PLAYING) {
      if (now - t0 < 6000) drawLabel(0, 63, "HOLD B: MAP", false);
      if (held(K_B)) drawMiniMap();
    } else {
      snprintf(buf, sizeof(buf), "Time %lu.%lus", (unsigned long)(finish / 1000), (unsigned long)((finish / 100) % 10));
      drawMessage("ESCAPED!", buf);
    }
    present();
  }
  mazeActive = false;
}

// ================= GAME 3: RACER =================
#define NCARS      3
#define R_MAXSPD   5.0f
struct Car { float z, lane; };
Car   cars[NCARS];
float rPos, rCamX, rCurve;

static inline float curveAt(float p) { return 1.3f * sinf(p * 0.30f) * sinf(p * 0.071f + 0.5f); }
static inline float randLane() { return (float)((int)random(3) - 1) * 0.5f; }

void drawTraffic() {
  uint8_t idx[NCARS]; float dz[NCARS]; int n = 0;
  for (int i = 0; i < NCARS; i++) {
    float d = cars[i].z - rPos;
    if (d > 0.22f && d < 9.0f) { dz[n] = d; idx[n] = i; n++; }
  }
  for (int a = 1; a < n; a++) {             // far first
    float kd = dz[a]; uint8_t ki = idx[a];
    int b = a - 1;
    while (b >= 0 && dz[b] < kd) { dz[b + 1] = dz[b]; idx[b + 1] = idx[b]; b--; }
    dz[b + 1] = kd; idx[b + 1] = ki;
  }
  for (int k = 0; k < n; k++) {
    int t = (int)(10.0f / dz[k]);
    if (t < 2) continue;
    int y = HOR + t;
    float p = t / (float)(SCR_H - 1 - HOR);
    int hw = (int)(p * 56.0f);
    float q = (1.0f - p) * (1.0f - p) * 50.0f;
    int cx = 64 + (int)(rCurve * q) - (int)(rCamX * hw) + (int)(cars[idx[k]].lane * hw);
    int w = (hw * 9) / 25; if (w < 3) w = 3;
    int h = (w * 3) / 4;   if (h < 2) h = 2;
    int x0 = cx - w / 2, y0 = y - h;

    // Skip traffic vehicles completely outside display
    if (x0 + w <= 0 || x0 >= SCR_W || y0 + h <= 0 || y0 >= SCR_H) continue;
    
    // Clear silhouette space
    u8g2.setDrawColor(0);
    drawBoxSafe(x0 - 1, y0 - 1, w + 2, h + 2);
    u8g2.setDrawColor(1);
    
    if (w < 7) {
      drawBoxSafe(x0, y0, w, h);
    } else {
      // Aerodynamic roof and cabin
      int rw = (w * 3) / 5;
      int rx0 = cx - rw / 2;
      int rh = h / 2;
      drawBoxSafe(rx0, y0, rw, rh);
      // Rear windshield
      u8g2.setDrawColor(0);
      drawBoxSafe(rx0 + 1, y0 + 1, rw - 2, rh - 1);
      u8g2.setDrawColor(1);
      // Main car body
      drawBoxSafe(x0, y0 + rh, w, h - rh);
      // Spoiler lip
      drawHLineSafe(x0, y0 + rh, w);
      // Dual distinct taillights on outer corners
      int tw = imax(1, w / 6);
      int th = imax(1, (h - rh) / 2);
      u8g2.setDrawColor(0);
      drawBoxSafe(x0 + 1 + tw, y0 + rh + 1, w - 2 - 2 * tw, th);
      u8g2.setDrawColor(1);
      // Underbody tires
      drawBoxSafe(x0 - 1, y0 + h - 1, imax(1, w / 5), 2);
      drawBoxSafe(x0 + w - imax(1, w / 5) + 1, y0 + h - 1, imax(1, w / 5), 2);
    }
  }
}

void drawPlayerCar(int lean) {
  int x = 64 + lean;
  // Clear bounding box behind player vehicle
  u8g2.setDrawColor(0);
  drawBoxSafe(x - 14, 48, 29, 16);
  u8g2.setDrawColor(1);

  // Aerodynamic raised rear wing / spoiler
  drawBoxSafe(x - 13, 49, 27, 2);
  drawVLineSafe(x - 9, 51, 3);
  drawVLineSafe(x + 9, 51, 3);

  // Fastback cabin & rear louvered windshield
  drawBoxSafe(x - 7, 51, 15, 6);
  u8g2.setDrawColor(0);
  drawBoxSafe(x - 5, 52, 11, 4);
  u8g2.setDrawColor(1);
  drawHLineSafe(x - 4, 53, 9);
  drawHLineSafe(x - 3, 55, 7);

  // Wide sports car body shell
  drawBoxSafe(x - 12, 56, 25, 5);

  // Horizontal Testarossa ribbed tail light bar
  u8g2.setDrawColor(0);
  drawHLineSafe(x - 10, 57, 21);
  drawHLineSafe(x - 10, 59, 21);
  drawBoxSafe(x - 3, 58, 7, 3); // License plate recess
  u8g2.setDrawColor(1);

  // Wide treaded racing tires with rims
  drawBoxSafe(x - 14, 57, 3, 7);
  drawBoxSafe(x + 12, 57, 3, 7);
  u8g2.setDrawColor(0);
  drawVLineSafe(x - 13, 59, 3);
  drawVLineSafe(x + 13, 59, 3);
  u8g2.setDrawColor(1);

  // Dual twin exhaust tips
  drawFrameSafe(x - 8, 61, 3, 2);
  drawFrameSafe(x + 6, 61, 3, 2);

  // Dynamic exhaust flames when pressing gas (A button)
  if (held(K_A) && ((now >> 4) & 1)) {
    drawPixelSafe(x - 7, 63);
    drawPixelSafe(x + 7, 63);
    if ((now >> 5) & 1) {
      drawPixelSafe(x - 7, 64);
      drawPixelSafe(x + 7, 64);
    }
  }
}

void gameRacer() {
  startGame();
  float speed, timeLeft, nextCheck, bgScroll = 0;
  int score = 0;
  GState st;
  uint32_t since = 0, crashUntil = 0, msgUntil = 0;

  auto reset = [&]() {
    rPos = 0; rCamX = 0; speed = 0; timeLeft = 30.0f; nextCheck = 64.0f;
    score = 0; st = PLAYING; crashUntil = msgUntil = 0;
    for (int i = 0; i < NCARS; i++) { cars[i].z = 4.0f + i * 3.0f + random(0, 10) * 0.1f; cars[i].lane = randLane(); }
  };
  reset();

  while (tick()) {
    if (st == PLAYING) {
      float spdN = speed / R_MAXSPD;
      if (held(K_A)) speed += 2.2f * dt; else speed -= 0.6f * dt;
      if (held(K_B)) speed -= 5.0f * dt;
      if (fabsf(rCamX) > 1.0f && speed > 1.6f) speed -= 5.0f * dt;     // off-road drag
      speed = clampf(speed, 0.0f, R_MAXSPD);

      float steer = (held(K_RIGHT) ? 1.0f : 0.0f) - (held(K_LEFT) ? 1.0f : 0.0f);
      float cNow = curveAt(rPos);
      rCamX += (steer * 1.7f * (0.35f + spdN) - cNow * 0.9f * spdN) * dt;
      rCamX = clampf(rCamX, -1.8f, 1.8f);
      rPos += speed * dt;
      bgScroll += cNow * spdN * dt * 40.0f;
      timeLeft -= dt;
      score = (int)(rPos * 10.0f);

      if (rPos >= nextCheck) { nextCheck += 64.0f; timeLeft += 11.0f; msgUntil = now + 1500; sfx(1200, 120); }

      for (int i = 0; i < NCARS; i++) {
        cars[i].z += 1.5f * dt;
        float d = cars[i].z - rPos;
        if (d < -1.0f) { cars[i].z = rPos + 8.0f + random(0, 60) * 0.1f; cars[i].lane = randLane(); }
        else if (d < 0.45f && d > -0.3f && fabsf(cars[i].lane - rCamX) < 0.3f && now > crashUntil) {
          speed *= 0.3f; crashUntil = now + 600; sfx(100, 200);
          cars[i].z = rPos - 2.0f;                                     // car is smashed away
          fxSpawn(64, 55, random(7, 13), 28.0f, 75.0f);               // crash debris/sparks
        }
      }
      if (timeLeft <= 0) {
        timeLeft = 0; st = DEAD; since = now; hiScore("racer", score); sfx(90, 400);
      }
    } else if (pressed(K_A) && now - since > 700) reset();

    // ---------- draw ----------
    u8g2.clearBuffer();
    fb = u8g2.getBufferPtr();

    int sc = ((int)bgScroll) & 1023;
    for (int x = 0; x < SCR_W; x++) {       // dithered mountains
      int a = (x + sc) & 31;           int ta = a < 16 ? a : 31 - a;
      int bb = (x + sc / 2 + 11) % 23; int tb = bb < 12 ? bb : 22 - bb;
      int h = (ta >> 1) + (tb >> 1) + 1;
      colSpan(x, HOR - h, HOR, (x & 1) ? 0x55 : 0xAA);
      setPx(x, HOR - h);
    }
    hspan(fb + (HOR >> 3) * SCR_W, 1 << (HOR & 7), 0, SCR_W - 1);       // horizon line

    rCurve = curveAt(rPos + 1.5f);
    for (int y = HOR + 1; y < SCR_H; y++) {
      int hw = rowHW[y];
      int cx = 64 + (int)(rCurve * rowQ[y]) - (int)(rCamX * hw);
      bool ph = ((int)((rowD[y] + rPos) * 2.0f)) & 1;
      uint8_t *row = fb + (y >> 3) * SCR_W;
      uint8_t m = 1 << (y & 7);
      int L = cx - hw, R = cx + hw;
      int rum = (hw >> 3) + 1;
      if (!ph) {
        if (!(y & 1)) { hspanDither(row, m, 0, L - rum - 1); hspanDither(row, m, R + rum + 1, SCR_W - 1); }
        hspan(row, m, L - rum, L - 1);
        hspan(row, m, R + 1, R + rum);
        int w = hw >> 5;
        hspan(row, m, cx - w, cx + w);       // centre dashes
      }
      hspan(row, m, L, L);
      hspan(row, m, R, R);
    }

    drawTraffic();
    float steerNow = (held(K_RIGHT) ? 1.0f : 0.0f) - (held(K_LEFT) ? 1.0f : 0.0f);
    if (!((int32_t)(now - crashUntil) < 0 && ((now >> 6) & 1))) drawPlayerCar((int)(steerNow * 2.0f));
    fxUpdateAndDraw(dt);

    char buf[20];
    u8g2.setFont(u8g2_font_5x7_tr);
    snprintf(buf, sizeof(buf), "T %d", (int)timeLeft);   drawLabel(0, 7, buf, false);
    snprintf(buf, sizeof(buf), "%dm", score);            drawLabel(0, 7, buf, true);
    snprintf(buf, sizeof(buf), "%d", (int)(speed * 40.0f)); drawLabel(0, 63, buf, false);
#if SHOW_FPS
    snprintf(buf, sizeof(buf), "%dfps", fps);            drawLabel(0, 63, buf, true);
#endif
    if (st == PLAYING && (int32_t)(now - msgUntil) < 0) drawLabel(38, 20, "TIME EXTENDED", false);
    if (st == DEAD) {
      snprintf(buf, sizeof(buf), "Score %d", score);
      drawMessage("TIME UP", buf);
    }
    present();
  }
}

// ================= GAME 4: SNAKE =================
#define SN_W 31
#define SN_H 13
uint8_t snx[SN_W * SN_H + 1], sny[SN_W * SN_H + 1];

void gameSnake() {
  startGame();
  static const int8_t DX[4] = {0, 1, 0, -1}, DY[4] = {-1, 0, 1, 0};
  int len = 4, dir = 1, nd = 1, score = 0, fx = 0, fy = 0;
  uint32_t interval = 140, nextMove = 0, since = 0;
  GState st = PLAYING;
  bool dirty = true;

  auto onSnake = [&](int x, int y) {
    for (int i = 0; i < len; i++) if (snx[i] == x && sny[i] == y) return true;
    return false;
  };
  auto placeFood = [&]() {
    do { fx = random(SN_W); fy = random(SN_H); } while (onSnake(fx, fy));
  };
  auto reset = [&]() {
    len = 4; dir = nd = 1; score = 0; interval = 140; st = PLAYING;
    for (int i = 0; i < len; i++) { snx[i] = 15 - i; sny[i] = 6; }
    placeFood();
    nextMove = now + 400; dirty = true;
  };
  reset();

  while (tick()) {
    if (st == PLAYING) {
      if (pressed(K_UP)    && dir != 2) nd = 0;
      if (pressed(K_RIGHT) && dir != 3) nd = 1;
      if (pressed(K_DOWN)  && dir != 0) nd = 2;
      if (pressed(K_LEFT)  && dir != 1) nd = 3;

      if ((int32_t)(now - nextMove) >= 0) {
        nextMove = now + interval;
        dir = nd;
        int hx = snx[0] + DX[dir], hy = sny[0] + DY[dir];
        bool eat = (hx == fx && hy == fy);
        bool dead = (hx < 0 || hy < 0 || hx >= SN_W || hy >= SN_H);
        if (!dead) {
          int lim = eat ? len : len - 1;
          for (int i = 0; i < lim; i++) if (snx[i] == hx && sny[i] == hy) { dead = true; break; }
        }
        if (dead) {
          st = DEAD; since = now; hiScore("snake", score); sfx(100, 300);
        } else {
          memmove(&snx[1], &snx[0], len);
          memmove(&sny[1], &sny[0], len);
          snx[0] = hx; sny[0] = hy;
          if (eat) {
            len++; score++; sfx(900, 40);
            if (interval > 55) interval -= 3;
            if (len >= SN_W * SN_H) { st = WON; since = now; hiScore("snake", score); }
            else placeFood();
          }
        }
        dirty = true;
      }
    } else if (pressed(K_A) && now - since > 600) reset();

    if (comboSince) dirty = true;
    if (!dirty) { delay(1); continue; }
    dirty = false;

    u8g2.clearBuffer();
    u8g2.drawFrame(0, 8, SCR_W, 56);

    // Food: Apple with stem and shine
    int ax = 2 + fx * 4, ay = 10 + fy * 4;
    u8g2.drawBox(ax, ay + 1, 4, 3);
    u8g2.drawPixel(ax + 1, ay);     // stem base
    u8g2.drawPixel(ax + 2, ay - 1); // stem leaf
    u8g2.setDrawColor(0);
    u8g2.drawPixel(ax, ay + 3);     // rounded bottom left
    u8g2.drawPixel(ax + 3, ay + 3); // rounded bottom right
    u8g2.drawPixel(ax + 1, ay + 1); // apple shine highlight
    u8g2.setDrawColor(1);

    // Snake Tail: tapered tip
    if (len > 1) {
      int tx = 2 + snx[len - 1] * 4, ty = 10 + sny[len - 1] * 4;
      u8g2.drawBox(tx + 1, ty + 1, 2, 2);
    }

    // Snake Body: textured reptile scale segments
    for (int i = len - 2; i >= 1; i--) {
      int bx = 2 + snx[i] * 4, by = 10 + sny[i] * 4;
      u8g2.drawBox(bx, by, 3, 3);
      if (i & 1) {
        u8g2.setDrawColor(0);
        u8g2.drawPixel(bx + 1, by + 1);
        u8g2.setDrawColor(1);
      }
    }

    // Snake Head: rounded head with directional eyes & flicking tongue
    int hx = 2 + snx[0] * 4, hy = 10 + sny[0] * 4;
    u8g2.drawBox(hx, hy, 4, 4);
    u8g2.setDrawColor(0);
    if (dir == 0) { // UP
      u8g2.drawPixel(hx, hy); u8g2.drawPixel(hx + 3, hy);
    } else if (dir == 1) { // RIGHT
      u8g2.drawPixel(hx + 3, hy); u8g2.drawPixel(hx + 3, hy + 3);
    } else if (dir == 2) { // DOWN
      u8g2.drawPixel(hx, hy + 3); u8g2.drawPixel(hx + 3, hy + 3);
    } else { // LEFT
      u8g2.drawPixel(hx, hy); u8g2.drawPixel(hx, hy + 3);
    }
    u8g2.setDrawColor(1);
    // Flicking tongue
    if (dir == 0) { u8g2.drawPixel(hx + 1, hy - 1); u8g2.drawPixel(hx + 2, hy - 1); }
    else if (dir == 1) { u8g2.drawPixel(hx + 4, hy + 1); u8g2.drawPixel(hx + 4, hy + 2); }
    else if (dir == 2) { u8g2.drawPixel(hx + 1, hy + 4); u8g2.drawPixel(hx + 2, hy + 4); }
    else if (dir == 3) { u8g2.drawPixel(hx - 1, hy + 1); u8g2.drawPixel(hx - 1, hy + 2); }
    char buf[20];
    u8g2.setFont(u8g2_font_5x7_tr);
    snprintf(buf, sizeof(buf), "SCORE %d", score);              u8g2.drawStr(2, 7, buf);
    snprintf(buf, sizeof(buf), "BEST %lu", (unsigned long)hiGet("snake") > (unsigned long)score ? (unsigned long)hiGet("snake") : (unsigned long)score);
    u8g2.drawStr(SCR_W - u8g2.getStrWidth(buf) - 2, 7, buf);
    if (st != PLAYING) {
      snprintf(buf, sizeof(buf), "Score %d", score);
      drawMessage(st == WON ? "PERFECT!" : "GAME OVER", buf);
    }
    present();
  }
}

// ================= GAME 5: PONG =================
void gamePong() {
  startGame();
  const int PH = 14;
  float py = 25, cy = 25, bx = 64, by = 32, vx = 70, vy = 0;
  int ps = 0, cs = 0, phase = 0;           // phase: 0 serve, 1 play, 2 game over
  uint32_t since = now, invertUntil = 0;

  auto serve = [&](int d) {
    bx = 64; by = 32; vx = d * 70.0f; vy = (float)random(-40, 41);
    phase = 0; since = now;
  };
  serve(1);

  while (tick()) {
    if (phase != 2) {
      float mv = (held(K_DOWN) ? 1.0f : 0.0f) - (held(K_UP) ? 1.0f : 0.0f);
      py = clampf(py + mv * 75.0f * dt, 0, SCR_H - PH);

      float target = (vx > 0) ? by - PH / 2 : 32 - PH / 2;               // CPU AI
      float diff = target - cy, sp = 52.0f * dt;
      if (diff > 2) cy += fminf(diff, sp); else if (diff < -2) cy -= fminf(-diff, sp);
      cy = clampf(cy, 0, SCR_H - PH);

      if (phase == 0) { if (now - since > 800) phase = 1; }
      else {
        bx += vx * dt; by += vy * dt;
        if (by < 1)  { by = 1;  vy = -vy; }
        if (by > 62) { by = 62; vy = -vy; }

        if (vx < 0 && bx < 8 && bx > -1 && by >= py - 1 && by <= py + PH + 1) {
          bx = 8; vx = -vx * 1.06f; vy = (by - (py + PH / 2)) * 9.0f; sfx(600, 25);
        }
        if (vx > 0 && bx > 119 && bx < 129 && by >= cy - 1 && by <= cy + PH + 1) {
          bx = 119; vx = -vx * 1.06f; vy = (by - (cy + PH / 2)) * 9.0f; sfx(500, 25);
        }
        vx = clampf(vx, -150, 150);
        vy = clampf(vy, -110, 110);

        if (bx < -4)  { cs++; sfx(150, 150); invertUntil = now + 240; serve(-1); }
        if (bx > 132) { ps++; sfx(900, 100); invertUntil = now + 240; serve(1); }
        if (ps >= 7 || cs >= 7) { phase = 2; since = now; sfx(ps > cs ? 1000 : 120, 300); }
      }
    } else if (pressed(K_A) && now - since > 700) { ps = cs = 0; py = cy = 25; serve(1); }

    u8g2.clearBuffer();
    for (int y = 0; y < SCR_H; y += 4) u8g2.drawVLine(63, y, 2);
    u8g2.drawBox(3, (int)py, 3, PH);
    u8g2.drawBox(122, (int)cy, 3, PH);
    if (phase != 0 || ((now >> 7) & 1)) u8g2.drawBox((int)bx - 1, (int)by - 1, 3, 3);
    char buf[12];
    u8g2.setFont(u8g2_font_6x10_tr);
    snprintf(buf, sizeof(buf), "%d", ps); u8g2.drawStr(50, 10, buf);
    snprintf(buf, sizeof(buf), "%d", cs); u8g2.drawStr(72, 10, buf);
    if (phase == 2) drawMessage(ps > cs ? "YOU WIN!" : "CPU WINS", "A: play again");
    if ((int32_t)(now - invertUntil) < 0) {
      uint8_t *b = u8g2.getBufferPtr();
      for (int i = 0; i < SCR_W * SCR_H / 8; i++) b[i] ^= 0xFF;
    }
    present();
  }
}

// ================= GAME 6: BREAKOUT =================
void gameBreakout() {
  startGame();
  struct Ball { float x, y, vx, vy; bool active; };
  static const int MAX_BALLS = 6;
  Ball balls[MAX_BALLS];

  struct PowerUp { float x, y, vy; uint8_t type; bool active; };
  static const int MAX_PWRS = 4;
  PowerUp pwrs[MAX_PWRS];

  struct Laser { float x, y; bool active; };
  Laser lasers[4];
  for (int i = 0; i < 4; i++) lasers[i].active = false;
  uint32_t nextLaserShot = 0;

  int level = 1;
  int numRows = 4, numCols = 8;
  float brickW = 14.0f, brickH = 4.0f, pitchX = 16.0f, startX = 1.0f;
  uint16_t rows[6];
  int leftBricks = 0;

  float pad = 54.0f, basePadW = 20.0f, baseSpeed = 72.0f;
  int lives = 3, score = 0;
  bool stuck = true;
  GState st = PLAYING;
  uint32_t since = 0, levelBannerUntil = 0;
  uint32_t wideUntil = 0, laserUntil = 0, slowUntil = 0;

  auto setupLevel = [&](int lvl) {
    level = lvl;
    if (lvl == 1) {
      numRows = 4; numCols = 8; brickW = 14.0f; brickH = 4.0f; pitchX = 16.0f; startX = 1.0f;
    } else if (lvl == 2) {
      numRows = 5; numCols = 10; brickW = 10.0f; brickH = 3.0f; pitchX = 12.0f; startX = 4.0f;
    } else if (lvl == 3) {
      numRows = 5; numCols = 12; brickW = 8.0f; brickH = 3.0f; pitchX = 10.0f; startX = 4.0f;
    } else if (lvl == 4) {
      numRows = 6; numCols = 14; brickW = 7.0f; brickH = 3.0f; pitchX = 9.0f; startX = 1.0f;
    } else {
      numRows = 6; numCols = 16; brickW = 6.0f; brickH = 3.0f; pitchX = 8.0f; startX = 0.0f;
    }

    uint16_t colMask = (1 << numCols) - 1;
    leftBricks = numRows * numCols;
    for (int r = 0; r < 6; r++) rows[r] = (r < numRows) ? colMask : 0;

    for (int i = 0; i < MAX_BALLS; i++) balls[i].active = false;
    float curPadW = (now < wideUntil) ? 28.0f : basePadW;
    balls[0].active = true;
    balls[0].x = pad + curPadW * 0.5f;
    balls[0].y = 57.0f;
    balls[0].vx = 0;
    balls[0].vy = 0;
    stuck = true;

    for (int i = 0; i < MAX_PWRS; i++) pwrs[i].active = false;
    for (int i = 0; i < 4; i++) lasers[i].active = false;
  };

  auto spawnPowerUp = [&](float x, float y) {
    if (random(100) >= (26 + level * 3)) return;
    for (int i = 0; i < MAX_PWRS; i++) {
      if (!pwrs[i].active) {
        pwrs[i].x = x; pwrs[i].y = y; pwrs[i].vy = 28.0f;
        int r = random(100);
        if (r < 30)      pwrs[i].type = 0; // Multiball (+3 balls)
        else if (r < 55) pwrs[i].type = 1; // Wide paddle
        else if (r < 75) pwrs[i].type = 2; // Lasers
        else if (r < 90) pwrs[i].type = 3; // Slow motion
        else             pwrs[i].type = 4; // Extra Life
        pwrs[i].active = true;
        break;
      }
    }
  };

  auto hitBrick = [&](float x, float y) -> bool {
    if (y < 11.0f || y > 11.0f + numRows * 5.5f) return false;
    int r = (int)((y - 11.0f) / 5.0f);
    if (r < 0 || r >= numRows) return false;
    int c = (int)((x - startX) / pitchX);
    if (c < 0 || c >= numCols) return false;
    if (!(rows[r] & (1 << c))) return false;

    rows[r] &= ~(1 << c);
    score += 10 * level;
    leftBricks--;
    sfx(700 + r * 90, 25);
    fxSpawn(startX + c * pitchX + brickW * 0.5f, 11 + r * 5.0f + brickH * 0.5f, random(4, 8), 16.0f, 45.0f, 4.0f, 1.2f);
    spawnPowerUp(startX + c * pitchX + brickW * 0.5f, 11 + r * 5.0f + brickH * 0.5f);
    return true;
  };

  auto resetGame = [&]() {
    level = 1; lives = 3; score = 0; st = PLAYING; baseSpeed = 72.0f;
    setupLevel(1);
  };
  resetGame();

  while (tick()) {
    float curPadW = (now < wideUntil) ? 28.0f : basePadW;
    float curSpeed = (now < slowUntil) ? baseSpeed * 0.75f : baseSpeed;

    if (st == PLAYING) {
      float mv = (held(K_RIGHT) ? 1.0f : 0.0f) - (held(K_LEFT) ? 1.0f : 0.0f);
      pad = clampf(pad + mv * 115.0f * dt, 1, SCR_W - 1 - curPadW);

      // Laser cannons
      if (now < laserUntil) {
        if (pressed(K_A) && (int32_t)(now - nextLaserShot) >= 0) {
          nextLaserShot = now + 240;
          for (int l = 0; l < 4; l += 2) {
            if (!lasers[l].active && !lasers[l + 1].active) {
              lasers[l].x = pad + 2; lasers[l].y = 57; lasers[l].active = true;
              lasers[l + 1].x = pad + curPadW - 3; lasers[l + 1].y = 57; lasers[l + 1].active = true;
              sfx(1600, 20);
              break;
            }
          }
        }
      }

      // Update lasers
      for (int l = 0; l < 4; l++) {
        if (!lasers[l].active) continue;
        lasers[l].y -= 150.0f * dt;
        if (lasers[l].y < 10.0f) lasers[l].active = false;
        else if (hitBrick(lasers[l].x, lasers[l].y)) lasers[l].active = false;
      }

      // Launch stuck ball
      if (stuck) {
        balls[0].x = pad + curPadW * 0.5f;
        balls[0].y = 57.0f;
        if (pressed(K_A)) {
          balls[0].vx = curSpeed * 0.5f * (random(2) ? 1 : -1);
          balls[0].vy = -curSpeed * 0.85f;
          stuck = false;
          sfx(800, 30);
        }
      }

      // Update active balls
      int activeBalls = 0;
      for (int b = 0; b < MAX_BALLS; b++) {
        if (!balls[b].active) continue;

        if (!stuck || b > 0) {
          float nx = balls[b].x + balls[b].vx * dt;
          if (nx < 2)   { nx = 2;   balls[b].vx = -balls[b].vx; sfx(400, 15); }
          if (nx > 125) { nx = 125; balls[b].vx = -balls[b].vx; sfx(400, 15); }
          if (hitBrick(nx, balls[b].y)) balls[b].vx = -balls[b].vx; else balls[b].x = nx;

          float ny = balls[b].y + balls[b].vy * dt;
          if (ny < 10)  { ny = 10;  balls[b].vy = -balls[b].vy; sfx(400, 15); }
          if (hitBrick(balls[b].x, ny)) balls[b].vy = -balls[b].vy; else balls[b].y = ny;

          // Paddle bounce
          if (balls[b].vy > 0 && balls[b].y >= 56 && balls[b].y <= 62 &&
              balls[b].x >= pad - 1 && balls[b].x <= pad + curPadW + 1) {
            float off = clampf((balls[b].x - (pad + curPadW * 0.5f)) / (curPadW * 0.5f + 1), -1.0f, 1.0f);
            balls[b].vx = off * curSpeed * 0.88f;
            balls[b].vy = -sqrtf(fmaxf(100.0f, curSpeed * curSpeed - balls[b].vx * balls[b].vx));
            balls[b].y = 55;
            sfx(600, 25);
          }

          if (balls[b].y > 66) {
            balls[b].active = false;
          } else {
            activeBalls++;
          }
        } else {
          activeBalls++;
        }
      }

      // All balls fell off screen -> lose life
      if (activeBalls == 0 && !stuck) {
        lives--;
        sfx(150, 200);
        fxSpawn(pad + curPadW * 0.5f, 60, random(8, 14), 22.0f, 55.0f);
        if (lives <= 0) {
          st = DEAD; since = now; hiScore("brk", score);
        } else {
          balls[0].active = true;
          balls[0].x = pad + curPadW * 0.5f;
          balls[0].y = 57.0f;
          balls[0].vx = 0; balls[0].vy = 0;
          stuck = true;
        }
      }

      // Update powerups
      for (int i = 0; i < MAX_PWRS; i++) {
        if (!pwrs[i].active) continue;
        pwrs[i].y += pwrs[i].vy * dt;
        if (pwrs[i].y > 66) { pwrs[i].active = false; continue; }

        // Catch powerup with paddle
        if (pwrs[i].y >= 57 && pwrs[i].y <= 63 &&
            pwrs[i].x >= pad - 3 && pwrs[i].x <= pad + curPadW + 3) {
          pwrs[i].active = false;
          sfx(1500, 60);

          if (pwrs[i].type == 0) {
            // Multiball (+3 balls)
            int spawned = 0;
            for (int b = 0; b < MAX_BALLS && spawned < 3; b++) {
              if (!balls[b].active) {
                balls[b].active = true;
                balls[b].x = pad + curPadW * 0.5f;
                balls[b].y = 56.0f;
                float ang = (spawned == 0) ? -0.45f : (spawned == 1 ? 0.45f : 0.0f);
                balls[b].vx = curSpeed * sinf(ang);
                balls[b].vy = -curSpeed * cosf(ang);
                spawned++;
              }
            }
          } else if (pwrs[i].type == 1) {
            wideUntil = now + 12000;
          } else if (pwrs[i].type == 2) {
            laserUntil = now + 10000;
          } else if (pwrs[i].type == 3) {
            slowUntil = now + 8000;
          } else if (pwrs[i].type == 4) {
            lives++; sfx(1800, 150);
          }
        }
      }

      // Level Cleared!
      if (leftBricks <= 0) {
        if (level >= 5) {
          st = WON; since = now; score += 1000; hiScore("brk", score); sfx(1800, 350);
        } else {
          level++;
          lives++; // +1 LIFE after each level!
          baseSpeed = fminf(baseSpeed + 10.0f, 125.0f);
          setupLevel(level);
          levelBannerUntil = now + 2000;
          sfx(1400, 250);
        }
      }
    } else if (pressed(K_A) && now - since > 700) {
      resetGame();
    }

    u8g2.clearBuffer();
    u8g2.drawHLine(0, 9, SCR_W);

    // Draw Bricks
    for (int r = 0; r < numRows; r++) {
      for (int c = 0; c < numCols; c++) {
        if (rows[r] & (1 << c)) {
          int bx0 = (int)(startX + c * pitchX), by0 = (int)(11 + r * 5.0f);
          int bw = (int)brickW, bh = (int)brickH;
          u8g2.drawFrame(bx0, by0, bw, bh);
          if (bw > 8 && bh > 2) {
            u8g2.drawBox(bx0 + 2, by0 + 1, bw - 4, bh - 2);
          } else if (bw > 5) {
            u8g2.drawHLine(bx0 + 1, by0 + 1, bw - 2);
          }
        }
      }
    }

    // Draw Powerups (floating capsules with letters [M], [W], [L], [S], [+])
    u8g2.setFont(u8g2_font_5x7_tr);
    for (int i = 0; i < MAX_PWRS; i++) {
      if (!pwrs[i].active) continue;
      int px0 = (int)pwrs[i].x - 4, py0 = (int)pwrs[i].y - 3;
      u8g2.drawFrame(px0, py0, 9, 7);
      char code = (pwrs[i].type == 0) ? 'M' : (pwrs[i].type == 1 ? 'W' : (pwrs[i].type == 2 ? 'L' : (pwrs[i].type == 3 ? 'S' : '+')));
      char s[2] = {code, 0};
      u8g2.drawStr(px0 + 2, py0 + 6, s);
    }

    // Draw Lasers
    for (int l = 0; l < 4; l++) {
      if (lasers[l].active) {
        int lx = (int)lasers[l].x, ly = (int)lasers[l].y;
        u8g2.drawVLine(lx, ly, 4);
      }
    }

    // Draw Paddle
    int px = (int)pad;
    int pw = (int)curPadW;
    u8g2.drawHLine(px + 1, 59, pw - 2);
    u8g2.drawBox(px, 60, pw, 3);
    u8g2.drawHLine(px + 1, 63, pw - 2);
    u8g2.setDrawColor(0);
    u8g2.drawPixel(px + 1, 61);
    u8g2.drawPixel(px + pw - 2, 61);
    u8g2.drawHLine(px + 4, 61, pw - 8);
    u8g2.setDrawColor(1);
    u8g2.drawPixel(px + pw / 2, 61);
    if (now < laserUntil) {
      // Laser turrets on paddle ends
      u8g2.drawVLine(px + 1, 57, 3);
      u8g2.drawVLine(px + pw - 2, 57, 3);
    }

    // Draw Active Balls
    for (int b = 0; b < MAX_BALLS; b++) {
      if (!balls[b].active) continue;
      int ibx = (int)balls[b].x, iby = (int)balls[b].y;
      u8g2.drawBox(ibx - 1, iby, 3, 1);
      u8g2.drawVLine(ibx, iby - 1, 3);
    }

    fxUpdateAndDraw(dt, 75.0f);

    // HUD Header
    char buf[28];
    u8g2.setFont(u8g2_font_5x7_tr);
    snprintf(buf, sizeof(buf), "LV:%d SC:%d", level, score); u8g2.drawStr(2, 7, buf);
    snprintf(buf, sizeof(buf), "LIVES:%d", lives); u8g2.drawStr(SCR_W - u8g2.getStrWidth(buf) - 2, 7, buf);

    if (stuck && st == PLAYING && now >= levelBannerUntil) {
      int w = u8g2.getStrWidth("A = LAUNCH"); drawLabel((SCR_W - w) / 2 - 1, 48, "A = LAUNCH", false);
    }

    // Level Clear & +1 Life Popup
    if (st == PLAYING && now < levelBannerUntil) {
      char lbuf[20];
      snprintf(lbuf, sizeof(lbuf), "LEVEL %d", level);
      drawMessage("+1 LIFE!", lbuf);
    }

    if (st == DEAD) {
      snprintf(buf, sizeof(buf), "Score %d (Lv %d)", score, level);
      drawMessage("GAME OVER", buf);
    } else if (st == WON) {
      snprintf(buf, sizeof(buf), "Master! Score %d", score);
      drawMessage("VICTORY!", buf);
    }
    present();
  }
}

// ================= GAME 7: SKY RAID (3D flight) =================
// Rear-view flight over a wireframe ground grid. Fly through rings, dodge pylons,
// shoot drones. Objects are real 3D points projected with x/z, y/z perspective.
#define FL_N    10
#define FL_F    60.0f          // focal length in pixels
#define FL_HOR  24             // horizon row
Obj fo[FL_N];

void flSpawn(Obj &o, float px, float z) {
  int r = random(100);
  o.z = z;
  o.x = px + random(-90, 91) * 0.1f;
  if (r < 40)      { o.type = 1; o.y = 1.5f + random(0, 60) * 0.1f; }
  else if (r < 75) { o.type = 0; o.y = 0.0f; }
  else             { o.type = 2; o.y = 1.5f + random(0, 60) * 0.1f; }
}

void gameFlight() {
  startGame();
  float px, py, vx, vy, speed, gz, dist, hp;
  int bonus, streak, score = 0, expX = 0, expY = 0;
  GState st;
  uint32_t since = 0, nextShot = 0, tracerUntil = 0, hurtUntil = 0, expUntil = 0;

  bool isInverted = false;
  bool wiping = false;
  uint32_t wipeStart = 0;
  const uint32_t WIPE_DUR = 650;
  int lastMilestone = 0;

  auto reset = [&]() {
    px = 0; py = 4; vx = vy = 0; speed = 9; gz = 0; dist = 0; hp = 100;
    bonus = 0; streak = 0; score = 0; st = PLAYING;
    tracerUntil = hurtUntil = expUntil = 0;
    isInverted = false; wiping = false; lastMilestone = 0;
    for (int i = 0; i < FL_N; i++) flSpawn(fo[i], 0, 20.0f + i * 6.0f);
  };
  auto hurt = [&](int d) { hp -= d; hurtUntil = now + 300; sfx(100, 200); streak = 0; };
  reset();

  while (tick()) {
    if (st == PLAYING) {
      float tx = ((held(K_RIGHT) ? 1.0f : 0.0f) - (held(K_LEFT) ? 1.0f : 0.0f)) * 7.0f;
      float ty = ((held(K_UP)    ? 1.0f : 0.0f) - (held(K_DOWN) ? 1.0f : 0.0f)) * 5.0f;
      float k = fminf(1.0f, 6.0f * dt);
      vx += (tx - vx) * k;
      vy += (ty - vy) * k;
      px += vx * dt;
      py += vy * dt;
      if (py > 11.0f) { py = 11.0f; vy = 0; }
      if (py < 0.5f) { py = 0.5f; vy = 0; if ((int32_t)(now - hurtUntil) >= 0) hurt(6); }

      speed = 9.0f + fminf(score * 0.01f, 7.0f) + (held(K_B) ? 6.0f : 0.0f);
      gz += speed * dt; if (gz >= 5.0f) gz -= 5.0f;
      dist += speed * dt;

      // Fire: hitscan straight ahead, nearest drone inside a cone
      if (held(K_A) && (int32_t)(now - nextShot) >= 0) {
        nextShot = now + 160; tracerUntil = now + 60; sfx(1100, 20);
        int best = -1; float bz = 45.0f;
        for (int i = 0; i < FL_N; i++) {
          Obj &o = fo[i];
          if (o.type != 2 || o.z < 1.0f || o.z > bz) continue;
          float tol = 1.3f + o.z * 0.05f;
          if (fabsf(o.x - px) < tol && fabsf(o.y - py) < tol) { best = i; bz = o.z; }
        }
        if (best >= 0) {
          Obj &o = fo[best];
          float inv = FL_F / o.z;
          expX = 64 + (int)((o.x - px) * inv);
          expY = FL_HOR + (int)((py - o.y) * inv);
          expUntil = now + 250; bonus += 100; sfx(250, 120);
          fxSpawn(expX, expY, random(8, 14), 22.0f, 65.0f);
          flSpawn(o, px, 50.0f + random(0, 200) * 0.1f);
        }
      }

      // Move world toward the camera and resolve passes
      for (int i = 0; i < FL_N; i++) {
        Obj &o = fo[i];
        o.z -= (speed + (o.type == 2 ? 3.5f : 0.0f)) * dt;
        if (o.type == 2) {                                   // drones home in
          o.x += clampf(px - o.x, -1.0f, 1.0f) * 1.2f * dt;
          o.y += clampf(py - o.y, -1.0f, 1.0f) * 0.8f * dt;
          if (o.y < 1.0f) o.y = 1.0f;
        }
        if (o.z < 0.8f) {
          float dx = o.x - px, dy = o.y - py;
          if (o.type == 0)      { if (fabsf(dx) < 1.2f && py < 5.4f) hurt(35); }
          else if (o.type == 1) {
            if (dx * dx + dy * dy < 4.84f) {
              streak++; bonus += 50 + 10 * imin(streak, 5);
              hp = fminf(100.0f, hp + 4.0f); sfx(1300, 60);
            } else streak = 0;
          } else { if (fabsf(dx) < 1.4f && fabsf(dy) < 1.2f) hurt(25); }
          flSpawn(o, px, 50.0f + random(0, 200) * 0.1f);
        }
      }

      score = (int)(dist * 0.5f) + bonus;

      // Vertical wipe transition milestone trigger every 250 points
      int curMilestone = score / 250;
      if (curMilestone > lastMilestone && !wiping && st == PLAYING) {
        lastMilestone = curMilestone;
        wiping = true;
        wipeStart = now;
        sfx(1750, 120);
      }

      if (hp <= 0) { hp = 0; st = DEAD; since = now; hiScore("flight", score); sfx(90, 400); }
    } else if (pressed(K_A) && now - since > 700) reset();

    // ---------- draw ----------
    u8g2.clearBuffer();
    u8g2.drawHLine(0, FL_HOR, SCR_W);

    for (int k = 1; k <= 12; k++) {                 // scrolling cross lines
      float z = k * 5.0f - gz;
      if (z < 0.5f) continue;
      int sy = FL_HOR + (int)(py * FL_F / z);
      if (sy <= 63) u8g2.drawHLine(0, sy, SCR_W);
    }
    float zbB = py * FL_F / (SCR_H - 1 - FL_HOR);   // depth where the ground hits the screen bottom
    int k0 = (int)floorf((px - 32.0f) * 0.125f);
    for (int k = k0; k < k0 + 9; k++) {             // perspective lines (cut early if far off-screen)
      float rel = k * 8.0f - px;
      float zb = fmaxf(fmaxf(zbB, fabsf(rel) * (FL_F / 180.0f)), 1.0f);
      float inv = FL_F / zb;
      drawLineSafe(64 + (int)rel, FL_HOR + (int)py, 64 + (int)(rel * inv), FL_HOR + (int)(py * inv));
    }

    uint8_t ord[FL_N]; int n = 0;                   // objects far to near
    for (int i = 0; i < FL_N; i++) {
      if (fo[i].z < 0.8f || fo[i].z > 58.0f) continue;
      int j = n++;
      while (j > 0 && fo[ord[j - 1]].z < fo[i].z) { ord[j] = ord[j - 1]; j--; }
      ord[j] = i;
    }
    for (int q = 0; q < n; q++) {
      Obj &o = fo[ord[q]];
      float inv = FL_F / o.z;
      int sx = 64 + (int)((o.x - px) * inv);
      if (sx < -40 || sx > SCR_W + 40) continue;
      if (o.type == 0) {                            // pylon: black-filled box hides the grid
        int hw = imax(1, (int)(0.8f * inv));
        int yb = FL_HOR + (int)(py * inv), yt = FL_HOR + (int)((py - 5.0f) * inv);
        u8g2.setDrawColor(0);
        drawBoxSafe(sx - hw, yt, 2 * hw + 1, yb - yt + 1);
        u8g2.setDrawColor(1);
        drawFrameSafe(sx - hw, yt, 2 * hw + 1, yb - yt + 1);
        drawBoxSafe(sx - hw, yt, 2 * hw + 1, imax(2, (yb - yt) / 8));
      } else {
        int cy = FL_HOR + (int)((py - o.y) * inv);
        if (o.type == 1) {                          // ring
          int r = imax(2, (int)(2.2f * inv));
          if (r < 120) {
            drawCircleSafe(sx, cy, r);
            if (r > 5) drawCircleSafe(sx, cy, r - 1);
          }
        } else {                                    // drone
          int s = imax(2, (int)(1.3f * inv));
          drawBoxSafe(sx - 1, cy - 1, 3, 3);
          drawHLineSafe(sx - s, cy, 2 * s + 1);
          drawVLineSafe(sx, cy - s / 2, s + 1);
          if (s >= 3) {
            drawVLineSafe(sx - s, cy - 1, 3);
            drawVLineSafe(sx + s, cy - 1, 3);
            u8g2.setDrawColor(0); drawPixelSafe(sx, cy); u8g2.setDrawColor(1);
          }
        }
      }
    }

    if ((int32_t)(now - tracerUntil) < 0) {         // gun tracers
      drawLineSafe(54, 52, 64, FL_HOR + 2);
      drawLineSafe(74, 52, 64, FL_HOR + 2);
    }
    if ((int32_t)(now - expUntil) < 0) {            // explosion rings
      int e = (int)((250 - (int32_t)(expUntil - now)) / 40);
      drawCircleSafe(expX, expY, 2 + e * 3);
      drawCircleSafe(expX, expY, 1 + e * 2);
    }

    if (st == PLAYING) {                            // crosshair
      drawPixelSafe(64, FL_HOR - 3); drawPixelSafe(64, FL_HOR + 3);
      drawPixelSafe(61, FL_HOR);     drawPixelSafe(67, FL_HOR);
    }

    if (!((int32_t)(now - hurtUntil) < 0 && ((now >> 6) & 1))) {   // player plane
      int x = 64 + (int)(vx * 0.6f), y = 51;
      int b = (int)(clampf(vx / 7.0f, -1.0f, 1.0f) * 4.0f);
      drawTriangleSafe(x - 13, y + 4 + b, x + 13, y + 4 - b, x, y - 4);
      drawVLineSafe(x - 13, y + 2 + b, 4);
      drawVLineSafe(x + 13, y + 2 - b, 4);
      drawBoxSafe(x - 1, y - 6, 3, 10);
      u8g2.setDrawColor(0);
      drawPixelSafe(x, y - 3);
      u8g2.setDrawColor(1);
      drawLineSafe(x - 4, y + 4 + b / 2, x - 5, y);
      drawLineSafe(x + 4, y + 4 - b / 2, x + 5, y);
      if ((now >> 4) & 1) {
        drawPixelSafe(x - 2, y + 6);
        drawPixelSafe(x + 2, y + 6);
        if ((now >> 5) & 1) {
          drawPixelSafe(x - 2, y + 7);
          drawPixelSafe(x + 2, y + 7);
        }
      }
    }
    fxUpdateAndDraw(dt);

    char buf[20];
    u8g2.setFont(u8g2_font_5x7_tr);
    u8g2.drawFrame(0, 0, 34, 6);
    u8g2.drawBox(1, 1, (int)(clampf(hp, 0.0f, 100.0f) * 0.32f), 4);
    snprintf(buf, sizeof(buf), "%d", score);  drawLabel(0, 7, buf, true);
    if (streak > 1) { snprintf(buf, sizeof(buf), "x%d", streak); drawLabel(0, 16, buf, true); }
    u8g2.drawVLine(1, 12, 38);                                      // altitude gauge
    u8g2.drawBox(0, 49 - (int)(py * 3.3f), 3, 2);
#if SHOW_FPS
    snprintf(buf, sizeof(buf), "%dfps", fps); drawLabel(0, 63, buf, true);
#endif
    if (st == DEAD) { snprintf(buf, sizeof(buf), "Score %d", score); drawMessage("CRASHED", buf); }

    // Vertical sweep inversion transition (milestones every 250 points)
    int splitY = -1;
    if (wiping) {
      uint32_t elapsed = now - wipeStart;
      if (elapsed >= WIPE_DUR) {
        wiping = false;
        isInverted = !isInverted;
      } else {
        splitY = (int)(((float)elapsed / (float)WIPE_DUR) * SCR_H);
      }
    }

    if (wiping || isInverted) {
      uint8_t *buffer = u8g2.getBufferPtr();
      for (int pg = 0; pg < 8; pg++) {
        int yTop = pg * 8;
        uint8_t mask = 0;
        for (int bit = 0; bit < 8; bit++) {
          int y = yTop + bit;
          bool invThis = wiping ? ((y < splitY) ? !isInverted : isInverted) : isInverted;
          if (invThis) mask |= (1 << bit);
        }
        if (mask != 0) {
          uint8_t *rowPtr = buffer + pg * SCR_W;
          for (int x = 0; x < SCR_W; x++) {
            rowPtr[x] ^= mask;
          }
        }
      }
      if (wiping && splitY >= 0 && splitY < SCR_H) {
        u8g2.setDrawColor(2);
        u8g2.drawHLine(0, splitY, SCR_W);
        if (splitY > 0) u8g2.drawHLine(0, splitY - 1, SCR_W);
        u8g2.setDrawColor(1);
      }
    }

    present();
  }
}

// ================= GAME 8: TETRA-DROP =================
void gameTetra() {
  startGame();
  uint16_t board[20] = {0};
  const int8_t SHAPES[7][4][2] = {
    {{0,-1}, {0,0}, {0,1}, {0,2}},  // I
    {{0,-1}, {0,0}, {0,1}, {-1,1}}, // J
    {{0,-1}, {0,0}, {0,1}, {1,1}},  // L
    {{0,0}, {1,0}, {0,1}, {1,1}},   // O
    {{0,0}, {-1,0}, {0,1}, {1,1}},  // S
    {{-1,0}, {0,0}, {1,0}, {0,1}},  // T
    {{0,0}, {1,0}, {0,1}, {-1,1}},  // Z
  };

  int curPiece = 0, curRot = 0, curX = 4, curY = 0;
  int nextPiece = random(7);
  int holdPiece = -1;
  bool canHold = true;
  int score = 0, linesTotal = 0;
  uint32_t fallTimer = 0, fallInterval = 550;
  int dasDir = 0;                 // left/right auto-repeat
  uint32_t dasNext = 0;
  GState st = PLAYING;
  uint32_t since = 0;

  auto getBlockPos = [&](int shape, int rot, int b, int &ox, int &oy) {
    int x = SHAPES[shape][b][0];
    int y = SHAPES[shape][b][1];
    for (int r = 0; r < (rot & 3); r++) { int t = x; x = -y; y = t; }
    ox = x; oy = y;
  };

  auto collides = [&](int shape, int rot, int px, int py) -> bool {
    for (int b = 0; b < 4; b++) {
      int ox, oy;
      getBlockPos(shape, rot, b, ox, oy);
      int bx = px + ox, by = py + oy;
      if (bx < 0 || bx >= 10 || by >= 20) return true;
      if (by >= 0 && (board[by] & (1 << bx))) return true;
    }
    return false;
  };

  auto gameOver = [&]() {
    st = DEAD; since = now; hiScore("tetra", score); sfx(100, 350);
  };

  auto spawn = [&]() {
    curPiece = nextPiece;
    nextPiece = random(7);
    curRot = 0;
    curX = 4; curY = 0;
    canHold = true;
    fallTimer = now;
    if (collides(curPiece, curRot, curX, curY)) gameOver();
  };

  auto shift = [&](int d) {
    if (!collides(curPiece, curRot, curX + d, curY)) { curX += d; sfx(1200, 10); }
  };

  // Rotate with small wall kicks. The O piece must not rotate at all:
  // rotating its offsets around (0,0) makes it creep one column sideways.
  auto tryRotate = [&]() {
    if (curPiece == 3) return;
    int nr = (curRot + 1) & 3;
    const int8_t KICK[5] = {0, -1, 1, -2, 2};
    for (int k = 0; k < 5; k++) {
      if (!collides(curPiece, nr, curX + KICK[k], curY)) {
        curX += KICK[k]; curRot = nr; sfx(1400, 15);
        return;
      }
    }
  };

  auto reset = [&]() {
    for (int i = 0; i < 20; i++) board[i] = 0;
    score = 0; linesTotal = 0; fallInterval = 550;
    holdPiece = -1; nextPiece = random(7);
    dasDir = 0;
    st = PLAYING;
    spawn();
  };
  reset();

  while (tick()) {
    if (st == PLAYING) {
      // Left/right: first press moves at once, holding repeats (DAS).
      int mv = (held(K_RIGHT) ? 1 : 0) - (held(K_LEFT) ? 1 : 0);
      if (mv == 0) dasDir = 0;
      else if (mv != dasDir) { dasDir = mv; shift(mv); dasNext = now + 170; }
      else if ((int32_t)(now - dasNext) >= 0) { shift(mv); dasNext = now + 55; }

      if (pressed(K_A)) tryRotate();

      if (pressed(K_B) && canHold) {
        int incoming = holdPiece;
        holdPiece = curPiece;
        if (incoming < 0) spawn();               // first hold takes the next piece
        else {
          curPiece = incoming; curRot = 0; curX = 4; curY = 0;
          fallTimer = now;
          if (collides(curPiece, curRot, curX, curY)) gameOver();
        }
        canHold = false;                         // spawn() re-arms it, so disarm after it
        sfx(900, 20);
      }

      if (st == PLAYING && pressed(K_UP)) {      // hard drop
        while (!collides(curPiece, curRot, curX, curY + 1)) curY++;
        fallTimer = 0;
        sfx(1600, 20);
      }

      bool dropTick = (st == PLAYING) && (now - fallTimer >= (held(K_DOWN) ? 45 : fallInterval));
      if (dropTick) {
        fallTimer = now;
        if (!collides(curPiece, curRot, curX, curY + 1)) {
          curY++;
        } else {
          for (int b = 0; b < 4; b++) {
            int ox, oy;
            getBlockPos(curPiece, curRot, b, ox, oy);
            int bx = curX + ox, by = curY + oy;
            if (by >= 0 && by < 20) board[by] |= (1 << bx);
          }
          sfx(800, 25);

          int linesCleared = 0;
          for (int y = 19; y >= 0; y--) {
            if (board[y] == 0x3FF) {
              linesCleared++;
              for (int ny = y; ny > 0; ny--) board[ny] = board[ny - 1];
              board[0] = 0;
              y++;
            }
          }
          if (linesCleared > 0) {
            linesTotal += linesCleared;
            score += (linesCleared == 1) ? 100 : (linesCleared == 2) ? 250 : (linesCleared == 3) ? 500 : 800;
            if (fallInterval > 120) fallInterval -= 15 * linesCleared;
            sfx(linesCleared == 4 ? 1800 : 1200 + linesCleared * 100, 50);
          }
          spawn();
        }
      }
    } else if (pressed(K_A) && (now - since) > 700) {
      reset();
    }

    u8g2.clearBuffer();
    const int ox = 49, oy = 2;
    u8g2.drawFrame(ox - 1, oy - 1, 32, 62);

    for (int y = 0; y < 20; y++) {
      for (int x = 0; x < 10; x++) {
        if (board[y] & (1 << x)) u8g2.drawBox(ox + x * 3, oy + y * 3, 2, 2);
      }
    }

    if (st == PLAYING) {
      int gy = curY;
      while (!collides(curPiece, curRot, curX, gy + 1)) gy++;
      for (int b = 0; b < 4; b++) {
        int bx, by; getBlockPos(curPiece, curRot, b, bx, by);
        if (curY + by >= 0) u8g2.drawPixel(ox + (curX + bx) * 3, oy + (gy + by) * 3);
      }
      for (int b = 0; b < 4; b++) {
        int bx, by; getBlockPos(curPiece, curRot, b, bx, by);
        if (curY + by >= 0) u8g2.drawBox(ox + (curX + bx) * 3, oy + (curY + by) * 3, 2, 2);
      }
    }

    u8g2.setFont(u8g2_font_5x7_tr);
    u8g2.drawStr(4, 10, "HOLD");
    u8g2.drawFrame(4, 13, 22, 18);
    if (holdPiece >= 0) {
      for (int b = 0; b < 4; b++) {
        int bx = SHAPES[holdPiece][b][0], by = SHAPES[holdPiece][b][1];
        u8g2.drawBox(14 + bx * 3, 21 + by * 3, 2, 2);
      }
    }

    u8g2.drawStr(88, 10, "NEXT");
    u8g2.drawFrame(88, 13, 22, 18);
    for (int b = 0; b < 4; b++) {
      int bx = SHAPES[nextPiece][b][0], by = SHAPES[nextPiece][b][1];
      u8g2.drawBox(98 + bx * 3, 21 + by * 3, 2, 2);
    }

    char buf[16];
    snprintf(buf, sizeof(buf), "%d", score);
    u8g2.drawStr(88, 42, "SCORE");
    u8g2.drawStr(88, 51, buf);
    snprintf(buf, sizeof(buf), "L:%d", linesTotal);
    u8g2.drawStr(88, 62, buf);

    if (st == DEAD) {
      snprintf(buf, sizeof(buf), "Score %d", score);
      drawMessage("GAME OVER", buf);
    }
    present();
  }
}

// ================= GAME 9: LUNAR ESCAPE =================
void gameLunar() {
  startGame();
  float lx, ly, lvx, lvy, lang, fuel;
  int lives = 3, score = 0, pad1X = 20, pad2X = 85;
  int targetPad = 1;
  GState st = PLAYING;
  uint32_t since = 0, winUntil = 0;

  int8_t terrain[SCR_W];
  auto makeTerrain = [&]() {
    pad1X = random(12, 35);
    pad2X = random(70, 95);
    for (int x = 0; x < SCR_W; x++) {
      if ((x >= pad1X && x <= pad1X + 18) || (x >= pad2X && x <= pad2X + 18)) {
        terrain[x] = 56;
      } else {
        float f = sinf(x * 0.08f) * 8.0f + cosf(x * 0.17f) * 6.0f;
        terrain[x] = (int8_t)(48 + f);
      }
    }
  };

  auto resetLander = [&]() {
    lx = 64; ly = 12; lvx = (float)random(-10, 11) * 0.04f; lvy = 0; lang = 0;
    targetPad = random(2) ? 1 : 2;
  };

  auto resetGame = [&]() {
    score = 0; lives = 3; fuel = 100.0f; st = PLAYING;
    makeTerrain();
    resetLander();
  };
  resetGame();

  while (tick()) {
    if (st == PLAYING) {
      if (now < winUntil) {
        // Paused celebrating touchdown
      } else {
        float steer = (held(K_RIGHT) ? 1.0f : 0.0f) - (held(K_LEFT) ? 1.0f : 0.0f);
        lang += steer * 2.4f * dt;
        lang = clampf(lang, -1.2f, 1.2f);

        bool thrusting = held(K_A) && fuel > 0;
        if (thrusting) {
          fuel -= 14.0f * dt;
          if (fuel < 0) fuel = 0;
          lvx += sinf(lang) * 3.5f * dt;
          lvy -= cosf(lang) * 3.5f * dt;
          sfx(120 + random(0, 40), 20);
          if (random(2) == 0) fxSpawn(lx - sinf(lang) * 4.0f, ly + 5.0f, 1, 8.0f, 18.0f);
        }

        lvy += 0.85f * dt;
        lx += lvx * dt * 20.0f;
        ly += lvy * dt * 20.0f;

        if (lx < 8) { lx = 8; lvx = -lvx * 0.5f; }
        if (lx > SCR_W - 9) { lx = SCR_W - 9; lvx = -lvx * 0.5f; }

        int ix = (int)lx;
        if (ly + 4 >= terrain[ix]) {
          bool onPad1 = (ix >= pad1X && ix <= pad1X + 18);
          bool onPad2 = (ix >= pad2X && ix <= pad2X + 18);
          bool correctPad = (targetPad == 1 && onPad1) || (targetPad == 2 && onPad2);
          bool safeSpeed = (lvy < 0.95f) && (fabsf(lvx) < 0.6f);
          bool safeAngle = (fabsf(lang) < 0.35f);

          if (correctPad && safeSpeed && safeAngle) {
            score += 150 + (int)fuel;
            fuel = fminf(100.0f, fuel + 30.0f);
            sfx(1400, 150);
            winUntil = now + 1200;
            makeTerrain();
            resetLander();
          } else {
            lives--;
            sfx(90, 350);
            // Lander shatters into 10-15 pieces on crash
            fxSpawn(lx, ly, random(10, 16), 18.0f, 55.0f);
            if (lives <= 0) {
              st = DEAD; since = now; hiScore("lunar", score);
            } else {
              resetLander();
            }
          }
        }
      }
    } else if (pressed(K_A) && (now - since) > 700) {
      resetGame();
    }

    u8g2.clearBuffer();
    for (int x = 0; x < SCR_W - 1; x++) {
      drawLineSafe(x, terrain[x], x + 1, terrain[x + 1]);
    }
    drawHLineSafe(pad1X, 57, 18);
    drawHLineSafe(pad2X, 57, 18);
    // Pad runway beacons
    drawVLineSafe(pad1X, 55, 3); drawVLineSafe(pad1X + 18, 55, 3);
    drawVLineSafe(pad2X, 55, 3); drawVLineSafe(pad2X + 18, 55, 3);

    int tpx = (targetPad == 1) ? pad1X + 9 : pad2X + 9;
    // Illuminated beacon flag on target pad
    drawVLineSafe(tpx, 51, 6);
    drawBoxSafe(tpx + 1, 51, 3, 2);
    if ((now >> 6) & 1) drawPixelSafe(tpx, 50); // Strobe light
    fxUpdateAndDraw(dt, 40.0f); // Render scattering lunar debris with gravity

    int clx = (int)lx, cly = (int)ly;
    float sa = sinf(lang), ca = cosf(lang);
    auto rotP = [&](float px, float py, int &rx, int &ry) {
      rx = clx + (int)(px * ca - py * sa);
      ry = cly + (int)(px * sa + py * ca);
    };

    // Apollo Lunar Excursion Module (LEM)
    int x1, y1, x2, y2;
    // Ascent stage crew cabin (faceted octagon)
    rotP(-2, -5, x1, y1); rotP(2, -5, x2, y2);  drawLineSafe(x1, y1, x2, y2);
    rotP(2, -5, x1, y1);  rotP(4, -2, x2, y2);  drawLineSafe(x1, y1, x2, y2);
    rotP(4, -2, x1, y1);  rotP(3, 1, x2, y2);   drawLineSafe(x1, y1, x2, y2);
    rotP(3, 1, x1, y1);   rotP(-3, 1, x2, y2);  drawLineSafe(x1, y1, x2, y2);
    rotP(-3, 1, x1, y1);  rotP(-4, -2, x2, y2); drawLineSafe(x1, y1, x2, y2);
    rotP(-4, -2, x1, y1); rotP(-2, -5, x2, y2); drawLineSafe(x1, y1, x2, y2);
    // Top docking antenna
    rotP(0, -5, x1, y1);  rotP(0, -7, x2, y2);  drawLineSafe(x1, y1, x2, y2);
    rotP(-1, -7, x1, y1); rotP(1, -7, x2, y2);  drawLineSafe(x1, y1, x2, y2);
    // Cockpit observation window
    rotP(0, -3, x1, y1);  rotP(1, -3, x2, y2);  drawLineSafe(x1, y1, x2, y2);
    // Descent stage engine nozzle
    rotP(-2, 1, x1, y1);  rotP(-1, 3, x2, y2);  drawLineSafe(x1, y1, x2, y2);
    rotP(2, 1, x1, y1);   rotP(1, 3, x2, y2);   drawLineSafe(x1, y1, x2, y2);
    rotP(-1, 3, x1, y1);  rotP(1, 3, x2, y2);   drawLineSafe(x1, y1, x2, y2);
    // Left landing gear strut & footpad plate
    rotP(-3, 1, x1, y1);  rotP(-6, 5, x2, y2);  drawLineSafe(x1, y1, x2, y2);
    rotP(-7, 5, x1, y1);  rotP(-5, 5, x2, y2);  drawLineSafe(x1, y1, x2, y2);
    // Right landing gear strut & footpad plate
    rotP(3, 1, x1, y1);   rotP(6, 5, x2, y2);   drawLineSafe(x1, y1, x2, y2);
    rotP(5, 5, x1, y1);   rotP(7, 5, x2, y2);   drawLineSafe(x1, y1, x2, y2);

    // Dynamic multi-stream rocket flame
    if (held(K_A) && fuel > 0 && st == PLAYING) {
      rotP(0, 3, x1, y1);
      rotP((float)random(-2, 3), (float)random(7, 12), x2, y2);
      drawLineSafe(x1, y1, x2, y2);
      rotP(-1, 3, x1, y1);
      rotP(0, (float)random(5, 9), x2, y2);
      drawLineSafe(x1, y1, x2, y2);
      rotP(1, 3, x1, y1);
      rotP(0, (float)random(5, 9), x2, y2);
      drawLineSafe(x1, y1, x2, y2);
    }

    char buf[20];
    u8g2.setFont(u8g2_font_5x7_tr);
    snprintf(buf, sizeof(buf), "SCORE %d", score); drawLabel(0, 7, buf, false);
    snprintf(buf, sizeof(buf), "LIFE %d", lives);  drawLabel(0, 7, buf, true);
    u8g2.drawFrame(2, 57, 34, 5);
    u8g2.drawBox(3, 58, (int)(clampf(fuel, 0.0f, 100.0f) * 0.32f), 3);
    u8g2.drawStr(38, 62, "F");

    if (st == DEAD) {
      snprintf(buf, sizeof(buf), "Score %d", score);
      drawMessage("CRASHED", buf);
    }
    present();
  }
}

// ================= GAME 10: VOID STRIKER =================
void gameVoid() {
  startGame();
  float px = 64, py = 52;
  int score = 0, bombs = 2, lives = 3;
  int lastSector = 0;
  GState st = PLAYING;
  uint32_t since = 0, bombUntil = 0, warpUntil = 0;

  struct Star { float x, y, spd; };
  Star stars[16];
  for (int i = 0; i < 16; i++) { stars[i].x = random(SCR_W); stars[i].y = random(SCR_H); stars[i].spd = 0.5f + random(0, 20) * 0.1f; }

  struct Bullet { float x, y, vx, vy; bool active; };
  Bullet bullets[8];
  for (int i = 0; i < 8; i++) bullets[i].active = false;
  uint32_t nextShot = 0;

  struct Foe { float x, y, vx, vy; bool active; uint8_t hp; };
  Foe foes[6];
  for (int i = 0; i < 6; i++) foes[i].active = false;
  uint32_t nextSpawn = 0;

  auto checkSector = [&]() {
    int curSec = score / 200;
    if (curSec != lastSector) {
      lastSector = curSec;
      warpUntil = now + 900;
      sfx(1600, 180);
    }
  };

  auto reset = [&]() {
    px = 64; py = 52; score = 0; bombs = 2; lives = 3; st = PLAYING;
    lastSector = 0; warpUntil = 0;
    for (int i = 0; i < 8; i++) bullets[i].active = false;
    for (int i = 0; i < 6; i++) foes[i].active = false;
  };

  while (tick()) {
    if (st == PLAYING) {
      float dx = (held(K_RIGHT) ? 1.0f : 0.0f) - (held(K_LEFT) ? 1.0f : 0.0f);
      float dy = (held(K_DOWN)  ? 1.0f : 0.0f) - (held(K_UP)   ? 1.0f : 0.0f);
      px = clampf(px + dx * 80.0f * dt, 7, SCR_W - 7);
      py = clampf(py + dy * 70.0f * dt, 10, SCR_H - 7);

      if (held(K_A) && (int32_t)(now - nextShot) >= 0) {
        nextShot = now + 160;
        for (int i = 0; i < 8; i++) {
          if (!bullets[i].active) {
            bullets[i].x = px; bullets[i].y = py - 4;
            bullets[i].vx = 0; bullets[i].vy = -150.0f;
            bullets[i].active = true;
            sfx(1400, 15);
            break;
          }
        }
      }

      if (pressed(K_B) && bombs > 0) {
        bombs--; bombUntil = now + 180;
        for (int i = 0; i < 6; i++) if (foes[i].active) {
          foes[i].active = false; score += 20;
          fxSpawn(foes[i].x, foes[i].y, random(5, 9), 25.0f, 75.0f);
        }
        checkSector();
        sfx(200, 200);
      }

      for (int i = 0; i < 8; i++) {
        if (!bullets[i].active) continue;
        bullets[i].y += bullets[i].vy * dt;
        if (bullets[i].y < -2) bullets[i].active = false;
      }

      if ((int32_t)(now - nextSpawn) >= 0) {
        nextSpawn = now + random(500, 1100);
        for (int i = 0; i < 6; i++) {
          if (!foes[i].active) {
            foes[i].x = random(10, SCR_W - 10); foes[i].y = -6;
            foes[i].vx = (float)random(-20, 21); foes[i].vy = random(25, 45);
            foes[i].hp = 1;
            foes[i].active = true;
            break;
          }
        }
      }

      for (int i = 0; i < 6; i++) {
        if (!foes[i].active) continue;
        foes[i].x += foes[i].vx * dt;
        foes[i].y += foes[i].vy * dt;
        if (foes[i].y > SCR_H + 8 || foes[i].x < -15 || foes[i].x > SCR_W + 15) foes[i].active = false;

        for (int b = 0; b < 8; b++) {
          if (!bullets[b].active) continue;
          if (fabsf(bullets[b].x - foes[i].x) < 6.0f && fabsf(bullets[b].y - foes[i].y) < 6.0f) {
            bullets[b].active = false;
            foes[i].active = false;
            score += 25;
            checkSector();
            sfx(800, 30);
            // Enemy ship shatters into 5-9 pieces dispersing in random directions
            fxSpawn(foes[i].x, foes[i].y, random(5, 10), 22.0f, 65.0f);
            break;
          }
        }

        if (foes[i].active && fabsf(px - foes[i].x) < 7.0f && fabsf(py - foes[i].y) < 6.0f) {
          foes[i].active = false;
          lives--;
          sfx(100, 300);
          // Player ship breaks into 12-18 pieces on impact
          fxSpawn(px, py, random(12, 19), 30.0f, 85.0f);
          if (lives <= 0) { st = DEAD; since = now; hiScore("void", score); }
        }
      }
    } else if (pressed(K_A) && (now - since) > 700) {
      reset();
    }

    u8g2.clearBuffer();
    for (int i = 0; i < 16; i++) {
      stars[i].y += stars[i].spd;
      if (stars[i].y >= SCR_H) stars[i].y = 0;
      drawPixelSafe((int)stars[i].x, (int)stars[i].y);
    }

    for (int i = 0; i < 8; i++) {
      if (bullets[i].active) {
        int bx = (int)bullets[i].x, by = (int)bullets[i].y;
        drawVLineSafe(bx, by - 1, 4);
        drawPixelSafe(bx - 1, by);
        drawPixelSafe(bx + 1, by);
      }
    }

    // Bio-Mechanical Alien Beetle Cruisers
    for (int i = 0; i < 6; i++) {
      if (!foes[i].active) continue;
      int fx = (int)foes[i].x, fy = (int)foes[i].y;
      drawBoxSafe(fx - 3, fy - 2, 7, 3);
      drawLineSafe(fx - 4, fy - 2, fx - 2, fy + 4);
      drawLineSafe(fx + 4, fy - 2, fx + 2, fy + 4);
      drawPixelSafe(fx - 5, fy - 1);
      drawPixelSafe(fx + 5, fy - 1);
      u8g2.setDrawColor(0);
      drawPixelSafe(fx, fy - 1);
      u8g2.setDrawColor(1);
    }

    // Render space debris
    fxUpdateAndDraw(dt);

    // Delta Interceptor Starfighter
    if (!((int32_t)(now - bombUntil) < 0 && ((now >> 5) & 1))) {
      int ipx = (int)px, ipy = (int)py;
      // Fuselage needle nose
      drawLineSafe(ipx, ipy - 7, ipx, ipy + 3);
      // Cockpit canopy highlight
      drawPixelSafe(ipx, ipy - 2);
      // Swept delta wings
      drawLineSafe(ipx, ipy - 3, ipx - 6, ipy + 3);
      drawLineSafe(ipx, ipy - 3, ipx + 6, ipy + 3);
      drawLineSafe(ipx - 6, ipy + 3, ipx - 1, ipy + 4);
      drawLineSafe(ipx + 6, ipy + 3, ipx + 1, ipy + 4);
      // Wingtip plasma cannon barrels
      drawVLineSafe(ipx - 6, ipy, 4);
      drawVLineSafe(ipx + 6, ipy, 4);
      // Twin ion engine flame trails
      int fl = ((now >> 4) & 1) ? 3 : 2;
      drawVLineSafe(ipx - 2, ipy + 5, fl);
      drawVLineSafe(ipx + 2, ipy + 5, fl);
    }

    if ((int32_t)(now - bombUntil) < 0) {
      u8g2.setDrawColor(2); drawBoxSafe(0, 0, SCR_W, SCR_H); u8g2.setDrawColor(1);
    }

    // Warp transition speed streaks and sector announcement
    if (st == PLAYING && now < warpUntil) {
      for (int k = 0; k < 10; k++) {
        int wy = (k * 13 + (now / 12)) % SCR_H;
        int wx = (k * 29 + (now / 4)) % SCR_W;
        drawHLineSafe(wx, wy, 20);
      }
      char wbuf[24];
      snprintf(wbuf, sizeof(wbuf), "WARP: SECTOR %d", (score / 200) + 1);
      drawLabel(24, 28, wbuf, true);
    }

    char buf[20];
    u8g2.setFont(u8g2_font_5x7_tr);
    snprintf(buf, sizeof(buf), "S%d %d", (score / 200) + 1, score); drawLabel(0, 7, buf, false);
    snprintf(buf, sizeof(buf), "B:%d L:%d", bombs, lives); drawLabel(0, 7, buf, true);

    if (st == DEAD) {
      snprintf(buf, sizeof(buf), "Score %d", score);
      drawMessage("SYSTEM DOWN", buf);
    }

    // Palette inversion on alternating sectors (like Chrome Dino night mode)
    if ((score / 200) & 1) {
      uint8_t *b = u8g2.getBufferPtr();
      for (int i = 0; i < SCR_W * SCR_H / 8; i++) b[i] ^= 0xFF;
    }
    present();
  }
}

// ================= GAME 11: TINY SOKOBAN =================
// Levels now carry an explicit keeper ('@'), are fully walled in, and every one
// is verified solvable. (The old ones auto-picked "the first empty cell" as the
// start, which for 2 of the 3 levels put the keeper in a region where nothing
// could ever be pushed.)
void gameSokoban() {
  startGame();
  const int SW = 8, SH = 7, NLV = 5;
  // '#' wall  '.' target  'o' box  '*' box on target  '@' keeper  '+' keeper on target
  static const char* const LEVELS[NLV][SH] = {
    { "######  ", "#    #  ", "# #@ #  ", "# o* #  ", "# .* #  ", "#    #  ", "######  " },
    { "####    ", "# .#    ", "#  ###  ", "#*@  #  ", "#  o #  ", "#  ###  ", "####    " },
    { "########", "#      #", "# .**o@#", "#      #", "#####  #", "    ####", "        " },
    { "########", "#  #   #", "#  oo .#", "# o#   #", "##@  ..#", "########", "        " },
    { "########", "# #   ##", "# # o ##", "# oo  ##", "#.   ###", "#.. @ ##", "########" }
  };

  uint8_t grid[SH][SW];
  int px = 1, py = 1, curLvl = 0, moves = 0, totalMoves = 0;
  GState st = PLAYING;
  uint32_t since = 0, resetHold = 0;

  struct UndoStep { uint8_t g[SH][SW]; int8_t px, py; };
  const int HIST = 24;
  UndoStep history[HIST];
  int histCount = 0;

  auto loadLevel = [&](int lvl) {
    histCount = 0; moves = 0;
    for (int y = 0; y < SH; y++) {
      for (int x = 0; x < SW; x++) {
        char c = LEVELS[lvl][y][x];
        uint8_t t = 0;
        if (c == '#') t = 1;
        else if (c == '.') t = 2;
        else if (c == 'o') t = 3;
        else if (c == '*') t = 4;
        else if (c == '@') { px = x; py = y; }
        else if (c == '+') { px = x; py = y; t = 2; }
        grid[y][x] = t;
      }
    }
  };
  loadLevel(0);

  auto saveUndo = [&]() {
    if (histCount == HIST) {                  // full: drop the oldest instead of silently not recording
      memmove(&history[0], &history[1], sizeof(UndoStep) * (HIST - 1));
      histCount = HIST - 1;
    }
    memcpy(history[histCount].g, grid, sizeof(grid));
    history[histCount].px = px;
    history[histCount].py = py;
    histCount++;
  };

  while (tick()) {
    if (st == PLAYING) {
      int dx = 0, dy = 0;
      if (pressed(K_UP))    dy = -1;
      if (pressed(K_DOWN))  dy =  1;
      if (pressed(K_LEFT))  dx = -1;
      if (pressed(K_RIGHT)) dx =  1;

      if (dx != 0 || dy != 0) {
        int nx = px + dx, ny = py + dy;
        if (nx >= 0 && nx < SW && ny >= 0 && ny < SH) {
          uint8_t dest = grid[ny][nx];
          if (dest == 1) {
            sfx(200, 10);
          } else if (dest == 3 || dest == 4) {
            int bx = nx + dx, by = ny + dy;
            if (bx >= 0 && bx < SW && by >= 0 && by < SH) {
              uint8_t boxDest = grid[by][bx];
              if (boxDest == 0 || boxDest == 2) {
                saveUndo();
                grid[ny][nx] = (dest == 4) ? 2 : 0;
                grid[by][bx] = (boxDest == 2) ? 4 : 3;
                px = nx; py = ny; moves++;
                sfx(600, 15);
              }
            }
          } else {
            saveUndo();
            px = nx; py = ny; moves++;
            sfx(1000, 8);
          }

          bool complete = true;
          for (int y = 0; y < SH; y++)
            for (int x = 0; x < SW; x++)
              if (grid[y][x] == 3) complete = false;
          if (complete) {
            sfx(1600, 100);
            totalMoves += moves;
            curLvl++;
            if (curLvl >= NLV) {
              int score = 1500 - totalMoves * 5; if (score < 100) score = 100;
              st = WON; since = now; hiScore("soko", score);
            } else {
              loadLevel(curLvl);
            }
          }
        }
      }

      if (pressed(K_B) && histCount > 0) {
        histCount--;
        memcpy(grid, history[histCount].g, sizeof(grid));
        px = history[histCount].px;
        py = history[histCount].py;
        if (moves > 0) moves--;
        sfx(400, 15);
      }

      // Hold A to restart the level (you can paint yourself into a corner).
      // Ignored while B is down so it can't fight the A+B "back to menu" combo.
      if (held(K_A) && !held(K_B)) {
        if (!resetHold) resetHold = now;
        else if (now - resetHold > 700) { loadLevel(curLvl); resetHold = 0; sfx(500, 40); }
      } else resetHold = 0;
    } else if (pressed(K_A) && (now - since) > 700) {
      curLvl = 0; totalMoves = 0; st = PLAYING; loadLevel(0);
    }

    u8g2.clearBuffer();
    const int ox = 36, oy = 8, ts = 7;
    for (int y = 0; y < SH; y++) {
      for (int x = 0; x < SW; x++) {
        int cx = ox + x * ts, cy = oy + y * ts;
        uint8_t t = grid[y][x];
        if (t == 1) {
          // Dungeon stone brick masonry
          u8g2.drawFrame(cx, cy, ts, ts);
          u8g2.drawHLine(cx + 1, cy + 3, ts - 2);
          if ((x + y) & 1) {
            u8g2.drawVLine(cx + 3, cy + 1, 2);
            u8g2.drawVLine(cx + 5, cy + 4, 2);
          } else {
            u8g2.drawVLine(cx + 5, cy + 1, 2);
            u8g2.drawVLine(cx + 2, cy + 4, 2);
          }
        } else if (t == 2) {
          // Recessed target pedestal with diamond crosshair
          u8g2.drawFrame(cx + 1, cy + 1, ts - 2, ts - 2);
          u8g2.drawPixel(cx + 3, cy + 1); u8g2.drawPixel(cx + 3, cy + 5);
          u8g2.drawPixel(cx + 1, cy + 3); u8g2.drawPixel(cx + 5, cy + 3);
          if ((now >> 7) & 1) u8g2.drawPixel(cx + 3, cy + 3);
        } else if (t == 3 || t == 4) {
          // Reinforced cargo crate with steel corner brackets
          u8g2.drawFrame(cx + 1, cy + 1, ts - 2, ts - 2);
          u8g2.drawPixel(cx + 2, cy + 2); u8g2.drawPixel(cx + 4, cy + 2);
          u8g2.drawPixel(cx + 2, cy + 4); u8g2.drawPixel(cx + 4, cy + 4);
          if (t == 3) {
            u8g2.drawLine(cx + 2, cy + 2, cx + 4, cy + 4);
          } else {
            u8g2.drawBox(cx + 2, cy + 2, 3, 3);
            u8g2.setDrawColor(0);
            u8g2.drawPixel(cx + 3, cy + 3);
            u8g2.setDrawColor(1);
          }
        }
      }
    }

    // Warehouse keeper character sprite (cap, face, overalls)
    int kx = ox + px * ts, ky = oy + py * ts;
    u8g2.setDrawColor(0);
    u8g2.drawBox(kx, ky, ts, ts);                // clear the tile so a target under the keeper doesn't clutter him
    u8g2.setDrawColor(1);
    u8g2.drawBox(kx + 2, ky, 3, 2);              // Cap crown
    u8g2.drawHLine(kx + 3, ky + 1, 3);           // Cap brim
    u8g2.drawBox(kx + 2, ky + 2, 3, 2);          // Face
    u8g2.setDrawColor(0);
    u8g2.drawPixel(kx + 4, ky + 2);              // Eye
    u8g2.setDrawColor(1);
    u8g2.drawBox(kx + 1, ky + 4, 5, 2);          // Overalls/torso
    u8g2.drawPixel(kx + 1, ky + 6);              // Left foot
    u8g2.drawPixel(kx + 5, ky + 6);              // Right foot

    char buf[24];
    u8g2.setFont(u8g2_font_5x7_tr);
    snprintf(buf, sizeof(buf), "LVL %d/%d", imin(curLvl + 1, NLV), NLV); drawLabel(0, 7, buf, false);
    snprintf(buf, sizeof(buf), "M:%d", moves);               drawLabel(0, 7, buf, true);
    drawLabel(0, 63, "B: UNDO", false);
    drawLabel(0, 63, "HOLD A: RESET", true);

    if (st == WON) {
      snprintf(buf, sizeof(buf), "Moves %d", totalMoves);
      drawMessage("ALL CLEARED!", buf);
    }
    present();
  }
}

// ================= GAME 12: MICRO BEAT (Rhythm & Timing) =================
void gameMicroBeat() {
  startGame();
  struct BeatNote {
    int8_t lane;   // 0: Left, 1: Up, 2: Down, 3: A/Right
    float y;
    uint16_t freq;
    bool active;
  };
  const int MAX_BEATS = 12;
  BeatNote notes[MAX_BEATS];
  for (int i = 0; i < MAX_BEATS; i++) notes[i].active = false;

  const uint16_t MELODY[16] = {
    440, 494, 523, 587, 659, 587, 523, 494,
    440, 523, 659, 784, 880, 784, 659, 523
  };
  const int8_t LANES[16] = {
    0, 1, 2, 3, 2, 1, 0, 1,
    2, 3, 1, 0, 3, 2, 1, 0
  };
  const uint8_t LANE_KEYS[4] = { K_LEFT, K_UP, K_DOWN, (uint8_t)(K_A | K_RIGHT) };

  uint32_t lastSpawn = millis();
  int beatIndex = 0;
  uint32_t score = 0, since = 0;
  int combo = 0;
  int health = 80;
  char feedback[12] = "";
  uint32_t feedbackUntil = 0;
  GState st = PLAYING;

  while (tick()) {
    if (st == PLAYING) {
      if (millis() - lastSpawn >= 340) {
        lastSpawn = millis();
        for (int i = 0; i < MAX_BEATS; i++) {
          if (!notes[i].active) {
            notes[i].lane = LANES[beatIndex % 16];
            notes[i].freq = MELODY[beatIndex % 16];
            notes[i].y = 12.0f;
            notes[i].active = true;
            beatIndex++;
            break;
          }
        }
      }

      // Every lane key is judged independently (the old else-if chain dropped
      // a second key pressed in the same frame).
      for (int l = 0; l < 4; l++) {
        if (!pressed(LANE_KEYS[l])) continue;
        int bestIdx = -1;
        float bestDist = 999.0f;
        for (int i = 0; i < MAX_BEATS; i++) {
          if (notes[i].active && notes[i].lane == l) {
            float dist = fabsf(notes[i].y - 52.0f);
            if (dist < bestDist) { bestDist = dist; bestIdx = i; }
          }
        }

        if (bestIdx >= 0 && bestDist <= 10.0f) {
          notes[bestIdx].active = false;
          combo++;
          if (bestDist <= 4.0f) {
            score += 100 * (1 + combo / 10);
            health = imin(100, health + 4);
            strcpy(feedback, "PERFECT!");
          } else {
            score += 50 * (1 + combo / 10);
            health = imin(100, health + 2);
            strcpy(feedback, "GOOD!");
          }
          sfx(notes[bestIdx].freq, 140);       // was buzz(): the tone never stopped
          feedbackUntil = millis() + 400;
        } else {
          combo = 0;
          health = imax(0, health - 3);
          sfx(300, 40);
        }
      }

      // Gentle ramp so the song doesn't stay at the same speed forever
      float fall = 38.0f + (float)imin(beatIndex / 6, 16);
      for (int i = 0; i < MAX_BEATS; i++) {
        if (notes[i].active) {
          notes[i].y += fall * dt;
          if (notes[i].y > 58.0f) {
            notes[i].active = false;
            combo = 0;
            health = imax(0, health - 10);
            strcpy(feedback, "MISS!");
            feedbackUntil = millis() + 350;
            sfx(200, 50);
          }
        }
      }

      if (health <= 0) {
        st = DEAD; since = now;
        hiScore("mbeat", score);
        sfx(250, 200);
      }
    } else if (pressed(K_A) && (now - since) > 700) {   // A is a lane key: give the result screen time to be seen
      st = PLAYING;
      score = 0; combo = 0; health = 80; beatIndex = 0;
      for (int i = 0; i < MAX_BEATS; i++) notes[i].active = false;
      startGame();
      lastSpawn = millis();
    }

    u8g2.clearBuffer();
    const int laneX[4] = {18, 44, 70, 96};
    for (int l = 0; l < 4; l++) {
      drawVLineSafe(laneX[l] - 10, 10, 46);
    }
    drawVLineSafe(laneX[3] + 16, 10, 46);

    drawHLineSafe(8, 52, 104);
    const char* lLabels[4] = {"<", "^", "v", "A"};
    for (int l = 0; l < 4; l++) {
      drawCircleSafe(laneX[l] + 3, 52, 5);
      u8g2.setFont(u8g2_font_5x7_tr);
      u8g2.drawStr(laneX[l] + 1, 55, lLabels[l]);
    }

    for (int i = 0; i < MAX_BEATS; i++) {
      if (notes[i].active) {
        int nx = laneX[notes[i].lane] + 3;
        int ny = (int)notes[i].y;
        drawDiscSafe(nx, ny, 4);
      }
    }

    drawFrameSafe(116, 12, 8, 44);
    int barH = (health * 40) / 100;
    if (barH > 0) drawBoxSafe(118, 54 - barH, 4, barH);

    char sBuf[20];
    snprintf(sBuf, sizeof(sBuf), "SCR %lu", (unsigned long)score);
    u8g2.setFont(u8g2_font_5x7_tr);
    u8g2.drawStr(2, 7, sBuf);
    if (combo > 1) {
      char cBuf[16];
      snprintf(cBuf, sizeof(cBuf), "%dx", combo);
      u8g2.drawStr(60, 7, cBuf);
    }

    if (millis() < feedbackUntil) {
      u8g2.setFont(u8g2_font_5x7_tr);
      u8g2.drawStr(76, 7, feedback);
    }

    if (st == DEAD) {
      snprintf(sBuf, sizeof(sBuf), "Final %lu", (unsigned long)score);
      drawMessage("SONG OVER", sBuf);
    }
    present();
  }
}

// ================= GAME 13: ARTILLERY CLASH (Ballistics Duel) =================
// Physics is now in real units (pixels, seconds). The old version moved shells
// ~570 px/s with 294 px/s^2 gravity, so almost any power overshot the 128px screen
// and the CPU (which always asked for power 95) could never hit anything.
void gameArtillery() {
  startGame();
  const float G = 85.0f;            // px/s^2
  const float VSC = 1.45f;          // power -> px/s  (power 60 @ 45deg lands ~90px away)
  const float WIND_K = 2.5f;        // wind units -> px/s^2
  const float STEP = 1.0f / 60.0f;  // fixed flight step (the CPU aiming simulates with the same step)
  const int px = 18, ex = 108;

  // ammo: 0 STD (unlimited)  1 HEAVY (hits hard)  2 WIDE (big crater, light damage)
  const uint8_t SHELL_R[3]   = {5, 6, 11};
  const uint8_t SHELL_DMG[3] = {30, 45, 20};
  const char* const SHELL_NAME[3] = {"STD", "HVY", "WIDE"};

  uint8_t terrain[SCR_W];
  int playerHp = 100, cpuHp = 100, wind = 0;
  float angle = 45.0f, power = 60.0f, cpuAngle = 45.0f;
  uint8_t ammoType = 0, ammoLeft[3] = {255, 3, 3}, shellType = 0;
  bool playerShell = true;

  enum TurnState { P_AIM, SHELL_FLY, CPU_AIM, GAME_END };
  TurnState tState = P_AIM;

  float sx = 0, sy = 0, svx = 0, svy = 0, acc = 0;
  GState st = PLAYING;
  uint32_t score = 0, since = 0, cpuAt = 0, flyStart = 0, explUntil = 0;
  int explX = 0, explY = 0, explR = 0;
  uint8_t rptKey = 0;
  uint32_t rptNext = 0;

  auto reset = [&]() {
    float ph = (float)random(0, 628) * 0.01f;
    for (int x = 0; x < SCR_W; x++) {
      float h = 42.0f + sinf(x * 0.05f + ph) * 10.0f + cosf(x * 0.11f + ph * 1.7f) * 5.0f;
      terrain[x] = (uint8_t)clampf(h, 28.0f, 56.0f);
    }
    playerHp = cpuHp = 100;
    angle = 45.0f; power = 60.0f; cpuAngle = 45.0f;
    ammoType = 0; ammoLeft[1] = 3; ammoLeft[2] = 3;
    wind = (int)random(-3, 4);
    score = 0; st = PLAYING; tState = P_AIM; explUntil = 0;
    rptKey = 0;
  };
  reset();

  auto crater = [&](int cx, int cy, int r) {
    for (int x = cx - r; x <= cx + r; x++) {
      if (x >= 0 && x < SCR_W) {
        int dxx = x - cx;
        int dyy = (int)sqrtf((float)(r * r - dxx * dxx));
        terrain[x] = (uint8_t)imin(SCR_H - 1, imax((int)terrain[x], cy + dyy));
      }
    }
  };

  // Where would a shell launched like this come down? (same integrator as the real flight)
  auto simLand = [&](float x0, float y0, float vx0, float vy0) -> float {
    float x = x0, y = y0, vx = vx0, vy = vy0;
    for (int i = 0; i < 900; i++) {
      x += vx * STEP; y += vy * STEP;
      vy += G * STEP; vx += wind * WIND_K * STEP;
      if (x < 0 || x >= SCR_W) return x;
      if (y >= terrain[(int)x]) return x;
    }
    return x;
  };

  auto launch = [&](bool byPlayer, float angDeg, float pwr, uint8_t type) {
    float rad = angDeg * 0.0174533f;
    float dirx = byPlayer ? 1.0f : -1.0f;
    int bx = byPlayer ? px : ex;
    float v = pwr * VSC;
    sx = bx + dirx * cosf(rad) * 7.0f;
    sy = terrain[bx] - 3 - sinf(rad) * 7.0f;
    svx = dirx * cosf(rad) * v;
    svy = -sinf(rad) * v;
    playerShell = byPlayer; shellType = type;
    tState = SHELL_FLY; acc = 0; flyStart = now;
    sfx(1800, 40);
  };

  auto afterShot = [&]() {
    buzz(0);
    if (cpuHp <= 0 && playerHp > 0) {
      st = WON; tState = GAME_END; since = now;
      score += 1000 + playerHp * 5;
      hiScore("arty", score); sfx(1500, 200);
    } else if (playerHp <= 0) {
      st = DEAD; tState = GAME_END; since = now;
      hiScore("arty", score); sfx(100, 300);
    } else if (playerShell) {
      tState = CPU_AIM; cpuAt = now + 800;
    } else {
      tState = P_AIM;
      wind = (int)random(-4, 5);
    }
  };

  auto explode = [&](int ix, int iy) {
    int r = SHELL_R[shellType];
    crater(ix, iy, r);
    explX = ix; explY = iy; explR = r; explUntil = now + 250;
    fxSpawn((float)ix, (float)iy, random(7, 12), 20.0f, 60.0f);
    sfx(160, 70);
    int reach = r + 4;
    int dE = abs(ix - ex), dP = abs(ix - px);
    if (dE <= reach) {
      int d = SHELL_DMG[shellType] * (100 - dE * 60 / reach) / 100;   // closer = more damage
      cpuHp = imax(0, cpuHp - d);
      if (playerShell) score += (uint32_t)d * 10;
    }
    if (dP <= reach) {
      int d = SHELL_DMG[shellType] * (100 - dP * 60 / reach) / 100;
      playerHp = imax(0, playerHp - d);
    }
    afterShot();
  };

  while (tick()) {
    if (st == PLAYING) {
      if (tState == P_AIM) {
        // Aim keys auto-repeat: tap = 1 step, hold = fast.
        uint8_t aim = btn & (K_LEFT | K_RIGHT | K_UP | K_DOWN);
        bool step = false;
        if (aim == 0) rptKey = 0;
        else if (aim != rptKey) { rptKey = aim; step = true; rptNext = now + 250; }
        else if ((int32_t)(now - rptNext) >= 0) { step = true; rptNext = now + 55; }
        if (step) {
          if (aim & K_LEFT)  angle = clampf(angle + 1.0f, 10.0f, 85.0f);
          if (aim & K_RIGHT) angle = clampf(angle - 1.0f, 10.0f, 85.0f);
          if (aim & K_UP)    power = clampf(power + 1.0f, 15.0f, 100.0f);
          if (aim & K_DOWN)  power = clampf(power - 1.0f, 15.0f, 100.0f);
          sfx(1300, 5);
        }

        if (pressed(K_B)) {                      // next ammo type that still has shells
          for (int i = 1; i <= 3; i++) {
            uint8_t n = (ammoType + i) % 3;
            if (ammoLeft[n] > 0) { ammoType = n; break; }
          }
          sfx(1600, 15);
        }

        if (pressed(K_A)) {
          uint8_t t = ammoType;
          if (ammoLeft[t] != 255 && ammoLeft[t] > 0) ammoLeft[t]--;
          launch(true, angle, power, t);
          if (ammoLeft[ammoType] == 0) ammoType = 0;
        }
      } else if (tState == SHELL_FLY) {
        acc += dt;
        while (acc >= STEP && tState == SHELL_FLY) {
          acc -= STEP;
          sx += svx * STEP; sy += svy * STEP;
          svy += G * STEP; svx += wind * WIND_K * STEP;

          if (sx < 0 || sx >= SCR_W || (int32_t)(now - flyStart) > 9000) {   // flew off the map: wasted shot
            afterShot();
            break;
          }
          int ix = (int)sx;
          bool hit = (sy >= terrain[ix]);
          if (!hit) {                                                       // direct hit on a tank body
            for (int k = 0; k < 2 && !hit; k++) {
              int tx = k ? ex : px;
              bool own = (k == 0) == playerShell;
              if (own && (int32_t)(now - flyStart) < 350) continue;
              int ty = terrain[tx];
              if (sx >= tx - 4 && sx <= tx + 4 && sy >= ty - 4 && sy <= ty + 1) hit = true;
            }
          }
          if (hit) { explode(ix, (int)sy); break; }
        }
        if (tState == SHELL_FLY)
          buzz((uint16_t)clampf(1400.0f - sy * 12.0f, 200.0f, 2400.0f));
      } else if (tState == CPU_AIM && (int32_t)(now - cpuAt) >= 0) {
        // Pick a lob angle, then binary-search the power that lands on the player
        // (terrain and wind included), and finally add a human-ish error.
        float th = 35.0f + (float)random(0, 26);
        float rad = th * 0.0174533f;
        float lo = 15.0f, hi = 100.0f;
        for (int i = 0; i < 14; i++) {
          float mid = (lo + hi) * 0.5f, v = mid * VSC;
          float lx = simLand(ex - cosf(rad) * 7.0f, terrain[ex] - 3 - sinf(rad) * 7.0f,
                             -cosf(rad) * v, -sinf(rad) * v);
          if (lx > px) lo = mid; else hi = mid;    // came down short -> more power
        }
        float pw = (lo + hi) * 0.5f * (1.0f + (float)random(-12, 13) * 0.01f);
        int r = (int)random(10);
        uint8_t t = (r < 6) ? 0 : (r < 8 ? 1 : 2);
        cpuAngle = th;
        launch(false, th, clampf(pw, 15.0f, 100.0f), t);
      }
    } else if (pressed(K_A) && (now - since) > 700) {
      reset();
      startGame();
    }

    u8g2.clearBuffer();
    for (int x = 0; x < SCR_W; x++) drawVLineSafe(x, terrain[x], SCR_H - terrain[x]);

    int py = terrain[px], ey = terrain[ex];
    drawBoxSafe(px - 4, py - 3, 8, 4);
    float pRad = angle * 0.0174533f;
    drawLineSafe(px, py - 3, px + (int)(cosf(pRad) * 6.0f), py - 3 - (int)(sinf(pRad) * 6.0f));

    drawBoxSafe(ex - 4, ey - 3, 8, 4);
    float cRad = cpuAngle * 0.0174533f;
    drawLineSafe(ex, ey - 3, ex - (int)(cosf(cRad) * 6.0f), ey - 3 - (int)(sinf(cRad) * 6.0f));

    if (tState == SHELL_FLY) drawDiscSafe((int)sx, (int)sy, 2);
    if ((int32_t)(explUntil - now) > 0) {
      int k = 250 - (int)(explUntil - now);
      u8g2.setDrawColor(0);
      drawDiscSafe(explX, explY, 1 + k * explR / 250);
      u8g2.setDrawColor(1);
      drawCircleSafe(explX, explY, 2 + k * (explR + 3) / 250);
    }
    fxUpdateAndDraw(dt, 60.0f);

    u8g2.setFont(u8g2_font_5x7_tr);
    char buf[28];
    snprintf(buf, sizeof(buf), "P%d C%d", playerHp, cpuHp);
    u8g2.drawStr(2, 7, buf);
    snprintf(buf, sizeof(buf), "A%d P%d W%+d", (int)angle, (int)power, wind);
    u8g2.drawStr(SCR_W - u8g2.getStrWidth(buf) - 2, 7, buf);

    if (st == PLAYING) {
      if (tState == CPU_AIM) drawLabel(0, 63, "CPU AIMING...", false);
      else {
        if (ammoLeft[ammoType] == 255) snprintf(buf, sizeof(buf), "B:%s", SHELL_NAME[ammoType]);
        else snprintf(buf, sizeof(buf), "B:%s x%d", SHELL_NAME[ammoType], ammoLeft[ammoType]);
        drawLabel(0, 63, buf, false);
      }
    }

    if (st == WON) drawMessage("VICTORY!", "CPU DESTROYED");
    else if (st == DEAD) drawMessage("DEFEATED", "TANK DESTROYED");

    present();
  }
  buzz(0);
}

// ================= GAME 14: SUB-HUNTER (Tactical Naval Sonar) =================
void gameSubHunter() {
  startGame();
  const float PI_F = 3.14159f;
  float subX = 60.0f, subY = 60.0f, subH = 0.0f;
  int depth = 1;                    // 0 surface, 1 periscope, 2 deep
  int torpedoes = 6;
  uint32_t score = 0, since = 0;
  float hull = 100.0f;
  GState st = PLAYING;
  bool outOfTorps = false, detected = false;

  struct Ship { float x, y, heading, speed; bool alive, isDestroyer; };
  Ship ships[3];
  struct Torp { float x, y, vx, vy, life; bool active; } torp;

  float radarAngle = 0.0f;
  uint32_t pingStart = 0, pingEchoUntil = 0, nextHurtSfx = 0;
  bool pingActive = false;

  auto reset = [&]() {
    subX = subY = 60.0f; subH = 0.0f; depth = 1;
    torpedoes = 6; hull = 100.0f; score = 0; st = PLAYING;
    outOfTorps = false; detected = false; pingActive = false;
    torp.active = false;
    Ship init[3] = {
      {30.0f, 30.0f, 0.8f, 6.0f, true, true},
      {90.0f, 40.0f, 2.2f, 5.0f, true, true},
      {70.0f, 90.0f, 3.4f, 4.0f, true, false}
    };
    for (int i = 0; i < 3; i++) ships[i] = init[i];
  };
  reset();

  while (tick()) {
    if (st == PLAYING) {
      // Turning is continuous now (the old 0.15 rad per press needed ~40 presses for a full circle)
      float turn = (held(K_RIGHT) ? 1.0f : 0.0f) - (held(K_LEFT) ? 1.0f : 0.0f);
      subH += turn * 1.9f * dt;
      if (subH > TWO_PI) subH -= TWO_PI; else if (subH < 0) subH += TWO_PI;
      if (pressed(K_UP))    { if (depth > 0) depth--; sfx(1400, 15); }
      if (pressed(K_DOWN))  { if (depth < 2) depth++; sfx(1400, 15); }

      float speed = (depth == 0) ? 9.0f : ((depth == 1) ? 6.0f : 3.5f);
      subX = clampf(subX + cosf(subH) * speed * dt, 10.0f, 110.0f);
      subY = clampf(subY + sinf(subH) * speed * dt, 10.0f, 110.0f);

      if (pressed(K_B)) {
        sfx(2200, 35);                           // no more blocking delay() in the frame loop
        pingActive = true; pingStart = now;
        pingEchoUntil = now + 1500;
      }
      if (pingActive && (int32_t)(now - pingEchoUntil) > 0) pingActive = false;

      if (pressed(K_A) && !torp.active && torpedoes > 0 && depth <= 1) {
        torpedoes--;
        torp.active = true; torp.life = 3.2f;
        torp.x = subX; torp.y = subY;
        torp.vx = cosf(subH) * 22.0f;
        torp.vy = sinf(subH) * 22.0f;
        sfx(800, 50);
      }

      if (torp.active) {
        torp.x += torp.vx * dt;
        torp.y += torp.vy * dt;
        torp.life -= dt;
        if (torp.life <= 0 || torp.x < 0 || torp.x > 120 || torp.y < 0 || torp.y > 120) torp.active = false;

        for (int i = 0; i < 3 && torp.active; i++) {
          if (!ships[i].alive) continue;
          float ddx = torp.x - ships[i].x;
          float ddy = torp.y - ships[i].y;
          if (ddx * ddx + ddy * ddy < 25.0f) {
            ships[i].alive = false;
            torp.active = false;
            score += ships[i].isDestroyer ? 300 : 500;
            sfx(140, 80);
          }
        }
      }

      int aliveCount = 0;
      detected = false;
      for (int i = 0; i < 3; i++) {
        Ship &s = ships[i];
        if (!s.alive) continue;
        aliveCount++;
        s.x = clampf(s.x + cosf(s.heading) * s.speed * dt, 10.0f, 110.0f);
        s.y = clampf(s.y + sinf(s.heading) * s.speed * dt, 10.0f, 110.0f);
        // Bounce off all four edges. The old code only bounced left/right, so ships that
        // reached the top/bottom edge slid along it forever.
        float c = cosf(s.heading), sn = sinf(s.heading);
        if ((s.x <= 11.0f && c < 0) || (s.x >= 109.0f && c > 0)) s.heading = PI_F - s.heading;
        if ((s.y <= 11.0f && sn < 0) || (s.y >= 109.0f && sn > 0)) s.heading = -s.heading;

        if (s.isDestroyer && (pingActive || depth == 0) && depth < 2) {
          float ddx = subX - s.x, ddy = subY - s.y;
          if (ddx * ddx + ddy * ddy < 160.0f) detected = true;
        }
      }

      if (detected) {                            // damage per second, not per frame
        hull -= 16.0f * dt;
        if ((int32_t)(now - nextHurtSfx) >= 0) { sfx(110, 60); nextHurtSfx = now + 300; }
        if (hull <= 0) { hull = 0; st = DEAD; since = now; hiScore("sub", score); sfx(100, 200); }
      }

      if (st == PLAYING) {
        if (aliveCount == 0) {
          st = WON; since = now;
          score += 1000 + (uint32_t)hull * 5;
          hiScore("sub", score);
        } else if (torpedoes == 0 && !torp.active) {   // nothing left to shoot with and targets remain
          st = DEAD; since = now; outOfTorps = true;
          hiScore("sub", score); sfx(100, 200);
        }
      }

      radarAngle += 2.5f * dt;
      if (radarAngle > 6.283f) radarAngle -= 6.283f;
    } else if (pressed(K_A) && (now - since) > 700) {
      reset();
      startGame();
    }

    u8g2.clearBuffer();
    const int cx = 32, cy = 32, r = 28;
    drawCircleSafe(cx, cy, r);
    drawCircleSafe(cx, cy, r / 2);
    drawPixelSafe(cx, cy);

    int rx = cx + (int)(cosf(radarAngle) * r);
    int ry = cy + (int)(sinf(radarAngle) * r);
    drawLineSafe(cx, cy, rx, ry);

    if (pingActive) {                            // expanding ping ring
      int pr = (int)((now - pingStart) * r / 1500);
      if (pr > 1 && pr < r) drawCircleSafe(cx, cy, pr);
    }

    drawDiscSafe(cx, cy, 2);
    drawLineSafe(cx, cy, cx + (int)(cosf(subH) * 5.0f), cy + (int)(sinf(subH) * 5.0f));

    for (int i = 0; i < 3; i++) {
      if (!ships[i].alive) continue;
      float relX = (ships[i].x - subX) * 0.45f;
      float relY = (ships[i].y - subY) * 0.45f;
      if (sqrtf(relX * relX + relY * relY) < r) {
        int bx = cx + (int)relX, by = cy + (int)relY;
        if (pingActive) drawDiscSafe(bx, by, ships[i].isDestroyer ? 2 : 3);
        else drawPixelSafe(bx, by);
      }
    }

    if (torp.active) {
      float relX = (torp.x - subX) * 0.45f, relY = (torp.y - subY) * 0.45f;
      if (sqrtf(relX * relX + relY * relY) < r - 2) drawCircleSafe(cx + (int)relX, cy + (int)relY, 2);  // stay inside the scope
    }

    u8g2.setFont(u8g2_font_5x7_tr);
    char buf[20];
    const char* dNames[3] = {"SURF", "PERI", "DEEP"};
    snprintf(buf, sizeof(buf), "D: %s", dNames[depth]);
    u8g2.drawStr(66, 12, buf);
    snprintf(buf, sizeof(buf), "HULL: %d%%", (int)hull);
    u8g2.drawStr(66, 24, buf);
    snprintf(buf, sizeof(buf), "TORP: %d", torpedoes);
    u8g2.drawStr(66, 36, buf);
    snprintf(buf, sizeof(buf), "SCR: %lu", (unsigned long)score);
    u8g2.drawStr(66, 48, buf);
    u8g2.drawStr(66, 60, pingActive ? "PING ACTIVE" : "[B: PING]");
    if (detected && ((now >> 7) & 1)) u8g2.drawStr(120, 12, "!");

    if (st == DEAD) {
      if (outOfTorps) drawMessage("OUT OF TORPS", "CONVOY ESCAPED");
      else drawMessage("HULL BREACHED", "SUB DESTROYED");
    } else if (st == WON) drawMessage("MISSION CLEAR", "CONVOY SUNK");

    present();
  }
}

// ================= GAME 15: CRYPT ROGUE (Turn-Based Roguelike) =================
// Fixes: the exit was walled off from the start room (unwinnable), monsters standing in
// your column never advanced, the ranged attack was a free action that worked through
// walls, floors/chests/monsters never reset, and nothing scaled between floors.
void gameCryptRogue() {
  startGame();
  const int RW = 12, RH = 8, LAST_FLOOR = 5;
  static const char* const TEMPLATE[RH] = {
    "############",
    "#....#....>#",
    "#..C.......#",     // gap in the middle wall (col 5) connects the two top rooms
    "##.####.####",
    "#....#.....#",
    "#.......C..#",     // second gap links the bottom rooms
    "#....#.....#",
    "############"
  };
  static const int8_t MSTART[3][2] = { {7, 2}, {2, 5}, {8, 6} };
  static const int8_t MHP[3] = {8, 6, 12};
  static const int8_t MATK[3] = {2, 2, 3};

  char dungeon[RH][RW + 1];
  struct Mon { int x, y, hp, atk; bool alive; };
  Mon monsters[3];

  int px = 1, py = 1, fdx = 1, fdy = 0;
  int hp = 24, maxHp = 24, atk = 5, potions = 2, bolts = 3, gold = 0, floorNo = 1;
  GState st = PLAYING;
  uint32_t since = 0, hurtUntil = 0, boltUntil = 0;
  int boltX = 0, boltY = 0;

  auto loadFloor = [&]() {
    for (int y = 0; y < RH; y++) memcpy(dungeon[y], TEMPLATE[y], RW + 1);
    px = 1; py = 1;
    for (int i = 0; i < 3; i++) {
      monsters[i].x = MSTART[i][0]; monsters[i].y = MSTART[i][1];
      monsters[i].hp  = MHP[i] + 3 * (floorNo - 1);
      monsters[i].atk = MATK[i] + (floorNo - 1) / 2;
      monsters[i].alive = true;
    }
  };
  auto newGame = [&]() {
    hp = maxHp = 24; atk = 5; potions = 2; bolts = 3; gold = 0; floorNo = 1;
    fdx = 1; fdy = 0; st = PLAYING; boltUntil = hurtUntil = 0;
    loadFloor();
  };
  newGame();

  auto monsterAt = [&](int x, int y, int ignore) -> int {
    for (int i = 0; i < 3; i++)
      if (i != ignore && monsters[i].alive && monsters[i].x == x && monsters[i].y == y) return i;
    return -1;
  };
  auto finish = [&](GState s) {
    st = s; since = now;
    hiScore("crogue", (uint32_t)(gold + (s == WON ? hp * 2 : 0)));
  };
  auto hurtMonster = [&](int i, int dmg) {
    monsters[i].hp -= dmg;
    if (monsters[i].hp <= 0) { monsters[i].alive = false; gold += 15; sfx(2400, 40); }
    else sfx(1800, 20);
  };

  // Everything the player does that takes a turn ends here: monsters get to act.
  auto monstersAct = [&]() {
    for (int i = 0; i < 3; i++) {
      Mon &m = monsters[i];
      if (!m.alive || st != PLAYING) continue;
      int ddx = px - m.x, ddy = py - m.y;
      int dist = abs(ddx) + abs(ddy);
      if (dist == 1) {
        hp = imax(0, hp - m.atk);
        hurtUntil = now + 150;
        sfx(400, 30);
        if (hp <= 0) finish(DEAD);
      } else if (dist < 6) {
        int sx = (ddx > 0) - (ddx < 0), sy = (ddy > 0) - (ddy < 0);
        // try the longer axis first, then the other one; never step into walls, you or a friend
        int ax[2], ay[2];
        if (abs(ddx) >= abs(ddy)) { ax[0] = sx; ay[0] = 0; ax[1] = 0; ay[1] = sy; }
        else                      { ax[0] = 0; ay[0] = sy; ax[1] = sx; ay[1] = 0; }
        for (int k = 0; k < 2; k++) {
          if (!ax[k] && !ay[k]) continue;
          int nx = m.x + ax[k], ny = m.y + ay[k];
          if (dungeon[ny][nx] == '#') continue;
          if ((nx == px && ny == py) || monsterAt(nx, ny, i) >= 0) continue;
          m.x = nx; m.y = ny;
          break;
        }
      }
    }
  };

  while (tick()) {
    if (st == PLAYING) {
      int dx = 0, dy = 0;
      if (pressed(K_LEFT))       dx = -1;
      else if (pressed(K_RIGHT)) dx = 1;
      else if (pressed(K_UP))    dy = -1;
      else if (pressed(K_DOWN))  dy = 1;

      bool acted = false;

      if (pressed(K_A) && potions > 0 && hp < maxHp) {      // quaff (costs a turn)
        potions--;
        hp = imin(maxHp, hp + 10);
        sfx(1600, 30);
        acted = true;
      }

      if (pressed(K_B) && bolts > 0) {                      // magic bolt: straight line in the facing direction
        int bx = px, by = py, hit = -1;
        for (int step = 0; step < 5; step++) {
          int nx = bx + fdx, ny = by + fdy;
          if (dungeon[ny][nx] == '#') break;
          bx = nx; by = ny;
          hit = monsterAt(bx, by, -1);
          if (hit >= 0) break;
        }
        if (hit >= 0) {
          bolts--;
          hurtMonster(hit, 8);
          boltX = bx; boltY = by; boltUntil = now + 150;
          acted = true;
        } else sfx(200, 30);                                // fizzled: no target, no charge spent
      }

      if (dx != 0 || dy != 0) {
        fdx = dx; fdy = dy;
        int nx = px + dx, ny = py + dy;
        int mi = monsterAt(nx, ny, -1);
        if (mi >= 0) {
          hurtMonster(mi, atk);                             // bump attack
          acted = true;
        } else {
          char cell = dungeon[ny][nx];
          if (cell != '#') {                                // bumping a wall costs nothing
            px = nx; py = ny;
            acted = true;
            if (cell == 'C') {
              gold += 25; potions++; bolts++;
              dungeon[ny][nx] = '.';
              sfx(2200, 50);
            } else if (cell == '>') {
              gold += 50;
              sfx(1500, 80);
              if (floorNo >= LAST_FLOOR) { finish(WON); acted = false; }
              else {
                floorNo++;
                maxHp += 4; hp = imin(maxHp, hp + 8); atk++; bolts++;
                loadFloor();
                acted = false;                              // fresh floor: monsters don't get a free hit
              }
            }
          }
        }
      }

      if (acted && st == PLAYING) monstersAct();
    } else if (pressed(K_A) && (now - since) > 700) {
      newGame();
    }

    u8g2.clearBuffer();
    const int tw = 8, th = 6, ox = 4, oy = 9;
    for (int y = 0; y < RH; y++) {
      for (int x = 0; x < RW; x++) {
        char c = dungeon[y][x];
        int sx = ox + x * tw, sy = oy + y * th;
        if (abs(x - px) + abs(y - py) <= 4) {
          if (c == '#') {
            drawBoxSafe(sx, sy, tw - 1, th - 1);
          } else if (c == 'C') {
            drawFrameSafe(sx + 1, sy + 1, tw - 3, th - 2);
          } else if (c == '>') {
            drawLineSafe(sx + 1, sy + 1, sx + 5, sy + 3);
            drawLineSafe(sx + 5, sy + 3, sx + 1, sy + 5);
          }
        }
      }
    }

    for (int i = 0; i < 3; i++) {
      if (monsters[i].alive && abs(monsters[i].x - px) + abs(monsters[i].y - py) <= 4)
        drawDiscSafe(ox + monsters[i].x * tw + 3, oy + monsters[i].y * th + 2, 2);
    }

    int pxs = ox + px * tw, pys = oy + py * th;
    drawFrameSafe(pxs, pys, tw - 1, th - 1);
    drawPixelSafe(pxs + 3, pys + 2);

    if ((int32_t)(boltUntil - now) > 0)
      drawLineSafe(pxs + 3, pys + 2, ox + boltX * tw + 3, oy + boltY * th + 2);

    u8g2.setFont(u8g2_font_5x7_tr);
    char buf[32];
    snprintf(buf, sizeof(buf), "HP%d/%d P%d B%d G%d F%d", hp, maxHp, potions, bolts, gold, floorNo);
    u8g2.drawStr(2, 7, buf);
    if (st == PLAYING) u8g2.drawStr(2, 63, "A:HEAL B:BOLT MOVE:HIT");

    if ((int32_t)(hurtUntil - now) > 0) {
      u8g2.setDrawColor(2);
      u8g2.drawFrame(0, 0, SCR_W, SCR_H);
      u8g2.setDrawColor(1);
    }

    if (st == DEAD) drawMessage("YOU DIED", "CRYPT OVER");
    else if (st == WON) drawMessage("DUNGEON CLEARED", "LEGENDARY HERO");

    present();
  }
}

// ================= GAME 16: PIXEL PLATFORMER (Precision Dash & Jump) =================
// Fixes: physics ran once per rendered frame (speed depended on FPS) -> fixed 60 Hz step;
// the dash only lasted a single frame; fast falls could tunnel through 4px platforms;
// you could leave a room anywhere on the right edge instead of reaching the door;
// rooms 3-6 were just rooms 0-2 again. All six rooms below were checked with a search
// over the exact physics in this function: every one is beatable, and rooms 4 and 5
// can only be beaten with the dash.
void gamePlatformer() {
  startGame();
  struct Plat { int16_t x, y, w, h; };
  struct Room { Plat p[5]; uint8_t n; int16_t sx, sy, doorTop; const char* hint; };
  static const Room ROOMS[6] = {
    { {{0,56,50,8}, {62,48,26,4}, {100,40,28,4}},                         3, 6, 48, 24, "A:JUMP  B:DASH" },
    { {{0,56,36,8}, {48,44,22,4}, {84,36,22,4}, {104,28,24,4}},           4, 6, 48, 12, "HOLD A = HIGHER" },
    { {{0,56,28,8}, {40,44,16,4}, {70,56,20,8}, {96,38,14,4}, {112,26,16,4}}, 5, 6, 48, 10, nullptr },
    { {{0,56,24,8}, {104,56,24,8}},                                       2, 6, 48, 40, "DASH IN MIDAIR" },
    { {{0,56,22,8}, {90,50,12,4}, {112,30,16,4}},                         3, 6, 48, 14, "B + DIRECTION" },
    { {{0,56,16,8}, {40,44,8,4}, {100,44,8,4}, {116,30,12,4}},            4, 6, 48, 14, nullptr }
  };
  const int NROOMS = 6;
  const float STEP = 1.0f / 60.0f;

  float px = 6, py = 48, vx = 0, vy = 0, acc = 0;
  bool onGround = true, canDash = true, jumpLatch = false, dashLatch = false;
  int dashF = 0, coyote = 0, jbuf = 0, room = 0, deaths = 0;
  uint32_t score = 0, since = 0;
  GState st = PLAYING;

  auto enterRoom = [&](int r) {
    const Room &R = ROOMS[r];
    px = R.sx; py = R.sy; vx = vy = 0;
    onGround = true; canDash = true; dashF = 0; coyote = 5; jbuf = 0;
  };
  enterRoom(0);

  while (tick()) {
    if (st == PLAYING) {
      const Room &R = ROOMS[room];
      // button edges are latched so they survive frames that run 0 physics steps
      if (pressed(K_A)) jumpLatch = true;
      if (pressed(K_B)) dashLatch = true;

      acc += dt;
      for (int n = 0; n < 3 && acc >= STEP && st == PLAYING; n++) {
        acc -= STEP;
        bool L = held(K_LEFT), Rt = held(K_RIGHT), U = held(K_UP), D = held(K_DOWN), jh = held(K_A);
        bool jp = jumpLatch, dp = dashLatch;
        jumpLatch = dashLatch = false;

        if (onGround) coyote = 5; else if (coyote > 0) coyote--;     // a few frames of grace after leaving a ledge
        if (jp) jbuf = 6; else if (jbuf > 0) jbuf--;                 // and a few before landing

        if (dp && canDash && dashF == 0) {
          float ddx = (Rt ? 1.0f : 0.0f) - (L ? 1.0f : 0.0f);
          float ddy = (D ? 1.0f : 0.0f) - (U ? 1.0f : 0.0f);
          if (ddx == 0.0f && ddy == 0.0f) ddx = (vx >= 0) ? 1.0f : -1.0f;
          float len = sqrtf(ddx * ddx + ddy * ddy);
          vx = ddx / len * 4.2f; vy = ddy / len * 4.2f;
          dashF = 7; canDash = false; jbuf = 0;                      // 7 frames, no gravity: ~29px
          sfx(2800, 35);
        }

        if (dashF > 0) {
          dashF--;
          if (dashF == 0) { vx = clampf(vx, -2.4f, 2.4f); vy = clampf(vy, -2.0f, 2.0f); }
        } else {
          float move = (Rt ? 1.0f : 0.0f) - (L ? 1.0f : 0.0f);
          vx = clampf(vx + move * 2.0f, -2.4f, 2.4f);
          if (move == 0.0f) vx *= 0.70f;
          if (jbuf > 0 && coyote > 0) {
            vy = -4.5f; jbuf = 0; coyote = 0; onGround = false;
            sfx(1600, 20);
          }
          if (!jh && vy < -1.5f) vy = -1.5f;                         // release A early = short hop
          vy = fminf(vy + 0.32f, 5.5f);                              // terminal velocity < platform thickness
        }

        float prevFeet = py + 8;
        px += vx; py += vy;
        if (px < 0)   { px = 0;   vx = 0; }
        if (px > 122) { px = 122; vx = 0; }

        onGround = false;
        for (int i = 0; i < R.n; i++) {                              // one-way platforms, swept so nothing is tunnelled
          const Plat &p = R.p[i];
          if (px + 5 > p.x && px + 1 < p.x + p.w && vy >= 0 && prevFeet <= p.y && py + 8 >= p.y) {
            py = p.y - 8; vy = 0; onGround = true;
            if (dashF == 0) canDash = true;
          }
        }

        if (py > 60.0f) {                                            // fell into the pit
          deaths++; sfx(200, 60);
          enterRoom(room);
          continue;
        }

        // reached the door (must actually touch it, not just the screen edge)
        if (px + 5 >= 122 && py + 8 >= R.doorTop && py + 2 <= R.doorTop + 16) {
          room++; score += 200; sfx(2200, 50);
          if (room >= NROOMS) {
            int bonus = 600 - deaths * 40; if (bonus < 0) bonus = 0;
            score += (uint32_t)bonus;
            st = WON; since = now; hiScore("plat", score);
          } else enterRoom(room);
          break;
        }
      }
    } else if (pressed(K_A) && (now - since) > 700) {
      room = 0; score = 0; deaths = 0; acc = 0; st = PLAYING;
      jumpLatch = dashLatch = false;
      enterRoom(0);
    }

    u8g2.clearBuffer();
    const Room &RD = ROOMS[room < NROOMS ? room : NROOMS - 1];
    for (int i = 0; i < RD.n; i++) drawBoxSafe(RD.p[i].x, RD.p[i].y, RD.p[i].w, RD.p[i].h);

    for (int s = 0; s < SCR_W; s += 8) {            // spikes only where there is actually a pit
      bool covered = false;
      for (int i = 0; i < RD.n; i++)
        if (RD.p[i].y >= 56 && RD.p[i].x < s + 8 && RD.p[i].x + RD.p[i].w > s) covered = true;
      if (!covered) drawTriangleSafe(s, 63, s + 4, 57, s + 8, 63);
    }

    drawFrameSafe(122, RD.doorTop, 6, 16);          // exit door
    drawDiscSafe(124, RD.doorTop + 9, 1);

    drawBoxSafe((int)px + 1, (int)py + 2, 4, 6);
    drawPixelSafe((int)px + 2, (int)py + 1);
    if (dashF > 0) drawLineSafe((int)px + 3, (int)py + 5, (int)(px + 3 - vx * 4), (int)(py + 5 - vy * 4));

    u8g2.setFont(u8g2_font_5x7_tr);
    if (RD.hint && st == PLAYING) u8g2.drawStr(2, 22, RD.hint);
    char buf[24];
    snprintf(buf, sizeof(buf), "RM:%d/%d D:%d", (room < NROOMS ? room : NROOMS - 1) + 1, NROOMS, deaths);
    u8g2.drawStr(2, 7, buf);
    if (canDash) drawBoxSafe(66, 1, 5, 5); else drawFrameSafe(66, 1, 5, 5);   // dash ready indicator
    snprintf(buf, sizeof(buf), "SCR:%lu", (unsigned long)score);
    u8g2.drawStr(SCR_W - u8g2.getStrWidth(buf) - 2, 7, buf);

    if (st == WON) drawMessage("SUMMIT REACHED", "CONGRATULATIONS!");

    present();
  }
}


// ================= GAME 17: SPACE INVADERS =================
void gameInvaders() {
  startGame();
  struct Alien { int8_t x, y; bool alive; };
  Alien aliens[18]; // 3 rows of 6
  int aliensAlive = 18;
  int gridX = 10, gridY = 8;
  int alienDir = 1;
  uint32_t lastStep = 0;
  uint8_t alienFrame = 0;
  uint8_t stepTone = 0;
  const uint16_t TONES[4] = {160, 140, 120, 100};

  float px = 64.0f;
  const int py = 56;
  struct Bullet { float x, y; bool active; };
  Bullet pBullets[2] = {{0,0,false}, {0,0,false}};
  Bullet aBullets[2] = {{0,0,false}, {0,0,false}};

  uint8_t bunkers[3] = {4, 4, 4};
  const int bnkX[3] = {22, 58, 94};

  int score = 0, lives = 3;
  GState st = PLAYING;
  uint32_t since = 0;

  auto initAliens = [&]() {
    aliensAlive = 18;
    gridX = 10; gridY = 8; alienDir = 1;
    for (int r = 0; r < 3; r++) {
      for (int c = 0; c < 6; c++) {
        aliens[r * 6 + c] = {(int8_t)(c * 16), (int8_t)(r * 10), true};
      }
    }
  };
  initAliens();

  auto resetGame = [&]() {
    score = 0; lives = 3; st = PLAYING;
    bunkers[0] = bunkers[1] = bunkers[2] = 4;
    pBullets[0].active = pBullets[1].active = false;
    aBullets[0].active = aBullets[1].active = false;
    px = 64.0f;
    initAliens();
  };

  while (tick()) {
    if (st == PLAYING) {
      float mv = ((held(K_RIGHT) ? 1.0f : 0.0f) - (held(K_LEFT) ? 1.0f : 0.0f));
      px = clampf(px + mv * 48.0f * dt, 8.0f, 120.0f);

      if (pressed(K_A)) {
        for (int i = 0; i < 2; i++) {
          if (!pBullets[i].active) {
            pBullets[i].x = px;
            pBullets[i].y = py - 3;
            pBullets[i].active = true;
            sfx(1600, 15);
            break;
          }
        }
      }

      for (int i = 0; i < 2; i++) {
        if (pBullets[i].active) {
          pBullets[i].y -= 95.0f * dt;
          if (pBullets[i].y < 2) pBullets[i].active = false;
          else {
            for (int b = 0; b < 3; b++) {
              if (bunkers[b] > 0 && pBullets[i].x >= bnkX[b] && pBullets[i].x <= bnkX[b] + 12 &&
                  pBullets[i].y >= 44 && pBullets[i].y <= 50) {
                bunkers[b]--;
                pBullets[i].active = false;
                sfx(400, 20);
                break;
              }
            }
            if (!pBullets[i].active) continue;

            for (int a = 0; a < 18; a++) {
              if (aliens[a].alive) {
                int ax = gridX + aliens[a].x;
                int ay = gridY + aliens[a].y;
                if (pBullets[i].x >= ax && pBullets[i].x <= ax + 12 &&
                    pBullets[i].y >= ay && pBullets[i].y <= ay + 8) {
                  aliens[a].alive = false;
                  pBullets[i].active = false;
                  aliensAlive--;
                  int row = a / 6;
                  score += (row == 0 ? 30 : (row == 1 ? 20 : 10));
                  sfx(280, 50);
                  fxSpawn(ax + 5, ay + 4, random(6, 11), 16.0f, 40.0f);
                  break;
                }
              }
            }
          }
        }
      }

      uint32_t stepInterval = 80 + (aliensAlive * 28);
      if (now - lastStep >= stepInterval) {
        lastStep = now;
        alienFrame ^= 1;
        stepTone = (stepTone + 1) & 3;
        sfx(TONES[stepTone], 12);

        bool hitWall = false;
        for (int a = 0; a < 18; a++) {
          if (aliens[a].alive) {
            int ax = gridX + aliens[a].x + alienDir * 4;
            if (ax < 2 || ax > 116) { hitWall = true; break; }
          }
        }

        if (hitWall) {
          alienDir = -alienDir;
          gridY += 4;
          for (int a = 0; a < 18; a++) {
            if (aliens[a].alive && (gridY + aliens[a].y + 8 >= py)) {
              st = DEAD; since = now; hiScore("invad", score); sfx(90, 450);
              break;
            }
          }
        } else {
          gridX += alienDir * 4;
        }

        if (aliensAlive > 0 && random(100) < 40) {
          for (int i = 0; i < 2; i++) {
            if (!aBullets[i].active) {
              int pick = random(18);
              for (int t = 0; t < 18; t++) {
                int idx = (pick + t) % 18;
                if (aliens[idx].alive) {
                  aBullets[i].x = gridX + aliens[idx].x + 5;
                  aBullets[i].y = gridY + aliens[idx].y + 8;
                  aBullets[i].active = true;
                  break;
                }
              }
              break;
            }
          }
        }
      }

      for (int i = 0; i < 2; i++) {
        if (aBullets[i].active) {
          aBullets[i].y += 55.0f * dt;
          if (aBullets[i].y > SCR_H) aBullets[i].active = false;
          else {
            for (int b = 0; b < 3; b++) {
              if (bunkers[b] > 0 && aBullets[i].x >= bnkX[b] && aBullets[i].x <= bnkX[b] + 12 &&
                  aBullets[i].y >= 44 && aBullets[i].y <= 50) {
                bunkers[b]--;
                aBullets[i].active = false;
                sfx(350, 30);
                break;
              }
            }
            if (!aBullets[i].active) continue;

            if (fabsf(aBullets[i].x - px) < 6.0f && fabsf(aBullets[i].y - py) < 5.0f) {
              aBullets[i].active = false;
              lives--;
              sfx(120, 250);
              fxSpawn((int)px, py, 12, 18.0f, 50.0f);
              if (lives <= 0) {
                st = DEAD; since = now; hiScore("invad", score); sfx(80, 500);
              }
            }
          }
        }
      }

      if (aliensAlive == 0) {
        score += 200;
        sfx(1800, 150);
        initAliens();
      }
    } else if (pressed(K_A) && (now - since) > 700) {
      resetGame();
    }

    u8g2.clearBuffer();
    u8g2.setFont(u8g2_font_5x7_tr);
    char sbuf[24];
    snprintf(sbuf, sizeof(sbuf), "SCR:%d", score);
    u8g2.drawStr(2, 7, sbuf);
    snprintf(sbuf, sizeof(sbuf), "LIVES:%d", lives);
    u8g2.drawStr(SCR_W - u8g2.getStrWidth(sbuf) - 2, 7, sbuf);

    for (int a = 0; a < 18; a++) {
      if (!aliens[a].alive) continue;
      int ax = gridX + aliens[a].x;
      int ay = gridY + aliens[a].y;
      int row = a / 6;
      if (row == 0) {
        u8g2.drawXBM(ax + 2, ay, 8, 8, SPR_SQUID);
      } else if (row == 1) {
        u8g2.drawXBM(ax, ay, 11, 8, alienFrame ? SPR_CRAB_B : SPR_CRAB_A);
      } else {
        u8g2.drawXBM(ax, ay, 12, 8, SPR_OCTOPUS);
      }
    }

    for (int b = 0; b < 3; b++) {
      if (bunkers[b] > 0) {
        int h = bunkers[b] * 2 - 1;
        drawBoxSafe(bnkX[b], 50 - h, 12, h);
        u8g2.setDrawColor(0);
        drawBoxSafe(bnkX[b] + 4, 48, 4, 3);
        u8g2.setDrawColor(1);
      }
    }

    int ipx = (int)px;
    drawBoxSafe(ipx - 5, py, 11, 5);
    drawBoxSafe(ipx - 1, py - 3, 3, 3);
    drawPixelSafe(ipx, py - 4);

    for (int i = 0; i < 2; i++) {
      if (pBullets[i].active) drawVLineSafe((int)pBullets[i].x, (int)pBullets[i].y, 3);
    }

    for (int i = 0; i < 2; i++) {
      if (aBullets[i].active) {
        int by = (int)aBullets[i].y;
        int bx = (int)aBullets[i].x;
        drawPixelSafe(bx, by);
        drawPixelSafe(bx + ((by & 2) ? 1 : -1), by + 1);
        drawPixelSafe(bx, by + 2);
      }
    }

    fxUpdateAndDraw(dt);

    if (st == DEAD) {
      snprintf(sbuf, sizeof(sbuf), "Final: %d", score);
      drawMessage("GAME OVER", sbuf);
    }
    present();
  }
}

// ================= GAME 18: PAC-CHASE =================
void gamePacChase() {
  startGame();
  uint8_t maze[8][16] = {
    {1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1},
    {1,2,0,0,0,1,0,0,0,0,1,0,0,0,2,1},
    {1,0,1,1,0,1,0,1,1,0,1,0,1,1,0,1},
    {1,0,0,0,0,0,0,3,3,0,0,0,0,0,0,1},
    {1,0,1,1,0,1,0,3,3,0,1,0,1,1,0,1},
    {1,0,0,1,0,0,0,0,0,0,0,0,1,0,0,1},
    {1,2,0,0,0,1,1,0,0,1,1,0,0,0,2,1},
    {1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1}
  };

  float px = 1.5f * 8.0f, py = 5.0f * 8.0f;
  int pDir = 0, nextDir = 0;
  const int8_t DIRS[4][2] = {{1,0}, {0,1}, {-1,0}, {0,-1}};

  struct Ghost { float x, y; int dir; bool alive; };
  Ghost ghosts[2] = {
    {7.0f * 8.0f, 3.5f * 8.0f, 0, true},
    {8.0f * 8.0f, 3.5f * 8.0f, 2, true}
  };

  int score = 0, lives = 3, dotsLeft = 0;
  uint32_t frightenedUntil = 0;
  int ghostCombo = 1;
  GState st = PLAYING;
  uint32_t since = 0;
  bool mouthOpen = true;

  auto resetBoard = [&]() {
    for (int y = 0; y < 8; y++) {
      for (int x = 0; x < 16; x++) {
        if ((y == 1 && (x == 1 || x == 14)) || (y == 6 && (x == 1 || x == 14))) maze[y][x] = 2;
        else if (maze[y][x] != 1 && !(y >= 3 && y <= 4 && x >= 7 && x <= 8)) maze[y][x] = 0;
      }
    }
    dotsLeft = 0;
    for (int y = 0; y < 8; y++)
      for (int x = 0; x < 16; x++)
        if (maze[y][x] == 0 || maze[y][x] == 2) dotsLeft++;
    px = 1.5f * 8.0f; py = 5.0f * 8.0f; pDir = 0; nextDir = 0;
    ghosts[0] = {7.0f * 8.0f, 3.5f * 8.0f, 0, true};
    ghosts[1] = {8.0f * 8.0f, 3.5f * 8.0f, 2, true};
    frightenedUntil = 0;
  };
  resetBoard();

  while (tick()) {
    if (st == PLAYING) {
      if (held(K_RIGHT)) nextDir = 0;
      if (held(K_DOWN))  nextDir = 1;
      if (held(K_LEFT))  nextDir = 2;
      if (held(K_UP))    nextDir = 3;

      int curTx = (int)(px / 8.0f), curTy = (int)(py / 8.0f);
      float cellOffX = px - (curTx * 8.0f + 4.0f);
      float cellOffY = py - (curTy * 8.0f + 4.0f);

      if (nextDir != pDir && fabsf(cellOffX) < 2.5f && fabsf(cellOffY) < 2.5f) {
        int ntx = curTx + DIRS[nextDir][0];
        int nty = curTy + DIRS[nextDir][1];
        if (ntx >= 0 && ntx < 16 && nty >= 0 && nty < 8 && maze[nty][ntx] != 1) {
          pDir = nextDir;
          px = curTx * 8.0f + 4.0f;
          py = curTy * 8.0f + 4.0f;
        }
      }

      int ftx = curTx + DIRS[pDir][0];
      int fty = curTy + DIRS[pDir][1];
      bool canMove = true;
      if (DIRS[pDir][0] > 0 && cellOffX >= 0 && (ftx >= 16 || maze[curTy][ftx] == 1)) canMove = false;
      if (DIRS[pDir][0] < 0 && cellOffX <= 0 && (ftx < 0  || maze[curTy][ftx] == 1)) canMove = false;
      if (DIRS[pDir][1] > 0 && cellOffY >= 0 && (fty >= 8  || maze[fty][curTx] == 1)) canMove = false;
      if (DIRS[pDir][1] < 0 && cellOffY <= 0 && (fty < 0  || maze[fty][curTx] == 1)) canMove = false;

      if (canMove) {
        px += DIRS[pDir][0] * 36.0f * dt;
        py += DIRS[pDir][1] * 36.0f * dt;
        mouthOpen = ((now / 120) & 1);
      }

      int etx = (int)(px / 8.0f), ety = (int)(py / 8.0f);
      if (etx >= 0 && etx < 16 && ety >= 0 && ety < 8) {
        if (maze[ety][etx] == 0) {
          maze[ety][etx] = 3;
          dotsLeft--;
          score += 10;
          sfx(440 + (score % 60), 12);
        } else if (maze[ety][etx] == 2) {
          maze[ety][etx] = 3;
          dotsLeft--;
          score += 50;
          frightenedUntil = now + 6500;
          ghostCombo = 1;
          sfx(880, 100);
        }
      }

      bool isFrightened = (now < frightenedUntil);
      for (int g = 0; g < 2; g++) {
        Ghost &gh = ghosts[g];
        float gspd = isFrightened ? 20.0f : 28.0f;
        int gtx = (int)(gh.x / 8.0f), gty = (int)(gh.y / 8.0f);
        float goffX = gh.x - (gtx * 8.0f + 4.0f);
        float goffY = gh.y - (gty * 8.0f + 4.0f);

        if (fabsf(goffX) < 1.5f && fabsf(goffY) < 1.5f) {
          gh.x = gtx * 8.0f + 4.0f;
          gh.y = gty * 8.0f + 4.0f;
          int possible[4], nPos = 0;
          for (int d = 0; d < 4; d++) {
            if (d == ((gh.dir + 2) % 4)) continue;
            int ntx = gtx + DIRS[d][0], nty = gty + DIRS[d][1];
            if (ntx >= 0 && ntx < 16 && nty >= 0 && nty < 8 && maze[nty][ntx] != 1) {
              possible[nPos++] = d;
            }
          }
          if (nPos > 0) {
            int bestD = possible[0];
            float bestDist = isFrightened ? -1e9f : 1e9f;
            for (int p = 0; p < nPos; p++) {
              int d = possible[p];
              float tx = gh.x + DIRS[d][0] * 8.0f - px;
              float ty = gh.y + DIRS[d][1] * 8.0f - py;
              float dist = tx * tx + ty * ty;
              if (isFrightened ? (dist > bestDist) : (dist < bestDist)) {
                bestDist = dist;
                bestD = d;
              }
            }
            gh.dir = bestD;
          } else {
            gh.dir = (gh.dir + 2) % 4;
          }
        }
        gh.x += DIRS[gh.dir][0] * gspd * dt;
        gh.y += DIRS[gh.dir][1] * gspd * dt;

        float cdx = gh.x - px, cdy = gh.y - py;
        if (cdx * cdx + cdy * cdy < 36.0f) {
          if (isFrightened) {
            score += 200 * ghostCombo;
            ghostCombo *= 2;
            sfx(1400, 120);
            gh.x = 7.5f * 8.0f; gh.y = 3.5f * 8.0f;
            fxSpawn((int)px, (int)py, 8, 16.0f, 35.0f);
          } else {
            lives--;
            sfx(150, 350);
            fxSpawn((int)px, (int)py, 14, 20.0f, 60.0f);
            if (lives <= 0) {
              st = DEAD; since = now; hiScore("pac", score); sfx(80, 500);
            } else {
              px = 1.5f * 8.0f; py = 5.0f * 8.0f; pDir = 0;
              ghosts[0].x = 7.0f * 8.0f; ghosts[0].y = 3.5f * 8.0f;
              ghosts[1].x = 8.0f * 8.0f; ghosts[1].y = 3.5f * 8.0f;
            }
          }
        }
      }

      if (dotsLeft == 0) {
        score += 500;
        sfx(1600, 200);
        resetBoard();
      }
    } else if (pressed(K_A) && (now - since) > 700) {
      score = 0; lives = 3; st = PLAYING; resetBoard();
    }

    u8g2.clearBuffer();
    for (int y = 0; y < 8; y++) {
      for (int x = 0; x < 16; x++) {
        uint8_t t = maze[y][x];
        int wx = x * 8, wy = y * 8;
        if (t == 1) {
          drawBoxSafe(wx, wy, 8, 8);
          u8g2.setDrawColor(0);
          drawBoxSafe(wx + 1, wy + 1, 6, 6);
          u8g2.setDrawColor(1);
        } else if (t == 0) {
          drawPixelSafe(wx + 4, wy + 4);
        } else if (t == 2) {
          if ((now >> 7) & 1) drawDiscSafe(wx + 4, wy + 4, 2);
        }
      }
    }

    int ipx = (int)px - 6;
    int ipy = (int)py - 6;
    if (mouthOpen) {
      u8g2.drawXBM(ipx, ipy, 13, 13, SPR_PACMAN);
    } else {
      drawDiscSafe((int)px, (int)py, 5);
    }

    bool isFrightened = (now < frightenedUntil);
    for (int g = 0; g < 2; g++) {
      int igx = (int)ghosts[g].x - 7;
      int igy = (int)ghosts[g].y - 7;
      if (isFrightened) {
        bool flashScared = ((frightenedUntil - now) < 1800) && ((now >> 7) & 1);
        if (!flashScared) {
          u8g2.drawXBM(igx, igy, 14, 14, SPR_GHOST);
          u8g2.setDrawColor(0);
          drawPixelSafe(igx + 4, igy + 6); drawPixelSafe(igx + 9, igy + 6);
          drawHLineSafe(igx + 4, igy + 10, 6);
          u8g2.setDrawColor(1);
        }
      } else {
        u8g2.drawXBM(igx, igy, 14, 14, SPR_GHOST);
      }
    }

    u8g2.setFont(u8g2_font_5x7_tr);
    char buf[20];
    snprintf(buf, sizeof(buf), "%d", score);
    u8g2.drawStr(2, 7, buf);
    snprintf(buf, sizeof(buf), "L:%d", lives);
    u8g2.drawStr(SCR_W - u8g2.getStrWidth(buf) - 2, 7, buf);

    fxUpdateAndDraw(dt);

    if (st == DEAD) {
      snprintf(buf, sizeof(buf), "Final: %d", score);
      drawMessage("PAC DIED", buf);
    }
    present();
  }
}

// ================= GAME 19: BARREL KONG =================
void gameKong() {
  startGame();
  float px = 16.0f, py = 56.0f;
  float vy = 0.0f;
  bool jumping = false;
  bool climbing = false;
  int score = 0, lives = 3, level = 1;
  GState st = PLAYING;
  uint32_t since = 0;
  uint32_t nextBarrel = 0;

  struct Barrel { float x, y, vx; int tier; bool active; };
  Barrel barrels[4];
  for (int i = 0; i < 4; i++) barrels[i].active = false;

  auto resetPlayer = [&]() {
    px = 16.0f; py = 56.0f; vy = 0.0f;
    jumping = false; climbing = false;
  };

  while (tick()) {
    if (st == PLAYING) {
      bool onLadder1 = fabsf(px - 32.0f) < 4.0f && py <= 56.0f && py >= 38.0f;
      bool onLadder2 = fabsf(px - 96.0f) < 4.0f && py <= 40.0f && py >= 22.0f;
      bool nearLadder = onLadder1 || onLadder2;

      if (nearLadder && (held(K_UP) || held(K_DOWN))) {
        climbing = true;
        jumping = false;
        vy = 0;
        if (held(K_UP)) py -= 32.0f * dt;
        if (held(K_DOWN)) py += 32.0f * dt;
      } else {
        climbing = false;
        float mv = (held(K_RIGHT) ? 1.0f : 0.0f) - (held(K_LEFT) ? 1.0f : 0.0f);
        px = clampf(px + mv * 42.0f * dt, 4.0f, 122.0f);

        float floorY = 56.0f;
        if (py < 44.0f && py >= 26.0f) floorY = 38.0f;
        else if (py < 26.0f) floorY = 20.0f;

        if (pressed(K_A) && !jumping) {
          jumping = true;
          vy = -3.8f;
          sfx(950, 30);
        }

        if (jumping) {
          vy += 12.0f * dt;
          py += vy * 20.0f * dt;
          if (py >= floorY) {
            py = floorY;
            jumping = false;
            vy = 0;
          }
        } else {
          py = floorY;
        }
      }

      if (now >= nextBarrel) {
        nextBarrel = now + random(2200, 3400) / level;
        for (int i = 0; i < 4; i++) {
          if (!barrels[i].active) {
            barrels[i] = {46.0f, 20.0f, 32.0f + level * 5.0f, 2, true};
            sfx(200, 40);
            break;
          }
        }
      }

      for (int i = 0; i < 4; i++) {
        if (!barrels[i].active) continue;
        Barrel &b = barrels[i];
        b.x += b.vx * dt;

        if (b.tier == 2 && b.x > 116.0f) {
          b.tier = 1; b.y = 38.0f; b.vx = -fabsf(b.vx);
          sfx(160, 30);
        } else if (b.tier == 1 && b.x < 12.0f) {
          b.tier = 0; b.y = 56.0f; b.vx = fabsf(b.vx);
          sfx(160, 30);
        } else if (b.tier == 0 && b.x > 124.0f) {
          b.active = false;
        }

        float dx = b.x - px, dy = b.y - py;
        if (fabsf(dx) < 6.0f && fabsf(dy) < 5.0f) {
          lives--;
          sfx(110, 250);
          fxSpawn((int)px, (int)py, 12, 16.0f, 45.0f);
          if (lives <= 0) {
            st = DEAD; since = now; hiScore("kong", score); sfx(80, 500);
          } else {
            resetPlayer();
          }
        } else if (jumping && fabsf(dx) < 6.0f && dy > 4.0f && dy < 16.0f) {
          score += 100;
          sfx(1500, 40);
        }
      }

      if (py <= 22.0f && px >= 85.0f) {
        score += 500;
        level++;
        sfx(1200, 80); sfx(1500, 100); sfx(1800, 200);
        resetPlayer();
        for (int i = 0; i < 4; i++) barrels[i].active = false;
      }
    } else if (pressed(K_A) && (now - since) > 700) {
      score = 0; lives = 3; level = 1; st = PLAYING; resetPlayer();
    }

    u8g2.clearBuffer();
    u8g2.drawXBM(4, 0, 40, 31, SPR_KONG);

    drawDiscSafe(100, 12, 3);
    drawDiscSafe(104, 12, 3);
    drawTriangleSafe(97, 13, 107, 13, 102, 18);

    drawHLineSafe(44, 22, 80);
    for (int x = 44; x < 124; x += 6) drawLineSafe(x, 22, x + 4, 25);
    drawHLineSafe(44, 25, 80);

    drawHLineSafe(8, 40, 102);
    for (int x = 8; x < 110; x += 6) drawLineSafe(x, 40, x + 4, 43);
    drawHLineSafe(8, 43, 102);

    drawHLineSafe(0, 58, 128);
    for (int x = 0; x < 128; x += 6) drawLineSafe(x, 58, x + 4, 61);
    drawHLineSafe(0, 61, 128);

    auto drawLadder = [](int lx, int yTop, int yBot) {
      drawVLineSafe(lx - 3, yTop, yBot - yTop);
      drawVLineSafe(lx + 3, yTop, yBot - yTop);
      for (int y = yTop + 3; y < yBot; y += 4) drawHLineSafe(lx - 3, y, 7);
    };
    drawLadder(96, 22, 40);
    drawLadder(32, 40, 58);

    for (int i = 0; i < 4; i++) {
      if (!barrels[i].active) continue;
      int bx = (int)barrels[i].x, by = (int)barrels[i].y;
      drawDiscSafe(bx, by, 3);
      u8g2.setDrawColor(0);
      drawPixelSafe(bx, by);
      u8g2.setDrawColor(1);
    }

    int ipx = (int)px, ipy = (int)py;
    drawBoxSafe(ipx - 2, ipy - 8, 5, 8);
    drawDiscSafe(ipx, ipy - 9, 2);

    u8g2.setFont(u8g2_font_5x7_tr);
    char buf[20];
    snprintf(buf, sizeof(buf), "%d", score);
    drawLabel(0, 7, buf, false);
    snprintf(buf, sizeof(buf), "LIV:%d LV:%d", lives, level);
    drawLabel(0, 7, buf, true);

    fxUpdateAndDraw(dt);

    if (st == DEAD) {
      snprintf(buf, sizeof(buf), "Score: %d", score);
      drawMessage("CRUSHED", buf);
    }
    present();
  }
}

// ================= GAME 20: FLAPPY C3 =================
void gameFlapper() {
  startGame();
  float px = 28.0f, py = 28.0f, vy = 0.0f;
  int score = 0;
  GState st = PLAYING;
  uint32_t since = 0;

  struct Pipe { float x; int gapY; bool scored; };
  Pipe pipes[2];
  pipes[0] = {128.0f, 26, false};
  pipes[1] = {128.0f + 75.0f, 34, false};
  const int GAP_H = 22;

  auto reset = [&]() {
    px = 28.0f; py = 28.0f; vy = 0.0f;
    score = 0; st = PLAYING;
    pipes[0] = {128.0f, 26, false};
    pipes[1] = {128.0f + 75.0f, 34, false};
  };

  while (tick()) {
    if (st == PLAYING) {
      if (pressed(K_A) || pressed(K_UP)) {
        vy = -3.8f;
        sfx(1150, 18);
      }
      vy += 11.5f * dt;
      py += vy * 18.0f * dt;

      if (py >= 54.0f) {
        py = 54.0f; st = DEAD; since = now;
        hiScore("flap", score); sfx(110, 250);
        fxSpawn((int)px, (int)py, 12, 16.0f, 40.0f);
      } else if (py < 2.0f) {
        py = 2.0f; vy = 0.0f;
      }

      for (int i = 0; i < 2; i++) {
        pipes[i].x -= 38.0f * dt;
        if (!pipes[i].scored && pipes[i].x + 10.0f < px) {
          pipes[i].scored = true;
          score++;
          sfx(1650, 35);
        }
        if (pipes[i].x < -16.0f) {
          pipes[i].x = 128.0f + 16.0f;
          pipes[i].gapY = random(12, 36);
          pipes[i].scored = false;
        }

        if (fabsf(pipes[i].x + 6.0f - px) < 8.0f) {
          if (py - 3.0f < pipes[i].gapY || py + 3.0f > pipes[i].gapY + GAP_H) {
            st = DEAD; since = now;
            hiScore("flap", score); sfx(90, 350);
            fxSpawn((int)px, (int)py, 14, 18.0f, 50.0f);
          }
        }
      }
    } else if ((pressed(K_A) || pressed(K_UP)) && (now - since) > 700) {
      reset();
    }

    u8g2.clearBuffer();

    int cloudX = (int)(now / 120) % (SCR_W + 20);
    drawDiscSafe(SCR_W - cloudX, 10, 6);
    drawDiscSafe(SCR_W - cloudX + 7, 9, 8);
    drawDiscSafe(SCR_W - cloudX + 14, 11, 5);

    for (int i = 0; i < 2; i++) {
      int ipx = (int)pipes[i].x;
      int gy = pipes[i].gapY;
      drawBoxSafe(ipx, 0, 12, gy);
      drawBoxSafe(ipx - 1, gy - 4, 14, 4);
      drawBoxSafe(ipx, gy + GAP_H, 12, 58 - (gy + GAP_H));
      drawBoxSafe(ipx - 1, gy + GAP_H, 14, 4);
    }

    drawHLineSafe(0, 58, SCR_W);
    int grassOff = (int)(now / 25) % 6;
    for (int x = -grassOff; x < SCR_W; x += 6) drawLineSafe(x, 63, x + 3, 58);

    int ipx = (int)px, ipy = (int)py;
    drawDiscSafe(ipx, ipy, 4);
    drawTriangleSafe(ipx + 3, ipy - 2, ipx + 3, ipy + 2, ipx + 6, ipy);
    int wingY = ipy + (vy < 0 ? -2 : 1);
    drawLineSafe(ipx - 3, ipy, ipx - 1, wingY);
    u8g2.setDrawColor(0);
    drawPixelSafe(ipx + 1, ipy - 2);
    u8g2.setDrawColor(1);

    u8g2.setFont(u8g2_font_7x14_tr);
    char buf[16];
    snprintf(buf, sizeof(buf), "%d", score);
    u8g2.drawStr((SCR_W - u8g2.getStrWidth(buf)) / 2, 14, buf);

    fxUpdateAndDraw(dt);

    if (st == DEAD) {
      snprintf(buf, sizeof(buf), "Score: %d", score);
      drawMessage("FLAP CRASH", buf);
    }
    present();
  }
}

// ================= GAME 21: ASTEROIDS =================
void gameAsteroids() {
  startGame();
  float px = 64.0f, py = 32.0f, vx = 0.0f, vy = 0.0f, angle = 0.0f;
  int score = 0, lives = 3;
  GState st = PLAYING;
  uint32_t since = 0;

  struct Laser { float x, y, vx, vy; uint32_t expire; bool active; };
  Laser lasers[3];
  for (int i = 0; i < 3; i++) lasers[i].active = false;

  struct Asteroid { float x, y, vx, vy; int8_t size; bool active; };
  Asteroid asts[8];

  auto spawnWave = [&]() {
    for (int i = 0; i < 8; i++) asts[i].active = false;
    for (int i = 0; i < 3; i++) {
      float ax = random(2) == 0 ? random(0, 30) : random(98, 128);
      float ay = random(0, 64);
      asts[i] = {ax, ay, random(-20, 20) * 0.7f, random(-20, 20) * 0.7f, 3, true};
    }
  };
  spawnWave();

  auto reset = [&]() {
    px = 64.0f; py = 32.0f; vx = 0.0f; vy = 0.0f; angle = 0.0f;
    score = 0; lives = 3; st = PLAYING;
    for (int i = 0; i < 3; i++) lasers[i].active = false;
    spawnWave();
  };

  while (tick()) {
    if (st == PLAYING) {
      if (held(K_LEFT))  angle -= 3.8f * dt;
      if (held(K_RIGHT)) angle += 3.8f * dt;

      if (held(K_UP) || held(K_B)) {
        vx += cosf(angle) * 55.0f * dt;
        vy += sinf(angle) * 55.0f * dt;
        sfx(180, 15);
        float exX = px - cosf(angle) * 6.0f;
        float exY = py - sinf(angle) * 6.0f;
        fxSpawn((int)exX, (int)exY, 2, 8.0f, 25.0f);
      }

      vx *= 0.985f; vy *= 0.985f;
      px += vx * dt; py += vy * dt;

      if (px < 0) px += SCR_W;
      if (px >= SCR_W) px -= SCR_W;
      if (py < 0) py += SCR_H;
      if (py >= SCR_H) py -= SCR_H;

      if (pressed(K_A)) {
        for (int i = 0; i < 3; i++) {
          if (!lasers[i].active) {
            lasers[i] = {
              px + cosf(angle) * 6.0f,
              py + sinf(angle) * 6.0f,
              vx + cosf(angle) * 110.0f,
              vy + sinf(angle) * 110.0f,
              now + 750,
              true
            };
            sfx(1500, 18);
            break;
          }
        }
      }

      if (pressed(K_DOWN)) {
        px = random(10, 118); py = random(10, 54);
        vx = 0; vy = 0;
        sfx(2200, 25);
      }

      for (int i = 0; i < 3; i++) {
        if (!lasers[i].active) continue;
        if (now > lasers[i].expire) { lasers[i].active = false; continue; }
        lasers[i].x += lasers[i].vx * dt;
        lasers[i].y += lasers[i].vy * dt;
        if (lasers[i].x < 0) lasers[i].x += SCR_W;
        if (lasers[i].x >= SCR_W) lasers[i].x -= SCR_W;
        if (lasers[i].y < 0) lasers[i].y += SCR_H;
        if (lasers[i].y >= SCR_H) lasers[i].y -= SCR_H;

        for (int a = 0; a < 8; a++) {
          if (!asts[a].active) continue;
          float rad = asts[a].size == 3 ? 9.0f : (asts[a].size == 2 ? 6.0f : 3.5f);
          float dx = lasers[i].x - asts[a].x, dy = lasers[i].y - asts[a].y;
          if (dx * dx + dy * dy < rad * rad) {
            lasers[i].active = false;
            asts[a].active = false;
            fxSpawn((int)asts[a].x, (int)asts[a].y, random(7, 12), 16.0f, 40.0f);
            if (asts[a].size == 3) {
              score += 20; sfx(250, 50);
              for (int k = 0, s = 0; k < 8 && s < 2; k++) {
                if (!asts[k].active) {
                  asts[k] = {asts[a].x, asts[a].y, random(-30, 30) * 0.8f, random(-30, 30) * 0.8f, 2, true};
                  s++;
                }
              }
            } else if (asts[a].size == 2) {
              score += 50; sfx(350, 40);
              for (int k = 0, s = 0; k < 8 && s < 2; k++) {
                if (!asts[k].active) {
                  asts[k] = {asts[a].x, asts[a].y, random(-40, 40) * 0.9f, random(-40, 40) * 0.9f, 1, true};
                  s++;
                }
              }
            } else {
              score += 100; sfx(500, 30);
            }
            break;
          }
        }
      }

      int activeAsts = 0;
      for (int a = 0; a < 8; a++) {
        if (!asts[a].active) continue;
        activeAsts++;
        asts[a].x += asts[a].vx * dt;
        asts[a].y += asts[a].vy * dt;
        if (asts[a].x < 0) asts[a].x += SCR_W;
        if (asts[a].x >= SCR_W) asts[a].x -= SCR_W;
        if (asts[a].y < 0) asts[a].y += SCR_H;
        if (asts[a].y >= SCR_H) asts[a].y -= SCR_H;

        float rad = asts[a].size == 3 ? 9.0f : (asts[a].size == 2 ? 6.0f : 3.5f);
        float dx = px - asts[a].x, dy = py - asts[a].y;
        if (dx * dx + dy * dy < (rad + 4.0f) * (rad + 4.0f)) {
          lives--;
          sfx(90, 350);
          fxSpawn((int)px, (int)py, 16, 22.0f, 60.0f);
          if (lives <= 0) {
            st = DEAD; since = now; hiScore("ast", score);
          } else {
            px = 64.0f; py = 32.0f; vx = 0; vy = 0;
          }
        }
      }

      if (activeAsts == 0) {
        score += 300;
        sfx(1800, 120);
        spawnWave();
      }
    } else if (pressed(K_A) && (now - since) > 700) {
      reset();
    }

    u8g2.clearBuffer();

    for (int a = 0; a < 8; a++) {
      if (!asts[a].active) continue;
      int ax = (int)asts[a].x, ay = (int)asts[a].y;
      int r = asts[a].size == 3 ? 8 : (asts[a].size == 2 ? 5 : 3);
      drawCircleSafe(ax, ay, r);
      if (r >= 5) drawPixelSafe(ax - 2, ay - 1);
    }

    for (int i = 0; i < 3; i++) {
      if (lasers[i].active) drawDiscSafe((int)lasers[i].x, (int)lasers[i].y, 1);
    }

    float cosA = cosf(angle), sinA = sinf(angle);
    int x1 = (int)(px + cosA * 7.0f);
    int y1 = (int)(py + sinA * 7.0f);
    int x2 = (int)(px - cosA * 5.0f - sinA * 4.0f);
    int y2 = (int)(py - sinA * 5.0f + cosA * 4.0f);
    int x3 = (int)(px - cosA * 5.0f + sinA * 4.0f);
    int y3 = (int)(py - sinA * 5.0f - cosA * 4.0f);
    drawLineSafe(x1, y1, x2, y2);
    drawLineSafe(x2, y2, (int)(px - cosA * 2.0f), (int)(py - sinA * 2.0f));
    drawLineSafe((int)(px - cosA * 2.0f), (int)(py - sinA * 2.0f), x3, y3);
    drawLineSafe(x3, y3, x1, y1);

    u8g2.setFont(u8g2_font_5x7_tr);
    char buf[20];
    snprintf(buf, sizeof(buf), "%d", score);
    drawLabel(0, 7, buf, false);
    snprintf(buf, sizeof(buf), "SHIPS:%d", lives);
    drawLabel(0, 7, buf, true);

    fxUpdateAndDraw(dt);

    if (st == DEAD) {
      snprintf(buf, sizeof(buf), "Score: %d", score);
      drawMessage("DESTROYED", buf);
    }
    present();
  }
}

// ================= EXTRAS & UTILITIES SUITE =================

void drawExtraIcon(int id, int x, int y, uint8_t f) {
  uint8_t g = f ^ 1;
  u8g2.setDrawColor(f);
  switch (id) {
    case 0: // Calculator
      drawFrameSafe(x + 3, y + 2, 14, 16);
      drawBoxSafe(x + 5, y + 4, 10, 3);
      drawBoxSafe(x + 5, y + 9, 2, 2);  drawBoxSafe(x + 9, y + 9, 2, 2);  drawBoxSafe(x + 13, y + 9, 2, 2);
      drawBoxSafe(x + 5, y + 13, 2, 2); drawBoxSafe(x + 9, y + 13, 2, 2); drawBoxSafe(x + 13, y + 13, 2, 2);
      break;
    case 1: // Piano / Synth
      drawFrameSafe(x + 1, y + 3, 18, 14);
      for (int k = 1; k < 6; k++) drawVLineSafe(x + 1 + k * 3, y + 3, 14);
      drawBoxSafe(x + 3, y + 3, 2, 8);
      drawBoxSafe(x + 6, y + 3, 2, 8);
      drawBoxSafe(x + 12, y + 3, 2, 8);
      drawBoxSafe(x + 15, y + 3, 2, 8);
      break;
    case 2: // Chrono / Stopwatch
      drawDiscSafe(x + 10, y + 11, 7);
      u8g2.setDrawColor(g);
      drawDiscSafe(x + 10, y + 11, 5);
      u8g2.setDrawColor(f);
      drawBoxSafe(x + 9, y + 2, 2, 2);
      drawLineSafe(x + 10, y + 11, x + 10, y + 7);
      drawLineSafe(x + 10, y + 11, x + 13, y + 11);
      break;
    case 3: // Metronome
      drawLineSafe(x + 10, y + 2, x + 3, y + 18);
      drawLineSafe(x + 10, y + 2, x + 17, y + 18);
      drawHLineSafe(x + 3, y + 18, 15);
      drawLineSafe(x + 10, y + 16, x + 14, y + 6);
      drawDiscSafe(x + 13, y + 9, 2);
      break;
    case 4: // Dice / RNG
      drawFrameSafe(x + 3, y + 3, 14, 14);
      drawBoxSafe(x + 5, y + 5, 2, 2);   drawBoxSafe(x + 13, y + 5, 2, 2);
      drawBoxSafe(x + 9, y + 9, 2, 2);
      drawBoxSafe(x + 5, y + 13, 2, 2);  drawBoxSafe(x + 13, y + 13, 2, 2);
      break;
    case 5: // Soundboard
      drawBoxSafe(x + 2, y + 7, 4, 6);
      drawLineSafe(x + 6, y + 7, x + 11, y + 3);
      drawLineSafe(x + 6, y + 12, x + 11, y + 16);
      drawVLineSafe(x + 11, y + 3, 14);
      drawCircleSafe(x + 11, y + 10, 4);
      drawCircleSafe(x + 11, y + 10, 7);
      break;
    case 6: // Torch / SOS
      drawBoxSafe(x + 2, y + 8, 7, 4);
      drawLineSafe(x + 9, y + 8, x + 13, y + 5);
      drawLineSafe(x + 9, y + 11, x + 13, y + 14);
      drawVLineSafe(x + 13, y + 5, 10);
      drawLineSafe(x + 15, y + 6, x + 18, y + 4);
      drawLineSafe(x + 15, y + 10, x + 19, y + 10);
      drawLineSafe(x + 15, y + 13, x + 18, y + 15);
      break;
    case 7: // System Telemetry
      drawFrameSafe(x + 4, y + 4, 12, 12);
      drawBoxSafe(x + 7, y + 7, 6, 6);
      drawVLineSafe(x + 6, y + 1, 3);  drawVLineSafe(x + 10, y + 1, 3);  drawVLineSafe(x + 13, y + 1, 3);
      drawVLineSafe(x + 6, y + 16, 3); drawVLineSafe(x + 10, y + 16, 3); drawVLineSafe(x + 13, y + 16, 3);
      drawHLineSafe(x + 1, y + 6, 3);  drawHLineSafe(x + 1, y + 10, 3);  drawHLineSafe(x + 1, y + 13, 3);
      drawHLineSafe(x + 16, y + 6, 3); drawHLineSafe(x + 16, y + 10, 3); drawHLineSafe(x + 16, y + 13, 3);
      break;
    case 8: // Button Tester
      drawFrameSafe(x + 2, y + 4, 16, 12);
      drawBoxSafe(x + 5, y + 9, 4, 2);
      drawBoxSafe(x + 6, y + 8, 2, 4);
      drawDiscSafe(x + 12, y + 11, 1);
      drawDiscSafe(x + 15, y + 9, 1);
      break;
    case 9: // WiFi Scanner
      drawDiscSafe(x + 10, y + 15, 1);
      drawCircleSafe(x + 10, y + 15, 5);
      drawCircleSafe(x + 10, y + 15, 9);
      drawCircleSafe(x + 10, y + 15, 13);
      u8g2.setDrawColor(g);
      drawBoxSafe(x + 2, y + 16, 16, 4);
      u8g2.setDrawColor(f);
      break;
    case 10: // Reaction Tester
      drawCircleSafe(x + 10, y + 10, 8);
      drawLineSafe(x + 11, y + 4, x + 8, y + 10);
      drawLineSafe(x + 8, y + 10, x + 12, y + 10);
      drawLineSafe(x + 12, y + 10, x + 9, y + 16);
      break;
    case 11: // Unit Converter
      drawLineSafe(x + 4, y + 7, x + 16, y + 7);
      drawLineSafe(x + 13, y + 4, x + 16, y + 7);
      drawLineSafe(x + 13, y + 10, x + 16, y + 7);
      drawLineSafe(x + 16, y + 13, x + 4, y + 13);
      drawLineSafe(x + 7, y + 10, x + 4, y + 13);
      drawLineSafe(x + 7, y + 16, x + 4, y + 13);
      break;
    case 12: // I2C Scanner
      drawFrameSafe(x + 5, y + 4, 10, 12);
      drawBoxSafe(x + 7, y + 7, 6, 6);
      drawHLineSafe(x + 2, y + 6, 3);  drawHLineSafe(x + 2, y + 10, 3);  drawHLineSafe(x + 2, y + 13, 3);
      drawHLineSafe(x + 15, y + 6, 3); drawHLineSafe(x + 15, y + 10, 3); drawHLineSafe(x + 15, y + 13, 3);
      break;
    case 13: // Morse Code Lab
      drawHLineSafe(x + 2, y + 14, 16);
      drawVLineSafe(x + 5, y + 10, 4);
      drawLineSafe(x + 5, y + 10, x + 15, y + 8);
      drawDiscSafe(x + 15, y + 7, 2);
      drawBoxSafe(x + 4, y + 4, 3, 2);
      drawBoxSafe(x + 9, y + 4, 7, 2);
      break;
    case 14: // Cipher & Password
      drawFrameSafe(x + 4, y + 9, 12, 9);
      drawCircleSafe(x + 10, y + 8, 4);
      drawBoxSafe(x + 9, y + 12, 2, 3);
      break;
    case 15: // 4-Channel Tally
      drawFrameSafe(x + 3, y + 5, 14, 11);
      drawBoxSafe(x + 5, y + 7, 10, 5);
      u8g2.setDrawColor(g);
      drawHLineSafe(x + 6, y + 9, 8);
      u8g2.setDrawColor(f);
      drawBoxSafe(x + 8, y + 2, 4, 3);
      break;
    case 16: // Pixel Pad
      drawFrameSafe(x + 2, y + 3, 16, 14);
      drawLineSafe(x + 13, y + 5, x + 8, y + 11);
      drawLineSafe(x + 8, y + 11, x + 6, y + 13);
      drawPixelSafe(x + 5, y + 14);
      break;
    case 17: // Breathing Zen
      drawCircleSafe(x + 10, y + 10, 8);
      drawCircleSafe(x + 10, y + 10, 5);
      drawDiscSafe(x + 10, y + 10, 2);
      break;
    case 18: // Digital Logic Lab
      drawLineSafe(x + 3, y + 5, x + 8, y + 5);
      drawLineSafe(x + 3, y + 15, x + 8, y + 15);
      drawVLineSafe(x + 8, y + 5, 11);
      drawLineSafe(x + 8, y + 5, x + 14, y + 10);
      drawLineSafe(x + 8, y + 15, x + 14, y + 10);
      drawLineSafe(x + 14, y + 10, x + 18, y + 10);
      break;
    case 19: // Spirit Level
      drawCircleSafe(x + 10, y + 10, 8);
      drawCircleSafe(x + 10, y + 10, 3);
      drawDiscSafe(x + 10, y + 10, 2);
      drawVLineSafe(x + 10, y + 1, 2);
      drawVLineSafe(x + 10, y + 17, 2);
      drawHLineSafe(x + 1, y + 10, 2);
      drawHLineSafe(x + 17, y + 10, 2);
      break;
    case 20: // Resistor Code
      drawHLineSafe(x + 1, y + 10, 4);
      drawHLineSafe(x + 15, y + 10, 4);
      drawFrameSafe(x + 5, y + 6, 10, 8);
      drawVLineSafe(x + 7, y + 6, 8);
      drawVLineSafe(x + 9, y + 6, 8);
      drawVLineSafe(x + 11, y + 6, 8);
      break;
    case 21: // Tone Generator
      drawVLineSafe(x + 7, y + 3, 9);
      drawVLineSafe(x + 13, y + 3, 9);
      drawHLineSafe(x + 7, y + 12, 7);
      drawVLineSafe(x + 10, y + 12, 6);
      drawCircleSafe(x + 16, y + 7, 3);
      break;
    case 22: // Periodic Table
      drawFrameSafe(x + 4, y + 4, 12, 12);
      u8g2.setDrawColor(f);
      drawBoxSafe(x + 6, y + 6, 8, 4);
      u8g2.setDrawColor(g);
      drawHLineSafe(x + 7, y + 7, 6);
      u8g2.setDrawColor(f);
      drawBoxSafe(x + 8, y + 12, 4, 2);
      break;
    case 23: // Base / Radix
      drawFrameSafe(x + 3, y + 5, 14, 11);
      drawBoxSafe(x + 5, y + 7, 3, 3);
      drawFrameSafe(x + 9, y + 7, 3, 3);
      drawBoxSafe(x + 13, y + 7, 3, 3);
      drawHLineSafe(x + 5, y + 13, 10);
      break;
  }
  u8g2.setDrawColor(1);
}

// --- 1. CALCULATOR ---
void appCalculator() {
  startGame();
  double acc = 0.0;
  char buf[16] = "0";
  char op = 0;
  bool startNew = true;
  int curR = 0, curC = 0;

  const char* keys[4][4] = {
    {"7", "8", "9", "/"},
    {"4", "5", "6", "*"},
    {"1", "2", "3", "-"},
    {"0", ".", "=", "+"}
  };

  while (tick()) {
    if (pressed(K_LEFT))  { curC = (curC + 3) % 4; sfx(1200, 10); }
    if (pressed(K_RIGHT)) { curC = (curC + 1) % 4; sfx(1200, 10); }
    if (pressed(K_UP))    { curR = (curR + 3) % 4; sfx(1200, 10); }
    if (pressed(K_DOWN))  { curR = (curR + 1) % 4; sfx(1200, 10); }

    if (pressed(K_B)) {
      sfx(800, 20);
      int len = strlen(buf);
      if (len > 1 && !startNew) {
        buf[len - 1] = '\0';
      } else {
        strcpy(buf, "0");
        startNew = true;
        if (len <= 1) { op = 0; acc = 0.0; }
      }
    }

    if (pressed(K_A)) {
      const char* k = keys[curR][curC];
      if (k[0] >= '0' && k[0] <= '9') {
        sfx(1600, 12);
        if (startNew || strcmp(buf, "0") == 0) {
          buf[0] = k[0];
          buf[1] = '\0';
          startNew = false;
        } else if (strlen(buf) < 11) {
          int len = strlen(buf);
          buf[len] = k[0];
          buf[len + 1] = '\0';
        }
      } else if (k[0] == '.') {
        sfx(1600, 12);
        if (startNew) {
          strcpy(buf, "0.");
          startNew = false;
        } else if (!strchr(buf, '.') && strlen(buf) < 10) {
          strcat(buf, ".");
        }
      } else if (k[0] == '=') {
        sfx(2200, 30);
        if (op != 0) {
          double bVal = atof(buf);
          double res = 0.0;
          bool err = false;
          if (op == '+') res = acc + bVal;
          else if (op == '-') res = acc - bVal;
          else if (op == '*') res = acc * bVal;
          else if (op == '/') {
            if (fabs(bVal) < 1e-9) err = true;
            else res = acc / bVal;
          }
          if (err) {
            strcpy(buf, "Div by 0");
            startNew = true;
            op = 0; acc = 0;
          } else {
            snprintf(buf, sizeof(buf), "%.5g", res);
            acc = res;
            op = 0;
            startNew = true;
          }
        }
      } else { // Operator (+, -, *, /)
        sfx(1800, 18);
        if (op != 0 && !startNew) {
          double bVal = atof(buf);
          if (op == '+') acc += bVal;
          else if (op == '-') acc -= bVal;
          else if (op == '*') acc *= bVal;
          else if (op == '/' && fabs(bVal) >= 1e-9) acc /= bVal;
          snprintf(buf, sizeof(buf), "%.5g", acc);
        } else {
          acc = atof(buf);
        }
        op = k[0];
        startNew = true;
      }
    }

    u8g2.clearBuffer();
    u8g2.setFont(u8g2_font_5x7_tr);
    u8g2.drawStr(2, 7, "CALCULATOR");
    char opBadge[12] = "";
    if (op) snprintf(opBadge, sizeof(opBadge), "[%c]", op);
    u8g2.drawStr(58, 7, opBadge);
    u8g2.drawStr(78, 7, "B:Del A+B:X");

    drawFrameSafe(2, 9, 124, 13);
    u8g2.setFont(u8g2_font_6x10_tr);
    int txtW = u8g2.getStrWidth(buf);
    u8g2.drawStr(122 - txtW, 19, buf);

    int kw = 28, kh = 9;
    int kx0 = 4, ky0 = 24;
    for (int r = 0; r < 4; r++) {
      for (int c = 0; c < 4; c++) {
        int x = kx0 + c * (kw + 3);
        int y = ky0 + r * (kh + 1);
        bool sel = (r == curR && c == curC);
        if (sel) {
          drawBoxSafe(x, y, kw, kh);
          u8g2.setDrawColor(0);
        } else {
          drawFrameSafe(x, y, kw, kh);
          u8g2.setDrawColor(1);
        }
        u8g2.setFont(u8g2_font_5x7_tr);
        int kwTxt = u8g2.getStrWidth(keys[r][c]);
        u8g2.drawStr(x + (kw - kwTxt) / 2, y + 7, keys[r][c]);
        u8g2.setDrawColor(1);
      }
    }
    present();
  }
}

// --- 2. BUZZER PIANO & SYNTH ---
const float PIANO_FREQ[12] = {
  261.63f, 277.18f, 293.66f, 311.13f, 329.63f, 349.23f,
  369.99f, 392.00f, 415.30f, 440.00f, 466.16f, 493.88f
};
const char* const PIANO_NAMES[12] = {
  "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"
};

void appPiano() {
  startGame();
  const uint16_t FREQS[6] = {262, 294, 330, 349, 392, 440};
  const char* const NOTES[6] = {"C4", "D4", "E4", "F4", "G4", "A4"};
  const char* const BTN_LBL[6] = {"LEFT", "DOWN", "UP", "RGHT", "A", "B"};
  const uint8_t BTN_MASK[6] = {K_LEFT, K_DOWN, K_UP, K_RIGHT, K_A, K_B};

  uint32_t exitHoldStart = 0;

  while (tick()) {
    // Hold UP + DOWN together for > 500ms to exit
    if (held(K_UP) && held(K_DOWN)) {
      if (exitHoldStart == 0) exitHoldStart = millis();
      else if (millis() - exitHoldStart > 500) {
        buzz(0);
        sfx(1200, 20);
        return;
      }
    } else {
      exitHoldStart = 0;
    }

    int activeKey = -1;
    for (int i = 5; i >= 0; i--) {
      if (held(BTN_MASK[i])) {
        activeKey = i;
        break;
      }
    }

    if (activeKey >= 0 && exitHoldStart == 0) {
      buzz(FREQS[activeKey]);
    } else {
      buzz(0);
    }

    u8g2.clearBuffer();
    u8g2.setFont(u8g2_font_5x7_tr);
    u8g2.drawStr(2, 7, "6-KEY LIVE PIANO");
    u8g2.drawStr(68, 7, "[HOLD U+D: EXIT]");

    if (activeKey >= 0) {
      u8g2.setFont(u8g2_font_7x14_tr);
      char nbuf[16];
      snprintf(nbuf, sizeof(nbuf), "%s", NOTES[activeKey]);
      u8g2.drawStr(6, 21, nbuf);

      u8g2.setFont(u8g2_font_5x7_tr);
      snprintf(nbuf, sizeof(nbuf), "%u Hz", FREQS[activeKey]);
      u8g2.drawStr(32, 21, nbuf);

      // Soundwave animation
      for (int x = 75; x < 125; x += 2) {
        int wy = 17 + (int)(sinf((millis() * 0.03f) + x * 0.4f) * 5.0f);
        drawPixelSafe(x, wy);
      }
    } else {
      u8g2.setFont(u8g2_font_5x7_tr);
      u8g2.drawStr(6, 21, "PRESS ANY BUTTON TO PLAY");
    }

    int kw = 19;
    int kh = 37;
    int kx0 = 7;
    int ky0 = 25;

    for (int i = 0; i < 6; i++) {
      int kx = kx0 + i * kw;
      bool isPressed = held(BTN_MASK[i]);
      if (isPressed) {
        drawBoxSafe(kx, ky0, kw, kh);
        u8g2.setDrawColor(0);
        u8g2.setFont(u8g2_font_5x7_tr);
        u8g2.drawStr(kx + (kw - u8g2.getStrWidth(BTN_LBL[i])) / 2, ky0 + 12, BTN_LBL[i]);
        u8g2.setFont(u8g2_font_6x10_tr);
        u8g2.drawStr(kx + (kw - u8g2.getStrWidth(NOTES[i])) / 2, ky0 + 26, NOTES[i]);
        u8g2.setDrawColor(1);
      } else {
        drawFrameSafe(kx, ky0, kw, kh);
        u8g2.setFont(u8g2_font_5x7_tr);
        u8g2.drawStr(kx + (kw - u8g2.getStrWidth(BTN_LBL[i])) / 2, ky0 + 12, BTN_LBL[i]);
        u8g2.setFont(u8g2_font_6x10_tr);
        u8g2.drawStr(kx + (kw - u8g2.getStrWidth(NOTES[i])) / 2, ky0 + 26, NOTES[i]);
      }
    }

    if (exitHoldStart > 0) {
      int pw = (int)(((millis() - exitHoldStart) * 120) / 500);
      drawBoxSafe(4, 61, imin(pw, 120), 2);
    }

    present();
  }
  buzz(0);
}

// --- 3. CHRONO & COUNTDOWN TIMER ---
void appChrono() {
  startGame();
  uint8_t mode = 0; // 0: Stopwatch, 1: Countdown Timer

  // Stopwatch state
  uint32_t swElapsed = 0, swLast = 0;
  bool swRunning = false;
  uint32_t laps[3] = {0, 0, 0};
  uint8_t lapCount = 0;

  // Timer state
  int32_t timerSetSec = 60;
  int32_t timerRemainMs = 60000;
  uint32_t timerLast = 0;
  bool timerRunning = false, timerAlarm = false;

  while (tick()) {
    uint32_t cur = millis();

    // Mode Toggle
    if (pressed(K_UP) || pressed(K_DOWN)) {
      mode ^= 1;
      sfx(1400, 15);
    }

    if (mode == 0) {
      // Stopwatch logic
      if (swRunning) {
        swElapsed += (cur - swLast);
        swLast = cur;
      }
      if (pressed(K_A)) {
        swRunning = !swRunning;
        swLast = cur;
        sfx(swRunning ? 1800 : 1200, 20);
      }
      if (pressed(K_B)) {
        if (swRunning) {
          laps[2] = laps[1];
          laps[1] = laps[0];
          laps[0] = swElapsed;
          if (lapCount < 3) lapCount++;
          sfx(1600, 20);
        } else {
          swElapsed = 0;
          lapCount = 0;
          laps[0] = laps[1] = laps[2] = 0;
          sfx(900, 25);
        }
      }
    } else {
      // Timer logic
      if (timerRunning) {
        int32_t delta = cur - timerLast;
        timerLast = cur;
        timerRemainMs -= delta;
        if (timerRemainMs <= 0) {
          timerRemainMs = 0;
          timerRunning = false;
          timerAlarm = true;
        }
      }

      if (timerAlarm) {
        if (btn != 0) {
          timerAlarm = false;
          buzz(0);
        } else {
          if ((cur / 250) % 2 == 0) buzz(2200); else buzz(1100);
        }
      } else {
        if (!timerRunning) {
          if (pressed(K_LEFT))  { timerSetSec = imax(10, timerSetSec - 10); timerRemainMs = timerSetSec * 1000; sfx(1200, 10); }
          if (pressed(K_RIGHT)) { timerSetSec = imin(3600, timerSetSec + 10); timerRemainMs = timerSetSec * 1000; sfx(1200, 10); }
        }
        if (pressed(K_A)) {
          timerRunning = !timerRunning;
          timerLast = cur;
          sfx(timerRunning ? 1800 : 1200, 20);
        }
        if (pressed(K_B)) {
          timerRunning = false;
          timerRemainMs = timerSetSec * 1000;
          sfx(900, 20);
        }
      }
    }

    u8g2.clearBuffer();
    u8g2.setFont(u8g2_font_5x7_tr);
    u8g2.drawStr(2, 7, mode == 0 ? "STOPWATCH" : "TIMER");
    u8g2.drawStr(60, 7, "U/D:MODE");
    u8g2.drawStr(108, 7, "A+B:X");

    if (mode == 0) {
      // Stopwatch Display
      uint32_t totalSec = swElapsed / 1000;
      uint32_t mm = totalSec / 60;
      uint32_t ss = totalSec % 60;
      uint32_t hh = (swElapsed % 1000) / 10;

      char timeBuf[24];
      snprintf(timeBuf, sizeof(timeBuf), "%02lu:%02lu.%02lu", (unsigned long)mm, (unsigned long)ss, (unsigned long)hh);
      u8g2.setFont(u8g2_font_7x14_tr);
      int w = u8g2.getStrWidth(timeBuf);
      u8g2.drawStr((SCR_W - w) / 2, 23, timeBuf);

      u8g2.setFont(u8g2_font_5x7_tr);
      u8g2.drawStr(2, 33, swRunning ? "[A: PAUSE  B: LAP]" : "[A: START  B: RESET]");

      for (int i = 0; i < lapCount; i++) {
        uint32_t lSec = laps[i] / 1000;
        char lapBuf[24];
        snprintf(lapBuf, sizeof(lapBuf), "L%d %02lu:%02lu.%02lu", i + 1, (unsigned long)(lSec / 60), (unsigned long)(lSec % 60), (unsigned long)((laps[i] % 1000) / 10));
        u8g2.drawStr(12, 43 + i * 9, lapBuf);
      }
    } else {
      // Countdown Timer Display
      uint32_t totalSec = (timerRemainMs + 999) / 1000;
      uint32_t mm = totalSec / 60;
      uint32_t ss = totalSec % 60;

      char timeBuf[24];
      snprintf(timeBuf, sizeof(timeBuf), "%02lu:%02lu", (unsigned long)mm, (unsigned long)ss);
      u8g2.setFont(u8g2_font_logisoso16_tr);
      int w = u8g2.getStrWidth(timeBuf);
      u8g2.drawStr((SCR_W - w) / 2, 30, timeBuf);

      // Progress bar
      drawFrameSafe(14, 34, 100, 6);
      int barW = (timerSetSec > 0) ? (int)((timerRemainMs * 96) / (timerSetSec * 1000)) : 0;
      if (barW > 96) barW = 96;
      if (barW > 0) drawBoxSafe(16, 36, barW, 2);

      u8g2.setFont(u8g2_font_5x7_tr);
      if (timerAlarm) {
        u8g2.drawStr(28, 52, "TIME'S UP! [KEY]");
      } else {
        u8g2.drawStr(2, 52, timerRunning ? "[A: PAUSE  B: RESET]" : "[A: START  B: RESET]");
        u8g2.drawStr(2, 62, "L/R: +/- 10 SEC");
      }
    }
    present();
  }
  buzz(0);
}

// --- 4. METRONOME & TAP TEMPO ---
void appMetronome() {
  startGame();
  int bpm = 120;
  int bpb = 4; // 2, 3, 4, 6
  bool running = true;
  uint32_t lastBeat = millis();
  int curBeat = 0;

  // Tap tempo buffer
  uint32_t taps[4] = {0};
  uint8_t tapIdx = 0;

  while (tick()) {
    uint32_t cur = millis();
    uint32_t period = 60000 / bpm;

    // Tap tempo on A
    if (pressed(K_A)) {
      taps[tapIdx % 4] = cur;
      tapIdx++;
      if (tapIdx >= 2) {
        int count = (tapIdx >= 4) ? 4 : tapIdx;
        uint32_t totalDelta = 0;
        int pairs = 0;
        for (int i = 1; i < count; i++) {
          int i1 = (tapIdx - i) % 4;
          int i0 = (tapIdx - i - 1) % 4;
          uint32_t d = taps[i1] - taps[i0];
          if (d > 200 && d < 2000) { totalDelta += d; pairs++; }
        }
        if (pairs > 0) {
          uint32_t avg = totalDelta / pairs;
          bpm = clampf(60000.0f / avg, 30.0f, 250.0f);
          lastBeat = cur;
          curBeat = 0;
        }
      }
      sfx(2000, 15);
    }

    if (pressed(K_B)) {
      running = !running;
      sfx(running ? 1600 : 1000, 15);
      lastBeat = cur;
      curBeat = 0;
    }

    if (pressed(K_LEFT))  { if (bpm > 30)  bpm--; sfx(1200, 8); }
    if (pressed(K_RIGHT)) { if (bpm < 250) bpm++; sfx(1200, 8); }
    if (pressed(K_UP))    { if (bpb == 2) bpb = 3; else if (bpb == 3) bpb = 4; else if (bpb == 4) bpb = 6; else bpb = 2; sfx(1400, 10); }
    if (pressed(K_DOWN))  { if (bpb == 6) bpb = 4; else if (bpb == 4) bpb = 3; else if (bpb == 3) bpb = 2; else bpb = 6; sfx(1400, 10); }

    if (running && cur - lastBeat >= period) {
      curBeat = (curBeat + 1) % bpb;
      lastBeat = cur;
      if (curBeat == 0) sfx(2400, 18); // Accented downbeat
      else sfx(1200, 10);              // Regular beat
    }

    u8g2.clearBuffer();
    u8g2.setFont(u8g2_font_5x7_tr);
    u8g2.drawStr(2, 7, "METRONOME");
    char bpbStr[12];
    snprintf(bpbStr, sizeof(bpbStr), "%d/4 (U/D)", bpb);
    u8g2.drawStr(60, 7, bpbStr);
    u8g2.drawStr(108, 7, "A+B:X");

    // Beat indicator lights
    for (int b = 0; b < bpb; b++) {
      int bx = 64 - (bpb * 10) / 2 + b * 10 + 5;
      if (b == curBeat && running) {
        drawDiscSafe(bx, 13, (b == 0) ? 3 : 2);
      } else {
        drawCircleSafe(bx, 13, (b == 0) ? 3 : 2);
      }
    }

    // BPM Readout
    char bpmBuf[16];
    snprintf(bpmBuf, sizeof(bpmBuf), "%d BPM", bpm);
    u8g2.setFont(u8g2_font_7x14_tr);
    u8g2.drawStr(4, 28, bpmBuf);

    // Tempo Italian label
    u8g2.setFont(u8g2_font_5x7_tr);
    const char* tempoName = "Andante";
    if (bpm < 60) tempoName = "Largo";
    else if (bpm < 76) tempoName = "Adagio";
    else if (bpm < 108) tempoName = "Andante";
    else if (bpm < 120) tempoName = "Moderato";
    else if (bpm < 156) tempoName = "Allegro";
    else tempoName = "Presto";
    u8g2.drawStr(4, 38, tempoName);

    // Animated Swinging Pendulum
    float progress = (period > 0) ? (float)(cur - lastBeat) / (float)period : 0.0f;
    float phase = (curBeat + progress) * 3.14159f;
    float angle = running ? 0.45f * sinf(phase) : 0.0f;
    int pivotX = 96, pivotY = 56;
    int rodLen = 26;
    int bobX = pivotX + (int)(sinf(angle) * rodLen);
    int bobY = pivotY - (int)(cosf(angle) * rodLen);

    drawLineSafe(pivotX - 10, pivotY + 2, pivotX + 10, pivotY + 2);
    drawLineSafe(pivotX, pivotY, bobX, bobY);
    drawDiscSafe(bobX, bobY, 4);

    u8g2.drawStr(4, 52, "[A: TAP TEMPO]");
    u8g2.drawStr(4, 62, running ? "[B: STOP]" : "[B: START]");
    present();
  }
}

// --- 5. DICE, COIN & RNG SUITE ---
void appDiceRNG() {
  startGame();
  uint8_t tool = 0; // 0: Coin, 1: D6, 2: D20, 3: Magic 8-Ball
  int anim = 0;
  int headsCount = 0, tailsCount = 0;
  int coinRes = 0;
  int d6Res = 1;
  int d20Res = 20;
  int eightRes = 0;

  const char* const eightAnswers[8] = {
    "YES!", "NO WAY", "ABSOLUTELY", "DOUBTFUL",
    "ASK AGAIN", "MOST LIKELY", "NO CHANCE", "OUTLOOK GOOD"
  };

  while (tick()) {
    if (pressed(K_UP))   { tool = (tool + 3) % 4; anim = 0; sfx(1200, 10); }
    if (pressed(K_DOWN)) { tool = (tool + 1) % 4; anim = 0; sfx(1200, 10); }

    if (pressed(K_A) && anim == 0) {
      anim = 20;
      sfx(2000, 30);
    }

    if (anim > 0) {
      anim--;
      if (tool == 0) {
        if (anim == 0) {
          coinRes = random(2);
          if (coinRes == 0) headsCount++; else tailsCount++;
          sfx(2600, 40);
        } else {
          sfx(1600 + random(800), 6);
        }
      } else if (tool == 1) {
        d6Res = random(1, 7);
        sfx(800 + random(600), 8);
        if (anim == 0) sfx(1800, 30);
      } else if (tool == 2) {
        d20Res = random(1, 21);
        sfx(1000 + random(800), 8);
        if (anim == 0) {
          if (d20Res == 20) sfx(2400, 80);
          else if (d20Res == 1) sfx(300, 80);
          else sfx(1600, 25);
        }
      } else if (tool == 3) {
        eightRes = random(8);
        sfx(1200 + random(400), 10);
        if (anim == 0) sfx(2000, 40);
      }
    }

    u8g2.clearBuffer();
    u8g2.setFont(u8g2_font_5x7_tr);
    const char* titles[4] = {"COIN FLIPPER", "D6 DICE", "D20 DICE", "MAGIC 8-BALL"};
    u8g2.drawStr(2, 7, titles[tool]);
    u8g2.drawStr(84, 7, "U/D:CYCLE");

    if (tool == 0) {
      // Coin graphic
      int cx = 40, cy = 34;
      int r = 16;
      if (anim > 0) {
        float ang = anim * 0.5f;
        int rh = abs((int)(sinf(ang) * r));
        if (rh < 1) rh = 1;
        drawFrameSafe(cx - r, cy - rh, r * 2, rh * 2);
      } else {
        drawCircleSafe(cx, cy, r);
        drawCircleSafe(cx, cy, r - 3);
        u8g2.setFont(u8g2_font_7x14_tr);
        u8g2.drawStr(cx - 4, cy + 5, (coinRes == 0) ? "H" : "T");
      }
      u8g2.setFont(u8g2_font_6x10_tr);
      u8g2.drawStr(70, 26, (coinRes == 0) ? "HEADS" : "TAILS");
      u8g2.setFont(u8g2_font_5x7_tr);
      char statBuf[24];
      int total = headsCount + tailsCount;
      int pct = (total > 0) ? (headsCount * 100 / total) : 0;
      snprintf(statBuf, sizeof(statBuf), "H:%d  T:%d (%d%%)", headsCount, tailsCount, pct);
      u8g2.drawStr(70, 38, statBuf);
    } else if (tool == 1) {
      // D6 Die
      int dx = 28, dy = 18;
      drawFrameSafe(dx, dy, 32, 32);
      if (d6Res % 2 == 1) drawDiscSafe(dx + 16, dy + 16, 2);
      if (d6Res >= 2) { drawDiscSafe(dx + 8, dy + 8, 2);   drawDiscSafe(dx + 24, dy + 24, 2); }
      if (d6Res >= 4) { drawDiscSafe(dx + 24, dy + 8, 2);  drawDiscSafe(dx + 8, dy + 24, 2); }
      if (d6Res == 6) { drawDiscSafe(dx + 8, dy + 16, 2);  drawDiscSafe(dx + 24, dy + 16, 2); }

      char resBuf[16];
      snprintf(resBuf, sizeof(resBuf), "ROLL: %d", d6Res);
      u8g2.setFont(u8g2_font_7x14_tr);
      u8g2.drawStr(72, 34, resBuf);
    } else if (tool == 2) {
      // D20 Die
      int cx = 40, cy = 34;
      drawTriangleSafe(cx, cy - 16, cx - 18, cy + 12, cx + 18, cy + 12);
      drawTriangleSafe(cx, cy + 6, cx - 12, cy - 8, cx + 12, cy - 8);

      char resBuf[16];
      snprintf(resBuf, sizeof(resBuf), "%d", d20Res);
      u8g2.setFont(u8g2_font_7x14_tr);
      int w = u8g2.getStrWidth(resBuf);
      u8g2.drawStr(cx - w / 2, cy + 4, resBuf);

      u8g2.setFont(u8g2_font_5x7_tr);
      if (d20Res == 20) u8g2.drawStr(70, 30, "NATURAL 20!");
      else if (d20Res == 1) u8g2.drawStr(70, 30, "CRITICAL FAIL!");
      else {
        snprintf(resBuf, sizeof(resBuf), "D20: %d", d20Res);
        u8g2.drawStr(70, 30, resBuf);
      }
    } else {
      // Magic 8-Ball
      int cx = 32, cy = 34;
      int sx = (anim > 0) ? random(-2, 3) : 0;
      drawCircleSafe(cx + sx, cy, 18);
      drawDiscSafe(cx + sx, cy, 8);
      u8g2.setDrawColor(0);
      u8g2.setFont(u8g2_font_5x7_tr);
      u8g2.drawStr(cx + sx - 2, cy + 3, "8");
      u8g2.setDrawColor(1);

      u8g2.drawStr(60, 28, "THE 8-BALL SAYS:");
      u8g2.setFont(u8g2_font_6x10_tr);
      u8g2.drawStr(60, 42, eightAnswers[eightRes]);
    }

    u8g2.setFont(u8g2_font_5x7_tr);
    u8g2.drawStr(2, 63, anim > 0 ? "ROLLING..." : "[A: ROLL / FLIP / SHAKE]");
    present();
  }
}

// --- 6. 8-BIT SOUNDBOARD ---
void appSoundboard() {
  startGame();
  int selR = 0, selC = 0;
  const char* sfxNames[12] = {
    "COIN", "JUMP", "LASER", "EXPLODE",
    "PWRUP", "1-UP", "WARP", "SIREN",
    "HIT", "PHAZER", "OVER", "WIN"
  };

  auto playSynth = [](int id) {
    switch (id) {
      case 0: // Coin
        buzz(987); delay(60); buzz(1318); delay(160); buzz(0); break;
      case 1: // Jump
        for (int f = 160; f < 650; f += 28) { buzz(f); delay(4); } buzz(0); break;
      case 2: // Laser
        for (int f = 1900; f > 300; f -= 50) { buzz(f); delay(3); } buzz(0); break;
      case 3: // Explode
        for (int f = 150; f > 30; f -= 5) { buzz(f + random(30)); delay(8); } buzz(0); break;
      case 4: // Powerup
        buzz(262); delay(45); buzz(330); delay(45); buzz(392); delay(45); buzz(523); delay(120); buzz(0); break;
      case 5: // 1-Up
        buzz(330); delay(60); buzz(392); delay(60); buzz(659); delay(60); buzz(523); delay(60); buzz(587); delay(60); buzz(784); delay(140); buzz(0); break;
      case 6: // Warp
        for (int i = 0; i < 4; i++) { buzz(420); delay(25); buzz(780); delay(25); } buzz(0); break;
      case 7: // Siren
        for (int i = 0; i < 3; i++) { buzz(880); delay(70); buzz(660); delay(70); } buzz(0); break;
      case 8: // Hit
        for (int f = 320; f > 60; f -= 40) { buzz(f); delay(8); } buzz(0); break;
      case 9: // Phazer
        for (int i = 0; i < 6; i++) { buzz(1200); delay(15); buzz(900); delay(15); } buzz(0); break;
      case 10: // Game Over
        buzz(400); delay(90); buzz(380); delay(90); buzz(350); delay(90); buzz(300); delay(180); buzz(0); break;
      case 11: // Win
        buzz(523); delay(50); buzz(659); delay(50); buzz(784); delay(50); buzz(1046); delay(180); buzz(0); break;
    }
  };

  while (tick()) {
    if (pressed(K_LEFT))  { selC = (selC + 3) % 4; sfx(1200, 8); }
    if (pressed(K_RIGHT)) { selC = (selC + 1) % 4; sfx(1200, 8); }
    if (pressed(K_UP))    { selR = (selR + 2) % 3; sfx(1200, 8); }
    if (pressed(K_DOWN))  { selR = (selR + 1) % 3; sfx(1200, 8); }

    if (pressed(K_A)) {
      int idx = selR * 4 + selC;
      playSynth(idx);
    }

    u8g2.clearBuffer();
    u8g2.setFont(u8g2_font_5x7_tr);
    u8g2.drawStr(2, 7, "8-BIT SOUNDBOARD");
    u8g2.drawStr(80, 7, "[A: PLAY]");

    int bw = 29, bh = 14;
    int bx0 = 3, by0 = 12;
    for (int r = 0; r < 3; r++) {
      for (int c = 0; c < 4; c++) {
        int idx = r * 4 + c;
        int x = bx0 + c * (bw + 2);
        int y = by0 + r * (bh + 2);
        bool sel = (r == selR && c == selC);
        if (sel) {
          drawBoxSafe(x, y, bw, bh);
          u8g2.setDrawColor(0);
        } else {
          drawFrameSafe(x, y, bw, bh);
          u8g2.setDrawColor(1);
        }
        u8g2.setFont(u8g2_font_5x7_tr);
        int tw = u8g2.getStrWidth(sfxNames[idx]);
        u8g2.drawStr(x + (bw - tw) / 2, y + 10, sfxNames[idx]);
        u8g2.setDrawColor(1);
      }
    }

    // Audio Visualizer waveform bars along footer
    for (int b = 0; b < 16; b++) {
      int h = 1 + (int)(sinf((millis() * 0.015f) + b * 0.6f) * 3.0f + 3.0f);
      drawVLineSafe(10 + b * 7, 63 - h, h);
    }
    present();
  }
}

// --- 7. TORCH & SOS STROBE ---
void appTorch() {
  startGame();
  uint8_t mode = 0; // 0: Still Torch, 1: Strobing, 2: Flashing, 3: SOS Beacon, 4: Night Light
  int strobeRate = 8; // Hz
  bool soundOn = false;
  uint32_t sosTimer = 0;
  int sosStep = 0;

  // International SOS: ... --- ... (1 = light, 0 = dark)
  const uint16_t sosPattern[] = {
    1, 100, 0, 100, 1, 100, 0, 100, 1, 100, 0, 300,       // S
    1, 300, 0, 100, 1, 300, 0, 100, 1, 300, 0, 300,       // O
    1, 100, 0, 100, 1, 100, 0, 100, 1, 100, 0, 700        // S
  };
  const int sosLen = sizeof(sosPattern) / (sizeof(sosPattern[0]) * 2);

  while (tick()) {
    if (pressed(K_B)) { buzz(0); return; }
    if (pressed(K_UP))   { mode = (mode + 4) % 5; buzz(0); sfx(1400, 10); }
    if (pressed(K_DOWN)) { mode = (mode + 1) % 5; buzz(0); sfx(1400, 10); }
    if (pressed(K_A))    { soundOn = !soundOn; sfx(soundOn ? 1800 : 600, 25); }

    if (mode == 1) {
      if (pressed(K_LEFT))  { if (strobeRate > 2)  strobeRate--; }
      if (pressed(K_RIGHT)) { if (strobeRate < 20) strobeRate++; }
    }

    bool lightOn = false;

    if (mode == 0) {
      // STILL TORCH
      lightOn = true;
      memset(fb, 0xFF, SCR_W * SCR_H / 8);
      if (btn & (K_UP | K_DOWN | K_LEFT | K_RIGHT | K_A)) {
        u8g2.setDrawColor(0);
        u8g2.setFont(u8g2_font_5x7_tr);
        u8g2.drawStr(2, 7, "STILL TORCH [U/D:MODE]");
        u8g2.drawStr(2, 17, soundOn ? "BUZZ: ON [A]" : "BUZZ: OFF [A]");
        u8g2.setDrawColor(1);
      }
    } else if (mode == 1) {
      // STROBING
      uint32_t period = 1000 / strobeRate;
      lightOn = ((millis() % period) < (period / 2));
      if (lightOn) memset(fb, 0xFF, SCR_W * SCR_H / 8); else memset(fb, 0x00, SCR_W * SCR_H / 8);
      u8g2.setDrawColor(lightOn ? 0 : 1);
      u8g2.setFont(u8g2_font_6x10_tr);
      u8g2.drawStr(12, 24, "STROBING");
      u8g2.setFont(u8g2_font_5x7_tr);
      char buf[32];
      snprintf(buf, sizeof(buf), "RATE: %d Hz (L/R)", strobeRate);
      u8g2.drawStr(12, 38, buf);
      snprintf(buf, sizeof(buf), "[A: BUZZ %s] [U/D:MODE]", soundOn ? "ON" : "OFF");
      u8g2.drawStr(12, 50, buf);
      u8g2.setDrawColor(1);
    } else if (mode == 2) {
      // FLASHING (Hazard beacon pulse)
      uint32_t period = 600;
      lightOn = ((millis() % period) < 200);
      if (lightOn) memset(fb, 0xFF, SCR_W * SCR_H / 8); else memset(fb, 0x00, SCR_W * SCR_H / 8);
      u8g2.setDrawColor(lightOn ? 0 : 1);
      u8g2.setFont(u8g2_font_6x10_tr);
      u8g2.drawStr(14, 24, "FLASHING BEACON");
      u8g2.setFont(u8g2_font_5x7_tr);
      char buf[32];
      snprintf(buf, sizeof(buf), "[A: BUZZ %s] [U/D:MODE]", soundOn ? "ON" : "OFF");
      u8g2.drawStr(14, 44, buf);
      u8g2.setDrawColor(1);
    } else if (mode == 3) {
      // SOS BEACON
      uint32_t cur = millis();
      if (cur >= sosTimer) {
        sosStep = (sosStep + 1) % sosLen;
        uint8_t on = sosPattern[sosStep * 2];
        uint16_t dur = sosPattern[sosStep * 2 + 1];
        sosTimer = cur + dur;
      }
      lightOn = (sosPattern[sosStep * 2] != 0);
      if (lightOn) memset(fb, 0xFF, SCR_W * SCR_H / 8); else memset(fb, 0x00, SCR_W * SCR_H / 8);
      u8g2.setDrawColor(lightOn ? 0 : 1);
      u8g2.setFont(u8g2_font_7x14_tr);
      u8g2.drawStr(24, 26, "SOS BEACON");
      u8g2.setFont(u8g2_font_5x7_tr);
      u8g2.drawStr(24, 40, "... --- ...");
      char buf[32];
      snprintf(buf, sizeof(buf), "[A: BUZZ %s] [U/D:MODE]", soundOn ? "ON" : "OFF");
      u8g2.drawStr(12, 52, buf);
      u8g2.setDrawColor(1);
    } else {
      // NIGHT LIGHT (50% Dither)
      memset(fb, 0, SCR_W * SCR_H / 8);
      for (int y = 0; y < SCR_H; y++) {
        for (int x = 0; x < SCR_W; x += 2) {
          setPx(x + (y % 2), y);
        }
      }
      u8g2.setFont(u8g2_font_6x10_tr);
      u8g2.drawStr(24, 30, "NIGHT LIGHT");
      u8g2.setFont(u8g2_font_5x7_tr);
      u8g2.drawStr(24, 44, "[U/D: CHANGE MODE]");
    }

    if (soundOn && lightOn && mode != 0 && mode != 4) {
      buzz(1900);
    } else if (soundOn && mode == 0) {
      buzz(1000);
    } else {
      buzz(0);
    }

    present();
  }
  buzz(0);
}

// --- 8. SYSTEM TELEMETRY ---
void appSysInfo() {
  startGame();
  while (tick()) {
    if (pressed(K_B)) { sfx(1200, 15); return; }

    u8g2.clearBuffer();
    u8g2.setFont(u8g2_font_5x7_tr);
    u8g2.drawStr(2, 7, "SYSTEM TELEMETRY");
    u8g2.drawStr(90, 7, "[B: EXIT]");

    char buf[32];
    snprintf(buf, sizeof(buf), "CPU Freq : %d MHz", ESP.getCpuFreqMHz());
    u8g2.drawStr(6, 18, buf);

    snprintf(buf, sizeof(buf), "Free Heap: %lu KB", (unsigned long)(ESP.getFreeHeap() / 1024));
    u8g2.drawStr(6, 27, buf);

    snprintf(buf, sizeof(buf), "Min Heap : %lu KB", (unsigned long)(ESP.getMinFreeHeap() / 1024));
    u8g2.drawStr(6, 36, buf);

    snprintf(buf, sizeof(buf), "Flash Sz : %lu MB", (unsigned long)(ESP.getFlashChipSize() / (1024 * 1024)));
    u8g2.drawStr(6, 45, buf);

    snprintf(buf, sizeof(buf), "Chip Rev : ESP32-C3 r%d", ESP.getChipRevision());
    u8g2.drawStr(6, 54, buf);

    uint32_t sec = millis() / 1000;
    snprintf(buf, sizeof(buf), "Uptime: %02lu:%02lu:%02lu", (unsigned long)(sec / 3600), (unsigned long)((sec % 3600) / 60), (unsigned long)(sec % 60));
    u8g2.drawStr(6, 63, buf);

    present();
  }
}

// --- 8B. DEDICATED CONTROLLER & BUTTON TESTER ---
void appButtonTester() {
  startGame();
  uint16_t cntU = 0, cntD = 0, cntL = 0, cntR = 0, cntA = 0, cntB = 0;
  uint32_t exitHoldStart = 0;

  while (tick()) {
    if (pressed(K_UP))    { cntU++; sfx(520, 20); }
    if (pressed(K_DOWN))  { cntD++; sfx(650, 20); }
    if (pressed(K_LEFT))  { cntL++; sfx(780, 20); }
    if (pressed(K_RIGHT)) { cntR++; sfx(910, 20); }
    if (pressed(K_A))     { cntA++; sfx(1200, 20); }
    if (pressed(K_B))     { cntB++; sfx(1500, 20); }

    // Hold UP + DOWN together for > 700ms to exit
    if (held(K_UP) && held(K_DOWN)) {
      if (exitHoldStart == 0) exitHoldStart = millis();
      else if (millis() - exitHoldStart > 700) {
        buzz(0);
        sfx(1200, 25);
        return;
      }
    } else {
      exitHoldStart = 0;
    }

    u8g2.clearBuffer();
    u8g2.setFont(u8g2_font_5x7_tr);
    u8g2.drawStr(2, 7, "BUTTON TESTER");
    u8g2.drawStr(68, 7, "[HOLD U+D: EXIT]");

    auto drawBtn = [](int x, int y, int w, int h, bool on, const char* name, uint16_t count) {
      if (on) {
        drawBoxSafe(x, y, w, h);
        u8g2.setDrawColor(0);
        u8g2.setFont(u8g2_font_5x7_tr);
        u8g2.drawStr(x + (w - u8g2.getStrWidth(name)) / 2, y + 8, name);
        char cbuf[8];
        snprintf(cbuf, sizeof(cbuf), "%u", count);
        u8g2.drawStr(x + (w - u8g2.getStrWidth(cbuf)) / 2, y + h - 2, cbuf);
        u8g2.setDrawColor(1);
      } else {
        drawFrameSafe(x, y, w, h);
        u8g2.setFont(u8g2_font_5x7_tr);
        u8g2.drawStr(x + (w - u8g2.getStrWidth(name)) / 2, y + 8, name);
        char cbuf[8];
        snprintf(cbuf, sizeof(cbuf), "%u", count);
        u8g2.drawStr(x + (w - u8g2.getStrWidth(cbuf)) / 2, y + h - 2, cbuf);
      }
    };

    // D-Pad Cross
    int cx = 35, cy = 35;
    drawBtn(cx - 8,  cy - 22, 16, 17, held(K_UP),    "UP", cntU);
    drawBtn(cx - 8,  cy + 6,  16, 17, held(K_DOWN),  "DN", cntD);
    drawBtn(cx - 24, cy - 8,  16, 17, held(K_LEFT),  "LT", cntL);
    drawBtn(cx + 8,  cy - 8,  16, 17, held(K_RIGHT), "RT", cntR);

    // Center pivot
    drawBoxSafe(cx - 5, cy - 5, 10, 10);

    // Action buttons A and B
    int ax = 85, ay = 35;
    drawBtn(ax,      ay + 2, 18, 18, held(K_A), "A", cntA);
    drawBtn(ax + 20, ay - 9, 18, 18, held(K_B), "B", cntB);

    if (exitHoldStart > 0) {
      int prog = (int)(((millis() - exitHoldStart) * 124) / 700);
      drawBoxSafe(2, 60, imin(prog, 124), 3);
    } else {
      u8g2.setFont(u8g2_font_5x7_tr);
      u8g2.drawStr(12, 61, "REALTIME DEBOUNCE COUNTERS");
    }

    present();
  }
  buzz(0);
}

// --- 9. REACTION SPEED TESTER ---
void appReaction() {
  startGame();
  enum State { IDLE, WAITING, SIGNAL, RESULT, FALSE_START };
  State st = IDLE;
  uint32_t waitTarget = 0, signalTime = 0, reactionMs = 0;
  uint32_t bestMs = prefs.getUInt("reac_best", 999);

  while (tick()) {
    if (pressed(K_B)) { buzz(0); return; }

    if (st == IDLE) {
      if (pressed(K_A)) {
        st = WAITING;
        waitTarget = now + random(1400, 3600);
        sfx(800, 20);
      }
    } else if (st == WAITING) {
      if (pressed(K_A)) {
        st = FALSE_START;
        sfx(200, 250);
      } else if (now >= waitTarget) {
        st = SIGNAL;
        signalTime = now;
        buzz(1800);
      }
    } else if (st == SIGNAL) {
      if (pressed(K_A)) {
        reactionMs = now - signalTime;
        buzz(0);
        st = RESULT;
        sfx(1400, 50);
        if (reactionMs < bestMs) {
          bestMs = reactionMs;
          prefs.putUInt("reac_best", bestMs);
        }
      }
    } else if (st == RESULT || st == FALSE_START) {
      if (pressed(K_A)) {
        st = WAITING;
        waitTarget = now + random(1400, 3600);
        sfx(800, 20);
      }
    }

    u8g2.clearBuffer();
    u8g2.setFont(u8g2_font_5x7_tr);
    u8g2.drawStr(2, 7, "REACTION TESTER");
    u8g2.drawStr(90, 7, "[B: EXIT]");

    if (st == IDLE) {
      u8g2.setFont(u8g2_font_6x10_tr);
      u8g2.drawStr(22, 28, "REFLEX TRAINER");
      u8g2.setFont(u8g2_font_5x7_tr);
      u8g2.drawStr(24, 42, "PRESS [A] TO START");
      char bbuf[24];
      if (bestMs < 999) snprintf(bbuf, sizeof(bbuf), "BEST: %lu ms", (unsigned long)bestMs);
      else snprintf(bbuf, sizeof(bbuf), "BEST: ---");
      u8g2.drawStr(36, 56, bbuf);
    } else if (st == WAITING) {
      u8g2.setFont(u8g2_font_6x10_tr);
      u8g2.drawStr(20, 28, "WAIT FOR SIGNAL...");
      int dots = (now / 300) % 4;
      for (int d = 0; d < dots; d++) u8g2.drawDisc(56 + d * 6, 42, 2);
    } else if (st == SIGNAL) {
      u8g2.drawBox(0, 10, SCR_W, 54);
      u8g2.setDrawColor(0);
      u8g2.setFont(u8g2_font_7x14_tr);
      u8g2.drawStr(20, 38, "PRESS [A] NOW!");
      u8g2.setDrawColor(1);
    } else if (st == FALSE_START) {
      u8g2.setFont(u8g2_font_7x14_tr);
      u8g2.drawStr(24, 30, "TOO EARLY!");
      u8g2.setFont(u8g2_font_5x7_tr);
      u8g2.drawStr(22, 44, "Penalized false start");
      u8g2.drawStr(26, 58, "[A: TRY AGAIN]");
    } else if (st == RESULT) {
      char rbuf[24];
      snprintf(rbuf, sizeof(rbuf), "%lu ms", (unsigned long)reactionMs);
      u8g2.setFont(u8g2_font_7x14_tr);
      u8g2.drawStr(64 - u8g2.getStrWidth(rbuf) / 2, 27, rbuf);

      u8g2.setFont(u8g2_font_5x7_tr);
      const char* rank = (reactionMs < 200) ? "RANK: PRO GAMER!" : (reactionMs < 250 ? "RANK: EXCELLENT" : (reactionMs < 320 ? "RANK: GOOD / AVERAGE" : "RANK: SLOW"));
      u8g2.drawStr(64 - u8g2.getStrWidth(rank) / 2, 40, rank);

      char bbuf[24];
      snprintf(bbuf, sizeof(bbuf), "BEST: %lu ms  [A: RETRY]", (unsigned long)bestMs);
      u8g2.drawStr(64 - u8g2.getStrWidth(bbuf) / 2, 56, bbuf);
    }
    present();
  }
  buzz(0);
}

// --- 10. UNIT CONVERTER ---
void appConverter() {
  startGame();
  int cat = 0; // 0: Length, 1: Weight, 2: Temp, 3: Speed, 4: Data
  double val = 10.0;
  bool rev = false;

  const char* const catNames[5] = {"LENGTH", "WEIGHT", "TEMP", "SPEED", "DATA"};

  while (tick()) {
    if (pressed(K_B)) return;

    if (pressed(K_RIGHT)) { cat = (cat + 1) % 5; val = 10.0; sfx(1400, 10); }
    if (pressed(K_LEFT))  { cat = (cat + 4) % 5; val = 10.0; sfx(1400, 10); }
    if (pressed(K_UP))    { val += (held(K_A) ? 10.0 : 1.0); sfx(1200, 8); }
    if (pressed(K_DOWN))  { val -= (held(K_A) ? 10.0 : 1.0); if (cat != 2 && val < 0) val = 0; sfx(1200, 8); }
    if (pressed(K_A))     { rev = !rev; sfx(1600, 15); }

    double outVal = 0.0;
    const char *uIn = "", *uOut = "";

    if (cat == 0) {
      if (!rev) { uIn = "m"; uOut = "ft"; outVal = val * 3.28084; }
      else      { uIn = "ft"; uOut = "m"; outVal = val / 3.28084; }
    } else if (cat == 1) {
      if (!rev) { uIn = "kg"; uOut = "lbs"; outVal = val * 2.20462; }
      else      { uIn = "lbs"; uOut = "kg"; outVal = val / 2.20462; }
    } else if (cat == 2) {
      if (!rev) { uIn = "'C"; uOut = "'F"; outVal = val * 1.8 + 32.0; }
      else      { uIn = "'F"; uOut = "'C"; outVal = (val - 32.0) / 1.8; }
    } else if (cat == 3) {
      if (!rev) { uIn = "km/h"; uOut = "mph"; outVal = val * 0.621371; }
      else      { uIn = "mph"; uOut = "km/h"; outVal = val / 0.621371; }
    } else {
      if (!rev) { uIn = "MB"; uOut = "GB"; outVal = val / 1024.0; }
      else      { uIn = "GB"; uOut = "MB"; outVal = val * 1024.0; }
    }

    u8g2.clearBuffer();
    u8g2.setFont(u8g2_font_5x7_tr);
    u8g2.drawStr(2, 7, "UNIT CONVERTER");
    u8g2.drawStr(90, 7, "[B: EXIT]");

    char cbuf[24];
    snprintf(cbuf, sizeof(cbuf), "< %s >", catNames[cat]);
    u8g2.drawStr(64 - u8g2.getStrWidth(cbuf) / 2, 17, cbuf);

    char inStr[24];
    snprintf(inStr, sizeof(inStr), "%.1f %s", val, uIn);
    u8g2.setFont(u8g2_font_7x14_tr);
    u8g2.drawStr(12, 33, inStr);

    u8g2.setFont(u8g2_font_5x7_tr);
    u8g2.drawStr(24, 45, "= [A: FLIP DIRECTION]");

    char outStr[24];
    snprintf(outStr, sizeof(outStr), "%.2f %s", outVal, uOut);
    u8g2.setFont(u8g2_font_7x14_tr);
    u8g2.drawStr(12, 59, outStr);

    present();
  }
}

// --- 11. I2C HARDWARE BUS SCANNER ---
void appI2CScanner() {
  startGame();
  uint8_t foundAddrs[16];
  int count = 0;

  auto scan = [&]() {
    count = 0;
    for (uint8_t a = 1; a < 127; a++) {
      Wire.beginTransmission(a);
      if (Wire.endTransmission() == 0) {
        if (count < 16) foundAddrs[count++] = a;
      }
    }
  };
  scan();

  while (tick()) {
    if (pressed(K_B)) return;
    if (pressed(K_A)) {
      sfx(1400, 20);
      scan();
    }

    u8g2.clearBuffer();
    u8g2.setFont(u8g2_font_5x7_tr);
    u8g2.drawStr(2, 7, "I2C BUS SCANNER");
    u8g2.drawStr(88, 7, "[A:SCAN]");

    char cbuf[24];
    snprintf(cbuf, sizeof(cbuf), "SDA:8 SCL:9  FOUND:%d", count);
    u8g2.drawStr(2, 17, cbuf);

    for (int row = 0; row < 8; row++) {
      for (int col = 0; col < 16; col++) {
        uint8_t addr = row * 16 + col;
        int px = 4 + col * 4, py = 22 + row * 4;
        if (addr >= 1 && addr < 127) {
          bool hit = false;
          for (int k = 0; k < count; k++) if (foundAddrs[k] == addr) { hit = true; break; }
          if (hit) u8g2.drawBox(px - 1, py - 1, 3, 3);
          else     u8g2.drawPixel(px, py);
        }
      }
    }

    u8g2.setFont(u8g2_font_5x7_tr);
    int yOff = 24;
    for (int k = 0; k < count && k < 4; k++) {
      char dbuf[32];
      const char* name = (foundAddrs[k] == 0x3C || foundAddrs[k] == 0x3D) ? "SSD1306 OLED" :
                         (foundAddrs[k] == 0x68) ? "MPU6050/RTC" :
                         (foundAddrs[k] == 0x76 || foundAddrs[k] == 0x77) ? "BMP280 SENS" : "I2C SLAVE";
      snprintf(dbuf, sizeof(dbuf), "0x%02X %s", foundAddrs[k], name);
      u8g2.drawStr(72, yOff, dbuf);
      yOff += 9;
    }
    if (count == 0) u8g2.drawStr(72, 32, "NO DEVICES");

    u8g2.drawStr(2, 62, "[B: BACK TO EXTRAS]");
    present();
  }
}

// --- 12. WIFI NETWORK SCANNER ---
void appWiFiScanner() {
  startGame();
  struct WifiAp {
    char ssid[24];
    int32_t rssi;
    uint8_t ch;
    bool enc;
  };
  const int MAX_APS = 24;
  WifiAp aps[MAX_APS];
  int count = 0;
  int sel = 0;

  auto doScan = [&]() {
    u8g2.clearBuffer();
    u8g2.setFont(u8g2_font_6x10_tr);
    u8g2.drawStr(14, 28, "SCANNING 2.4GHz...");
    u8g2.setFont(u8g2_font_5x7_tr);
    u8g2.drawStr(22, 42, "INITIALIZING RF...");
    present();

    WiFi.mode(WIFI_STA);
    WiFi.disconnect();
    delay(100);

    int found = WiFi.scanNetworks(false, true);
    count = 0;
    if (found > 0) {
      count = imin(found, MAX_APS);
      for (int i = 0; i < count; i++) {
        strncpy(aps[i].ssid, WiFi.SSID(i).c_str(), sizeof(aps[i].ssid) - 1);
        aps[i].ssid[sizeof(aps[i].ssid) - 1] = '\0';
        if (strlen(aps[i].ssid) == 0) strcpy(aps[i].ssid, "<HIDDEN SSID>");
        aps[i].rssi = WiFi.RSSI(i);
        aps[i].ch = WiFi.channel(i);
        aps[i].enc = (WiFi.encryptionType(i) != WIFI_AUTH_OPEN);
      }
    }
    WiFi.scanDelete();
    sel = 0;
  };

  doScan();

  while (tick()) {
    if (pressed(K_B)) {
      WiFi.scanDelete();
      WiFi.mode(WIFI_OFF);
      sfx(1200, 15);
      return;
    }

    if (pressed(K_A)) {
      doScan();
      startGame();
      continue;
    }

    if (count > 0) {
      if (pressed(K_DOWN)) { sel = (sel + 1) % count; sfx(1400, 8); }
      if (pressed(K_UP))   { sel = (sel + count - 1) % count; sfx(1400, 8); }
    }

    u8g2.clearBuffer();
    u8g2.setFont(u8g2_font_5x7_tr);
    char hbuf[32];
    snprintf(hbuf, sizeof(hbuf), "WIFI: %d NETS", count);
    u8g2.drawStr(2, 7, hbuf);
    u8g2.drawStr(72, 7, "[A:SCAN B:EX]");

    if (count == 0) {
      u8g2.setFont(u8g2_font_6x10_tr);
      u8g2.drawStr(18, 34, "NO NETWORKS FOUND");
      u8g2.setFont(u8g2_font_5x7_tr);
      u8g2.drawStr(28, 48, "PRESS A TO RETRY");
    } else {
      int page = sel / 4;
      int startIdx = page * 4;
      for (int i = 0; i < 4; i++) {
        int idx = startIdx + i;
        if (idx >= count) break;
        int y = 18 + i * 11;
        bool isSel = (idx == sel);

        if (isSel) {
          drawBoxSafe(0, y - 8, SCR_W, 11);
          u8g2.setDrawColor(0);
        }

        u8g2.setFont(u8g2_font_5x7_tr);
        u8g2.drawStr(3, y, aps[idx].ssid);

        if (aps[idx].enc) {
          u8g2.drawStr(80, y, "[*]");
        } else {
          u8g2.drawStr(80, y, "[O]");
        }

        char cbuf[10];
        snprintf(cbuf, sizeof(cbuf), "c%d", aps[idx].ch);
        u8g2.drawStr(98, y, cbuf);

        int bars = 1;
        if (aps[idx].rssi > -60) bars = 4;
        else if (aps[idx].rssi > -70) bars = 3;
        else if (aps[idx].rssi > -80) bars = 2;

        for (int b = 0; b < 4; b++) {
          int bh = 2 + b * 2;
          int bx = 114 + b * 3;
          if (b < bars) {
            drawBoxSafe(bx, y - bh, 2, bh);
          } else {
            drawPixelSafe(bx, y - 1);
          }
        }

        u8g2.setDrawColor(1);
      }

      u8g2.drawHLine(0, 62, SCR_W);
      if (count > 4) {
        int barW = imax(6, (SCR_W * 4) / count);
        int barX = (sel * (SCR_W - barW)) / (count - 1);
        drawBoxSafe(barX, 62, barW, 2);
      }
    }
    present();
  }

  WiFi.scanDelete();
  WiFi.mode(WIFI_OFF);
}

// --- 13. MORSE CODE LAB ---
void appMorse() {
  startGame();
  static const char* const MORSE_DICT[36] = {
    ".-", "-...", "-.-.", "-..", ".", "..-.", "--.", "....", "..", ".---",
    "-.-", ".-..", "--", "-.", "---", ".--.", "--.-", ".-.", "...", "-",
    "..-", "...-", ".--", "-..-", "-.--", "--..",
    "-----", ".----", "..---", "...--", "....-", ".....", "-....", "--...", "---..", "----."
  };
  int mode = 0;
  int dictIdx = 0;
  char liveStr[17] = "";
  int liveLen = 0;
  uint32_t aPressTime = 0;

  while (tick()) {
    if (pressed(K_B)) { buzz(0); return; }

    if (pressed(K_UP) || pressed(K_DOWN)) { mode ^= 1; sfx(1400, 10); }

    if (mode == 0) {
      if (pressed(K_RIGHT)) { dictIdx = (dictIdx + 1) % 36; sfx(1200, 8); }
      if (pressed(K_LEFT))  { dictIdx = (dictIdx + 35) % 36; sfx(1200, 8); }
      if (pressed(K_A)) {
        const char* code = MORSE_DICT[dictIdx];
        while (*code) {
          buzz(900);
          delay(*code == '.' ? 80 : 220);
          buzz(0);
          delay(80);
          code++;
        }
      }
    } else {
      if (pressed(K_A)) {
        aPressTime = now;
        buzz(900);
      }
      if (btnPrev & K_A && !(btn & K_A)) {
        buzz(0);
        uint32_t dur = now - aPressTime;
        char sym = (dur < 180) ? '.' : '-';
        if (liveLen < 16) {
          liveStr[liveLen++] = sym;
          liveStr[liveLen] = 0;
        }
      }
      if (pressed(K_RIGHT) && liveLen < 16) { liveStr[liveLen++] = ' '; liveStr[liveLen] = 0; }
      if (pressed(K_LEFT) && liveLen > 0)  { liveStr[--liveLen] = 0; }
    }

    u8g2.clearBuffer();
    u8g2.setFont(u8g2_font_5x7_tr);
    u8g2.drawStr(2, 7, "MORSE CODE LAB");
    u8g2.drawStr(74, 7, "[U/D: MODE]");

    if (mode == 0) {
      char ch = (dictIdx < 26) ? ('A' + dictIdx) : ('0' + dictIdx - 26);
      char cbuf[16];
      snprintf(cbuf, sizeof(cbuf), "CHAR: %c", ch);
      u8g2.setFont(u8g2_font_7x14_tr);
      u8g2.drawStr(16, 26, cbuf);

      char mbuf[32];
      snprintf(mbuf, sizeof(mbuf), "MORSE: %s", MORSE_DICT[dictIdx]);
      u8g2.drawStr(16, 42, mbuf);

      u8g2.setFont(u8g2_font_5x7_tr);
      u8g2.drawStr(16, 58, "[A: HEAR AUDIO]  [L/R: NEXT]");
    } else {
      u8g2.setFont(u8g2_font_5x7_tr);
      u8g2.drawStr(4, 18, "LIVE KEYER: TAP/HOLD [A]");
      u8g2.drawFrame(4, 22, 120, 24);
      u8g2.setFont(u8g2_font_6x10_tr);
      u8g2.drawStr(8, 38, liveStr);
      u8g2.setFont(u8g2_font_5x7_tr);
      u8g2.drawStr(4, 58, "[R: SPACE] [L: BKSP] [B: EXIT]");
    }
    present();
  }
  buzz(0);
}

// --- 14. CIPHER & PASSWORD GENERATOR ---
void appCipher() {
  startGame();
  int mode = 0;
  int passLen = 12;
  char pass[17] = "";

  auto genPass = [&]() {
    const char charset[] = "ABCDEFGHJKLMNPQRSTUVWXYZabcdefghijkmnopqrstuvwxyz23456789#$*!";
    for (int i = 0; i < passLen; i++) {
      pass[i] = charset[esp_random() % (sizeof(charset) - 1)];
    }
    pass[passLen] = 0;
  };
  genPass();

  int rot = 3;
  char sample[] = "NEVO CONSOLE";
  char cipher[17] = "";

  auto updateCipher = [&]() {
    for (int i = 0; sample[i]; i++) {
      if (sample[i] >= 'A' && sample[i] <= 'Z') {
        cipher[i] = 'A' + (sample[i] - 'A' + rot) % 26;
      } else {
        cipher[i] = sample[i];
      }
    }
    cipher[strlen(sample)] = 0;
  };
  updateCipher();

  while (tick()) {
    if (pressed(K_B)) return;
    if (pressed(K_UP) || pressed(K_DOWN)) { mode ^= 1; sfx(1400, 10); }

    if (mode == 0) {
      if (pressed(K_RIGHT)) { if (passLen < 16) { passLen += 4; genPass(); sfx(1200, 10); } }
      if (pressed(K_LEFT))  { if (passLen > 4)  { passLen -= 4; genPass(); sfx(1200, 10); } }
      if (pressed(K_A))     { genPass(); sfx(1600, 25); }
    } else {
      if (pressed(K_RIGHT)) { rot = (rot + 1) % 26; updateCipher(); sfx(1200, 10); }
      if (pressed(K_LEFT))  { rot = (rot + 25) % 26; updateCipher(); sfx(1200, 10); }
    }

    u8g2.clearBuffer();
    u8g2.setFont(u8g2_font_5x7_tr);
    u8g2.drawStr(2, 7, "CIPHER & SEC PASS");
    u8g2.drawStr(74, 7, "[U/D: MODE]");

    if (mode == 0) {
      char lbuf[24];
      snprintf(lbuf, sizeof(lbuf), "PASSWORD (%d CHARS):", passLen);
      u8g2.drawStr(4, 20, lbuf);
      u8g2.drawFrame(4, 24, 120, 20);
      u8g2.setFont(u8g2_font_6x10_tr);
      u8g2.drawStr(8, 38, pass);
      u8g2.setFont(u8g2_font_5x7_tr);
      u8g2.drawStr(4, 58, "[A: RE-GEN]  [L/R: LENGTH]");
    } else {
      char rbuf[24];
      snprintf(rbuf, sizeof(rbuf), "CAESAR WHEEL (ROT-%d):", rot);
      u8g2.drawStr(4, 20, rbuf);
      u8g2.setFont(u8g2_font_6x10_tr);
      char pbuf[32];
      snprintf(pbuf, sizeof(pbuf), "IN : %s", sample);
      u8g2.drawStr(4, 34, pbuf);
      char cbuf[32];
      snprintf(cbuf, sizeof(cbuf), "OUT: %s", cipher);
      u8g2.drawStr(4, 46, cbuf);
      u8g2.setFont(u8g2_font_5x7_tr);
      u8g2.drawStr(4, 59, "[L/R: SHIFT KEY] [B: EXIT]");
    }
    present();
  }
}

// --- 15. 4-CHANNEL PERSISTENT TALLY COUNTER ---
void appTally() {
  startGame();
  int ch = 0;
  int counts[4] = {
    (int)prefs.getInt("tally_0", 0),
    (int)prefs.getInt("tally_1", 0),
    (int)prefs.getInt("tally_2", 0),
    (int)prefs.getInt("tally_3", 0)
  };

  while (tick()) {
    if (pressed(K_B)) return;

    if (pressed(K_RIGHT)) { ch = (ch + 1) % 4; sfx(1400, 10); }
    if (pressed(K_LEFT))  { ch = (ch + 3) % 4; sfx(1400, 10); }

    if (pressed(K_UP)) {
      counts[ch]++;
      char k[10]; snprintf(k, sizeof(k), "tally_%d", ch);
      prefs.putInt(k, counts[ch]);
      sfx(1600, 15);
    }
    if (pressed(K_DOWN)) {
      if (counts[ch] > 0) counts[ch]--;
      char k[10]; snprintf(k, sizeof(k), "tally_%d", ch);
      prefs.putInt(k, counts[ch]);
      sfx(1000, 15);
    }
    if (pressed(K_A)) {
      counts[ch] = 0;
      char k[10]; snprintf(k, sizeof(k), "tally_%d", ch);
      prefs.putInt(k, 0);
      sfx(400, 40);
    }

    u8g2.clearBuffer();
    u8g2.setFont(u8g2_font_5x7_tr);
    u8g2.drawStr(2, 7, "4-CHANNEL TALLY");
    u8g2.drawStr(90, 7, "[B: EXIT]");

    for (int i = 0; i < 4; i++) {
      int tx = 8 + i * 28;
      if (i == ch) u8g2.drawBox(tx - 2, 11, 24, 9);
      else         u8g2.drawFrame(tx - 2, 11, 24, 9);
      u8g2.setDrawColor(i == ch ? 0 : 1);
      char tab[8]; snprintf(tab, sizeof(tab), "CH%d", i + 1);
      u8g2.drawStr(tx + 2, 18, tab);
      u8g2.setDrawColor(1);
    }

    char cbuf[16];
    snprintf(cbuf, sizeof(cbuf), "%d", counts[ch]);
    u8g2.setFont(u8g2_font_logisoso22_tr);
    int cw = u8g2.getStrWidth(cbuf);
    u8g2.drawStr(64 - cw / 2, 46, cbuf);

    int nMarks = imin(counts[ch], 20);
    for (int m = 0; m < nMarks; m++) {
      int group = m / 5;
      int idx = m % 5;
      int mx = 18 + group * 24 + idx * 4;
      if (idx < 4) u8g2.drawVLine(mx, 50, 6);
      else         u8g2.drawLine(mx - 16, 55, mx - 2, 51);
    }

    u8g2.setFont(u8g2_font_5x7_tr);
    u8g2.drawStr(2, 63, "[U:+1] [D:-1] [A:RESET] [L/R:CH]");
    present();
  }
}

// --- 16. PIXEL PAD ---
void appPixelPad() {
  startGame();
  uint8_t canvas[64 * 48 / 8] = {0};
  int curX = 32, curY = 24;
  uint8_t penMode = 1;

  while (tick()) {
    if (pressed(K_B)) return;

    if (held(K_LEFT)  && curX > 0)  curX--;
    if (held(K_RIGHT) && curX < 63) curX++;
    if (held(K_UP)    && curY > 0)  curY--;
    if (held(K_DOWN)  && curY < 47) curY++;

    if (pressed(K_A)) {
      penMode ^= 1;
      sfx(penMode ? 1600 : 800, 10);
    }

    int byteIdx = curY * 8 + (curX / 8);
    int bitIdx = curX & 7;
    if (held(K_A) || held(K_UP) || held(K_DOWN) || held(K_LEFT) || held(K_RIGHT)) {
      if (penMode) canvas[byteIdx] |= (1 << bitIdx);
      else         canvas[byteIdx] &= ~(1 << bitIdx);
    }

    u8g2.clearBuffer();
    u8g2.setFont(u8g2_font_5x7_tr);
    u8g2.drawStr(2, 7, "PIXEL PAD [64x48]");
    char pbuf[16];
    snprintf(pbuf, sizeof(pbuf), "PEN:%s", penMode ? "DRAW" : "ERASE");
    u8g2.drawStr(80, 7, pbuf);

    u8g2.drawFrame(0, 9, 66, 50);
    for (int y = 0; y < 48; y++) {
      for (int x = 0; x < 64; x++) {
        if (canvas[y * 8 + (x / 8)] & (1 << (x & 7))) {
          u8g2.drawPixel(1 + x, 10 + y);
        }
      }
    }

    if ((now / 150) & 1) {
      u8g2.setDrawColor(2);
      u8g2.drawPixel(1 + curX, 10 + curY);
      u8g2.setDrawColor(1);
    }

    u8g2.setFont(u8g2_font_5x7_tr);
    u8g2.drawStr(72, 22, "D-PAD: MOVE");
    u8g2.drawStr(72, 34, "[A: MODE]");
    u8g2.drawStr(72, 46, "[B: EXIT]");
    char cbuf[16];
    snprintf(cbuf, sizeof(cbuf), "X:%d Y:%d", curX, curY);
    u8g2.drawStr(72, 58, cbuf);

    present();
  }
}

// --- 17. BOX BREATHING & ZEN ---
void appBreathing() {
  startGame();
  int mode = 0;
  uint32_t tStart = now;

  while (tick()) {
    if (pressed(K_B)) { buzz(0); return; }
    if (pressed(K_A) || pressed(K_RIGHT) || pressed(K_LEFT)) {
      mode ^= 1;
      tStart = now;
      sfx(1200, 15);
    }

    int totalCycle = (mode == 0) ? 16 : 19;
    float elapsed = ((now - tStart) % (totalCycle * 1000)) / 1000.0f;

    const char* phaseStr = "";
    float progress = 0.0f;
    float radius = 8.0f;

    if (mode == 0) {
      if (elapsed < 4.0f) {
        phaseStr = "INHALE...";
        progress = elapsed / 4.0f;
        radius = 8.0f + progress * 16.0f;
      } else if (elapsed < 8.0f) {
        phaseStr = "HOLD BREATH";
        progress = (elapsed - 4.0f) / 4.0f;
        radius = 24.0f;
      } else if (elapsed < 12.0f) {
        phaseStr = "EXHALE...";
        progress = (elapsed - 8.0f) / 4.0f;
        radius = 24.0f - progress * 16.0f;
      } else {
        phaseStr = "HOLD (REST)";
        progress = (elapsed - 12.0f) / 4.0f;
        radius = 8.0f;
      }
    } else {
      if (elapsed < 4.0f) {
        phaseStr = "INHALE (4s)";
        progress = elapsed / 4.0f;
        radius = 8.0f + progress * 16.0f;
      } else if (elapsed < 11.0f) {
        phaseStr = "HOLD (7s)";
        progress = (elapsed - 4.0f) / 7.0f;
        radius = 24.0f;
      } else {
        phaseStr = "EXHALE (8s)";
        progress = (elapsed - 11.0f) / 8.0f;
        radius = 24.0f - progress * 16.0f;
      }
    }

    u8g2.clearBuffer();
    u8g2.setFont(u8g2_font_5x7_tr);
    u8g2.drawStr(2, 7, "BREATHING ZEN");
    u8g2.drawStr(74, 7, mode == 0 ? "[BOX 4-4]" : "[4-7-8 RELAX]");

    int cx = 36, cy = 36;
    u8g2.drawCircle(cx, cy, (int)radius);
    if (radius > 12.0f) u8g2.drawCircle(cx, cy, (int)(radius * 0.65f));

    u8g2.setFont(u8g2_font_6x10_tr);
    u8g2.drawStr(66, 30, phaseStr);

    char sbuf[16];
    snprintf(sbuf, sizeof(sbuf), "%.1f s", elapsed);
    u8g2.setFont(u8g2_font_5x7_tr);
    u8g2.drawStr(66, 42, sbuf);
    u8g2.drawStr(66, 56, "[A: TOGGLE MODE]");

    present();
  }
}

// --- 18. DIGITAL LOGIC LAB ---
void appLogicLab() {
  startGame();
  int gate = 0;
  const char* const gateNames[6] = {"AND", "OR", "XOR", "NAND", "NOR", "NOT"};
  bool inA = false, inB = false;

  while (tick()) {
    if (pressed(K_B)) return;

    if (pressed(K_RIGHT)) { gate = (gate + 1) % 6; sfx(1400, 10); }
    if (pressed(K_LEFT))  { gate = (gate + 5) % 6; sfx(1400, 10); }
    if (pressed(K_UP))    { inA = !inA; sfx(1200, 8); }
    if (pressed(K_DOWN))  { inB = !inB; sfx(1200, 8); }

    bool out = false;
    if (gate == 0) out = inA && inB;
    else if (gate == 1) out = inA || inB;
    else if (gate == 2) out = inA ^ inB;
    else if (gate == 3) out = !(inA && inB);
    else if (gate == 4) out = !(inA || inB);
    else if (gate == 5) out = !inA;

    u8g2.clearBuffer();
    u8g2.setFont(u8g2_font_5x7_tr);
    u8g2.drawStr(2, 7, "DIGITAL LOGIC LAB");
    u8g2.drawStr(90, 7, "[B: EXIT]");

    char gbuf[24];
    snprintf(gbuf, sizeof(gbuf), "< %s GATE >", gateNames[gate]);
    u8g2.drawStr(64 - u8g2.getStrWidth(gbuf) / 2, 17, gbuf);

    char abuf[16];
    snprintf(abuf, sizeof(abuf), "A: %d", inA ? 1 : 0);
    u8g2.drawStr(4, 28, abuf);
    if (gate != 5) {
      char bbuf[16];
      snprintf(bbuf, sizeof(bbuf), "B: %d", inB ? 1 : 0);
      u8g2.drawStr(4, 42, bbuf);
    }

    u8g2.drawFrame(28, 24, 24, 22);
    u8g2.setFont(u8g2_font_6x10_tr);
    u8g2.drawStr(32, 38, gateNames[gate]);

    u8g2.drawHLine(52, 35, 10);
    if (out) u8g2.drawDisc(66, 35, 4);
    else     u8g2.drawCircle(66, 35, 4);

    char obuf[16];
    snprintf(obuf, sizeof(obuf), "OUT = %d", out ? 1 : 0);
    u8g2.setFont(u8g2_font_5x7_tr);
    u8g2.drawStr(74, 38, obuf);

    u8g2.drawFrame(78, 44, 48, 18);
    u8g2.drawStr(81, 52, "TRUTH TABLE");
    char tbuf[16];
    snprintf(tbuf, sizeof(tbuf), "%d,%d => %d", inA ? 1 : 0, inB ? 1 : 0, out ? 1 : 0);
    u8g2.drawStr(81, 60, tbuf);

    u8g2.drawStr(2, 60, "[U:TGL A] [D:TGL B]");
    present();
  }
}

// --- 19. DIGITAL SPIRIT LEVEL ---
void appSpiritLevel() {
  startGame();
  float bubbleX = 0.0f, bubbleY = 0.0f;
  float offX = 0.0f, offY = 0.0f;

  while (tick()) {
    if (pressed(K_B)) { buzz(0); return; }

    if (held(K_LEFT))  bubbleX -= 35.0f * dt;
    if (held(K_RIGHT)) bubbleX += 35.0f * dt;
    if (held(K_UP))    bubbleY -= 35.0f * dt;
    if (held(K_DOWN))  bubbleY += 35.0f * dt;

    bubbleX += (0.0f - bubbleX) * 1.5f * dt;
    bubbleY += (0.0f - bubbleY) * 1.5f * dt;

    if (pressed(K_A)) {
      offX = bubbleX; offY = bubbleY;
      sfx(1600, 20);
    }

    float effX = bubbleX - offX;
    float effY = bubbleY - offY;

    u8g2.clearBuffer();
    u8g2.setFont(u8g2_font_5x7_tr);
    u8g2.drawStr(2, 7, "DIGITAL SPIRIT LEVEL");
    u8g2.drawStr(70, 7, "[A:CAL B:EX]");

    int cx = 40, cy = 36;
    drawCircleSafe(cx, cy, 22);
    drawCircleSafe(cx, cy, 7);
    drawVLineSafe(cx, cy - 25, 50);
    drawHLineSafe(cx - 25, cy, 50);

    int bx = clampf(cx + (int)effX, cx - 20, cx + 20);
    int by = clampf(cy + (int)effY, cy - 20, cy + 20);
    drawDiscSafe(bx, by, 4);
    u8g2.setDrawColor(0);
    drawDiscSafe(bx, by, 2);
    u8g2.setDrawColor(1);

    char abuf[32];
    float pitchDeg = effY * 1.5f;
    float rollDeg = effX * 1.5f;
    snprintf(abuf, sizeof(abuf), "ROLL : %+.1f*", rollDeg);
    u8g2.drawStr(72, 24, abuf);
    snprintf(abuf, sizeof(abuf), "PITCH: %+.1f*", pitchDeg);
    u8g2.drawStr(72, 36, abuf);

    bool isLevel = (fabsf(effX) < 1.2f && fabsf(effY) < 1.2f);
    if (isLevel) {
      u8g2.drawStr(72, 50, "[PERFECT]");
      buzz(1900);
    } else {
      u8g2.drawStr(72, 50, "TILTED");
      buzz(0);
    }

    drawFrameSafe(14, 59, 100, 4);
    int vialX = clampf(64 + (int)effX * 2, 16, 110);
    drawBoxSafe(vialX - 2, 59, 5, 4);

    present();
  }
  buzz(0);
}

// --- 20. RESISTOR COLOR CODE DECODER ---
void appResistorCode() {
  startGame();
  int band = 0;
  int b1 = 4;
  int b2 = 7;
  int b3 = 2;
  int b4 = 2;

  const char* const COLORS[10] = {
    "BLACK", "BROWN", "RED", "ORANGE", "YELLOW",
    "GREEN", "BLUE", "VIOLET", "GRAY", "WHITE"
  };
  const char* const MULT_NAMES[7] = {"x1", "x10", "x100", "x1k", "x10k", "x100k", "x1M"};
  const double MULT_VALS[7] = {1, 10, 100, 1000, 10000, 100000, 1000000};
  const char* const TOL_NAMES[4] = {"+/-1%", "+/-2%", "+/-5%", "+/-10%"};

  while (tick()) {
    if (pressed(K_B)) return;

    if (pressed(K_RIGHT)) { band = (band + 1) % 4; sfx(1400, 10); }
    if (pressed(K_LEFT))  { band = (band + 3) % 4; sfx(1400, 10); }

    if (pressed(K_UP)) {
      if (band == 0) b1 = (b1 + 1) % 10;
      else if (band == 1) b2 = (b2 + 1) % 10;
      else if (band == 2) b3 = (b3 + 1) % 7;
      else b4 = (b4 + 1) % 4;
      sfx(1200, 8);
    }
    if (pressed(K_DOWN)) {
      if (band == 0) b1 = (b1 + 9) % 10;
      else if (band == 1) b2 = (b2 + 9) % 10;
      else if (band == 2) b3 = (b3 + 6) % 7;
      else b4 = (b4 + 3) % 4;
      sfx(1200, 8);
    }

    double ohms = (b1 * 10 + b2) * MULT_VALS[b3];

    u8g2.clearBuffer();
    u8g2.setFont(u8g2_font_5x7_tr);
    u8g2.drawStr(2, 7, "RESISTOR COLOR CODE");
    u8g2.drawStr(90, 7, "[B: EXIT]");

    drawHLineSafe(8, 25, 24);
    drawHLineSafe(96, 25, 24);
    drawFrameSafe(32, 17, 64, 16);
    drawBoxSafe(34, 18, 4, 14);
    drawBoxSafe(90, 18, 4, 14);

    int bx[4] = {42, 54, 66, 80};
    for (int i = 0; i < 4; i++) {
      if (i == band) {
        drawTriangleSafe(bx[i] - 2, 13, bx[i] + 4, 13, bx[i] + 1, 16);
        drawTriangleSafe(bx[i] - 2, 37, bx[i] + 4, 37, bx[i] + 1, 34);
      }
      drawBoxSafe(bx[i], 17, 3, 16);
    }

    char rbuf[32];
    if (ohms >= 1000000) snprintf(rbuf, sizeof(rbuf), "%.1f M Ohm %s", ohms / 1000000.0, TOL_NAMES[b4]);
    else if (ohms >= 1000) snprintf(rbuf, sizeof(rbuf), "%.1f k Ohm %s", ohms / 1000.0, TOL_NAMES[b4]);
    else snprintf(rbuf, sizeof(rbuf), "%.0f Ohm %s", ohms, TOL_NAMES[b4]);

    u8g2.setFont(u8g2_font_7x14_tr);
    u8g2.drawStr((SCR_W - u8g2.getStrWidth(rbuf)) / 2, 50, rbuf);

    u8g2.setFont(u8g2_font_5x7_tr);
    char inf[32];
    if (band == 0) snprintf(inf, sizeof(inf), "BAND 1: %s (%d)", COLORS[b1], b1);
    else if (band == 1) snprintf(inf, sizeof(inf), "BAND 2: %s (%d)", COLORS[b2], b2);
    else if (band == 2) snprintf(inf, sizeof(inf), "MULT  : %s", MULT_NAMES[b3]);
    else snprintf(inf, sizeof(inf), "TOL   : %s", TOL_NAMES[b4]);
    u8g2.drawStr(2, 62, inf);
    u8g2.drawStr(90, 62, "[L/R/U/D]");

    present();
  }
}

// --- 21. AUDIO TONE & FREQUENCY LAB ---
void appToneGen() {
  startGame();
  uint16_t freq = 440;
  int stepIdx = 1;
  const uint16_t STEPS[4] = {1, 10, 100, 500};
  const char* const STEP_LBL[4] = {"1Hz", "10Hz", "100Hz", "500Hz"};
  int waveType = 0;
  const char* const WAVE_LBL[4] = {"SINE", "SQUARE", "TRI", "SAW"};
  bool soundOn = true;

  while (tick()) {
    if (pressed(K_B)) { buzz(0); return; }

    if (pressed(K_A)) {
      stepIdx = (stepIdx + 1) % 4;
      sfx(1400, 10);
    }

    if (pressed(K_RIGHT)) { waveType = (waveType + 1) % 4; sfx(1200, 8); }
    if (pressed(K_LEFT))  { waveType = (waveType + 3) % 4; sfx(1200, 8); }

    if (pressed(K_UP))    { if (freq + STEPS[stepIdx] <= 8000) freq += STEPS[stepIdx]; }
    if (pressed(K_DOWN))  { if (freq > STEPS[stepIdx] + 30) freq -= STEPS[stepIdx]; else freq = 30; }

    if (soundOn) buzz(freq); else buzz(0);

    u8g2.clearBuffer();
    u8g2.setFont(u8g2_font_5x7_tr);
    u8g2.drawStr(2, 7, "TONE & FREQ LAB");
    char hbuf[16];
    snprintf(hbuf, sizeof(hbuf), "[A: %s]", STEP_LBL[stepIdx]);
    u8g2.drawStr(80, 7, hbuf);

    char fbuf[24];
    snprintf(fbuf, sizeof(fbuf), "%u Hz", freq);
    u8g2.setFont(u8g2_font_7x14_tr);
    u8g2.drawStr(6, 22, fbuf);

    u8g2.setFont(u8g2_font_5x7_tr);
    char pbuf[24];
    snprintf(pbuf, sizeof(pbuf), "T: %.2f ms", 1000.0f / freq);
    u8g2.drawStr(72, 20, pbuf);
    snprintf(pbuf, sizeof(pbuf), "WAVE: %s", WAVE_LBL[waveType]);
    u8g2.drawStr(72, 28, pbuf);

    drawFrameSafe(4, 32, 120, 24);
    drawHLineSafe(4, 44, 120);

    for (int x = 5; x < 123; x++) {
      float phase = (x * 0.12f) + (millis() * 0.015f);
      int wy = 44;
      if (waveType == 0) {
        wy = 44 + (int)(sinf(phase) * 9.0f);
      } else if (waveType == 1) {
        wy = (sinf(phase) >= 0) ? 36 : 52;
      } else if (waveType == 2) {
        float t = fmodf(phase, 6.28318f) / 6.28318f;
        wy = 44 + (int)((fabsf(t - 0.5f) * 4.0f - 1.0f) * 9.0f);
      } else {
        float t = fmodf(phase, 6.28318f) / 6.28318f;
        wy = 44 + (int)((t * 2.0f - 1.0f) * 9.0f);
      }
      drawPixelSafe(x, wy);
    }

    u8g2.setFont(u8g2_font_5x7_tr);
    u8g2.drawStr(2, 63, "[U/D: FREQ]  [L/R: WAVE]  [B: EXIT]");

    present();
  }
  buzz(0);
}

// --- 22. PERIODIC TABLE EXPLORER ---
void appPeriodicTable() {
  startGame();
  int z = 1;
  struct Element {
    uint8_t z;
    const char* sym;
    const char* name;
    const char* mass;
    const char* config;
  };
  static const Element ELEMENTS[20] = {
    {1,  "H",  "HYDROGEN",  "1.008",  "1"},
    {2,  "He", "HELIUM",    "4.003",  "2"},
    {3,  "Li", "LITHIUM",   "6.941",  "2,1"},
    {4,  "Be", "BERYLLIUM", "9.012",  "2,2"},
    {5,  "B",  "BORON",     "10.81",  "2,3"},
    {6,  "C",  "CARBON",    "12.01",  "2,4"},
    {7,  "N",  "NITROGEN",  "14.01",  "2,5"},
    {8,  "O",  "OXYGEN",    "16.00",  "2,6"},
    {9,  "F",  "FLUORINE",  "19.00",  "2,7"},
    {10, "Ne", "NEON",      "20.18",  "2,8"},
    {11, "Na", "SODIUM",    "22.99",  "2,8,1"},
    {12, "Mg", "MAGNESIUM", "24.31",  "2,8,2"},
    {13, "Al", "ALUMINIUM", "26.98",  "2,8,3"},
    {14, "Si", "SILICON",   "28.09",  "2,8,4"},
    {15, "P",  "PHOSPHORUS","30.97",  "2,8,5"},
    {16, "S",  "SULFUR",    "32.07",  "2,8,6"},
    {17, "Cl", "CHLORINE",  "35.45",  "2,8,7"},
    {18, "Ar", "ARGON",     "39.95",  "2,8,8"},
    {19, "K",  "POTASSIUM", "39.10",  "2,8,8,1"},
    {20, "Ca", "CALCIUM",   "40.08",  "2,8,8,2"}
  };

  while (tick()) {
    if (pressed(K_B)) return;

    if (pressed(K_RIGHT)) { z = (z % 20) + 1; sfx(1400, 10); }
    if (pressed(K_LEFT))  { z = (z == 1 ? 20 : z - 1); sfx(1400, 10); }

    const Element &e = ELEMENTS[z - 1];

    u8g2.clearBuffer();
    u8g2.setFont(u8g2_font_5x7_tr);
    u8g2.drawStr(2, 7, "PERIODIC TABLE");
    char pbuf[16];
    snprintf(pbuf, sizeof(pbuf), "%d/20", z);
    u8g2.drawStr(96, 7, pbuf);

    drawFrameSafe(4, 12, 44, 48);
    char zbuf[8];
    snprintf(zbuf, sizeof(zbuf), "%d", e.z);
    u8g2.drawStr(7, 21, zbuf);

    u8g2.setFont(u8g2_font_7x14_tf);
    u8g2.drawStr(12, 38, e.sym);

    u8g2.setFont(u8g2_font_5x7_tr);
    u8g2.drawStr(7, 52, e.mass);

    u8g2.setFont(u8g2_font_6x10_tr);
    u8g2.drawStr(54, 23, e.name);

    u8g2.setFont(u8g2_font_5x7_tr);
    char cbuf[32];
    snprintf(cbuf, sizeof(cbuf), "SHELL: %s", e.config);
    u8g2.drawStr(54, 35, cbuf);

    int bx = 98, by = 47;
    drawDiscSafe(bx, by, 3);
    float a = millis() * 0.005f;
    drawCircleSafe(bx, by, 8);
    int ex1 = bx + (int)(cosf(a) * 8.0f);
    int ey1 = by + (int)(sinf(a) * 8.0f);
    drawPixelSafe(ex1, ey1);
    if (e.z > 2) {
      drawCircleSafe(bx, by, 14);
      int ex2 = bx + (int)(cosf(-a * 0.7f) * 14.0f);
      int ey2 = by + (int)(sinf(-a * 0.7f) * 14.0f);
      drawPixelSafe(ex2, ey2);
    }

    u8g2.drawStr(2, 63, "[L/R: NEXT ELEMENT]  [B: EXIT]");
    present();
  }
}

// --- 23. NUMBER BASE & RADIX CONVERTER ---
void appRadixConvert() {
  startGame();
  uint16_t val = 170;
  uint32_t repNext = 0;

  while (tick()) {
    if (pressed(K_B)) return;

    if (pressed(K_UP))   { val++; sfx(1200, 6); }
    if (pressed(K_DOWN)) { val--; sfx(1200, 6); }

    if ((held(K_UP) || held(K_DOWN)) && now >= repNext) {
      if (held(K_UP)) val += 5;
      if (held(K_DOWN)) val -= 5;
      repNext = now + 65;
    }

    if (pressed(K_LEFT))  { val <<= 1; sfx(1500, 8); }
    if (pressed(K_RIGHT)) { val >>= 1; sfx(1500, 8); }
    if (pressed(K_A))     { val = (uint16_t)(~val); sfx(1800, 15); }

    u8g2.clearBuffer();
    u8g2.setFont(u8g2_font_5x7_tr);
    u8g2.drawStr(2, 7, "BASE / RADIX CONVERTER");
    u8g2.drawStr(90, 7, "[B: EXIT]");

    char dbuf[32];
    snprintf(dbuf, sizeof(dbuf), "DEC: %u", val);
    u8g2.setFont(u8g2_font_6x10_tr);
    u8g2.drawStr(6, 20, dbuf);

    char hbuf[32];
    snprintf(hbuf, sizeof(hbuf), "HEX: 0x%04X", val);
    u8g2.drawStr(72, 20, hbuf);

    char cbuf[16];
    char ch = (val >= 32 && val <= 126) ? (char)val : '.';
    snprintf(cbuf, sizeof(cbuf), "ASC: '%c'", ch);
    u8g2.drawStr(6, 32, cbuf);

    char bbuf[24];
    for (int i = 15; i >= 0; i--) {
      bbuf[15 - i] = ((val >> i) & 1) ? '1' : '0';
    }
    bbuf[16] = '\0';
    u8g2.setFont(u8g2_font_5x7_tr);
    u8g2.drawStr(6, 44, "BIN: ");
    u8g2.drawStr(28, 44, bbuf);

    int rx0 = 6;
    for (int i = 15; i >= 0; i--) {
      int x = rx0 + (15 - i) * 7 + (i < 8 ? 4 : 0);
      bool on = (val >> i) & 1;
      if (on) drawBoxSafe(x, 48, 6, 6);
      else    drawFrameSafe(x, 48, 6, 6);
    }

    u8g2.drawStr(2, 63, "[U/D:+-] [L/R:<<>>] [A:~NOT]");
    present();
  }
}

// --- EXTRAS HUB MENU ---
void gameExtras() {
  startGame();
  int sel = 0;
  const int NEXTRAS = 24;
  const char* const extraNames[NEXTRAS] = {
    "CALCULATOR",
    "BUZZER PIANO",
    "CHRONO/TIMER",
    "METRONOME",
    "DICE & RNG",
    "SOUNDBOARD",
    "TORCH & SOS",
    "SYSTEM INFO",
    "BUTTON TEST",
    "WIFI SCANNER",
    "REACTION TEST",
    "UNIT CONVERT",
    "I2C SCANNER",
    "MORSE CODE",
    "CIPHER & PASS",
    "TALLY COUNTER",
    "PIXEL PAD",
    "BREATHING ZEN",
    "LOGIC LAB",
    "SPIRIT LEVEL",
    "RESISTOR CODE",
    "TONE LAB",
    "PERIODIC TABLE",
    "BASE CONVERT"
  };

  while (tick()) {
    if (pressed(K_RIGHT)) { sel = (sel + 1) % NEXTRAS; sfx(1400, 10); }
    if (pressed(K_LEFT))  { sel = (sel + NEXTRAS - 1) % NEXTRAS; sfx(1400, 10); }
    if (pressed(K_DOWN))  {
      if (sel + 4 < NEXTRAS) sel += 4;
      else sel = sel % 4;
      sfx(1400, 10);
    }
    if (pressed(K_UP))    {
      if (sel >= 4) sel -= 4;
      else {
        int col = sel % 4;
        sel = col + ((NEXTRAS - 1 - col) / 4) * 4;
      }
      sfx(1400, 10);
    }
    if (pressed(K_B))     { sfx(1200, 15); return; }

    if (pressed(K_A)) {
      sfx(1800, 30);
      switch (sel) {
        case 0:  appCalculator();   break;
        case 1:  appPiano();        break;
        case 2:  appChrono();       break;
        case 3:  appMetronome();    break;
        case 4:  appDiceRNG();      break;
        case 5:  appSoundboard();   break;
        case 6:  appTorch();        break;
        case 7:  appSysInfo();      break;
        case 8:  appButtonTester(); break;
        case 9:  appWiFiScanner();  break;
        case 10: appReaction();     break;
        case 11: appConverter();    break;
        case 12: appI2CScanner();   break;
        case 13: appMorse();        break;
        case 14: appCipher();       break;
        case 15: appTally();        break;
        case 16: appPixelPad();     break;
        case 17: appBreathing();    break;
        case 18: appLogicLab();     break;
        case 19: appSpiritLevel();  break;
        case 20: appResistorCode(); break;
        case 21: appToneGen();      break;
        case 22: appPeriodicTable();break;
        case 23: appRadixConvert(); break;
      }
      startGame();
    }

    int page = sel / 8;
    int pageStart = page * 8;
    int totalPages = (NEXTRAS + 7) / 8;

    u8g2.clearBuffer();
    u8g2.setFont(u8g2_font_5x7_tr);
    u8g2.drawStr(2, 7, "EXTRAS & UTILITIES");

    char pbuf[16];
    snprintf(pbuf, sizeof(pbuf), "%d/%d", page + 1, totalPages);
    u8g2.drawStr(SCR_W / 2 - u8g2.getStrWidth(pbuf) / 2, 7, pbuf);

    const char* backStr = "[B: BACK]";
    u8g2.drawStr(SCR_W - u8g2.getStrWidth(backStr) - 2, 7, backStr);

    for (int i = 0; i < 8; i++) {
      int idx = pageStart + i;
      if (idx >= NEXTRAS) break;
      int cx = 2 + (i % 4) * 31;
      int cy = 10 + (i / 4) * 23;
      if (idx == sel) {
        u8g2.setDrawColor(1);
        u8g2.drawRBox(cx, cy, 30, 22, 3);
        drawExtraIcon(idx, cx + 5, cy + 1, 0);
      } else {
        drawExtraIcon(idx, cx + 5, cy + 1, 1);
      }
      u8g2.setDrawColor(1);
    }

    u8g2.setFont(u8g2_font_6x10_tr);
    u8g2.drawStr(2, 63, extraNames[sel]);
    u8g2.setFont(u8g2_font_5x7_tr);
    const char* hint = "[A: OPEN]";
    u8g2.drawStr(SCR_W - u8g2.getStrWidth(hint) - 2, 63, hint);

    present();
  }
}

// ================= MENU =================
struct Game { const char* name; void (*run)(); const char* key; uint8_t kind; }; // kind: 0 none, 1 time, 2 score

const Game GAMES[NGAMES] = {
  {"DOOM-LITE", gameDoom,      "doom",   1},
  {"3D MAZE",   gameMaze,      "maze",   1},
  {"RACER",     gameRacer,     "racer",  2},
  {"SNAKE",     gameSnake,     "snake",  2},
  {"PONG",      gamePong,      nullptr,  0},
  {"BREAKOUT",  gameBreakout,  "brk",    2},
  {"SKY RAID",  gameFlight,    "flight", 2},
  {"TETRA-DROP",gameTetra,     "tetra",  2},
  {"LUNAR ESC", gameLunar,     "lunar",  2},
  {"VOID STRK", gameVoid,      "void",   2},
  {"SOKOBAN",   gameSokoban,   "soko",   2},
  {"MICRO BEAT",gameMicroBeat, "mbeat",  2},
  {"ARTILLERY", gameArtillery, "arty",   2},
  {"SUB-HUNTER",gameSubHunter, "sub",    2},
  {"CRYPT ROG", gameCryptRogue,"crogue", 2},
  {"PIXEL PLAT",gamePlatformer,"plat",   2},
  {"INVADERS",  gameInvaders,  "invad",  2},
  {"PAC-CHASE", gamePacChase,  "pac",    2},
  {"BARREL KONG",gameKong,     "kong",   2},
  {"FLAPPY C3", gameFlapper,   "flap",   2},
  {"ASTEROIDS", gameAsteroids, "ast",    2},
  {"EXTRAS",    gameExtras,    nullptr,  0},
};

uint32_t hiCache[NGAMES];
int sel = 0;

void loadHi() {
  for (int i = 0; i < NGAMES; i++) hiCache[i] = GAMES[i].key ? hiGet(GAMES[i].key) : 0;
}

// 20x20 icons drawn with primitives (f = foreground, g = contrast colour)
void drawIcon(int i, int x, int y, uint8_t f) {
  uint8_t g = f ^ 1;
  u8g2.setDrawColor(f);
  switch (i) {
    case 0:  // skull
      u8g2.drawRBox(x + 3, y + 1, 14, 12, 3);
      u8g2.drawBox(x + 6, y + 13, 8, 5);
      u8g2.setDrawColor(g);
      u8g2.drawBox(x + 6, y + 6, 3, 3); u8g2.drawBox(x + 11, y + 6, 3, 3);
      u8g2.drawBox(x + 9, y + 10, 2, 2);
      u8g2.drawVLine(x + 8, y + 14, 4); u8g2.drawVLine(x + 10, y + 14, 4); u8g2.drawVLine(x + 12, y + 14, 4);
      break;
    case 1:  // maze
      u8g2.drawFrame(x + 1, y + 1, 18, 18);
      u8g2.drawHLine(x + 1, y + 6, 12);
      u8g2.drawVLine(x + 13, y + 6, 7);
      u8g2.drawHLine(x + 6, y + 12, 8);
      u8g2.drawVLine(x + 6, y + 12, 5);
      u8g2.drawHLine(x + 9, y + 16, 10);
      u8g2.drawBox(x + 15, y + 8, 2, 2);
      break;
    case 2:  // road
      u8g2.drawHLine(x, y + 3, 20);
      u8g2.drawLine(x + 8, y + 3, x, y + 19);
      u8g2.drawLine(x + 11, y + 3, x + 19, y + 19);
      u8g2.drawBox(x + 9, y + 5, 2, 2); u8g2.drawBox(x + 9, y + 9, 2, 3); u8g2.drawBox(x + 9, y + 15, 2, 4);
      break;
    case 3:  // snake
      for (int k = 0; k < 5; k++) u8g2.drawBox(x + 2 + k * 3, y + 14, 3, 3);
      u8g2.drawBox(x + 14, y + 11, 3, 3);
      u8g2.drawBox(x + 14, y + 8, 3, 3);
      u8g2.drawBox(x + 11, y + 8, 3, 3);
      u8g2.drawBox(x + 6, y + 7, 5, 5);
      u8g2.setDrawColor(g); u8g2.drawPixel(x + 9, y + 9); u8g2.setDrawColor(f);
      u8g2.drawFrame(x + 3, y + 2, 4, 4);
      break;
    case 4:  // pong
      u8g2.drawBox(x + 1, y + 4, 3, 9);
      u8g2.drawBox(x + 16, y + 7, 3, 9);
      u8g2.drawBox(x + 9, y + 9, 3, 3);
      for (int k = 0; k < 5; k++) u8g2.drawVLine(x + 10, y + 1 + k * 4, 2);
      break;
    case 5:  // breakout
      for (int r = 0; r < 3; r++) for (int c = 0; c < 4; c++) u8g2.drawBox(x + 1 + c * 5, y + 2 + r * 3, 4, 2);
      u8g2.drawBox(x + 9, y + 12, 2, 2);
      u8g2.drawBox(x + 5, y + 17, 10, 2);
      break;
    case 6:  // plane over a grid
      u8g2.drawHLine(x, y + 9, 20);
      u8g2.drawLine(x + 10, y + 9, x + 1, y + 19);
      u8g2.drawLine(x + 10, y + 9, x + 6, y + 19);
      u8g2.drawVLine(x + 10, y + 9, 11);
      u8g2.drawLine(x + 10, y + 9, x + 14, y + 19);
      u8g2.drawLine(x + 10, y + 9, x + 19, y + 19);
      u8g2.drawHLine(x + 3, y + 13, 14);
      u8g2.drawTriangle(x + 2, y + 7, x + 18, y + 7, x + 10, y + 2);
      u8g2.setDrawColor(g); u8g2.drawVLine(x + 10, y + 3, 5); u8g2.setDrawColor(f);
      u8g2.drawVLine(x + 10, y, 3);
      break;
    case 7:  // tetra-drop
      u8g2.drawBox(x + 4, y + 4, 12, 4);
      u8g2.drawBox(x + 8, y + 8, 4, 4);
      u8g2.drawFrame(x + 13, y + 13, 4, 4);
      break;
    case 8:  // lunar escape
      u8g2.drawBox(x + 6, y + 5, 8, 6);
      u8g2.drawLine(x + 6, y + 11, x + 3, y + 16);
      u8g2.drawLine(x + 13, y + 11, x + 16, y + 16);
      u8g2.drawHLine(x + 1, y + 17, 18);
      break;
    case 9:  // void striker
      u8g2.drawTriangle(x + 3, y + 16, x + 17, y + 16, x + 10, y + 4);
      u8g2.drawVLine(x + 10, y + 2, 3);
      u8g2.setDrawColor(g);
      u8g2.drawPixel(x + 10, y + 10);
      u8g2.setDrawColor(f);
      break;
    case 10: // sokoban
      u8g2.drawFrame(x + 2, y + 5, 10, 10);
      u8g2.drawLine(x + 2, y + 5, x + 11, y + 14);
      u8g2.drawLine(x + 11, y + 5, x + 2, y + 14);
      u8g2.drawDisc(x + 16, y + 8, 2, U8G2_DRAW_ALL);
      u8g2.drawVLine(x + 16, y + 10, 5);
      break;
    case 11: // micro beat
      drawDiscSafe(x + 5, y + 14, 3);
      drawDiscSafe(x + 13, y + 11, 3);
      drawVLineSafe(x + 7, y + 4, 10);
      drawVLineSafe(x + 15, y + 1, 10);
      drawBoxSafe(x + 7, y + 2, 9, 3);
      drawVLineSafe(x + 2, y + 7, 6);
      drawVLineSafe(x + 18, y + 5, 8);
      break;
    case 12: // artillery
      drawLineSafe(x + 1, y + 18, x + 19, y + 12);
      drawBoxSafe(x + 5, y + 10, 8, 4);
      drawBoxSafe(x + 7, y + 8, 4, 3);
      drawLineSafe(x + 9, y + 8, x + 15, y + 4);
      drawPixelSafe(x + 17, y + 2);
      break;
    case 13: // sub-hunter
      drawBoxSafe(x + 2, y + 9, 14, 6);
      drawBoxSafe(x + 7, y + 6, 4, 3);
      drawVLineSafe(x + 8, y + 3, 3);
      drawPixelSafe(x + 9, y + 3);
      drawVLineSafe(x + 1, y + 10, 4);
      drawCircleSafe(x + 14, y + 12, 4);
      break;
    case 14: // crypt rogue
      drawVLineSafe(x + 9, y + 2, 11);
      drawVLineSafe(x + 10, y + 2, 11);
      drawPixelSafe(x + 9, y + 1); drawPixelSafe(x + 10, y + 1);
      drawHLineSafe(x + 5, y + 13, 10);
      drawVLineSafe(x + 9, y + 14, 3); drawVLineSafe(x + 10, y + 14, 3);
      drawDiscSafe(x + 9, y + 18, 1);
      drawFrameSafe(x + 2, y + 4, 6, 10);
      break;
    case 15: // pixel platformer
      drawDiscSafe(x + 9, y + 5, 2);
      drawVLineSafe(x + 9, y + 8, 5);
      drawLineSafe(x + 9, y + 13, x + 6, y + 17);
      drawLineSafe(x + 9, y + 13, x + 13, y + 16);
      drawLineSafe(x + 9, y + 9, x + 3, y + 11);
      drawLineSafe(x + 9, y + 10, x + 2, y + 14);
      drawPixelSafe(x + 4, y + 7); drawPixelSafe(x + 2, y + 10);
      break;
    case 16: // invaders (alien crab)
      drawBoxSafe(x + 5, y + 5, 10, 8);
      drawBoxSafe(x + 3, y + 7, 14, 4);
      u8g2.setDrawColor(g);
      drawPixelSafe(x + 7, y + 7); drawPixelSafe(x + 12, y + 7);
      u8g2.setDrawColor(f);
      drawVLineSafe(x + 4, y + 13, 4); drawVLineSafe(x + 15, y + 13, 4);
      drawVLineSafe(x + 7, y + 13, 3); drawVLineSafe(x + 12, y + 13, 3);
      break;
    case 17: // pac-chase (pacman + pellet)
      drawDiscSafe(x + 8, y + 10, 7);
      u8g2.setDrawColor(g);
      drawTriangleSafe(x + 8, y + 10, x + 16, y + 5, x + 16, y + 15);
      u8g2.setDrawColor(f);
      drawDiscSafe(x + 17, y + 10, 2);
      break;
    case 18: // barrel kong (rolling barrel)
      drawFrameSafe(x + 3, y + 4, 14, 12);
      drawBoxSafe(x + 5, y + 6, 10, 8);
      drawHLineSafe(x + 1, y + 8, 18);
      drawHLineSafe(x + 1, y + 12, 18);
      break;
    case 19: // flappy c3 (bird)
      drawDiscSafe(x + 8, y + 10, 6);
      drawTriangleSafe(x + 12, y + 8, x + 12, y + 12, x + 17, y + 10);
      drawLineSafe(x + 4, y + 10, x + 8, y + 7);
      u8g2.setDrawColor(g);
      drawPixelSafe(x + 10, y + 8);
      u8g2.setDrawColor(f);
      break;
    case 20: // asteroids (vector ship + tumbling rock)
      drawLineSafe(x + 10, y + 2, x + 4, y + 16);
      drawLineSafe(x + 10, y + 2, x + 16, y + 16);
      drawLineSafe(x + 4, y + 16, x + 10, y + 13);
      drawLineSafe(x + 16, y + 16, x + 10, y + 13);
      drawCircleSafe(x + 16, y + 5, 3);
      drawCircleSafe(x + 4, y + 4, 2);
      break;
    case 21: // extras / toolbox (pinned at end!)
      u8g2.drawFrame(x + 2, y + 5, 16, 12);
      u8g2.drawHLine(x + 2, y + 8, 16);
      u8g2.drawFrame(x + 7, y + 2, 6, 4);
      u8g2.setDrawColor(g);
      u8g2.drawHLine(x + 8, y + 5, 4);
      u8g2.setDrawColor(f);
      u8g2.drawBox(x + 8, y + 7, 4, 3);
      u8g2.drawHLine(x + 6, y + 13, 8);
      u8g2.drawVLine(x + 9, y + 11, 5);
      u8g2.drawVLine(x + 10, y + 11, 5);
      break;
  }
  u8g2.setDrawColor(1);
}

int runMenu() {
  loadHi();
  startGame();
  while (true) {
    tick();
    if (pressed(K_RIGHT)) { sel = (sel + 1) % NGAMES; sfx(1400, 12); }
    if (pressed(K_LEFT))  { sel = (sel + NGAMES - 1) % NGAMES; sfx(1400, 12); }
    if (pressed(K_DOWN)) {
      if (sel + 4 < NGAMES) sel += 4;
      else sel = sel % 4;
      sfx(1400, 12);
    }
    if (pressed(K_UP)) {
      if (sel >= 4) sel -= 4;
      else {
        int col = sel % 4;
        sel = col + ((NGAMES - 1 - col) / 4) * 4;
      }
      sfx(1400, 12);
    }
    if (pressed(K_A)) { sfx(1800, 40); return sel; }

    int page = sel / 8;
    int pageStart = page * 8;
    int totalPages = (NGAMES + 7) / 8;

    u8g2.clearBuffer();
    u8g2.setFont(u8g2_font_5x7_tr);
    u8g2.drawStr(2, 7, "C3 ARCADE");

    char buf[24];
    if (totalPages > 1) {
      snprintf(buf, sizeof(buf), "%d/%d", page + 1, totalPages);
      u8g2.drawStr(SCR_W / 2 - u8g2.getStrWidth(buf) / 2, 7, buf);
    }
#if SHOW_FPS
    snprintf(buf, sizeof(buf), "%d fps", fps);
    u8g2.drawStr(SCR_W - u8g2.getStrWidth(buf) - 2, 7, buf);
#endif

    for (int i = 0; i < 8; i++) {
      int gIdx = pageStart + i;
      if (gIdx >= NGAMES) break;
      int cx = 2 + (i % 4) * 31, cy = 10 + (i / 4) * 23;
      if (gIdx == sel) {
        u8g2.setDrawColor(1);
        u8g2.drawRBox(cx, cy, 30, 22, 3);
        drawIcon(gIdx, cx + 5, cy + 1, 0);
      } else {
        drawIcon(gIdx, cx + 5, cy + 1, 1);
      }
      u8g2.setDrawColor(1);
    }

    u8g2.setFont(u8g2_font_6x10_tr);
    u8g2.drawStr(2, 63, GAMES[sel].name);
    uint32_t v = hiCache[sel];
    buf[0] = 0;
    if (v && GAMES[sel].kind == 1) snprintf(buf, sizeof(buf), "BEST %lu.%lus", (unsigned long)(v / 10), (unsigned long)(v % 10));
    else if (v && GAMES[sel].kind == 2) snprintf(buf, sizeof(buf), "BEST %lu", (unsigned long)v);
    if (buf[0]) u8g2.drawStr(SCR_W - u8g2.getStrWidth(buf) - 2, 63, buf);
    present();
  }
}

// ================= MINIMAL BOOT ANIMATION =================
void playBootAnimation() {
  const char* text = BOOT_TEXT;
  int textLen = strlen(text);

  readButtons();
  uint8_t initialBtn = btn;
  auto skipRequested = [&]() -> bool {
    readButtons();
    return (btn != 0 && btn != initialBtn);
  };

  u8g2.setFont(u8g2_font_logisoso16_tr);
  int totalW = u8g2.getStrWidth(text);
  int startX = (SCR_W - totalW) / 2;
  int textY = 36;

  char typed[24] = {0};

  // Phase 1: Clean typing in giant font with sleek cursor bar & soft micro-clicks
  for (int i = 0; i <= textLen; i++) {
    if (skipRequested()) { buzz(0); return; }

    for (int k = 0; k < i; k++) typed[k] = text[k];
    typed[i] = '\0';

    u8g2.clearBuffer();
    u8g2.setFont(u8g2_font_logisoso16_tr);
    u8g2.drawStr(startX, textY, typed);

    if (i < textLen) {
      int curX = startX + u8g2.getStrWidth(typed);
      u8g2.drawBox(curX + 2, 20, 2, 16);
    }
    u8g2.sendBuffer();

    if (i > 0) {
      buzz(2400);
      delay(3);
      buzz(0);
    }

    uint32_t tStart = millis();
    while (millis() - tStart < 65) {
      if (skipRequested()) { buzz(0); return; }
      delay(4);
    }
  }

  // Phase 2: Elegant hairline underline expands from center
  const int targetLineW = totalW + 12;
  const int lineY = 41;
  const int steps = 14;

  for (int s = 1; s <= steps; s++) {
    if (skipRequested()) { buzz(0); return; }

    int currentW = (s * targetLineW) / steps;
    int lx = 64 - currentW / 2;

    u8g2.clearBuffer();
    u8g2.setFont(u8g2_font_logisoso16_tr);
    u8g2.drawStr(startX, textY, typed);
    u8g2.drawHLine(lx, lineY, currentW);
    u8g2.sendBuffer();

    delay(14);
  }

  // Phase 3: Minimal, crystal-clear signature chime & subtitle reveal
  u8g2.clearBuffer();
  u8g2.setFont(u8g2_font_logisoso16_tr);
  u8g2.drawStr(startX, textY, typed);
  u8g2.drawHLine(64 - targetLineW / 2, lineY, targetLineW);

  u8g2.setFont(u8g2_font_5x7_tr);
  const char* sub = "C3 ARCADE";
  int subW = u8g2.getStrWidth(sub);
  u8g2.drawStr((SCR_W - subW) / 2, 54, sub);
  u8g2.sendBuffer();

  if (!skipRequested()) {
    buzz(1046); // C6
    delay(40);
    buzz(0);
    delay(20);
    buzz(1568); // G6
    delay(90);
    buzz(0);
  }

  // Phase 4: Gentle hold before loading menu
  uint32_t holdStart = millis();
  while (millis() - holdStart < 550) {
    if (skipRequested()) break;
    delay(10);
  }

  buzz(0);
  u8g2.setDrawColor(1);
}

// ================= SETUP / LOOP =================
void setup() {
  Serial.begin(115200);
  for (int i = 0; i < 6; i++) pinMode(pins[i], INPUT_PULLUP);
  Wire.begin(OLED_SDA, OLED_SCL);
  u8g2.setBusClock(I2C_SPEED);
  u8g2.begin();
  fb = u8g2.getBufferPtr();
  buzzInit();
  buildTables();
  playBootAnimation();
  now = lastMs = millis();
}

void loop() {
  int g = runMenu();
  GAMES[g].run();
  buzz(0);
}