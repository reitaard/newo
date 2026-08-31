#!/usr/bin/env python3
"""Local-only warm Pocket TTS bridge. Emits native f32le; Node owns PCM16 conversion."""
from __future__ import annotations
import argparse
import threading
from contextlib import contextmanager
from typing import Iterator

import torch
import uvicorn
from fastapi import FastAPI, HTTPException
from fastapi.responses import StreamingResponse
from pydantic import BaseModel
from pocket_tts import TTSModel

SAMPLE_RATE = 24_000
VOICE = "michael"
app = FastAPI(docs_url=None, redoc_url=None)
model: TTSModel | None = None
voice_state: dict | None = None
# Pocket TTSModel streaming is non-thread-safe. One warm model, one generation at once.
generation_lock = threading.Lock()

class SynthesisRequest(BaseModel):
    input: str
    voice: str = VOICE

@contextmanager
def exclusive_generation():
    generation_lock.acquire()
    try:
        yield
    finally:
        generation_lock.release()

@app.post("/v1/audio/newo-stream")
def stream(request: SynthesisRequest):
    if not request.input.strip():
        raise HTTPException(400, "input must not be empty")
    if request.voice != VOICE:
        raise HTTPException(400, "this service is configured only for michael")

    def chunks() -> Iterator[bytes]:
        # No WAV, resampling, ffmpeg, segmentation, Opus, or complete-audio buffer.
        # On disconnect Starlette closes this iterator. Pocket has no hard cancellation API,
        # so its internal workers may finish the current short generation in the background.
        with exclusive_generation():
            assert model is not None and voice_state is not None
            for chunk in model.generate_audio_stream(voice_state, request.input):
                yield chunk.detach().to(device="cpu", dtype=torch.float32).contiguous().numpy().tobytes()

    return StreamingResponse(chunks(), media_type="application/octet-stream", headers={
        "X-Audio-Sample-Rate": str(SAMPLE_RATE), "X-Audio-Channels": "1",
        "X-Audio-Format": "pcm_f32le", "Cache-Control": "no-store",
    })

def main() -> None:
    global model, voice_state
    parser = argparse.ArgumentParser(description="Local warm Pocket INT8 Michael service")
    parser.add_argument("--port", type=int, default=8123)
    args = parser.parse_args()
    model = TTSModel.load_model(language="english", quantize=True)
    voice_state = model.get_state_for_audio_prompt(VOICE)
    uvicorn.run(app, host="127.0.0.1", port=args.port, log_level="info")

if __name__ == "__main__":
    main()
