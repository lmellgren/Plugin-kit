"""Dynamics measurement (Vari-mu P2): stimuli and analysis for compressors / limiters, independent of the plugin.

Stimuli are sines whose level changes only at whole cycles (positive-going zero crossings), so a level step adds no
click of its own. Levels are RMS dBFS (a sine of level L has peak 10**(L/20) * sqrt(2)).

  staircase(levels_db, step_s, f0, fs)   -> x, segments          static curve in one render (ascending: attack only)
  piecewise([(level_db, dur_s), ...], f0, fs) -> x, segments      bursts, program-dependent release patterns
  segment_stats(y, x, fs, f0, segments, measure_s)               per segment: in / out level, gain, harmonics, THD+N
  gain_trace(y, x, fs, f0, smooth_s)                             instantaneous gain in dB (analytic envelopes)
  attack_time(...), release_fit(...)                             time constants from a gain trace
  gr_readback(out_db, in_db, linear_gain_db)                     GR from the output when the plugin has no GR meter

A segment is (start_sample, end_sample, level_db). Pure numpy/scipy, no plugin knowledge.
"""
from __future__ import annotations

import numpy as np

from .analysis import harmonics, thd_n


def _cycles(dur_s: float, f0: float) -> int:
    return max(1, int(round(dur_s * f0)))


def piecewise(parts, f0: float, fs: float, phase0: float = 0.0):
    """Sine at f0 whose RMS level (dBFS; None = silence) is held for each (level_db, dur_s) in parts. Durations are
    rounded to whole cycles and every change happens at the start of a cycle. Returns (x float64, segments)."""
    n_tot = 0
    bounds = []
    for level, dur in parts:
        n = int(round(_cycles(dur, f0) * fs / f0))
        bounds.append((n_tot, n_tot + n, level))
        n_tot += n
    t = np.arange(n_tot) / fs
    s = np.sin(2 * np.pi * f0 * t + phase0)
    amp = np.zeros(n_tot)
    for a, b, level in bounds:
        if level is not None:
            amp[a:b] = 10 ** (level / 20) * np.sqrt(2)
    return amp * s, bounds


def staircase(levels_db, step_s: float, f0: float, fs: float):
    """Ascending (or any) level staircase, each level held step_s: the static curve in one render."""
    return piecewise([(lv, step_s) for lv in levels_db], f0, fs)


def _tone_level_db(seg: np.ndarray, fs: float, f0: float) -> float:
    a, _ = harmonics(seg, fs, f0, kmax=1)
    return float(20 * np.log10(max(a[1], 1e-15) / np.sqrt(2)))


def segment_stats(y, x, fs: float, f0: float, segments, measure_s: float = 0.1, kmax: int = 7, offset: int = 0):
    """Steady-state numbers over the last measure_s (whole cycles) of every segment. y may be [n] or [n, ch]
    (channel 0 is used); offset = output delay in samples (latency). Returns a list of dicts: level_in_db, out_db
    (fundamental), gain_db, h_dbc {k: dBc} for k = 2..kmax, thd_n_db, dc."""
    y = np.asarray(y, float)
    y = y[:, 0] if y.ndim == 2 else y
    n_meas = int(round(_cycles(measure_s, f0) * fs / f0))
    res = []
    for a, b, level in segments:
        if level is None:
            continue
        a0 = max(a, b - n_meas)
        xi = x[a0:b]
        yo = y[a0 + offset:b + offset]
        amp, _ = harmonics(yo, fs, f0, kmax=kmax)
        fund = max(amp[1], 1e-15)
        lin = _tone_level_db(xi, fs, f0)
        out = float(20 * np.log10(fund / np.sqrt(2)))
        h = {k: float(20 * np.log10(max(amp[k], 1e-15) / fund)) for k in range(2, kmax + 1) if np.isfinite(amp[k])}
        res.append({"level_in_db": lin, "out_db": out, "gain_db": out - lin, "h_dbc": h,
                    "thd_n_db": float(20 * np.log10(max(thd_n(yo, fs, f0), 1e-15))), "dc": float(amp[0])})
    return res


def _smooth(v: np.ndarray, n: int) -> np.ndarray:
    if n <= 1:
        return v
    k = np.ones(n) / n
    return np.convolve(np.pad(v, (n // 2, n - 1 - n // 2), mode="edge"), k, mode="valid")


def gain_trace(y, x, fs: float, f0: float, smooth_s: float | None = None, offset: int = 0, floor_db: float = -120.0):
    """Instantaneous gain in dB from the analytic envelopes of output and input (Hilbert), smoothed over smooth_s
    (default: one period of f0, which removes the ripple from harmonics). Returns (t, gain_db); samples where the input
    is below floor_db are NaN."""
    from scipy.signal import hilbert
    y = np.asarray(y, float)
    y = y[:, 0] if y.ndim == 2 else y
    y = y[offset:offset + len(x)]
    if y.size < len(x):
        y = np.pad(y, (0, len(x) - y.size))
    ex = np.abs(hilbert(np.asarray(x, float)))
    ey = np.abs(hilbert(y))
    n = int(round(fs / f0)) if smooth_s is None else max(1, int(round(smooth_s * fs)))
    ex, ey = _smooth(ex, n), _smooth(ey, n)
    g = 20 * np.log10(np.maximum(ey, 1e-20) / np.maximum(ex, 1e-20))
    g[20 * np.log10(np.maximum(ex, 1e-20) / np.sqrt(2)) < floor_db] = np.nan
    return np.arange(len(x)) / fs, g


def gr_readback(out_db, in_db, linear_gain_db):
    """Gain reduction (dB, positive) from output and input levels and the gain with no gain reduction."""
    return np.asarray(linear_gain_db) - (np.asarray(out_db) - np.asarray(in_db))


def _level(t, g, t0, t1):
    m = (t >= t0) & (t < t1) & np.isfinite(g)
    return float(np.median(g[m])) if m.any() else float("nan")


def attack_time(t, g_db, t_on: float, t_end: float, frac: float = 0.63, pre_s: float = 0.02, tail_frac: float = 0.2):
    """Attack after a level step up at t_on (gain falls). Before = median gain over pre_s before t_on, final = median
    over the last tail_frac of [t_on, t_end]. Returns dict: gr_db (change), t_frac (s after t_on to reach frac of the
    change), overshoot_db (lowest gain below final), t_90."""
    before = _level(t, g_db, t_on - pre_s, t_on)
    final = _level(t, g_db, t_end - tail_frac * (t_end - t_on), t_end)
    d = before - final
    m = (t >= t_on) & (t < t_end)
    tt, gg = t[m] - t_on, g_db[m]

    def cross(f):
        idx = np.where(gg <= before - f * d)[0]
        return float(tt[idx[0]]) if idx.size and d > 0 else float("nan")
    return {"gr_db": float(d), "t_frac": cross(frac), "t_90": cross(0.9),
            "overshoot_db": float(final - np.nanmin(gg)) if gg.size else float("nan"), "before_db": before, "final_db": final}


def release_fit(t, g_db, t_off: float, t_end: float, ref_db: float | None = None, lo: float = 0.1, hi: float = 0.9,
                pre_s: float = 0.05):
    """Recovery after a level step down at t_off. GR(t) = ref - g(t), with ref the fully recovered gain (default: the
    median of the last 5 % of the window). Fits ln GR = a - t / tau between hi and lo of the initial GR (single slope),
    and a two-slope fit (best breakpoint) for program-dependent release. Returns dict: gr0_db, tau_s, t_63 (time to
    recover 63 % of GR0), t_10db (time from GR0 >= 10 dB to GR 10/e dB, the manual's «from 10 dB»), tau1_s, tau2_s,
    t_break_s, rms_err_db single / two."""
    m = (t >= t_off) & (t < t_end) & np.isfinite(g_db)
    tt, gg = t[m] - t_off, g_db[m]
    if ref_db is None:
        ref_db = float(np.median(gg[int(0.95 * gg.size):]))
    gr = ref_db - gg
    gr0 = float(np.median((ref_db - g_db)[(t >= t_off - pre_s) & (t < t_off) & np.isfinite(g_db)]))
    out = {"gr0_db": gr0, "ref_db": ref_db}
    idx = np.where(gr <= gr0 / np.e)[0]
    out["t_63"] = float(tt[idx[0]]) if idx.size else float("nan")
    sel = (gr < hi * gr0) & (gr > lo * gr0) & (gr > 0.05)
    if sel.sum() < 4:
        out.update(tau_s=float("nan"), tau1_s=float("nan"), tau2_s=float("nan"), t_break_s=float("nan"))
        return out
    T, L = tt[sel], np.log(gr[sel])
    p = np.polyfit(T, L, 1)
    out["tau_s"] = float(-1 / p[0]) if p[0] < 0 else float("inf")
    out["rms_err_db"] = float(np.sqrt(np.mean((np.exp(np.polyval(p, T)) - gr[sel]) ** 2)))
    best = None   # two slopes, continuous at the break: search the break over the selected range
    # breaks spaced in log time (the fast part is short); chosen by the error in ln GR, reported in dB
    for tb in np.geomspace(max(T[1], 1e-4), T[-2], 64):
        A = np.column_stack([np.ones_like(T), T, np.maximum(T - tb, 0)])
        c, *_ = np.linalg.lstsq(A, L, rcond=None)
        el = float(np.mean((A @ c - L) ** 2))
        if best is None or el < best[0]:
            best = (el, tb, c, float(np.sqrt(np.mean((np.exp(A @ c) - gr[sel]) ** 2))))
    _, tb, c, e = best
    s1, s2 = c[1], c[1] + c[2]
    out.update(tau1_s=float(-1 / s1) if s1 < 0 else float("inf"), tau2_s=float(-1 / s2) if s2 < 0 else float("inf"),
               t_break_s=float(tb), rms_err2_db=e)
    return out
