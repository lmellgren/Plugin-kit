"""Generic modified nodal analysis for linear circuit models (LOMAS Plugin-kit).

Generalised from the Passive EQ network model: a plugin describes its circuit as a Netlist (element
values may be overridden per setting), and gets from the same description
  * response(freqs, values)      complex V(out)/V(in), numpy
  * state_space(values)          continuous (A, B, C, D) with PHYSICAL states (capacitor voltages,
                                 inductor currents) - what the realtime FOH engine (lkit/Foh.h) needs
  * symbolic_H()                 sympy N(s)/D(s) in the element symbols
  * spice_deck(values)           an ngspice deck of the same circuit (cross-check, lkit.spice)

Elements: R, C, L (a b value), K (mutual coupling between two inductors: k), E (VCVS:
out+ out- ctrl+ ctrl- gain), and one ideal input source "in" (node "in" driven by u). Ground is "0".

    n = Netlist(out="out")
    n.R("Rs", "in", "a", 600); n.C("C1", "a", "out", 100e-9); n.R("Rl", "out", "0", 10e3)
    H = n.response(np.geomspace(20, 20e3, 200))
"""
from __future__ import annotations

from dataclasses import dataclass

import numpy as np

GND = "0"


@dataclass
class Element:
    name: str
    kind: str          # R C L K E
    nodes: tuple
    value: float
    refs: tuple = ()   # K: the two inductor names


class Netlist:
    def __init__(self, out: str = "out", inp: str = "in"):
        self.out, self.inp = out, inp
        self.elements: list[Element] = []

    # ---- building --------------------------------------------------------------------------
    def _add(self, e: Element) -> Element:
        if any(x.name == e.name for x in self.elements):
            raise ValueError(f"duplicate element {e.name}")
        self.elements.append(e)
        return e

    def R(self, name, a, b, value): return self._add(Element(name, "R", (a, b), float(value)))
    def C(self, name, a, b, value): return self._add(Element(name, "C", (a, b), float(value)))
    def L(self, name, a, b, value): return self._add(Element(name, "L", (a, b), float(value)))
    def K(self, name, l1, l2, k): return self._add(Element(name, "K", (), float(k), (l1, l2)))
    def E(self, name, op, om, cp, cm, gain): return self._add(Element(name, "E", (op, om, cp, cm), float(gain)))

    def nodes(self) -> list:
        ns = []
        for e in self.elements:
            for n in e.nodes:
                if n != GND and n not in ns:
                    ns.append(n)
        if self.inp not in ns:
            ns.insert(0, self.inp)
        return ns

    def values(self, overrides: dict | None = None) -> dict:
        v = {e.name: e.value for e in self.elements}
        for k, x in (overrides or {}).items():
            if k not in v:
                raise KeyError(f"no element {k}")
            v[k] = float(x)
        return v

    def by_kind(self, kind: str) -> list:
        return [e for e in self.elements if e.kind == kind]

    # ---- numeric AC ------------------------------------------------------------------------
    def _index(self):
        nodes = self.nodes()
        ni = {n: i for i, n in enumerate(nodes)}
        branches = ["Vin"] + [e.name for e in self.elements if e.kind in ("L", "E")]
        bi = {b: len(nodes) + i for i, b in enumerate(branches)}
        return nodes, ni, bi, len(nodes) + len(branches)

    def _mutual(self, vals: dict) -> dict:
        Ls = {e.name: e for e in self.by_kind("L")}
        M = {}
        for k in self.by_kind("K"):
            a, b = k.refs
            m = vals[k.name] * np.sqrt(vals[a] * vals[b])
            M[(a, b)] = M[(b, a)] = m
            if a not in Ls or b not in Ls:
                raise KeyError(f"{k.name}: unknown inductor")
        return M

    def response(self, freqs, values: dict | None = None, node: str | None = None) -> np.ndarray:
        """V(node)/u at freqs (Hz), node = the output by default."""
        vals = self.values(values)
        nodes, ni, bi, n = self._index()
        M = self._mutual(vals)
        f = np.atleast_1d(np.asarray(freqs, float))
        s = 2j * np.pi * f
        Y = np.zeros((f.size, n, n), complex)
        rhs = np.zeros((f.size, n), complex)

        def st(a, b, y):
            ia, ib = ni.get(a), ni.get(b)
            if ia is not None: Y[:, ia, ia] += y
            if ib is not None: Y[:, ib, ib] += y
            if ia is not None and ib is not None:
                Y[:, ia, ib] -= y
                Y[:, ib, ia] -= y

        def branch(row, a, b):   # KCL coupling of a branch current and its voltage
            ia, ib = ni.get(a), ni.get(b)
            if ia is not None: Y[:, ia, row] += 1; Y[:, row, ia] += 1
            if ib is not None: Y[:, ib, row] -= 1; Y[:, row, ib] -= 1

        branch(bi["Vin"], self.inp, GND)
        rhs[:, bi["Vin"]] = 1.0
        for e in self.elements:
            if e.kind == "R":
                st(*e.nodes, 1.0 / vals[e.name])
            elif e.kind == "C":
                st(*e.nodes, s * vals[e.name])
            elif e.kind == "L":
                r = bi[e.name]
                ia, ib = ni.get(e.nodes[0]), ni.get(e.nodes[1])
                if ia is not None: Y[:, ia, r] += 1; Y[:, r, ia] += 1
                if ib is not None: Y[:, ib, r] -= 1; Y[:, r, ib] -= 1
                Y[:, r, r] -= s * vals[e.name]
                for (a, b), m in M.items():
                    if a == e.name:
                        Y[:, r, bi[b]] -= s * m
            elif e.kind == "E":
                r = bi[e.name]
                op, om, cp, cm = e.nodes
                for nd, sg in ((op, 1), (om, -1)):
                    if nd in ni: Y[:, ni[nd], r] += sg; Y[:, r, ni[nd]] += sg
                for nd, sg in ((cp, 1), (cm, -1)):
                    if nd in ni: Y[:, r, ni[nd]] -= sg * vals[e.name]
        x = np.linalg.solve(Y, rhs[..., None])[..., 0]
        return x[:, ni[node or self.out]]

    # ---- state space with physical states ---------------------------------------------------
    def state_names(self) -> list:
        return [f"v({e.name})" for e in self.by_kind("C")] + [f"i({e.name})" for e in self.by_kind("L")]

    def state_space(self, values: dict | None = None, node: str | None = None):
        """(A, B, C, D) with x = [capacitor voltages (a - b) ..., inductor currents (a -> b) ...].

        The resistive network is solved with each capacitor replaced by a voltage source (its state)
        and each inductor by a current source (its state), once per state and once for the input:
            C_k dv_k/dt = i_k,   L di/dt = v_L (L the inductance matrix incl. mutual terms).
        """
        vals = self.values(values)
        caps, inds = self.by_kind("C"), self.by_kind("L")
        nodes = self.nodes()
        ni = {nd: i for i, nd in enumerate(nodes)}
        extra = ["Vin"] + [c.name for c in caps] + [e.name for e in self.by_kind("E")]
        bi = {b: len(nodes) + i for i, b in enumerate(extra)}
        n = len(nodes) + len(extra)
        ns = len(caps) + len(inds)
        G = np.zeros((n, n))

        def st(a, b, y):
            ia, ib = ni.get(a), ni.get(b)
            if ia is not None: G[ia, ia] += y
            if ib is not None: G[ib, ib] += y
            if ia is not None and ib is not None:
                G[ia, ib] -= y
                G[ib, ia] -= y

        def vsrc(row, a, b):
            for nd, sg in ((a, 1), (b, -1)):
                if nd in ni:
                    G[ni[nd], row] += sg
                    G[row, ni[nd]] += sg

        vsrc(bi["Vin"], self.inp, GND)
        for c in caps:
            vsrc(bi[c.name], *c.nodes)
        for e in self.elements:
            if e.kind == "R":
                st(*e.nodes, 1.0 / vals[e.name])
            elif e.kind == "E":
                r = bi[e.name]
                op, om, cp, cm = e.nodes
                vsrc(r, op, om)
                for nd, sg in ((cp, 1), (cm, -1)):
                    if nd in ni: G[r, ni[nd]] -= sg * vals[e.name]
        R = np.zeros((n, ns + 1))
        for k, c in enumerate(caps):
            R[bi[c.name], k] = 1.0                       # v_c = state k
        for j, l in enumerate(inds):                      # current source a -> b
            a, b = l.nodes
            if a in ni: R[ni[a], len(caps) + j] -= 1.0
            if b in ni: R[ni[b], len(caps) + j] += 1.0
        R[bi["Vin"], ns] = 1.0
        X = np.linalg.solve(G, R)
        # The branch current of a voltage source stamped (a, b) flows a -> b through it: for a capacitor
        # replaced by a source, that is the capacitor current.
        A = np.zeros((ns, ns))
        B = np.zeros(ns)
        for k, c in enumerate(caps):
            row = X[bi[c.name]] / vals[c.name]
            A[k], B[k] = row[:ns], row[ns]
        if inds:
            vL = np.zeros((len(inds), ns + 1))
            for j, l in enumerate(inds):
                a, b = l.nodes
                va = X[ni[a]] if a in ni else 0.0
                vb = X[ni[b]] if b in ni else 0.0
                vL[j] = va - vb
            Lm = np.diag([vals[l.name] for l in inds])
            M = self._mutual(vals)
            names = [l.name for l in inds]
            for (a, b), m in M.items():
                Lm[names.index(a), names.index(b)] = m
            d = np.linalg.solve(Lm, vL)
            A[len(caps):] = d[:, :ns]
            B[len(caps):] = d[:, ns]
        out = X[ni[node or self.out]]
        return A, B, out[:ns].copy(), float(out[ns])

    @staticmethod
    def ss_response(ss, freqs) -> np.ndarray:
        A, B, C, D = ss
        f = np.atleast_1d(np.asarray(freqs, float))
        I = np.eye(A.shape[0])
        return np.array([C @ np.linalg.solve(2j * np.pi * fi * I - A, B) + D for fi in f])

    # ---- symbolic ---------------------------------------------------------------------------
    def symbolic_H(self, node: str | None = None):
        """(H, s, symbols): H(s) = V(node)/u as a sympy rational function of the element symbols."""
        import sympy as sp
        s = sp.Symbol("s")
        sym = {e.name: sp.Symbol(e.name, positive=e.kind != "E") for e in self.elements}
        nodes, ni, bi, n = self._index()
        Y = sp.zeros(n, n)
        rhs = sp.zeros(n, 1)

        def st(a, b, y):
            ia, ib = ni.get(a), ni.get(b)
            if ia is not None: Y[ia, ia] += y
            if ib is not None: Y[ib, ib] += y
            if ia is not None and ib is not None:
                Y[ia, ib] -= y
                Y[ib, ia] -= y

        def vb(row, a, b):
            for nd, sg in ((a, 1), (b, -1)):
                if nd in ni:
                    Y[ni[nd], row] += sg
                    Y[row, ni[nd]] += sg

        vb(bi["Vin"], self.inp, GND)
        rhs[bi["Vin"]] = 1
        for e in self.elements:
            if e.kind == "R":
                st(*e.nodes, 1 / sym[e.name])
            elif e.kind == "C":
                st(*e.nodes, s * sym[e.name])
            elif e.kind == "L":
                r = bi[e.name]
                vb(r, *e.nodes)
                Y[r, r] -= s * sym[e.name]
            elif e.kind == "K":
                a, b = e.refs
                m = sym[e.name] * sp.sqrt(sym[a] * sym[b])
                Y[bi[a], bi[b]] -= s * m
                Y[bi[b], bi[a]] -= s * m
            elif e.kind == "E":
                r = bi[e.name]
                op, om, cp, cm = e.nodes
                vb(r, op, om)
                for nd, sg in ((cp, 1), (cm, -1)):
                    if nd in ni: Y[r, ni[nd]] -= sg * sym[e.name]
        x = Y.LUsolve(rhs)
        H = sp.cancel(sp.together(x[ni[node or self.out]]))
        return H, s, sym

    # ---- ngspice ----------------------------------------------------------------------------
    def spice_deck(self, values: dict | None = None, title: str = "lkit netlist") -> str:
        vals = self.values(values)
        nm = lambda x: "0" if x == GND else x  # noqa: E731
        L = [f"* {title}", f"Vin {nm(self.inp)} 0 AC 1"]
        for e in self.elements:
            if e.kind in ("R", "C", "L"):
                L.append(f"{e.kind}{e.name} {nm(e.nodes[0])} {nm(e.nodes[1])} {vals[e.name]!r}")
            elif e.kind == "K":
                L.append(f"K{e.name} L{e.refs[0]} L{e.refs[1]} {vals[e.name]!r}")
            elif e.kind == "E":
                L.append(f"E{e.name} " + " ".join(nm(x) for x in e.nodes) + f" {vals[e.name]!r}")
        L.append(".end")
        return "\n".join(L) + "\n"
