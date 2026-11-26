"""Parser for Resident Evil 2 (PC, Sourcenext/GOG/Steam data) room files (ROOMsrrp.RDT).

File layout (all little-endian):
  0x00  header: nSprite, nCut (cameras), nOmodel, nItem, nDoor, nRoom_at, reverb, unk
  0x08  23 x uint32 chunk offsets (see CHUNKS)

Format knowledge comes from the community: IntelOrca's biohazard-utils (BioRand),
OpenBiohazard2, and the RE2 SCD opcode tables.  Chunk lengths are not stored, so a
chunk runs until the next-highest chunk offset.
"""
from __future__ import annotations

import os
import struct
from dataclasses import dataclass, field

CHUNKS = [
    "edt", "vh", "vb", "trial_vh", "trial_vb", "ova", "sca", "rid", "rvd", "lit",
    "objtable", "flr", "blk", "msg_ja", "msg_en", "tim_scroll", "scd_init", "scd_main",
    "esp_id", "esp_eff", "tim_esp", "tim_obj", "rbj",
]

MSG_TABLE = (" .___()__“”_0123456789:_,\"!?_ABCDEFGHIJKLMNOPQRSTUVWXYZ[/]'-_"
             "abcdefghijklmnopqrstuvwxyz_________")


@dataclass
class Camera:
    index: int
    flag: int
    dist: int
    frm: tuple   # camera position (x, y, z)
    to: tuple    # camera target (x, y, z)


@dataclass
class CameraSwitch:
    flag: int
    floor: int
    cam0: int
    cam1: int
    quad: list   # 4 (x, z) points


@dataclass
class Collision:
    index: int
    x: int
    z: int
    w: int
    d: int
    shape: int      # 0 rect, 1-3 triangles, 6 circle, 7/8 ellipse, 9 climb-up box, 10 jump-down, 11 slope, 12 stairs
    floor_mask: int
    raw_type: int
    raw_flag: int

    def on_floor(self, floor: int) -> bool:
        if floor < 0 or floor > 31:
            return False
        return bool(self.floor_mask & (1 << floor))

    def contains(self, px: float, pz: float) -> bool:
        x, z, w, d = self.x, self.z, self.w, self.d
        s = self.shape
        if s in (0, 9, 10, 11, 12, 13, 14, 15):
            return x <= px <= x + w and z <= pz <= z + d
        if s == 4:   # triangle |\  : (x, z) (x+w, z) (x, z+d)  (bio2.exe 0x4E14B0)
            return _in_tri(px, pz, (x, z), (x + w, z), (x, z + d))
        if s == 5:   # diamond: corners at the box's side midpoints (bio2.exe 0x4E17B0)
            a, b = w / 2, d / 2
            if a <= 0 or b <= 0:
                return False
            return abs(px - (x + a)) / a + abs(pz - (z + b)) / b <= 1.0
        if s == 1:   # triangle \|  : (x, z+d) (x+w, z+d) (x+w, z)
            return _in_tri(px, pz, (x, z + d), (x + w, z + d), (x + w, z))
        if s == 2:   # triangle |/  : (x, z) (x, z+d) (x+w, z+d)
            return _in_tri(px, pz, (x, z), (x, z + d), (x + w, z + d))
        if s == 3:   # triangle /|  : (x, z) (x+w, z+d) (x+w, z)
            return _in_tri(px, pz, (x, z), (x + w, z + d), (x + w, z))
        if s == 6:
            r = w / 2
            return (px - (x + r)) ** 2 + (pz - (z + r)) ** 2 <= r * r
        if s in (7, 8):
            a, b = w / 2, d / 2
            if a <= 0 or b <= 0:
                return False
            return ((px - (x + a)) / a) ** 2 + ((pz - (z + b)) / b) ** 2 <= 1.0
        return x <= px <= x + w and z <= pz <= z + d

    @property
    def rise(self) -> int:
        """Floors a stairway or slope rises: its floor mask runs from the bottom floor to the
        top one (116's long flight: floors 1 to 4); the type's multiplier agrees for ordinary
        stairs but not for those."""
        if not self.floor_mask:
            return 1
        low = (self.floor_mask & -self.floor_mask).bit_length() - 1
        top = self.floor_mask.bit_length() - 1
        if top > low:
            return top - low
        return max(1, (self.raw_type >> 6) & 0x1F)

    @property
    def shape_name(self) -> str:
        return SHAPE_NAMES.get(self.shape, f"shape{self.shape}")


SHAPE_NAMES = {0: "box", 1: "triangle", 2: "triangle", 3: "triangle", 4: "triangle", 5: "diamond", 6: "round object",
               7: "rounded box", 8: "rounded box", 9: "climbable box", 10: "drop-down ledge",
               11: "slope", 12: "stairs"}


def _in_tri(px, pz, a, b, c):
    def sign(p1, p2, p3):
        return (p1[0] - p3[0]) * (p2[1] - p3[1]) - (p2[0] - p3[0]) * (p1[1] - p3[1])
    p = (px, pz)
    d1, d2, d3 = sign(p, a, b), sign(p, b, c), sign(p, c, a)
    has_neg = d1 < 0 or d2 < 0 or d3 < 0
    has_pos = d1 > 0 or d2 > 0 or d3 > 0
    return not (has_neg and has_pos)


@dataclass
class FloorSound:
    x: int
    z: int
    w: int
    d: int
    se: int
    height: int


def decode_message(data: bytes) -> str:
    out = []
    i = 0
    while i < len(data):
        b = data[i]
        if b == 0xF9:          # colour change (green highlight on/off)
            i += 2
            continue
        if b == 0xFA:          # start text + speed
            i += 2
            continue
        if b == 0xFB:          # yes/no question: the message ends there (some have no FE after it)
            out.append(" [Yes/No]")
            break
        if b == 0xFC:
            out.append(" ")
        elif b == 0xFD:        # page break
            out.append(" ")
            i += 1
        elif b == 0xFE:
            break
        elif b == 0xF8:        # item-name insertion (amount / name) – keep a marker
            out.append("<item>")
            i += 1
        elif b == 0xF3:
            out.append("?")
        elif b == 0xEF:        # text colour (one parameter byte)
            i += 1
        elif b == 0xF0:        # extra glyph: 09 0A 0B draw the S.T.A.R.S. logo
            if i + 1 < len(data) and data[i + 1] == 0x09:
                out.append("S.T.A.R.S.")
            i += 1
        elif b < len(MSG_TABLE):
            out.append(MSG_TABLE[b])
        i += 1
    s = "".join(out)
    while "  " in s:
        s = s.replace("  ", " ")
    return s.strip()


@dataclass
class Room:
    path: str
    stage: int          # 1-7 (file digit)
    room: int           # 0x00-0xFF
    player: int         # 0 Leon, 1 Claire
    header: tuple
    offsets: list
    data: bytes = field(repr=False)
    cameras: list = field(default_factory=list)
    switches: list = field(default_factory=list)
    collisions: list = field(default_factory=list)
    boundary: tuple = (0, 0, 0, 0)
    floor_sounds: list = field(default_factory=list)
    messages: list = field(default_factory=list)
    scd_init: list = field(default_factory=list)   # list of procedure byte strings
    scd_main: list = field(default_factory=list)

    @property
    def rid(self) -> str:
        """Room id as the game and community write it, e.g. '100' = stage 1 room 00."""
        return f"{self.stage:X}{self.room:02X}"

    def chunk(self, name: str) -> bytes:
        idx = CHUNKS.index(name)
        start = self.offsets[idx]
        if start == 0:
            return b""
        ends = sorted(set(o for o in self.offsets if o > start))
        # camera mask blocks can sit between chunks; they are referenced from RID entries
        end = ends[0] if ends else len(self.data)
        return self.data[start:end]


def _procs(blob: bytes) -> list:
    if len(blob) < 2:
        return []
    first = struct.unpack_from("<H", blob, 0)[0]
    n = first // 2
    if n == 0 or first > len(blob):
        return []
    offs = list(struct.unpack_from(f"<{n}H", blob, 0))
    procs = []
    for i, o in enumerate(offs):
        e = offs[i + 1] if i + 1 < n else len(blob)
        procs.append(blob[o:e])
    return procs


def load_room(path: str) -> Room:
    data = open(path, "rb").read()
    name = os.path.basename(path).upper()
    # Stages are 1-7. The Japanese/Sourcenext data also has a second set named A-G
    # (A = stage 1 ... G = stage 7); see README "Sourcenext data".
    stage = "ABCDEFG".index(name[4]) + 1 if name[4] in "ABCDEFG" else int(name[4], 16)
    room = int(name[5:7], 16)
    player = int(name[7], 16)
    header = struct.unpack_from("<8B", data, 0)
    offsets = list(struct.unpack_from("<23I", data, 8))
    r = Room(path, stage, room, player, header, offsets, data)

    # cameras (RID): 0x20 bytes each
    o = offsets[CHUNKS.index("rid")]
    for i in range(header[1]):
        flag, dist, fx, fy, fz, tx, ty, tz, mask = struct.unpack_from("<HHiiiiiiI", data, o + i * 0x20)
        r.cameras.append(Camera(i, flag, dist, (fx, fy, fz), (tx, ty, tz)))

    # camera switches (RVD)
    o = offsets[CHUNKS.index("rvd")]
    if o:
        while o + 20 <= len(data):
            flag, floor, c0, c1 = struct.unpack_from("<4B", data, o)
            if flag == 0xFF and floor == 0xFF and c0 == 0xFF and c1 == 0xFF:
                break
            pts = struct.unpack_from("<8h", data, o + 4)
            r.switches.append(CameraSwitch(flag, floor, c0, c1,
                                           [(pts[0], pts[1]), (pts[2], pts[3]), (pts[4], pts[5]), (pts[6], pts[7])]))
            o += 20

    # collision (SCA)
    o = offsets[CHUNKS.index("sca")]
    if o:
        cx, cz, count, cy, cw, cd = struct.unpack_from("<hhIiHH", data, o)
        r.boundary = (cx, cz, cw, cd)
        p = o + 16
        for i in range(max(0, count - 1)):
            x, z, w, d, flg, typ, fmask = struct.unpack_from("<hhHHHHI", data, p)
            r.collisions.append(Collision(i, x, z, w, d, flg & 0x0F, fmask, typ, flg))
            p += 16

    # floor sounds (FLR)
    o = offsets[CHUNKS.index("flr")]
    if o:
        n = struct.unpack_from("<H", data, o)[0]
        for i in range(n):
            x, z, w, d, se, h = struct.unpack_from("<hhHHHH", data, o + 2 + i * 12)
            r.floor_sounds.append(FloorSound(x, z, w, d, se, h))

    # English messages
    blob = r.chunk("msg_en")
    if len(blob) >= 2:
        first = struct.unpack_from("<H", blob, 0)[0]
        n = first // 2
        if 0 < first <= len(blob):
            offs = list(struct.unpack_from(f"<{n}H", blob, 0))
            for i, mo in enumerate(offs):
                e = offs[i + 1] if i + 1 < n else len(blob)
                r.messages.append(decode_message(blob[mo:e]))

    r.scd_init = _procs(r.chunk("scd_init"))
    r.scd_main = _procs(r.chunk("scd_main"))
    return r


class RoomLibrary:
    """Loads and caches all room files from a game data directory."""

    def __init__(self, gamedata: str):
        self.gamedata = gamedata
        self._cache = {}

    def _room_dir(self, player: int) -> str:
        # English/US data keeps rooms in PlN/Rdu; the Japanese Sourcenext data used by
        # Classic REbirth keeps them in PlN/Rdt.
        for sub in ("Rdu", "Rdt", "RDT", "rdt"):
            d = os.path.join(self.gamedata, f"Pl{player}", sub)
            if os.path.isdir(d):
                return d
        return os.path.join(self.gamedata, f"Pl{player}", "Rdu")

    def path_for(self, player: int, stage: int, room: int) -> str:
        return os.path.join(self._room_dir(player), f"ROOM{stage:X}{room:02X}{player}.RDT")

    def exists(self, player: int, stage: int, room: int) -> bool:
        return os.path.exists(self.path_for(player, stage, room))

    def get(self, player: int, stage: int, room: int) -> Room:
        key = (player, stage, room)
        if key not in self._cache:
            self._cache[key] = load_room(self.path_for(player, stage, room))
        return self._cache[key]

    def all_rooms(self, player: int):
        d = self._room_dir(player)
        for n in sorted(os.listdir(d)):
            if n.upper().endswith(".RDT") and n[4] in "1234567":
                yield self.get(player, int(n[4], 16), int(n[5:7], 16))
