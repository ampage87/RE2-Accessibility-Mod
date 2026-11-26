"""The RE2 accessibility mod logic (prototype), run inside the simulator.

The mod sees the game only through GameView (memory + room files) and the player's
key presses.  It produces speech (sent to Tolk/screen reader in the real DLL) and
audio cues (sound effects in the DLL; printed as [cue] lines here).

Hotkeys are named so they map onto real keys later; see HOTKEYS.
"""
from __future__ import annotations

import json
import math
import os

from .view import GameView
from .world import (relative_direction, metres, compass, dir_towards, angle_diff, facing_vector,
                    NavGrid, blocking_at, PLAYER_RADIUS, clock_position)
from .scd import item_name

HOTKEYS = {
    "next": "Cycle to next point of interest (doors, items, objects)",
    "prev": "Cycle to previous point of interest",
    "where": "Repeat direction and distance to the selected point",
    "go": "Auto-walk to the selected point (or as close as currently possible)",
    "explore": "Auto-walk to the farthest reachable part of the room you have not visited",
    "face": "Turn to face the selected point",
    "target": "Turn toward the nearest enemy",
    "compass": "Say which way you are facing",
    "room": "Say the room name and what is in it",
    "enemies": "List enemies with direction and distance",
    "status": "Say health condition",
    "inv": "Read the inventory",
    "scan": "Describe walls/obstacles around you",
    "repeat": "Repeat the last spoken line",
    "filter": "Change which kinds of points are cycled (all / exits / items / objects)",
}

KIND_LABEL = {"door": "Exit", "item": "Item", "message": "Examine", "event": "Interaction", "save": "Typewriter",
              "itembox": "Item box", "flag_chg": "Item-use spot", "hikidashi": "Search spot", "auto": "Interaction",
              "status": "Status", "windows": "Window", "water": "Water"}
FILTERS = ["all", "exits", "items", "objects"]


def _load_room_names():
    p = os.path.join(os.path.dirname(__file__), "room_names.json")
    if os.path.exists(p):
        with open(p, encoding="utf-8") as f:
            return {k.upper(): v for k, v in json.load(f).items() if not k.startswith("_")}
    return {}


class AccessibilityMod:
    def __init__(self, view: GameView, speak, cue=None, autowalk_driver=None):
        self.v = view
        self._speak = speak
        self._cue = cue or (lambda name, **kw: None)
        self.room_names = _load_room_names()
        self.last = ""
        self.selected = None          # aot id
        self.filter = "all"
        self.prev_room = None
        self.prev_msg = None
        self.prev_hp = None
        self.prev_condition = None
        self.prev_actionable = set()
        self.prev_enemy_hp = {}
        self.prev_pos = None
        self.bump_frames = 0
        self.bumped = False
        self.close_enemies = set()
        self.nav_cache = {}
        self.autowalk = None          # active path
        self.autowalk_target = None
        self.autowalk_stuck = 0
        self.prev_frozen = False
        self.prev_aiming = False
        self.visited = set()
        self.visited_cells = {}
        self.prev_ammo = None

    # ---------------------------------------------------------------- output
    def say(self, text, interrupt=False):
        self.last = text
        self._speak(text, interrupt)

    def cue(self, name, **kw):
        self._cue(name, **kw)

    def room_name(self, rid=None):
        rid = (rid or self.v.room_id).upper()
        return self.room_names.get(rid) or f"room {rid}"

    # ---------------------------------------------------------------- polling
    def on_frame(self):
        v = self.v
        # room change
        if v.room_id != self.prev_room:
            self.prev_room = v.room_id
            self.selected = None
            self.autowalk = None
            self.close_enemies = set()
            self.prev_actionable = set()
            self.prev_enemy_hp = {e.id: e.hp for e in v.enemies()}
            first = v.room_id not in self.visited
            self.visited.add(v.room_id)
            self.announce_room(brief=not first)
        # stairs and level changes.  Height bands are 1800 units; a storey is several bands,
        # so announce stairs as a unit (on / off) and sudden jumps (climb, drop) as level changes.
        on_stairs = v.on_stairs()
        if getattr(self, "_prev_floor_room", None) == v.room_id:
            if on_stairs and not self._prev_stairs:
                self._stairs_y = self._prev_y
                self.say("Stairs.")
            elif not on_stairs and self._prev_stairs:
                dy = v.y - self._stairs_y
                if dy <= -900:
                    self.say("Top of the stairs.")
                elif dy >= 900:
                    self.say("Bottom of the stairs.")
            elif not on_stairs and abs(v.y - self._prev_y) >= 900 and not v.frozen:
                self.say("Up a level." if v.y < self._prev_y else "Down a level.")
        self._prev_floor, self._prev_floor_room = v.floor, v.room_id
        self._prev_stairs, self._prev_y = on_stairs, v.y
        # on-screen text
        msg = v.message
        if msg != self.prev_msg:
            self.prev_msg = msg
            if msg:
                text, yesno = msg
                if text:
                    self.say(text + (" Yes or no." if yesno and "[Yes/No]" not in text else ""), interrupt=True)
        # health
        if self.prev_hp is not None and v.hp < self.prev_hp and not v.dead:
            if v.condition != self.prev_condition:
                self.say(f"Condition {v.condition}.", interrupt=True)
            else:
                self.cue("hurt")
        self.prev_hp = v.hp
        self.prev_condition = v.condition
        if v.dead and not getattr(self, "_said_dead", False):
            self._said_dead = True
            self.say("You died.", interrupt=True)
        # frozen by a cutscene / event
        if v.frozen and not self.prev_frozen and not (v.message and v.message[0]):
            self.cue("event_start")
        if not v.frozen and self.prev_frozen:
            self.cue("event_end")
        self.prev_frozen = v.frozen
        # things in reach
        act = {a.id: a for a in v.actionable()}
        new = set(act) - self.prev_actionable
        for aid in new:
            a = act[aid]
            self.say(f"{self.describe_aot(a, short=True)} in front of you.")
        self.prev_actionable = set(act)
        climb = v.climbable()
        if climb and climb != getattr(self, "_prev_climb", None):
            self.say(f"You can {climb} here.")
        self._prev_climb = climb
        # wall bump: forward/back held but no movement for several frames
        pos = (round(v.x), round(v.z))
        moving_keys = {"up", "down"} & v.held
        if moving_keys and not v.frozen and pos == self.prev_pos:
            self.bump_frames += 1
            if self.bump_frames == 3 and not self.bumped:
                self.bumped = True
                self.cue("wall")
                self.say(self.wall_description(), interrupt=False)
        else:
            if pos != self.prev_pos:
                self.bumped = False
            self.bump_frames = 0
        self.prev_pos = pos
        self._mark_visited()
        # enemies
        for e in v.enemies():
            if not e.hostile:
                continue
            prev = self.prev_enemy_hp.get(e.id)
            if prev is not None and e.hp < prev:
                if not e.alive:
                    self.say(f"{self.enemy_label(e)} down.", interrupt=True)
                else:
                    self.cue("hit", hp=e.hp)
            self.prev_enemy_hp[e.id] = e.hp
            if not e.alive or not v.in_play(e):
                self.close_enemies.discard(e.id)
                continue
            dist = math.hypot(e.x - v.x, e.z - v.z)
            if dist < 5000 and e.id not in self.close_enemies:
                self.close_enemies.add(e.id)
                self.say(f"{self.enemy_label(e)} {metres(dist)}, {relative_direction(v.dir, v.x, v.z, e.x, e.z)}.", interrupt=True)
            elif dist > 7000:
                self.close_enemies.discard(e.id)
        # ammo in the equipped weapon (inventory slot quantity)
        ammo = None
        if v.equipped and v.equipped != 1:
            ammo = next((q for i, q, n in v.inventory() if i == v.equipped), 0)
        if self.prev_ammo is not None and ammo is not None and ammo != self.prev_ammo:
            if ammo == 0:
                self.say("Magazine empty." if self._spare_ammo() else "Out of ammo.", interrupt=True)
            elif ammo > self.prev_ammo:
                self.say(f"Reloaded, {ammo}.")
            elif ammo <= 3:
                self.say(f"{ammo} left.")
        self.prev_ammo = ammo
        if v.aiming and not self.prev_aiming:
            tgt = self.aim_target()
            if tgt:
                self.say(f"Aiming at {self.enemy_label(tgt)}, {metres(math.hypot(tgt.x - v.x, tgt.z - v.z))}.")
            else:
                self.say("Aiming. No target in front.")
        self.prev_aiming = v.aiming

    def _spare_ammo(self):
        from .scd import AMMO_FOR
        kind = AMMO_FOR.get(self.v.equipped)
        return sum(q for i, q, n in self.v.inventory() if i == kind)

    def on_input(self, key):
        """Discrete button presses seen by the input hook."""
        v = self.v
        if key == "action" and not v.message and not v.aiming and not v.actionable() and not v.climbable():
            self.cue("nothing_here")
        if key == "fire" and v.equipped and v.equipped != 1:
            ammo = next((q for i, q, n in v.inventory() if i == v.equipped), 0)
            if ammo == 0 and not self._spare_ammo():
                self.say("Empty.", interrupt=True)

    # ---------------------------------------------------------------- descriptions
    def enemy_label(self, e):
        n = e.name
        if n.startswith("Zombie (") or n == "Zombie (Random)":
            return "Zombie"
        return n

    def aim_target(self):
        v = self.v
        fx, fz = facing_vector(v.dir)
        best = None
        for e in v.enemies():
            if not e.alive or not e.hostile:
                continue
            dx, dz = e.x - v.x, e.z - v.z
            along = dx * fx + dz * fz
            if along > 0 and abs(dx * fz - dz * fx) < 900:
                if best is None or along < best[0]:
                    best = (along, e)
        return best[1] if best else None

    def describe_aot(self, a, short=False):
        if a.kind == "door":
            name = self.room_name(a.door_target)
            lock = ", locked" if a.door_locked else ""
            return f"Exit to {name}{lock}"
        if a.kind == "item":
            amt = f", {a.item_amount}" if a.item_amount > 1 else ""
            return f"{item_name(a.item)}{amt}"
        label = KIND_LABEL.get(a.kind, a.kind)
        door = self._static_door_at(a)
        if door and a.kind in ("event", "message"):
            return f"Door to {self.room_name(door)}"
        if a.kind == "message":
            text = self._message_preview(a)
            if text and not short:
                return f"{label}: {text}"
        return label

    def _static_doors(self):
        """Door AOTs declared anywhere in the room's scripts (the DLL can parse the RDT the same way).
        Scripts often re-type a door into an event (locked, cutscene, 'won't open')."""
        rid = self.v.room_id
        cache = getattr(self, "_sd_cache", {})
        if rid not in cache:
            from .scd import decode
            from .world import rect_quad
            doors = []
            for proc in self.v.room.scd_init + self.v.room.scd_main:
                for ins in decode(proc):
                    if ins.name == "door_aot_se":
                        a = ins.args
                        doors.append((rect_quad(a[5], a[6], a[7], a[8]), f"{a[13] + 1:X}{a[14]:02X}"))
                    elif ins.name == "door_aot_set_4p":
                        a = ins.args
                        q = [(a[5], a[6]), (a[7], a[8]), (a[9], a[10]), (a[11], a[12])]
                        doors.append((q, f"{a[17] + 1:X}{a[18]:02X}"))
            cache[rid] = doors
            self._sd_cache = cache
        return cache[rid]

    def _static_door_at(self, a):
        cx, cz = a.centre
        from .world import point_in_quad, quad_centre
        for q, target in self._static_doors():
            qx, qz = quad_centre(q)
            if point_in_quad(cx, cz, q) or math.hypot(qx - cx, qz - cz) < 600:
                return target
        return None

    def _message_preview(self, a):
        g = self.v._g
        aot = g.aots.get(a.id)
        if aot and aot.data:
            mid = aot.data[0]
            msgs = self.v.room.messages
            if mid < len(msgs):
                words = msgs[mid].split()
                return " ".join(words[:6]) + ("..." if len(words) > 6 else "")
        return ""

    def points(self):
        pts = [a for a in self.v.aots() if a.kind not in ("normal", "damage", "status", "water", "auto")
               and not self._hidden(a)]
        if self.filter == "exits":
            pts = [a for a in pts if a.kind == "door"]
        elif self.filter == "items":
            pts = [a for a in pts if a.kind == "item"]
        elif self.filter == "objects":
            pts = [a for a in pts if a.kind not in ("door", "item")]
        # skip invisible helper triggers: event AOTs that fire automatically
        pts = [a for a in pts if a.manual or a.kind in ("door", "item")]
        v = self.v
        pts.sort(key=lambda a: math.hypot(a.centre[0] - v.x, a.centre[1] - v.z))
        return pts

    def _hidden(self, a):
        """Scripts park trigger areas they fire themselves (aot_on) at a zero-size spot or
        far outside the room. Those are not places the player can walk to."""
        xs = [p[0] for p in a.quad]
        zs = [p[1] for p in a.quad]
        if max(xs) - min(xs) < 60 or max(zs) - min(zs) < 60:
            return True
        cols = self.v.collisions
        if cols:
            minx = min(c.x for c in cols); maxx = max(c.x + c.w for c in cols)
            minz = min(c.z for c in cols); maxz = max(c.z + c.d for c in cols)
            cx, cz = a.centre
            if not (minx - 2000 <= cx <= maxx + 2000 and minz - 2000 <= cz <= maxz + 2000):
                return True
        return False

    def locate(self, a):
        v = self.v
        cx, cz = a.centre
        dist = math.hypot(cx - v.x, cz - v.z)
        return f"{metres(dist)}, {relative_direction(v.dir, v.x, v.z, cx, cz)}"

    def announce_room(self, brief=False):
        v = self.v
        name = self.room_name()
        pts = self.points()
        exits = [a for a in pts if a.kind == "door"]
        items = [a for a in pts if a.kind == "item"]
        parts = [f"{name}."]
        if not brief:
            parts.append(f"{len(exits)} exit{'s' if len(exits) != 1 else ''}")
            if items:
                parts.append(f"{len(items)} item{'s' if len(items) != 1 else ''}")
        hostile = [e for e in v.enemies() if e.hostile and e.alive and self.v.in_play(e)]
        if hostile:
            parts.append(f"{len(hostile)} enem{'ies' if len(hostile) != 1 else 'y'} nearby")
        self.say(" ".join(parts[:1]) + (" " + ", ".join(parts[1:]) + "." if len(parts) > 1 else ""), interrupt=True)

    def wall_description(self):
        return "Blocked."

    # ---------------------------------------------------------------- hotkeys
    def hotkey(self, key, arg=None):
        fn = getattr(self, f"key_{key}", None)
        if fn is None:
            self.say(f"Unknown mod key {key}.")
            return
        return fn(arg) if arg is not None else fn()

    def _selected_aot(self):
        for a in self.v.aots():
            if a.id == self.selected:
                return a
        return None

    def key_next(self, step=1):
        pts = self.points()
        if not pts:
            self.say("Nothing to select here.")
            return
        ids = [a.id for a in pts]
        if self.selected in ids:
            i = (ids.index(self.selected) + step) % len(ids)
        else:
            i = 0 if step > 0 else len(ids) - 1
        a = pts[i]
        self.selected = a.id
        self.say(f"{self.describe_aot(a)}. {self.locate(a)}. {i + 1} of {len(pts)}.", interrupt=True)

    def key_prev(self):
        self.key_next(-1)

    def key_where(self):
        a = self._selected_aot()
        if not a:
            self.say("Nothing selected.")
            return
        self.say(f"{self.describe_aot(a, short=True)}, {self.locate(a)}.", interrupt=True)

    def key_compass(self):
        self.say(f"Facing {compass(self.v.dir)}.", interrupt=True)

    def key_room(self):
        self.announce_room(brief=False)

    def key_status(self):
        v = self.v
        extra = ", poisoned" if v.poisoned else ""
        self.say(f"{v.condition}{extra}. Health {v.hp} of {v.max_hp}.", interrupt=True)

    def key_inv(self):
        inv = self.v.inventory()
        if not inv:
            self.say("Inventory empty.")
            return
        parts = []
        for i, q, n in inv:
            eq = ", equipped" if i == self.v.equipped else ""
            parts.append(f"{n}{' ' + str(q) if q > 1 or 2 <= i <= 19 else ''}{eq}")
        self.say("; ".join(parts) + ".", interrupt=True)

    def key_enemies(self):
        v = self.v
        es = [e for e in v.enemies() if e.hostile and e.alive and self.v.in_play(e)]
        if not es:
            self.say("No enemies detected.", interrupt=True)
            return
        es.sort(key=lambda e: math.hypot(e.x - v.x, e.z - v.z))
        parts = [f"{self.enemy_label(e)} {metres(math.hypot(e.x - v.x, e.z - v.z))} {relative_direction(v.dir, v.x, v.z, e.x, e.z)}"
                 for e in es[:5]]
        self.say(f"{len(es)} enem{'ies' if len(es) != 1 else 'y'}: " + "; ".join(parts) + ".", interrupt=True)

    def key_filter(self):
        self.filter = FILTERS[(FILTERS.index(self.filter) + 1) % len(FILTERS)]
        self.selected = None
        self.say(f"Showing {self.filter}.", interrupt=True)

    def key_repeat(self):
        self._speak(self.last, True)

    def key_scan(self):
        """Probe 8 directions for the distance to the nearest obstacle."""
        v = self.v
        cols = v.collisions
        out = []
        for label, off in (("ahead", 0), ("right", 1024), ("behind", 2048), ("left", 3072)):
            fx, fz = facing_vector(v.dir + off)
            dist = None
            for step in range(1, 40):
                px, pz = v.x + fx * step * 250, v.z + fz * step * 250
                if blocking_at(cols, v.floor, px, pz, radius=150):
                    dist = step * 250
                    break
            out.append(f"{label} {metres(dist) if dist else 'open'}")
        self.say("Walls: " + ", ".join(out) + ".", interrupt=True)

    def key_target(self):
        """Turn toward the nearest enemy (any direction). Returns ('face', dir)."""
        v = self.v
        es = [e for e in v.enemies() if e.hostile and e.alive and v.in_play(e)]
        if not es:
            self.say("No enemies.", interrupt=True)
            return None
        e = min(es, key=lambda e: math.hypot(e.x - v.x, e.z - v.z))
        self.say(f"Turning to {self.enemy_label(e)}, {metres(math.hypot(e.x - v.x, e.z - v.z))}.", interrupt=True)
        return ("face", dir_towards(v.x, v.z, e.x, e.z))

    def key_face(self):
        a = self._selected_aot()
        if not a:
            self.say("Nothing selected.")
            return None
        want = dir_towards(self.v.x, self.v.z, *a.centre)
        return ("face", want)

    def key_go(self):
        """Plan a path to the selected point; the driver steers along it each frame."""
        a = self._selected_aot()
        if not a:
            self.say("Nothing selected.")
            return None
        grid = self._grid()
        self._alt_grid = None
        gx, gz = self.stand_point(a, grid)
        path = grid.find_path(self.v.x, self.v.z, gx, gz, goal_radius_cells=0)
        if not path:
            # target may be on another level: plan on the collision layout of the other levels
            # that stairs in this room connect (re-planned again after each flight of stairs)
            for f in self._stair_floors():
                if f == self.v.floor:
                    continue
                g2 = self._grid(f)
                sx, sz = self.stand_point(a, g2, f)
                p2 = g2.find_path(self.v.x, self.v.z, sx, sz, goal_radius_cells=0)
                if p2:
                    path, gx, gz = p2, sx, sz
                    self._alt_grid = g2
                    break
        if path:
            # finish exactly on the stand point, but only via a segment with full clearance
            last = path[-1]
            used_grid = grid if not getattr(self, "_alt_grid", None) else self._alt_grid
            if used_grid._line_free(last, (gx, gz)):
                path.append((gx, gz))
            elif len(path) >= 2 and used_grid._line_free(path[-2], (gx, gz)):
                path[-1] = (gx, gz)
        if not path:
            # walk as close as the currently open area allows; something else must open the way
            reach = grid.reachable(self.v.x, self.v.z)
            if not reach:
                self.say("No path found.", interrupt=True)
                return None
            best = min(reach, key=lambda c: math.hypot(grid.to_world(*c)[0] - gx, grid.to_world(*c)[1] - gz))
            bx, bz = grid.to_world(*best)
            gap = math.hypot(bx - gx, bz - gz)
            path = grid.find_path(self.v.x, self.v.z, bx, bz, goal_radius_cells=0)
            if not path or gap < 400:
                self.say("No path found.", interrupt=True)
                return None
            self.say(f"Can't reach it from here yet; the way is blocked. Walking to the closest point, "
                     f"{metres(gap)} short.", interrupt=True)
            self.autowalk = path[1:] if len(path) > 1 else path
            self.autowalk_target = a.id
            self.autowalk_stuck = 0
            self._aw_floor = self.v.floor
            self._partial = True
            return ("go", path)
        self._partial = False
        self.autowalk = path[1:] if len(path) > 1 else path
        self.autowalk_target = a.id
        self.autowalk_stuck = 0
        self._aw_floor = self.v.floor
        if not getattr(self, "_replanning", False):
            self.say(f"Walking to {self.describe_aot(a, short=True)}.", interrupt=True)
        return ("go", path)

    def stand_point(self, a, grid, floor=None):
        """Where to stand to use an object: a free spot inside its trigger area, closest to its centre."""
        floor = self.v.floor if floor is None else floor
        from .world import point_in_quad
        cx, cz = a.centre
        xs = [p[0] for p in a.quad]
        zs = [p[1] for p in a.quad]
        best = None
        step = 50
        x = min(xs)
        while x <= max(xs):
            z = min(zs)
            while z <= max(zs):
                if point_in_quad(x, z, a.quad) and not blocking_at(self.v.collisions, floor, x, z):
                    d = math.hypot(x - cx, z - cz)
                    if best is None or d < best[0]:
                        best = (d, x, z)
                z += step
            x += step
        if best:
            return best[1], best[2]
        # trigger area sits inside a wall (most doors): stand at the free spot closest to it,
        # the game checks a point in front of the player
        x0, x1, z0, z1 = min(xs) - 1500, max(xs) + 1500, min(zs) - 1500, max(zs) + 1500
        x = x0
        while x <= x1:
            z = z0
            while z <= z1:
                if not blocking_at(self.v.collisions, floor, x, z):
                    dx = max(min(xs) - x, 0, x - max(xs))
                    dz = max(min(zs) - z, 0, z - max(zs))
                    d = math.hypot(dx, dz) * 10 + math.hypot(x - cx, z - cz) * 0.01
                    if best is None or d < best[0]:
                        best = (d, x, z)
                z += step
            x += step
        if best:
            return best[1], best[2]
        cell = grid.nearest_free(*grid.to_cell(cx, cz))
        return grid.to_world(*cell) if cell else (cx, cz)

    def _mark_visited(self):
        v = self.v
        key = v.room_id
        cells = self.visited_cells.setdefault(key, set())
        cells.add((int(v.x // 1500), int(v.z // 1500)))

    def key_explore(self):
        """Walk toward the farthest reachable part of the room you have not been to yet."""
        v = self.v
        grid = self._grid()
        reach = grid.reachable(v.x, v.z)
        seen = self.visited_cells.get(v.room_id, set())
        cand = []
        for c in reach:
            x, z = grid.to_world(*c)
            if (int(x // 1500), int(z // 1500)) in seen:
                continue
            cand.append((math.hypot(x - v.x, z - v.z), x, z))
        if not cand:
            self.say("You have been everywhere you can reach in this room.", interrupt=True)
            return None
        # the farthest unexplored reachable spot: crosses the most new ground per trip
        d, x, z = max(cand)
        path = grid.find_path(v.x, v.z, x, z, goal_radius_cells=1)
        if not path:
            self.say("Nothing new to explore.", interrupt=True)
            return None
        self.autowalk = path[1:] if len(path) > 1 else path
        self.autowalk_target = None
        self.selected = None
        self._partial = True
        self.say(f"Exploring, {metres(d)} {relative_direction(v.dir, v.x, v.z, x, z)}.", interrupt=True)
        return ("go", path)

    def _grid(self, floor=None):
        floor = self.v.floor if floor is None else floor
        key = (self.v.room_id, floor, self.v.collision_version)
        if key not in self.nav_cache:
            self.nav_cache[key] = NavGrid(self.v.collisions, floor, cell=200, radius=PLAYER_RADIUS - 50)
        return self.nav_cache[key]

    def _stair_floors(self):
        """Levels joined by stairs/ramps in this room, nearest to the player's level first."""
        out = set()
        for c in self.v.collisions:
            if c.shape in (11, 12) and c.floor_mask:
                low = (c.floor_mask & -c.floor_mask).bit_length() - 1
                out |= {low, low + c.rise}
        return sorted(out, key=lambda f: abs(f - self.v.floor))

    # ---------------------------------------------------------------- auto-walk steering
    def steer(self):
        """Called every frame while auto-walking. Returns the set of keys the mod presses."""
        v = self.v
        if not self.autowalk:
            return None
        if v.frozen or v.message:
            return set()
        for e in v.enemies():
            if e.hostile and e.alive and self.v.in_play(e) and math.hypot(e.x - v.x, e.z - v.z) < 2500:
                self.autowalk = None
                self.say(f"Stopped. {self.enemy_label(e)} close, {relative_direction(v.dir, v.x, v.z, e.x, e.z)}.", interrupt=True)
                return set()
        # after stairs the collision set is different: plan again from here
        if (self.autowalk_target is not None and not v.on_stairs()
                and v.floor != (getattr(self, "_aw_floor", v.floor) if getattr(self, "_aw_floor", v.floor) is not None else -1)):
            self._replanning = True
            try:
                self.key_go()
            finally:
                self._replanning = False
            if not self.autowalk:
                return set()
        # final step: turn to face the target object
        if self.autowalk[0] == "face" and getattr(self, "_partial", False):
            self.autowalk = None
            self._partial = False
            self.say("As close as you can get for now.", interrupt=True)
            return set()
        if self.autowalk[0] == "face":
            a = self._selected_aot()
            want = dir_towards(v.x, v.z, *a.centre) if a else v.dir
            if abs(angle_diff(v.dir, want)) <= 100:
                self.autowalk = None
                self.say(f"Arrived. Facing {self.describe_aot(a, short=True) if a else 'target'}.", interrupt=True)
                return set()
            return self._turn_keys(want)
        tx, tz = self.autowalk[0]
        last = len(self.autowalk) == 1
        if math.hypot(tx - v.x, tz - v.z) < (70 if last else 180):
            self.autowalk.pop(0)
            if not self.autowalk:
                self.autowalk = ["face"]
            return self.steer()
        want = dir_towards(v.x, v.z, tx, tz)
        diff = angle_diff(v.dir, want)
        keys = set()
        if abs(diff) > 64:
            keys.add("right" if diff > 0 else "left")
        if abs(diff) < 160:      # turn on the spot first so corners are not cut into walls
            keys.add("up")
        pos = (round(v.x), round(v.z))
        if "up" in keys and pos == getattr(self, "_aw_prev", None):
            if v.stairs_ahead() and self.autowalk_stuck >= 3:
                self.say("Taking the stairs.")
                self.autowalk_stuck = 0
                return {"action"}
            climb = v.climbable()
            if climb and self.autowalk_stuck >= 3:
                self.say("Climbing." if climb == "climb up" else "Dropping down.")
                self.autowalk_stuck = 0
                self._aw_floor = None          # force a re-plan from the new level
                return {"action"}
            self.autowalk_stuck += 1
            if self.autowalk_stuck > 45:
                self.autowalk = None
                self.say("Auto-walk blocked.", interrupt=True)
                return set()
        else:
            self.autowalk_stuck = 0
        self._aw_prev = pos
        return keys

    def _turn_keys(self, want):
        diff = angle_diff(self.v.dir, want)
        return {"right" if diff > 0 else "left"}
