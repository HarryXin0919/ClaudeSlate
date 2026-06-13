# ClaudeSlate

**English** | [简体中文](README.zh-CN.md)

A low‑power desk **slate that shows your Claude plan usage, a clock, and a 5‑day
weather forecast** — on a 4.2" reflective (e‑paper‑like) ESP32‑S3 display. No backlight,
sips power, always‑on, easy on the eyes.

It's the flat sibling of [ClaudeOrb](https://github.com/HarryXin0919/ClaudeOrb): same idea
(your real Claude session/weekly limits, read from your local Claude Code login — no API
key). It has **three pages** — press the **BOOT** button to cycle them.

```
PAGE 1 — HOME                                   PAGE 2 — CLAUDE
┌──────────────────────────────────────────────┐ ┌──────────────────────────────────────────────┐
│  14:32        │  [☀] 18°  feels 20            │ │ CLAUDE DETAILS          Max  ·  ~$34 today    │
│  TUE · Jun 9  │   Clear  85%RH 2km/h          │ ├──────────────────────────────────────────────┤
├──────────────────────────────────────────────┤ │ TIER     USAGE          PCT    RESETS IN       │
│ CLAUDE Max       today 30.1M tok      ~$34.23 │ │ 5h ses [###-------]      1%    4h 41m          │
│ SESS [#---------]   1%        resets 4h 41m   │ │ Week   [##--------]    18%    3d 16h           │
│ WEEK [##--------]  18%        resets 3d 16h   │ │ Sonnet [#---------]     0%    3d 16h           │
│ 7d ▁▄▃▇█▂▁   wk 620M   pk 190M                │ │ Opus   [----------]    --                      │
├──────────────────────────────────────────────┤ │ Extra  [##########]on  92%    11h              │
│  TUE   WED   THU   FRI   SAT   (icons hi/lo)  │ ├──────────────────────────────────────────────┤
├──────────────────────────────────────────────┤ │ 7-DAY ▁▄▃▇█▂▁   wk 620M  avg 88M  pk 190M     │
│  Room 28.0C 47%RH      live      [▮▮▮] 96%    │ │ SPEND today ~$34  7d ~$475  30d ~$1242         │
└──────────────────────────────────────────────┘ │ TODAY 30.1M  in 30.0M out 0.1M                │
                                                   │ ACTIVE finditem  38 msg  idle 2m              │
PAGE 3 — WEATHER & ROOM: current temp + feels/    └──────────────────────────────────────────────┘
humidity/wind, a next-12h temperature trend line,
a 5-day hi/lo range bar, and room comfort.
```

---

## How it works

A tiny Python **proxy on your PC** reads the OAuth token Claude Code already stores
locally, calls Anthropic's usage endpoint, scans your local Claude Code logs to compute
today's / 7‑day / 30‑day token use and a rough **USD spend estimate** (per‑model rates),
also fetches the weather from **Open‑Meteo** (free, no key), and serves one small JSON on
your LAN. The ESP32 polls that over Wi‑Fi.
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
  `WX_LAT=40.71  WX_LON=-74.01  WX_CITY="New York"  WX_TZ="America/New_York"  python proxy/claude_limits_proxy.py`
  (latitude/longitude of your city; find them on any maps site). **`WX_TZ`** is the
  [IANA timezone](https://en.wikipedia.org/wiki/List_of_tz_database_time_zones) name —
  set it to match your city or the next‑12h forecast will be shifted by your offset.
- No need to look up your PC's IP — the screen **finds the proxy by UDP broadcast**
  (port 8788) and follows it if your PC's IP changes.
- **Allow it through the firewall** (TCP 8787 + UDP 8788) when prompted.
- Can't reach Anthropic/Open‑Meteo directly (e.g. mainland China)? set
  `UPSTREAM_PROXY=http://127.0.0.1:7890` (a local Clash/HTTP proxy).
- **Security:** the proxy binds **loopback‑only by default** (`BIND_HOST=127.0.0.1`) and
  never returns your OAuth token — only derived percentages. To let the screen reach it
  over the LAN, set `BIND_HOST=0.0.0.0` **and** `PROXY_TOKEN=<secret>` (binding to a
  non‑loopback address without a token is refused — it falls back to 127.0.0.1 with a
  warning). With a token set, also add `#define NET_PROXY_QUERY "?token=<secret>"` in the
  firmware (or send `Authorization: Bearer <secret>`); requests without it get 401.

### 2) Get the display driver (one‑time)

This board's ST7305 driver isn't bundled here. From Waveshare's official demo
[`waveshareteam/ESP32-S3-RLCD-4.2`](https://github.com/waveshareteam/ESP32-S3-RLCD-4.2):

- copy **`ST7305_U8g2.h`** and **`ST7305_U8g2.cpp`** (in `02_Example/Arduino/10_U8G2_Test/`)
  into `firmware/claude_slate/` next to the `.ino`;
- you'll compile against the **U8g2** that ships in that repo's `01_Arduino_Libraries/`
  (it has the patched `u8x8_d_st7305`).

### 3) Flash the firmware

Arduino **esp32 core 3.x**. Optionally edit the `NET_DEF_*` defaults at the top of
`firmware/claude_slate/claude_slate.ino` (2.4 GHz WiFi only) — or just flash as-is:
if the screen can't join WiFi it opens a **setup hotspot** (`ClaudeSlate-Setup`);
join it from your phone, the config page pops up (or open `http://192.168.4.1`),
pick your WiFi and save. The proxy is auto-discovered, so no IP to type. You can
re-enter setup anytime by **holding BOOT for 3 s**.

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
