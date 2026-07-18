#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <HTTPClient.h>
#include <time.h>
#include <Wire.h>
#include <Adafruit_SSD1306.h>
#include <SPIFFS.h>
#include "FluxGarage_RoboEyes.h"
#include "SleepingCat.h"
#include <HWCDC.h>
HWCDC USBSer;
#define Serial USBSer

// 本地凭证（src/secrets.h，gitignored）：拷贝 src/secrets.h.example 填自己的值
// 没配置也能编译，但 WiFi 连不上、天气功能不可用
#if __has_include("secrets.h")
  #include "secrets.h"
#endif

/* ================= OLED ================= */
#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 64
#define OLED_RESET -1
#define OLED_SDA 8
#define OLED_SCL 7

Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RESET);
RoboEyes<Adafruit_SSD1306> roboEyes(display);
SleepingCat<Adafruit_SSD1306> sleepingCat(display);
bool oledAvailable = false;

/* ================= MOTOR PIN ================= */
#define LF   0
#define LB   1
#define RF   2
#define RB   3
#define STBY 10
#define BOOT_BTN 9   // GPIO9 = BOOT 键

/* ================= WIFI (STA 模式) =================
 * 凭证在 src/secrets.h 里配置（拷贝 src/secrets.h.example 填自己的值，
 * 该文件已 gitignore）。没配置也能编译，但会 fallback 到明显占位字符串，
 * WiFi 连不上，方便第一时间发现配置缺失。
 */
WebServer server(80);
#ifndef WIFI_SSID
  #define WIFI_SSID "CONFIGURE_IN_secrets_h"
#endif
#ifndef WIFI_PASS
  #define WIFI_PASS "CONFIGURE_IN_secrets_h"
#endif

// 推屏。ESP32 I2C 驱动基于中断，不能用 noInterrupts 包裹。
// weatherTask 跑 TLS 时不再阻塞主 loop，所以这里不需要任何特殊保护。
void safeDisplay() {
  display.display();
}

/* ================= 显示配置（存 SPIFFS） ================= */
struct DisplayConfig {
  bool showTime = true;      // 时间
  bool showWeekday = true;   // 周几
  bool showUptime = true;    // 开机时间
  bool showMoon = true;      // 月相
  bool showWeather = true;   // 天气
};
DisplayConfig cfg;

void loadConfig() {
  File f = SPIFFS.open("/cfg.dat", "r");
  if (f && f.size() == sizeof(cfg)) {
    f.read((uint8_t*)&cfg, sizeof(cfg));
    f.close();
  } else {
    if (f) f.close();
  }
}
void saveConfig() {
  File f = SPIFFS.open("/cfg.dat", "w");
  if (f) {
    f.write((const uint8_t*)&cfg, sizeof(cfg));
    f.close();
  }
}

/* ================= 天气同步（Open-Meteo，独立任务，不阻塞主 loop） ================= */
// 广州坐标 23.13, 113.26，WMO weathercode 标准
volatile int weatherCode = 999;    // 999=未获取
volatile bool weatherReady = false;
volatile bool wifiBusy = false;    // WiFi 传输中，主 loop 检测到就暂停刷新

// 日出日落（Unix epoch 秒，UTC）
volatile time_t sunriseTime = 0;
volatile time_t sunsetTime = 0;

// 默认坐标：广州 23.13, 113.26。在 src/secrets.h 里改成你自己的位置
#ifndef LAT
  #define LAT 23.13
#endif
#ifndef LON
  #define LON 113.26
#endif

void weatherTask(void *param) {
  // 等待 WiFi 连上
  while (WiFi.status() != WL_CONNECTED) {
    vTaskDelay(pdMS_TO_TICKS(2000));
  }

  // 关键：WiFi 连上后先让出 10 秒，等主 loop 完成 setupServer() + 首次 HTTP 请求
  // 否则 TLS 握手会占满 CPU，主 loop 没机会启动 web server
  vTaskDelay(pdMS_TO_TICKS(10000));

  while (true) {
    // 不再设置 wifiBusy —— 主 loop 不被阻塞，OLED 正常刷帧
    // open-meteo 只支持 HTTPS，TLS 握手在 ESP32-C3 单核上会挤压主 loop 数秒
    // 但天气同步是低频操作（成功 2h 一次），可以接受
    WiFiClientSecure client;
    client.setInsecure();
    HTTPClient http;
    // 加 daily=sunrise,sunset 拿日出日落时间
    char url[160];
    snprintf(url, sizeof(url),
             "https://api.open-meteo.com/v1/forecast?latitude=%.2f&longitude=%.2f&current_weather=true&daily=sunrise,sunset&timezone=Asia/Shanghai",
             (double)LAT, (double)LON);
    http.begin(client, url);
    http.setTimeout(5000);  // 5 秒超时
    int code = http.GET();
    if (code == 200) {
      String resp = http.getString();
      // 当前天气
      int cwIdx = resp.indexOf("\"current_weather\":");
      if (cwIdx >= 0) {
        int wcIdx = resp.indexOf("\"weathercode\":", cwIdx);
        if (wcIdx >= 0) {
          int start = wcIdx + 14;
          int end = resp.indexOf(',', start);
          if (end < 0) end = resp.indexOf('}', start);
          String codeStr = resp.substring(start, end);
          codeStr.trim();
          weatherCode = codeStr.toInt();
          weatherReady = true;
        }
      }
      // 日出日落："sunrise":["2025-xx-xxT06:00","..."],"sunset":["2025-xx-xxT18:00","..."]
      int srIdx = resp.indexOf("\"sunrise\":[\"");
      if (srIdx >= 0) {
        int s = srIdx + 12;  // "sunrise":[" = 12 字符
        int e = resp.indexOf('"', s);
        if (e > s) {
          struct tm tm = {0};
          strptime(resp.substring(s, e).c_str(), "%Y-%m-%dT%H:%M", &tm);
          sunriseTime = mktime(&tm) - 8 * 3600;  // mktime 按本地时区算，转 UTC
        }
      }
      int ssIdx = resp.indexOf("\"sunset\":[\"");
      if (ssIdx >= 0) {
        int s = ssIdx + 11;  // "sunset":[" = 11 字符
        int e = resp.indexOf('"', s);
        if (e > s) {
          struct tm tm = {0};
          strptime(resp.substring(s, e).c_str(), "%Y-%m-%dT%H:%M", &tm);
          sunsetTime = mktime(&tm) - 8 * 3600;
        }
      }
    }
    http.end();

    // 成功 2 小时同步一次；失败 60 秒后重试
    if (code == 200) {
      vTaskDelay(pdMS_TO_TICKS(2 * 60 * 60 * 1000));
    } else {
      vTaskDelay(pdMS_TO_TICKS(60 * 1000));
    }
  }
}

// WMO weathercode → 天气类型
// 0=晴 1=多云 2=阴 3=小雨 4=大雨 5=雷雨 6=雪 7=雾
int getWeatherType() {
  if (weatherCode == 999) return 0;
  if (weatherCode == 0) return 0;              // 晴
  if (weatherCode >= 1 && weatherCode <= 2) return 1;  // 少云/多云
  if (weatherCode == 3) return 2;              // 阴
  if (weatherCode >= 45 && weatherCode <= 48) return 7;  // 雾
  if (weatherCode >= 51 && weatherCode <= 57) return 3;  // 毛毛雨
  if (weatherCode >= 61 && weatherCode <= 65) return 4;  // 中大雨
  if (weatherCode >= 66 && weatherCode <= 67) return 3;  // 冻雨
  if (weatherCode >= 71 && weatherCode <= 77) return 6;  // 雪
  if (weatherCode >= 80 && weatherCode <= 81) return 3;  // 阵雨
  if (weatherCode == 82) return 4;            // 暴阵雨
  if (weatherCode >= 95 && weatherCode <= 99) return 5;  // 雷雨
  return 0;
}

/* ================= 农历月相计算 ================= */
// 农历数据表 2023-2030（每年 19 位：12 个月大小 + 闰月大小）
// 简化版：用公历日期估算月相（从已知满月日推算）
// 月相周期 29.53 天，2024-01-25 是满月（农历十五）
// moonPhase: 0=新月 1=蛾眉 2=上弦 3=盈凸 4=满月 5=亏凸 6=下弦 7=残月

// 月相缓存：当天内不变，避免视觉上"月相变来变去"
int cachedMoonPhase = -1;     // -1 = 尚未计算
int cachedMoonDay = -1;       // 缓存对应的日期（tm_mday）

int getMoonPhase() {
  struct tm timeinfo;
  if (!getLocalTime(&timeinfo)) return (cachedMoonPhase >= 0 ? cachedMoonPhase : 4);

  // 当天内只算一次
  if (cachedMoonPhase >= 0 && cachedMoonDay == timeinfo.tm_mday) {
    return cachedMoonPhase;
  }

  // 转成天数（从 2000-01-01 算）
  long days = (timeinfo.tm_year - 100) * 365L + (timeinfo.tm_year - 100) / 4;
  static const int monthDays[] = {31,28,31,30,31,30,31,31,30,31,30,31};
  for (int m = 0; m < timeinfo.tm_mon; m++) {
    days += monthDays[m];
    if (m == 1 && (timeinfo.tm_year % 4 == 0)) days++;  // 闰年 2 月
  }
  days += timeinfo.tm_mday;
  // 已知 2024-01-25 是满月，days for that date = 24*365 + 24/4 + 25 = 8801
  // 用 2000-01-06 是新月（农历初一），days = 6
  long knownNewMoon = 6;  // 2000-01-06 新月
  long phaseDays = (days - knownNewMoon) % 30;  // 简化用 30 天周期
  if (phaseDays < 0) phaseDays += 30;
  // phaseDays: 0=新月, 7-8=上弦, 14-16=满月, 22-23=下弦
  int phase;
  if (phaseDays < 2) phase = 0;       // 新月
  else if (phaseDays < 7) phase = 1;  // 蛾眉月
  else if (phaseDays < 9) phase = 2;  // 上弦月
  else if (phaseDays < 14) phase = 3; // 盈凸月
  else if (phaseDays < 17) phase = 4; // 满月
  else if (phaseDays < 22) phase = 5; // 亏凸月
  else if (phaseDays < 24) phase = 6; // 下弦月
  else phase = 7;                     // 残月

  cachedMoonPhase = phase;
  cachedMoonDay = timeinfo.tm_mday;
  return phase;
}

// 画月相（cx,cy 中心，phase 0-7）
void drawMoon(int cx, int cy, int phase) {
  int r = 4;
  switch (phase) {
    case 4:  // 满月
      display.fillCircle(cx, cy, r, SSD1306_WHITE);
      break;
    case 0:  // 新月（只画轮廓）
      display.drawCircle(cx, cy, r, SSD1306_WHITE);
      break;
    case 2:  // 上弦月（右半亮）
      display.fillCircle(cx, cy, r, SSD1306_WHITE);
      display.fillCircle(cx + r/2, cy, r, SSD1306_BLACK);
      break;
    case 6:  // 下弦月（左半亮）
      display.fillCircle(cx, cy, r, SSD1306_WHITE);
      display.fillCircle(cx - r/2, cy, r, SSD1306_BLACK);
      break;
    case 1:  // 蛾眉月（右侧弯月）
      display.fillCircle(cx, cy, r, SSD1306_WHITE);
      display.fillCircle(cx + r, cy, r, SSD1306_BLACK);
      break;
    case 7:  // 残月（左侧弯月）
      display.fillCircle(cx, cy, r, SSD1306_WHITE);
      display.fillCircle(cx - r, cy, r, SSD1306_BLACK);
      break;
    case 3:  // 盈凸月（大部分亮，左侧缺一点）
      display.fillCircle(cx, cy, r, SSD1306_WHITE);
      display.fillCircle(cx - r - 1, cy, r - 1, SSD1306_BLACK);
      break;
    case 5:  // 亏凸月（大部分亮，右侧缺一点）
      display.fillCircle(cx, cy, r, SSD1306_WHITE);
      display.fillCircle(cx + r + 1, cy, r - 1, SSD1306_BLACK);
      break;
  }
}

// 画天气图标（小图标，在小猫上方天空区域）
void drawWeatherIcon(int type, int x, int y) {
  switch (type) {
    case 0:  // 晴：太阳（光线）
      display.fillCircle(x, y, 2, SSD1306_WHITE);
      display.drawLine(x, y-3, x, y-5, SSD1306_WHITE);
      display.drawLine(x, y+3, x, y+5, SSD1306_WHITE);
      display.drawLine(x-3, y, x-5, y, SSD1306_WHITE);
      display.drawLine(x+3, y, x+5, y, SSD1306_WHITE);
      display.drawLine(x-2, y-2, x-4, y-4, SSD1306_WHITE);
      display.drawLine(x+2, y+2, x+4, y+4, SSD1306_WHITE);
      display.drawLine(x-2, y+2, x-4, y+4, SSD1306_WHITE);
      display.drawLine(x+2, y-2, x+4, y-4, SSD1306_WHITE);
      break;
    case 1:  // 多云：太阳+云
      display.fillCircle(x-3, y-2, 2, SSD1306_WHITE);
      display.fillCircle(x+2, y, 3, SSD1306_WHITE);
      display.fillCircle(x-1, y, 3, SSD1306_WHITE);
      break;
    case 2:  // 阴：纯云
      display.fillCircle(x-3, y, 3, SSD1306_WHITE);
      display.fillCircle(x, y-1, 3, SSD1306_WHITE);
      display.fillCircle(x+3, y, 3, SSD1306_WHITE);
      break;
    case 3:  // 小雨：云+小雨滴
      display.fillCircle(x-3, y-2, 3, SSD1306_WHITE);
      display.fillCircle(x, y-3, 3, SSD1306_WHITE);
      display.fillCircle(x+3, y-2, 3, SSD1306_WHITE);
      display.drawPixel(x-2, y+3, SSD1306_WHITE);
      display.drawPixel(x+2, y+3, SSD1306_WHITE);
      break;
    case 4:  // 大雨：云+多雨滴
      display.fillCircle(x-3, y-2, 3, SSD1306_WHITE);
      display.fillCircle(x, y-3, 3, SSD1306_WHITE);
      display.fillCircle(x+3, y-2, 3, SSD1306_WHITE);
      for (int i = -3; i <= 3; i += 3) {
        display.drawLine(x+i, y+2, x+i, y+5, SSD1306_WHITE);
      }
      break;
    case 5:  // 雷雨：云+闪电
      display.fillCircle(x-3, y-2, 3, SSD1306_WHITE);
      display.fillCircle(x, y-3, 3, SSD1306_WHITE);
      display.fillCircle(x+3, y-2, 3, SSD1306_WHITE);
      // 闪电
      display.drawLine(x-1, y+1, x+1, y+1, SSD1306_WHITE);
      display.drawLine(x+1, y+1, x-1, y+4, SSD1306_WHITE);
      display.drawLine(x-1, y+4, x+1, y+4, SSD1306_WHITE);
      break;
    case 6:  // 雪：云+雪花
      display.fillCircle(x-3, y-2, 3, SSD1306_WHITE);
      display.fillCircle(x, y-3, 3, SSD1306_WHITE);
      display.fillCircle(x+3, y-2, 3, SSD1306_WHITE);
      display.drawPixel(x-2, y+3, SSD1306_WHITE);
      display.drawPixel(x, y+4, SSD1306_WHITE);
      display.drawPixel(x+2, y+3, SSD1306_WHITE);
      break;
    case 7:  // 雾：横线
      display.drawLine(x-5, y-3, x+5, y-3, SSD1306_WHITE);
      display.drawLine(x-4, y-1, x+4, y-1, SSD1306_WHITE);
      display.drawLine(x-5, y+1, x+5, y+1, SSD1306_WHITE);
      display.drawLine(x-3, y+3, x+3, y+3, SSD1306_WHITE);
      break;
  }
}

/* ================= STATE ================= */
volatile bool manualActive = false;
unsigned long showIpUntil = 0;   // 按 BOOT 键显示 IP 的截止时间

enum RandomMode { RANDOM_OFF, RANDOM_SOFT, RANDOM_NORMAL };
volatile RandomMode randomMode = RANDOM_OFF;  // 默认休眠模式

/* ================= WIFI MOTOR ================= */
void motorWifi(byte c) {
  digitalWrite(STBY, HIGH);
  switch (c) {
    case 0:
      digitalWrite(LF,LOW); digitalWrite(LB,LOW);
      digitalWrite(RF,LOW); digitalWrite(RB,LOW);
      break;
    case 1:
      digitalWrite(LF,HIGH); digitalWrite(LB,LOW);
      digitalWrite(RF,LOW);  digitalWrite(RB,HIGH);
      break;
    case 2:
      digitalWrite(LF,LOW);  digitalWrite(LB,HIGH);
      digitalWrite(RF,HIGH); digitalWrite(RB,LOW);
      break;
    case 3:
      digitalWrite(LF,LOW);  digitalWrite(LB,HIGH);
      digitalWrite(RF,LOW);  digitalWrite(RB,HIGH);
      break;
    case 4:
      digitalWrite(LF,HIGH); digitalWrite(LB,LOW);
      digitalWrite(RF,HIGH); digitalWrite(RB,LOW);
      break;
  }
}

/* ================= RANDOM MOTOR ================= */
void MOTOR(byte c,int t1,int t2,int Time){
  for(int i=0;i<Time;i++){
    switch (c) {
      case 0: digitalWrite(LF,LOW); digitalWrite(LB,LOW); digitalWrite(RF,LOW); digitalWrite(RB,LOW); break;
      case 1: digitalWrite(LF,LOW); digitalWrite(LB,HIGH);digitalWrite(RF,LOW); digitalWrite(RB,HIGH);break;
      case 2: digitalWrite(LF,HIGH);digitalWrite(LB,LOW); digitalWrite(RF,HIGH);digitalWrite(RB,LOW); break;
      case 3: digitalWrite(LF,LOW); digitalWrite(LB,HIGH);digitalWrite(RF,HIGH);digitalWrite(RB,LOW); break;
      case 4: digitalWrite(LF,HIGH);digitalWrite(LB,LOW); digitalWrite(RF,LOW); digitalWrite(RB,HIGH);break;
      case 5: digitalWrite(LF,LOW); digitalWrite(LB,HIGH);digitalWrite(RF,LOW); digitalWrite(RB,LOW); break;
      case 6: digitalWrite(LF,LOW); digitalWrite(LB,LOW); digitalWrite(RF,LOW); digitalWrite(RB,HIGH);break;
      case 7: digitalWrite(LF,HIGH);digitalWrite(LB,LOW); digitalWrite(RF,LOW); digitalWrite(RB,LOW); break;
      case 8: digitalWrite(LF,LOW); digitalWrite(LB,LOW); digitalWrite(RF,HIGH);digitalWrite(RB,LOW); break;
    }
    delay(t1);
    digitalWrite(LF,LOW); digitalWrite(LB,LOW);
    digitalWrite(RF,LOW); digitalWrite(RB,LOW);
    delay(t2);
  }
}

/* ================= 小猫动画 =================
 * 小猫的状态机、绘制、帧率控制全部封装在 SleepingCat 类里
 * （见 SleepingCat.h），跟 RoboEyes 一样的 begin()/update() 用法。
 * 天空背景（drawSky）仍留在 main.cpp，通过回调注入。
 */

// 用真实时间判断是否为夜晚
// 优先用 sunrise/sunset，拿不到就用 6:00-18:00 兜底
bool isNight() {
  struct tm timeinfo;
  if (!getLocalTime(&timeinfo)) return false;  // 时间没拿到，先当白天
  time_t nowLocal = mktime(&timeinfo);  // 本地时间 epoch

  if (sunriseTime != 0 && sunsetTime != 0) {
    // sunriseTime/sunsetTime 是 UTC，转成本地 epoch 比较
    time_t srLocal = sunriseTime + 8 * 3600;
    time_t ssLocal = sunsetTime + 8 * 3600;
    // 注意：mktime 会根据 tm_isdst 处理夏令时，但中国无 DST，可直接比较
    // 跨日处理：日落后到现在 < 第二天日出 = 夜晚
    if (nowLocal >= ssLocal) {
      // 今天的日落之后，看是否在明天日出之前
      time_t nextSunrise = srLocal + 86400;
      return nowLocal < nextSunrise;
    } else if (nowLocal < srLocal) {
      // 今天的日出之前（凌晨），属于夜晚
      return true;
    }
    return false;  // 日出后到日落前 = 白天
  }

  // 兜底：6:00-18:00 为白天，其余为夜晚
  int minutes = timeinfo.tm_hour * 60 + timeinfo.tm_min;
  return minutes < 6 * 60 || minutes >= 18 * 60;
}

bool isDaytime() {
  return !isNight();
}

// 画云（多片云，从左往右飘）；y 起点降到 15 让出顶部时间显示区
void drawClouds(int count) {
  unsigned long t = millis() / 200;  // 云移动速度
  for (int i = 0; i < count; i++) {
    int cx = (t * 2 + i * 35) % 140 - 10;
    int cy = 15 + (i % 3) * 5;  // 高度从 5 降到 15，避开顶部时间显示区
    display.fillCircle(cx, cy, 3, SSD1306_WHITE);
    display.fillCircle(cx + 3, cy, 4, SSD1306_WHITE);
    display.fillCircle(cx + 7, cy, 3, SSD1306_WHITE);
    display.fillCircle(cx + 4, cy - 2, 3, SSD1306_WHITE);
  }
}

// 计算天体 X 坐标（基于真实日出日落）
// 白天：日出（X=10，最左）→ 日落（X=120，最右）
// 夜晚：日落后立刻在 X=120（最右）→ 日出前到 X=10（最左）
int getCelestialX(bool night) {
  struct tm timeinfo;
  if (!getLocalTime(&timeinfo)) return 60;  // 时间没拿到，放中间
  time_t nowLocal = mktime(&timeinfo);

  if (sunriseTime != 0 && sunsetTime != 0) {
    time_t srLocal = sunriseTime + 8 * 3600;
    time_t ssLocal = sunsetTime + 8 * 3600;
    if (!night) {
      double p = (double)(nowLocal - srLocal) / (ssLocal - srLocal);
      if (p < 0) p = 0; if (p > 1) p = 1;
      return 10 + (int)(p * 110);
    } else {
      time_t nightStart = (nowLocal >= ssLocal) ? ssLocal : ssLocal - 86400;  // 今晚或昨晚的日落
      time_t nightEnd = (nowLocal >= ssLocal) ? srLocal + 86400 : srLocal;   // 明天或今天的日出
      double p = (double)(nowLocal - nightStart) / (nightEnd - nightStart);
      if (p < 0) p = 0; if (p > 1) p = 1;
      return 120 - (int)(p * 110);  // 从右往左
    }
  }

  // 兜底：6:00-18:00 白天，18:00-次日6:00 夜晚
  int minutes = timeinfo.tm_hour * 60 + timeinfo.tm_min;
  if (!night) {
    // 6:00 (360) → 18:00 (1080) 映射 X: 10→120
    int p = minutes - 360;
    if (p < 0) p = 0; if (p > 720) p = 720;
    return 10 + p * 110 / 720;
  } else {
    // 18:00 (1080) → 次日 6:00 (360+1440) 映射 X: 120→10
    int startMin = (minutes >= 1080) ? 1080 : 1080 - 1440;  // 今晚或昨晚的 18:00
    int p = minutes - startMin;
    int nightLen = 1440 - 720;  // 12 小时
    return 120 - p * 110 / nightLen;
  }
}

void drawSky() {
  int weatherType = getWeatherType();
  bool night = isNight();  // 用真实时间判断昼夜

  // 天体 Y 坐标降到 14，避开顶部时间显示区 (0-9)
  int celestialY = 14;
  int celestialX = getCelestialX(night);

  // 月亮：仅晴/多云/阴可见，受 cfg.showMoon 开关控制
  bool showMoon = night && cfg.showMoon && (weatherType <= 2);
  if (showMoon) {
    drawMoon(celestialX, celestialY, getMoonPhase());
  }

  // 太阳：仅白天，受 cfg.showWeather 开关控制
  bool showSun = !night && cfg.showWeather && (weatherType <= 2);
  if (showSun) {
    display.fillCircle(celestialX, celestialY, 3, SSD1306_WHITE);
    display.drawLine(celestialX, celestialY - 5, celestialX, celestialY - 7, SSD1306_WHITE);
    display.drawLine(celestialX, celestialY + 5, celestialX, celestialY + 7, SSD1306_WHITE);
    display.drawLine(celestialX - 5, celestialY, celestialX - 7, celestialY, SSD1306_WHITE);
    display.drawLine(celestialX + 5, celestialY, celestialX + 7, celestialY, SSD1306_WHITE);
    display.drawLine(celestialX - 4, celestialY - 4, celestialX - 6, celestialY - 6, SSD1306_WHITE);
    display.drawLine(celestialX + 4, celestialY + 4, celestialX + 6, celestialY + 6, SSD1306_WHITE);
    display.drawLine(celestialX - 4, celestialY + 4, celestialX - 6, celestialY + 6, SSD1306_WHITE);
    display.drawLine(celestialX + 4, celestialY - 4, celestialX + 6, celestialY - 6, SSD1306_WHITE);
  }

  // 星星：晚上一定画，无论天气（下雨也画），独立于月亮开关
  // 位置固定不变，原地变大变小实现闪烁
  // 每种天气的星星位置都均匀分布在屏幕宽度上，避免集中
  if (night) {
    // 星星数量按天气递减：晴 7 → 多云 5 → 阴 3 → 雨/雪/雾 1
    int starCount;
    if (weatherType == 0) starCount = 7;
    else if (weatherType == 1) starCount = 5;
    else if (weatherType == 2) starCount = 3;
    else starCount = 1;

    // 每种数量的星星位置数组（均匀分布在 0-128 宽度上）
    static const int stars7[][2] = {{8,12},{24,9},{40,16},{56,11},{72,15},{88,10},{104,14}};
    static const int stars5[][2] = {{8,12},{36,9},{64,15},{92,11},{118,16}};
    static const int stars3[][2] = {{10,12},{64,15},{118,11}};
    static const int stars1[][2] = {{64,15}};

    const int (*stars)[2];
    if (starCount == 7) stars = stars7;
    else if (starCount == 5) stars = stars5;
    else if (starCount == 3) stars = stars3;
    else stars = stars1;

    unsigned long t = millis();
    for (int i = 0; i < starCount; i++) {
      int x = stars[i][0], y = stars[i][1];
      // 每颗星错开相位，周期 2 秒（4 阶段：大→小→空→小→大）
      // 0=大(2x2) 1=小(1x1) 2=空 3=小(1x1)
      int phase = ((t / 500) + i * 137) % 4;
      if (phase == 0) {
        // 大：2x2 像素
        display.drawPixel(x, y, SSD1306_WHITE);
        display.drawPixel(x + 1, y, SSD1306_WHITE);
        display.drawPixel(x, y + 1, SSD1306_WHITE);
        display.drawPixel(x + 1, y + 1, SSD1306_WHITE);
      } else if (phase == 1 || phase == 3) {
        // 小：1x1
        display.drawPixel(x, y, SSD1306_WHITE);
      }
      // phase == 2: 不画（暗）
    }
  }

  // 天气元素（受开关控制）
  if (!cfg.showWeather) {
    display.drawLine(0, 56, 128, 56, SSD1306_WHITE);
    return;
  }

  if (weatherType == 1) {
    // 多云：多几片云
    drawClouds(3);
  } else if (weatherType == 2) {
    // 阴：更多云
    drawClouds(4);
  } else if (weatherType == 3 || weatherType == 4 || weatherType == 5) {
    // 雨/雷雨：云 + 雨滴
    drawClouds(2);
    // 雨点
    int rainOffset = (millis() / 80) % 10;
    int rainCount = (weatherType == 4 || weatherType == 5) ? 10 : 5;
    for (int i = 0; i < rainCount; i++) {
      int rx = (i * 13 + rainOffset * 2) % 128;
      int ry = 22 + ((rainOffset + i * 3) % 28);
      display.drawLine(rx, ry, rx, ry + 3, SSD1306_WHITE);
    }
    // 雷雨：闪电
    if (weatherType == 5 && (millis() % 2000) < 200) {
      display.drawLine(60, 22, 55, 35, SSD1306_WHITE);
      display.drawLine(55, 35, 62, 35, SSD1306_WHITE);
      display.drawLine(62, 35, 57, 48, SSD1306_WHITE);
    }
  } else if (weatherType == 6) {
    // 雪：云 + 雪花飘落
    drawClouds(2);
    int snowOffset = (millis() / 200) % 10;
    for (int i = 0; i < 6; i++) {
      int sx = 20 + i * 18 + (snowOffset % 4);
      int sy = 22 + ((snowOffset + i * 4) % 28);
      display.drawPixel(sx, sy, SSD1306_WHITE);
      display.drawPixel(sx + 1, sy, SSD1306_WHITE);
      display.drawPixel(sx, sy + 1, SSD1306_WHITE);
      display.drawPixel(sx + 1, sy + 1, SSD1306_WHITE);
    }
  } else if (weatherType == 7) {
    // 雾：横线
    display.drawLine(0, 25, 40, 25, SSD1306_WHITE);
    display.drawLine(50, 30, 90, 30, SSD1306_WHITE);
    display.drawLine(20, 35, 60, 35, SSD1306_WHITE);
    display.drawLine(70, 40, 110, 40, SSD1306_WHITE);
    display.drawLine(10, 45, 50, 45, SSD1306_WHITE);
  }

  // 地平线
  display.drawLine(0, 56, 128, 56, SSD1306_WHITE);
}

/* ================= WEB UI ================= */
void handleRoot() {
  String page = R"rawliteral(
<!DOCTYPE html>
<html>
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<style>
body{margin:0;height:100vh;background:radial-gradient(circle at top,#0f2027,#000);color:#00ffe1;font-family:Arial;display:flex;align-items:center;justify-content:center;}
.panel{width:260px;padding:20px;border-radius:18px;background:rgba(0,255,225,0.05);border:1px solid rgba(0,255,225,0.4);box-shadow:0 0 25px rgba(0,255,225,0.3);}
h2{text-align:center;margin:0 0 14px;letter-spacing:2px;}
.grid{display:grid;grid-template-columns:1fr 1fr 1fr;grid-template-rows:60px 60px 60px;gap:10px;}
button{border:none;border-radius:12px;font-size:16px;font-weight:bold;background:linear-gradient(145deg,#0ff,#00b3a4);}
.stop{background:linear-gradient(145deg,#ff5555,#aa0000);color:#fff;}
.empty{background:none;}
.mode{margin-top:12px;display:flex;gap:6px;}
.mode button{flex:1;font-size:13px;opacity:0.6;}
.mode button.active{opacity:1;background:linear-gradient(145deg,#00ff9c,#00c46a);box-shadow:0 0 12px rgba(0,255,180,0.8);}
.footer{margin-top:14px;text-align:center;font-size:11px;opacity:0.6;letter-spacing:1px;}
</style>
</head>
<body>
<div class="panel">
<h2>MOCHAN 摸铲</h2>
<div class="grid">
  <div class="empty"></div>
  <button onclick="fetch('/f')">前进</button>
  <div class="empty"></div>
  <button onclick="fetch('/l')">左转</button>
  <button class="stop" onclick="fetch('/s')">停止</button>
  <button onclick="fetch('/r')">右转</button>
  <div class="empty"></div>
  <button onclick="fetch('/b')">后退</button>
  <div class="empty"></div>
</div>
<div class="mode">
  <button id="btn_sleep" class="active" onclick="setMode('off')">休眠</button>
  <button id="btn_wiggle" onclick="setMode('soft')">摇摆</button>
  <button id="btn_curious" onclick="setMode('normal')">好奇</button>
</div>
<div class="footer"><a href="/bg" style="color:#00ffe1">背景设置</a></div>
</div>
<script>
function clearActive(){document.getElementById('btn_sleep').classList.remove('active');document.getElementById('btn_wiggle').classList.remove('active');document.getElementById('btn_curious').classList.remove('active');}
function setMode(mode){fetch('/mode_'+mode);clearActive();if(mode==='off')document.getElementById('btn_sleep').classList.add('active');if(mode==='soft')document.getElementById('btn_wiggle').classList.add('active');if(mode==='normal')document.getElementById('btn_curious').classList.add('active');}
</script>
</body></html>)rawliteral";
  server.send(200, "text/html; charset=utf-8", page);
}

/* ================= 上传背景图 ================= */
void handleUploadPage() {
  String page = R"rawliteral(
<!DOCTYPE html><html><head><meta charset=UTF-8><meta name=viewport content="width=device-width,initial-scale=1">
<style>*{box-sizing:border-box}body{margin:0;background:#111;color:#0ff;font-family:Arial;padding:12px;font-size:14px}
h2{margin:0 0 8px;font-size:18px}.row{margin:8px 0}
input[type=file],input[type=range]{background:#022;border:1px solid #0ff;color:#0ff;padding:6px;border-radius:6px;width:100%}
button{background:#022;border:1px solid #0ff;color:#0ff;padding:8px 12px;border-radius:6px;margin:4px 2px;cursor:pointer}
button:active{background:#0ff;color:#000}
a{color:#0ff;font-size:13px}
.prev{display:flex;gap:8px;flex-wrap:wrap;margin:8px 0}
.prev div{flex:1;min-width:200px}
.prev h3{margin:0 0 4px;font-size:12px;color:#0aa}
canvas{border:1px solid #0ff;background:#000;display:block;width:100%;image-rendering:pixelated;aspect-ratio:2/1}
</style></head><body>
<h2>OLED 背景上传</h2>
<div class=row><input type=file id=f accept="image/*"></div>
<div class=row><label>阈值 <input type=range id=th min=0 max=255 value=128></label></div>
<div class=row id=switches style="display:flex;flex-wrap:wrap;gap:6px">
<label><input type=checkbox id=sw_time checked>时间</label>
<label><input type=checkbox id=sw_weekday checked>周几</label>
<label><input type=checkbox id=sw_uptime checked>开机</label>
<label><input type=checkbox id=sw_moon checked>月相</label>
<label><input type=checkbox id=sw_weather checked>天气</label>
<button onclick=saveCfg()>保存开关</button>
</div>
<div class=prev>
<div><h3>原图预览 (128x64)</h3><canvas id=r width=128 height=64></canvas></div>
<div><h3>休眠效果预览</h3><canvas id=p width=128 height=64></canvas></div>
</div>
<div class=row>
<button onclick=upload()>上传</button>
<button onclick=delBg()>删除背景</button>
<a href=/>返回控制</a>
</div>
<script>
const R=document.getElementById('r'),RX=R.getContext('2d');
const P=document.getElementById('p'),PX=P.getContext('2d');
const img=new Image();let bits=new Uint8Array(1024);
document.getElementById('th').oninput=()=>{if(img.src)render();};
document.getElementById('f').onchange=e=>{const r=new FileReader();r.onload=()=>{img.onload=render;img.src=r.result;};r.readAsDataURL(e.target.files[0]);};
function render(){
RX.fillStyle='#000';RX.fillRect(0,0,128,64);RX.drawImage(img,0,0,128,64);
const d=RX.getImageData(0,0,128,64).data;const th=+document.getElementById('th').value;
bits.fill(0);
for(let y=0;y<64;y++)for(let x=0;x<128;x++){const i=(y*128+x)*4;const g=(d[i]+d[i+1]+d[i+2])/3;if(g>th)bits[(y>>3)*128+x]|=1<<(y&7);}
drawPreview();
}
function drawPreview(){
PX.fillStyle='#000';PX.fillRect(0,0,128,64);
for(let y=0;y<64;y++)for(let x=0;x<128;x++){if(bits[(y>>3)*128+x]&(1<<(y&7))){PX.fillStyle='#0ff';PX.fillRect(x,y,1,1);}}
// 模拟左上时钟
PX.fillStyle='#000';PX.fillRect(0,0,30,9);PX.fillStyle='#0ff';PX.font='8px Arial';PX.fillText('12:34',1,8);
// 模拟右上运行时间
PX.fillStyle='#000';PX.fillRect(95,0,33,9);PX.fillStyle='#0ff';PX.fillText('UP5m',96,8);
}
function upload(){fetch('/delbg',{method:'POST'}).finally(()=>{const fd=new FormData();fd.append('bg',new Blob([bits.buffer]),'bg.dat');fetch('/upload',{method:'POST',body:fd}).then(r=>r.text()).then(t=>alert('上传成功 '+t)).catch(e=>alert('失败 '+e));});}
function delBg(){fetch('/delbg',{method:'POST'}).then(r=>alert('已删除'));}
function saveCfg(){
  const p=new URLSearchParams();
  p.append('time',document.getElementById('sw_time').checked?'1':'0');
  p.append('weekday',document.getElementById('sw_weekday').checked?'1':'0');
  p.append('uptime',document.getElementById('sw_uptime').checked?'1':'0');
  p.append('moon',document.getElementById('sw_moon').checked?'1':'0');
  p.append('weather',document.getElementById('sw_weather').checked?'1':'0');
  fetch('/cfg',{method:'POST',body:p}).then(r=>alert('开关已保存'));
}
fetch('/cfg').then(r=>r.json()).then(c=>{
  document.getElementById('sw_time').checked=c.time;
  document.getElementById('sw_weekday').checked=c.weekday;
  document.getElementById('sw_uptime').checked=c.uptime;
  document.getElementById('sw_moon').checked=c.moon;
  document.getElementById('sw_weather').checked=c.weather;
});
</script></body></html>)rawliteral";
  server.send(200, "text/html; charset=utf-8", page);
}

File uploadFile;
void handleUpload() {
  // POST 完成后的响应
  server.send(200, "text/plain", "OK");
}

void handleUploadFile() {
  // multipart 上传：逐块接收
  HTTPUpload& upload = server.upload();
  if (upload.status == UPLOAD_FILE_START) {
    uploadFile = SPIFFS.open("/bg.dat", "w");
  } else if (upload.status == UPLOAD_FILE_WRITE) {
    if (uploadFile) {
      uploadFile.write(upload.buf, upload.currentSize);
    }
  } else if (upload.status == UPLOAD_FILE_END) {
    if (uploadFile) {
      uploadFile.close();
    }
  }
}

void handleDelBg() {
  if (SPIFFS.exists("/bg.dat")) {
    SPIFFS.remove("/bg.dat");
  }
  server.send(200, "text/plain", "OK");
}

void setupServer() {
  server.on("/", handleRoot);
  // 诊断端点：返回当前状态，便于排查
  server.on("/ping", [](){
    server.send(200, "text/plain", "ok");
  });
  server.on("/f", [](){ manualActive=true; motorWifi(1); server.send(200); manualActive=false; });
  server.on("/b", [](){ manualActive=true; motorWifi(2); server.send(200); manualActive=false; });
  server.on("/l", [](){ manualActive=true; motorWifi(3); server.send(200); manualActive=false; });
  server.on("/r", [](){ manualActive=true; motorWifi(4); server.send(200); manualActive=false; });
  server.on("/s", [](){ manualActive=true; motorWifi(0); server.send(200); manualActive=false; });
  server.on("/mode_off",    [](){
    randomMode = RANDOM_OFF;
    oledAvailable = false;             // 休眠：关眼睛动画，loop 里改显示时间
    display.clearDisplay();
    server.send(200);
  });
  server.on("/mode_soft",   [](){
    randomMode = RANDOM_SOFT;
    oledAvailable = true;              // 恢复眼睛
    roboEyes.setCuriosity(false);
    roboEyes.setIdleMode(ON, 2, 2);
    roboEyes.setMood(HAPPY);
    roboEyes.open();
    server.send(200);
  });
  server.on("/mode_normal", [](){
    randomMode = RANDOM_NORMAL;
    oledAvailable = true;
    roboEyes.setCuriosity(true);
    roboEyes.setIdleMode(ON, 2, 2);
    roboEyes.setMood(DEFAULT);
    roboEyes.open();
    server.send(200);
  });
  server.onNotFound(handleRoot);
  server.on("/bg", HTTP_GET, handleUploadPage);
  server.on("/upload", HTTP_POST, handleUpload, handleUploadFile);
  server.on("/delbg", HTTP_POST, handleDelBg);
  server.on("/cfg", HTTP_GET, [](){
    String json = "{";
    json += "\"time\":" + String(cfg.showTime ? "true" : "false") + ",";
    json += "\"weekday\":" + String(cfg.showWeekday ? "true" : "false") + ",";
    json += "\"uptime\":" + String(cfg.showUptime ? "true" : "false") + ",";
    json += "\"moon\":" + String(cfg.showMoon ? "true" : "false") + ",";
    json += "\"weather\":" + String(cfg.showWeather ? "true" : "false");
    json += "}";
    server.send(200, "application/json", json);
  });
  server.on("/cfg", HTTP_POST, [](){
    if (server.hasArg("time")) cfg.showTime = server.arg("time") == "1";
    if (server.hasArg("weekday")) cfg.showWeekday = server.arg("weekday") == "1";
    if (server.hasArg("uptime")) cfg.showUptime = server.arg("uptime") == "1";
    if (server.hasArg("moon")) cfg.showMoon = server.arg("moon") == "1";
    if (server.hasArg("weather")) cfg.showWeather = server.arg("weather") == "1";
    saveConfig();
    server.send(200, "text/plain", "OK");
  });
  server.begin();
}

/* ================= SETUP ================= */
void setup() {
  delay(3000);
  Serial.begin(115200);
  delay(50);
  // 不降频。ESP32-C3 在 80MHz 下 I2C 驱动分频配置不稳，
  // 会导致 SSD1306 初始化失败 → 黑屏。保持默认 160MHz。
  // 省电请通过 WiFi.setSleep(true) 或 lightsleep 实现，不要降 CPU 频率。

  pinMode(STBY,OUTPUT); digitalWrite(STBY,LOW);
  pinMode(LF,OUTPUT); pinMode(LB,OUTPUT);
  pinMode(RF,OUTPUT); pinMode(RB,OUTPUT);
  pinMode(BOOT_BTN, INPUT_PULLUP);  // BOOT 键，按下=LOW

  // SPIFFS 初始化（存储背景图 + 配置）
  if (SPIFFS.begin(true)) {
    loadConfig();
  }

  // 频率直接传给 Wire.begin，避免 v3 setClock() 不生效的 bug
  Wire.begin(OLED_SDA, OLED_SCL, 400000);
  if (display.begin(SSD1306_SWITCHCAPVCC, 0x3C)) {
    display.clearDisplay();
    display.display();
    oledAvailable = true;
  }

  // 12 fps：减少刷新频率，单帧幅度更大才看起来不卡
  roboEyes.begin(SCREEN_WIDTH, SCREEN_HEIGHT, 12);
  roboEyes.setAutoblinker(ON, 3, 2);
  roboEyes.setIdleMode(ON, 2, 2);
  roboEyes.setMood(DEFAULT);

  // 小猫动画引擎：跟 RoboEyes 同样的 begin()/update() 用法
  // 10 fps + 指数缓动，走路/尾巴/呼吸全部丝滑
  sleepingCat.begin(SCREEN_WIDTH, SCREEN_HEIGHT, 10);
  sleepingCat.setBackgroundCallback(drawSky);  // 天空/天气/月相仍由 main.cpp 画

  // WiFi：异步连接，不阻塞 setup
  WiFi.mode(WIFI_STA);
  // 不调 setTxPower，用默认最大功率 19.5dBm
  // 之前用 8.5dBm 最低档，TCP 包太大在弱信号下握手丢包
  WiFi.begin(WIFI_SSID, WIFI_PASS);

  // 启动天气同步任务（独立 FreeRTOS 任务，5 小时一次）
  // 不再设置 wifiBusy，主 loop 自由刷屏
  xTaskCreate(weatherTask, "weather", 4096, NULL, 1, NULL);

  digitalWrite(STBY, HIGH);
}

/* ================= LOOP ================= */
void loop() {
  // 注意：weatherTask 不再设 wifiBusy，主 loop 不被阻塞
  // 偶尔有花屏可接受，黑屏不可接受

  // BOOT 键：连上 WiFi 时按下显示 IP（3 秒）
  static bool lastBtn = true;
  bool btn = digitalRead(BOOT_BTN);
  if (lastBtn == true && btn == false) {  // 下降沿（按下）
    if (WiFi.status() == WL_CONNECTED) {
      showIpUntil = millis() + 3000;
    }
  }
  lastBtn = btn;

  // 显示 IP（覆盖其他显示 3 秒）
  if (millis() < showIpUntil) {
    static unsigned long lastIpUpdate = 0;
    if (millis() - lastIpUpdate > 200) {
      lastIpUpdate = millis();
      display.clearDisplay();
      display.setTextSize(2);
      display.setTextColor(SSD1306_WHITE);
      display.setCursor(5, 5);
      display.println("IP:");
      display.setTextSize(1);
      display.setCursor(5, 30);
      display.println(WiFi.localIP());
      display.setCursor(5, 45);
      display.println("(boot to show)");
      safeDisplay();
    }
    return;  // 显示 IP 期间跳过其他显示
  }

  // 非休眠模式：最先刷眼睛，不被 WiFi 处理拖慢
  if (randomMode != RANDOM_OFF && oledAvailable) {
    roboEyes.update();
  }

  // 休眠模式
  if (randomMode == RANDOM_OFF) {
    bool hasBg = SPIFFS.exists("/bg.dat");
    if (hasBg) {
      // 背景图模式：1s 刷新（静态图 + 时钟，省 I2C 带宽）
      static unsigned long lastBgUpdate = 0;
      if (millis() - lastBgUpdate > 1000) {
        lastBgUpdate = millis();
        struct tm timeinfo;
        char timeStr[6];
        char fullStr[14];  // "7 12:34" 带周几（周日显示为 7 而不是 0）
        if (getLocalTime(&timeinfo)) {
          strftime(timeStr, sizeof(timeStr), "%H:%M", &timeinfo);
          int wday = timeinfo.tm_wday == 0 ? 7 : timeinfo.tm_wday;  // 周日 0→7
          snprintf(fullStr, sizeof(fullStr), "%d %s", wday, timeStr);
        } else {
          strcpy(timeStr, "--:--");
          strcpy(fullStr, "- --:--");
        }
        File f = SPIFFS.open("/bg.dat", "r");
        if (f) {
          uint8_t buf[1024];
          f.read(buf, 1024);
          f.close();
          display.clearDisplay();
          for (int y = 0; y < 64; y++) {
            for (int x = 0; x < 128; x++) {
              if (buf[(y / 8) * 128 + x] & (1 << (y % 8))) {
                display.drawPixel(x, y, SSD1306_WHITE);
              }
            }
          }
          // 左上：时间/周几（根据开关）
          if (cfg.showTime || cfg.showWeekday) {
            display.fillRect(0, 0, 50, 10, SSD1306_BLACK);
            display.setTextSize(1);
            display.setTextColor(SSD1306_WHITE);
            display.setCursor(1, 1);
            if (cfg.showWeekday && cfg.showTime) display.print(fullStr);
            else if (cfg.showWeekday) display.printf("%d ", timeinfo.tm_wday == 0 ? 7 : timeinfo.tm_wday);
            else display.print(timeStr);
          }
          // 右上：运行时间
          if (cfg.showUptime) {
            unsigned long upMin = millis() / 60000;
            display.fillRect(85, 0, 43, 10, SSD1306_BLACK);
            display.setCursor(86, 1);
            if (upMin < 60) display.printf("UP%dm", upMin);
            else display.printf("UP%dh", upMin / 60);
          }
          safeDisplay();
        }
      }
    } else {
      // 小猫动画：交给 SleepingCat 引擎，跟 RoboEyes 同样的 update() 用法。
      // 引擎内部自己限帧（10fps）、跑状态机、做指数缓动、画时钟叠加。
      // main.cpp 只负责每秒喂一次时间字符串 + 同步开关。
      static unsigned long lastTimeFeed = 0;
      if (millis() - lastTimeFeed > 1000) {
        lastTimeFeed = millis();
        struct tm timeinfo;
        char timeStr[8], weekdayStr[4], uptimeStr[8];
        if (getLocalTime(&timeinfo)) {
          strftime(timeStr, sizeof(timeStr), "%H:%M", &timeinfo);
          snprintf(weekdayStr, sizeof(weekdayStr), "%d",
                   timeinfo.tm_wday == 0 ? 7 : timeinfo.tm_wday);
        } else {
          strcpy(timeStr, "--:--");
          strcpy(weekdayStr, "-");
        }
        unsigned long upMin = millis() / 60000;
        if (upMin < 60) snprintf(uptimeStr, sizeof(uptimeStr), "UP%dm", (int)upMin);
        else            snprintf(uptimeStr, sizeof(uptimeStr), "UP%dh", (int)(upMin / 60));
        sleepingCat.setOverlay(cfg.showTime, cfg.showWeekday, cfg.showUptime);
        sleepingCat.setTimeInfo(timeStr, weekdayStr, uptimeStr);
      }
      sleepingCat.update();
    }
  }

  // 后台 WiFi 管理：未连接时每 10 秒尝试一次，5 秒内连不上就放弃
  static unsigned long lastWifiAttempt = 0;
  static bool wifiServerStarted = false;
  static unsigned long wifiAttemptStart = 0;
  static bool wifiAttempting = false;

  if (WiFi.status() == WL_CONNECTED) {
    if (!wifiServerStarted) {
      setupServer();
      wifiServerStarted = true;
      // 首次连上时同步 NTP 时间（东八区）
      configTime(8 * 3600, 0, "pool.ntp.org", "time.nist.gov");
    }
    // 天气同步由独立任务处理，这里不调
    server.handleClient();
  } else {
    // 断线后重置 server 状态
    if (wifiServerStarted) {
      wifiServerStarted = false;
      lastWifiAttempt = millis();  // 断线后从现在开始计时
    }
    // 每 10 秒尝试一次，最多等 5 秒
    if (!wifiAttempting) {
      if (millis() - lastWifiAttempt > 10000) {
        lastWifiAttempt = millis();
        wifiAttemptStart = millis();
        wifiAttempting = true;
        WiFi.disconnect();
        WiFi.begin(WIFI_SSID, WIFI_PASS);
      }
    } else {
      // 正在尝试，5 秒内没连上就放弃
      if (millis() - wifiAttemptStart > 5000) {
        wifiAttempting = false;
        WiFi.disconnect();
      }
    }
  }

  static unsigned long lastTick = 0;
  if (!manualActive && millis() - lastTick > 40) {
    lastTick = millis();
    if (randomMode == RANDOM_SOFT) {
      // 摇摆：只左右转（case 3=左转, case 4=右转），不前后
      if (random(120) == 1) {
        byte dir = random(2) ? 3 : 4;
        MOTOR(dir, random(6,18), random(40,90), 1);
      }
    }
    else if (randomMode == RANDOM_NORMAL) {
      // 好奇：左右走动（前进 case 2 + 左转 case 3 + 右转 case 4 混合）
      if (random(100) == 1) {
        byte dirs[] = {2, 3, 4};
        byte dir = dirs[random(3)];
        MOTOR(dir, random(5,50), random(10,100), random(20));
      }
    }
  }
}
