"""lkit.analysis on curves with known answers; lkit.params_header output (moved from Pre 73, Vari-mu P1)."""
import sys
from pathlib import Path

import numpy as np
import pytest

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "python"))
from lkit.analysis import alias_report, corner_freq, harmonics, peak_freq, shelf_mid_freq, thd_n  # noqa: E402
from lkit.params_header import cpp_name, params_header, write_params_header  # noqa: E402

F = 10 * 2 ** (np.arange(0, 48 * 11) / 48)


def test_peak():
    s = 1j * F / 1600
    q = 1.2
    H = (1 + 8 * s / q + s * s) / (1 + s / q + s * s)   # bell at 1.6 kHz
    f, g = peak_freq(F, 20 * np.log10(np.abs(H)))
    assert abs(f / 1600 - 1) < 0.005 and abs(g - 20 * np.log10(8)) < 0.05


def test_shelf_and_corner():
    s = 1j * F / 100
    H = (s + 8) / (s + 1)
    db = 20 * np.log10(np.abs(H))
    f = shelf_mid_freq(F, db, low_shelf=True)
    assert abs(20 * np.log10(abs((1j * f / 100 + 8) / (1j * f / 100 + 1))) - db.max() / 2) < 0.05
    w = 1j * F / 80
    hp = 20 * np.log10(np.abs(w ** 3 / (w ** 3 + 2 * w ** 2 + 2 * w + 1)))
    assert abs(corner_freq(F, hp) / 80 - 1) < 0.01          # 3rd-order Butterworth: -3 dB at f0


def test_harmonics_and_thdn():
    fs, f0 = 48000, 1000.0
    n = np.arange(9600)
    x = 0.5 * np.sin(2 * np.pi * f0 * n / fs) + 0.005 * np.sin(2 * np.pi * 3 * f0 * n / fs + 0.3) + 0.01
    a, _ = harmonics(x, fs, f0, kmax=7)
    assert abs(a[1] - 0.5) < 1e-9 and abs(a[3] - 0.005) < 1e-9 and abs(a[0] - 0.01) < 1e-12 and a[2] < 1e-9
    assert abs(thd_n(x, fs, f0) - 0.01) < 1e-6
    assert np.isnan(harmonics(x, fs, 5000.0, kmax=7)[0][5])   # 25 kHz is above Nyquist


def test_alias_report():
    fs, f0 = 48000, 10000.0
    n = np.arange(32768)
    fa = 3 * f0 - fs   # 3rd harmonic folds to |30 k - 48 k| = 18 kHz
    x = np.sin(2 * np.pi * f0 * n / fs) + 1e-3 * np.sin(2 * np.pi * abs(fa) * n / fs)
    (worst, freq, k), _ = alias_report(x, fs, f0)
    assert abs(freq - 18000) < 1 and abs(worst + 60) < 0.5   # k is ambiguous: 3rd and 27th fold to 18 kHz


def test_params_header(tmp_path):
    vals = {"r1": 1000, "c.tc": 4.7e-9, "linked": True, "2nd": 0.1}
    h = params_header(vals, "demo::params", source="test")
    assert "namespace demo::params {" in h and "inline constexpr double r1 = 1000.0;" in h
    assert "inline constexpr double cptc = 4.7e-09;" in h and "inline constexpr bool linked = true;" in h
    assert "_2nd" in h and cpp_name("a-b") == "a_b"
    out = write_params_header(vals, tmp_path / "p" / "P.h", "demo::params", source="test")
    t0 = out.stat().st_mtime_ns
    write_params_header(vals, out, "demo::params", source="test")
    assert out.stat().st_mtime_ns == t0          # unchanged text: not rewritten
    with pytest.raises(ValueError):
        params_header({"a.b": 1, "apb": 2}, "x")
