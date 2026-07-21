/*
 * SleepingCat — a RoboEyes-style animation engine for the sleeping cat scene.
 *
 * Design mirrors FluxGarage RoboEyes:
 *   - Template class bound to an Adafruit display object
 *   - begin(width, height, fps) + update() public interface
 *   - Internal frame-rate throttling (fpsTimer / frameInterval)
 *   - Exponential easing: renderX = (renderX + walkX) / 2
 *   - All animation is time-based (millis) or phase-based (continuous float)
 *
 * The caller provides a background-drawing callback (e.g. drawSky) so that
 * weather/moon/sky rendering stays decoupled from the cat animation logic.
 */

#ifndef _SLEEPING_CAT_H
#define _SLEEPING_CAT_H

#include <Arduino.h>

template<typename AdafruitDisplay>
class SleepingCat
{
public:
  AdafruitDisplay *display;

  int screenWidth  = 128;
  int screenHeight = 64;
  int frameInterval = 100;          // 10 fps default (1000/10)
  unsigned long fpsTimer = 0;

  // Background callback — called after clearDisplay, before the cat.
  // Use this for sky/weather/moon rendering (main.cpp owns that logic).
  void (*drawBackground)(void) = nullptr;

  // Overlay switches (clock + uptime info, top corners)
  bool overlayTime    = true;
  bool overlayWeekday = true;
  bool overlayUptime  = true;

  // ----------------------------------------------------------------- ctor
  SleepingCat(AdafruitDisplay &disp) : display(&disp) {}

  // ----------------------------------------------------------------- begin
  void begin(int width, int height, byte fps) {
    screenWidth  = width;
    screenHeight = height;
    frameInterval = 1000 / fps;
    stateTimer = millis();
  }

  void setFramerate(byte fps) { frameInterval = 1000 / fps; }

  void setBackgroundCallback(void (*cb)(void)) { drawBackground = cb; }

  // 坏天气/夜晚时猫躲进房子里睡觉，不出来活动
  // main.cpp 根据天气+时间调用
  void setHideInHouse(bool h) { hideInHouse = h; }

  void setOverlay(bool t, bool w, bool u) {
    overlayTime = t;
    overlayWeekday = w;
    overlayUptime = u;
  }

  // Cache the time/uptime strings.  Call this whenever they change
  // (e.g. once per second from main.cpp).  The class uses the cached
  // values at render time, so there's no need to call every frame.
  void setTimeInfo(const char* t, const char* w, const char* u) {
    if (t) { strncpy(ovTimeStr,    t, sizeof(ovTimeStr)-1);    ovTimeStr[sizeof(ovTimeStr)-1]    = 0; }
    if (w) { strncpy(ovWeekdayStr, w, sizeof(ovWeekdayStr)-1); ovWeekdayStr[sizeof(ovWeekdayStr)-1] = 0; }
    if (u) { strncpy(ovUptimeStr,  u, sizeof(ovUptimeStr)-1);  ovUptimeStr[sizeof(ovUptimeStr)-1]  = 0; }
  }

  // ----------------------------------------------------------------- update
  void update() {
    if (millis() - fpsTimer >= frameInterval) {
      fpsTimer = millis();
      tick();
      drawScene();
      display->display();
    }
  }

private:
  // SSD1306 color constants (0 = black, 1 = white)
  static const uint16_t C_BLACK = 0;
  static const uint16_t C_WHITE = 1;

  // ---- State machine ----
  // 0 = deep sleep, 1 = tail wag, 2 = walking, 3 = heart
  // Cycle: sleep 30s (25s deep + 5s wag) + awake 10s (7s walk + 3s heart)
  int catState = 0;
  unsigned long stateTimer = 0;
  unsigned long stateDuration = 25000;

  // ---- 坏天气/夜晚：猫躲房子里睡觉 ----
  bool hideInHouse = false;

  // ---- Position (tweened: renderX eases toward walkX) ----
  float walkX   = 58.0f;
  float renderX = 58.0f;
  int   walkDir = 1;                // 1 = facing right, -1 = facing left

  // ---- Tail: continuous phase for smooth wag ----
  float tailPhase = 0.0f;

  // ---- Legs: continuous phase for walk cycle ----
  float legPhase = 0.0f;

  // ---- Cached overlay strings ----
  char ovTimeStr[8]    = "--:--";
  char ovWeekdayStr[4] = "-";
  char ovUptimeStr[8]  = "";

  // ---- helper ----
  static float lerpf(float a, float b, float t) { return a + (b - a) * t; }

  // =====================================================================
  //  tick — advance state machine + animation phases
  // =====================================================================
  void tick() {
    // 坏天气/夜晚：猫在房子里睡觉，不跑状态机
    if (hideInHouse) {
      // 尾巴慢摆（在房子里睡觉）
      tailPhase += 1.0f * frameInterval / 1000.0f;
      return;
    }

    // ---- State transitions (millis-based, frame-rate independent) ----
    // Deterministic cycle: sleep 30s (25s deep + 5s wag) + awake 10s (7s walk + 3s heart)
    if (millis() - stateTimer > stateDuration) {
      stateTimer = millis();
      switch (catState) {
        case 0:  catState = 1; stateDuration = 5000;  break;            // deep sleep -> tail wag
        case 1:  catState = 2; stateDuration = 7000;  walkDir = random(2) ? 1 : -1; break;  // wag -> walking
        case 2:  catState = 3; stateDuration = 3000;  break;            // walking -> heart
        case 3:  catState = 0; stateDuration = 25000; break;            // heart -> deep sleep
      }
    }

    // ---- Tail wag speed by state ----
    float tailSpeed;
    if      (catState == 0) tailSpeed = 1.5f;   // slow wag while sleeping
    else if (catState == 1) tailSpeed = 4.0f;   // fast wag
    else                    tailSpeed = 2.0f;   // gentle wag while walking / heart
    tailPhase += tailSpeed * frameInterval / 1000.0f;

    // ---- Walking: continuous smooth movement ----
    if (catState == 2) {
      walkX += walkDir * 0.025f * frameInterval;   // ~25 px/s
      if (walkX > 90) { walkX = 90; walkDir = -1; }
      if (walkX < 30) { walkX = 30; walkDir =  1; }
      if (random(16) == 0) walkDir = -walkDir;
      legPhase += 4.0f * frameInterval / 1000.0f;  // walk-cycle speed
    }

    // ---- Exponential easing (RoboEyes trick) ----
    // renderX chases walkX, giving natural acceleration / deceleration.
    renderX = (renderX + walkX) / 2.0f;
  }

  // =====================================================================
  //  drawScene — clear, background, house, cat, overlay
  // =====================================================================
  void drawScene() {
    display->clearDisplay();
    if (drawBackground) drawBackground();

    // 始终画右下角的小房子
    drawHouse();

    if (hideInHouse) {
      // 坏天气/夜晚：猫在房子里睡觉，只画 zzz 从房子飘出
      drawHouseZzz();
    } else {
      // 天气好白天：猫出来活动
      bool awake = (catState == 2 || catState == 3);
      int bx = (int)renderX;
      int by = 48;

      // Breathing: body Y oscillates ±1 px while sleeping / tail-wagging
      if (catState == 0 || catState == 1) {
        float s = sinf(millis() * 0.002f);
        by += (s > 0.5f) ? 1 : (s < -0.5f) ? -1 : 0;
      }

      drawBody(awake, bx, by);
      drawTail(bx, by);
      if (awake) drawLegs(bx, by);
      if (catState == 0 || catState == 1) drawZzz(bx);
      if (catState == 3) drawHeart(bx);
    }

    drawOverlay();
  }

  // =====================================================================
  //  drawHouse — 右下角小房子（始终画）
  //  位置：x=104-120, y=40-56（16 宽 16 高）
  // =====================================================================
  void drawHouse() {
    int hx = 104, hy = 56;   // 房子左下角
    // 屋顶（三角形）
    display->fillTriangle(hx, hy - 12, hx + 8, hy - 18, hx + 16, hy - 12, C_WHITE);
    // 墙（矩形）
    display->fillRect(hx, hy - 12, 16, 12, C_WHITE);
    // 门（黑色矩形挖洞）
    display->fillRect(hx + 6, hy - 6, 4, 6, C_BLACK);
    // 窗户（黑色方块）
    display->fillRect(hx + 2, hy - 10, 3, 3, C_BLACK);
  }

  // =====================================================================
  //  drawHouseZzz — 猫在房子里睡觉时 zzz 从窗户飘出
  // =====================================================================
  void drawHouseZzz() {
    display->setTextSize(1);
    display->setTextColor(C_WHITE);
    int zPhase = (millis() / 100) % 25;
    if (zPhase < 22) {
      int zy = 36 - zPhase;     // 从房子窗户(y=36)向上飘
      display->setCursor(108, zy);
      display->print("z");
      display->setCursor(113, zy - 4);
      display->print("Z");
    }

    // 门里上下动的小点 = 模拟猫在房子里活动
    // 门范围：x=110-113, y=50-55（黑色），白点在黑色门上可见
    float bounce = sinf(millis() * 0.003f);   // -1..1
    int doorX = 111 + (int)(bounce * 0.5f);   // 门中间 x，左右微动
    int doorY = 52 + (int)(bounce * 1.5f);     // 门中间 y，上下动
    display->drawPixel(doorX, doorY, C_WHITE);
    display->drawPixel(doorX + 1, doorY, C_WHITE);
  }

  // =====================================================================
  //  Body + head + ears + eyes  (no legs, no tail)
  // =====================================================================
  void drawBody(bool awake, int bx, int by) {
    bool headOnRight = (walkDir > 0);

    display->fillRoundRect(bx, by, 14, 7, 3, C_WHITE);

    if (!headOnRight) {
      // Facing left: head on the left side
      display->fillRect(bx - 2, by + 1, 3, 4, C_WHITE);
      display->fillCircle(bx - 4, by - 1, 4, C_WHITE);
      display->fillTriangle(bx - 7, by - 3, bx - 6, by - 7, bx - 5, by - 3, C_WHITE);
      display->fillTriangle(bx - 3, by - 3, bx - 2, by - 7, bx - 1, by - 3, C_WHITE);
      if (awake) {
        display->drawPixel(bx - 5, by - 1, C_BLACK);
        display->drawPixel(bx - 2, by - 1, C_BLACK);
      } else {
        display->drawLine(bx - 6, by - 1, bx - 4, by - 1, C_BLACK);
        display->drawLine(bx - 3, by - 1, bx - 1, by - 1, C_BLACK);
      }
      display->drawPixel(bx - 3, by + 1, C_BLACK);
      display->drawLine(bx - 8, by, bx - 11, by - 1, C_WHITE);
      display->drawLine(bx - 8, by + 1, bx - 11, by + 2, C_WHITE);
    } else {
      // Facing right: head on the right side
      display->fillRect(bx + 13, by + 1, 3, 4, C_WHITE);
      display->fillCircle(bx + 18, by - 1, 4, C_WHITE);
      display->fillTriangle(bx + 21, by - 3, bx + 20, by - 7, bx + 19, by - 3, C_WHITE);
      display->fillTriangle(bx + 17, by - 3, bx + 16, by - 7, bx + 15, by - 3, C_WHITE);
      if (awake) {
        display->drawPixel(bx + 19, by - 1, C_BLACK);
        display->drawPixel(bx + 16, by - 1, C_BLACK);
      } else {
        display->drawLine(bx + 20, by - 1, bx + 18, by - 1, C_BLACK);
        display->drawLine(bx + 17, by - 1, bx + 15, by - 1, C_BLACK);
      }
      display->drawPixel(bx + 17, by + 1, C_BLACK);
      display->drawLine(bx + 22, by, bx + 25, by - 1, C_WHITE);
      display->drawLine(bx + 22, by + 1, bx + 25, by + 2, C_WHITE);
    }
  }

  // =====================================================================
  //  Tail — continuous interpolation between the 3 original key-frames
  //  raise = 0  → frame 0 (horizontal, pointing behind)
  //  raise = 1  → frame 2 (pointing up)
  // =====================================================================
  void drawTail(int bx, int by) {
    float raise  = 0.5f + 0.5f * sinf(tailPhase);   // 0 .. 1
    float behind = -(float)walkDir;                  // direction away from head
    int   tx     = (walkDir > 0) ? bx : bx + 14;     // tail root at back edge
    int   ty     = by + 3;

    // Interpolate segment endpoints between frame 0 and frame 2
    float s1b = lerpf(6, 3, raise);   float s1u = lerpf(0, 5, raise);
    float s2b = lerpf(9, 5, raise);   float s2u = lerpf(2, 8, raise);

    int p1x = tx + (int)(behind * s1b),  p1y = ty - (int)s1u;
    int p2x = tx + (int)(behind * s2b),  p2y = ty - (int)s2u;

    display->drawLine(tx,  ty,  p1x, p1y, C_WHITE);
    display->drawLine(p1x, p1y, p2x, p2y, C_WHITE);
  }

  // =====================================================================
  //  Legs — continuous walk cycle (only when awake)
  //  Two legs slide forward / backward in opposite phase.
  // =====================================================================
  void drawLegs(int bx, int by) {
    float p = (sinf(legPhase) + 1.0f) / 2.0f;   // 0 .. 1
    int legY = by + 7;
    int x1 = bx + 2 + (int)(p * 3.0f);           // 2 .. 5
    int x2 = bx + 10 - (int)((1 - p) * 2.0f);    // 8 .. 10  (opposite phase)
    int len = 3;
    display->drawLine(x1, legY, x1, legY + len, C_WHITE);
    display->drawLine(x2, legY, x2, legY + len, C_WHITE);
  }

  // =====================================================================
  //  zzz — smooth upward float, ~2.5 s per cycle, top 3 frames hidden
  // =====================================================================
  void drawZzz(int bx) {
    display->setTextSize(1);
    display->setTextColor(C_WHITE);
    int zPhase = (millis() / 100) % 25;          // 0 .. 24
    if (zPhase < 22) {                            // hide wrap-around
      int zy = 40 - zPhase;                       // rises 40 → 18
      display->setCursor(bx + 18, zy);
      display->print("z");
      display->setCursor(bx + 23, zy - 4);
      display->print("Z");
    }
  }

  // =====================================================================
  //  Heart — pulse ~1.7 Hz, shown when state == 3
  //  Drawn at the horizontal center of the cat's body (body is 14 wide,
  //  so center is bx+7), not the left edge.
  // =====================================================================
  void drawHeart(int bx) {
    int hx = bx + 7, hy = 35;
    display->fillCircle(hx - 2, hy, 2, C_WHITE);
    display->fillCircle(hx + 2, hy, 2, C_WHITE);
    display->fillTriangle(hx - 4, hy, hx + 4, hy, hx, hy + 5, C_WHITE);
    if (millis() % 600 < 300) {                   // big pulse
      display->fillCircle(hx - 3, hy, 3, C_WHITE);
      display->fillCircle(hx + 3, hy, 3, C_WHITE);
      display->fillTriangle(hx - 5, hy, hx + 5, hy, hx, hy + 7, C_WHITE);
    }
  }

  // =====================================================================
  //  Overlay — top-left: weekday/time,  top-right: uptime
  // =====================================================================
  void drawOverlay() {
    if (overlayTime || overlayWeekday) {
      display->fillRect(0, 0, 50, 10, C_BLACK);
      display->setTextSize(1);
      display->setTextColor(C_WHITE);
      display->setCursor(1, 1);
      if (overlayWeekday && overlayTime) {
        display->print(ovWeekdayStr);
        display->print(" ");
        display->print(ovTimeStr);
      } else if (overlayWeekday) {
        display->print(ovWeekdayStr);
      } else {
        display->print(ovTimeStr);
      }
    }
    if (overlayUptime) {
      display->fillRect(85, 0, 43, 10, C_BLACK);
      display->setCursor(86, 1);
      display->print(ovUptimeStr);
    }
  }
};

#endif // _SLEEPING_CAT_H
