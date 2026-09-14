#!/usr/bin/env python3
"""Persistent CPU-only inference service for the frozen SetFit v2 router."""

import argparse
import json
import os
import sys
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path

MAX_BODY_BYTES = 4096


def load_registry(path):
    document = json.loads(Path(path).read_text(encoding="utf-8"))
    capabilities = document["capabilities"]
    ids = [item["id"] for item in capabilities]
    return document["abstain_label"], ids, {item["id"]: item["source_need"] for item in capabilities}


def normalize_prediction(vector, labels, abstain_label, source_needs, latency_ms):
    probabilities = [float(value) for value in vector]
    if len(probabilities) != len(labels):
        raise RuntimeError(f"probability width {len(probabilities)} does not match {len(labels)} labels")
    ranked = sorted(range(len(probabilities)), key=lambda index: probabilities[index], reverse=True)
    best = ranked[0]
    raw_primary = labels[best]
    confidence = float(probabilities[best])
    margin = float(probabilities[best] - probabilities[ranked[1]]) if len(ranked) > 1 else confidence
    abstain = raw_primary == abstain_label
    primary = None if abstain else raw_primary
    return {
        "primary": primary,
        "raw_primary": raw_primary,
        "capabilities": [] if abstain else [raw_primary],
        "scores": {label: float(probabilities[index]) for index, label in enumerate(labels)},
        "confidence": confidence,
        "margin": margin,
        "source_need": "unknown" if abstain else source_needs[raw_primary],
        "abstain": abstain,
        "latency_ms": latency_ms,
    }


class RouterRuntime:
    def __init__(self, model_path, registry_path):
        os.environ["CUDA_VISIBLE_DEVICES"] = ""
        import numpy as np
        from setfit import SetFitModel

        self.np = np
        self.abstain_label, capability_labels, self.source_needs = load_registry(registry_path)
        self.model_path = str(Path(model_path).resolve())
        started = time.perf_counter()
        self.model = SetFitModel.from_pretrained(self.model_path, device="cpu")
        self.load_ms = (time.perf_counter() - started) * 1000
        self.labels = list(self.model.labels or [])
        expected = capability_labels + [self.abstain_label]
        if self.labels != expected:
            raise RuntimeError(f"model label order mismatch: expected {expected}, got {self.labels}")
        warm_started = time.perf_counter()
        self.model.predict_proba(["warm up the capability classifier"], show_progress_bar=False)
        self.warmup_ms = (time.perf_counter() - warm_started) * 1000

    def route(self, text):
        started = time.perf_counter()
        values = self.model.predict_proba([text], as_numpy=True, show_progress_bar=False)
        latency_ms = (time.perf_counter() - started) * 1000
        vector = values if self.np.asarray(values).ndim == 1 else values[0]
        return normalize_prediction(vector, self.labels, self.abstain_label, self.source_needs, latency_ms)


class RouterState:
    def __init__(self, model_path, registry_path):
        self.model_path = str(Path(model_path).resolve())
        self.runtime = None
        self.status = "model_missing" if not Path(model_path).is_dir() else "model_unavailable"
        self.error_type = None
        if self.status == "model_missing":
            return
        try:
            self.runtime = RouterRuntime(model_path, registry_path)
            self.status = "ready"
        except Exception as error:
            self.error_type = type(error).__name__
            print(json.dumps({"event": "capability_router_not_ready", "status": self.status,
                              "error_type": self.error_type}), file=sys.stderr, flush=True)


def handler_for(state):
    class Handler(BaseHTTPRequestHandler):
        server_version = "NewoCapabilityRouter/2"

        def log_message(self, fmt, *args):
            print(json.dumps({"event": "capability_router_http", "message": fmt % args}), flush=True)

        def respond(self, status, body):
            payload = json.dumps(body, separators=(",", ":")).encode("utf-8")
            self.send_response(status)
            self.send_header("Content-Type", "application/json")
            self.send_header("Content-Length", str(len(payload)))
            self.send_header("Cache-Control", "no-store")
            self.end_headers()
            self.wfile.write(payload)

        def do_GET(self):
            if self.path != "/healthz":
                return self.respond(404, {"error": {"code": "not_found"}})
            if not state.runtime:
                return self.respond(503, {"status": state.status, "model": "setfit-minilm-router-v2",
                                          "device": "cpu", "error": {"code": state.status}})
            runtime = state.runtime
            return self.respond(200, {"status": "ready", "model": "setfit-minilm-router-v2", "device": "cpu",
                                      "labels": len(runtime.labels), "load_ms": runtime.load_ms, "warmup_ms": runtime.warmup_ms})

        def do_POST(self):
            if self.path != "/v1/route":
                return self.respond(404, {"error": {"code": "not_found"}})
            if not state.runtime:
                return self.respond(503, {"error": {"code": state.status}})
            try:
                length = int(self.headers.get("Content-Length", "0"))
                if length < 2 or length > MAX_BODY_BYTES:
                    return self.respond(413, {"error": {"code": "invalid_request_size"}})
                body = json.loads(self.rfile.read(length))
                text = body.get("text")
                if not isinstance(text, str) or not text.strip() or len(text) > 800:
                    return self.respond(400, {"error": {"code": "invalid_text"}})
                return self.respond(200, state.runtime.route(text.strip()))
            except json.JSONDecodeError:
                return self.respond(400, {"error": {"code": "invalid_json"}})
            except Exception as error:
                print(json.dumps({"event": "capability_router_error", "error_type": type(error).__name__}),
                      file=sys.stderr, flush=True)
                return self.respond(500, {"error": {"code": "inference_failed"}})

    return Handler


def main():
    parser = argparse.ArgumentParser(description="Serve the frozen Newo SetFit v2 capability router")
    parser.add_argument("--model-path", default=os.getenv("CAPABILITY_ROUTER_MODEL_PATH", "/srv/newo-models/capability-router-v2"))
    parser.add_argument("--registry", default=os.getenv("CAPABILITY_ROUTER_REGISTRY", "config/capability-router.json"))
    parser.add_argument("--host", default=os.getenv("CAPABILITY_ROUTER_HOST", "127.0.0.1"))
    parser.add_argument("--port", type=int, default=int(os.getenv("CAPABILITY_ROUTER_PORT", "8791")))
    args = parser.parse_args()
    state = RouterState(args.model_path, args.registry)
    event = "capability_router_ready" if state.runtime else "capability_router_degraded"
    detail = {"event": event, "status": state.status, "model": "setfit-minilm-router-v2", "device": "cpu"}
    if state.runtime:
        detail.update({"load_ms": state.runtime.load_ms, "warmup_ms": state.runtime.warmup_ms,
                       "labels": len(state.runtime.labels)})
    print(json.dumps(detail), flush=True)
    ThreadingHTTPServer((args.host, args.port), handler_for(state)).serve_forever()


if __name__ == "__main__":
    main()
