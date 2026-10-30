# Pocket Companion

一个放在桌上的语音小伙伴：按住说话，它用一张方形 Nomi 风格的脸回应你。

![六个表情状态](docs/face-states.png)

从左到右、从上到下依次是：待机、聆听、思考、说话、开心、疑惑。这张图是 `firmware/main/face.c` 在电脑上直接渲染出来的，和设备上画的是同一份代码。

## 架构

```text
┌─────────── 设备（ESP32-S3） ───────────┐        ┌──────────── 代理（电脑 / 小服务器） ────────────┐
│ 按键录音 → 16k PCM ───────────────────┼─ WS ──▶│ ASR（OpenAI 兼容 /audio/transcriptions）       │
│                                        │        │   → LLM（/chat/completions，回答带情绪标签）     │
│ 表情状态机 ◀── state / emotion ────────┼◀─ WS ──│   → edge-tts → ffmpeg 转 16k PCM               │
│ I2S 播放   ◀── PCM ────────────────────┼◀─ WS ──│                                                 │
└────────────────────────────────────────┘        └─────────────────────────────────────────────────┘
```

设备只负责录音、播放和画脸，ASR、LLM、TTS 都放在代理里。这样密钥不进固件，换哪家服务也只改代理。协议见 [docs/protocol.md](docs/protocol.md)。

## 目录

| 路径 | 内容 |
|---|---|
| `firmware/` | ESP-IDF 工程。`main/face.c` 表情，`main/raster.c` 软件光栅，`main/display.c` 屏幕驱动 |
| `firmware/host/` | 在电脑上编译表情代码、渲染成图片的小工具 |
| `server/` | 代理服务和模拟设备脚本 |
| `preview/` | 浏览器里的表情预览，调外观用 |
| `docs/` | 硬件引脚、通信协议 |

## 硬件

SpotPear ESP32-S3 3.5 寸 AI 板（ESP32-S3R8，8MB PSRAM，480×320 ST7796S，ES8311 + NS4150B，GT911 触摸）。引脚和注意事项见 [docs/hardware.md](docs/hardware.md)。

## 快速开始

### 浏览器预览

直接打开 `preview/nomi-square.html`，按 1–6 切换状态。

### 代理

需要 Python 3.10+ 和 ffmpeg。

```bash
cd server
pip install -r requirements.txt
cp .env.example .env        # 填 ASR / LLM 的地址和 key
python companion_server.py  # 监听 ws://0.0.0.0:8765/ws
```

不填 key 也能先测协议：`MOCK=1` 会跳过 ASR 和 LLM，只走真实的 edge-tts。

```bash
MOCK=1 python companion_server.py
python fake_device.py --text "你好"          # 另开一个终端
```

### 固件

需要 ESP-IDF 5.3 或更新版本。

```bash
cd firmware
idf.py set-target esp32s3
idf.py build flash monitor
```

### 设备模拟器（浏览器里跑固件的表情代码）

把 `face.c` 和 `raster.c` 编译成 WebAssembly，在浏览器里实时运行，和板子上是同一份代码：

```bash
cd firmware
python -m ziglang cc -target wasm32-wasi -mexec-model=reactor -O2 -Imain \
  $(for f in fb w h init set_mode get_mode set_level step render dirty_x0 dirty_y0 dirty_x1 dirty_y1; do printf -- '-Wl,--export=sim_%s ' $f; done) \
  host/sim/sim_exports.c main/face.c main/raster.c -o host/sim/face.wasm
cd host/sim && python -m http.server 8800
```

然后打开 http://127.0.0.1:8800/sim.html，按 1–6 或空格切换表情。

### 在电脑上渲染表情

```bash
cd firmware
python -m ziglang cc -std=c11 -O2 -Imain host/render_frames.c main/face.c main/raster.c -o render
./render host/out   # 每个状态输出一张 PPM
```

## 进度

- [x] 方形 Nomi 表情：六个状态、弹簧动画、局部刷新（电脑上渲染验证过，平均每帧只刷约 18% 的屏幕）
- [x] 屏幕驱动、BOOT 键切换表情（引脚按原理图核对过；**固件还没在 ESP-IDF 下编译过，也没上机**）
- [x] 代理服务：ASR → LLM → edge-tts，WebSocket 协议（MOCK 模式在本机跑通）
- [ ] 设备端音频：ES8311 初始化、按键录音、WebSocket 客户端、I2S 播放
- [ ] 触摸（GT911 轮询）
- [ ] 本地工具：红外 / 继电器

## 致谢

本仓库最初由 [FoloToy/ai-passport](https://github.com/FoloToy/ai-passport) 复制而来，现已改为面向另一块硬件的独立项目，原有代码已全部替换。许可证见 [LICENSE](LICENSE)。
