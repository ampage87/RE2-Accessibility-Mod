"""Whole-game sweep: for every room, load it, run its scripts for a few seconds, then for each
exit try to auto-walk there (mod path-finding + real movement/collision) and press action.

Reports per room: script errors, simulator gaps, exits that cannot be reached or used.

  python tests/sweep_rooms.py [--claire] [--rooms 100,101] [--out report.txt]
"""
import argparse
import os
import sys
import time

sys.path.insert(0, os.path.join(os.path.dirname(__file__), ".."))

from re2sim.session import Session  # noqa: E402
from re2sim.__main__ import find_data  # noqa: E402


from re2sim.mapdata import door_entries  # noqa: E402


def start_session(data, character, rid, entry):
    s = Session(data, character=character, echo=False, start="100" if character == 0 else "104", auto_dismiss=True)
    s.game.watchdog_seconds = 8
    st, rm = int(rid[0], 16), int(rid[1:], 16)
    if entry:
        s.game.enter_room(st, rm, entry["x"], entry["y"], entry["z"], entry["d"], entry["cut"])
    else:
        s.game.enter_room(st, rm)
    return s


def sweep(data, character, only=None, settle=6):
    s0 = Session(data, character=character, echo=False)
    rooms = [r.rid for r in s0.game.lib.all_rooms(character)]
    entries = door_entries(s0.game.lib, character)
    if only:
        rooms = [r for r in rooms if r in only]
    results = []
    for rid in rooms:
        res = {"room": rid, "exits": [], "errors": [], "gaps": []}
        seen, ents = set(), []
        for e in entries.get(rid, []):
            k = (e["x"] // 500, e["z"] // 500, e["y"])
            if k not in seen:
                seen.add(k)
                ents.append(e)
        ents = ents[:6] or [None]
        res["entries"] = len(ents)
        s = start_session(data, character, rid, ents[0])
        try:
            s.tick(settle * 30)
        except Exception as e:
            res["errors"].append(f"settle: {type(e).__name__}: {e}")
        res["errors"] += [l for l in s.lines if l.startswith("[sim error]")]
        exits = [a for a in s.mod.points() if a.kind == "door"]
        res["n_exits"] = len(exits)
        res["n_items"] = len([a for a in s.mod.points() if a.kind == "item"])
        res["n_enemies"] = len([e for e in s.view.enemies() if e.hostile and e.alive and s.view.in_play(e)])
        start = (s.game.player.x, s.game.player.y, s.game.player.z, s.game.player.d)
        for a in exits:
          for ent in ents:
              t = start_session(data, character, rid, ent)
              t.tick(settle * 30)
              g = t.game
              if g.room.rid != rid:
                  continue
              # remove enemies so the walk is not interrupted; this sweep tests navigation
              for e in g.enemies.values():
                  e.alive = False
              g.release_lock()
              t.mod.selected = a.id
              outcome = "?"
              try:
                  if t.game.message:
                      g.answer(True)
                  r = t.mod.hotkey("go")
                  if not r:
                      outcome = "no path"
                  else:
                      t.walk_until_arrived(90)
                      in_reach = any(x.id == a.id for x in t.view.actionable())
                      if not in_reach:
                          outcome = "arrived but not usable" if not t.mod.autowalk else "walk timed out"
                      else:
                          before = g.room.rid
                          nev = len(g.events)
                          g.press_action()
                          t.tick(5)
                          locked = [d for f, k, d in g.events[nev:] if k == "door_locked"]
                          if locked and g.room.rid == before:
                              outcome = "message: locked"
                          if g.message:
                              msg = g.message.text
                              g.answer(True)
                              t.tick(5)
                              outcome = f"message: {msg[:60]}" if g.room.rid == before else f"ok -> {g.room.rid} (after: {msg[:40]})"
                          if g.room.rid != before:
                              outcome = f"ok -> {g.room.rid}"
                          elif outcome == "?":
                              outcome = "no transition"
              except Exception as e:
                  outcome = f"error {type(e).__name__}: {e}"
              res["gaps"] += [gp for gp in g.gaps if "spawn" not in gp]
              if outcome.startswith("ok") or outcome.startswith("message") or ent is ents[-1]:
                  res["exits"].append((a.id, a.door_target, a.door_locked, outcome + ("" if ent is None else f" [from {ent['frm']}]")))
                  break
        res["gaps"] += [gp for gp in s.game.gaps if "spawn" not in gp]
        res["gaps"] = sorted(set(res["gaps"]))
        results.append(res)
        print(f"{rid}: exits={res['n_exits']} items={res['n_items']} enemies={res['n_enemies']} "
              + "; ".join(f"{e[1]}{'(L)' if e[2] else ''}:{e[3]}" for e in res["exits"]), flush=True)
    return results


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--data")
    ap.add_argument("--claire", action="store_true")
    ap.add_argument("--rooms")
    ap.add_argument("--out")
    a = ap.parse_args()
    t0 = time.time()
    res = sweep(find_data(a.data), 1 if a.claire else 0, a.rooms.upper().split(",") if a.rooms else None)
    lines = []
    tot = sum(len(r["exits"]) for r in res)
    ok = sum(1 for r in res for e in r["exits"] if e[3].startswith("ok") or e[3].startswith("message"))
    lines.append(f"Rooms: {len(res)}  exits tested: {tot}  usable (moved or gave a door message): {ok}  "
                 f"time {time.time() - t0:.0f}s")
    for r in res:
        bad = [e for e in r["exits"] if not (e[3].startswith("ok") or e[3].startswith("message"))]
        if bad or r["errors"] or r["gaps"]:
            lines.append(f"\n== {r['room']}")
            for e in bad:
                lines.append(f"  exit aot {e[0]} -> {e[1]}{' (locked)' if e[2] else ''}: {e[3]}")
            for er in r["errors"]:
                lines.append(f"  ERROR {er}")
            for gp in r["gaps"]:
                lines.append(f"  gap: {gp}")
    text = "\n".join(lines)
    print(text)
    if a.out:
        with open(a.out, "w", encoding="utf-8") as f:
            f.write(text + "\n")


if __name__ == "__main__":
    main()
