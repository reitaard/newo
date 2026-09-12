#!/usr/bin/env python3
"""Local-only warm Pocket TTS bridge. Emits native f32le; Node owns PCM16 conversion."""
from __future__ import annotations
import argparse
import os
import threading
import time
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
QUANTIZED = True
app = FastAPI(docs_url=None, redoc_url=None)
model: TTSModel | None = None
voice_state: dict | None = None
# Pocket TTSModel streaming is non-thread-safe. The lock remains held even after a
# client disconnect: the remaining upstream iterator is drained before release.
generation_lock = threading.Lock()
generation_state_lock = threading.Lock()
generation_playback_id: str | None = None
generation_progress_at = 0.0

class SynthesisRequest(BaseModel):
    input: str
    voice: str = VOICE
    playback_id: str | None = None

@contextmanager
def exclusive_generation():
    generation_lock.acquire()
    try:
        yield
    finally:
        generation_lock.release()

def float32le(chunk: torch.Tensor) -> bytes:
    return chunk.detach().to(device="cpu", dtype=torch.float32).contiguous().numpy().tobytes()

def note_generation(playback_id: str | None) -> None:
    global generation_playback_id, generation_progress_at
    with generation_state_lock:
        generation_playback_id = playback_id or "-"
        generation_progress_at = time.monotonic()

def note_generation_progress() -> None:
    global generation_progress_at
    with generation_state_lock:
        if generation_playback_id is not None:
            generation_progress_at = time.monotonic()

def clear_generation() -> None:
    global generation_playback_id, generation_progress_at
    with generation_state_lock:
        generation_playback_id = None
        generation_progress_at = 0.0

def stalled_generation(now: float, timeout_seconds: float) -> tuple[str, float] | None:
    with generation_state_lock:
        if generation_playback_id is None or now - generation_progress_at <= timeout_seconds:
            return None
        return generation_playback_id, now - generation_progress_at

def generation_watchdog(timeout_seconds: float, poll_seconds: float = 0.5) -> None:
    while True:
        time.sleep(poll_seconds)
        stalled = stalled_generation(time.monotonic(), timeout_seconds)
        if stalled is None:
            continue
        playback_id, stalled_seconds = stalled
        print(
            "POCKET_GENERATION_STALLED"
            f" playback_id={playback_id} stalled_ms={stalled_seconds * 1_000:.0f} action=restart",
            flush=True,
        )
        # Pocket exposes no safe cancellation for a native generation blocked in
        # next(). Exit the worker instead of leaving all future requests queued
        # forever; the system service restarts the already-warm process.
        os._exit(70)

def serialized_native_stream(tts_model, state: dict, text: str, playback_id: str | None = None,
                             lock: threading.Lock = generation_lock) -> Iterator[bytes]:
    """Yield native audio, draining Pocket completely before releasing model ownership.

    StreamingResponse closes this generator on client disconnect. Pocket has no hard
    cancellation API, so `finally` consumes/discards the rest while the lock is held.
    This is deliberately synchronous: a new request cannot touch model state early.
    """
    with lock:
        note_generation(playback_id)
        upstream = None
        exhausted = False
        chunks = max_gap_ms = over_40 = over_80 = over_120 = over_200 = 0
        def next_native():
            nonlocal chunks, max_gap_ms, over_40, over_80, over_120, over_200
            # Time the actual upstream next() call. Time spent suspended at our HTTP
            # yield is intentionally excluded: it is consumer backpressure, not Pocket.
            waited_from = time.monotonic()
            chunk = next(upstream)
            note_generation_progress()
            gap_ms = (time.monotonic() - waited_from) * 1_000
            if chunks:
                max_gap_ms = max(max_gap_ms, gap_ms)
                over_40 += gap_ms > 40
                over_80 += gap_ms > 80
                over_120 += gap_ms > 120
                over_200 += gap_ms > 200
            chunks += 1
            return chunk
        try:
            upstream = iter(tts_model.generate_audio_stream(state, text))
            while True:
                try:
                    yield float32le(next_native())
                except StopIteration:
                    exhausted = True
                    break
        finally:
            outcome = "completed"
            if upstream is not None and not exhausted:
                outcome = "drained_after_client_cancel"
                while True:
                    try:
                        next_native()
                    except StopIteration:
                        break
            print(
                "POCKET_NATIVE_CONTINUITY"
                f" playback_id={playback_id or '-'} outcome={outcome} chunks={chunks}"
                f" max_gap_ms={max_gap_ms:.1f} over_40={over_40} over_80={over_80}"
                f" over_120={over_120} over_200={over_200}",
                flush=True,
            )
            clear_generation()

@app.get("/healthz")
def healthz():
    # Uvicorn starts only after main() has loaded model and Michael's state.
    if model is None or voice_state is None:
        raise HTTPException(503, "Pocket model is not ready")
    return {"status": "ok", "voice": VOICE, "sample_rate": SAMPLE_RATE, "quantized": QUANTIZED}

@app.post("/v1/audio/newo-stream")
def stream(request: SynthesisRequest):
    if not request.input.strip():
        raise HTTPException(400, "input must not be empty")
    if request.voice != VOICE:
        raise HTTPException(400, "this service is configured only for michael")
    assert model is not None and voice_state is not None
    return StreamingResponse(serialized_native_stream(model, voice_state, request.input, request.playback_id), media_type="application/octet-stream", headers={
        "X-Audio-Sample-Rate": str(SAMPLE_RATE), "X-Audio-Channels": "1",
        "X-Audio-Format": "pcm_f32le", "Cache-Control": "no-store",
    })

def main() -> None:
    global model, voice_state
    parser = argparse.ArgumentParser(description="Local warm Pocket INT8 Michael service")
    parser.add_argument("--port", type=int, default=8123)
    parser.add_argument("--generation-stall-seconds", type=float, default=15.0)
    args = parser.parse_args()
    model = TTSModel.load_model(language="english", quantize=QUANTIZED)
    voice_state = model.get_state_for_audio_prompt(VOICE)
    threading.Thread(target=generation_watchdog, args=(args.generation_stall_seconds,),
                     name="pocket-generation-watchdog", daemon=True).start()
    uvicorn.run(app, host="127.0.0.1", port=args.port, log_level="info")

if __name__ == "__main__":
    main()
