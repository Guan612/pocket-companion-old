"""Pocket Companion 代理服务。

设备按住说话时把 16k PCM 推上来；松手后这里依次调用
ASR（OpenAI 兼容 /audio/transcriptions）→ LLM（/chat/completions）→ edge-tts，
再把 16k PCM 分块流回设备，同时下发表情状态。协议见 docs/protocol.md。

运行：python companion_server.py
只测 TTS：python companion_server.py --say "你好呀" --out hello.wav
"""

import argparse
import asyncio
import io
import json
import logging
import os
import re
import wave

import edge_tts
from aiohttp import ClientSession, ClientTimeout, FormData, WSMsgType, web

SAMPLE_RATE = 16000
BYTES_PER_SEC = SAMPLE_RATE * 2
MAX_RECORD_SEC = 30
MIN_RECORD_SEC = 0.3
TTS_CHUNK = 4096
HISTORY_LIMIT = 12
EMOTIONS = ("happy", "neutral", "confused")

SYSTEM_PROMPT = (
    "你是一个放在桌面上的小伙伴，用口语化的中文回答，每次不超过三句话，不要用 markdown。"
    "回答开头必须带一个情绪标签，只能是 [happy]、[neutral]、[confused] 之一，"
    "例如：[happy]好呀，我来帮你看看。"
)

log = logging.getLogger("companion")


def load_dotenv(path=os.path.join(os.path.dirname(os.path.abspath(__file__)), ".env")):
    """读同目录的 .env；已有的环境变量优先。"""
    if not os.path.exists(path):
        return
    with open(path, encoding="utf-8") as f:
        for line in f:
            line = line.strip()
            if line and not line.startswith("#") and "=" in line:
                k, v = line.split("=", 1)
                os.environ.setdefault(k.strip(), v.strip())


load_dotenv()


def env(key, default=None):
    return os.environ.get(key) or default


MOCK = env("MOCK") == "1"
DEVICE_TOKEN = env("DEVICE_TOKEN", "")
TTS_VOICE = env("TTS_VOICE", "zh-CN-XiaoxiaoNeural")


def pcm_to_wav(pcm: bytes) -> bytes:
    buf = io.BytesIO()
    with wave.open(buf, "wb") as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(SAMPLE_RATE)
        w.writeframes(pcm)
    return buf.getvalue()


def api(prefix):
    base = env(f"{prefix}_BASE_URL") or env("OPENAI_BASE_URL", "https://api.openai.com/v1")
    key = env(f"{prefix}_API_KEY") or env("OPENAI_API_KEY", "")
    return base.rstrip("/"), {"Authorization": f"Bearer {key}"} if key else {}


async def transcribe(http: ClientSession, pcm: bytes) -> str:
    if MOCK:
        return env("MOCK_ASR_TEXT", "你好呀，今天过得怎么样？")
    base, headers = api("ASR")
    form = FormData()
    form.add_field("file", pcm_to_wav(pcm), filename="speech.wav", content_type="audio/wav")
    form.add_field("model", env("ASR_MODEL", "whisper-1"))
    form.add_field("language", "zh")
    async with http.post(f"{base}/audio/transcriptions", data=form, headers=headers) as r:
        if r.status >= 400:
            raise RuntimeError(f"ASR {r.status}: {(await r.text())[:200]}")
        return (await r.json())["text"].strip()


async def chat(http: ClientSession, history: list) -> str:
    if MOCK:
        return f"[happy]我听到你说：{history[-1]['content']}。这是测试模式的回复。"
    base, headers = api("LLM")
    payload = {
        "model": env("LLM_MODEL", "gpt-4o-mini"),
        "messages": [{"role": "system", "content": SYSTEM_PROMPT}, *history],
        "temperature": 0.7,
    }
    async with http.post(f"{base}/chat/completions", json=payload, headers=headers) as r:
        if r.status >= 400:
            raise RuntimeError(f"LLM {r.status}: {(await r.text())[:200]}")
        return (await r.json())["choices"][0]["message"]["content"].strip()


def split_emotion(reply: str):
    m = re.match(r"\s*\[(\w+)\]\s*", reply)
    if m and m.group(1) in EMOTIONS:
        return m.group(1), reply[m.end():].strip()
    return "neutral", reply.strip()


def split_sentences(text: str):
    """按句切开，逐句合成，第一句能尽早开始播放。太短的句子并到下一句。"""
    parts = [p.strip() for p in re.split(r"(?<=[。！？!?；;\n])", text) if p.strip()]
    out = []
    for p in parts:
        if out and len(out[-1]) < 6:
            out[-1] += p
        else:
            out.append(p)
    return out


async def tts_pcm(text: str):
    """edge-tts 出的是 MP3，用 ffmpeg 边收边转成 16k 单声道 PCM。"""
    proc = await asyncio.create_subprocess_exec(
        "ffmpeg", "-loglevel", "error", "-f", "mp3", "-i", "pipe:0",
        "-f", "s16le", "-ac", "1", "-ar", str(SAMPLE_RATE), "pipe:1",
        stdin=asyncio.subprocess.PIPE, stdout=asyncio.subprocess.PIPE,
    )

    async def feed():
        try:
            async for chunk in edge_tts.Communicate(text, TTS_VOICE).stream():
                if chunk["type"] == "audio":
                    proc.stdin.write(chunk["data"])
                    await proc.stdin.drain()
        finally:
            proc.stdin.close()

    feeder = asyncio.create_task(feed())
    try:
        while data := await proc.stdout.read(TTS_CHUNK):
            yield data
        await feeder
    finally:
        if proc.returncode is None:
            proc.kill()
        await proc.wait()


async def send(ws, **msg):
    await ws.send_str(json.dumps(msg, ensure_ascii=False))


async def handle_turn(ws, http, history, pcm=None, text=None):
    try:
        await send(ws, type="state", state="think")
        if text is None:
            if len(pcm) < MIN_RECORD_SEC * BYTES_PER_SEC:
                await send(ws, type="state", state="idle")
                return
            text = await transcribe(http, pcm)
            await send(ws, type="asr", text=text)
        if not text:
            await send(ws, type="state", state="confused")
            return

        history.append({"role": "user", "content": text})
        emotion, body = split_emotion(await chat(http, history))
        history.append({"role": "assistant", "content": body})
        del history[:-HISTORY_LIMIT]

        await send(ws, type="reply", text=body, emotion=emotion)
        await send(ws, type="state", state="speak", emotion=emotion)
        for sentence in split_sentences(body):
            async for chunk in tts_pcm(sentence):
                await ws.send_bytes(chunk)
        await send(ws, type="tts_end")
        await send(ws, type="state", state="happy" if emotion == "happy" else "idle")
    except asyncio.CancelledError:
        raise
    except Exception as e:
        log.exception("turn failed")
        await send(ws, type="error", message=str(e))
        await send(ws, type="state", state="confused")


async def ws_handler(request):
    if DEVICE_TOKEN and request.query.get("token") != DEVICE_TOKEN:
        raise web.HTTPUnauthorized()
    ws = web.WebSocketResponse(heartbeat=20, max_msg_size=1 << 20)
    await ws.prepare(request)
    http = request.app["http"]
    history, pcm, listening, task = [], bytearray(), False, None
    log.info("device connected: %s", request.remote)

    def start(**kw):
        nonlocal task
        if task and not task.done():
            task.cancel()  # 新一轮开始就打断上一轮
        task = asyncio.create_task(handle_turn(ws, http, history, **kw))

    try:
        async for msg in ws:
            if msg.type == WSMsgType.BINARY:
                if listening and len(pcm) < MAX_RECORD_SEC * BYTES_PER_SEC:
                    pcm += msg.data
            elif msg.type == WSMsgType.TEXT:
                m = json.loads(msg.data)
                kind = m.get("type")
                if kind == "listen_start":
                    if task and not task.done():
                        task.cancel()
                    pcm.clear()
                    listening = True
                    await send(ws, type="state", state="listen")
                elif kind == "listen_stop":
                    listening = False
                    start(pcm=bytes(pcm))
                elif kind == "text":
                    start(text=m.get("text", ""))
    finally:
        if task and not task.done():
            task.cancel()
        log.info("device disconnected")
    return ws


async def make_app():
    app = web.Application()
    app.router.add_get("/ws", ws_handler)

    async def session_ctx(app):
        app["http"] = ClientSession(timeout=ClientTimeout(total=60))
        yield
        await app["http"].close()

    app.cleanup_ctx.append(session_ctx)
    return app


async def say_to_file(text, out):
    pcm = bytearray()
    for sentence in split_sentences(text):
        async for chunk in tts_pcm(sentence):
            pcm += chunk
    with open(out, "wb") as f:
        f.write(pcm_to_wav(bytes(pcm)))
    print(f"wrote {out}: {len(pcm) / BYTES_PER_SEC:.2f}s")


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--host", default=env("HOST", "0.0.0.0"))
    parser.add_argument("--port", type=int, default=int(env("PORT", "8765")))
    parser.add_argument("--say", help="只测 TTS：把这段文字合成到 --out")
    parser.add_argument("--out", default="say.wav")
    args = parser.parse_args()
    logging.basicConfig(level=logging.INFO, format="%(asctime)s %(levelname)s %(message)s")
    if args.say:
        asyncio.run(say_to_file(args.say, args.out))
        return
    log.info("listening on ws://%s:%d/ws%s", args.host, args.port, " (MOCK)" if MOCK else "")
    web.run_app(make_app(), host=args.host, port=args.port, print=None)


if __name__ == "__main__":
    main()
