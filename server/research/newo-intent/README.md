# Newo intent training assets

This is a broad router seed set for present and planned Newo capabilities; it is deliberately not camera-only. It classifies intent but never authorizes actions. Deterministic policy must still validate slots, permissions and destructive operations.

On the RTX 3060 machine:

```bash
python -m venv .venv
source .venv/Scripts/activate
python -m pip install -r requirements.txt
python train.py --output artifacts/newo-intent-setfit
```

The script makes a stratified holdout, prints per-class metrics, and writes both a normal SetFit directory and `model.onnx`. Add real phrasing and hard negatives before treating the scores as production-calibrated. The existing Newo capability-router endpoint can remain disabled until the trained directory is copied to the VPS and benchmarked there.
