/* ============================================================================
   ClaudeSlate — Claude usage + clock + weather on a Waveshare ESP32-S3-RLCD-4.2
   (ST7305 400x300 reflective LCD). Top: clock + date + today's weather.
   Middle: Claude SESSION / WEEKLY. Bottom: 5-day forecast + room temp/humidity.

   Data: claude_limits_proxy.py (Claude usage + Open-Meteo weather). Time via NTP;
   room temp/humidity from the onboard SHTC3.

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
const char* WIFI_SSID = "YOUR_WIFI_SSID";       // 2.4GHz only (ESP32-S3 has no 5GHz)
const char* WIFI_PASS = "YOUR_WIFI_PASSWORD";
const char* PROXY_URL = "http://192.168.1.50:8787/usage";   // your PC's LAN IP
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
#define W 400
#define H 300

static ST7305_U8g2 lcd(RLCD_SCK, RLCD_MOSI, RLCD_DC, RLCD_CS, RLCD_RST);
static U8G2* u = nullptr;

// ---------------------- 数据 ----------------------
struct WxDay { char d[6]; int c, hi, lo; };
struct Data {
  bool ok = false, stale = false;
  float sessionPct = 0, weekPct = 0;
  int  wxNowT = 0, wxNowC = -1;
  WxDay days[5]; int nDays = 0;
  float roomT = 0, roomH = 0; bool roomOk = false;
} g;
uint32_t lastPoll = 0, lastRender = 0;
bool timeOK = false;

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

// ---------------------- 绘制小工具 ----------------------
void strAt(int x, int y, const char* s) { u->drawStr(x, y, s); }
void strCenter(int cx, int y, const char* s) { u->drawStr(cx - u->getStrWidth(s) / 2, y, s); }
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
      for (int i = -1; i <= 1; i++) u->drawLine(cx + i * 7, cy + 10, cx + i * 7 - 2, cy + 16);
      break;
    case ICON_SNOW:
      drawCloud(cx, cy - 4, 8);
      for (int i = -1; i <= 1; i++) { u->drawDisc(cx + i * 8, cy + 13, 2); }
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

// 黑字白底：整帧像素取反后再送(配合未改动的官方驱动，其默认 0x21=反显开)
void flush() {
#if INVERT_DISPLAY
  uint8_t* bp = u->getBufferPtr();
  int n = u->getBufferTileWidth() * u->getBufferTileHeight() * 8;
  for (int i = 0; i < n; i++) bp[i] ^= 0xFF;
#endif
  (*u).sendBuffer();
}

// ---------------------- 渲染 ----------------------
void render() {
  u->clearBuffer();
  u->setDrawColor(1);
  struct tm t; bool haveT = timeOK && getLocalTime(&t, 50);

  // ---- 顶部：时钟 + 日期 + 今日天气 ----
  char hh[4] = "--", mm[4] = "--";
  if (haveT) { snprintf(hh, sizeof(hh), "%02d", t.tm_hour); snprintf(mm, sizeof(mm), "%02d", t.tm_min); }
  u->setFont(u8g2_font_logisoso32_tn);
  int cx0 = 10, cy0 = 44;
  u->drawStr(cx0, cy0, hh);
  int wHH = u->getStrWidth(hh);
  int colX = cx0 + wHH + 7;
  u->drawBox(colX, cy0 - 23, 5, 5); u->drawBox(colX, cy0 - 9, 5, 5);
  u->drawStr(colX + 12, cy0, mm);

  u->setFont(u8g2_font_helvB12_tf);
  if (haveT) {
    const char* wd[] = {"Sun","Mon","Tue","Wed","Thu","Fri","Sat"};
    const char* mo[] = {"Jan","Feb","Mar","Apr","May","Jun","Jul","Aug","Sep","Oct","Nov","Dec"};
    char date[24]; snprintf(date, sizeof(date), "%s, %s %d", wd[t.tm_wday], mo[t.tm_mon], t.tm_mday);
    u->drawStr(170, 22, date);
  }
  // 今日天气(右上)
  if (g.wxNowC >= 0) {
    drawWx(330, 26, wxType(g.wxNowC));
    u->setFont(u8g2_font_logisoso20_tn);
    char nt[6]; snprintf(nt, sizeof(nt), "%d", g.wxNowT);
    u->drawStr(352, 36, nt);
    u->drawCircle(352 + u->getStrWidth(nt) + 3, 22, 2);
  }
  u->drawHLine(8, 56, W - 16);

  // ---- Claude 用量 ----
  u->setFont(u8g2_font_helvB12_tf);
  u->drawStr(10, 78, "CLAUDE");
  u->setFont(u8g2_font_6x13_tf);
  char ps[8];
  u->drawStr(110, 80, "SESSION"); bar(180, 70, 150, 12, g.sessionPct / 100.0);
  snprintf(ps, sizeof(ps), "%.0f%%", g.sessionPct); u->drawStr(345, 80, ps);
  u->drawStr(110, 100, "WEEKLY");  bar(180, 90, 150, 12, g.weekPct / 100.0);
  snprintf(ps, sizeof(ps), "%.0f%%", g.weekPct); u->drawStr(345, 100, ps);
  u->drawHLine(8, 112, W - 16);

  // ---- 5天预报 ----
  for (int i = 0; i < g.nDays && i < 5; i++) {
    int cx = 40 + i * 80;
    u->setFont(u8g2_font_helvB12_tf);
    strCenter(cx, 134, g.days[i].d);
    drawWx(cx, 168, wxType(g.days[i].c));
    u->setFont(u8g2_font_helvB12_tf);
    tempCenter(cx, 210, g.days[i].hi);
    u->setFont(u8g2_font_6x13_tf);
    tempCenter(cx, 232, g.days[i].lo);
    if (i) u->drawVLine(i * 80, 122, 120);
  }
  u->drawHLine(8, 250, W - 16);

  // ---- 底部：室温 + 状态 ----
  u->setFont(u8g2_font_6x13_tf);
  char room[40];
  if (g.roomOk) snprintf(room, sizeof(room), "Room %.1fC  %.0f%%RH", g.roomT, g.roomH);
  else snprintf(room, sizeof(room), "Room --");
  u->drawStr(10, 276, room);
  char st[28];
  if (!g.ok) snprintf(st, sizeof(st), "proxy: no data");
  else if (haveT) snprintf(st, sizeof(st), "%s %02d:%02d", g.stale ? "stale" : "ok", t.tm_hour, t.tm_min);
  else snprintf(st, sizeof(st), g.ok ? "ok" : "...");
  u->drawStr(W - 16 - u->getStrWidth(st), 276, st);

  flush();
}

// ---------------------- 拉数据 ----------------------
bool poll() {
  if (WiFi.status() != WL_CONNECTED) return false;
  HTTPClient http; http.setConnectTimeout(8000); http.setTimeout(8000);
  if (!http.begin(PROXY_URL)) return false;
  int code = http.GET();
  if (code != 200) { http.end(); g.ok = false; return false; }
  String payload = http.getString(); http.end();
  JsonDocument doc;
  if (deserializeJson(doc, payload)) { g.ok = false; return false; }
  g.ok = doc["ok"] | false;
  g.stale = doc["stale"] | false;
  g.sessionPct = doc["session_pct"] | 0.0;
  g.weekPct = doc["week_pct"] | 0.0;
  JsonObject wx = doc["weather"];
  if (!wx.isNull()) {
    g.wxNowT = wx["now_t"] | 0;
    g.wxNowC = wx["now_c"] | -1;
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
  return true;
}

void connectWiFi() {
  WiFi.mode(WIFI_STA); WiFi.begin(WIFI_SSID, WIFI_PASS);
  u->clearBuffer(); u->setFont(u8g2_font_helvB12_tf);
  strCenter(W / 2, H / 2, "connecting wifi...");
  flush();
  uint32_t t0 = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - t0 < 20000) delay(250);
}

// ---------------------- setup / loop ----------------------
void setup() {
  Serial.begin(115200);
  Wire.begin(I2C_SDA, I2C_SCL);
  lcd.begin(0, U8G2_R1);
  u = lcd.getU8g2();

  u->clearBuffer(); u->setFont(u8g2_font_helvB12_tf);
  strCenter(W / 2, H / 2, "Claude RLCD booting...");
  flush();

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
  if (now - lastPoll >= POLL_MS) {
    if (WiFi.status() != WL_CONNECTED) connectWiFi();
    poll();
    g.roomOk = shtc3Read(g.roomT, g.roomH);
    render();
    lastPoll = lastRender = now;
  } else if (now - lastRender >= RENDER_MS) {
    render();
    lastRender = now;
  }
  delay(20);
}
