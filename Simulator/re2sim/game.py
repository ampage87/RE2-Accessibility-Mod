"""The RE2 simulator core: game state, room loading, SCD script VM, player physics,
AOT (trigger) handling, a simplified enemy/combat model and an event bus.

Fidelity: room geometry, triggers, doors, items, messages, flags and script logic are
driven by the real game files.  Animation, rendering, sound and exact enemy AI are
approximated; every place the simulator has to guess is reported through
`Game.gaps` so mod testing can tell a real issue apart from a simulator limitation.
"""
from __future__ import annotations

import copy
import struct
import math
import random
from dataclasses import dataclass, field

from . import scd
from .rdt import RoomLibrary, Collision, decode_message
from .scd import OP, FG, decode, item_name, enemy_name, SCE_NAMES
from .world import (facing_vector, blocking_at, point_in_quad, quad_centre, rect_quad,
                    dir_towards, angle_diff, DIR_FULL, PLAYER_RADIUS, push_out)

FPS = 30
FLOOR_UNIT = -1800

WALK_SPEED = 70        # units per frame (~2.1 m/s)
RUN_SPEED = 140        # (~4.2 m/s)
BACK_SPEED = 40
TURN_SPEED = 64        # dir units per frame (~170 degrees per second)

V_USED_ITEM, V_STAGE, V_ROOM, V_CUT, V_LAST_RDT, V_RNG = 2, 24, 25, 26, 27, 29

MEM_X, MEM_Y, MEM_Z, MEM_XDIR, MEM_YDIR, MEM_ZDIR, MEM_FLOOR = 11, 12, 13, 14, 15, 16, 17

ENEMY_HP = {  # rough Normal-difficulty values; good enough to exercise combat cues
    "Zombie": 90, "Zombie Dog": 60, "Crow": 10, "Licker": 250, "Alligator": 1200, "Spider": 220,
    "Baby Spider": 5, "G Embryo": 50, "G Adult": 600, "Cockroach": 5, "Tyrant": 1500, "Zombie Arms": 60,
    "Ivy": 300, "Vines": 50, "Birkin": 1200, "Giant Moth": 300, "Maggots": 10,
}
ENEMY_SPEED = {"Zombie": 14, "Zombie Dog": 90, "Crow": 80, "Licker": 60, "Spider": 30, "G Adult": 30,
               "Tyrant": 40, "Ivy": 8, "Birkin": 50, "Alligator": 30}
WEAPON_DAMAGE = {1: 6, 2: 18, 3: 18, 4: 22, 5: 120, 6: 140, 7: 80, 8: 100, 9: 120, 10: 100, 11: 110,
                 12: 60, 13: 30, 14: 40, 15: 12, 16: 15, 17: 400, 18: 30, 19: 18}
WEAPON_RANGE = {1: 900}
DEFAULT_RANGE = 15000
AMMO_ITEMS = set(range(20, 30))
STACK_MAX = {20: 255, 21: 255, 22: 255, 23: 255, 24: 255, 25: 255, 26: 255, 27: 255, 28: 255, 29: 255,
             30: 9, 35: 1}
HEALING = {38: 30, 39: 0, 40: 0, 41: 80, 42: 200, 43: 30, 44: 200, 45: 80, 46: 200, 35: 200}


def enemy_family(type_id: int) -> str:
    n = enemy_name(type_id)
    for fam in sorted(ENEMY_HP, key=len, reverse=True):
        if n.startswith(fam):
            return fam
    return n


def is_hostile(type_id: int) -> bool:
    return 0x10 <= type_id < 0x40


@dataclass
class Aot:
    id: int
    sce: int
    sat: int
    floor: int
    quad: list
    data: tuple = ()
    door: dict = None
    item: dict = None
    source: str = ""
    sup: int = 0          # 0x80: the area follows room object (sup & 0x3F) (0x40: an enemy); bio2.exe 0x4E9227
    base: list = None     # the area relative to that object

    @property
    def kind(self) -> str:
        return SCE_NAMES[self.sce] if self.sce < len(SCE_NAMES) else f"sce{self.sce}"

    @property
    def centre(self):
        return quad_centre(self.quad)

    def contains(self, x, z) -> bool:
        return point_in_quad(x, z, self.quad)

    @property
    def manual(self) -> bool:
        return bool(self.sat & scd.SAT_MANUAL)

    @property
    def player_triggers(self) -> bool:
        return bool(self.sat & scd.SAT_PL)


@dataclass
class Enemy:
    id: int
    type: int
    x: float
    y: float
    z: float
    d: int
    floor: int
    ai: int
    state: int
    killid: int
    hp: int
    max_hp: int
    active: bool = True
    alive: bool = True
    aware: bool = False
    attack_cd: int = 0
    members: dict = field(default_factory=dict)
    boss: object = None

    @property
    def name(self) -> str:
        return enemy_name(self.type)

    @property
    def family(self) -> str:
        return enemy_family(self.type)

    @property
    def hostile(self) -> bool:
        return is_hostile(self.type)


@dataclass
class Player:
    x: float = 0.0
    y: float = 0.0
    z: float = 0.0
    d: int = 0
    hp: int = 200
    max_hp: int = 200
    poisoned: bool = False
    character: int = 0       # 0 Leon, 1 Claire
    members: dict = field(default_factory=dict)
    stair: int = -1           # the stairway being climbed (collision index), -1 none

    @property
    def floor(self) -> int:
        return max(0, int(round(self.y / FLOOR_UNIT)))

    @property
    def condition(self) -> str:
        if self.hp <= 0:
            return "dead"
        if self.hp > 120:
            return "Fine"
        if self.hp > 60:
            return "Caution"
        return "Danger"


@dataclass
class Frame:
    proc: int
    pc: int
    blocks: list = field(default_factory=list)   # control-flow block stack for this procedure


@dataclass
class Thread:
    tid: int
    procs: dict          # proc index -> {pc: Instr}
    frames: list
    wait: int = 0
    waiting_message: bool = False
    done: bool = False
    spin: int = 0
    origin: str = ""
    work_kind: int = 1
    work_id: int = 0
    last_pc: tuple = None


@dataclass
class Message:
    text: str
    yesno: bool
    source: str
    thread: Thread = None
    on_answer: object = None
    msg_id: int = -1          # room message number (its bytes live in Game.msg_chunk), -1 = system text
    pages: int = 1            # pages (0xFD ends a page); action goes to the next one
    page: int = 0
    page_frames: int = 0      # frames since the page started (it "types" for a few frames)


@dataclass
class SceneObject:
    """A room object (statue, crate) set by obj_model_set: OBJECT_WORK at 0x98A61C + 0x1F8 * idx."""
    idx: int
    x: float
    y: float
    z: float
    d: int = 0
    floor: int = 0
    kind: int = 0
    aot: int = 255            # AOT the object stands in this frame (script member 9)
    members: dict = field(default_factory=dict)
    be: int = 0               # obj_model_set be_flg (0x80 pushable, 0x100 climbable): OBJECT_WORK +0
    ofs: tuple = (0, 0, 0)    # collision offset x, y, z (+0x94, +0x98, +0x96)
    size: tuple = (0, 0, 0)   # collision size w, h, d (+0x9A, +0x9E, +0x9C)

    def box(self):
        """Collision box as the game builds it (bio2.exe 0x4CD610): centre = position + offset,
        half sizes w/d, x and z swapped when the direction has bit 0x400. None if it has none."""
        w, h, d = self.size
        if w <= 0 or d <= 0 or h <= 0 or (self.be & 2):
            return None
        ox, oy, oz = self.ofs
        swap = (int(self.d) & 0x400) != 0
        cx = self.x + (oz if swap else ox)
        cz = self.z + (ox if swap else oz)
        hx, hz = (d, w) if swap else (w, d)
        cy = self.y + oy
        return cx, cz, hx, hz, cy - h, cy + h


IDLE_OPS = {"evt_next", "nop", "ck", "cmp", "while", "ewhile", "do", "edwhile", "if", "endif", "else", "goto",
            "member_cmp", "wsleep", "wsleeping", "sleeping", "keep_item_ck", "sce_key_ck", "sce_trg_ck", "work_copy"}


FILE_FIRST, FILE_LAST = 104, 128


class Game:
    """One play session.  Call step()/hold() with controls; read events from `events`."""

    def __init__(self, gamedata: str, character: int = 0, scenario: int = 0, seed: int = 1):
        self.lib = RoomLibrary(gamedata)
        self.rng = random.Random(seed)
        self.frame = 0
        self.player = Player(character=character)
        self.flags = {}
        self.vars = [0] * 64
        self.inventory = []      # list of [item_id, qty]
        self.inventory_size = 8
        self._member_waits = {}
        self.file_flags = 0      # files collected (bit 0x80000000 >> (item - 104))
        self.item_box = []
        self.equipped = None
        self.room = None
        self.cut = 0
        self.aots = {}
        self.enemies = {}
        self.threads = []
        self.next_tid = 1
        self.message = None
        self.events = []          # (frame, type, dict)
        self.gaps = []            # simulator fidelity notes encountered during play
        self._gap_keys = set()
        self.listeners = []
        self.controls = set()
        self.aiming = False
        self.dead = False
        self.in_aots = set()
        self.saves = {}
        self.cutscene_lock = False
        self._main_procs = {}
        self.movers = []
        self.collisions = []
        self.collision_version = 0
        self.partner = Player()          # the sub-player (Ada/Sherry when controlled) - position only
        self.script_progress_frame = 0
        self.locked_frames = 0
        self.watchdog_seconds = 10
        self.set_flag(FG["FG_STATUS"], 0, character)
        self.set_flag(FG["FG_STATUS"], 1, scenario)
        self.scenario = scenario
        if character == 0:
            self.inventory = [[1, 1], [2, 18]]
        else:
            self.inventory = [[1, 1], [3, 13], [48, 1]]
        self.equipped = self.inventory[1][0]

    # ------------------------------------------------------------------ events
    def emit(self, _kind: str, **data):
        ev = (self.frame, _kind, data)
        self.events.append(ev)
        for fn in self.listeners:
            fn(_kind, data)

    def gap(self, key: str, detail: str):
        if key in self._gap_keys:
            return
        self._gap_keys.add(key)
        self.gaps.append(detail)
        self.emit("sim_gap", detail=detail)

    # ------------------------------------------------------------------ flags / vars
    def get_flag(self, group: int, idx: int) -> int:
        return 1 if idx in self.flags.get(group, set()) else 0

    def set_flag(self, group: int, idx: int, val: int):
        s = self.flags.setdefault(group, set())
        if val == 0:
            s.discard(idx)
        elif val == 1:
            s.add(idx)
        else:
            s.symmetric_difference_update({idx})

    def getv(self, i):
        if i == V_RNG:
            self.vars[V_RNG] = self.rng.randint(0, 0x7FFF)
        return self.vars[i] if 0 <= i < len(self.vars) else 0

    def setv(self, i, v):
        if 0 <= i < len(self.vars):
            v = ((int(v) + 0x8000) & 0xFFFF) - 0x8000
            self.vars[i] = v

    # ------------------------------------------------------------------ room loading
    def enter_room(self, stage: int, room: int, x=None, y=None, z=None, d=None, cut=0, via=None):
        """stage is 1-based (file digit)."""
        prev = self.room
        if prev is not None:
            self.setv(V_LAST_RDT, prev.stage * 0x100 + prev.room)   # 539 = 0x21B: the file digit (21B checks it)
        self.room = self.lib.get(self.player.character, stage, room)
        # live copy: scripts toggle collision boxes with sca_id_set
        self.collisions = [copy.copy(c) for c in self.room.collisions]
        self.collision_version += 1
        # the map's "been here" bit (bio2.exe 0x4E4040: per-stage base + room)
        if 1 <= stage <= 7:
            self.set_flag(FG["FG_MAP"], (0, 30, 58, 72, 89, 99, 123)[stage - 1] + room, 1)
        self.flags[FG["FG_ROOM"]] = set()
        # [sim note] the player's stop flag does not carry into the next room: 410's scene with
        # Ada ends with set(2, 7, 1) and then opens the ladder door it sends the player through
        self.set_flag(FG["FG_STOP"], 7, 0)
        self.flags[FG["FG_ROOM_ENEMY"]] = set()
        self.flags[FG["FG_USE"]] = set()
        self.aots = {}
        self.enemies = {}
        self.objects = {}
        self.msg_chunk = bytearray(self.room.chunk("msg_en"))   # live copy: the mod may patch text
        self.threads = []
        self.message = None
        self.in_aots = set()
        self.movers = []
        self.cut = cut
        self.setv(V_STAGE, stage - 1)
        self.setv(V_ROOM, room)
        self.setv(V_CUT, cut)
        if x is not None:
            self.player.x, self.player.y, self.player.z = x, y or 0, z
        if d is not None:
            self.player.d = d % DIR_FULL
        self.emit("room_load", stage=stage, room=room, rid=self.room.rid, via=via)
        # init script runs to completion before the first frame
        self._main_procs = {i: {ins.pc: ins for ins in decode(pr)} for i, pr in enumerate(self.room.scd_main)}
        if self.room.scd_init:
            t = self._new_thread(self.room.scd_init, 0, "init")
            self._run_thread(t, init=True)
            self.threads = [th for th in self.threads if th is not t]
        if x is None:
            self._default_spawn()
        if self.room.scd_main:
            t = Thread(self.next_tid, self._main_procs, [Frame(0, 0)], origin="main:0")
            self.next_tid += 1
            self.threads.append(t)
        self.emit("room_enter", stage=stage, room=room, rid=self.room.rid)

    def _default_spawn(self):
        """New-game start: the exe sets this.  Pick a free spot near camera 0's target that is
        not inside a damage zone and not next to an enemy, facing the nearest door."""
        cam = self.room.cameras[0] if self.room.cameras else None
        tx, tz = (cam.to[0], cam.to[2]) if cam else (0, 0)
        bad = [a for a in self.aots.values() if a.sce in (11,)]

        def ok(x, z):
            if blocking_at(self.collisions, 0, x, z, radius=PLAYER_RADIUS + 200):
                return False
            if any(a.contains(x, z) for a in bad):
                return False
            for e in self.enemies.values():
                if e.hostile and math.hypot(e.x - x, e.z - z) < 4000:
                    return False
            return True
        best = None
        for r in range(0, 20000, 300):
            for k in range(24):
                ang = k * math.pi / 12
                x, z = tx + r * math.cos(ang), tz + r * math.sin(ang)
                if ok(x, z):
                    best = (x, z)
                    break
            if best:
                break
        self.player.x, self.player.z = best if best else (tx, tz)
        self.player.y = 0
        doors = [a for a in self.aots.values() if a.door]
        if doors:
            dx, dz = min((a.centre for a in doors), key=lambda c: math.hypot(c[0] - self.player.x, c[1] - self.player.z))
            self.player.d = dir_towards(self.player.x, self.player.z, dx, dz)
        self.gap("spawn", "New-game start position is chosen by the simulator (the real one is set by the game executable).")

    # ------------------------------------------------------------------ script VM
    def _new_thread(self, proclist, proc, origin):
        # init procedure 0 is the init script; other procedure numbers resolve to the main list
        procs = dict(self._main_procs)
        for i, p in enumerate(proclist):
            procs[i] = {ins.pc: ins for ins in decode(p)}
        t = Thread(self.next_tid, procs, [Frame(proc, 0)], origin=f"{origin}:{proc}")
        self.next_tid += 1
        self.threads.append(t)
        return t

    def _run_thread(self, t: Thread, init=False, budget=20000):
        steps = 0
        while not t.done and steps < budget:
            if t.waiting_message:
                return
            if t.wait > 0:
                if init:
                    t.wait = 0
                else:
                    t.wait -= 1
                    return
            fr = t.frames[-1]
            code = t.procs.get(fr.proc, {})
            ins = code.get(fr.pc)
            if ins is None:
                self._return(t)
                continue
            steps += 1
            if ins.name not in IDLE_OPS:
                self.script_progress_frame = self.frame
            yielded = self._exec(t, fr, ins, init)
            if yielded and not init:
                return
        if steps >= budget and not t.done:
            self.gap(f"budget:{t.origin}", f"Script {t.origin} in room {self.room.rid} ran {budget} instructions without yielding; "
                                           f"it was paused (likely waiting on something the simulator does not model).")

    def _return(self, t: Thread):
        t.frames.pop()
        if not t.frames:
            t.done = True

    def _cond_fail(self, t: Thread, fr: Frame):
        """A condition failed: leave the innermost conditional block."""
        while fr.blocks:
            b = fr.blocks.pop()
            if b[0] in ("if", "while"):
                fr.pc = b[1]
                return
            if b[0] == "edwhile":
                # do-while condition false: exit loop; pop the 'do' too
                fr.pc = b[2]
                if fr.blocks and fr.blocks[-1][0] == "do":
                    fr.blocks.pop()
                return
        # condition outside any block: treat as no-op
        fr.pc += 0

    def _exec(self, t: Thread, fr: Frame, ins, init: bool) -> bool:
        """Execute one instruction. Returns True if the thread yields this frame."""
        n = ins.name
        a = ins.args
        nxt = ins.pc + ins.size
        fr.pc = nxt
        cond_ops = ("ck", "cmp", "keep_item_ck", "member_cmp", "sce_key_ck", "sce_trg_ck", "work_copy", "dir_ck",
                    "poison_ck", "sce_item_ck_lost")

        # loop back for do/edwhile when conditions finished successfully
        def check_edwhile_done():
            if fr.blocks and fr.blocks[-1][0] == "edwhile":
                code = t.procs[fr.proc]
                nins = code.get(fr.pc)
                if nins is None or nins.name not in cond_ops:
                    b = fr.blocks.pop()
                    fr.pc = b[1]          # loop start
                    return True
            return False

        if n in ("nop", "nop_20", "nop_1E", "nop_1F", "break_point", "nop_8a", "nop_8b", "nop_8c"):
            return False
        if n == "evt_end":
            if len(t.frames) > 1:
                t.frames.pop()
            else:
                t.done = True
            return False
        if n == "evt_next":
            return True
        if n == "evt_exec":
            if len(a) >= 3 and a[1] == OP["gosub"]:
                # don't stack a second copy of a procedure that is still running (main 1 is
                # re-run on camera changes and would otherwise start its helpers again)
                if any(not th.done and th is not t and th.frames and th.frames[0].proc == a[2]
                       for th in self.threads):
                    return False
                nt = self._new_thread_from(t, a[2])
                if init:
                    self._run_thread(nt, init=True)
            return False
        if n == "evt_chain":
            if len(a) >= 3:
                t.frames = [Frame(a[2], 0)]
            return False
        if n == "evt_kill":
            tid = ins.raw[1]
            for th in self.threads:
                if th is not t and th.origin.endswith(f":{tid}"):
                    th.done = True
            return False
        if n == "gosub":
            t.frames.append(Frame(a[0], 0))
            return False
        if n == "return":
            self._return(t)
            return False
        if n == "if":
            fr.blocks.append(("if", nxt + a[1]))
            return False
        if n == "else":
            if fr.blocks and fr.blocks[-1][0] == "if":
                fr.blocks.pop()
            fr.pc = ins.pc + a[1]
            return False
        if n == "endif":
            if fr.blocks and fr.blocks[-1][0] == "if":
                fr.blocks.pop()
            return False
        if n == "while":
            fr.blocks.append(("while", nxt + a[1], ins.pc))
            return False
        if n == "ewhile":
            # jump back to re-evaluate the while header
            for i in range(len(fr.blocks) - 1, -1, -1):
                if fr.blocks[i][0] == "while":
                    start = fr.blocks[i][2]
                    del fr.blocks[i:]
                    fr.pc = start
                    break
            return self._spin_guard(t, ins)
        if n == "do":
            fr.blocks.append(("do", nxt, nxt + a[1]))
            return False
        if n == "edwhile":
            for i in range(len(fr.blocks) - 1, -1, -1):
                if fr.blocks[i][0] == "do":
                    fr.blocks.append(("edwhile", fr.blocks[i][1], fr.blocks[i][2]))
                    break
            if check_edwhile_done():
                return self._spin_guard(t, ins)
            return False
        if n == "for":
            count = a[2]
            if count == 0:
                fr.pc = nxt + a[1]
            else:
                fr.blocks.append(("for", nxt, nxt + a[1], count))
            return False
        if n == "next":
            for i in range(len(fr.blocks) - 1, -1, -1):
                if fr.blocks[i][0] == "for":
                    kind, start, end, count = fr.blocks[i]
                    del fr.blocks[i + 1:]
                    count -= 1
                    if count > 0:
                        fr.blocks[i] = (kind, start, end, count)
                        fr.pc = start
                    else:
                        fr.blocks.pop()
                    break
            return False
        if n == "switch":
            val = self.getv(a[0])
            end = nxt + a[1]
            code = t.procs[fr.proc]
            pc = nxt
            target = None
            guard = 0
            while pc < end and guard < 256:
                guard += 1
                ci = code.get(pc)
                if ci is None:
                    break
                if ci.name == "case":
                    if ci.args[2] == (val & 0xFFFF):
                        target = pc + ci.size
                        break
                    pc = pc + ci.size + ci.args[1]
                elif ci.name == "default":
                    target = pc + ci.size
                    break
                elif ci.name == "eswitch":
                    break
                else:
                    break
            fr.blocks.append(("switch", end))
            fr.pc = target if target is not None else end
            return False
        if n in ("case", "default"):
            return False
        if n == "eswitch":
            if fr.blocks and fr.blocks[-1][0] == "switch":
                fr.blocks.pop()
            return False
        if n == "break":
            while fr.blocks:
                b = fr.blocks.pop()
                if b[0] == "switch":
                    fr.pc = b[1]
                    code = t.procs[fr.proc]
                    if code.get(fr.pc) and code[fr.pc].name == "eswitch":
                        fr.pc += code[fr.pc].size
                    break
                if b[0] in ("for", "do", "while"):
                    fr.pc = b[2] if b[0] in ("for", "do") else b[1]
                    break
            return False
        if n == "goto":
            fr.pc = ins.pc + a[3]
            # a[0]=if-depth, a[1]=loop-depth at target; trim block stack accordingly
            ifs = [b for b in fr.blocks if b[0] == "if"]
            keep_if = a[0] + 1 if a[0] != 0xFF else 0
            if len(ifs) > keep_if:
                trimmed, seen = [], 0
                for b in fr.blocks:
                    if b[0] == "if":
                        seen += 1
                        if seen > keep_if:
                            continue
                    trimmed.append(b)
                fr.blocks = trimmed
            return self._spin_guard(t, ins)
        if n == "sleep":
            t.wait = max(0, a[1] - 1)
            return True
        if n in ("sleeping", "wsleep", "wsleeping"):
            return True

        # ---- conditions
        if n in cond_ops:
            ok = self._condition(t, n, ins)
            if not ok:
                self._cond_fail(t, fr)
                return False
            if check_edwhile_done():
                return self._spin_guard(t, ins)
            return False

        # ---- flags and variables
        if n == "set":
            self.set_flag(a[0], a[1], a[2])
            return False
        if n == "save":
            self.setv(a[0], a[1])
            return False
        if n == "copy":
            self.setv(a[0], self.getv(a[1]))
            return False
        if n == "calc":
            self.setv(a[2], self._calc(a[1], self.getv(a[2]), a[3]))
            return False
        if n == "calc2":
            self.setv(a[1], self._calc(a[0], self.getv(a[1]), self.getv(a[2])))
            return False
        if n == "sce_rnd":
            self.getv(V_RNG)
            return False

        # ---- camera
        if n == "cut_chg":
            self._set_cut(ins.raw[1], "script")
            return False
        if n in ("cut_old", "cut_auto", "cut_replace", "cut_be_set"):
            return False

        # ---- messages
        if n == "message_on":
            msg_id = a[1]
            text = self.room.messages[msg_id] if msg_id < len(self.room.messages) else f"<message {msg_id}>"
            if init:
                self.emit("message", text=text, yesno=False, source="init")
                return False
            text = self.room_message_text(msg_id) or text
            self.show_message(text, "[Yes/No]" in text, source=f"room {self.room.rid} msg {msg_id}", thread=t,
                              msg_id=msg_id)
            return True

        # ---- AOTs
        if n == "aot_set":
            idd, sce, sat, flr, sup, x, z, w, d = a[:9]
            q = rect_quad(x, z, w, d)
            self.aots[idd] = Aot(idd, sce, sat, flr, q, tuple(a[9:15]), source=t.origin, sup=sup,
                                 base=list(q) if sup & 0x80 else None)
            self._update_attached_aots()
            return False
        if n == "aot_set_4p":
            idd, sce, sat, flr, sup = a[:5]
            pts = a[5:13]
            quad = [(pts[0], pts[1]), (pts[2], pts[3]), (pts[4], pts[5]), (pts[6], pts[7])]
            self.aots[idd] = Aot(idd, sce, sat, flr, quad, tuple(a[13:19]), source=t.origin, sup=sup,
                                 base=list(quad) if sup & 0x80 else None)
            self._update_attached_aots()
            return False
        if n in ("door_aot_se", "door_aot_set_4p"):
            self._door_aot(ins)
            return False
        if n in ("item_aot_set", "item_aot_set_4p"):
            self._item_aot(ins)
            return False
        if n == "aot_reset":
            idd, sce, sat = a[0], a[1], a[2]
            old = self.aots.get(idd)
            if old:
                old.sce, old.sat, old.data = sce, sat, tuple(a[3:9])
                raw6 = bytes(int(v) & 0xFF for v in a[3:9])
                if sce == 1 and old.door:
                    # the six data bytes land on the start of the door record: the arrival point
                    # (21B: the painting's passage leads to x 24730, z -15507 of the same room)
                    nx, ny, nz = struct.unpack("<hhh", raw6)
                    old.door = dict(old.door, next_x=nx, next_y=ny, next_z=nz)
                if sce == 2 and old.item:
                    # an item spot's data: item, amount, pick-up flag (111: the Knight plug for Leon)
                    it, am, gid = struct.unpack("<HHH", raw6)
                    old.item = dict(old.item, item=it, amount=am, gid=gid)
            else:
                self.aots[idd] = Aot(idd, sce, sat, 0, [(0, 0)] * 4, tuple(a[3:9]), source=t.origin)
            return False
        if n == "aot_on":
            aot = self.aots.get(a[0])
            if aot:
                self.activate_aot(aot, scripted=True)
            return False

        # ---- work targets / positions
        if n == "work_set":
            t.work_kind, t.work_id = a[0], a[1]
            return False
        if n == "pos_set":
            ent = self._work(t)
            if ent is not None:
                self._set_pos(ent, a[1], a[2], a[3])
            return False
        if n == "dir_set":
            ent = self._work(t)
            if ent is not None:
                self._set_dir(ent, a[2])
            return False
        if n == "member_set":
            self._member_set(t, a[0], a[1])
            return False
        if n == "member_set2":
            self._member_set(t, a[0], self.getv(a[1]))
            return False
        if n == "member_copy":
            self.setv(a[0], self._member_get(t, a[1]))
            return False
        if n == "member_calc":
            cur = self._member_get(t, a[1])
            self._member_set(t, a[1], self._calc(a[0], cur, a[2]))
            return False
        if n == "member_calc2":
            cur = self._member_get(t, a[1])
            self._member_set(t, a[1], self._calc(a[0], cur, self.getv(a[2])))
            return False
        if n == "plc_dest":
            # scripted walk: entity walks to (x, z); FG_ROOM flag a[2] is set on arrival
            # a[1] is the motion: 21 = turn to direction a[3]; 20 = stop; others walk/run to (a[3], a[4])
            ent = self._work(t)
            mode = a[1]
            if ent is None or mode == 20 or (a[3] == 0 and a[4] == 0 and mode != 21):
                self.set_flag(FG["FG_ROOM"], a[2], 1)
            elif mode == 21:
                ent.d = a[3] % DIR_FULL
                self.set_flag(FG["FG_ROOM"], a[2], 1)
            else:
                self.set_flag(FG["FG_ROOM"], a[2], 0)
                self.movers = [m for m in self.movers if m[0] is not ent]
                self.movers.append((ent, a[3], a[4], a[2]))
            return False
        if n in ("speed_set", "add_speed") and isinstance(self._work(t), SceneObject):
            # objects moved by script (the statue lowering the red jewel in room 10C):
            # speed slot 7 is the vertical speed (approximation)
            ob = self._work(t)
            if n == "speed_set":
                ob.members[("speed", ins.raw[1])] = struct.unpack_from("<h", ins.raw, 2)[0]
            else:
                ob.y += ob.members.get(("speed", 7), 0)
                # slots 0 and 2 move it across the floor (the library's sliding shelves: slot 2,
                # 900 units a place)
                ob.x += ob.members.get(("speed", 0), 0)
                ob.z += ob.members.get(("speed", 2), 0)
            return False
        if n in ("speed_set", "add_speed", "add_aspeed") and self._work(t) is self.player:
            # the player moved by script (falling through the library floor): slot 1 is the
            # vertical speed, slot 7 its change per frame (approximation)
            pl = self.player
            sp = pl.__dict__.setdefault("_speed", {})
            if n == "speed_set":
                sp[ins.raw[1]] = struct.unpack_from("<h", ins.raw, 2)[0]
            elif n == "add_speed":
                pl.y += sp.get(1, 0)
            else:
                sp[1] = sp.get(1, 0) + sp.get(7, 0)
            return False
        if n in ("plc_motion", "plc_neck", "plc_ret", "plc_flg", "plc_rot", "plc_gun", "plc_gun_eff",
                 "plc_stop", "plc_cnt", "splc_ret", "splc_sce", "speed_set", "add_speed", "add_aspeed",
                 "flr_set", "super_set", "super_reset", "super_on", "parts_set", "kage_set",
                 "mirror_set", "rbj_reset", "col_chg_set"):
            return False

        if n == "sca_id_set":
            idx = ins.raw[1]
            val = ins.raw[2] | (ins.raw[3] << 8)
            if idx < len(self.collisions):
                c = self.collisions[idx]
                before = bool(c.raw_flag & 0x8000)
                c.raw_flag = (val & 0xFF80) | (c.raw_flag & 0x007F)   # bio2.exe 0x4E6DF0
                self.collision_version += 1
                after = bool(c.raw_flag & 0x8000)
                if before != after:
                    self.emit("collision_change", index=idx, blocking=after)
            return False

        # ---- enemies
        if n == "sce_em_set":
            self._spawn_enemy(a)
            return False

        # ---- items / player status
        if n == "sce_item_lost":
            self.remove_item(a[0], all_=True)
            return False
        if n == "sce_item_get":
            self.add_item(ins.raw[1], ins.raw[2] if len(ins.raw) > 2 else 1, source="script")
            return False
        if n == "heal":
            self.player.hp = self.player.max_hp
            return False
        if n == "poison_clr":
            self.player.poisoned = False
            return False

        # ---- audio/visual cues worth surfacing to the mod
        if n == "se_on":
            self.emit("sound", id=a[1] if len(a) > 1 else 0, pos=(a[3], a[4], a[5]) if len(a) > 5 else None)
            return False
        if n == "xa_on":
            self.emit("voice", id=a[1] if len(a) > 1 else 0)
            # [sim note] status flag 1.11 taken as "a voice line is playing": scenes wait for it
            # (409's scene with Ada: one thread waits while 1.11 is 0, the other plays the line).
            # Lines are not timed here: 3 seconds each
            self.set_flag(1, 11, 1)
            self.xa_frames = 90
            return False
        if n == "movie_on":
            self.emit("movie", id=ins.raw[1])
            return False
        if n in ("sce_bgm_control", "sce_bgmtbl_set", "xa_vol", "se_vol"):
            return False
        if n == "obj_model_set":
            raw = ins.raw
            idx = raw[1]
            if idx in self.objects:          # bio2.exe 0x4E56B0: setting a live object removes it
                del self.objects[idx]
            else:
                sv = lambda o: struct.unpack_from("<h", raw, o)[0]
                self.objects[idx] = SceneObject(idx, sv(0x0E), sv(0x10), sv(0x12), sv(0x16) % DIR_FULL, raw[6],
                                                struct.unpack_from("<H", raw, 0x08)[0],
                                                be=struct.unpack_from("<H", raw, 0x0A)[0],
                                                ofs=(sv(0x1A), sv(0x1C), sv(0x1E)),
                                                size=(struct.unpack_from("<H", raw, 0x20)[0], struct.unpack_from("<H", raw, 0x22)[0],
                                                      struct.unpack_from("<H", raw, 0x24)[0]))
                self._objects_changed = True
            return False
        if n in ("obj_model_set", "sce_espr_on", "sce_espr_on2", "sce_espr3d_on", "sce_espr3d_on2", "sce_espr_kill",
                 "sce_espr_kill2", "sce_espr_control", "light_pos_set", "light_kido_set", "light_color_set",
                 "light_pos_set2", "light_kido_set2", "light_color_set2", "sce_fade_set", "sce_fade_adjust",
                 "sce_shake_on", "mizu_div_set", "sce_scr_move", "sce_line_start", "sce_line_main", "sce_line_end",
                 "sce_parts_bomb", "sce_parts_down", "weapon_chg", "heal_partner", "dir_ck"):
            return False
        self.gap(f"op:{n}", f"Opcode {n} is not modelled (first seen in room {self.room.rid}).")
        return False

    def _spin_guard(self, t: Thread, ins) -> bool:
        """Back-edges (loops) yield one frame so polling loops behave like the game."""
        return True

    def _new_thread_from(self, parent: Thread, proc: int) -> Thread:
        t = Thread(self.next_tid, self._main_procs, [Frame(proc, 0)], origin=f"main:{proc}")
        self.next_tid += 1
        self.threads.append(t)
        return t

    def _calc(self, op, a, b):
        try:
            return [lambda: a + b, lambda: a - b, lambda: a * b, lambda: int(a / b) if b else 0,
                    lambda: a % b if b else 0, lambda: a | b, lambda: a & b, lambda: a ^ b, lambda: ~a,
                    lambda: a << b, lambda: (a & 0xFFFF) >> b, lambda: a >> b][op]()
        except IndexError:
            return a

    def _condition(self, t, n, ins) -> bool:
        a = ins.args
        if n == "ck":
            return self.get_flag(a[0], a[1]) == a[2]
        if n == "cmp":
            v, c, val = self.getv(a[1]), a[2], a[3]
            if a[1] == 1:
                # variable 1 is kept by the engine (a camera cut the game picks while a scripted
                # character walks, 30A's Sherry cutscene): never set here, so a script waiting on
                # it would wait forever. After a second of asking, say the wait is over.
                key = (id(t), ins.pc)
                n_ = self._member_waits.get(key, 0) + 1
                self._member_waits[key] = n_
                if n_ > 30:
                    return False
            return [v == val, v > val, v >= val, v < val, v <= val, v != val][c] if c < 6 else False
        if n == "keep_item_ck":
            return self.has_item(a[0])
        if n == "member_cmp":
            raw = ins.raw
            m, c = raw[2], raw[3]
            val = int.from_bytes(raw[4:6], "little", signed=True)
            v = self._member_get(t, m)
            res = [v == val, v > val, v >= val, v < val, v <= val, v != val][c] if c < 6 else False
            ent_ = self._work(t)
            explicit = ent_ is not None and m in getattr(ent_, "members", {})   # set by a boss model or a script
            if m not in (MEM_X, MEM_Y, MEM_Z, MEM_YDIR, MEM_FLOOR, 7, 8, 9) and not explicit:
                # members the game's animation code changes (e.g. 26: an animation still playing):
                # the sim never changes them, so a script waiting on one would wait forever.
                # After a second of asking, the wait is over: the answer flips (a "while" loop
                # waits on True, 509's "if ... goto out" on False)
                key = (id(t), ins.pc)
                n_ = self._member_waits.get(key, 0) + 1
                self._member_waits[key] = n_
                if n_ > 30:
                    return not res
            return res
        if n == "sce_key_ck":
            return False     # "is a button held?" – the sim does not hold buttons during cutscenes
        if n == "sce_trg_ck":
            return False
        if n == "work_copy":
            # bio2.exe 0x4E4B80: copy variable raw[1] into the script itself, raw[2] bytes into the
            # next instruction (a byte, or a word when raw[3] is set) - how scripts index flags
            # and objects by a variable ("set(5, <var 4>, 1)" in the library shelves)
            raw = ins.raw
            if len(raw) >= 4:
                fr = t.frames[-1]
                code = t.procs.get(fr.proc, {})
                nxt_ins = code.get(ins.pc + ins.size)
                if nxt_ins is not None:
                    v = self.getv(raw[1])
                    b = bytearray(nxt_ins.raw)
                    off = raw[2]
                    if raw[3]:
                        if off + 2 <= len(b):
                            struct.pack_into("<h", b, off, ((int(v) + 0x8000) & 0xFFFF) - 0x8000)
                    elif off < len(b):
                        b[off] = int(v) & 0xFF
                    new = scd.Instr(nxt_ins.pc, nxt_ins.op, nxt_ins.size, scd.decode_args(nxt_ins.op, bytes(b)), bytes(b))
                    code[nxt_ins.pc] = new
            return True
        if n == "poison_ck":
            return self.player.poisoned
        if n == "sce_item_ck_lost":
            return not self.has_item(a[0])
        if n == "dir_ck":
            return True
        return False

    def _work(self, t):
        if t.work_kind == 1:
            return self.player
        if t.work_kind == 2:
            return self.partner
        if t.work_kind == 3:
            return self.enemies.get(t.work_id)
        if t.work_kind == 4:
            return self.objects.get(t.work_id)
        return None

    def _set_pos(self, ent, x, y, z):
        ent.x, ent.y, ent.z = x, y, z
        if ent is self.player:
            self.emit("player_moved_by_script", x=x, z=z)

    def _set_dir(self, ent, d):
        ent.d = d % DIR_FULL

    def _member_get(self, t, m):
        ent = self._work(t)
        if ent is None:
            return 0
        if m == MEM_X:
            return int(ent.x)
        if m == MEM_Y:
            return int(ent.y)
        if m == MEM_Z:
            return int(ent.z)
        if m == MEM_YDIR:
            return int(ent.d)
        if m == MEM_FLOOR:
            return ent.floor
        if m == 9 and isinstance(ent, SceneObject):
            return ent.aot
        if m in (8, 9) and ent is self.player:
            # +0x0A/+0x0B: the AOT the player stands in this frame (bio2.exe 0x4E93B0); item-use
            # scripts check it ("is the player at the statue?")
            here = [a for a in self._aots_here() if a.sce == 6] or self._aots_here()
            return here[0].id if here else 255
        if m == 7 and isinstance(ent, Enemy) and 7 not in ent.members:
            return ((ent.ai & 0xFF) << 8 | (ent.state & 0xFF)) | (0 if ent.active else 0x8000)
        return ent.members.get(m, 0)

    def _member_set(self, t, m, v):
        ent = self._work(t)
        if ent is None:
            return
        if m == MEM_X:
            ent.x = v
        elif m == MEM_Y:
            ent.y = v
        elif m == MEM_Z:
            ent.z = v
        elif m == MEM_YDIR:
            # not wrapped: the game keeps the 16-bit value (704's last scene turns the player
            # 150 a frame until the direction passes 4096)
            ent.d = v
        elif m == MEM_FLOOR and isinstance(ent, SceneObject):   # 603's crate after it has fallen
            ent.floor = v
        else:
            ent.members[m] = v
            if m == 7 and isinstance(ent, Enemy):      # M_TYPE: 0x8000 = asleep (scripts wake enemies with it)
                ent.active = not (v & 0x8000)
        if ent is self.player and m in (MEM_X, MEM_Z):
            self.emit("player_moved_by_script", x=ent.x, z=ent.z)

    # ------------------------------------------------------------------ AOT construction
    def _door_aot(self, ins):
        a = ins.args
        if ins.name == "door_aot_se":
            idd, sce, sat, flr, sup, x, z, w, d = a[:9]
            quad = rect_quad(x, z, w, d)
            rest = a[9:]
        else:
            idd, sce, sat, flr, sup = a[:5]
            p = a[5:13]
            quad = [(p[0], p[1]), (p[2], p[3]), (p[4], p[5]), (p[6], p[7])]
            rest = a[13:]
        nx, ny, nz, nd, st, rm, cut, nfloor, tex, anim, snd, lock_id, lock_type, free = rest[:14]
        door = dict(next_x=nx, next_y=ny, next_z=nz, next_d=nd, stage=st + 1, room=rm, cut=cut, next_floor=nfloor,
                    texture=tex, animation=anim, sound=snd, lock_id=lock_id, lock_type=lock_type)
        self.aots[idd] = Aot(idd, sce, sat, flr, quad, (), door=door)

    def _item_aot(self, ins):
        a = ins.args
        if ins.name == "item_aot_set":
            idd, sce, sat, flr, sup, x, z, w, d = a[:9]
            quad = rect_quad(x, z, w, d)
            item, amount, gid, md1, act = a[9:14]
        else:
            idd, sce, sat, flr, sup = a[:5]
            p = a[5:13]
            quad = [(p[0], p[1]), (p[2], p[3]), (p[4], p[5]), (p[6], p[7])]
            item, amount, gid, md1, act = a[13:18]
        if self.get_flag(self.item_group(), gid):
            return            # already picked up
        self.aots[idd] = Aot(idd, sce, sat, flr, quad, (), item=dict(item=item, amount=amount, gid=gid, md1=md1))

    def _spawn_enemy(self, a):
        # u, id, type, state, ai, floor, sound bank, texture, killid, x, y, z, d, u, u
        _, eid, typ, state, ai, flr, sb, tex, killid, x, y, z, d = a[:13]
        if killid != 0xFF and self.get_flag(FG["FG_ENEMY"], killid):
            return
        fam = enemy_family(typ)
        hp = ENEMY_HP.get(fam, 100) if is_hostile(typ) else 9999
        e = Enemy(eid, typ, x, y, z, d, flr, ai, state, killid, hp, hp, active=not (ai & 0x80))
        self.enemies[eid] = e
        from . import bosses
        e.boss = bosses.attach(self, e)
        if is_hostile(typ) and fam not in ENEMY_SPEED:
            self.gap(f"enemy:{fam}", f"Enemy '{e.name}' uses a generic movement model.")
        self.emit("enemy_spawn", id=eid, type=typ, name=e.name)

    # ------------------------------------------------------------------ messages
    def room_message_offset(self, msg_id):
        b = self.msg_chunk
        if len(b) < 2:
            return None
        first = struct.unpack_from("<H", b, 0)[0]
        if msg_id < 0 or msg_id * 2 + 2 > first or first > len(b):
            return None
        return struct.unpack_from("<H", b, msg_id * 2)[0]

    def room_message_text(self, msg_id):
        """The text of room message msg_id as it is now in memory (the mod may have patched it)."""
        o = self.room_message_offset(msg_id)
        if o is None or o >= len(self.msg_chunk):
            return None
        end = self.msg_chunk.find(b"\xfe", o)
        return decode_message(bytes(self.msg_chunk[o:end + 2 if end >= 0 else len(self.msg_chunk)]))

    def room_message_pages(self, msg_id):
        o = self.room_message_offset(msg_id)
        if o is None:
            return 1
        b = self.msg_chunk
        n, i = 1, o
        while i < len(b) and b[i] != 0xFE:
            if b[i] == 0xFD:
                n += 1
                i += 2
                continue
            if b[i] == 0xFB:          # a yes/no question ends the message
                break
            i += 2 if b[i] in (0xFA, 0xF9, 0xF8, 0xEF, 0xF0) else 1
        return n

    def room_message_choices(self, msg_id):
        """How many choices the question offers: 0xFB's parameter picks a set (bio2.exe
        0x52E528): 0 Yes/No, 1 three, 2 the key pad's six digits, 3 two, 4 three, 5 three."""
        off = self.room_message_offset(msg_id)
        if off is None:
            return 2
        b = self.msg_chunk
        i = off
        while i < len(b) - 1:
            if b[i] == 0xFE:
                return 2
            if b[i] == 0xFB:
                return {0: 2, 1: 3, 2: 6, 3: 2, 4: 3, 5: 3}.get(b[i + 1], 2)
            i += 2 if b[i] in (0xFA, 0xF9, 0xF8, 0xEF, 0xF0, 0xFD) else 1
        return 2

    def refresh_message_text(self):
        m = self.message
        if m is not None and m.msg_id >= 0:
            t = self.room_message_text(m.msg_id)
            if t is not None:
                m.text = t
                m.yesno = "[Yes/No]" in t
                m.pages = self.room_message_pages(m.msg_id)

    def show_message(self, text, yesno=False, source="", thread=None, on_answer=None, msg_id=-1):
        old = self.message
        if old is not None and old.thread is not None and old.thread is not thread:
            old.thread.waiting_message = False      # a newer message replaced it (cutscene lines)
        self.message = Message(text, yesno, source, thread, on_answer, msg_id)
        if msg_id >= 0:
            self.message.pages = self.room_message_pages(msg_id)
            self.message.choices = self.room_message_choices(msg_id)
        if thread is not None:
            thread.waiting_message = True
        self.emit("message", text=text, yesno=yesno, source=source)

    def answer(self, yes=True):
        """Dismiss the current message (yes/no answer for questions)."""
        m = self.message
        if m is None:
            return
        if m.pages > 1 and m.page_frames < 3:   # still typing: action finishes the page first
            m.page_frames = 3
            return
        if m.page < m.pages - 1:          # more pages: action shows the next one
            m.page += 1
            m.page_frames = 0
            self.emit("message_page", page=m.page)
            return
        self.message = None
        if m.yesno:
            # the cursor goes to bits 29-31 of the message flags (yes/no: bit 31; 20B's key pad
            # reads all three)
            c = getattr(m, "cursor", None)
            if c is None or (c == 0 and not yes):
                c = 0 if yes else 1
            if getattr(m, "choices", 2) > 2:
                for k, bit in ((29, 4), (30, 2), (31, 1)):
                    self.set_flag(FG["FG_MESSAGE"], k, 1 if c & bit else 0)
            else:
                self.set_flag(FG["FG_MESSAGE"], 31, c & 1)
        if m.thread is not None:
            m.thread.waiting_message = False
        self.emit("message_closed", yes=yes)
        if m.on_answer:
            m.on_answer(yes)

    # ------------------------------------------------------------------ inventory
    def has_item(self, item):
        return any(s[0] == item for s in self.inventory)

    def add_item(self, item, qty=1, source=""):
        if FILE_FIRST <= item <= FILE_LAST:
            # files go into the file folder (FILE_FLG), never an inventory slot
            self.file_flags |= 0x80000000 >> (item - FILE_FIRST)
            self.emit("item_get", item=item, name=item_name(item), qty=qty, source=source)
            return True
        for s in self.inventory:
            if s[0] == item and item in STACK_MAX:
                s[1] = min(STACK_MAX[item], s[1] + qty)
                self.emit("item_get", item=item, name=item_name(item), qty=qty, source=source)
                return True
        if len(self.inventory) >= self.inventory_size:
            self.emit("inventory_full", item=item, name=item_name(item))
            return False
        self.inventory.append([item, qty])
        self.emit("item_get", item=item, name=item_name(item), qty=qty, source=source)
        return True

    def remove_item(self, item, qty=1, all_=False):
        for s in list(self.inventory):
            if s[0] == item:
                if all_ or s[1] <= qty:
                    self.inventory.remove(s)
                else:
                    s[1] -= qty
                if self.equipped == item and not self.has_item(item):
                    self.equipped = None
                return True
        return False

    def use_item(self, item):
        """Use/equip an inventory item (healing, keys at item-use spots, weapons)."""
        if not self.has_item(item):
            return False
        if 1 <= item <= 19:
            self.equipped = item
            self.emit("equip", item=item, name=item_name(item))
            return True
        if item in HEALING:
            heal = HEALING[item]
            if item == 40 or item in (43, 45, 46):
                self.player.poisoned = False
            self.player.hp = min(self.player.max_hp, self.player.hp + heal)
            self.remove_item(item)
            self.emit("heal", item=item, name=item_name(item), hp=self.player.hp)
            return True
        # Key items: the game puts the item in variable 2 and runs main procedure 1, the room's
        # "item used" handler (it checks the item and which AOT the player stands in)
        self.setv(V_USED_ITEM, item)
        here = self._aots_here()
        # [sim note] variable 0 is the item-use spot the item was used in (10B's reliefs check
        # it). Where several spots overlap (303's plug panel: ids 6, 3, 4, 5, one per plug) the
        # script checks for 6, the highest, whatever the plug: taken here as the highest id
        spots = [a.id for a in here if a.sce == 6]
        for aot in here:
            if aot.sce == 6 and aot.data and aot.data[2] == item:
                # the spot marks FG_USE[item] while the player stands in it: the item can be used
                self.set_flag(aot.data[0], aot.data[2], 1)
                self.setv(0, max(spots))
                self.emit("item_used", item=item, name=item_name(item), where=aot.id)
                self._item_pending = True      # main 1 sees variable 2 on the next frame
                if len(aot.data) > 4 and aot.data[4]:
                    pass
                return True
        door = self._facing_door()
        if door and door.door["lock_type"] == item:
            self.set_flag(FG["FG_KEY"], door.door["lock_id"] & 0x7F, 1)
            self.emit("door_unlocked", item=item, name=item_name(item), aot=door.id)
            self.show_message(f"You used the {item_name(item)}.", source="system")
            if item_is_consumed_key(item, self):
                self.remove_item(item)
            return True
        self.emit("item_no_use", item=item, name=item_name(item))
        self.show_message("You can't use this item here.", source="system")
        return False

    # ------------------------------------------------------------------ AOT activation
    def _aots_here(self):
        p = self.player
        # bio2.exe 0x4E9150: sat bit 0x20 tests the point 0x26C (620) in front of the player,
        # bit 0x40 the player's own position
        probe = self._front_probe(620)
        # the game walks SCE_AOT[0..31] in order, so the lowest id wins (110: the ladder event 1
        # before the door 2 it opens)
        return sorted([a for a in self.aots.values() if a.player_triggers and self._floor_ok(a) and
                       (a.contains(p.x, p.z) or ((a.sat & scd.SAT_FRONT) and a.contains(*probe)))],
                      key=lambda a: a.id)

    def _floor_ok(self, aot):
        return aot.floor in (0, 0xFF) or aot.floor == self.player.floor or True

    def _front_probe(self, dist=800):
        fx, fz = facing_vector(self.player.d)
        return self.player.x + fx * dist, self.player.z + fz * dist

    def _facing_ok(self, aot):
        if not (aot.sat & scd.SAT_FRONT):
            return True
        px, pz = self.player.x, self.player.z
        cx, cz = aot.centre
        if aot.contains(*self._front_probe(400)):
            return True
        if math.hypot(cx - px, cz - pz) < 300:
            return True
        want = dir_towards(px, pz, cx, cz)
        return abs(angle_diff(self.player.d, want)) < 1024   # within 90 degrees

    def actionable_aots(self):
        """AOTs the action button would trigger right now, best first."""
        p = self.player
        probe = self._front_probe()
        out = []
        for aot in self.aots.values():
            if not aot.player_triggers or aot.sce == 0:
                continue
            if aot.sce in (3, 11, 12):
                continue
            # bio2.exe 0x4E921B: an AOT floor byte below 0x80 must match the player's floor
            if aot.floor < 0x80 and aot.floor != p.floor:
                continue
            inside = aot.contains(p.x, p.z) or aot.contains(*probe)
            if inside and self._facing_ok(aot) and (aot.manual or aot.sce in (1, 2, 4, 9, 10)):
                out.append(aot)
        # bio2.exe 0x4E9150 walks SCE_AOT[0..31] in order and the first hit wins
        out.sort(key=lambda a: a.id)
        return out

    def _facing_door(self):
        for a in self.actionable_aots():
            if a.door and a.sce == 1:
                return a
        return None

    def press_action(self):
        if self.message:
            self.answer(True)
            return
        if self.aiming:
            self.fire()
            return
        acts = self.actionable_aots()
        if not acts:
            if self.try_climb():
                return
            self.emit("action_nothing")
            return
        self.activate_aot(acts[0])

    def object_collisions(self):
        """Room objects' collision boxes as SCA-style boxes (bio2.exe 0x4CD610), on the floor
        bands their height covers."""
        out = []
        for ob in getattr(self, "objects", {}).values():
            b = ob.box()
            if b is None:
                continue
            cx, cz, hx, hz, ytop, ybot = b
            mask = 0
            for f in range(16):
                top, bottom = -(f + 1) * 1800, -f * 1800
                if min(bottom, ybot) - max(top, ytop) > 100:
                    mask |= 1 << f
            out.append(Collision(900 + ob.idx, round(cx - hx), round(cz - hz), round(2 * hx), round(2 * hz), 0,
                                 mask or (1 << ob.floor), 0, 0x8000))
        return out

    def stair_walls(self):
        """Stairs are walls for the player (bio2.exe 0x53AD00: shape 12 pushes out like a box;
        the player's move passes include bit 0x8000) except the one being climbed: the player
        lines up at an end and presses action to take them (see _stair_enter)."""
        out = []
        for c in self.collisions:
            if c.shape == 12 and (c.raw_flag & 0x8000) and not (c.raw_flag & 0x100) and c.index != self.player.stair:
                out.append(Collision(c.index, c.x, c.z, c.w, c.d, 0, c.floor_mask, c.raw_type, c.raw_flag))
        return out

    def move_collisions(self):
        extra = self.object_collisions() + self.stair_walls()
        return self.collisions + extra if extra else self.collisions

    def stair_ahead(self):
        """The stairway the action button would start climbing now, or None."""
        p = self.player
        if p.stair >= 0:
            return None
        fx, fz = facing_vector(p.d)
        for c in self.collisions:
            if c.shape != 12 or not (c.raw_flag & 0x8000) or not c.on_floor(p.floor):
                continue
            along_x = ((c.raw_type >> 4) & 3) in (0, 1)
            if (abs(fx) < 0.7 if along_x else abs(fz) < 0.7):
                continue
            for dist in (300, 500, 700, 900):
                if c.contains(p.x + fx * dist, p.z + fz * dist):
                    return c
        return None

    def _stair_enter(self):
        """Action at a stairway's end, facing along it: start climbing it."""
        p = self.player
        fx, fz = facing_vector(p.d)
        for c in self.collisions:
            if c.shape != 12 or not (c.raw_flag & 0x8000) or not c.on_floor(p.floor):
                continue
            kind = (c.raw_type >> 4) & 3
            along_x = kind in (0, 1)
            if (abs(fx) < 0.7 if along_x else abs(fz) < 0.7):
                continue                      # not facing along the stairs
            for dist in (300, 500, 700, 900):
                x, z = p.x + fx * dist, p.z + fz * dist
                if c.contains(x, z):
                    p.stair = c.index
                    p.members["stair_on"] = False
                    self.emit("stairs_enter", index=c.index)
                    return True
        return False

    def _stair_leave(self):
        p = self.player
        if p.stair < 0:
            return
        for c in self.collisions:
            if c.index == p.stair:
                inside = c.x <= p.x <= c.x + c.w and c.z <= p.z <= c.z + c.d
                if inside:
                    p.members["stair_on"] = True
                m = 150 if p.members.get("stair_on") else 1000     # stepping on from the end first
                if not (c.x - m <= p.x <= c.x + c.w + m and c.z - m <= p.z <= c.z + c.d + m):
                    p.members["stair_on"] = False
                    p.stair = -1
                    p.y = p.floor * FLOOR_UNIT       # settle on the landing
                    self.emit("stairs_leave", index=c.index)
                return
        p.stair = -1

    def _object_climb(self):
        """Climbable objects (be_flg 0x100, the archive room's stepladder): action facing one
        climbs on top; action on top facing out of it gets down. Approximate: the game animates it."""
        p = self.player
        fx, fz = facing_vector(p.d)
        for ob in getattr(self, "objects", {}).values():
            b = ob.box()
            if b is None or not (ob.be & 0x100):
                continue
            cx, cz, hx, hz, ytop, ybot = b
            inside = abs(p.x - cx) <= hx and abs(p.z - cz) <= hz
            top_floor = max(0, int(round(ytop / FLOOR_UNIT)))
            if not inside:
                if p.floor != ob.floor:
                    continue
                for dist in (300, 500, 700):
                    x, z = p.x + fx * dist, p.z + fz * dist
                    if abs(x - cx) <= hx and abs(z - cz) <= hz:
                        old = p.floor
                        p.x, p.z, p.y = cx, cz, ytop
                        self.emit("climb", up=True, floor=p.floor, old=old, obj=ob.idx)
                        self.emit("floor_change", floor=p.floor, old=old, shape="climb")
                        return True
            elif p.floor == top_floor and p.floor > 0:
                for dist in (max(hx, hz) + 500, 1200, 1500):
                    x, z = p.x + fx * dist, p.z + fz * dist
                    if not (abs(x - cx) <= hx and abs(z - cz) <= hz) and \
                            blocking_at(self.move_collisions(), ob.floor, x, z) is None:
                        old = p.floor
                        p.x, p.z, p.y = x, z, ob.floor * FLOOR_UNIT
                        self.emit("climb", up=False, floor=p.floor, old=old, obj=ob.idx)
                        self.emit("floor_change", floor=p.floor, old=old, shape="drop")
                        return True
        return False

    def climb_target(self):
        """A climbable box (shape 9, climb up) or ledge (shape 10, drop down) directly in front."""
        p = self.player
        fx, fz = facing_vector(p.d)
        for c in self.collisions:
            if c.shape not in (9, 10) or not (c.raw_flag & 0x8000):   # the player's climb check (bio2.exe 0x4CDE9A)
                continue
            for dist in (300, 500, 700):
                x, z = p.x + fx * dist, p.z + fz * dist
                if c.contains(x, z) and c.on_floor(p.floor):   # the floor mask test (bio2.exe 0x4E03B0)
                    return c
        return None

    def climb_direction(self, c):
        """True = climb up onto it, False = drop down from it, None = not possible."""
        p = self.player
        if c.shape == 9 and c.on_floor(p.floor):
            return True
        if p.floor > 0 and c.shape == 10 and c.on_floor(p.floor):
            return False
        return None

    def climb_plan(self):
        """(up, landing_x, landing_y, landing_z) if the action button would climb/drop here."""
        c = self.climb_target()
        if c is None:
            return None
        p = self.player
        up = self.climb_direction(c)
        if up is None:
            return None
        fx, fz = facing_vector(p.d)
        ny = p.y + (FLOOR_UNIT if up else -FLOOR_UNIT)
        floor = max(0, int(round(ny / FLOOR_UNIT)))
        for dist in (900, 1200, 1500, 700, 1800, 2200):
            x, z = p.x + fx * dist, p.z + fz * dist
            if blocking_at(self.collisions, floor, x, z) is None:
                return up, x, ny, z
        return None

    def try_climb(self):
        if self._stair_enter():
            return True
        if self._object_climb():
            return True
        plan = self.climb_plan()
        if plan is None:
            return False
        up, x, y, z = plan
        p = self.player
        old = p.floor
        p.x, p.y, p.z = x, y, z
        self.emit("climb", up=up, floor=p.floor, old=old)
        self.emit("floor_change", floor=p.floor, old=old, shape="climb" if up else "drop")
        return True

    def activate_aot(self, aot: Aot, scripted=False):
        self.emit("aot_activate", id=aot.id, aot_kind=aot.kind, scripted=scripted)
        if aot.sce == 1 and aot.door:
            self._use_door(aot)
        elif aot.sce == 2 and aot.item:
            self._pickup(aot)
        elif aot.sce == 4:      # message
            mid = aot.data[0] if aot.data else 0
            text = self.room_message_text(mid) or (self.room.messages[mid] if mid < len(self.room.messages) else f"<message {mid}>")
            self.show_message(text, "[Yes/No]" in text, source=f"room {self.room.rid} msg {mid}", msg_id=mid)
        elif aot.sce == 0:
            return               # SCE_AUTO / reset: inert
        elif aot.sce == 5:       # event
            if aot.data and len(aot.data) >= 4 and aot.data[2] == OP["gosub"]:
                th = self._new_thread_from(self._main_thread_proto(), aot.data[3])
                th.origin = f"event aot {aot.id} -> main:{aot.data[3]}"
            elif aot.sce == 5:
                self.gap(f"evtaot:{self.room.rid}:{aot.id}", f"Event AOT {aot.id} in room {self.room.rid} has data {aot.data}; not understood.")
        elif aot.sce == 9:
            self._typewriter()
        elif aot.sce == 10:
            self.emit("item_box", items=list(self.item_box))
            self.show_message("Item box. (Use the 'box' command to store or take items.)", source="system")
        elif aot.sce == 6:
            if aot.data:
                self.set_flag(aot.data[0], aot.data[2], aot.data[4] if len(aot.data) > 4 else 1)
        elif aot.sce == 13:
            # a desk or drawer: data[2] names the hidden item AOT it hands over; once that is
            # taken, data[0] is the message it shows (an approximation of bio2.exe's handler)
            d = aot.data or (0, 0, 255)
            inner = self.aots.get(d[2]) if len(d) > 2 else None
            if inner is not None and inner.sce == 2 and inner.item and not self.get_flag(self.item_group(), inner.item["gid"]):
                self._pickup(inner)
            else:
                mid = d[0]
                text = self.room_message_text(mid) or (self.room.messages[mid] if mid < len(self.room.messages) else "Nothing.")
                self.show_message(text, source=f"room {self.room.rid} msg {mid}", msg_id=mid)
        elif aot.sce == 14:
            # a computer terminal (bio2.exe 0x4D1280): the session opens its screen
            if self.room is not None and self.room.rid in ("200",):
                self.pending_terminal = "doorlock"
            else:
                self.gap("terminal:" + (self.room.rid if self.room else "?"),
                         "This computer terminal (sce 14) is not modelled.")
        elif aot.sce == 1:
            self.gap(f"door-no-data:{aot.id}", f"Door AOT {aot.id} was re-typed by script without door data.")

    def _update_attached_aots(self):
        """Areas set with sup 0x80 move with a room object (the library's shelf switches)."""
        for a in self.aots.values():
            if not (a.sup & 0x80) or not a.base:
                continue
            ox = oz = None
            if a.sup & 0x40:
                e = self.enemies.get(a.sup & 0x3F)
                if e is not None:
                    ox, oz = e.x, e.z
            else:
                ob = getattr(self, "objects", {}).get(a.sup & 0x3F)
                if ob is not None:
                    ox, oz = ob.x, ob.z
            if ox is not None:
                a.quad = [(x + ox, z + oz) for x, z in a.base]

    def _main1_tick(self):
        """bio2.exe 0x4E42D0, called from the game loop every frame, restarts main procedure 1 at
        script level 0 (bio2.exe 0x4E3FA0 resets the level): rooms use it as a quick check of the
        view, lights and an item just used (10E: Sherry seen from camera 1 or 8; 112: the shelves
        in place; 111: the crank). Variable 2, the item used, counts for one run only."""
        if self.room is None or len(self.room.scd_main) < 2 or self.dead:
            return
        if getattr(self, "_item_pending", False):
            self._item_pending = False
        else:
            self.setv(V_USED_ITEM, 0)
        self.threads = [th for th in self.threads if not th.origin.endswith("-> main:1")]
        th = self._new_thread_from(self._main_thread_proto(), 1)
        th.origin = "frame -> main:1"
        self._run_thread(th)
        if th.done and th in self.threads:
            self.threads.remove(th)

    def _main_thread_proto(self):
        return Thread(0, self._main_procs, [Frame(0, 0)])

    def _use_door(self, aot: Aot):
        d = aot.door
        lt, lid = d["lock_type"], d["lock_id"] & 0x7F     # FG_KEY index: the id byte carries bit 0x80
        if lt == 0xFF and not self.get_flag(FG["FG_KEY"], lid):     # unlocked from the other side sets the flag
            self.emit("door_locked", aot=aot.id, key=None)
            self.show_message("It's locked.", source="system")
            return
        if lt not in (0, 0xFE, 0xFF) and not self.get_flag(FG["FG_KEY"], lid):
            if self.has_item(lt):
                self.set_flag(FG["FG_KEY"], lid, 1)
                self.emit("door_unlocked", item=lt, name=item_name(lt), aot=aot.id)
                if item_is_consumed_key(lt, self):
                    self.remove_item(lt)
                self.show_message(f"You used the {item_name(lt)}.", source="system",
                                  on_answer=lambda yes: self._go_through(aot))
                return
            self.emit("door_locked", aot=aot.id, key=lt, key_name=item_name(lt))
            self.show_message("It's locked.", source="system")
            return
        if lt == 0xFE and not self.get_flag(FG["FG_KEY"], lid):
            self.set_flag(FG["FG_KEY"], lid, 1)
            self.emit("door_unlocked", item=None, aot=aot.id)
        self._go_through(aot)

    def _go_through(self, aot: Aot):
        d = aot.door
        if not self.lib.exists(self.player.character, d["stage"], d["room"]):
            self.gap(f"missing-room:{d['stage']}{d['room']:02X}", f"Door leads to room {d['stage']:X}{d['room']:02X} which is not in the game data.")
            return
        self.emit("door_transition", frm=self.room.rid, to=f"{d['stage']:X}{d['room']:02X}", aot=aot.id)
        self.enter_room(d["stage"], d["room"], d["next_x"], d["next_y"], d["next_z"], d["next_d"], d["cut"], via=aot.id)

    def _pickup(self, aot: Aot):
        it = aot.item
        name = item_name(it["item"])
        qty = it["amount"]

        def take(yes):
            if not yes:
                return
            if self.add_item(it["item"], qty, source=f"aot {aot.id}"):
                self.set_flag(self.item_group(), it["gid"], 1)
                self.aots.pop(aot.id, None)
            else:
                self.show_message("You cannot carry any more items.", source="system")

        qtxt = f" ({qty})" if qty > 1 else ""
        self.show_message(f"You found {name}{qtxt}. Will you take it? [Yes/No]", yesno=True, source="item",
                          on_answer=take)

    def _typewriter(self):
        def save(yes):
            if yes and self.has_item(30):
                self.remove_item(30)
                slot = len(self.saves) + 1
                self.saves[slot] = self.snapshot()
                self.emit("saved", slot=slot)
                self.show_message(f"Game saved to slot {slot}.", source="system")
            elif yes:
                self.show_message("You need an ink ribbon to save.", source="system")
        self.show_message("It's a typewriter. Will you save your game? [Yes/No]", yesno=True, source="system",
                          on_answer=save)

    def snapshot(self):
        return dict(room=(self.room.stage, self.room.room), pos=(self.player.x, self.player.y, self.player.z, self.player.d),
                    hp=self.player.hp, inv=[list(s) for s in self.inventory], flags={k: set(v) for k, v in self.flags.items()},
                    vars=list(self.vars), cut=self.cut)

    # ------------------------------------------------------------------ camera
    def _set_cut(self, cut, why):
        if cut != self.cut:
            old = self.cut
            self.cut = cut
            self.setv(V_CUT, cut)
            self.emit("camera", cut=cut, old=old, why=why)

    def _update_camera(self):
        """RVD camera switching (as reverse-engineered by OpenBiohazard2): from the current camera,
        check the zones whose cam0 is this camera and cam1 != 0, plus the last cam1 == 0 zone
        when there are two or more (the first one describes the camera's own area)."""
        p = self.player
        cands = [sw for sw in self.room.switches if sw.cam0 == self.cut and sw.cam1 != 0]
        zero = [sw for sw in self.room.switches if sw.cam0 == self.cut and sw.cam1 == 0]
        if len(zero) >= 2:
            cands.append(zero[-1])
        for sw in cands:
            if sw.floor != 255 and sw.floor != p.floor:
                continue
            if sw.cam1 != self.cut and point_in_quad(p.x, p.z, sw.quad):
                self._set_cut(sw.cam1, "zone")
                return

    # ------------------------------------------------------------------ player physics
    def item_group(self) -> int:
        """Item pickup flags: group 8, from stage 5 on group 34 (bio2.exe 0x4E531D)."""
        return FG["FG_ITEM_2"] if self.room is not None and self.room.stage >= 5 else FG["FG_ITEM"]

    @property
    def player_locked(self) -> bool:
        return bool(self.message) or self.get_flag(FG["FG_STOP"], 7) == 1 or self.dead

    def _collision_floor(self):
        """Floors whose collision applies to the player.  On stairs/ramps both ends count, so
        neither the walls under the upper landing nor the open air above the lower floor block."""
        p = self.player
        for c in self.collisions:
            # an active stairway (player bit set; room 111's stairs are switched on by the crank)
            # counts as soon as the body reaches it, so the box under it does not stop the player
            if c.shape in (11, 12) and (c.raw_flag & 0x8000) and (c.shape == 11 or c.index == p.stair) and \
                    c.x - 450 <= p.x <= c.x + c.w + 450 and c.z - 450 <= p.z <= c.z + c.d + 450:
                low = (c.floor_mask & -c.floor_mask).bit_length() - 1 if c.floor_mask else 0
                mult = c.rise
                if low <= p.floor <= low + max(1, mult):
                    return (low, low + max(1, mult))
        return p.floor

    def _try_move(self, dist):
        p = self.player
        fx, fz = facing_vector(p.d)
        nx, nz = p.x + fx * dist, p.z + fz * dist
        floor = self._collision_floor()
        cols = self.move_collisions()
        res = push_out(cols, floor, p.x, p.z, nx, nz)
        if res is not None:
            hit = blocking_at(cols, floor, nx, nz)
            rx, rz = res
            if math.hypot(rx - p.x, rz - p.z) > 3:
                p.x, p.z = rx, rz
                self._apply_slope()
                return True, hit
            if hit is not None:
                return False, hit
        hit = blocking_at(cols, floor, nx, nz)
        if hit is not None and blocking_at(cols, floor, p.x, p.z) is not None:
            # already overlapping geometry (placed there by a script): let the player walk out
            here = blocking_at(cols, floor, p.x, p.z, radius=0)
            if here is None or not here.contains(nx, nz):
                hit = None
        if hit is None:
            p.x, p.z = nx, nz
            self._apply_slope()
            return True, None
        # slide along walls like the game does
        for sx, sz in ((nx, p.z), (p.x, nz)):
            if (sx, sz) != (p.x, p.z) and blocking_at(cols, floor, sx, sz) is None:
                moved = math.hypot(sx - p.x, sz - p.z)
                if moved > 3:
                    p.x, p.z = sx, sz
                    self._apply_slope()
                    return True, hit
        return False, hit

    def _apply_slope(self):
        """Stairs (12) and slopes (11) set the player's height.  The ramp rises from the lowest
        floor in its floor mask by (type >> 6) & 0x1F floors; (type >> 4) & 3 says which edge is low."""
        p = self.player
        self._stair_leave()
        for c in self.collisions:
            if c.shape not in (11, 12) or not (c.x <= p.x <= c.x + c.w and c.z <= p.z <= c.z + c.d):
                continue
            if c.shape == 12 and c.index != p.stair:
                continue                      # stairs only carry the player who took them
            low = (c.floor_mask & -c.floor_mask).bit_length() - 1 if c.floor_mask else 0
            mult = c.rise
            if not (low <= p.floor <= low + max(1, mult)):
                continue
            kind = (c.raw_type >> 4) & 3
            if kind in (0, 1):
                bottom = c.x if kind == 0 else c.x + c.w
                t = abs(p.x - bottom) / (c.w or 1)
            else:
                bottom = c.z if kind == 2 else c.z + c.d
                t = abs(p.z - bottom) / (c.d or 1)
            newy = low * FLOOR_UNIT + mult * FLOOR_UNIT * min(1.0, max(0.0, t))
            if abs(newy - p.y) > 700:
                continue          # the ramp is on another level (e.g. a staircase below a landing)
            if abs(newy - p.y) > 1:
                old = p.floor
                p.y = newy
                if p.floor != old:
                    self.emit("floor_change", floor=p.floor, old=old, shape=c.shape_name)
            return

    def _player_update(self):
        if self.player_locked:
            return
        c = self.controls
        p = self.player
        if "aim" in c:
            if not self.aiming:
                self.aiming = True
                self._auto_aim()
                self.emit("aim", on=True)
            return
        elif self.aiming:
            self.aiming = False
            self.emit("aim", on=False)
        turning = 0
        if "left" in c:
            turning -= TURN_SPEED
        if "right" in c:
            turning += TURN_SPEED
        if turning:
            p.d = (p.d + turning) % DIR_FULL
        if "up" in c:
            speed = RUN_SPEED if "run" in c else WALK_SPEED
            ok, hit = self._try_move(speed)
            if not ok:
                self.emit("bump", collision=hit.index if hit else None, shape=hit.shape_name if hit else "")
        elif "down" in c:
            ok, hit = self._try_move(-BACK_SPEED)
            if not ok:
                self.emit("bump", collision=hit.index if hit else None, shape=hit.shape_name if hit else "", backwards=True)

    def _auto_aim(self):
        tgt = self.nearest_enemy(max_angle=1024, max_dist=self._range())
        if tgt:
            self.player.d = dir_towards(self.player.x, self.player.z, tgt.x, tgt.z)
            self.emit("aim_target", id=tgt.id, name=tgt.name)

    def _range(self):
        return WEAPON_RANGE.get(self.equipped, DEFAULT_RANGE)

    def nearest_enemy(self, max_angle=2048, max_dist=1e9, hostile_only=True):
        p = self.player
        best = None
        for e in self.enemies.values():
            if not e.alive or (hostile_only and not e.hostile):
                continue
            dist = math.hypot(e.x - p.x, e.z - p.z)
            ang = abs(angle_diff(p.d, dir_towards(p.x, p.z, e.x, e.z)))
            if dist <= max_dist and ang <= max_angle:
                if best is None or dist < best[0]:
                    best = (dist, e)
        return best[1] if best else None

    def fire(self):
        w = self.equipped
        if w is None:
            self.emit("fire_nothing")
            return
        slot = next((s for s in self.inventory if s[0] == w), None)
        if w != 1:
            if slot is None or slot[1] <= 0:
                if not self._reload(w):
                    self.emit("out_of_ammo", weapon=item_name(w))
                    return
                slot = next((s for s in self.inventory if s[0] == w), None)
            slot[1] -= 1
        p = self.player
        fx, fz = facing_vector(p.d)
        hit = None
        best = None
        for e in self.enemies.values():
            if not e.alive or not e.hostile:
                continue
            dx, dz = e.x - p.x, e.z - p.z
            # above the ceiling or below the floor: out of the shot; aiming up (up held while
            # aiming) reaches one high on a wall [sim note: the game's aim-up angle is not modelled]
            up = getattr(self, "aim_up", False) and p.y - e.y > 1500
            if abs(e.y - p.y) > (9000 if up else 3000) or (getattr(self, "aim_up", False) and not up):
                continue
            along = dx * fx + dz * fz
            across = abs(dx * fz - dz * fx)
            if 0 < along <= self._range() and across < 900:
                if best is None or along < best:
                    best, hit = along, e
        self.emit("shot", weapon=item_name(w), hit=hit.id if hit else None)
        if hit:
            dmg = WEAPON_DAMAGE.get(w, 15)
            hit.hp -= dmg
            hit.aware = True
            hit.active = True
            if getattr(hit, "boss", None) is not None:
                hit.boss.on_hit(dmg)
            if hit.hp <= 0:
                hit.alive = False
                if hit.killid != 0xFF:
                    self.set_flag(FG["FG_ENEMY"], hit.killid, 1)
                self.emit("enemy_killed", id=hit.id, name=hit.name)
            else:
                self.emit("enemy_hit", id=hit.id, name=hit.name, hp=hit.hp, max_hp=hit.max_hp)

    def _reload(self, w):
        ammo = scd.AMMO_FOR.get(w)
        if ammo is None:
            return False
        a = next((s for s in self.inventory if s[0] == ammo), None)
        if not a:
            return False
        cap = {2: 18, 3: 13, 4: 18, 5: 8, 6: 8, 7: 5, 8: 7, 9: 1, 10: 1, 11: 1, 12: 18, 14: 100, 15: 100, 16: 100, 19: 15}.get(w, 10)
        slot = next((s for s in self.inventory if s[0] == w), None)
        take = min(cap - slot[1], a[1])
        slot[1] += take
        a[1] -= take
        if a[1] <= 0:
            self.inventory.remove(a)
        self.emit("reload", weapon=item_name(w), rounds=slot[1])
        return take > 0

    # ------------------------------------------------------------------ enemies
    def _enemy_update(self):
        p = self.player
        self.grab_cd = max(0, getattr(self, "grab_cd", 0) - 1)
        for e in self.enemies.values():
            if not e.alive or not e.hostile:
                continue
            if abs(e.x) >= 31000 or abs(e.z) >= 31000:
                continue          # parked off-map by the room script
            dist = math.hypot(e.x - p.x, e.z - p.z)
            if not e.active:
                if dist < 900:            # [sim note] touching a sleeping enemy wakes it here
                    e.active = True
                else:
                    continue
            if e.boss is not None and e.boss.update(dist):
                continue
            # a zombie lying on the floor (pose 2, AI flag 0x40: 20B in Claire's scenario) stays
            # down until the player comes close or hurts it [sim note: the distance is a guess]
            if (e.state & 0xFF) == 2 or (e.ai & 0x40):
                if dist < 1000 or e.hp < e.max_hp:
                    e.state, e.ai = 0, e.ai & ~0x40
                    self.emit("enemy_rises", id=e.id, name=e.name)
                else:
                    continue
            if not e.aware and dist < 9000:
                e.aware = True
                self.emit("enemy_alert", id=e.id, name=e.name)
            if not e.aware or self.player_locked:
                continue
            spd = ENEMY_SPEED.get(e.family, 25)
            # [sim note] high on a wall or ceiling (405's spider at y -7200): it does not reach
            # the player; a spider spits poison from there now and then instead
            if abs(e.y - p.y) > 2500:
                if e.family == "Spider" and dist < 5000:
                    if e.attack_cd <= 0:
                        e.attack_cd = 150
                        if not p.poisoned:
                            p.poisoned = True
                            self.emit("player_poisoned", by=e.name)
                    else:
                        e.attack_cd -= 1
                continue
            if dist > 700:
                vx, vz = (p.x - e.x) / dist, (p.z - e.z) / dist
                nx, nz = e.x + vx * spd, e.z + vz * spd
                if blocking_at(self.collisions, e.floor, nx, nz, radius=300) is None:
                    e.x, e.z = nx, nz
                e.d = dir_towards(e.x, e.z, p.x, p.z)
            else:
                if e.attack_cd <= 0 and self.grab_cd <= 0:
                    dmg = {"Zombie": 20, "Zombie Dog": 15, "Licker": 35, "Tyrant": 60}.get(e.family, 20)
                    p.hp -= dmg
                    e.attack_cd = 90
                    # [sim note] the game plays a grab / bite animation and the player shoves the
                    # attacker off: one attack at a time, then a moment to get away
                    self.grab_cd = 45
                    if dist > 1:
                        bx, bz = p.x + (p.x - e.x) / dist * 500, p.z + (p.z - e.z) / dist * 500
                        if blocking_at(self.collisions, p.floor, bx, bz) is None:
                            p.x, p.z = bx, bz
                    self.emit("player_hurt", by=e.name, damage=dmg, hp=p.hp, condition=p.condition)
                    if p.hp <= 0:
                        self.dead = True
                        self.emit("player_died", by=e.name)
                else:
                    e.attack_cd -= 1

    # ------------------------------------------------------------------ AOT auto triggers
    def _auto_triggers(self):
        p = self.player
        now = set()
        for aot in list(self.aots.values()):
            if not aot.player_triggers or aot.manual:
                continue
            if not aot.contains(p.x, p.z):
                continue
            now.add(aot.id)
            if aot.id in self.in_aots:
                if aot.sce == 11 and self.frame % 30 == 0:
                    self._damage_floor(aot)
                continue
            if aot.sce in (5, 6, 4) and self._facing_ok(aot):
                self.activate_aot(aot)
            elif aot.sce == 11:
                self._damage_floor(aot)
            elif aot.sce == 7:
                self.emit("water", id=aot.id)
        for left in self.in_aots - now:
            a = self.aots.get(left)
            if a is not None and a.sce == 6 and a.data and a.data[0] == FG["FG_USE"]:
                self.set_flag(a.data[0], a.data[2], 0)
        self.in_aots = now

    def _object_triggers(self):
        """Objects run AOTs whose sat has bit 4 (bio2.exe 0x4EA0D0 -> 0x4E9150 with mask 4): the
        object's AOT number goes in its member 9 every frame; an event fires when it arrives."""
        for ob in self.objects.values():
            prev = ob.aot
            ob.aot = 255
            for aot in self.aots.values():
                if not (aot.sat & 0x04) or (aot.floor < 0x80 and aot.floor != ob.floor):
                    continue
                if aot.contains(ob.x, ob.z):
                    ob.aot = aot.id
                    if prev != aot.id and aot.sce == 5:
                        self.emit("object_in_aot", obj=ob.idx, aot=aot.id)
                        self.activate_aot(aot, scripted=True)
                    break

    def _damage_floor(self, aot):
        self.player.hp -= 5
        self.emit("player_hurt", by="fire", damage=5, hp=self.player.hp, condition=self.player.condition)

    # ------------------------------------------------------------------ main loop
    def step(self, frames=1):
        for _ in range(frames):
            if self.dead:
                return
            self.frame += 1
            self._player_update()
            self._update_camera()
            self._update_attached_aots()
            self._auto_triggers()
            self._object_triggers()
            for t in list(self.threads):
                if self.threads and t in self.threads:
                    self._run_thread(t)
            self._main1_tick()
            self.threads = [t for t in self.threads if not t.done]
            self._enemy_update()
            self._follower_update()
            self._update_movers()
            self._update_message_timer()
            if getattr(self, "xa_frames", 0) > 0:
                self.xa_frames -= 1
                if self.xa_frames == 0:
                    self.set_flag(1, 11, 0)
            if self.message is not None:
                self.message.page_frames += 1
            self._watchdog()
            self.emit("frame")

    FOLLOWER_SPEED = 45    # a following partner (Sherry, Ada): slower than walking

    def _follower_update(self):
        """The partner who follows the player (pEnemy[1], script flag 1.3): walks straight
        toward the player, slower than they walk, and stops a metre short. No collision."""
        if not self.get_flag(1, 3) or self.room is None:
            self.follower = None
            return
        rid = self.room.rid
        if getattr(self, "follower", None) is None or self.follower.get("room") != rid:
            self.follower = {"room": rid, "x": self.player.x - 500, "z": self.player.z, "y": self.player.y}
            return
        f = self.follower
        dx, dz = self.player.x - f["x"], self.player.z - f["z"]
        d = math.hypot(dx, dz)
        if d > 1000:
            step = min(self.FOLLOWER_SPEED, d - 1000)
            f["x"] += dx / d * step
            f["z"] += dz / d * step
        f["y"] = self.player.y

    def _update_movers(self):
        keep = []
        for ent, tx, tz, flag in self.movers:
            dx, dz = tx - ent.x, tz - ent.z
            dist = math.hypot(dx, dz)
            if dist <= WALK_SPEED:
                ent.x, ent.z = tx, tz
                self.set_flag(FG["FG_ROOM"], flag, 1)
                if ent is self.player:
                    self.emit("player_moved_by_script", x=tx, z=tz)
                continue
            nx, nz = ent.x + dx / dist * WALK_SPEED, ent.z + dz / dist * WALK_SPEED
            ent.d = dir_towards(ent.x, ent.z, tx, tz)
            if ent is self.player:
                # a scripted walk is still a walk: walls stop it (408: walking up to the railing
                # while Annette talks; the script ends it with its own flag)
                cols = self.move_collisions()
                floor = self._collision_floor()
                if blocking_at(cols, floor, nx, nz) is not None and blocking_at(cols, floor, ent.x, ent.z) is None:
                    self.set_flag(FG["FG_ROOM"], flag, 1)      # as far as it goes: the walk is over
                    continue
            ent.x, ent.z = nx, nz
            keep.append((ent, tx, tz, flag))
        self.movers = keep

    def _update_message_timer(self):
        """Cutscene subtitles in the English PC release are empty strings shown while the voice
        plays; the game advances them itself.  Close them after a voice-length pause."""
        m = self.message
        if m is not None and not m.text.strip():
            m.timer = getattr(m, "timer", 0) + 1
            if m.timer >= 75:
                self.answer(True)

    def _watchdog(self):
        """Cutscenes can wait on animation states the simulator does not play.  If the player is
        frozen and no script has done real work for watchdog_seconds, report it and release."""
        busy = any(t.wait > 0 for t in self.threads) or self.movers or (self.message and self.message.text.strip())
        if busy:
            self.script_progress_frame = self.frame
        if self.get_flag(FG["FG_STOP"], 7) and not (self.message and self.message.text.strip()):
            if self.frame - self.script_progress_frame >= self.watchdog_seconds * FPS:
                stuck = sorted({f"{t.origin}@proc{t.frames[-1].proc}:{t.frames[-1].pc:04X}" for t in self.threads if not t.done})
                self.gap(f"stuck:{self.room.rid}:{','.join(stuck)}",
                         f"Room {self.room.rid}: player frozen and scripts idle for {self.watchdog_seconds}s "
                         f"(waiting threads: {', '.join(stuck) or 'none'}). Released by the watchdog.")
                self.release_lock()
                self.script_progress_frame = self.frame

    def release_lock(self):
        """Force-finish a stuck cutscene: unfreeze the player and stop waiting scripts."""
        self.set_flag(FG["FG_STOP"], 7, 0)
        self.locked_frames = 0
        self.emit("unstuck")

    def hold(self, keys, seconds):
        self.controls = set(keys)
        self.step(int(seconds * FPS))
        self.controls = set()


def item_is_consumed_key(item, game):
    """Keys like the spade key are discarded once all their doors are opened; the sim keeps them."""
    return False
