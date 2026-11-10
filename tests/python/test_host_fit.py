"""Parameter maps (panel units -> AU values) and the staged fit's holdout guard."""
import sys
from pathlib import Path

import numpy as np
import pytest

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "python"))
from lkit import host  # noqa: E402
from lkit.fit import HoldoutError, Param, StagedFit  # noqa: E402
from lkit.jobs import build_runs, level_tag, render_job_text  # noqa: E402

PMAP = {"controls": {
    "gain": {"id": 3, "kind": "linear", "panel": [-16, 16], "au": [0, 1], "flat": 0},
    "hpf": {"id": 7, "kind": "selector", "positions": {"Off": 0, "50": 0.25}, "flat": "Off"},
    "type": {"id": 9, "kind": "raw", "flat": 1},
    "quality": {"id": 30, "kind": "fixed", "au": 2},
}}


def test_to_au_params():
    assert host.to_au_params({}, PMAP) == {"3": 0.5, "7": 0.0, "9": 1.0, "30": 2.0}
    assert host.to_au_params({"gain": 16, "hpf": "50"}, PMAP)["3"] == 1.0
    assert host.to_au_params({"gain": 16, "hpf": "50"}, PMAP)["7"] == 0.25
    with pytest.raises(KeyError):
        host.to_au_params({"bogus": 1}, PMAP)
    with pytest.raises(KeyError):
        host.to_au_params({"hpf": "60"}, PMAP)


def test_list_ids_and_change():
    pm = {"controls": {"g": {"id": [6, 17], "kind": "linear", "panel": [-80, -20], "au": [0, 1], "flat": -80}}}
    assert host.to_au_params({"g": -50}, pm) == {"6": 0.5, "17": 0.5}
    job = {"plugin": "aumf:X:Y", "levels_dbfs": [-30],
           "settings": [{"id": "s", "panel": {"g": -80}, "change": {"at_s": 1.0, "panel": {"g": -20}}}]}
    run = build_runs(job, pm)[0]
    assert run["change"] == {"at_s": 1.0, "params": {"6": 1.0, "17": 1.0}}


def test_runs_and_text_job():
    job = {"plugin": "render:/bin/true", "levels_dbfs": [-30, 0], "repeat": 2,
           "settings": [{"id": "a", "panel": {"hpf": "50", "gain": 3}}]}
    runs = build_runs(job, None)
    assert [r["name"] for r in runs] == ["a__Lm30__r0", "a__Lm30__r1", "a__Lp0__r0", "a__Lp0__r1"]
    assert level_tag(-4.5) == "m4p5"
    text = render_job_text({"sample_rate": 48000, "block_size": 512, "preroll_s": 0.5, "tail_s": 2,
                            "stimulus": "/x.f32", "runs": runs[:1]})
    assert text.splitlines()[-1].startswith("run a__Lm30__r0 0.0316")
    assert text.splitlines()[-1].endswith("gain=3 hpf=50")


def test_staged_fit_and_holdout():
    true = {"a": 2.0, "b": 0.5}
    xs = np.linspace(0, 1, 20)

    def residual(v, settings):
        return np.concatenate([v["a"] * np.exp(-v["b"] * xs * s["k"]) - true["a"] * np.exp(-true["b"] * xs * s["k"])
                               for s in settings])

    grid = [{"id": "g1", "k": 1.0}, {"id": "g2", "k": 3.0}]
    fit = StagedFit([Param("a", 1.0, 0.1, 10), Param("b", 1.0, 0.01, 5)], residual)
    fit.stage("a only", ["a"], grid)
    r = fit.stage("both", ["a", "b"], grid)
    assert abs(fit.values["a"] - 2.0) < 1e-6 and abs(fit.values["b"] - 0.5) < 1e-6
    assert r.cost_after < 1e-12
    with pytest.raises(HoldoutError):
        fit.stage("bad", ["a"], grid + [{"id": "h1", "k": 2.0, "holdout": True}])
