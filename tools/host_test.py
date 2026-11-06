"""Build and run the display firmware's pure code on a computer.

    python3 tools/host_test.py                 the logic tests
    python3 tools/host_test.py --render DIR    plus every screen as a PNG in DIR
                                               (needs Pillow)

The logic run has two parts: the assertions inside test/host_main.cpp, and the
change fixture test/fixtures/glucose-change-cases.json, the same cases the
Sugar Sense apps run. The server shapes the snapshot's tail as [the change
partner, the newest reading]; this script shapes each case the same way and
checks that the display prints the fixture's text for it.
"""
import json
import os
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, ".."))
SRC = os.path.join(ROOT, "test", "host_main.cpp")
FIXTURE = os.path.join(ROOT, "test", "fixtures", "glucose-change-cases.json")
# Sugar Sense keeps a master copy of this fixture for all its apps. When this
# folder sits next to it, the two must carry the same constants and cases; on
# its own (this repository) that path simply is not there.
MASTER = os.path.abspath(os.path.join(ROOT, "..", "tools", "fixtures", "glucose-change-cases.json"))


def build(workdir):
    exe = os.path.join(workdir, "host")
    subprocess.run(["c++", "-std=c++17", "-O1", "-Wall", "-Wextra", "-Wno-unused-function",
                    "-o", exe, SRC], check=True)
    return exe


def check_fixture_copy():
    if not os.path.exists(MASTER):
        return 0
    master = json.load(open(MASTER, encoding="utf-8"))
    mine = json.load(open(FIXTURE, encoding="utf-8"))
    if master["constants"] == mine["constants"] and master["cases"] == mine["cases"]:
        return 0
    print("FAIL the change fixture's constants or cases differ from the master copy: bring them over and port the rule")
    return 1


def run_fixture(exe):
    cases = json.load(open(FIXTURE, encoding="utf-8"))["cases"]
    bad = 0
    for case in cases:
        pts = sorted(case["points"], key=lambda p: -p["ageSec"])   # oldest first
        expect = case["expect"]
        partner_age = expect.get("partnerAgeSec")
        if partner_age is not None:
            prev = next(p for p in pts if p["ageSec"] == partner_age)
        elif len(pts) >= 2:
            prev = pts[-2]          # no partner: the server sends the two newest readings
        else:
            prev = None
        if prev is None:
            got = None              # a lone reading: the display has no pair to show
        else:
            out = subprocess.run([exe, "tail", "1" if case["isMmol"] else "0", str(prev["ageSec"]),
                                  str(prev["mgdl"]), str(pts[-1]["mgdl"])],
                                 check=True, capture_output=True, text=True).stdout.strip()
            got = None if out == "null" else out
        if got != expect.get("text"):
            bad += 1
            print(f"FAIL fixture {case['id']}: got {got!r}, want {expect.get('text')!r}")
    print(f"change fixture: {len(cases) - bad}/{len(cases)} ok")
    return bad


def render(exe, outdir):
    from PIL import Image
    os.makedirs(outdir, exist_ok=True)
    with tempfile.TemporaryDirectory() as raw:
        subprocess.run([exe, "render", raw], check=True)
        for name in sorted(os.listdir(raw)):
            stem = name[:-4]
            w, h = (int(v) for v in stem.rsplit("_", 1)[1].split("x"))
            data = open(os.path.join(raw, name), "rb").read()
            img = Image.new("RGB", (w, h))
            px = img.load()
            for i in range(w * h):
                c = data[2 * i] | (data[2 * i + 1] << 8)
                r, g, b = (c >> 11) & 31, (c >> 5) & 63, c & 31
                px[i % w, i // w] = ((r * 527 + 23) >> 6, (g * 259 + 33) >> 6, (b * 527 + 23) >> 6)
            img.save(os.path.join(outdir, stem.rsplit("_", 1)[0] + ".png"))
    print("rendered to", outdir)


def main():
    with tempfile.TemporaryDirectory() as work:
        exe = build(work)
        failed = subprocess.run([exe, "logic"]).returncode
        failed += check_fixture_copy()
        failed += run_fixture(exe)
        if "--render" in sys.argv:
            render(exe, sys.argv[sys.argv.index("--render") + 1])
    sys.exit(1 if failed else 0)


if __name__ == "__main__":
    main()
