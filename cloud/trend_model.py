"""Cloud inference layer: reads readings from Firebase, computes energy, carbon, Carbon Credit Score,
and predicts monthly emissions with a scikit-learn linear regression.
Usage: python trend_model.py --url https://YOUR-PROJECT-default-rtdb.firebaseio.com --ef 0.71 --baseline 100
"""
import argparse, json, urllib.parse, urllib.request
import numpy as np
from sklearn.linear_model import LinearRegression

def fetch(url, device):
    q = urllib.parse.quote('"$key"')
    with urllib.request.urlopen(f"{url.rstrip('/')}/readings/{device}.json?orderBy={q}&limitToLast=1000", timeout=15) as r:
        data = json.load(r) or {}
    return sorted((v for v in data.values() if v and "ts" in v), key=lambda v: v["ts"])

def _num(v):
    return isinstance(v, (int, float)) and not isinstance(v, bool) and np.isfinite(v)

def analyze(rows, ef, baseline_w):
    rows = [r for r in rows if _num(r.get("ts")) and _num(r.get("power_w"))]   # drop unusable rows
    if len(rows) < 2:
        raise ValueError("Need at least 2 valid readings")
    ts = np.array([r["ts"] for r in rows], dtype=float)
    p = np.array([r["power_w"] for r in rows], dtype=float)
    idle = np.array([r.get("idle_waste", 0) for r in rows], dtype=float)
    gas = np.array([r["gas_raw"] if _num(r.get("gas_raw")) and r["gas_raw"] > 0 else np.nan for r in rows])
    dt_h = np.diff(ts) / 3.6e6
    dt_h[dt_h > 5 / 60] = 0                       # ignore gaps longer than 5 min
    e = p[1:] * dt_h / 1000                       # kWh per interval
    kwh, idle_kwh = e.sum(), (e * idle[1:]).sum()
    active = np.concatenate([[0], np.cumsum(dt_h)])   # elapsed hours with gaps removed, to match the energy
    hours = active[-1]
    avg_w = kwh / hours * 1000 if hours > 0 else 0
    g0, g1 = gas[:10][np.isfinite(gas[:10])], gas[-5:][np.isfinite(gas[-5:])]
    gas_base = np.median(g0) if g0.size else 0; gas_now = g1.mean() if g1.size else 0   # missing/zero gas -> no penalty
    idle_ratio = idle_kwh / kwh if kwh > 0 else 0
    over = min(max((avg_w - baseline_w) / baseline_w, 0), 1)
    gas_pen = min(max((gas_now - gas_base) / gas_base / 0.5, 0), 1) if gas_base > 0 and gas_now > 0 else 0
    score = round(float(np.clip(100 - 60 * idle_ratio - 25 * over - 15 * gas_pen, 0, 100)))
    cum = np.concatenate([[0], np.cumsum(e)])
    x = active.reshape(-1, 1)
    model = LinearRegression().fit(x, cum)
    month_kwh = max(model.coef_[0], 0) * 24 * 30
    return dict(readings=len(rows), kwh=kwh, idle_kwh=idle_kwh, co2_kg=kwh * ef, avg_w=avg_w,
                score=score, r2=model.score(x, cum), month_kwh=month_kwh, month_co2_kg=month_kwh * ef)

if __name__ == "__main__":
    ap = argparse.ArgumentParser()
    ap.add_argument("--url", required=True); ap.add_argument("--device", default="ccis01")
    ap.add_argument("--ef", type=float, default=0.71); ap.add_argument("--baseline", type=float, default=100)
    a = ap.parse_args()
    rows = fetch(a.url, a.device)
    if len(rows) < 5:
        raise SystemExit("Need at least 5 readings")
    for k, v in analyze(rows, a.ef, a.baseline).items():
        print(f"{k:14s} {v:.4f}" if isinstance(v, float) else f"{k:14s} {v}")
