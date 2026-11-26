"""Auto-player: plays forward using only the mod's own hotkeys, like a blind player would.

Strategy per room: pick up reachable items, then take the nearest exit not yet used
(preferring unlocked ones).  When the mod reports a close enemy, aim and fire.
Prints the speech transcript and a summary of rooms reached, items taken and simulator gaps.

  python tests/autoplay.py [--rooms 30] [--claire] [--quiet]
"""
import argparse
import os
import sys

sys.path.insert(0, os.path.join(os.path.dirname(__file__), ".."))
from re2sim.session import Session          # noqa: E402
from re2sim.__main__ import find_data       # noqa: E402


def fight(s):
    for _ in range(8):
        close = [e for e in s.view.enemies() if e.hostile and e.alive and s.view.in_play(e)
                 and ((e.x - s.view.x) ** 2 + (e.z - s.view.z) ** 2) ** 0.5 < 3500]
        if not close or s.game.dead:
            return
        s.run("target")
        s.run("fire 3")


def walk_to(s, aot_id, tries=6):
    """go -> fight if stopped -> go again, until the target is in reach."""
    for _ in range(tries):
        s.mod.selected = aot_id
        if not s.mod.hotkey("go"):
            return False
        s.walk_until_arrived(60)
        if any(x.id == aot_id for x in s.view.actionable()):
            return True
        fight(s)
        if s.game.dead:
            return False
    return False


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--rooms", type=int, default=30)
    ap.add_argument("--claire", action="store_true")
    ap.add_argument("--quiet", action="store_true")
    a = ap.parse_args()
    s = Session(find_data(None), character=1 if a.claire else 0, echo=not a.quiet, auto_dismiss=True)
    s.game.watchdog_seconds = 15
    s.run("give handgun bullets 200")
    used = set()
    path = [s.game.room.rid]
    stuck = 0
    while len(path) < a.rooms and not s.game.dead and stuck < 3:
        rid = s.game.room.rid
        s.run("wait 2")
        fight(s)
        # items first
        for _ in range(6):
            items = [p for p in s.mod.points() if p.kind == "item"]
            if not items:
                break
            s.mod.selected = items[0].id
            s.run("where")
            if walk_to(s, items[0].id):
                s.run("action")
                s.run("yes")
            else:
                break
        def try_exits(cands):
            for ex in cands:
                used.add((rid, ex.id))
                s.mod.selected = ex.id
                s.run("where")
                if not walk_to(s, ex.id):
                    continue
                s.run("action")
                if s.game.message:
                    s.run("ok")
                s.tick(10)
                if s.game.room.rid != rid:
                    path.append(s.game.room.rid)
                    return True
            return False

        def doors(fresh_only):
            ds = [p for p in s.mod.points() if p.kind == "door" and not p.door_locked]
            if fresh_only:
                ds = [p for p in ds if p.door_target not in path]
            return ds

        moved = try_exits(doors(True))
        if not moved and not s.game.dead:
            for _ in range(4):           # explore: events in unexplored parts often open new ways
                if not s.mod.hotkey("explore"):
                    break
                s.walk_until_arrived(60)
                fight(s)
            moved = try_exits(doors(True))
        if not moved and not s.game.dead:
            back = [p for p in doors(False) if (rid, p.id) not in used] or doors(False)
            back.sort(key=lambda p: path[::-1].index(p.door_target) if p.door_target in path else 99, reverse=True)
            moved = try_exits(back)
        if not moved:
            # explore the open parts of the room (may set off events that open new paths)
            for _ in range(4):
                if not s.mod.hotkey("explore"):
                    break
                s.walk_until_arrived(60)
                fight(s)
            # allow re-using exits once everything here has been tried
            used = {u for u in used if u[0] != rid}
            stuck += 1
        else:
            stuck = 0
    print("\n=== autoplay summary")
    print("rooms visited in order:", " -> ".join(path))
    print("inventory:", ", ".join(f"{n} {q}" for i, q, n in s.view.inventory()))
    print("health:", s.view.condition, s.view.hp, "dead" if s.game.dead else "")
    print("simulator gaps:")
    for g in s.game.gaps:
        print("  -", g)


if __name__ == "__main__":
    main()
