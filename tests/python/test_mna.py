"""Generic MNA: AC response = state space (physical states) = ngspice = sympy, on a circuit with
R, C, L, coupled inductors (transformer) and a VCVS (ideal amplifier)."""
import sys
from pathlib import Path

import numpy as np
import pytest

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "python"))
from lkit import spice  # noqa: E402
from lkit.mna import Netlist  # noqa: E402


def circuit():
    n = Netlist(out="out")
    n.R("Rs", "in", "p", 600)
    n.L("Lp", "p", "0", 2.0)          # transformer primary
    n.L("Ls", "s", "0", 8.0)          # secondary (1:2)
    n.K("K1", "Lp", "Ls", 0.995)
    n.R("Rt", "s", "0", 10e3)
    n.C("Cs", "s", "0", 200e-12)
    n.E("A1", "a", "0", "s", "0", 10.0)   # ideal amplifier, gain 10
    n.R("R1", "a", "b", 1e3)
    n.L("L1", "b", "c", 0.1)
    n.C("C1", "c", "out", 47e-9)
    n.R("Rl", "out", "0", 2.2e3)
    n.C("Cl", "out", "0", 1e-9)
    return n


F = np.geomspace(10, 22000, 120)


def db_deg(h, ref):
    r = h / ref
    return np.max(np.abs(20 * np.log10(np.abs(r)))), np.max(np.abs(np.degrees(np.angle(r))))


def test_state_space_matches_ac():
    n = circuit()
    ss = n.state_space()
    assert ss[0].shape == (6, 6)
    assert n.state_names() == ["v(Cs)", "v(C1)", "v(Cl)", "i(Lp)", "i(Ls)", "i(L1)"]
    db, deg = db_deg(Netlist.ss_response(ss, F), n.response(F))
    assert db < 1e-8 and deg < 1e-6


def test_overrides():
    n = circuit()
    a = n.response(F, {"C1": 100e-9})
    b = n.response(F)
    assert np.max(np.abs(a - b)) > 1e-3
    with pytest.raises(KeyError):
        n.response(F, {"nope": 1})


@pytest.mark.skipif(not spice.available(), reason="ngspice not installed")
def test_matches_ngspice():
    n = circuit()
    f, v = spice.netlist_ac(n, f_lo=10, f_hi=22000, per_dec=40)
    db, deg = db_deg(v, n.response(f))
    assert db < 0.01 and deg < 0.1


def test_symbolic_rc():
    n = Netlist(out="out")
    n.R("R1", "in", "out", 1e3)
    n.C("C1", "out", "0", 1e-6)
    H, s, sym = n.symbolic_H()
    import sympy as sp
    assert sp.simplify(H - 1 / (1 + s * sym["R1"] * sym["C1"])) == 0
