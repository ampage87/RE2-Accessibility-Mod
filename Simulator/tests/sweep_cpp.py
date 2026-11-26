"""Room sweep with the real C++ mod: for each room, enter it by each door that leads there, list
the catalog's exits, and for each exit select it, auto-walk there ('go'), press action and answer
Yes, and see whether the room changes. Enemies are removed (this tests navigation).

  python tests/sweep_cpp.py --rooms 400,401 [--claire] [--out report.txt] [--flags 1.0=1,3.34=1]
"""
import argparse
import os
import sys
import time

sys.path.insert(0, os.path.join(os.path.dirname(__file__), ".."))

from re2sim.session import Session  # noqa: E402
from re2sim.__main__ import find_data  # noqa: E402
from re2sim.mapdata import door_entries  # noqa: E402


def speech(s, start):
    return [l[8:] for l in s.lines[start:] if l.startswith("SPEECH: ")]


def fresh(data, character, rid, entry, flags, settle):
    s = Session(data, character=character, echo=False, start="104" if character else "100",
                auto_dismiss=True, mod_impl="cpp")
    g = s.game
    g.watchdog_seconds = 8
    for (gr, ix), v in flags.items():
        g.set_flag(gr, ix, v)
    st, rm = int(rid[0], 16), int(rid[1:], 16)
    if entry:
        g.enter_room(st, rm, entry["x"], entry["y"], entry["z"], entry["d"], entry["cut"])
    else:
        g.enter_room(st, rm)
    s.tick(settle * 30)
    for e in g.enemies.values():
        e.alive = False
    g.release_lock()
    s.tick(30)
    return s


def exits_of(s):
    """Names in the catalog's Exits category."""
    n0 = len(s.lines)
    s.run("cat")
    names = []
    for _ in range(30):
        s.run("next")
        sp = speech(s, n0)
        if not sp:
            break
        last = sp[-1]
        if " of " not in last:
            break
        name = last.rsplit(",", 1)[0]
        if name in names:
            break
        names.append(name)
    return names


def try_exit(s, name):
    g = s.game
    before = g.room.rid
    n0 = len(s.lines)
    s.run("find " + name)
    s.run("go")
    s.walk_until_arrived(90)
    s.tick(30)
    said = speech(s, n0)
    if g.room.rid != before:
        return f"ok -> {g.room.rid}", said
    s.run("action")
    for _ in range(4):
        s.tick(30)
        if g.message is not None:
            g.answer(True)
        if g.room.rid != before:
            break
    s.tick(60)
    said = speech(s, n0)
    if g.room.rid != before:
        return f"ok -> {g.room.rid}", said
    msgs = [x for x in said if not x.startswith(("Walking", "Arrived", "Facing")) and " of " not in x]
    return "stayed", said if not msgs else msgs


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--data")
    ap.add_argument("--claire", action="store_true")
    ap.add_argument("--rooms", required=True)
    ap.add_argument("--flags", default="")
    ap.add_argument("--settle", type=int, default=6)
    ap.add_argument("--out")
    a = ap.parse_args()
    data = find_data(a.data)
    ch = 1 if a.claire else 0
    flags = {}
    for f in filter(None, a.flags.split(",")):
        k, v = f.split("=")
        gr, ix = k.split(".")
        flags[(int(gr), int(ix))] = int(v)
    s0 = Session(data, character=ch, echo=False)
    ents = door_entries(s0.game.lib, ch)
    out = []
    t0 = time.time()
    for rid in a.rooms.upper().split(","):
        seen, es = set(), []
        for e in ents.get(rid, []):
            k = (e["x"] // 500, e["z"] // 500, e["y"])
            if k not in seen:
                seen.add(k)
                es.append(e)
        es = es[:4] or [None]
        for ent in es:
            frm = ent["frm"] if ent else "-"
            try:
                s = fresh(data, ch, rid, ent, flags, a.settle)
            except Exception as ex:
                out.append(f"{rid} from {frm}: could not enter: {ex}")
                continue
            if s.game.room.rid != rid:
                out.append(f"{rid} from {frm}: script moved the player to {s.game.room.rid}")
                continue
            names = exits_of(s)
            out.append(f"{rid} from {frm}: exits {names}")
            for nm in names:
                try:
                    t = fresh(data, ch, rid, ent, flags, a.settle)
                    res, said = try_exit(t, nm)
                except Exception as ex:
                    res, said = f"error {ex}", []
                line = f"   {nm}: {res}"
                if not res.startswith("ok"):
                    line += "  | " + " / ".join(said[-6:])
                out.append(line)
                print(line, flush=True)
            print(out[-len(names) - 1], flush=True)
    out.append(f"time {time.time() - t0:.0f}s")
    text = "\n".join(out)
    if a.out:
        open(a.out, "w").write(text)
    print(text)


if __name__ == "__main__":
    main()
