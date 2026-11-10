"""Validate the sweep analysis on systems with known responses."""
import sys
from pathlib import Path

import numpy as np
import pytest
from scipy import signal

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "python"))
from lkit.sweep import Sweep, analyze, hd, log_grid  # noqa: E402

FS = 48000.0


def render(sw: Sweep, gain: float, system, tail_s: float = 2.0) -> np.ndarray:
    x = np.concatenate([sw.x * gain, np.zeros(int(tail_s * FS))])
    return system(x)


@pytest.fixture(scope="module")
def sw():
    return Sweep(fs=FS)


def test_sync(sw):
    assert abs(sw.f1 * sw.L - round(sw.f1 * sw.L)) < 1e-9
    assert 2.5 < sw.T < 3.5


def test_identity_and_delay(sw):
    d = 37
    an = analyze(sw, render(sw, 0.01, lambda x: np.concatenate([np.zeros(d), x[:-d]])), 0.01)
    f = an.freqs
    band = (f >= 20) & (f <= 20000)
    H = an.H[1] * np.exp(2j * np.pi * f * d / FS)
    assert np.max(np.abs(20 * np.log10(np.abs(H[band])))) < 0.01
    assert np.max(np.abs(np.degrees(np.angle(H[band])))) < 0.1
    assert abs(an.latency_peak - d) < 0.01


def test_iir_filter(sw):
    # Low shelf + peak, including 20 Hz content with a long IR.
    b1, a1 = signal.iirpeak(30.0, 2.0, FS)
    b1 = 0.5 * b1 + 0.5 * a1  # +0..-6 dB-ish resonant shape around 30 Hz
    b2, a2 = signal.iirpeak(12000.0, 3.0, FS)
    b2 = a2 + 2.0 * b2
    sys_ = lambda x: signal.lfilter(b2, a2, signal.lfilter(b1, a1, x))  # noqa: E731
    an = analyze(sw, render(sw, 0.01, sys_), 0.01)
    f = an.freqs
    _, Href1 = signal.freqz(b1, a1, worN=f, fs=FS)
    _, Href2 = signal.freqz(b2, a2, worN=f, fs=FS)
    Href = Href1 * Href2
    band = (f >= 20) & (f <= 20000)
    err_db = 20 * np.log10(np.abs(an.H[1][band]) / np.abs(Href[band]))
    err_deg = np.degrees(np.angle(an.H[1][band] / Href[band]))
    assert np.max(np.abs(err_db)) < 0.01
    assert np.max(np.abs(err_deg)) < 0.1


def oversampled(fn, factor: int = 8):
    """Apply a memoryless nonlinearity without aliasing (as a careful plugin would)."""
    def run(x):
        up = signal.resample_poly(x, factor, 1)
        return signal.resample_poly(fn(up), 1, factor)[: x.size]
    return run


def test_polynomial_harmonics(sw):
    A, a2, a3 = 0.5, 0.02, 0.04
    an = analyze(sw, render(sw, A, oversampled(lambda x: x + a2 * x ** 2 + a3 * x ** 3)), A)
    f = an.freqs
    # H2 = a2*A/2, H3 = a3*A^2/4, H1 = 1 + 3*a3*A^2/4 (memoryless, flat).
    for k, expect in ((2, a2 * A / 2), (3, a3 * A ** 2 / 4)):
        m = (f >= 20) & (k * f <= 15000)
        err = np.abs(20 * np.log10(np.abs(an.H[k][m]) / expect))
        # Lowest fundamentals (20-30 Hz) are limited by the short harmonic windows.
        assert np.max(err) < 0.5, k
        assert np.max(err[f[m] >= 40]) < 0.2, k
    m = (f >= 20) & (f <= 10000)
    h1 = 1 + 3 * a3 * A ** 2 / 4
    # 0.03 dB: the test's own resampler ripples by 0.023 dB (identity checked separately).
    assert np.max(np.abs(20 * np.log10(np.abs(an.H[1][m]) / h1))) < 0.03
    # Ratio form: HD2 at fundamental f0.
    m2 = (f >= 40) & (2 * f <= 15000)
    assert np.allclose(hd(an, 2)[m2], (a2 * A / 2) / h1, rtol=0.02)
    # 4th/5th absent.
    m4 = (f >= 20) & (5 * f <= 15000)
    assert np.max(np.abs(an.H[4][m4])) < 1e-2 * a2 and np.max(np.abs(an.H[5][m4])) < 1e-2 * a2


def test_hammerstein_phase(sw):
    # Squarer followed by a delay: H2 must carry the delay phase at the output frequency.
    A, d = 0.5, 11
    sys_ = lambda x: np.concatenate([np.zeros(d), (x + 0.1 * x ** 2)[:-d]])  # noqa: E731
    an = analyze(sw, render(sw, A, sys_), A)
    f = an.freqs
    m = (f >= 50) & (2 * f <= 10000)
    H2 = an.H[2][m] * np.exp(2j * np.pi * 2 * f[m] * d / FS)
    # x^2 = A^2/2 (1 - cos 2phi) -> -cos = sin(2phi - pi/2) : phase -90 deg vs sin(2phi).
    # Static even harmonic: -90 deg in the sine convention (x^2 -> -cos 2phi).
    ph = np.degrees(np.angle(H2))
    assert np.max(np.abs(ph + 90)) < 1.0


def test_grid():
    g = log_grid()
    assert g[0] == 10 and g[-1] <= 22000 and abs(np.log2(g[1] / g[0]) - 1 / 48) < 1e-12
