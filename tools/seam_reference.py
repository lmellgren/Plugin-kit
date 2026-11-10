"""Render Seam's reference for a plugin window (read-only) to a PNG, for side-by-side review.

Imports <design>/tools/seamgen.py without changing anything, evaluates an expression in its namespace
(e.g. a plugin_window(...) call), writes a temporary HTML page and screenshots it with headless Chrome.
Generalised from Passive EQ tools/seam_reference.py.

Usage (seamgen.py needs Python 3.12+):
  python3.13 seam_reference.py OUT.png --expr "plugin_window('Pre 73', pre73_body(standalone=True))" \
      --size 460x480 [--design ~/Apps/Seam/design]
"""
import argparse
import subprocess
import sys
import tempfile
from pathlib import Path

ap = argparse.ArgumentParser()
ap.add_argument("out")
ap.add_argument("--expr", required=True, help="Python expression in seamgen's namespace returning HTML")
ap.add_argument("--size", default="744x366", help="window size WxH in px")
ap.add_argument("--design", default=str(Path.home() / "Apps/Seam/design"))
a = ap.parse_args()

out = Path(a.out).resolve()
design = Path(a.design).expanduser()
sys.argv = [str(design / "tools/seamgen.py"), str(design), tempfile.mkdtemp()]
sys.dont_write_bytecode = True   # the design system is read-only: no __pycache__ in Seam
sys.path.insert(0, str(design / "tools"))
import seamgen as S  # noqa: E402  (module level only reads tokens; build() runs under __main__)

body = eval(a.expr, vars(S))  # noqa: S307 - our own expression, read-only design code
w, h = a.size.split("x")
html = (f'<!doctype html><html><head><meta charset="utf-8">'
        f'<link href="https://fonts.googleapis.com/css2?family=Inter:wght@400;500;600&display=swap" rel="stylesheet">'
        f'<style>body{{margin:0;background:{S.T["bg400"]};font-family:Inter,system-ui,-apple-system,sans-serif}}</style>'
        f'</head><body>{body}</body></html>')
with tempfile.TemporaryDirectory() as td:
    page = Path(td) / "window.html"
    page.write_text(html)
    chrome = "/Applications/Google Chrome.app/Contents/MacOS/Google Chrome"
    subprocess.run([chrome, "--headless=new", "--disable-gpu", "--hide-scrollbars", "--force-device-scale-factor=1",
                    f"--window-size={w},{h}", f"--screenshot={out}", "--virtual-time-budget=4000", page.as_uri()],
                   check=True, capture_output=True)
print("wrote", out)
