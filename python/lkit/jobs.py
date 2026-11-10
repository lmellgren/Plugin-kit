"""Measurement jobs: settings x levels -> one .npz per render + MANIFEST.json (from Passive EQ run_job.py).

Job file (JSON):
{
  "name": "p2_hpf",
  "plugin": "aumf:PR73:Artu",            # AU: name, type:sub:manu or .component path;
                                         # "render:<executable>" for an offline core render (text job)
  "map": "pre1973",                      # tools/maps/<map>.json; null -> settings give raw "au" dicts
  "sample_rate": 48000,
  "sweep": {"f1": 10, "f2": 22000, "T": 3.0},
  "levels_dbfs": [-30],
  "settings": [{"id": "flat", "panel": {}, "holdout": false, "family": "flat"}],
  "block_size": 512, "settle_s": 0.5, "preroll_s": 0.5, "tail_s": 2.0,
  "repeat": 1,                           # measure every setting N times
  "channels": "both"                     # analyse channel 0 (default) and keep the L/R difference
}
A setting may carry "change": {"at_s": 1.0, "panel": {...}}: the AU host applies those controls at that time
during the render (settle-time measurements; use keep_raw to look at the waveform).

Output: <data>/runs/<name>-<timestamp>/  (gitignored)
  <setting>__L<level>[__r<i>].npz   freqs, H1..H5 (complex), ir, latency, settings, plugin
  MANIFEST.json                     run description
and a one-line entry appended to <data>/MANIFEST.json (committed).

Offline renders ("render:<exe>") get a text job with one line per run:
  sample_rate 48000 / block_size 512 / preroll_s 0.5 / tail_s 2 / stimulus PATH
  run NAME GAIN key=value key=value ...   (the setting's panel values)
and must write NAME.f32 (interleaved float32) + NAME.json (as lkit_measure) into the output folder.
"""
from __future__ import annotations

import datetime as dt
import json
import shutil
import subprocess
import time
from pathlib import Path

import numpy as np

from . import host
from .sweep import Sweep, analyze, log_grid

HARMONICS = (2, 3, 4, 5)


def level_tag(db: float) -> str:
    return ("m" if db < 0 else "p") + f"{abs(db):g}".replace(".", "p")


def render_job_text(host_job: dict) -> str:
    L = [f"sample_rate {host_job['sample_rate']}", f"block_size {host_job['block_size']}",
         f"preroll_s {host_job['preroll_s']}", f"tail_s {host_job['tail_s']}", f"stimulus {host_job['stimulus']}"]
    for r in host_job["runs"]:
        p = r["meta"]["setting"].get("panel", {})
        kv = " ".join(f"{k}={str(v).replace(' ', '_')}" for k, v in sorted(p.items()))
        L.append(f"run {r['name']} {r['gain']!r} {kv}".rstrip())
    return "\n".join(L) + "\n"


def build_runs(job: dict, pmap: dict | None) -> list:
    runs = []
    offline = str(job["plugin"]).startswith("render:")
    for s in job["settings"]:
        if offline:
            au = {}
        elif pmap:
            au = host.to_au_params(s.get("panel", {}), pmap)
        else:
            au = {str(k): v for k, v in s.get("au", {}).items()}
        for lvl in job.get("levels_dbfs", [-40]):
            n_rep = int(job.get("repeat", 1))
            for r in range(n_rep):
                name = f"{s['id']}__L{level_tag(lvl)}" + (f"__r{r}" if n_rep > 1 else "")
                run = {"name": name, "params": au, "gain": 10 ** (lvl / 20),
                       "meta": {"setting": s, "level_dbfs": lvl, "repeat": r}}
                if s.get("change") and not offline:   # mid-render change (AU host only)
                    merged = {**s.get("panel", {}), **s["change"]["panel"]}
                    after = host.to_au_params(merged, pmap) if pmap else dict(au, **s["change"].get("au", {}))
                    run["change"] = {"at_s": s["change"]["at_s"],
                                     "params": {k: v for k, v in after.items() if au.get(k) != v}}
                runs.append(run)
    return runs


def run_job(job: dict, out: Path | None = None, chunk: int = 40, keep_raw: bool = False, root: Path | None = None) -> Path:
    fs = float(job.get("sample_rate", 48000))
    sw = Sweep(fs=fs, **job.get("sweep", {}))
    freqs = log_grid(sw.f1, sw.f2)
    pmap = host.load_map(job["map"]) if job.get("map") else None
    data = host.paths()["data"]
    stamp = dt.datetime.now().strftime("%Y%m%d-%H%M%S")
    out = Path(out) if out else data / "runs" / f"{job['name']}-{stamp}"
    raw = out / "raw"
    raw.mkdir(parents=True, exist_ok=True)
    stim = out / "stimulus.f32"
    sw.x.astype(np.float32).tofile(stim)

    runs = build_runs(job, pmap)
    t0 = time.time()
    results, plugin_info = [], None
    for c in range(0, len(runs), chunk):
        part = runs[c:c + chunk]
        host_job = {
            "sample_rate": fs, "block_size": job.get("block_size", 512),
            "settle_s": job.get("settle_s", 0.5), "preroll_s": job.get("preroll_s", 0.5),
            "tail_s": job.get("tail_s", 2.0), "stimulus": str(stim), "runs": part,
        }
        if str(job["plugin"]).startswith("render:"):
            hj = raw / f"render_job_{c // chunk:04d}.txt"
            hj.write_text(render_job_text(host_job))
            p = subprocess.run([job["plugin"][len("render:"):], str(hj), str(raw)], capture_output=True, text=True)
            if p.returncode != 0:
                print(p.stderr[-2000:])
                raise RuntimeError("offline render failed")
        else:
            hj = raw / f"host_job_{c // chunk:04d}.json"
            hj.write_text(json.dumps(host_job))
            host.run_host("render", "--plugin", job["plugin"], "--job", str(hj), "--out", str(raw))
        for r in part:
            meta = json.loads((raw / f"{r['name']}.json").read_text())
            plugin_info = meta["plugin"]
            ch = meta["channels"]
            y = np.fromfile(raw / f"{r['name']}.f32", np.float32).reshape(-1, ch).astype(float)
            an = analyze(sw, y[:, 0], r["gain"], freqs=freqs, harmonics=HARMONICS)
            lr = 0.0
            if ch > 1:
                lr = float(np.max(np.abs(y[:, 1] - y[:, 0])) / max(np.max(np.abs(y[:, 0])), 1e-30))
            s = r["meta"]["setting"]
            np.savez_compressed(
                out / f"{r['name']}.npz",
                freqs=freqs, H1=an.H[1], **{f"H{k}": an.H[k] for k in HARMONICS},
                ir=an.ir, fs=fs,
                latency_reported=meta["latency_reported_s"] * fs,
                latency_peak=an.latency_peak, peak_dbfs=an.peak_dbfs,
                level_dbfs=r["meta"]["level_dbfs"], gain=r["gain"],
                setting_id=s["id"], panel=json.dumps(s.get("panel", {})), setting=json.dumps(s),
                family=s.get("family", ""), holdout=bool(s.get("holdout", False)), au_params=json.dumps(r["params"]),
                readback=json.dumps(meta.get("readback", {})), params_ok=bool(meta.get("params_ok", True)),
                lr_rel_diff=lr, plugin=json.dumps(plugin_info), sweep=json.dumps(sw.meta()),
            )
            if not keep_raw:
                (raw / f"{r['name']}.f32").unlink()
            results.append({"file": f"{r['name']}.npz", "setting": s["id"], "family": s.get("family", ""),
                            "level_dbfs": r["meta"]["level_dbfs"], "repeat": r["meta"]["repeat"],
                            "holdout": bool(s.get("holdout", False)), "params_ok": bool(meta.get("params_ok", True)),
                            "latency_reported": meta["latency_reported_s"] * fs,
                            "latency_peak": round(an.latency_peak, 3)})
        print(f"  {min(c + chunk, len(runs))}/{len(runs)} renders, {time.time() - t0:.0f} s", flush=True)

    manifest = {
        "name": job["name"], "date": dt.datetime.now().isoformat(timespec="seconds"),
        "job": job, "plugin": plugin_info, "sweep": sw.meta(),
        "grid": {"f_lo": float(freqs[0]), "f_hi": float(freqs[-1]), "per_octave": 48, "n": int(freqs.size)},
        "n_renders": len(results), "results": results,
        "failed_params": [r["file"] for r in results if not r["params_ok"]],
        "elapsed_s": round(time.time() - t0, 1),
    }
    (out / "MANIFEST.json").write_text(json.dumps(manifest, indent=1))
    if not keep_raw:
        shutil.rmtree(raw)

    if out.resolve().is_relative_to(data.resolve()):
        top = data / "MANIFEST.json"
        entries = json.loads(top.read_text())["runs"] if top.exists() else []
        rel = out.resolve().relative_to((root or data.resolve().parent).resolve())
        entries.append({"dir": str(rel), "name": job["name"], "date": manifest["date"],
                        "plugin": plugin_info, "renders": len(results), "levels_dbfs": job.get("levels_dbfs", [-40]),
                        "settings": [s["id"] for s in job["settings"]] if len(job["settings"]) <= 20
                        else f"{len(job['settings'])} settings",
                        "failed_params": len(manifest["failed_params"])})
        top.write_text(json.dumps({"runs": entries}, indent=1))
    print(f"wrote {out} ({len(results)} renders, {len(manifest['failed_params'])} param failures)")
    return out


def load_run(run_dir: Path) -> list:
    """All renders of a run as dicts of numpy arrays (np.load results)."""
    man = json.loads((Path(run_dir) / "MANIFEST.json").read_text())
    return [dict(np.load(Path(run_dir) / r["file"], allow_pickle=False)) for r in man["results"]]


def main(argv=None) -> None:
    import argparse
    ap = argparse.ArgumentParser(description="Run a measurement job")
    ap.add_argument("job")
    ap.add_argument("--out", help="run directory (default <data>/runs/<name>-<timestamp>)")
    ap.add_argument("--chunk", type=int, default=40, help="renders per host call")
    ap.add_argument("--keep-raw", action="store_true")
    a = ap.parse_args(argv)
    run_job(json.loads(Path(a.job).read_text()), Path(a.out) if a.out else None, a.chunk, a.keep_raw)
