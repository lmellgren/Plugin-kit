"""Staged, bounded fitting of physical parameters (LOMAS Plugin-kit; the Passive EQ M4 method).

A plugin defines its parameters (physical values with bounds) and a residual function over a set of
measured settings. Stages fit a few parameters at a time with the rest held; every stage is logged.
Holdout settings are refused: they are only for reporting (house rule, QA D1).

    P = [Param("Rs", 600, 300, 1200), Param("L1", 0.1, 0.05, 0.2)]
    fit = StagedFit(P, residual)                 # residual(values: dict, settings: list) -> 1-D array
    fit.stage("source", ["Rs"], grid_settings)
    fit.stage("inductor", ["L1", "Rs"], grid_settings)
    fit.values                                   # the fitted dict
"""
from __future__ import annotations

from dataclasses import dataclass, field

import numpy as np


@dataclass
class Param:
    name: str
    x0: float
    lo: float
    hi: float
    log: bool = True          # fit in log space (positive physical values)

    def to_x(self, v: float) -> float:
        return float(np.log(v)) if self.log else float(v)

    def from_x(self, x: float) -> float:
        return float(np.exp(x)) if self.log else float(x)

    def bounds(self) -> tuple:
        return (self.to_x(self.lo), self.to_x(self.hi))


class HoldoutError(ValueError):
    pass


def check_no_holdout(settings) -> None:
    bad = [s.get("id", "?") for s in settings if isinstance(s, dict) and s.get("holdout")]
    if bad:
        raise HoldoutError(f"holdout settings must never be fitted: {bad[:5]}{' ...' if len(bad) > 5 else ''}")


@dataclass
class StageResult:
    name: str
    params: list
    cost_before: float
    cost_after: float
    values: dict
    at_bounds: list = field(default_factory=list)
    nfev: int = 0


class StagedFit:
    def __init__(self, params: list, residual):
        self.params = {p.name: p for p in params}
        self.values = {p.name: p.x0 for p in params}
        self.residual = residual
        self.history: list[StageResult] = []

    def cost(self, settings, values: dict | None = None) -> float:
        r = np.asarray(self.residual(values or self.values, settings), float)
        return float(0.5 * np.sum(r * r))

    def stage(self, name: str, names: list, settings, **least_squares_kw) -> StageResult:
        from scipy.optimize import least_squares
        check_no_holdout(settings)
        ps = [self.params[n] for n in names]
        x0 = np.array([p.to_x(self.values[p.name]) for p in ps])
        lo = np.array([p.bounds()[0] for p in ps])
        hi = np.array([p.bounds()[1] for p in ps])
        x0 = np.clip(x0, lo, hi)

        def fun(x):
            v = dict(self.values)
            v.update({p.name: p.from_x(xi) for p, xi in zip(ps, x)})
            return np.asarray(self.residual(v, settings), float)

        before = self.cost(settings)
        kw = {"x_scale": "jac", "max_nfev": 400}
        kw.update(least_squares_kw)
        r = least_squares(fun, x0, bounds=(lo, hi), **kw)
        for p, xi in zip(ps, r.x):
            self.values[p.name] = p.from_x(xi)
        span = hi - lo
        at = [p.name for p, xi, l, h, sp_ in zip(ps, r.x, lo, hi, span) if min(xi - l, h - xi) < 1e-3 * sp_]
        res = StageResult(name, list(names), before, self.cost(settings), dict(self.values), at, int(r.nfev))
        self.history.append(res)
        return res

    def report_rows(self) -> list:
        return [{"stage": h.name, "params": ", ".join(h.params), "cost_before": h.cost_before,
                 "cost_after": h.cost_after, "at_bounds": ", ".join(h.at_bounds), "nfev": h.nfev}
                for h in self.history]
