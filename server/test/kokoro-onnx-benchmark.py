#!/usr/bin/env python3
"""Side-by-side CPU benchmark for Newo's current and ONNX Kokoro backends.

This deliberately does not start a service or alter Newo production.  It uses
Kokoro's published Misaki English G2P + vocab mapping, the exact am_michael
style vector, and emits native 24 kHz mono signed PCM16 LE results.
"""
import argparse
import json
import os
import statistics
import subprocess
import sys
import threading
import time
from pathlib import Path

import numpy as np
import onnxruntime as ort
import psutil
import requests
from misaki import en, espeak

SAMPLE_RATE = 24_000
PCM_LIMIT = 2_880_000
TEXTS = {
    "short": "Face curious.",
    "medium": "Hello, I'm Newo. This is a realtime voice latency test.",
    "long": "Newo status: Wi-Fi is connected, the speaker is ready, voice recognition is armed, battery telemetry is normal, and the local assistant is responding with low-latency speech.",
}


def median_worst(values):
    return {"median_ms": round(statistics.median(values), 2), "worst_ms": round(max(values), 2), "values_ms": [round(v, 2) for v in values]}


def pcm_metrics(pcm):
    samples = np.frombuffer(pcm, dtype="<i2")
    peak = int(np.abs(samples.astype(np.int32)).max())
    rms = float(np.sqrt(np.mean(samples.astype(np.float64) ** 2)))
    dbfs = lambda value: float("-inf") if value == 0 else round(20 * np.log10(value / 32768), 3)
    return {
        "sample_rate": SAMPLE_RATE, "channels": 1, "byte_format": "s16le", "pcm_bytes": len(pcm),
        "audio_duration_ms": round(1000 * len(pcm) / (SAMPLE_RATE * 2), 2),
        "peak_dbfs": dbfs(peak), "rms_dbfs": dbfs(rms), "clipping": bool(np.any(np.abs(samples.astype(np.int32)) >= 32767)),
    }


def conditioned_stream(chunks, started_at):
    """Condition iterable PCM chunks and return actual first output timing."""
    process = subprocess.Popen([
        "ffmpeg", "-hide_banner", "-loglevel", "error", "-fflags", "nobuffer", "-flags", "low_delay",
        "-probesize", "32", "-analyzeduration", "0", "-f", "s16le", "-ac", "1", "-ar", str(SAMPLE_RATE), "-i", "pipe:0",
        "-af", "highpass=f=110,volume=2dB:precision=float,alimiter=limit=0.95:attack=5:release=50:level=false:latency=true",
        "-flush_packets", "1", "-f", "s16le", "-acodec", "pcm_s16le", "-ac", "1", "-ar", str(SAMPLE_RATE), "pipe:1",
    ], stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    output, first_output_at = [], [None]
    def read_output():
        while data := process.stdout.read(4096):
            if first_output_at[0] is None: first_output_at[0] = time.perf_counter()
            output.append(data)
    reader = threading.Thread(target=read_output)
    reader.start()
    for chunk in chunks:
        process.stdin.write(chunk)
        process.stdin.flush()
    process.stdin.close()
    reader.join(timeout=30)
    stderr = process.stderr.read()
    code = process.wait(timeout=30)
    pcm = b"".join(output)
    if code or not pcm or len(pcm) & 1 or first_output_at[0] is None:
        raise RuntimeError(f"ffmpeg conditioning failed: {stderr.decode().strip()}")
    return pcm, (first_output_at[0] - started_at) * 1000


def condition_pcm(raw):
    started = time.perf_counter()
    pcm, _ = conditioned_stream([raw], started)
    return pcm


class OnnxKokoro:
    def __init__(self, root, model_name):
        root = Path(root)
        started = time.perf_counter()
        self.session = ort.InferenceSession(str(root / "onnx" / model_name), providers=["CPUExecutionProvider"])
        self.load_ms = (time.perf_counter() - started) * 1000
        self.vocab = json.loads((root / "config.json").read_text()).get("vocab")
        if not self.vocab:
            # The ONNX repository has only a minimal config; use the upstream compatible vocabulary.
            self.vocab = requests.get("https://huggingface.co/hexgrad/Kokoro-82M/resolve/main/config.json", timeout=20).json()["vocab"]
        self.voice = np.fromfile(root / "voices" / "am_michael.bin", dtype=np.float32).reshape(-1, 1, 256)
        if self.voice.shape[0] < 510:
            raise RuntimeError(f"invalid am_michael voice data shape: {self.voice.shape}")
        # Match KPipeline(lang_code="a"): the eSpeak fallback supplies OOD words
        # such as Newo instead of silently dropping their phonemes.
        self.g2p = en.G2P(british=False, fallback=espeak.EspeakFallback(british=False), unk="")

    def synthesize(self, text):
        phonemes, _ = self.g2p(text)
        try:
            token_ids = [self.vocab[p] for p in phonemes]
        except KeyError as error:
            raise RuntimeError(f"unsupported phoneme: {error}") from error
        if not token_ids or len(token_ids) > 510:
            raise RuntimeError(f"invalid token length {len(token_ids)} for {text!r}")
        output = self.session.run(None, {
            "input_ids": np.asarray([[0, *token_ids, 0]], dtype=np.int64),
            "style": self.voice[len(token_ids)],
            "speed": np.ones(1, dtype=np.float32),
        })[0]
        waveform = np.asarray(output, dtype=np.float32).reshape(-1)
        if not len(waveform) or not np.isfinite(waveform).all() or np.max(np.abs(waveform)) > 2:
            raise RuntimeError("invalid ONNX waveform")
        return (np.clip(waveform, -1, 1) * 32767).astype("<i2").tobytes(), len(token_ids)


def benchmark_onnx(root, model_name, output_dir, warmups, runs):
    process = psutil.Process()
    engine = OnnxKokoro(root, model_name)
    rss_after_load = process.memory_info().rss
    result = {"backend": model_name, "model_file_bytes": (Path(root) / "onnx" / model_name).stat().st_size,
              "startup_load_ms": round(engine.load_ms, 2), "rss_after_load_bytes": rss_after_load,
              "onnxruntime": ort.__version__, "voice": "am_michael", "speed": 1.0, "texts": {}}
    for label, text in TEXTS.items():
        for _ in range(warmups):
            engine.synthesize(text)
        measurements = []
        final_pcm = None
        for _ in range(runs):
            cpu_before = process.cpu_times().user + process.cpu_times().system
            started = time.perf_counter()
            raw, token_count = engine.synthesize(text)
            inference_ms = (time.perf_counter() - started) * 1000
            pcm, first_pcm_ms = conditioned_stream([raw], started)
            synthesis_total_ms = (time.perf_counter() - started) * 1000
            conditioning_ms = synthesis_total_ms - inference_ms
            cpu_ms = ((process.cpu_times().user + process.cpu_times().system) - cpu_before) * 1000
            if len(pcm) > PCM_LIMIT:
                raise RuntimeError("PCM limit exceeded")
            measurements.append({"first_playable_pcm_ms": first_pcm_ms, "synthesis_total_ms": synthesis_total_ms,
                                 "inference_ms": inference_ms, "conditioning_ms": conditioning_ms, "cpu_ms": cpu_ms})
            final_pcm = pcm
        (Path(output_dir) / f"{model_name}-{label}.s16le").write_bytes(final_pcm)
        audio = pcm_metrics(final_pcm)
        duration = audio["audio_duration_ms"]
        result["texts"][label] = {"text": text, "token_count": token_count, "audio": audio,
            "first_playable_pcm": median_worst([m["first_playable_pcm_ms"] for m in measurements]),
            "synthesis_total": median_worst([m["synthesis_total_ms"] for m in measurements]),
            "cpu": median_worst([m["cpu_ms"] for m in measurements]),
            "realtime_factor_median": round(statistics.median(m["synthesis_total_ms"] for m in measurements) / duration, 4),
        }
    result["rss_after_runs_bytes"] = process.memory_info().rss
    return result


def cgroup_cpu_usec(container):
    container_id = subprocess.check_output(["docker", "inspect", "-f", "{{.Id}}", container], text=True).strip()
    paths = list(Path("/sys/fs/cgroup").glob(f"**/*{container_id}*/cpu.stat"))
    if not paths:
        raise RuntimeError("could not locate container cgroup CPU stats")
    values = dict(line.split() for line in paths[0].read_text().splitlines())
    return int(values["usage_usec"])


def baseline_request(base_url, text):
    cpu_before = cgroup_cpu_usec("newo-kokoro")
    started = time.perf_counter()
    response = requests.post(f"{base_url.rstrip('/')}/v1/audio/realtime", json={"model": "kokoro", "input": text, "voice": "am_michael", "response_format": "pcm", "speed": 1.0}, headers={"accept": "application/octet-stream"}, stream=True, timeout=(10, 90))
    response.raise_for_status()
    first_raw_ms = [None]
    raw_done_at = [None]
    def upstream_chunks():
        for chunk in response.iter_content(chunk_size=None):
            if chunk:
                if first_raw_ms[0] is None: first_raw_ms[0] = (time.perf_counter() - started) * 1000
                yield chunk
        raw_done_at[0] = time.perf_counter()
    # Feed FFmpeg while requests is still reading the HTTP body: this mirrors the
    # production stream and makes this an actual playable-PCM TTFA measurement.
    pcm, conditioned_first_ms = conditioned_stream(upstream_chunks(), started)
    done = time.perf_counter()
    cpu_ms = (cgroup_cpu_usec("newo-kokoro") - cpu_before) / 1000
    return pcm, {"first_playable_pcm_ms": conditioned_first_ms, "first_raw_pcm_ms": first_raw_ms[0],
                  "synthesis_total_ms": (done - started) * 1000,
                  "raw_complete_ms": (raw_done_at[0] - started) * 1000, "cpu_ms": cpu_ms}


def benchmark_baseline(base_url, output_dir, warmups, runs):
    inspect = json.loads(subprocess.check_output(["docker", "inspect", "newo-kokoro"], text=True))[0]
    # Docker retains daemon-specific memory units, so record it verbatim.
    docker_memory = subprocess.check_output(["docker", "stats", "--no-stream", "--format", "{{.MemUsage}}", "newo-kokoro"], text=True).strip()
    result = {"backend": "OpenTTSGroup float32 Kokoro", "voice": "am_michael", "speed": 1.0,
              "docker_memory_after_warm": docker_memory, "container_started_at": inspect["State"]["StartedAt"], "texts": {}}
    for label, text in TEXTS.items():
        for _ in range(warmups): baseline_request(base_url, text)
        measurements = []
        final_pcm = None
        for _ in range(runs):
            pcm, metric = baseline_request(base_url, text)
            measurements.append(metric); final_pcm = pcm
        (Path(output_dir) / f"float32-{label}.s16le").write_bytes(final_pcm)
        audio = pcm_metrics(final_pcm); duration = audio["audio_duration_ms"]
        result["texts"][label] = {"text": text, "audio": audio,
            "first_playable_pcm": median_worst([m["first_playable_pcm_ms"] for m in measurements]),
            "synthesis_total": median_worst([m["synthesis_total_ms"] for m in measurements]),
            "cpu": median_worst([m["cpu_ms"] for m in measurements]),
            "realtime_factor_median": round(statistics.median(m["synthesis_total_ms"] for m in measurements) / duration, 4),
        }
    return result


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--root", default="data/kokoro-onnx")
    parser.add_argument("--base-url", default="http://127.0.0.1:8010")
    parser.add_argument("--output", default="/tmp/newo-kokoro-benchmark")
    parser.add_argument("--warmups", type=int, default=5)
    parser.add_argument("--runs", type=int, default=5)
    args = parser.parse_args()
    Path(args.output).mkdir(parents=True, exist_ok=True)
    report = {"sample_rate": SAMPLE_RATE, "format": "mono signed PCM16 little-endian", "warmups_per_text": args.warmups, "measured_runs_per_text": args.runs, "results": []}
    report["results"].append(benchmark_baseline(args.base_url, args.output, args.warmups, args.runs))
    for name in ("model_q8f16.onnx", "model_quantized.onnx"):
        report["results"].append(benchmark_onnx(args.root, name, args.output, args.warmups, args.runs))
    print(json.dumps(report, indent=2))

if __name__ == "__main__": main()
