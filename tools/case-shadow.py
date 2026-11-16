#!/usr/bin/env python3
"""Make a case-fixing shadow include dir for building on a case-sensitive filesystem.

The BakkesMod SDK ships lowercase file names, but code includes them with
Windows-style mixed case. For every #include in the given trees that only
resolves case-insensitively against the SDK, symlink the requested spelling
to the real file under OUT.  usage: case-shadow.py SDK_INCLUDE OUT SRC_DIR...
"""
import os, re, sys

sdk, out, srcs = sys.argv[1], sys.argv[2], sys.argv[3:]
real = {}
for root, _, files in os.walk(sdk):
    for f in files:
        rel = os.path.relpath(os.path.join(root, f), sdk)
        real[rel.lower()] = rel

# Mirror the whole SDK first, so includes relative to a linked header still resolve inside OUT
for rel in real.values():
    dst = os.path.join(out, rel)
    if not os.path.lexists(dst):
        os.makedirs(os.path.dirname(dst), exist_ok=True)
        os.symlink(os.path.join(sdk, rel), dst)

inc = re.compile(rb'#\s*include\s*[<"]([^>"]+)[>"]')
made = 0
for tree in [sdk, *srcs]:
    for root, _, files in os.walk(tree):
        for f in files:
            if not f.lower().endswith((".h", ".hpp", ".cpp", ".inl")):
                continue
            path = os.path.join(root, f)
            here = os.path.relpath(root, sdk) if path.startswith(sdk) else None
            for m in inc.finditer(open(path, "rb").read()):
                want = m.group(1).decode().replace("\\", "/")
                cands = [want]
                if here and here != ".":
                    cands.append(os.path.normpath(os.path.join(here, want)))
                for c in cands:
                    hit = real.get(c.lower())
                    if hit and hit != c:
                        dst = os.path.join(out, c)
                        if not os.path.lexists(dst):
                            os.makedirs(os.path.dirname(dst), exist_ok=True)
                            os.symlink(os.path.join(sdk, hit), dst)
                            made += 1
print(f"case-shadow: {made} links in {out}")
