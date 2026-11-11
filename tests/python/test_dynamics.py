"""lkit.dynamics on synthetic compressors with known answers."""
import sys
from pathlib import Path

import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "python"))
from lkit import dynamics as D  # noqa: E402

FS, F0 = 48000, 1000.0


def test_staircase_levels_and_whole_cycles():
    x, segs = D.staircase([-40, -20, 0], 0.1, F0, FS)
    assert len(segs) == 3 and all((b - a) % 48 == 0 for a, b, _ in segs)
    st = D.segment_stats(x, x, FS, F0, segs, measure_s=0.05)
    for s, lv in zip(st, (-40, -20, 0)):
        assert abs(s["level_in_db"] - lv) < 1e-6 and abs(s["gain_db"]) < 1e-9


def test_static_curve_and_harmonics():
    x, segs = D.staircase(np.arange(-30, 1, 10.0), 0.1, F0, FS)
    env = np.zeros_like(x)
    for a, b, lv in segs:                      # a 2:1 static curve above -20 dBFS, plus 1 % 3rd harmonic
        env[a:b] = 10 ** (-max(lv + 20, 0) / 2 / 20)
    y = x * env
    t = np.arange(x.size) / FS
    y = y + 0.01 * np.sqrt(2) * 10 ** (np.repeat([s[2] for s in segs], [s[1] - s[0] for s in segs]) / 20) * env * np.sin(3 * 2 * np.pi * F0 * t)
    st = D.segment_stats(y, x, FS, F0, segs)
    gr = D.gr_readback([s["out_db"] for s in st], [s["level_in_db"] for s in st], 0.0)
    assert np.allclose(gr, [0, 0, 5, 10], atol=1e-6)
    assert all(abs(s["h_dbc"][3] + 40) < 0.01 for s in st)
    assert all(s["h_dbc"][2] < -150 for s in st)


def _compressor(x, segs, att, rel, gr_hi):
    """Gain in dB following a target (gr_hi dB on the loud segment) with first-order attack / release (dB domain)."""
    target = np.zeros(x.size)
    a, b, _ = segs[1]
    target[a:b] = -gr_hi
    g = np.zeros(x.size)
    ka, kr = 1 - np.exp(-1 / (att * FS)), 1 - np.exp(-1 / (rel * FS))
    for i in range(1, x.size):
        k = ka if target[i] < g[i - 1] else kr
        g[i] = g[i - 1] + k * (target[i] - g[i - 1])
    return x * 10 ** (g / 20)


def test_attack_and_release():
    x, segs = D.piecewise([(-30, 0.2), (-10, 0.3), (-30, 2.0)], F0, FS)
    y = _compressor(x, segs, 0.002, 0.3, 10.0)
    t, g = D.gain_trace(y, x, FS, F0, smooth_s=0.0002)
    t_on, t_off = segs[1][0] / FS, segs[1][1] / FS
    a = D.attack_time(t, g, t_on, t_off)
    assert abs(a["gr_db"] - 10) < 0.05 and abs(a["t_frac"] - 0.002) < 0.0006
    r = D.release_fit(t, g, t_off, t[-1])
    assert abs(r["tau_s"] / 0.3 - 1) < 0.03 and abs(r["t_63"] / 0.3 - 1) < 0.05


def test_two_slope_release():
    x, segs = D.piecewise([(-30, 0.1), (-10, 0.2), (-30, 6.0)], F0, FS)
    t = np.arange(x.size) / FS
    t_off = segs[1][1] / FS
    gr = np.where(t < segs[1][0] / FS, 0.0, 12.0)
    after = t >= t_off
    tr = t[after] - t_off                     # fast 0.2 s to 6 dB, then slow 2 s
    gr[after] = np.where(tr < 0.2 * np.log(2), 12 * np.exp(-tr / 0.2), 6 * np.exp(-(tr - 0.2 * np.log(2)) / 2.0))
    y = x * 10 ** (-gr / 20)
    tt, g = D.gain_trace(y, x, FS, F0)
    r = D.release_fit(tt, g, t_off, tt[-1], ref_db=0.0, lo=0.02)
    assert r["rms_err2_db"] < r["rms_err_db"]
    assert abs(r["tau1_s"] / 0.2 - 1) < 0.15 and abs(r["tau2_s"] / 2.0 - 1) < 0.15


def test_gain_trace_ignores_lf_transient_with_hp():
    x, segs = D.piecewise([(-30, 0.2), (-10, 0.3), (-30, 1.0)], F0, FS)
    y = _compressor(x, segs, 0.002, 0.3, 10.0)
    t = np.arange(x.size) / FS
    t_on = segs[1][0] / FS
    thump = np.where(t >= t_on, 0.2 * np.exp(-(t - t_on) / 0.02) * np.sin(2 * np.pi * 20 * (t - t_on)), 0.0)
    _, g_bad = D.gain_trace(y + thump, x, FS, F0, smooth_s=0.0005)
    _, g_hp = D.gain_trace(y + thump, x, FS, F0, smooth_s=0.0005, hp_hz=300)
    _, g_ref = D.gain_trace(y, x, FS, F0, smooth_s=0.0005)
    m = (t > t_on + 0.005) & (t < t_on + 0.06)
    assert np.max(np.abs(g_hp[m] - g_ref[m])) < 0.2 and np.max(np.abs(g_bad[m] - g_ref[m])) > 1.0
