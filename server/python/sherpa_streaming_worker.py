#!/usr/bin/env python3
import base64
import json
import os
import resource
import sys
import time
from pathlib import Path

import numpy as np


def send(payload):
    sys.stdout.write(json.dumps(payload, separators=(",", ":")) + "\n")
    sys.stdout.flush()


def metrics():
    rss = resource.getrusage(resource.RUSAGE_SELF).ru_maxrss
    if sys.platform != "darwin":
        rss *= 1024
    return {"rss_bytes": rss, "cpu_seconds": round(time.process_time(), 6)}


def require_file(path):
    if not path.is_file():
        raise RuntimeError(f"required model file missing: {path}")
    return str(path)


class Worker:
    def __init__(self, options):
        import sherpa_onnx

        model_dir = Path(options["modelDirectory"]).resolve()
        libri_giga = options.get("model") == "libri-giga"
        lm = options.get("lm") or {}
        kwargs = dict(
            tokens=require_file(model_dir / "tokens.txt"),
            encoder=require_file(model_dir / "encoder-epoch-99-avg-1.int8.onnx"),
            decoder=require_file(model_dir / ("decoder-epoch-99-avg-1.onnx" if libri_giga else "decoder-epoch-99-avg-1.int8.onnx")),
            joiner=require_file(model_dir / "joiner-epoch-99-avg-1.int8.onnx"),
            num_threads=int(options.get("numThreads", 2)), provider="cpu", sample_rate=16000,
            feature_dim=80, model_type="zipformer", enable_endpoint_detection=True,
            rule1_min_trailing_silence=float(options.get("endpointRule1MinTrailingSilence", os.getenv("VOICE_ASR_ENDPOINT_RULE1_S", "2.0"))),
            rule2_min_trailing_silence=float(options.get("endpointRule2MinTrailingSilence", os.getenv("VOICE_ASR_ENDPOINT_RULE2_S", "1.0"))),
            rule3_min_utterance_length=float(options.get("endpointRule3MinUtteranceLength", os.getenv("VOICE_ASR_ENDPOINT_RULE3_S", "20"))),
            decoding_method="modified_beam_search", max_active_paths=int(options.get("maxActivePaths", 4)),
        )
        if libri_giga:
            kwargs.update(modeling_unit="bpe", bpe_vocab=require_file(model_dir / "bpe.vocab"))
        hotwords = options.get("hotwordsFile")
        if hotwords:
            kwargs.update(hotwords_file=require_file(Path(hotwords).resolve()), hotwords_score=float(options.get("hotwordsScore", 1.5)))
        if lm.get("active"):
            kwargs.update(lm=require_file(Path(lm["model"]).resolve()), lm_scale=float(lm["scale"]), lm_shallow_fusion=True)
        self.recognizer = sherpa_onnx.OnlineRecognizer.from_transducer(**kwargs)
        self.options = options
        self.sessions = {}
        self.next_session = 1
        self._prewarm()

    def _prewarm(self):
        stream = self.recognizer.create_stream()
        stream.accept_waveform(16000, np.zeros(16000, dtype=np.float32))
        self._decode(stream)

    def _decode(self, stream):
        while self.recognizer.is_ready(stream):
            self.recognizer.decode_stream(stream)

    def _text(self, stream):
        result = self.recognizer.get_result(stream)
        return ((result if isinstance(result, str) else result.text) or "").strip()

    def create(self):
        sid = self.next_session
        self.next_session += 1
        self.sessions[sid] = {"stream": self.recognizer.create_stream(), "partial": "", "final": ""}
        return sid

    def emit_result(self, sid, allow_endpoint=True):
        session = self.sessions[sid]
        text = self._text(session["stream"])
        if text and text != session["partial"]:
            session["partial"] = text
            send({"type": "event", "session_id": sid, "event": {"type": "partial", "stage": "partial", "text": text}})
        if allow_endpoint and self.recognizer.is_endpoint(session["stream"]):
            if text and text != session["final"]:
                session["final"] = text
                send({"type": "event", "session_id": sid, "event": {"type": "final", "stage": "first_pass_final", "text": text}})
            self.recognizer.reset(session["stream"])
            session["partial"] = ""
            session["final"] = ""

    def audio(self, sid, pcm):
        if len(pcm) % 2:
            raise RuntimeError("PCM16 audio must contain an even number of bytes")
        stream = self.sessions[sid]["stream"]
        samples = np.frombuffer(pcm, dtype="<i2").astype(np.float32) / 32768.0
        stream.accept_waveform(16000, samples)
        self._decode(stream)
        self.emit_result(sid)

    def stop(self, sid):
        session = self.sessions[sid]
        stream = session["stream"]
        stream.accept_waveform(16000, np.zeros(6400, dtype=np.float32))
        self._decode(stream)
        self.emit_result(sid)
        stream.input_finished()
        self._decode(stream)
        self.emit_result(sid, False)
        text = self._text(stream)
        if text and text != session["final"]:
            send({"type": "event", "session_id": sid, "event": {"type": "final", "stage": "first_pass_final", "text": text}})
        del self.sessions[sid]


worker = None
for raw_line in sys.stdin:
    try:
        message = json.loads(raw_line)
        kind = message.get("type")
        if kind == "shutdown":
            break
        if kind == "init":
            worker = Worker(message["options"])
            lm = message["options"].get("lm") or {}
            send({"type": "ready", "max_active_paths": message["options"].get("maxActivePaths", 4),
                  "lm_requested": bool(lm.get("requested")), "lm_enabled": bool(lm.get("active")),
                  "lm_type": lm.get("type"), "lm_path": lm.get("path"), "lm_scale": lm.get("scale"),
                  "lm_reason": lm.get("reason"), **metrics()})
            continue
        if worker is None:
            raise RuntimeError("worker is not initialized")
        request_id = message["request_id"]
        started = time.perf_counter()
        cpu_started = time.process_time()
        if kind == "create":
            response = {"session_id": worker.create()}
        elif kind == "audio":
            worker.audio(int(message["session_id"]), base64.b64decode(message["pcm"], validate=True))
            response = {}
        elif kind == "stop":
            worker.stop(int(message["session_id"]))
            response = {}
        else:
            raise RuntimeError("unsupported worker message")
        send({"request_id": request_id, **response, "decode_ms": round((time.perf_counter() - started) * 1000, 3),
              "cpu_ms": round((time.process_time() - cpu_started) * 1000, 3), **metrics()})
    except Exception as exc:
        if worker is None:
            send({"type": "fatal", "error": str(exc)})
        else:
            send({"request_id": message.get("request_id"), "error": str(exc), **metrics()})
