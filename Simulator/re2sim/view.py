"""GameView: the *only* window the accessibility mod has onto the game.

Everything here corresponds to something the real mod (a DLL inside the game process)
can obtain: game memory (player, enemies, AOT table, inventory, message buffer, flags)
or the room files on disk (collision, camera zones), which the DLL can parse itself
with the same RDT parser.  Keeping the mod behind this interface means logic proven
in the simulator ports to the DLL by swapping this class for memory reads.

Each property notes its real-game source.  Addresses are not yet known for the
Sourcenext 1.1.0 executable; MEMORY_TODO lists what must be located.
"""
from __future__ import annotations

from dataclasses import dataclass

from .scd import item_name

MEMORY_TODO = [
    "Current stage / room / camera cut (game globals; also V_STAGE/V_ROOM/V_CUT script variables)",
    "Player entity: x, y, z, Y-rotation (0-4095), HP, status flags",
    "Player input state (to know forward is held for wall-bump detection) - from the DInput hook",
    "AOT table: id, SCE type, SAT flags, bounds, per-type data (door target, item id/amount)",
    "Enemy entity list: type, x/z, HP, alive flag",
    "Inventory slots (item id, quantity), equipped weapon",
    "On-screen message text buffer + yes/no prompt state + currently-highlighted choice",
    "Script flag arrays (FG_STOP bit 7 = player frozen by event)",
]


@dataclass
class AotInfo:
    id: int
    kind: str          # door, item, message, event, save, itembox, ...
    quad: list
    centre: tuple
    manual: bool
    door_target: str = None      # room id like '101'
    door_locked: bool = False
    door_key: int = None
    item: int = None
    item_amount: int = 0


@dataclass
class EnemyInfo:
    id: int
    type: int
    name: str
    x: float
    z: float
    hp: int
    max_hp: int
    alive: bool
    hostile: bool


class GameView:
    def __init__(self, game):
        self._g = game

    # --- room ---------------------------------------------------------------
    @property
    def room_id(self) -> str:
        return self._g.room.rid if self._g.room else ""

    @property
    def stage(self) -> int:
        return self._g.room.stage

    @property
    def room(self):
        """Parsed room file (the DLL parses ROOMxxxx.RDT from disk itself)."""
        return self._g.room

    @property
    def collisions(self):
        """Live collision table (SCA in memory; scripts toggle entries with sca_id_set)."""
        return self._g.collisions

    @property
    def collision_version(self) -> int:
        return self._g.collision_version

    @property
    def cut(self) -> int:
        return self._g.cut

    # --- player -------------------------------------------------------------
    @property
    def x(self):
        return self._g.player.x

    @property
    def z(self):
        return self._g.player.z

    @property
    def y(self):
        return self._g.player.y

    @property
    def floor(self):
        return self._g.player.floor

    @property
    def dir(self) -> int:
        return self._g.player.d

    @property
    def hp(self):
        return self._g.player.hp

    @property
    def max_hp(self):
        return self._g.player.max_hp

    @property
    def condition(self):
        return self._g.player.condition

    @property
    def poisoned(self):
        return self._g.player.poisoned

    @property
    def frozen(self) -> bool:
        return self._g.player_locked

    @property
    def aiming(self) -> bool:
        return self._g.aiming

    @property
    def dead(self):
        return self._g.dead

    @property
    def frame(self):
        return self._g.frame

    # --- input (from the DInput hook in the real mod) -------------------------
    @property
    def held(self) -> set:
        return set(self._g.controls)

    # --- world objects --------------------------------------------------------
    def aots(self):
        out = []
        for a in self._g.aots.values():
            if not a.player_triggers:
                continue
            info = AotInfo(a.id, a.kind, a.quad, a.centre, a.manual)
            if a.door and a.sce == 1:
                d = a.door
                info.door_target = f"{d['stage']:X}{d['room']:02X}"
                lt = d["lock_type"]
                unlocked = self._g.get_flag(31, d["lock_id"]) == 1
                info.door_locked = lt == 0xFF or (lt not in (0, 0xFE) and not unlocked)
                info.door_key = lt if lt not in (0, 0xFE, 0xFF) else None
            if a.item and a.sce == 2:
                info.item = a.item["item"]
                info.item_amount = a.item["amount"]
            out.append(info)
        return out

    def actionable(self):
        """AOTs the game would trigger on the action button (game's own front check)."""
        ids = [a.id for a in self._g.actionable_aots()]
        return [a for a in self.aots() if a.id in ids]

    def on_stairs(self) -> bool:
        """Player is standing on a stair/ramp collision area (shapes 11/12)."""
        x, z = self.x, self.z
        return any(c.shape in (11, 12) and c.x <= x <= c.x + c.w and c.z <= z <= c.z + c.d
                   for c in self._g.collisions)

    def climbable(self):
        """Climbable box / drop-down ledge in front of the player (from the collision table)."""
        plan = self._g.climb_plan()
        return None if plan is None else ("climb up" if plan[0] else "drop down")

    def stairs_ahead(self):
        """Stairs the action button would start climbing (stairs are taken with action)."""
        return self._g.stair_ahead() is not None

    def enemies(self):
        return [EnemyInfo(e.id, e.type, e.name, e.x, e.z, e.hp, e.max_hp, e.alive, e.hostile)
                for e in self._g.enemies.values()]

    def in_play(self, e) -> bool:
        """Scripts park enemies outside the room (e.g. -32000 or 20000,20000) until a cutscene
        brings them in; treat anything outside the room's collision bounds as not present."""
        if abs(e.x) >= 31000 or abs(e.z) >= 31000:
            return False
        b = self._bounds()
        return b[0] - 1000 <= e.x <= b[1] + 1000 and b[2] - 1000 <= e.z <= b[3] + 1000

    def _bounds(self):
        key = self.room_id
        if getattr(self, "_bkey", None) != key:
            cols = [c for c in self._g.collisions if c.w < 30000 and c.d < 30000] or self._g.collisions
            self._b = (min(c.x for c in cols), max(c.x + c.w for c in cols),
                       min(c.z for c in cols), max(c.z + c.d for c in cols)) if cols else (-32768, 32767, -32768, 32767)
            self._bkey = key
        return self._b

    # --- inventory / UI -------------------------------------------------------
    def inventory(self):
        return [(i, q, item_name(i)) for i, q in self._g.inventory]

    @property
    def equipped(self):
        return self._g.equipped

    @property
    def message(self):
        m = self._g.message
        return None if m is None else (m.text, m.yesno)

    def flag(self, group, idx):
        return self._g.get_flag(group, idx)
