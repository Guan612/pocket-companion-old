# 设备 ↔ 代理协议

设备通过一条 WebSocket 连到代理：`ws://<代理地址>:8765/ws?token=<DEVICE_TOKEN>`。
音频统一是 **16kHz、单声道、16bit 小端 PCM**，上下行一样。

## 设备 → 代理

| 消息 | 说明 |
|---|---|
| `{"type":"listen_start"}` | 按下说话键。会打断正在进行的上一轮回答。 |
| 二进制帧 | 录音 PCM，建议每帧 100ms（3200 字节）。只在 listen_start 之后收，最长 30 秒。 |
| `{"type":"listen_stop"}` | 松开说话键，代理开始识别和回答。录音短于 0.3 秒直接回到 idle。 |
| `{"type":"text","text":"..."}` | 直接发文字（调试或键盘输入），跳过 ASR。 |

## 代理 → 设备

| 消息 | 说明 |
|---|---|
| `{"type":"state","state":"listen\|think\|speak\|happy\|idle\|confused","emotion":"..."}` | 切表情。`speak` 时带 `emotion`（happy / neutral / confused）。 |
| `{"type":"asr","text":"..."}` | 识别结果，可显示在屏上。 |
| `{"type":"reply","text":"...","emotion":"..."}` | 回答全文（已去掉情绪标签）。 |
| 二进制帧 | TTS 的 PCM，每帧 4096 字节（128ms）。下发速度快于实时，设备要自己缓冲后按采样率播放。 |
| `{"type":"tts_end"}` | 本轮语音发完。设备把缓冲播完后再回到待机表情。 |
| `{"type":"error","message":"..."}` | 出错，随后会发 `state: confused`。 |

## 一轮对话的顺序

```text
设备: listen_start → PCM … PCM → listen_stop
代理: state=listen
代理: state=think → asr → reply → state=speak → PCM … PCM → tts_end → state=happy/idle
```

## 本机测过的延迟（MOCK 模式，跳过 ASR 和 LLM）

| 情况 | 首包音频 |
|---|---|
| 冷启动第一轮（edge-tts 首次建连） | 约 4.5 秒 |
| 后续轮次 | 约 1.1 秒 |

接上真实 ASR 和 LLM 后，还要加上这两步各自的耗时。
