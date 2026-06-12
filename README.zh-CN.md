# ClaudeSlate

[English](README.md) | **简体中文**

一块低功耗桌面**平板,显示你的 Claude 套餐用量、时钟,和 5 天天气预报**——跑在 4.2"
反射式(类水墨)ESP32-S3 屏上。无背光、省电、常亮、不刺眼。

它是 [ClaudeOrb](https://github.com/HarryXin0919/ClaudeOrb) 的"平板版兄弟":一样的核心
(读你本机 Claude Code 登录态拿真实的会话/周限额,不需要 API key)。共 **三页**,按板上
**BOOT 键**翻页:

- **第 1 页 主页**:大时钟 + 当前天气(体感/湿度/风) + Claude 概览(今日 token/花费 $ + 7 天迷你柱图) + 5 天预报 + 底栏(室温/状态/电量)。
- **第 2 页 Claude 详情**:各档限额对齐表(5h/周/Sonnet/Opus/Extra,带条/% /重置倒计时) + 7 天用量柱图 + **花费(今日/7天/30天 美元)** + 今日 in/out + 当前活跃会话。
- **第 3 页 天气&室内**:当前大温度 + 体感/湿度/风 + 未来 12 小时温度折线 + 5 天高低温区间条 + 室内舒适度。

---

## 工作原理

电脑上跑一个极小的 Python **代理**:它读 Claude Code 已存在本地的 OAuth token,调
Anthropic 的用量接口,扫本地 Claude Code 日志算出今日/7天/30天 token 用量和按各模型单价
估算的 **美元花费**,再从 **Open-Meteo**(免费、免 key)拉天气,把一段精简 JSON 发到
局域网。ESP32 通过 Wi-Fi 轮询它。时间走 NTP;室温由板载 **SHTC3** 读取;电量由电池电压
**ADC**(GPIO4,用 Waveshare 板上的分压)读取。这块板没有电量计芯片,所以只显示电量,
不显示充电状态。

```
~/.claude/.credentials.json ─┐
   (Claude OAuth token)      ├─> 代理(你的电脑) ──(局域网 JSON)──> ESP32-S3-RLCD-4.2
Open-Meteo (天气) ───────────┘    8787/usage                     ClaudeSlate 固件
```

token 永远不离开你的电脑,也绝不烧进设备。

---

## 硬件

一块 **微雪 Waveshare ESP32-S3-RLCD-4.2** —— 4.2" **400×300** 反射式黑白屏
(**ST7305**, SPI, 无背光)+ ESP32-S3,板载 **SHTC3** 温湿度传感器和 **PCF85063A** RTC。约 ¥190。

引脚(微雪板子):LCD SCK=11, MOSI=12, DC=5, CS=40, RST=41;I²C SDA=13, SCL=14
(SHTC3 @ 0x70)。需要通过 Claude Code 使用的 **Claude Pro/Max** 订阅。

---

## 安装设置

### 1) 跑代理(你的电脑)

**Python 3.8+**,零第三方依赖。前提是已登录 **Claude Code**(`claude` → `/login`)。

```bash
python proxy/claude_limits_proxy.py
```

- 用环境变量设**天气城市**(默认上海):
  `WX_LAT=39.90  WX_LON=116.40  WX_CITY="Beijing"  python proxy/claude_limits_proxy.py`
  (你所在城市的经纬度,地图上一查就有)。
- 不用查电脑 IP——屏会用 **UDP 广播自动发现代理**(8788 端口),电脑 IP 变了也能跟上。
- 首次弹窗时**放行防火墙**(TCP 8787 + UDP 8788)。
- 直连不到 Anthropic / Open-Meteo(如中国大陆)?设
  `UPSTREAM_PROXY=http://127.0.0.1:7890`(本地 Clash/HTTP 代理)。
- **安全**:代理只在局域网内服务,且**绝不返回你的 OAuth token**——只给派生的百分比数字。
  在合租/公共等不可信网络上,设 `PROXY_TOKEN=<密码>`,固件里加
  `#define NET_PROXY_QUERY "?token=<密码>"`(或发 `Authorization: Bearer <密码>` 头);
  没带的请求一律 401。

### 2) 拿显示驱动(一次性)

本仓库不附带这块板的 ST7305 驱动。从微雪官方 demo
[`waveshareteam/ESP32-S3-RLCD-4.2`](https://github.com/waveshareteam/ESP32-S3-RLCD-4.2):

- 把 **`ST7305_U8g2.h`** 和 **`ST7305_U8g2.cpp`**(在 `02_Example/Arduino/10_U8G2_Test/`)
  复制到 `firmware/claude_slate/`,和 `.ino` 放一起;
- 编译时用该仓库 `01_Arduino_Libraries/` 里的 **U8g2**(带打过补丁的 `u8x8_d_st7305`)。

### 3) 烧固件

Arduino **esp32 core 3.x**。可以改 `firmware/claude_slate/claude_slate.ino` 顶部的
`NET_DEF_*` 出厂默认值(只支持 2.4GHz),也可以**直接烧**:连不上 WiFi 时屏会自动开
**配网热点**(`ClaudeSlate-Setup`),手机连上后自动弹出配置页(或手动开
`http://192.168.4.1`),选好 WiFi 保存即可。代理地址自动发现,不用填 IP。
之后想换网络,**长按 BOOT 3 秒**随时重新配网。

```bash
arduino-cli compile \
  --libraries /path/to/ESP32-S3-RLCD-4.2/01_Arduino_Libraries \
  --fqbn esp32:esp32:esp32s3:CDCOnBoot=cdc,PartitionScheme=huge_app,FlashSize=16M \
  -u -p <端口> firmware/claude_slate
```

IDE 设置:**ESP32S3 Dev Module**,USB CDC On Boot **Enabled**,
Partition **Huge APP (3MB)**,Flash **16MB**。

反射屏无背光——在正常室内光下看(黑字白底)。

---

## 说明

- **黑字白底**是软件做的(`INVERT_DISPLAY 1`,固件把整帧取反),这样微雪驱动不用改。
  想用硬件反显?把 `INVERT_DISPLAY` 设 0,并在 `ST7305_U8g2.cpp` 里把 `_cmd(0x21)` 改成
  `_cmd(0x20)`。
- **刷新**:屏每 ~10 秒重绘(时钟),每 60 秒拉一次代理;代理对限额缓存 180 秒、天气 30 分钟。
- **天气码**是 WMO 码;固件画的是简单黑白图标(晴/云/雾/雨/雪/雷)。

---

## 致谢与许可

- ClaudeSlate 固件、代理、文档:**MIT**(见 `LICENSE`)。作者:Harry Xin。
- ST7305 驱动(`ST7305_U8g2.*`)和打过补丁的 U8g2 来自**微雪** ESP32-S3-RLCD-4.2 demo
  ——请去他们仓库拿(本仓库不转发)。
- 字体/引擎:[U8g2](https://github.com/olikraus/u8g2)(Oliver Kraus)。
- 天气:[Open-Meteo](https://open-meteo.com)(免费、免 key)。
- "Claude" 是 **Anthropic** 的商标;ClaudeSlate 是非官方爱好者项目,与 Anthropic 无隶属、
  未获背书。详见 `NOTICE`。
