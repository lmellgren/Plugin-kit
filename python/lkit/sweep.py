"""Synchronized exponential sine sweep (LOMAS Plugin-kit; from Passive EQ) (Farina 2000, Novak et al. 2015) and analysis.

The sweep x(t) = sin(2 pi f1 L (exp(t/L) - 1)) with f1*L an integer has the property
sin(k phi(t)) = x(t + L ln k), so after deconvolution the k-th harmonic impulse response
sits at t = -L ln k with the correct phase.

Conventions
-----------
* Level: the sweep's peak amplitude in dBFS (a sine of peak 1.0 is 0 dBFS).
* H1(f): complex linear response (output/input), latency NOT removed.
* Hk(f0) for k >= 2: complex k-th harmonic response at output frequency k*f0, relative to
  the input amplitude A. The k-th harmonic distortion ratio at fundamental f0 is
  |Hk(f0)| / |H1(f0)|. NaN where k*f0 lies outside the sweep band.
"""
from __future__ import annotations

from dataclasses import dataclass, field

import numpy as np


def log_grid(f_lo: float = 10.0, f_hi: float = 22000.0, per_octave: int = 48) -> np.ndarray:
    n = int(np.floor(per_octave * np.log2(f_hi / f_lo) + 1e-9))
    return f_lo * 2.0 ** (np.arange(n + 1) / per_octave)


@dataclass
class Sweep:
    fs: float = 48000.0
    f1: float = 10.0
    f2: float = 22000.0
    T: float = 3.0
    fade_out_s: float = 0.002
    L: float = field(init=False)
    x: np.ndarray = field(init=False, repr=False)

    def __post_init__(self) -> None:
        # Make f1*L an integer (synchronization), then recompute the duration.
        self.L = np.round(self.f1 * self.T / np.log(self.f2 / self.f1)) / self.f1
        self.T = self.L * np.log(self.f2 / self.f1)
        n = int(np.floor(self.T * self.fs))
        t = np.arange(n) / self.fs
        x = np.sin(2 * np.pi * self.f1 * self.L * (np.exp(t / self.L) - 1.0))
        nf = max(1, int(self.fade_out_s * self.fs))
        x[-nf:] *= 0.5 * (1 + np.cos(np.pi * np.arange(nf) / nf))
        self.x = x

    def harmonic_delay(self, k: int) -> float:
        """Time advance (s) of the k-th harmonic IR relative to the linear IR."""
        return self.L * np.log(k)

    def meta(self) -> dict:
        return {"fs": self.fs, "f1": self.f1, "f2": self.f2, "T": self.T, "L": self.L,
                "fade_out_s": self.fade_out_s, "frames": int(self.x.size)}


@dataclass
class Analysis:
    freqs: np.ndarray
    H: dict            # k -> complex array on freqs (k=1 linear)
    ir: np.ndarray     # linear IR from t=0 (float32, truncated)
    latency_peak: float  # IR peak position in samples (parabolic interpolation)
    peak_dbfs: float     # output peak


# Causal band limit below the sweep (Butterworth HP): (order, corner as a multiple of f1).
# Linear IR: long window, so a low corner. Harmonic k: short window, so the corner sits at
# 0.5*k*f1 (output band starts at k*f1) and a 2nd order whose ringing dies inside the window.
HP_LINEAR = (4, 0.4)


def hp_harmonic(k: int) -> tuple:
    return (2, 0.5 * k)


def _band_limit(sweep: Sweep, f: np.ndarray, hp: tuple) -> np.ndarray:
    """Band-limit applied during deconvolution.

    Low edge: a causal (minimum-phase) Butterworth high-pass, so its long low-frequency
    ringing lands after t=0 inside the linear window; its known response is divided out
    on the analysis grid (see `band_limit_response`). High edge: a short zero-phase
    raised-cosine taper from f2 towards Nyquist.
    """
    return band_limit_response(sweep, f, hp) * _hf_taper(sweep, f)


def _hf_taper(sweep: Sweep, f: np.ndarray) -> np.ndarray:
    # Close the band at f2: above it the sweep has no energy, so anything there
    # (e.g. harmonics between f2 and Nyquist) would be amplified into junk.
    hi0, hi1 = 0.98 * sweep.f2, sweep.f2
    w = np.ones_like(f)
    m = (f > hi0) & (f < hi1)
    w[m] = 0.5 * (1 + np.cos(np.pi * (f[m] - hi0) / (hi1 - hi0)))
    w[f >= hi1] = 0.0
    return w


def _hf_div(sweep: Sweep, f: np.ndarray) -> np.ndarray:
    """HF taper to divide out; NaN where it is too small to invert."""
    w = _hf_taper(sweep, np.asarray(f, float))
    return np.where(w >= 0.1, w, np.nan)


def band_limit_response(sweep: Sweep, f: np.ndarray, hp: tuple) -> np.ndarray:
    from scipy import signal
    b, a = signal.butter(hp[0], hp[1] * sweep.f1, "highpass", fs=sweep.fs)
    _, B = signal.freqz(b, a, worN=np.asarray(f, float), fs=sweep.fs)
    return B


def deconvolve(sweep: Sweep, y: np.ndarray, gain: float, hp: tuple = HP_LINEAR) -> np.ndarray:
    """Return the circular impulse response h (length >= len(y)); t<0 wraps to the end."""
    n = max(y.size, sweep.x.size)
    N = int(2 ** np.ceil(np.log2(n)))
    X = np.fft.rfft(sweep.x * gain, N)
    Y = np.fft.rfft(y, N)
    f = np.fft.rfftfreq(N, 1 / sweep.fs)
    p = np.abs(X) ** 2
    ref = np.median(p[(f >= sweep.f1) & (f <= sweep.f2)])
    H = Y * np.conj(X) / (p + 1e-6 * ref) * _band_limit(sweep, f, hp)
    H[0] = 0.0
    return np.fft.irfft(H, N)


def _half_hann(n: int) -> np.ndarray:
    return 0.5 * (1 - np.cos(np.pi * np.arange(n) / max(n, 1)))


def _window(n_pre: int, n_post: int, fade_pre: int, fade_post: int) -> np.ndarray:
    w = np.ones(n_pre + n_post)
    w[:fade_pre] = _half_hann(fade_pre)
    w[n_pre + n_post - fade_post:] = _half_hann(fade_post)[::-1]
    return w


def _dft(seg: np.ndarray, t: np.ndarray, freqs: np.ndarray, chunk: int = 32) -> np.ndarray:
    """Exact DFT of samples seg at times t (s), evaluated at arbitrary freqs."""
    out = np.empty(freqs.size, complex)
    for i in range(0, freqs.size, chunk):
        f = freqs[i:i + chunk, None]
        out[i:i + chunk] = np.exp(-2j * np.pi * f * t[None, :]) @ seg
    return out


def analyze(sweep: Sweep, y: np.ndarray, gain: float, freqs: np.ndarray | None = None,
            harmonics=(2, 3, 4, 5), lin_post_s: float = 2.0, lin_pre_s: float = 0.01,
            ir_keep: int = 32768) -> Analysis:
    """Analyze one recorded channel y (starting at stimulus onset)."""
    fs = sweep.fs
    freqs = log_grid(sweep.f1, sweep.f2) if freqs is None else freqs
    y = np.asarray(y, float)
    h = deconvolve(sweep, y, gain, HP_LINEAR)
    N = h.size

    def segment(t0: float, t1: float, fade_pre: float, fade_post: float, h=h):
        i0, i1 = int(np.floor(t0 * fs)), int(np.ceil(t1 * fs))
        idx = np.arange(i0, i1)
        seg = h[idx % N]
        w =_window(0, idx.size, int(fade_pre * fs), int(fade_post * fs))
        return seg * w, idx / fs

    H = {}
    # Linear IR: [-pre, post], post limited so it stays clear of the harmonic IRs at the end.
    kmax_guard = max(harmonics) + 5
    post = min(lin_post_s, (N / fs) - sweep.harmonic_delay(kmax_guard) - 0.05)
    seg, t = segment(-lin_pre_s, post, lin_pre_s, 0.25 * post)
    H[1] = _dft(seg, t, freqs) / band_limit_response(sweep, freqs, HP_LINEAR) / _hf_div(sweep, freqs)
    lin_window_full = np.zeros(N)
    i0 = int(np.floor(-lin_pre_s * fs))
    lin_window_full[np.arange(i0, i0 + t.size) % N] = _window(0, t.size, int(lin_pre_s * fs),
                                                              int(0.25 * post * fs))

    for k in harmonics:
        d_k = sweep.harmonic_delay(k)
        d_prev = sweep.harmonic_delay(k - 1)
        d_next = sweep.harmonic_delay(k + 1)
        pre = min(0.005, 0.5 * (d_next - d_k))
        post_k = (d_k - d_prev) - 2 * pre if k > 2 else (d_k - d_prev) - lin_pre_s - pre
        hk = deconvolve(sweep, y, gain, hp_harmonic(k))
        if k % 2 == 0:
            # Even harmonics of a static nonlinearity are cosines (-90 deg in the sine
            # convention): their IR is a Hilbert kernel with a 1/t tail that a short window
            # cannot hold. Work in the quadrature domain instead (= deconvolving with the
            # cosine sweep), where it is local, and rotate back by +90 deg afterwards.
            # The windowed linear IR is removed first so its Hilbert tail does not leak in.
            hk = hk - lin_window_full * hk
            hk = np.fft.irfft(-1j * np.fft.rfft(hk), hk.size)
        seg, t = segment(-d_k - pre, -d_k + post_k, pre, 0.25 * post_k, h=hk)
        # response at output frequency k*f0
        Hk = (_dft(seg, t + d_k, k * freqs) / band_limit_response(sweep, k * freqs, hp_harmonic(k))
              / _hf_div(sweep, k * freqs))
        if k % 2 == 0:
            Hk = 1j * Hk
        Hk[(k * freqs > sweep.f2) | (k * freqs > fs / 2)] = np.nan
        H[k] = Hk

    ir = h[:ir_keep].astype(np.float32)
    i = int(np.argmax(np.abs(h[: int(0.5 * fs)])))
    if 0 < i < N - 1:
        a, b, c = np.abs(h[i - 1]), np.abs(h[i]), np.abs(h[i + 1])
        den = a - 2 * b + c
        frac = 0.5 * (a - c) / den if den != 0 else 0.0
    else:
        frac = 0.0
    peak = float(np.max(np.abs(y))) if y.size else 0.0
    return Analysis(freqs=freqs, H=H, ir=ir, latency_peak=i + frac,
                    peak_dbfs=20 * np.log10(max(peak, 1e-30)))


def hd(an: Analysis, k: int) -> np.ndarray:
    """k-th harmonic distortion ratio (linear) at fundamental frequencies an.freqs."""
    return np.abs(an.H[k]) / np.abs(an.H[1])
