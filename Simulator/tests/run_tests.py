"""Run every scenario in tests/scenarios/*.txt and report expect-assertion results.

A scenario's first lines may set options:  #! start=101 claire scenario=B auto-dismiss
  python tests/run_tests.py [name-filter] [-v]          Python prototype mod, tests/scenarios
  python tests/run_tests.py --cpp [name-filter] [-v]    the real C++ mod, tests/mod_scenarios
"""
import glob
import os
import sys

sys.path.insert(0, os.path.join(os.path.dirname(__file__), ".."))
from re2sim.session import Session          # noqa: E402
from re2sim.__main__ import find_data       # noqa: E402


def run(path, data, verbose=False, cpp=False):
    lines = open(path, encoding="utf-8").read().splitlines()
    opts = {}
    for l in lines:
        if l.startswith("#!"):
            for tok in l[2:].split():
                k, _, v = tok.partition("=")
                opts[k] = v or True
    s = Session(data, character=1 if opts.get("claire") else 0,
                scenario=1 if str(opts.get("scenario", "A")).upper() == "B" else 0,
                start=opts.get("start"), echo=verbose, auto_dismiss=bool(opts.get("auto-dismiss")),
                mod_impl="cpp" if cpp else "python")
    for l in lines:
        s.run(l)
    errors = [l for l in s.lines if l.startswith("[sim error]")]
    if cpp:
        s.mod.close()
    return s.results, errors, s


def main():
    verbose = "-v" in sys.argv
    cpp = "--cpp" in sys.argv
    filt = [a for a in sys.argv[1:] if not a.startswith("-")]
    data = find_data(None)
    here = os.path.dirname(__file__)
    total = failed = 0
    for path in sorted(glob.glob(os.path.join(here, "mod_scenarios" if cpp else "scenarios", "*.txt"))):
        name = os.path.basename(path)[:-4]
        if filt and not any(f in name for f in filt):
            continue
        results, errors, s = run(path, data, verbose, cpp)
        bad = [r for r in results if not r[0]]
        total += len(results)
        failed += len(bad) + len(errors)
        status = "ok" if not bad and not errors else "FAILED"
        print(f"{name}: {len(results) - len(bad)}/{len(results)} checks passed{', ' + str(len(errors)) + ' sim errors' if errors else ''} - {status}")
        for ok, d in bad:
            print(f"    FAIL {d}")
        for e in errors:
            print(f"    {e}")
    print(f"\n{total - failed if failed <= total else 0}/{total} checks passed" if total else "no checks")
    sys.exit(1 if failed else 0)


if __name__ == "__main__":
    main()
