"""Lays the simulator's state out in the real game's memory layout (bio2.exe Sourcenext 1.10,
as used by Classic REbirth), so the real C++ mod can run against the simulator.

Addresses and struct layouts match re2mod/src/re2_addr.h. Regions the game keeps inside its
own structures (G work, message window, status/title work, flag table) are written at their
real addresses; data the game keeps elsewhere (script AOT records, enemy works, flag words,
message text) is written at made-up addresses that the real structures point to, just as the
game's pointers would.
"""
from __future__ import annotations

import struct

import os
from .rdt import MSG_TABLE

# Parts of bio2.exe's data the mod reads directly (text tables for the Extreme Battle typed
# pages and the stage clear screen). Loaded once from the game data's bio2.exe.
EXE_RANGES = [(0x525200, 0x525500), (0x52CB00, 0x52CE00), (0x533100, 0x5331D0)]
_exe_cache = {}


def exe_data(gamedata):
    if gamedata in _exe_cache:
        return _exe_cache[gamedata]
    out = {}
    path = os.path.join(gamedata or "", "bio2.exe")
    try:
        d = open(path, "rb").read()
        pe = struct.unpack_from("<I", d, 0x3C)[0]
        n = struct.unpack_from("<H", d, pe + 6)[0]
        opt = struct.unpack_from("<H", d, pe + 20)[0]
        secs = []
        for i in range(n):
            o = pe + 24 + opt + 40 * i
            vs, va, rs, ro = struct.unpack_from("<IIII", d, o + 8)
            secs.append((va + 0x400000, rs, ro))
        for lo, hi in EXE_RANGES:
            for va, rs, ro in secs:
                if va <= lo and hi <= va + rs:
                    out[lo] = d[ro + lo - va:ro + hi - va]
    except OSError:
        pass
    _exe_cache[gamedata] = out
    return out
from .scd import FG

G_LO, G_HI = 0x988600, 0x98F200          # covers KEY .. item box
MOJI_LO, MOJI_HI = 0x985040, 0x9850C0     # message window fields
STATUS_LO, STATUS_HI = 0x691B80, 0x692000  # title work + status work
FLAG_TABLE = 0x53B218
AOT_BASE = 0x11000000
ENEMY_BASE = 0x12000000
FLAGS_BASE = 0x13000000
TEXT_BASE = 0x14000000
ROOM_IMAGE = 0x15000000   # the loaded RDT: header pointers (+0x40 = English messages), like [0x98861C]
MSG_CHUNK = 0x15100000
SCA_LIVE = 0x15200000      # the live collision table (scripts change entry flags with sca_id_set)
OBJ_WORK, OBJ_SIZE = 0x98A61C, 0x1F8

PAD = {"up": 0x1000, "down": 0x4000, "left": 0x8000, "right": 0x2000, "aim": 0x08, "action": 0x80, "run": 0x44}

_CHAR = {}
for _i, _c in enumerate(MSG_TABLE):
    if _c != "_" and _c not in _CHAR:
        _CHAR[_c] = _i


def encode_text(text: str, yesno: bool) -> bytes:
    out = bytearray([0xFA, 0x00])
    body = text.replace(" [Yes/No]", "")
    for ch in body:
        if ch in _CHAR:
            out.append(_CHAR[ch])
        elif ch in "’":
            out.append(_CHAR["'"])
    if yesno:
        out += bytes([0xFB, 0x00])
    out += bytes([0xFE, 0x00])
    return bytes(out)


class MemoryImage:
    def __init__(self):
        self.g = bytearray(G_HI - G_LO)
        self.moji = bytearray(MOJI_HI - MOJI_LO)
        self.status = bytearray(STATUS_HI - STATUS_LO)
        self.extra = {}            # addr -> bytes
        self.text_ptr = 0

    # --- writers ---------------------------------------------------------------------------
    def _w(self, buf, lo, addr, fmt, val):
        struct.pack_into(fmt, buf, addr - lo, val)

    def g_u8(self, a, v): self._w(self.g, G_LO, a, "<B", v & 0xFF)
    def g_s16(self, a, v): self._w(self.g, G_LO, a, "<h", int(v))
    def g_u16(self, a, v): self._w(self.g, G_LO, a, "<H", int(v) & 0xFFFF)
    def g_s32(self, a, v): self._w(self.g, G_LO, a, "<i", int(v))
    def g_u32(self, a, v): self._w(self.g, G_LO, a, "<I", int(v) & 0xFFFFFFFF)

    def regions(self):
        out = [(G_LO, bytes(self.g)), (MOJI_LO, bytes(self.moji)), (STATUS_LO, bytes(self.status))]
        out += sorted(self.extra.items())
        return out


def build(game, ui=None, pad=0) -> MemoryImage:
    """ui: optional dict with 'title' / 'status' / 'save_cursor' state from the menu model."""
    m = MemoryImage()
    in_game = ui is None or ui.get("in_game", True)
    m.g_u32(0x988604, pad)                                  # KEY
    sysf = 0x40000000 if in_game else 0
    if ui and ui.get("save_screen"):
        sysf |= 0x00040000
    if ui and ui.get("load_screen"):
        sysf |= 0x00020000
    if ui and ui.get("title", {}).get("tofu"):
        sysf |= 0x1                                         # To-fu mode (4th Survivor title picture)
    sysf |= (ui or {}).get("sys_or", 0)                     # extra System_flg bits ('ui sys HEX')
    m.g_u32(0x989E6C, sysf)                                 # System_flg
    stop = 1 if (game.player_locked or game.message is not None) else 0
    m.g_u32(0x989ED4, stop)                                 # Stop_flg
    claire_bit = 0x80000000 if (ui and ui.get("title", {}).get("claire")) else 0
    m.g_u32(0x989ED0, (0x40 if game.message is not None else 0) | claire_bit)
    m.g_u8(0x989E7C, (ui or {}).get("save_cursor", 0))
    room = game.room
    if room is not None:
        m.g_s16(0x98EB14, room.stage - 1)
        m.g_s16(0x98EB16, room.room)
        m.g_s16(0x98EB18, game.cut)
    p = game.player
    m.g_u8(0x98E9A6, p.character)
    P = 0x989EF0
    m.g_u32(P + 0x00, 1)
    m.g_u8(P + 0x08, getattr(p, "model", p.character))    # W_ID: the player's model (0 Leon, 1 Claire)
    m.g_u16(0x98A108, getattr(game, "poison", 0))         # poison timer (non-zero while poisoned)
    m.g_s32(P + 0x38, round(p.x)); m.g_s32(P + 0x3C, round(p.y)); m.g_s32(P + 0x40, round(p.z))
    m.g_s16(P + 0x76, p.d % 4096)
    m.g_u8(P + 0x106, p.floor)
    m.g_s16(P + 0x156, p.hp if p.hp > 0 else -1)
    m.g_s16(P + 0x162, p.max_hp)
    # inventory
    slots = 10 if getattr(game, "max_items", 8) == 10 else 8
    m.g_u8(0x98E9A4, slots)
    for i in range(11):
        iid, n = (game.inventory[i] if i < len(game.inventory) else (0, 0))
        struct.pack_into("<BBBB", m.g, 0x98ED34 + i * 4 - G_LO, iid & 0xFF, min(255, n), 0, 0)
    struct.pack_into("<B", m.status, 0x691F6A - STATUS_LO, (game.equipped or 0) & 0xFF)
    m.g_u32(0x98ED20, (ui or {}).get("file_flags", 0) | getattr(game, "file_flags", 0))
    # script variables (s16 array at 0x98EAE4: var 0 = AOT used, 2 = item used, 26 = camera...)
    for i, v in enumerate(list(getattr(game, "vars", []))[:64]):
        m.g_s16(0x98EAE4 + 2 * i, ((int(v) + 0x8000) & 0xFFFF) - 0x8000)
    # item box (64 slots)
    for i, (iid, n) in enumerate(getattr(game, "item_box", [])[:64]):
        struct.pack_into("<BBBB", m.g, 0x98ED60 + i * 4 - G_LO, iid & 0xFF, min(255, n), 0, 0)
    # AOTs: SCE_AOT[id] -> record laid out like the script instruction from byte 2
    n_aot = 0
    for aid, a in game.aots.items():
        if not (0 <= aid < 32):
            continue
        rec = bytearray(48)
        sat = a.sat | (0x80 if len(a.quad) == 4 and not _is_rect(a.quad) else 0)
        sup = getattr(a, "sup", 0) & 0xFF if getattr(a, "base", None) else 0
        quad = a.base if sup & 0x80 else a.quad      # attached areas are stored relative to their object
        rec[0], rec[1], rec[2], rec[3] = a.sce & 0xFF, sat & 0xFF, a.floor & 0xFF, sup
        if sat & 0x80:
            for k, (x, z) in enumerate(quad[:4]):
                struct.pack_into("<hh", rec, 4 + k * 4, int(x), int(z))
            data_off = 20
        else:
            xs = [q[0] for q in quad]; zs = [q[1] for q in quad]
            struct.pack_into("<hhHH", rec, 4, int(min(xs)), int(min(zs)), int(max(xs) - min(xs)), int(max(zs) - min(zs)))
            data_off = 12
        if a.door and a.sce == 1:
            d = a.door
            struct.pack_into("<hhhh", rec, data_off, int(d["next_x"]), int(d["next_y"]), int(d["next_z"]), int(d["next_d"]))
            struct.pack_into("<BBBBBBBBB", rec, data_off + 8, (d["stage"] - 1) & 0xFF, d["room"] & 0xFF, d["cut"] & 0xFF,
                             d["next_floor"] & 0xFF, d["texture"] & 0xFF, d["animation"] & 0xFF, d["sound"] & 0xFF,
                             d["lock_id"] & 0xFF, d["lock_type"] & 0xFF)
        elif a.item and a.sce in (2, 5):
            it = a.item
            struct.pack_into("<HHH", rec, data_off, it["item"], it["amount"], it["gid"])
        else:
            for k, v in enumerate(tuple(a.data)[:6]):
                rec[data_off + k] = int(v) & 0xFF
        addr = AOT_BASE + aid * 64
        m.extra[addr] = bytes(rec)
        m.g_u32(0x988850 + aid * 4, addr)
        n_aot += 1
    m.g_u8(0x98E528, n_aot)
    # the following partner: pEnemy[1]
    f = getattr(game, "follower", None)
    if f:
        w = bytearray(0x170)
        struct.pack_into("<I", w, 0, 1)
        w[8] = 0x45 if game.player.character == 1 else 0x48
        struct.pack_into("<iii", w, 0x38, round(f["x"]), round(f["y"]), round(f["z"]))
        struct.pack_into("<hh", w, 0x156, 9999, 0)
        addr = ENEMY_BASE + 1 * 0x200
        m.extra[addr] = bytes(w)
        m.g_u32(0x98A10C + 4, addr)
    # enemies: pEnemy[2..] -> enemy works
    slot = 2
    for e in game.enemies.values():
        if slot >= 34:
            break
        w = bytearray(0x170)
        struct.pack_into("<I", w, 0, 1)
        w[8] = e.type & 0xFF
        struct.pack_into("<iii", w, 0x38, round(e.x), round(e.y), round(e.z))
        struct.pack_into("<h", w, 0x76, int(e.d) % 4096)
        w[0x106] = e.floor & 0xFF
        pose = e.members.get(7, ((e.ai & 0xFF) << 8) | (e.state & 0xFF)) & 0x7FFF
        if not getattr(e, "active", True):
            pose |= 0x8000
        struct.pack_into("<H", w, 0x10E, pose & 0xFFFF)
        hp = e.hp if (e.alive and getattr(e, "hostile", True)) else (-1 if not e.alive else 9999)
        struct.pack_into("<hh", w, 0x156, max(-1, min(32767, hp)), 0)
        struct.pack_into("<h", w, 0x162, min(32767, e.max_hp))
        addr = ENEMY_BASE + slot * 0x200
        m.extra[addr] = bytes(w)
        m.g_u32(0x98A10C + slot * 4, addr)
        slot += 1
    # script flags: table of group pointers -> 8 words, MSB-first bits
    table = bytearray(64 * 4)
    for grp in range(64):
        addr = FLAGS_BASE + grp * 32
        struct.pack_into("<I", table, grp * 4, addr)
        words = [0] * 8
        for idx in game.flags.get(grp, set()):
            if 0 <= idx < 256:
                words[idx >> 5] |= 0x80000000 >> (idx & 31)
        m.extra[addr] = struct.pack("<8I", *words)
    m.extra[FLAG_TABLE] = bytes(table)
    # the room image: header pointer to the English message table, and the table itself
    chunk = getattr(game, "msg_chunk", None)
    if room is not None and chunk:
        hdr = bytearray(0x60)
        struct.pack_into("<I", hdr, 0x40, MSG_CHUNK)
        cols = getattr(game, "collisions", None) or []
        if cols:
            struct.pack_into("<I", hdr, 0x20, SCA_LIVE)
            sca = bytearray(16 + 16 * len(cols))
            struct.pack_into("<I", sca, 4, len(cols) + 1)
            for i, c in enumerate(cols):
                struct.pack_into("<hhHHHHI", sca, 16 + 16 * i, c.x, c.z, c.w, c.d, c.raw_flag & 0xFFFF,
                                 c.raw_type & 0xFFFF, c.floor_mask & 0xFFFFFFFF)
            m.extra[SCA_LIVE] = bytes(sca)
        m.extra[ROOM_IMAGE] = bytes(hdr)
        m.extra[MSG_CHUNK] = bytes(chunk)
        m.g_u32(0x98861C, ROOM_IMAGE)
    # room objects
    objs = getattr(game, "objects", {})
    if objs:
        ow = m.g                        # OBJECT_WORK lies inside the G region
        for idx, ob in objs.items():
            if not (0 <= idx < 32):
                continue
            b = OBJ_WORK - G_LO + idx * OBJ_SIZE
            struct.pack_into("<I", ow, b, 1 | (getattr(ob, "be", 0) & 0xFFFE))
            ox, oy, oz = getattr(ob, "ofs", (0, 0, 0))
            sw, sh, sd = getattr(ob, "size", (0, 0, 0))
            struct.pack_into("<hhhHHH", ow, b + 0x94, ox, oz, oy, sw, sd, sh)
            ow[b + 0x0B] = ob.aot & 0xFF
            struct.pack_into("<iii", ow, b + 0x38, round(ob.x), round(ob.y), round(ob.z))
            struct.pack_into("<h", ow, b + 0x76, int(ob.d) % 4096)
            ow[b + 0x106] = ob.floor & 0xFF
    # message window
    msg = game.message
    if msg is not None:
        enc = encode_text(msg.text, msg.yesno)
        m.extra[TEXT_BASE] = enc
        text_ptr = TEXT_BASE + 2
        if getattr(msg, "msg_id", -1) >= 0 and chunk:
            off = game.room_message_offset(msg.msg_id)
            if off is not None:
                text_ptr = MSG_CHUNK + off + 2     # room messages are read from the room image
        cursor = getattr(msg, "cursor", 0)
        struct.pack_into("<B", m.moji, 0x98504C - MOJI_LO, 0x80 | (cursor & 0x0F))
        # routine: 1 typing a page, 2 waiting for action at the end of a page, 4 yes/no choice
        last = getattr(msg, "page", 0) >= getattr(msg, "pages", 1) - 1
        if getattr(msg, "page_frames", 9) < 3:
            rno = 1
        elif not last:
            rno = 2
        else:
            rno = 4 if msg.yesno else 2
        struct.pack_into("<B", m.moji, 0x98504D - MOJI_LO, rno)
        struct.pack_into("<I", m.moji, 0x985054 - MOJI_LO, text_ptr)   # typing has begun
        struct.pack_into("<B", m.moji, 0x98504F - MOJI_LO, getattr(msg, "item", 0) & 0xFF)
    # menus (title / status / item box), from the menu model
    if ui:
        t = ui.get("title")
        if t:
            for off, key in ((0, "main_rtn"), (1, "sub_rtn0"), (2, "screen"), (3, "step"), (4, "sub4"), (6, "sel_mode"),
                             (7, "cursor"), (0x0E, "option")):
                m.status[0x691B80 + off - STATUS_LO] = t.get(key, 0) & 0xFF
            m.extra[0x6805B0] = bytes([1 if t.get("hard_ok") else 0])   # Hard difficulty unlocked
            # menu-word brightness (the special menu highlights by brightness)
            for i, lvl in (t.get("sprites") or {}).items():
                m.status[0x691BB8 + i * 0x14 + 4 - STATUS_LO] = lvl & 0xFF
        s = ui.get("status")
        if s:
            for off, key in ((0x00, "main_rtn"), (0x01, "sub_rtn0"), (0x02, "item_state"), (0x03, "item_cmd"),
                             (0x04, "item_step"), (0x0C, "cursor"), (0x0D, "cursor_sub"), (0x0E, "menu0"),
                             (0x0F, "menu1"), (0x10, "type"), (0x12, "file_sel"), (0x13, "file_page"),
                             (0x24, "ibox_sel")):
                m.status[0x691F60 + off - STATUS_LO] = s.get(key, 0) & 0xFF
            m.extra[0x689DF4] = bytes([1 if s.get("weapon_menu") else 0])
    # results screens after an ending (bio2.exe 0x4B69B0 table, 0x4B6E10 values)
    res = (ui or {}).get("results")
    tab = bytearray(33 * 12)
    if res:
        for i in res.get("shown", ()):
            tab[i * 12] = 0x01
        h, mi, sec = res.get("time", (0, 0, 0))
        m.extra[0x99CE20] = struct.pack("<HBBBB", res.get("rank", 0), h, mi, sec, res.get("saves", 0))
    m.extra[0x52D178] = bytes(tab)
    # the PC options menu (bio2.exe 0x44A0B0): runs while System_flg 0x08000000 is set
    o = ui.get("options") if ui else None
    st = bytearray(1)
    if o:
        m.g_u32(0x989E6C, sysf | 0x08000000)
        st[0] = o["state"] & 0xFF
        m.extra[0x99CF68] = struct.pack("<i", o["top"])
        m.extra[0x68915C] = struct.pack("<i", o["row"])
        m.extra[0x99CF40] = bytes(k & 0xFF for k in o["keys"])
        m.extra[0x6893C8] = bytes([o["choice"] & 0xFF])
        m.extra[0x669CFC] = struct.pack("<i", o["speaker"])
        m.extra[0x669F40] = struct.pack("<i", o["bits"])
        m.extra[0x669D9C] = struct.pack("<i", o["rate"])
        m.extra[0x693471] = bytes([o["bgm"] & 0xFF])
        m.extra[0x693C48] = bytes([o["se"] & 0xFF])
    m.extra[0x99D001] = bytes(st)
    # PC save / load screen cursor (bio2.exe 0x986390 top row, 0x986394 rows, 0x9864B4 row, 0x98638C choice)
    sv = bytearray(0x9864C0 - 0x986380)
    if ui and ui.get("save"):
        v = ui["save"]
        struct.pack_into("<i", sv, 0x986390 - 0x986380, v.get("save_top", 0))
        struct.pack_into("<i", sv, 0x986394 - 0x986380, v.get("save_rows", 10))
        struct.pack_into("<i", sv, 0x9864B4 - 0x986380, v.get("save_sel", 0))
        sv[0x98638C - 0x986380] = v.get("save_dsel", 0) & 0xFF
    m.extra[0x986380] = bytes(sv)
    # computer terminal: work pointer, state, text grid
    t = ui.get("terminal") if ui else None
    wp = bytearray(4)
    if t:
        struct.pack_into("<I", wp, 0, 0x13800000)
        m.extra[0x13800000] = t["grid"] if len(t["grid"]) > 360 else bytes([0, 0, 0, 0]) + t["grid"]
        m.g_u8(0x98E52D, t.get("state", 1))
        m.g_u32(0x989ED4, 0x01000000)   # play is frozen while the terminal is up
    m.extra[0x9800A0] = bytes(wp)
    # raw memory set by 'ui poke ADDR VALUE [SIZE]' (for screens the simulator does not model)
    for a, b in ((ui or {}).get("poke") or {}).items():
        for lo, hi, buf in ((G_LO, G_HI, m.g), (STATUS_LO, STATUS_HI, m.status), (MOJI_LO, MOJI_HI, m.moji)):
            if lo <= a and a + len(b) <= hi:
                buf[a - lo:a - lo + len(b)] = b
                break
        else:
            m.extra[a] = b
    for a, b in exe_data(getattr(getattr(game, "lib", None), "gamedata", None)).items():
        m.extra.setdefault(a, b)
    return m


def _is_rect(q):
    xs = sorted({x for x, z in q}); zs = sorted({z for x, z in q})
    return len(xs) <= 2 and len(zs) <= 2


def pad_bits(keys) -> int:
    v = 0
    for k in keys:
        v |= PAD.get(k, 0)
    return v
