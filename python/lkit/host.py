"""Measurement host invocation and parameter maps (LOMAS Plugin-kit; from Passive EQ peqm.py).

A plugin repo points the kit at its build and data folders:

    from lkit import host
    host.configure(measure="build/kit/measure/lkit_measure", data="data", maps="tools/maps")

Parameter maps (tools/maps/<name>.json) translate settings in panel units into AU parameter values:

    {"plugin": "aumf:PR73:Artu",
     "controls": {
        "high_gain": {"id": 12, "kind": "linear", "panel": [-16, 16], "au": [-16, 16], "flat": 0},
        "hpf":       {"id": 7,  "kind": "selector", "positions": {"Off": 0, "50": 0.1}, "flat": "Off"},
        "type":      {"id": 3,  "kind": "raw", "flat": 0},
        "quality":   {"id": 30, "kind": "fixed", "au": 1}}}

kind: linear (panel range -> AU range), selector (label -> AU value), raw (panel value = AU value),
fixed (always this AU value; not settable from a setting).
"""
from __future__ import annotations

import json
import os
import subprocess
from pathlib import Path

_cfg = {
    "measure": Path(os.environ.get("LKIT_MEASURE", "build/kit/measure/lkit_measure")),
    "data": Path(os.environ.get("LKIT_DATA", "data")),
    "maps": Path(os.environ.get("LKIT_MAPS", "tools/maps")),
}


def configure(measure: str | Path | None = None, data: str | Path | None = None, maps: str | Path | None = None) -> None:
    for k, v in (("measure", measure), ("data", data), ("maps", maps)):
        if v is not None:
            _cfg[k] = Path(v)


def paths() -> dict:
    return dict(_cfg)


def run_host(*args: str, quiet: bool = True) -> subprocess.CompletedProcess:
    """Run lkit_measure. Plugins print a lot of objc noise on stderr; keep only our lines."""
    p = subprocess.run([str(_cfg["measure"]), *args], capture_output=True, text=True)
    lines = [l for l in p.stderr.splitlines() if l.startswith(("error", "warning", "ok  ", "FAIL"))]
    if not quiet or p.returncode != 0:
        for l in lines:
            print("  host:", l)
    if p.returncode != 0:
        raise RuntimeError(f"lkit_measure {' '.join(args[:1])} failed with {p.returncode}")
    return p


def load_map(name: str) -> dict:
    return json.loads((_cfg["maps"] / f"{name}.json").read_text())


def to_au_params(setting: dict, pmap: dict) -> dict:
    """Setting in panel units -> {AU id (str): value}. Unspecified controls take the map's flat value."""
    out = {}
    controls = pmap["controls"]
    unknown = set(setting) - set(controls)
    if unknown:
        raise KeyError(f"unknown controls: {sorted(unknown)}")
    for name, c in controls.items():
        kind = c["kind"]
        if kind == "fixed":
            au = c["au"]
        else:
            v = setting.get(name, c.get("flat"))
            if v is None:
                continue
            if kind == "linear":
                p0, p1 = c["panel"]
                a0, a1 = c["au"]
                au = a0 + (a1 - a0) * (float(v) - p0) / (p1 - p0)
            elif kind == "selector":
                if str(v) not in c["positions"]:
                    raise KeyError(f"{name}: no position {v!r} (have {list(c['positions'])})")
                au = c["positions"][str(v)]
            elif kind == "raw":
                au = v
            else:
                raise ValueError(f"{name}: unknown kind {kind}")
        out[str(c["id"])] = float(au)
    return out
