"""模拟设备：连上代理，发一段录音或一句文字，把回来的语音存成 wav，并打印事件和延迟。

用法：
  python fake_device.py --text "你好"
  python fake_device.py --wav speech.wav      # 16k 单声道 16bit
"""

import argparse
import asyncio
import json
import time
import wave

from aiohttp import ClientSession, WSMsgType

SAMPLE_RATE = 16000


async def run(args):
    async with ClientSession() as http, http.ws_connect(args.url) as ws:
        t0 = time.monotonic()
        if args.wav:
            with wave.open(args.wav, "rb") as w:
                assert w.getframerate() == SAMPLE_RATE and w.getnchannels() == 1 and w.getsampwidth() == 2
                pcm = w.readframes(w.getnframes())
            await ws.send_str(json.dumps({"type": "listen_start"}))
            for i in range(0, len(pcm), 3200):  # 每块 100ms
                await ws.send_bytes(pcm[i:i + 3200])
            await ws.send_str(json.dumps({"type": "listen_stop"}))
        else:
            await ws.send_str(json.dumps({"type": "text", "text": args.text}, ensure_ascii=False))
        t0 = time.monotonic()

        audio, first_audio = bytearray(), None
        async for msg in ws:
            dt = time.monotonic() - t0
            if msg.type == WSMsgType.BINARY:
                if first_audio is None:
                    first_audio = dt
                    print(f"[{dt:5.2f}s] first audio chunk")
                audio += msg.data
            elif msg.type == WSMsgType.TEXT:
                m = json.loads(msg.data)
                print(f"[{dt:5.2f}s] {json.dumps(m, ensure_ascii=False)}")
                if m["type"] == "state" and m["state"] in ("idle", "happy", "confused") and (
                    audio or m["state"] == "confused"
                ):
                    break

        with wave.open(args.out, "wb") as w:
            w.setnchannels(1)
            w.setsampwidth(2)
            w.setframerate(SAMPLE_RATE)
            w.writeframes(bytes(audio))
        print(f"saved {args.out}: {len(audio) / SAMPLE_RATE / 2:.2f}s audio")


def main():
    p = argparse.ArgumentParser()
    p.add_argument("--url", default="ws://127.0.0.1:8765/ws")
    p.add_argument("--text", default="你好呀")
    p.add_argument("--wav")
    p.add_argument("--out", default="reply.wav")
    asyncio.run(run(p.parse_args()))


if __name__ == "__main__":
    main()
