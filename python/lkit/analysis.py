"""Analysis helpers for rig reports (moved from Pre 73 tools/measure/analysis.py, Vari-mu P1): frequency of a peak,
shelf midpoint, -3 dB corner, coherent harmonics, THD+N, harmonic tracks, worst alias."""
from __future__ import annotations

import numpy as np


def _interp_log(f, y, i, target):
    """Log-frequency interpolation of the crossing of `target` between samples i and i+1."""
    lf = np.log(f)
    t = (target - y[i]) / (y[i + 1] - y[i])
    return float(np.exp(lf[i] + t * (lf[i + 1] - lf[i])))


def peak_freq(f, db, lo=20.0, hi=20000.0):
    """Frequency of the maximum of db (parabolic refinement on log f)."""
    m = (f >= lo) & (f <= hi)
    ff, yy = f[m], db[m]
    i = int(np.nanargmax(yy))
    if 0 < i < ff.size - 1:
        a, b, c = yy[i - 1], yy[i], yy[i + 1]
        den = a - 2 * b + c
        d = 0.5 * (a - c) / den if den != 0 else 0.0
        lf = np.log(ff)
        return float(np.exp(lf[i] + d * (lf[i + 1] - lf[i - 1]) / 2)), float(b - 0.25 * (a - c) * d)
    return float(ff[i]), float(yy[i])


def shelf_mid_freq(f, db, low_shelf=True, lo=10.0, hi=20000.0):
    """Frequency where a shelf's boost falls to half of its maximum (searching away from the plateau)."""
    m = (f >= lo) & (f <= hi)
    ff, yy = f[m], db[m]
    half = 0.5 * np.nanmax(yy)
    idx = range(int(np.nanargmax(yy)), ff.size - 1) if low_shelf else range(int(np.nanargmax(yy)) - 1, -1, -1)
    for i in idx:
        if (yy[i] - half) * (yy[i + 1] - half) <= 0:
            return _interp_log(ff, yy, i, half)
    return float("nan")


def corner_freq(f, db, level=-3.0, lo=10.0, hi=2000.0):
    """Highest frequency below which db < level (a high-pass corner); nan if never below."""
    m = (f >= lo) & (f <= hi)
    ff, yy = f[m], db[m]
    below = np.where(yy < level)[0]
    if below.size == 0:
        return float("nan")
    i = int(below[-1])
    return _interp_log(ff, yy, i, level) if i < ff.size - 1 else float(ff[i])


def harmonics(x, fs, f0, kmax=7):
    """Coherent amplitudes (peak) and phases of DC and harmonics 1..kmax of x (rectangular window: x must
    hold an integer number of cycles of f0). Harmonics at or above Nyquist are NaN."""
    x = np.asarray(x, float)
    n = np.arange(x.size)
    amp = np.full(kmax + 1, np.nan)
    ph = np.full(kmax + 1, np.nan)
    amp[0] = x.mean()
    for k in range(1, kmax + 1):
        if k * f0 >= fs / 2:
            continue
        c = 2.0 / x.size * np.sum(x * np.exp(-2j * np.pi * k * f0 * n / fs))
        amp[k], ph[k] = abs(c), np.angle(c)
    return amp, ph


def thd_n(x, fs, f0, lo=20.0, hi=20000.0):
    """THD+N ratio (linear): RMS of everything in [lo, hi] except the fundamental, over the fundamental RMS."""
    x = np.asarray(x, float) - np.mean(x)
    X = np.fft.rfft(x)
    f = np.fft.rfftfreq(x.size, 1 / fs)
    band = (f >= lo) & (f <= min(hi, fs / 2))
    fund = np.argmin(np.abs(f - f0))
    p = np.abs(X) ** 2
    pf = p[fund]
    pr = p[band].sum() - (pf if band[fund] else 0.0)
    return float(np.sqrt(max(pr, 0.0) / pf))


def harmonic_track(y, fs, f0, win_s, hop_s, kmax=3, t0=0.0, t1=None):
    """Harmonic amplitudes over time with a 4-term Blackman-Harris window (sidelobes -92 dB), so slow linear
    transients do not leak into the harmonic bins. Returns times (window centres) and amplitudes [n, kmax+1]."""
    from scipy.signal.windows import blackmanharris
    y = np.asarray(y, float)
    n = int(round(win_s * fs))
    w = blackmanharris(n)
    cg = w.sum()
    t1 = y.size / fs - win_s if t1 is None else t1
    starts = np.arange(int(t0 * fs), int(t1 * fs), int(round(hop_s * fs)))
    m = np.arange(n)
    E = np.exp(-2j * np.pi * np.outer(np.arange(1, kmax + 1) * f0, m) / fs)
    A = np.array([np.abs(E @ (w * y[s:s + n])) * 2 / cg for s in starts])
    return (starts + n / 2) / fs, A


def alias_report(x, fs, f0, kmax=40):
    """Worst aliased harmonic (dBc) of a steady sine render x: harmonics k*f0 >= fs/2 folded back into the band.
    Returns (worst_dbc, freq, h_inband_dbc dict). Blackman-Harris window, x should be >= 16384 steady samples."""
    from scipy.signal.windows import blackmanharris
    x = np.asarray(x, float)
    w = blackmanharris(x.size)
    X = 20 * np.log10(np.abs(np.fft.rfft(x * w)) * 2 / w.sum() + 1e-20)
    f = np.fft.rfftfreq(x.size, 1 / fs)
    fund = X[np.argmin(np.abs(f - f0))]
    pk = lambda fr: float(X[np.abs(f - fr) < 3 * fs / x.size + 1].max() - fund)  # noqa: E731
    inband = {k: pk(k * f0) for k in range(2, kmax) if k * f0 < fs / 2 - 50}
    al = []
    for k in range(2, kmax):
        if k * f0 < fs / 2:
            continue
        fa = abs(((k * f0 + fs / 2) % fs) - fs / 2)
        if fa > 20 and all(abs(fa - j * f0) > 20 for j in range(1, kmax) if j * f0 < fs / 2):
            al.append((pk(fa), fa, k))
    worst = max(al) if al else (float("nan"), float("nan"), 0)
    return worst, inband
