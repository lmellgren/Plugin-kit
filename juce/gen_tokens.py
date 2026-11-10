"""Generate Tokens.h from the shared Seam design system's tokens.json (read-only, at build time).

Usage: python3 gen_tokens.py <seam design dir>/tokens.json <out>/Tokens.h
Never copy design files into this repo: the tokens come from ~/Apps/Seam/design.
"""
import json
import re
import sys


def ident(name):
    parts = re.split(r"[^A-Za-z0-9]+", name)
    s = parts[0] + "".join(p[:1].upper() + p[1:] for p in parts[1:])
    return ("k" + s[:1].upper() + s[1:]) if s[:1].isdigit() else s


def colour(v):
    v = v.strip()
    if v.startswith("#"):
        h = v[1:]
        if len(h) == 3:
            h = "".join(c * 2 for c in h)
        return f"0xff{h.lower()}"
    m = re.match(r"rgba?\(([^)]*)\)", v)
    r, g, b, *a = [x.strip() for x in m.group(1).split(",")]
    alpha = round(float(a[0]) * 255) if a else 255
    return f"0x{alpha:02x}{int(r):02x}{int(g):02x}{int(b):02x}"


def px(v):
    return float(str(v).replace("px", ""))


def main(src, dst):
    t = json.load(open(src))
    L = ["// GENERATED at build time by Plugin-kit juce/gen_tokens.py from the Seam design system", f"// ({src}).",
         "// Do not edit and do not commit: the design lives in ~/Apps/Seam/design.", "#pragma once", "#include <cstdint>", "",
         "namespace seam {", "namespace colour {"]
    for c in t["color"]["tokens"]:
        L.append(f"inline constexpr std::uint32_t {ident(c['name'])} = {colour(c['value'])};   // {c['name']}")
    L += ["} // namespace colour", "namespace space {"]
    for c in t["spacing"]["tokens"]:
        L.append(f"inline constexpr float {ident(c['name'])} = {px(c['value'])}f;")
    L += ["} // namespace space", "namespace radius {"]
    for c in t["radius"]["tokens"]:
        if str(c["value"]).endswith("px"):
            L.append(f"inline constexpr float {ident(c['name'])} = {px(c['value'])}f;")
    L += ["} // namespace radius", "namespace size {"]
    for c in t["size"]["tokens"]:
        if str(c["value"]).endswith("px"):
            L.append(f"inline constexpr float {ident(c['name'])} = {px(c['value'])}f;")
    L += ["} // namespace size", "struct TextStyle { float size, lineHeight; int weight; };", "namespace type {"]
    for g in t["type"]["groups"]:
        for st in g["styles"]:
            L.append(f"inline constexpr TextStyle {ident(st['name'])}{{{px(st['fontSize'])}f, {px(st['lineHeight'])}f, "
                     f"{int(st['fontWeight'])}}};")
    fam = t["type"]["families"]["sans"].split(",")[0].strip()
    L += [f'inline constexpr const char* family = "{fam}";', "} // namespace type", "} // namespace seam", ""]
    open(dst, "w").write("\n".join(L))


if __name__ == "__main__":
    main(sys.argv[1], sys.argv[2])
