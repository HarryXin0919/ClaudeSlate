# ClaudeSlate

**English** | [简体中文](README.zh-CN.md)

A low‑power desk **slate that shows your Claude plan usage, a clock, and a 5‑day
weather forecast** — on a 4.2" reflective (e‑paper‑like) ESP32‑S3 display. No backlight,
sips power, always‑on, easy on the eyes.

It's the flat sibling of [ClaudeOrb](https://github.com/HarryXin0919/ClaudeOrb): same idea
(your real Claude session/weekly limits, read from your local Claude Code login — no API
key), plus a big clock, today's weather, a 5‑day forecast, the room's temperature, and the
battery level.

```
┌──────────────────────────────────────────────┐
│  14:32        Sun, Jun 8            [☁] 22°   │  clock · date · today
├──────────────────────────────────────────────┤
│  CLAUDE   SESSION ▮▮▮▮▮▮▯▯▯▯▯▯  29%           │  Claude usage
│           WEEKLY  ▮▮▮▯▯▯▯▯▯▯▯▯  12%           │
├──────────────────────────────────────────────┤
│   Mon     Tue     Wed     Thu     Fri         │  5-day forecast
│   [☁]     [☁]     [☀]     [🌧]    [☀]         │
│   23°     25°     32°     33°     35°         │
│   20°     18°     15°     20°     22°         │
├──────────────────────────────────────────────┤
│  Room 26.5C 58%RH        ok 14:32   [▮▮▯] 84%│  room · status · battery
└──────────────────────────────────────────────┘
```

---

## How it works

A tiny Python **proxy on your PC** reads the OAuth token Claude Code already stores
locally, calls Anthropic's usage endpoint, also fetches the weather from **Open‑Meteo**
(free, no key), and serves one small JSON on your LAN. The ESP32 polls that over Wi‑Fi.
Time comes from NTP; the room temperature is read from the board's onboard **SHTC3**, and
the battery percentage is read from the battery‑voltage **ADC** (GPIO4, the divider the
Waveshare board uses). This board has no fuel‑gauge IC, so it shows the level only — no
charging indicator.

```
~/.claude/.credentials.json ─┐
   (Claude OAuth token)      ├─> proxy (your PC) ──(LAN JSON)──> ESP32-S3-RLCD-4.2
Open-Meteo (weather) ────────┘    8787/usage                    ClaudeSlate firmware
```

The token never leaves your PC and is never flashed into the device.

---

## Hardware

A **Waveshare ESP32‑S3‑RLCD‑4.2** — a 4.2" **400×300** reflective monochrome LCD
(**ST7305**, SPI, no backlight) on an ESP32‑S3, with an onboard **SHTC3** temp/humidity
sensor and **PCF85063A** RTC. ~$27.

Pins (from Waveshare's board): LCD SCK=11, MOSI=12, DC=5, CS=40, RST=41; I²C SDA=13,
SCL=14 (SHTC3 @ 0x70). Works with a **Claude Pro/Max** subscription used via Claude Code.

---

## Setup

### 1) Run the proxy (your PC)

**Python 3.8+**, zero third‑party packages. You must be logged into **Claude Code**
(`claude` → `/login`).

```bash
python proxy/claude_limits_proxy.py
```

- Set your **weather city** with env vars (default = Shanghai):
  `WX_LAT=40.71  WX_LON=-74.01  WX_CITY="New York"  python proxy/claude_limits_proxy.py`
  (latitude/longitude of your city; find them on any maps site).
- Find your **PC's LAN IP** (`ipconfig` / `ip addr`) — you'll put it in the firmware.
- **Allow it through the firewall** (port 8787) when prompted.
- Can't reach Anthropic/Open‑Meteo directly (e.g. mainland China)? set
  `UPSTREAM_PROXY=http://127.0.0.1:7890` (a local Clash/HTTP proxy).

### 2) Get the display driver (one‑time)

This board's ST7305 driver isn't bundled here. From Waveshare's official demo
[`waveshareteam/ESP32-S3-RLCD-4.2`](https://github.com/waveshareteam/ESP32-S3-RLCD-4.2):

- copy **`ST7305_U8g2.h`** and **`ST7305_U8g2.cpp`** (in `02_Example/Arduino/10_U8G2_Test/`)
  into `firmware/claude_slate/` next to the `.ino`;
- you'll compile against the **U8g2** that ships in that repo's `01_Arduino_Libraries/`
  (it has the patched `u8x8_d_st7305`).

### 3) Flash the firmware

Arduino **esp32 core 3.x**. Edit the top of `firmware/claude_slate/claude_slate.ino`
(`WIFI_SSID` / `WIFI_PASS` — 2.4 GHz; `PROXY_URL` = `http://<PC-LAN-IP>:8787/usage`).

```bash
arduino-cli compile \
  --libraries /path/to/ESP32-S3-RLCD-4.2/01_Arduino_Libraries \
  --fqbn esp32:esp32:esp32s3:CDCOnBoot=cdc,PartitionScheme=huge_app,FlashSize=16M \
  -u -p <PORT> firmware/claude_slate
```

Board settings if using the IDE: **ESP32S3 Dev Module**, USB CDC On Boot **Enabled**,
Partition **Huge APP (3MB)**, Flash **16MB**.

The reflective LCD has no backlight — read it under normal room light (black‑on‑white).

---

## Notes

- **Black‑on‑white** is produced in software (`INVERT_DISPLAY 1`, the firmware XORs the
  frame) so Waveshare's driver stays unmodified. Prefer hardware inversion? set
  `INVERT_DISPLAY 0` and change `_cmd(0x21)` → `_cmd(0x20)` in `ST7305_U8g2.cpp`.
- **Refresh:** the screen redraws every ~10 s (clock) and polls the proxy every 60 s;
  the proxy caches limits 180 s, weather 30 min.
- **Weather codes** are WMO codes; the firmware draws simple monochrome icons for
  clear / cloud / fog / rain / snow / thunder.

---

## Credits & license

- ClaudeSlate firmware, proxy and docs: **MIT** (see `LICENSE`). Author: Harry Xin.
- ST7305 driver (`ST7305_U8g2.*`) and the patched U8g2 come from **Waveshare's**
  ESP32‑S3‑RLCD‑4.2 demo — fetch them from their repo (not redistributed here).
- Fonts/engine: [U8g2](https://github.com/olikraus/u8g2) (Oliver Kraus).
- Weather: [Open‑Meteo](https://open-meteo.com) (free, no key).
- "Claude" is a trademark of **Anthropic**; ClaudeSlate is an unofficial, fan‑made
  project, not affiliated with or endorsed by Anthropic. See `NOTICE`.
