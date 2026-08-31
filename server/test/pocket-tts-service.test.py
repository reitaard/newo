#!/usr/bin/env python3
"""Focused ownership test; run with the isolated Pocket virtual environment."""
from __future__ import annotations
import importlib.util
import io
import threading
import time
from contextlib import redirect_stdout
from pathlib import Path
import torch

spec = importlib.util.spec_from_file_location("pocket_tts_service", Path(__file__).parents[1] / "pocket-tts-service.py")
service = importlib.util.module_from_spec(spec)
assert spec.loader is not None
spec.loader.exec_module(service)

class FakeModel:
    def __init__(self):
        self.entered: list[str] = []
        self.drain_started = threading.Event()
        self.allow_drain = threading.Event()

    def generate_audio_stream(self, _state, text):
        self.entered.append(text)
        yield torch.tensor([0.25], dtype=torch.float32)
        if text == "first":
            self.drain_started.set()
            assert self.allow_drain.wait(2), "test did not release discarded generation"
        yield torch.tensor([0.5], dtype=torch.float32)

model = FakeModel()
lock = threading.Lock()
first = service.serialized_native_stream(model, {}, "first", playback_id="cancel-test", lock=lock)
assert next(first), "first consumer must receive its first native chunk"

close_thread = threading.Thread(target=first.close)
close_thread.start()
assert model.drain_started.wait(1), "close must drain the remaining Pocket iterator"

second_entered = threading.Event()
def second_request():
    second = service.serialized_native_stream(model, {}, "second", playback_id="second-test", lock=lock)
    next(second)
    second_entered.set()
    second.close()

second_thread = threading.Thread(target=second_request)
second_thread.start()
assert not second_entered.wait(0.1), "new generation entered before cancelled generation drained"
assert model.entered == ["first"], "same Pocket model must remain exclusive while draining"
model.allow_drain.set()
close_thread.join(2)
second_thread.join(2)
assert not close_thread.is_alive() and not second_thread.is_alive()
assert second_entered.is_set()
assert model.entered == ["first", "second"]
class DelayedModel:
    def generate_audio_stream(self, _state, _text):
        yield torch.tensor([0.25], dtype=torch.float32)
        time.sleep(0.12)
        yield torch.tensor([0.5], dtype=torch.float32)

continuity_log = io.StringIO()
with redirect_stdout(continuity_log):
    assert list(service.serialized_native_stream(DelayedModel(), {}, "continuity", playback_id="native-gap", lock=threading.Lock()))
line = continuity_log.getvalue()
assert "POCKET_NATIVE_CONTINUITY playback_id=native-gap outcome=completed chunks=2" in line
assert "over_80=1" in line
print("PASS: cancelled client stream drains before exclusive model ownership is released")
print("PASS: native Pocket continuity aggregates delayed tensor gaps")
