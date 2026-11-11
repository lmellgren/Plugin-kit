"""Model values -> C++ header (moved from Pre 73 circuit/gen_params_header.py, Vari-mu P1).

The realtime core reads the circuit model's component values (nominal merged with fitted) from a generated header:
component values only, never response tables (house rule: physical model + fit, not curve tables).

    from lkit.params_header import write_params_header
    write_params_header({**NOMINAL, **fitted}, "core/params/VariMuParams.h", namespace="varimu::params",
                        source="circuit/gen_params_header.py from circuit/varimu_circuit.py + circuit/fit/fitted.json")

Keys become C++ identifiers ('.' -> 'p', anything else not [A-Za-z0-9_] -> '_'); bools -> constexpr bool, numbers ->
constexpr double (repr, so the value round-trips exactly). Sorted, so the file is stable under dict order.
"""
from __future__ import annotations

import re
from pathlib import Path


def cpp_name(key: str) -> str:
    n = re.sub(r"[^A-Za-z0-9_]", "_", key.replace(".", "p"))
    return "_" + n if n[:1].isdigit() else n


def params_header(values: dict, namespace: str, source: str = "", note: str = "") -> str:
    names = {}
    for k in values:
        n = cpp_name(k)
        if n in names:
            raise ValueError(f"keys {names[n]!r} and {k!r} both map to {n!r}")
        names[n] = k
    L = [f"// GENERATED{' by ' + source if source else ''}. Do not edit by hand."]
    if note:
        L.append(f"// {note}")
    L += ["#pragma once", "", f"namespace {namespace} {{"]
    for k in sorted(values):
        v = values[k]
        if isinstance(v, bool):
            L.append(f"inline constexpr bool {cpp_name(k)} = {'true' if v else 'false'};")
        else:
            L.append(f"inline constexpr double {cpp_name(k)} = {float(v)!r};")
    L += [f"}} // namespace {namespace}", ""]
    return "\n".join(L)


def write_params_header(values: dict, out: str | Path, namespace: str, source: str = "", note: str = "") -> Path:
    out = Path(out)
    out.parent.mkdir(parents=True, exist_ok=True)
    text = params_header(values, namespace, source, note)
    if not out.exists() or out.read_text() != text:   # keep the timestamp when nothing changed (no rebuild)
        out.write_text(text)
    return out
