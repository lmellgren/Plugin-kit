"""ngspice AC analysis runner (LOMAS Plugin-kit; from Passive EQ fit/spice.py).

    f, v = ac(deck_text, node="out")                # any deck ending in .end
    f, v = netlist_ac(netlist, values, node="out")  # an lkit.mna.Netlist
"""
from __future__ import annotations

import shutil
import subprocess
import tempfile
from pathlib import Path

import numpy as np


def available() -> bool:
    return shutil.which("ngspice") is not None


def ac(deck: str, node: str = "out", f_lo: float = 10, f_hi: float = 22000, per_dec: int = 100):
    """Run an AC sweep; return (freqs, complex V(node))."""
    with tempfile.TemporaryDirectory() as td:
        out = Path(td) / "ac.txt"
        ctrl = (f".control\nset wr_singlescale\nset wr_vecnames\noption numdgt=12\n"
                f"ac dec {per_dec} {f_lo} {f_hi}\nwrdata {out} real(v({node})) imag(v({node}))\n.endc\n")
        if "\n.end" not in deck:
            raise ValueError("deck must end with .end")
        text = deck.replace("\n.end", "\n" + ctrl + ".end")
        (Path(td) / "deck.cir").write_text(text)
        r = subprocess.run(["ngspice", "-b", str(Path(td) / "deck.cir")], capture_output=True, text=True)
        if r.returncode != 0 or not out.exists():
            raise RuntimeError(r.stdout[-2000:] + r.stderr[-2000:])
        d = np.loadtxt(out, skiprows=1)
    return d[:, 0], d[:, 1] + 1j * d[:, 2]


def netlist_ac(netlist, values: dict | None = None, node: str | None = None, **kw):
    return ac(netlist.spice_deck(values), node or netlist.out, **kw)
