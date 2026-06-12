/* ============================================================================
   ClaudeSlate — Claude usage + clock + weather on a Waveshare ESP32-S3-RLCD-4.2
   (ST7305 400x300 reflective LCD). 3 pages, BOOT (GPIO0) cycles them:
     P1 HOME    — clock + current weather + Claude overview (today $ / 7d spark) + 5-day
     P2 CLAUDE  — tier table (5h/Week/Sonnet/Opus/Extra) + 7-day chart + spend + active
     P3 WEATHER — current (feels/hum/wind) + next-12h trend + 5-day range + room/comfort

   Data: claude_limits_proxy.py (Claude usage + Open-Meteo weather). Time via NTP;
   room temp/humidity from the onboard SHTC3; battery from ADC (GPIO4, no fuel gauge).

   DRIVER (not bundled): copy ST7305_U8g2.h / ST7305_U8g2.cpp NEXT TO this .ino from
   Waveshare's official demo  https://github.com/waveshareteam/ESP32-S3-RLCD-4.2
   (02_Example/Arduino/10_U8G2_Test/), and compile with the U8g2 from that repo
   (01_Arduino_Libraries) via arduino-cli --libraries. Black-on-white is done in
   software here (INVERT_DISPLAY), so the Waveshare driver stays unmodified.

   Board: ESP32S3 Dev Module | USB CDC On Boot: Enabled | Partition: Huge APP (3MB)
   | Flash: 16MB.
   ============================================================================ */
#include <WiFi.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <Wire.h>
#include <time.h>
#include "ST7305_U8g2.h"

// ---------------------- EDIT THESE / 改这里 ----------------------
// Factory defaults only — runtime config lives in NVS. If WiFi fails, the
// device opens a setup hotspot (captive portal) so you never need to reflash;
// the proxy is found via UDP broadcast, so its IP may change freely.
#define NET_DEF_SSID       "YOUR_WIFI_SSID"     // 2.4GHz only (ESP32-S3 has no 5GHz)
#define NET_DEF_PASS       "YOUR_WIFI_PASSWORD"
#define NET_DEF_PROXY_HOST "192.168.1.50"       // fallback when discovery fails
#define NET_DEF_PROXY_PORT 8787
#define NET_AP_NAME        "ClaudeSlate-Setup"  // setup hotspot name
#include "net_portal.h"
const uint32_t POLL_MS   = 60000;    // poll proxy
const uint32_t RENDER_MS = 10000;    // full redraw (incl. clock)
#define INVERT_DISPLAY 1             // 1 = black-on-white (software invert; stock driver)

// ---------------------- 引脚 ----------------------
#define RLCD_SCK 11
#define RLCD_MOSI 12
#define RLCD_DC 5
#define RLCD_CS 40
#define RLCD_RST 41
#define I2C_SDA 13
#define I2C_SCL 14
#define SHTC3_ADDR 0x70
#define BTN_PIN 0       // BOOT 键，翻页
#define W 400
#define H 300

static ST7305_U8g2 lcd(RLCD_SCK, RLCD_MOSI, RLCD_DC, RLCD_CS, RLCD_RST);
static U8G2* u = nullptr;

// ---------------------- 数据 ----------------------
struct WxDay { char d[6]; int c, hi, lo; };
struct Data {
  bool ok = false, stale = false;
  float sessionPct = 0, weekPct = 0;
  long  sessionReset = 0, weekReset = 0, sonnetReset = -1, opusReset = -1, extraReset = -1, activeIdle = -1;
  float sonnetPct = -1, opusPct = -1, extraPct = -1; bool extraEnabled = false;
  float costToday = 0, cost7d = 0, cost30d = 0;
  int64_t tokToday = 0, tokIn = 0, tokOut = 0, activeTok = 0, weekTok = 0, tok7d[7] = {0};
  int   activeMsgs = 0;
  char  plan[16] = "", activeProj[24] = "";
  int   wxNowT = 0, wxNowC = -1, wxFeels = 0, wxHum = 0, wxWind = 0;
  int   h12t[12] = {0}, h12p[12] = {0}, nH12 = 0;
  char  wxCity[16] = "";
  WxDay days[5]; int nDays = 0;
  float roomT = 0, roomH = 0; bool roomOk = false;
  uint32_t fetchMs = 0;
} g;
uint32_t lastPoll = 0, lastRender = 0;
bool timeOK = false;
int  page = 0;                                      // 0=主页 1=Claude详情 2=天气&室内；BOOT 翻页
int  lastBtn = HIGH; uint32_t btnDownMs = 0;
uint32_t lastOnline = 0; int pollFails = 0;

// ---------------------- SHTC3(标准 Wire) ----------------------
bool shtc3Read(float& t, float& h) {
  Wire.beginTransmission(SHTC3_ADDR); Wire.write(0x35); Wire.write(0x17);   // wakeup
  if (Wire.endTransmission() != 0) return false;
  delayMicroseconds(300);
  Wire.beginTransmission(SHTC3_ADDR); Wire.write(0x78); Wire.write(0x66);   // 测温先、无时钟拉伸
  if (Wire.endTransmission() != 0) return false;
  delay(15);
  if (Wire.requestFrom(SHTC3_ADDR, 6) != 6) return false;
  uint8_t b[6]; for (int i = 0; i < 6; i++) b[i] = Wire.read();
  uint16_t tr = (b[0] << 8) | b[1], hr = (b[3] << 8) | b[4];
  t = -45.0f + 175.0f * tr / 65535.0f;
  h = 100.0f * hr / 65535.0f;
  Wire.beginTransmission(SHTC3_ADDR); Wire.write(0xB0); Wire.write(0x98); Wire.endTransmission(); // sleep
  return true;
}

// ---------------------- 电量 (ADC GPIO4，1/3 分压；本板无电量计芯片) ----------------------
#define BAT_ADC_PIN 4                              // ESP32-S3 ADC1_CH3 = GPIO4 (官方 demo 用法)
int g_bat = -1;
int readBattery() {                                // 电池电压(mV)=引脚×3 → 3.00V=0% 4.12V=100%
  long sum = 0; int n = 0;
  for (int i = 0; i < 8; i++) { int mv = analogReadMilliVolts(BAT_ADC_PIN); if (mv > 0) { sum += mv; n++; } delay(2); }
  if (n == 0) return g_bat;
  int mv = (int)(sum / n) * 3;                     // 还原 1/3 分压
  if (mv < 2500 || mv > 4600) return g_bat;        // 没接电池/异常 → 保持上次(-1=不画)
  float v = mv / 1000.0;
  int pct = (v < 3.0) ? 0 : (v > 4.12 ? 100 : (int)round((v - 3.0) / 1.12 * 100));
  if (g_bat >= 0 && abs(pct - g_bat) < 2) return g_bat;   // ±1 抖动不动
  return pct;
}

// ---------------------- 格式化(第2页用) ----------------------
String pctStr(float p) { if (p < 0) return "--"; char b[8]; snprintf(b, sizeof(b), "%.0f%%", p); return String(b); }
String humanTok(int64_t t) { char b[16];
  if (t >= 1000000) snprintf(b, sizeof(b), "%.1fM", t / 1000000.0);
  else if (t >= 1000) snprintf(b, sizeof(b), "%.0fK", t / 1000.0);
  else snprintf(b, sizeof(b), "%lld", (long long)t); return String(b); }
String fmtReset(long s) { char b[16];
  if (s <= 0) return "now";
  if (s < 3600) snprintf(b, sizeof(b), "%ldm", s / 60);
  else if (s < 86400) snprintf(b, sizeof(b), "%ldh %ldm", s / 3600, (s % 3600) / 60);
  else snprintf(b, sizeof(b), "%ldd %ldh", s / 86400, (s % 86400) / 3600); return String(b); }
String fmtIdle(long s) { char b[12];
  if (s < 0) return "?";
  if (s < 60) snprintf(b, sizeof(b), "%lds", s);
  else if (s < 3600) snprintf(b, sizeof(b), "%ldm", s / 60);
  else snprintf(b, sizeof(b), "%ldh", s / 3600); return String(b); }
long liveReset(long base) { long r = base - (long)((millis() - g.fetchMs) / 1000); return r < 0 ? 0 : r; }

// ---------------------- 绘制小工具 ----------------------
void strAt(int x, int y, const char* s) { u->drawStr(x, y, s); }
void strCenter(int cx, int y, const char* s) { u->drawStr(cx - u->getStrWidth(s) / 2, y, s); }
void strRight(int rx, int y, const char* s) { u->drawStr(rx - u->getStrWidth(s), y, s); }
// 数字 + 小圆圈度数, 居中于 cx, 基线 y
void tempCenter(int cx, int y, int v) {
  char b[8]; snprintf(b, sizeof(b), "%d", v);
  int w = u->getStrWidth(b);
  int x = cx - (w + 6) / 2;
  u->drawStr(x, y, b);
  u->drawCircle(x + w + 3, y - u->getAscent() + 1, 2);
}

// 天气码 -> 图标类型
enum { ICON_SUN, ICON_PCLOUD, ICON_CLOUD, ICON_FOG, ICON_RAIN, ICON_SNOW, ICON_THUNDER };
int wxType(int c) {
  if (c <= 0) return ICON_SUN;
  if (c <= 2) return ICON_PCLOUD;
  if (c == 3) return ICON_CLOUD;
  if (c == 45 || c == 48) return ICON_FOG;
  if ((c >= 71 && c <= 77) || c == 85 || c == 86) return ICON_SNOW;
  if (c >= 95) return ICON_THUNDER;
  if ((c >= 51 && c <= 67) || (c >= 80 && c <= 82)) return ICON_RAIN;
  return ICON_CLOUD;
}
void drawCloud(int cx, int cy, int s) {           // 填充云朵
  u->drawDisc(cx - s, cy + s / 2, s * 6 / 10);
  u->drawDisc(cx + s, cy + s / 2, s * 6 / 10);
  u->drawDisc(cx, cy - s / 3, s);
  u->drawBox(cx - s - s * 6 / 10, cy + s / 2 - s * 6 / 10 + 1, (s + s * 6 / 10) * 2, s * 6 / 10);
}
void drawSun(int cx, int cy, int r) {
  u->drawDisc(cx, cy, r);
  for (int i = 0; i < 8; i++) {
    float a = i * PI / 4;
    u->drawLine(cx + (r + 2) * cos(a), cy + (r + 2) * sin(a), cx + (r + 6) * cos(a), cy + (r + 6) * sin(a));
  }
}
void drawWx(int cx, int cy, int type) {           // 图标中心 (cx,cy)
  switch (type) {
    case ICON_SUN: drawSun(cx, cy, 9); break;
    case ICON_PCLOUD: drawSun(cx - 7, cy - 6, 6); drawCloud(cx + 3, cy + 3, 7); break;
    case ICON_CLOUD: drawCloud(cx, cy, 9); break;
    case ICON_FOG:
      for (int i = 0; i < 4; i++) u->drawHLine(cx - 12, cy - 6 + i * 5, 24);
      break;
    case ICON_RAIN:
      drawCloud(cx, cy - 4, 8);
      for (int i = -1; i <= 1; i++) u->drawLine(cx + i * 6, cy + 8, cx + i * 6 - 2, cy + 12);
      break;
    case ICON_SNOW:
      drawCloud(cx, cy - 4, 8);
      for (int i = -1; i <= 1; i++) { u->drawDisc(cx + i * 7, cy + 11, 1); }
      break;
    case ICON_THUNDER:
      drawCloud(cx, cy - 4, 8);
      u->drawTriangle(cx - 1, cy + 6, cx + 5, cy + 6, cx - 3, cy + 16);
      u->drawTriangle(cx + 3, cy + 6, cx - 1, cy + 14, cx + 5, cy + 8);
      break;
  }
}

// 分段进度条
void bar(int x, int y, int w, int h, float frac) {
  u->drawFrame(x, y, w, h);
  int n = (int)round(constrain(frac, 0.0f, 1.0f) * (w - 4));
  if (n > 0) u->drawBox(x + 2, y + 2, n, h - 4);
}
// 电量图标：整组右对齐到 rx，返回左边界。电池框 + 电量条 + 右侧 %
int drawBatteryRight(int rx, int yTop, int pct) {
  if (pct < 0) return rx;
  char b[6]; snprintf(b, sizeof(b), "%d%%", pct);
  u->setFont(u8g2_font_6x13_tf);
  int total = 24 + 4 + u->getStrWidth(b);          // 电池(框22+正极头2) + 间距 + %
  int bx = rx - total;
  u->drawFrame(bx, yTop, 22, 11);
  u->drawBox(bx + 22, yTop + 3, 2, 5);             // 正极头
  int fw = (int)round(pct / 100.0 * 18);
  if (fw > 0) u->drawBox(bx + 2, yTop + 2, fw, 7);
  u->drawStr(bx + 28, yTop + 10, b);
  return bx;
}

// 黑字白底：整帧像素取反后再送(配合未改动的官方驱动，其默认 0x21=反显开)
void flush() {
#if INVERT_DISPLAY
  uint8_t* bp = u->getBufferPtr();
  int n = u->getBufferTileWidth() * u->getBufferTileHeight() * 8;
  for (int i = 0; i < n; i++) bp[i] ^= 0xFF;
#endif
  (*u).sendBuffer();
}

// ---------------------- 渲染辅助 ----------------------
#define FS u8g2_font_helvR08_tf                     // 小号字(单位/副标/倒计时)
const char* wxText(int c) {
  if (c < 0) return "";
  if (c <= 0) return "Clear";
  if (c <= 2) return "P.Cloudy";
  if (c == 3) return "Cloudy";
  if (c == 45 || c == 48) return "Fog";
  if ((c >= 71 && c <= 77) || c == 85 || c == 86) return "Snow";
  if (c >= 95) return "Storm";
  if ((c >= 51 && c <= 67) || (c >= 80 && c <= 82)) return "Rain";
  return "Cloudy";
}
const char* comfort(float t, float h) {
  if (h < 35) return "DRY";
  if (h > 65) return "HUMID";
  if (t > 26) return "WARM";
  if (t < 19) return "COOL";
  return "OK";
}
int64_t maxTok7() { int64_t m = 0; for (int i = 0; i < 7; i++) if (g.tok7d[i] > m) m = g.tok7d[i]; return m; }
// 7 根柱(按最大值归一)；today 列空心高亮
void spark7(int x, int yBase, int pitch, int hMax, int todayIdx) {
  int64_t mx = maxTok7(); if (mx < 1) mx = 1;
  for (int i = 0; i < 7; i++) {
    int bx = x + i * pitch, bw = pitch - 2;
    int bh = (int)((double)hMax * g.tok7d[i] / mx); if (bh < 1 && g.tok7d[i] > 0) bh = 1;
    if (bh <= 0) continue;
    if (i == todayIdx) u->drawFrame(bx, yBase - bh, bw, bh);
    else u->drawBox(bx, yBase - bh, bw, bh);
  }
}
// 逐时温度折线 + 降水点
void hourly12(int x, int y, int w, int h) {
  int n = g.nH12; if (n < 2) return;
  int mn = g.h12t[0], mx = g.h12t[0];
  for (int i = 1; i < n; i++) { if (g.h12t[i] < mn) mn = g.h12t[i]; if (g.h12t[i] > mx) mx = g.h12t[i]; }
  if (mx == mn) mx = mn + 1;
  int pitch = w / (n - 1);
  for (int i = 0; i < n - 1; i++) {
    int y0 = y + h - (g.h12t[i]     - mn) * h / (mx - mn);
    int y1 = y + h - (g.h12t[i + 1] - mn) * h / (mx - mn);
    u->drawLine(x + i * pitch, y0, x + (i + 1) * pitch, y1);
  }
  for (int i = 0; i < n; i++) if (g.h12p[i] > 0) u->drawDisc(x + i * pitch, y + h + 6, g.h12p[i] > 50 ? 2 : 1);
}
// 浮动温度区间条：lo..hi 映射到固定窗(0..40°C)
void rangeBar(int x, int y, int trackW, int h, int lo, int hi) {
  u->drawFrame(x, y, trackW, h);
  int xl = x + constrain(lo, 0, 40) * trackW / 40;
  int xh = x + constrain(hi, 0, 40) * trackW / 40;
  if (xh <= xl) xh = xl + 1;
  u->drawBox(xl, y + 1, xh - xl, h - 2);
}

// ---------------------- 渲染 ----------------------
// 底部公共栏：室温(+舒适度) + 状态点 + 电量。三页共用
void drawBottom(bool haveT, struct tm* t) {
  u->drawHLine(8, 258, W - 16);
  u->setFont(u8g2_font_6x13_tf);
  char room[48];
  if (g.roomOk && page == 2) snprintf(room, sizeof(room), "Room %.1fC %.0f%%RH  %s", g.roomT, g.roomH, comfort(g.roomT, g.roomH));
  else if (g.roomOk)         snprintf(room, sizeof(room), "Room %.1fC %.0f%%RH", g.roomT, g.roomH);
  else                       snprintf(room, sizeof(room), "Room --");
  u->drawStr(10, 276, room);
  int batL = drawBatteryRight(W - 12, 265, g_bat);
  const char* sl = !g.ok ? "no data" : (g.stale ? "stale" : "live");
  bool live = g.ok && !g.stale;
  u->setFont(u8g2_font_6x13_tf);
  int sx = batL - 16 - u->getStrWidth(sl);
  u->drawStr(sx, 276, sl);
  if (live) u->drawDisc(sx - 9, 271, 3); else u->drawCircle(sx - 9, 271, 3);
}

// 第1页：时钟 + 当前天气 + Claude概览(含今日花费/7天) + 5天预报
void renderPage1(bool haveT, struct tm* t) {
  char hh[4] = "--", mm[4] = "--";
  if (haveT) { snprintf(hh, sizeof(hh), "%02d", t->tm_hour); snprintf(mm, sizeof(mm), "%02d", t->tm_min); }
  u->setFont(u8g2_font_logisoso32_tn);
  u->drawStr(8, 40, hh);
  int colX = 8 + u->getStrWidth(hh) + 6;
  u->drawBox(colX, 17, 4, 4); u->drawBox(colX, 31, 4, 4);
  u->drawStr(colX + 10, 40, mm);
  u->setFont(FS);
  if (haveT) {
    const char* wd[] = {"SUN","MON","TUE","WED","THU","FRI","SAT"};
    const char* mo[] = {"Jan","Feb","Mar","Apr","May","Jun","Jul","Aug","Sep","Oct","Nov","Dec"};
    char ds[24]; snprintf(ds, sizeof(ds), "%s . %s %d", wd[t->tm_wday], mo[t->tm_mon], t->tm_mday);
    u->drawStr(10, 54, ds);
  }
  u->drawVLine(150, 6, 48);
  if (g.wxNowC >= 0) {                              // 当前天气(右上)
    drawWx(176, 26, wxType(g.wxNowC));
    u->setFont(u8g2_font_logisoso20_tn);
    char nt[6]; snprintf(nt, sizeof(nt), "%d", g.wxNowT);
    u->drawStr(196, 36, nt);
    int tw = u->getStrWidth(nt);
    u->drawCircle(196 + tw + 4, 22, 2);
    u->setFont(FS);
    char fl[12]; snprintf(fl, sizeof(fl), "feels %d", g.wxFeels); u->drawStr(248, 24, fl);
    char w2[28]; snprintf(w2, sizeof(w2), "%s  %d%%RH %dkm/h", wxText(g.wxNowC), g.wxHum, g.wxWind);
    u->drawStr(196, 50, w2);
  }
  u->drawHLine(8, 60, W - 16); u->drawHLine(8, 61, W - 16);

  u->setFont(u8g2_font_helvB12_tf);                 // Claude 概览
  u->drawStr(10, 78, "CLAUDE");
  u->setFont(FS);
  { String pl = String(g.plan); pl.toUpperCase(); u->drawStr(82, 78, pl.c_str()); }
  char cost[12]; snprintf(cost, sizeof(cost), "~$%.2f", g.costToday);
  u->setFont(u8g2_font_helvB12_tf); strRight(W - 10, 78, cost);
  int cw = u->getStrWidth(cost);
  u->setFont(u8g2_font_6x13_tf);
  strRight(W - 10 - cw - 10, 78, (String("today ") + humanTok(g.tokToday) + " tok").c_str());
  u->drawStr(10, 97, "SESS"); bar(56, 87, 130, 11, g.sessionPct / 100.0);
  u->drawStr(196, 97, pctStr(g.sessionPct).c_str());
  u->setFont(FS); strRight(W - 10, 97, (String("resets ") + fmtReset(liveReset(g.sessionReset))).c_str());
  u->setFont(u8g2_font_6x13_tf);
  u->drawStr(10, 113, "WEEK"); bar(56, 103, 130, 11, g.weekPct / 100.0);
  u->drawStr(196, 113, pctStr(g.weekPct).c_str());
  u->setFont(FS); strRight(W - 10, 113, (String("resets ") + fmtReset(liveReset(g.weekReset))).c_str());
  u->drawStr(10, 131, "7d"); spark7(30, 132, 12, 15, 6);
  char wk[40]; snprintf(wk, sizeof(wk), "wk %s   pk %s", humanTok(g.weekTok).c_str(), humanTok(maxTok7()).c_str());
  strRight(W - 10, 129, wk);
  u->drawHLine(8, 137, W - 16); u->drawHLine(8, 138, W - 16);

  for (int i = 0; i < g.nDays && i < 5; i++) {      // 5天预报
    int cx = 40 + i * 80;
    u->setFont(FS); strCenter(cx, 154, g.days[i].d);
    drawWx(cx, 176, wxType(g.days[i].c));
    u->setFont(u8g2_font_helvB12_tf); tempCenter(cx, 213, g.days[i].hi);
    u->setFont(FS); tempCenter(cx, 229, g.days[i].lo);
    if (i) u->drawVLine(i * 80, 144, 94);
  }
}

// 第2页：Claude 详情(档位表 + 7天柱图 + 今日 + 活跃)
void renderPage2(bool haveT, struct tm* t) {
  u->setFont(u8g2_font_helvB12_tf);
  u->drawStr(10, 22, "CLAUDE DETAILS");
  u->setFont(FS);
  { String pl = String(g.plan); pl.toUpperCase();
    char hdr[28]; snprintf(hdr, sizeof(hdr), "%s   ~$%.2f today", pl.c_str(), g.costToday); strRight(W - 10, 20, hdr); }
  u->drawHLine(8, 28, W - 16); u->drawHLine(8, 29, W - 16);
  if (!g.ok) { u->setFont(u8g2_font_helvB12_tf); strCenter(W / 2, 150, "proxy: no data"); return; }

  u->setFont(FS);                                   // 表头
  u->drawStr(8, 44, "TIER"); u->drawStr(64, 44, "USAGE"); u->drawStr(196, 44, "PCT"); strRight(W - 10, 44, "RESETS IN");
  const int BX = 64, BW = 110;
  const char* nm[5] = {"5h ses", "Week", "Sonnet", "Opus", "Extra"};
  float pc[5] = {g.sessionPct, g.weekPct, g.sonnetPct, g.opusPct, g.extraPct};
  long  rs[5] = {liveReset(g.sessionReset), liveReset(g.weekReset), liveReset(g.sonnetReset), liveReset(g.opusReset), liveReset(g.extraReset)};
  for (int i = 0; i < 5; i++) {
    int y = 60 + i * 18;
    u->setFont(u8g2_font_6x13_tf);
    u->drawStr(8, y, nm[i]);
    if (pc[i] >= 0) bar(BX, y - 10, BW, 12, pc[i] / 100.0);
    u->drawStr(196, y, pctStr(pc[i]).c_str());
    u->setFont(FS);
    if (i == 4) u->drawStr(BX + BW + 3, y, g.extraEnabled ? "on" : "off");
    if (pc[i] >= 0 && rs[i] >= 0) strRight(W - 10, y, fmtReset(rs[i]).c_str());
  }
  u->drawHLine(8, 142, W - 16);

  u->setFont(FS);                                   // 7天柱图
  u->drawStr(10, 156, "7-DAY TOKENS");
  char ch[40]; snprintf(ch, sizeof(ch), "wk %s  avg %s  pk %s",
                        humanTok(g.weekTok).c_str(), humanTok(g.weekTok / 7).c_str(), humanTok(maxTok7()).c_str());
  strRight(W - 10, 156, ch);
  spark7(44, 192, 30, 30, 6);
  if (haveT) {
    const char* wd[] = {"S","M","T","W","T","F","S"};
    for (int i = 0; i < 7; i++) {
      int dow = (t->tm_wday - (6 - i) + 7) % 7;
      strCenter(44 + i * 30 + 14, 202, wd[dow]);
    }
  }
  u->drawHLine(8, 208, W - 16);

  u->setFont(u8g2_font_6x13_tf);                    // 花费:今日 / 7天 / 30天 美元
  char sp[60]; snprintf(sp, sizeof(sp), "SPEND  today ~$%.0f   7d ~$%.0f   30d ~$%.0f", g.costToday, g.cost7d, g.cost30d);
  u->drawStr(10, 222, sp);
  u->drawStr(10, 237, (String("TODAY ") + humanTok(g.tokToday) + " tok  in " + humanTok(g.tokIn) + " out " + humanTok(g.tokOut)).c_str());
  u->setFont(FS);                                   // 活跃会话(一行)
  u->drawStr(10, 252, (String("ACTIVE ") + g.activeProj + "   " + String(g.activeMsgs) + " msg  idle " + fmtIdle(g.activeIdle)).c_str());
}

// 第3页：天气 & 室内
void renderPage3(bool haveT, struct tm* t) {
  u->setFont(u8g2_font_helvB12_tf);
  u->drawStr(10, 22, (String("WEATHER  ") + (g.wxCity[0] ? g.wxCity : "")).c_str());
  u->setFont(FS);
  if (haveT) {
    const char* wd[] = {"SUN","MON","TUE","WED","THU","FRI","SAT"};
    const char* mo[] = {"Jan","Feb","Mar","Apr","May","Jun","Jul","Aug","Sep","Oct","Nov","Dec"};
    char ds[28]; snprintf(ds, sizeof(ds), "%s %s %d  %02d:%02d", wd[t->tm_wday], mo[t->tm_mon], t->tm_mday, haveT ? t->tm_hour : 0, haveT ? t->tm_min : 0);
    strRight(W - 10, 20, ds);
  }
  u->drawHLine(8, 28, W - 16); u->drawHLine(8, 29, W - 16);
  if (g.wxNowC < 0) { u->setFont(u8g2_font_helvB12_tf); strCenter(W / 2, 130, "no weather data"); return; }

  drawWx(42, 62, wxType(g.wxNowC));                 // 当前
  u->setFont(u8g2_font_logisoso32_tn);
  char nt[6]; snprintf(nt, sizeof(nt), "%d", g.wxNowT);
  u->drawStr(80, 72, nt);
  int tw = u->getStrWidth(nt);
  u->setFont(u8g2_font_helvB12_tf);
  u->drawCircle(80 + tw + 6, 46, 3);
  u->drawStr(80 + tw + 14, 58, wxText(g.wxNowC));
  u->setFont(FS);
  char l1[14], l2[14], l3[16];
  snprintf(l1, sizeof(l1), "feels %d", g.wxFeels);
  snprintf(l2, sizeof(l2), "hum %d%%", g.wxHum);
  snprintf(l3, sizeof(l3), "wind %dkm/h", g.wxWind);
  u->drawStr(255, 48, l1); u->drawStr(255, 62, l2); u->drawStr(255, 76, l3);
  u->drawHLine(8, 88, W - 16);

  u->setFont(FS);                                   // 逐时 12 小时
  u->drawStr(10, 102, "NEXT 12H");
  if (g.nH12 >= 2) {
    hourly12(80, 94, 300, 20);                          // (读全局 h12t/h12p/nH12)
    char e0[6], eN[6];
    snprintf(e0, sizeof(e0), "%d", g.h12t[0]); snprintf(eN, sizeof(eN), "%d", g.h12t[g.nH12 - 1]);
    u->drawStr(80, 102, e0); strRight(W - 10, 102, eN);
  }
  u->drawHLine(8, 126, W - 16);

  u->setFont(FS);                                   // 5天区间
  u->drawStr(10, 140, "5-DAY"); strRight(W - 10, 140, "hi / lo");
  for (int i = 0; i < g.nDays && i < 5; i++) {
    int y = 156 + i * 19;
    u->setFont(FS); u->drawStr(10, y, g.days[i].d);
    rangeBar(70, y - 9, 220, 11, g.days[i].lo, g.days[i].hi);
    u->setFont(u8g2_font_6x13_tf);
    char hl[12]; snprintf(hl, sizeof(hl), "%d / %d", g.days[i].hi, g.days[i].lo);
    strRight(W - 10, y, hl);
  }
}

void render() {
  u->clearBuffer();
  u->setDrawColor(1);
  g_bat = readBattery();                            // ADC 读电池电压→%
  struct tm t; bool haveT = timeOK && getLocalTime(&t, 50);
  if (page == 0)      renderPage1(haveT, &t);
  else if (page == 1) renderPage2(haveT, &t);
  else                renderPage3(haveT, &t);
  drawBottom(haveT, &t);
  flush();
}

// ---------------------- 拉数据 ----------------------
bool poll() {
  if (WiFi.status() != WL_CONNECTED) return false;
  HTTPClient http; http.setConnectTimeout(8000); http.setTimeout(8000);
  if (!http.begin(netProxyUrl())) return false;
  int code = http.GET();
  if (code != 200) { http.end(); g.ok = false; return false; }
  String payload = http.getString(); http.end();
  JsonDocument doc;
  if (deserializeJson(doc, payload)) { g.ok = false; return false; }
  g.ok = doc["ok"] | false;
  g.stale = doc["stale"] | false;
  g.sessionPct = doc["session_pct"] | 0.0;
  g.weekPct = doc["week_pct"] | 0.0;
  g.sessionReset = doc["session_reset_s"] | 0L;
  g.weekReset    = doc["week_reset_s"] | 0L;
  g.sonnetPct    = doc["sonnet_pct"] | -1.0;
  g.opusPct      = doc["opus_pct"] | -1.0;
  g.extraPct     = doc["extra_pct"] | -1.0;
  g.extraEnabled = doc["extra_enabled"] | false;
  g.sonnetReset  = doc["sonnet_reset_s"] | -1L;
  g.opusReset    = doc["opus_reset_s"] | -1L;
  g.extraReset   = doc["extra_reset_s"] | -1L;
  g.costToday    = doc["cost_today_usd"] | 0.0;
  g.cost7d       = doc["cost_7d_usd"] | 0.0;
  g.cost30d      = doc["cost_30d_usd"] | 0.0;
  g.tokToday     = doc["tok_today"].as<int64_t>();
  g.tokIn        = doc["tok_today_in"].as<int64_t>();
  g.tokOut       = doc["tok_today_out"].as<int64_t>();
  g.weekTok      = doc["week_tok"].as<int64_t>();
  { JsonArray a = doc["tok_7d"]; int i = 0; for (JsonVariant v : a) { if (i >= 7) break; g.tok7d[i++] = v.as<int64_t>(); } }
  g.activeTok    = doc["active_tok"].as<int64_t>();
  g.activeMsgs   = doc["active_msgs"] | 0;
  g.activeIdle   = doc["active_idle_s"] | -1L;
  strlcpy(g.plan, doc["plan"] | "", sizeof(g.plan));
  strlcpy(g.activeProj, doc["active_project"] | "?", sizeof(g.activeProj));
  JsonObject wx = doc["weather"];
  if (!wx.isNull()) {
    g.wxNowT  = wx["now_t"] | 0;
    g.wxNowC  = wx["now_c"] | -1;
    g.wxFeels = wx["now_feels"] | g.wxNowT;
    g.wxHum   = wx["now_hum"] | 0;
    g.wxWind  = wx["now_wind"] | 0;
    strlcpy(g.wxCity, wx["city"] | "", sizeof(g.wxCity));
    JsonArray h = wx["hourly_12h"]; g.nH12 = 0;
    for (JsonObject o : h) { if (g.nH12 >= 12) break; g.h12t[g.nH12] = o["t"] | 0; g.h12p[g.nH12] = o["p"] | 0; g.nH12++; }
    JsonArray ds = wx["days"];
    g.nDays = 0;
    for (JsonObject d : ds) {
      if (g.nDays >= 5) break;
      strlcpy(g.days[g.nDays].d, d["d"] | "", sizeof(g.days[g.nDays].d));
      g.days[g.nDays].c = d["c"] | 0;
      g.days[g.nDays].hi = d["hi"] | 0;
      g.days[g.nDays].lo = d["lo"] | 0;
      g.nDays++;
    }
  }
  g.fetchMs = millis();
  return true;
}

void showNetStatus(const char* l1, const char* l2, const char* l3) {
  u->clearBuffer();
  u->setFont(u8g2_font_helvB12_tf);
  strCenter(W / 2, H / 2 - 24, l1);
  u->setFont(u8g2_font_6x13_tf);
  if (l2) strCenter(W / 2, H / 2 + 2, l2);
  if (l3) strCenter(W / 2, H / 2 + 22, l3);
  flush();
}

void showSetupScreen() {
  char l2[40]; snprintf(l2, sizeof(l2), "join WiFi: %s", NET_AP_NAME);
  showNetStatus("SETUP MODE", l2, "then open http://192.168.4.1");
}

// Boot-time connect: on failure, drop into the captive portal
// (portal reboots on save or after a 5-minute timeout — never returns)
void connectWiFi() {
  showNetStatus("connecting wifi...", netCfg.ssid.c_str(), nullptr);
  if (netConnect(20000)) { netDiscoverProxy(2); return; }
  showSetupScreen();
  netStartPortal();
}

// ---------------------- setup / loop ----------------------
void setup() {
  Serial.begin(115200);
  pinMode(BTN_PIN, INPUT_PULLUP);                  // BOOT 键翻页
  Wire.begin(I2C_SDA, I2C_SCL);
  lcd.begin(0, U8G2_R1);
  u = lcd.getU8g2();

  u->clearBuffer(); u->setFont(u8g2_font_helvB12_tf);
  strCenter(W / 2, H / 2, "Claude RLCD booting...");
  flush();

  netBegin();
  connectWiFi();
  configTime(8 * 3600, 0, "ntp.aliyun.com", "ntp.tencent.com");
  struct tm t; timeOK = getLocalTime(&t, 6000);
  g.roomOk = shtc3Read(g.roomT, g.roomH);
  poll();
  render();
  lastPoll = lastRender = millis();
}

void loop() {
  uint32_t now = millis();

  // BOOT key: short press = next page, hold 3s = setup portal
  int b = digitalRead(BTN_PIN);
  if (lastBtn == HIGH && b == LOW) btnDownMs = now;
  if (b == LOW && btnDownMs && now - btnDownMs >= 3000) {
    showSetupScreen();
    netStartPortal();                              // never returns (reboots on save/timeout)
  }
  if (lastBtn == LOW && b == HIGH) {
    if (btnDownMs && now - btnDownMs >= 30 && now - btnDownMs < 1000) {
      page = (page + 1) % 3; render(); lastRender = now;
    }
    btnDownMs = 0;
  }
  lastBtn = b;

  if (now - lastPoll >= POLL_MS) {
    if (WiFi.status() != WL_CONNECTED) {
      WiFi.reconnect();
      if (now - lastOnline > 300000UL) ESP.restart();  // offline 5 min -> reboot (falls into portal if WiFi changed)
    } else {
      lastOnline = now;
      if (poll()) pollFails = 0;
      else if (++pollFails >= 3) {                 // proxy IP may have moved -> rediscover
        netDiscoverProxy(2);
        pollFails = 0;
      }
    }
    g.roomOk = shtc3Read(g.roomT, g.roomH);
    render();
    lastPoll = lastRender = now;
  } else if (now - lastRender >= RENDER_MS) {
    render();
    lastRender = now;
  }
  delay(20);
}
