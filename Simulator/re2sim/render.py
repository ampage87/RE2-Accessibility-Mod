"""Screenshots of the simulated game.

Draws what the player would see: the real pre-rendered background for the current fixed
camera (from Common/bin/ROOMCUT.BIN), with the simulator's state projected into it through
that camera (player, enemies, items, doors, triggers, walls). A top-down map of the room is
drawn beside it.

Pure standard library: ADT decompression is ported from OpenBiohazard2 (MIT, Samuel Yuan),
PNG is written with zlib. Text labels are drawn when Pillow is installed; without Pillow the
picture is the same but unlabelled.
"""
import math
import os
import struct
import zlib

from .world import facing_vector

W, H = 320, 240          # game screen
MAP_W = 320              # top-down panel width


# ---------------------------------------------------------------- ADT (background images)
class _Bits:
    __slots__ = ("d", "p", "b", "n")

    def __init__(self, data, pos):
        self.d = data; self.p = pos; self.b = 0; self.n = 0

    def bit(self):
        if self.n == 0:
            self.b = self.d[self.p] if self.p < len(self.d) else 0
            self.p += 1; self.n = 8
        self.n -= 1
        return (self.b >> self.n) & 1

    def bits(self, k):
        v = 0
        for _ in range(k):
            v = (v << 1) | self.bit()
        return v


class _Tree:
    """Canonical Huffman tree as built by the game's ADT unpacker."""

    def __init__(self, lengths):
        n = len(lengths)
        self.n = n
        freq = [0] * 17
        for L in lengths:
            if L <= 16:
                freq[L] += 1
        tmp = [0] * 18
        for i in range(16):
            tmp[i + 2] = ((tmp[i + 1] + freq[i + 1]) << 1) & 0xFFFF
        start = [-1] * n
        for i in range(18):
            for j in range(n):
                if lengths[j] == i:
                    start[j] = tmp[i] & 0xFFFF
                    tmp[i] += 1
        left = [-1] * (n * 2); right = [-1] * (n * 2)
        nxt = n + 1
        for i in range(n):
            L = lengths[i]; s = start[i]; cur = n
            for j in range(L):
                side = (s >> (L - j - 1)) & 1
                arr = right if side else left
                if j + 1 == L:
                    arr[cur] = i
                    break
                if arr[cur] == -1:
                    arr[cur] = nxt; left[nxt] = -1; right[nxt] = -1
                    cur = nxt; nxt += 1
                else:
                    cur = arr[cur]
        self.left, self.right = left, right

    def read(self, br):
        cur = self.n
        left, right, n = self.left, self.right, self.n
        while True:
            cur = right[cur] if br.bit() else left[cur]
            if cur < n:
                return cur


def _binnum(br):
    z = 0
    while br.bit() == 0:
        z += 1
    v = 1
    for _ in range(z):
        v = (v << 1) | br.bit()
    return v


def _small_lengths(br):
    out = []; prev = 0
    for _ in range(16):
        if br.bit():
            prev ^= _binnum(br)
        out.append(prev)
    return out


def adt_unpack(data: bytes, start: int = 0) -> bytes:
    br = _Bits(data, start + 4)          # skip the first uint32
    out = bytearray()
    win = bytearray(16384); wp = 0
    limit = 320 * 256 * 2 + 0x20000
    while len(out) < limit:
        lo = br.bits(8); hi = br.bits(8)
        blen = lo | (hi << 8)
        if blen == 0:
            break
        t1 = _Tree(_small_lengths(br))
        tmp = [0] * 512
        cur = br.bit(); j = 0
        while j < 512:
            cnt = _binnum(br)
            if cur == 1:
                for i in range(cnt):
                    if j + i < 512:
                        tmp[j + i] = t1.read(br)
                cur = 0
            else:
                cur = 1
            j += cnt
        lens = []; x = 0
        for i in range(512):
            x ^= tmp[i]; lens.append(x)
        t2 = _Tree(lens)
        t3 = _Tree(_small_lengths(br))
        for _ in range(blen):
            v = t2.read(br)
            if v < 256:
                out.append(v); win[wp] = v; wp = (wp + 1) & 0x3FFF
            else:
                cnt = v - 0xFD
                dist = t3.read(br)
                if dist != 0:
                    nb = dist - 1
                    dist = (br.bits(nb) & 0xFFFF) + (1 << nb)
                sp = (wp - dist - 1) & 0x3FFF
                for _ in range(cnt):
                    c = win[sp]; win[wp] = c; out.append(c)
                    sp = (sp + 1) & 0x3FFF; wp = (wp + 1) & 0x3FFF
    return bytes(out)


def adt_to_rgb(raw: bytes) -> bytearray:
    """The unpacked data is a 256-wide 16-bit picture; the right-hand 64 columns of the
    320x240 screen are stored interleaved below it. Returns 320x240 RGB bytes."""
    px = struct.unpack_from(f"<{len(raw) // 2}H", raw)
    rgb = bytearray(W * H * 3)

    def put(x, y, c):
        o = (y * W + x) * 3
        rgb[o] = (c & 31) << 3; rgb[o + 1] = ((c >> 5) & 31) << 3; rgb[o + 2] = ((c >> 10) & 31) << 3

    for y in range(H):
        row = 256 * y
        for x in range(256):
            put(x, y, px[row + x])
    oy = 256
    for y in range(0, 128, 2):
        for ox in range(64):
            put(256 + ox, y, px[ox + 256 * oy])
            put(256 + ox, y + 1, px[128 + ox + 256 * oy])
        oy += 1
    oy = 256
    for y in range(128, H, 2):
        for ox in range(64):
            put(256 + ox, y, px[64 + ox + 256 * oy])
            put(256 + ox, y + 1, px[192 + ox + 256 * oy])
        oy += 1
    return rgb


class RoomCut:
    """Common/bin/ROOMCUT.BIN: one background per (stage, room, camera)."""

    def __init__(self, path):
        self.path = path
        with open(path, "rb") as f:
            first = struct.unpack("<I", f.read(4))[0]
            f.seek(0)
            self.offs = list(struct.unpack(f"<{first // 4}I", f.read(first)))
        self.size = os.path.getsize(path)
        self._cache = {}
        cache_dir = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", ".cache", "backgrounds")
        self.cache_dir = os.path.abspath(cache_dir)

    @staticmethod
    def index(stage, room, cam):
        return (stage - 1) * 512 + room * 16 + cam

    def background(self, stage, room, cam):
        i = self.index(stage, room, cam)
        if i in self._cache:
            return self._cache[i]
        cf = os.path.join(self.cache_dir, f"{i:05d}.rgb")
        if os.path.exists(cf):
            rgb = bytearray(open(cf, "rb").read())
        else:
            if i >= len(self.offs) or self.offs[i] == 0:
                return None
            o = self.offs[i]
            nxt = [x for x in self.offs if x > o]
            end = min(nxt) if nxt else self.size
            with open(self.path, "rb") as f:
                f.seek(o); data = f.read(end - o)
            raw = adt_unpack(data)
            if len(raw) < 320 * 256 * 2:
                return None
            rgb = adt_to_rgb(raw)
            try:
                os.makedirs(self.cache_dir, exist_ok=True)
                open(cf, "wb").write(bytes(rgb))
            except OSError:
                pass
        self._cache[i] = rgb
        return rgb


def find_roomcut(gamedata):
    here = os.path.dirname(os.path.abspath(__file__))
    for p in (os.path.join(gamedata, "Common", "bin", "ROOMCUT.BIN"),
              os.path.join(gamedata, "Common", "bin", "roomcut.bin"),
              os.path.join(here, "..", "gamedata-rebirth", "Common", "bin", "roomcut.bin"),
              os.path.join(here, "..", "gamedata", "Common", "bin", "ROOMCUT.BIN"),
              os.path.join(here, "..", "..", "Game Files", "rebirth", "Common", "bin", "roomcut.bin")):
        if os.path.exists(p):
            return os.path.abspath(p)
    return None


# ---------------------------------------------------------------- camera projection
class Projector:
    """Projects world points through an RDT camera onto the 320x240 screen."""

    def __init__(self, cam):
        fx, fy, fz = cam.frm
        tx, ty, tz = cam.to
        self.eye = (fx, fy, fz)
        f = (tx - fx, ty - fy, tz - fz)
        n = math.sqrt(sum(c * c for c in f)) or 1.0
        f = tuple(c / n for c in f)
        up = (0.0, -1.0, 0.0)                       # the game's y axis points down
        s = (f[1] * up[2] - f[2] * up[1], f[2] * up[0] - f[0] * up[2], f[0] * up[1] - f[1] * up[0])
        n = math.sqrt(sum(c * c for c in s)) or 1.0
        s = tuple(c / n for c in s)
        u = (s[1] * f[2] - s[2] * f[1], s[2] * f[0] - s[0] * f[2], s[0] * f[1] - s[1] * f[0])
        self.f, self.s, self.u = f, s, u
        self.focal = (cam.dist >> 7) or 1

    def project(self, x, y, z):
        d = (x - self.eye[0], y - self.eye[1], z - self.eye[2])
        zc = sum(a * b for a, b in zip(d, self.f))
        if zc <= 50:
            return None
        xc = sum(a * b for a, b in zip(d, self.s))
        yc = sum(a * b for a, b in zip(d, self.u))
        return (W / 2 + self.focal * xc / zc, H / 2 - self.focal * yc / zc)


# ---------------------------------------------------------------- simple raster canvas
class Canvas:
    def __init__(self, w, h, rgb=None, fill=(0, 0, 0)):
        self.w, self.h = w, h
        self.px = bytearray(rgb) if rgb is not None else bytearray(bytes(fill) * (w * h))
        self.labels = []          # (x, y, text, colour), drawn by Pillow if available

    def dot(self, x, y, c):
        x = int(x); y = int(y)
        if 0 <= x < self.w and 0 <= y < self.h:
            o = (y * self.w + x) * 3
            self.px[o:o + 3] = bytes(c)

    def line(self, a, b, c, width=1):
        if a is None or b is None:
            return
        (x0, y0), (x1, y1) = a, b
        n = int(max(abs(x1 - x0), abs(y1 - y0))) + 1
        if n > 4000:
            return
        for i in range(n + 1):
            t = i / n
            x = x0 + (x1 - x0) * t; y = y0 + (y1 - y0) * t
            for dx in range(width):
                for dy in range(width):
                    self.dot(x + dx - width // 2, y + dy - width // 2, c)

    def poly(self, pts, c, width=1):
        for i in range(len(pts)):
            self.line(pts[i], pts[(i + 1) % len(pts)], c, width)

    def disc(self, x, y, r, c):
        for dy in range(-r, r + 1):
            for dx in range(-r, r + 1):
                if dx * dx + dy * dy <= r * r:
                    self.dot(x + dx, y + dy, c)

    def ring(self, x, y, r, c):
        for i in range(max(12, r * 6)):
            a = 2 * math.pi * i / max(12, r * 6)
            self.dot(x + r * math.cos(a), y + r * math.sin(a), c)

    def label(self, x, y, text, c):
        self.labels.append((x, y, text, c))

    def paste(self, other, ox, oy):
        for y in range(other.h):
            if 0 <= y + oy < self.h:
                s = y * other.w * 3
                d = ((y + oy) * self.w + ox) * 3
                n = min(other.w, self.w - ox) * 3
                self.px[d:d + n] = other.px[s:s + n]
        self.labels += [(x + ox, y + oy, t, c) for x, y, t, c in other.labels]

    def scaled(self, k):
        out = Canvas(self.w * k, self.h * k)
        for y in range(self.h):
            row = bytearray()
            for x in range(self.w):
                o = (y * self.w + x) * 3
                row += self.px[o:o + 3] * k
            for j in range(k):
                d = ((y * k + j) * out.w) * 3
                out.px[d:d + len(row)] = row
        out.labels = [(x * k, y * k, t, c) for x, y, t, c in self.labels]
        return out

    def save_png(self, path):
        try:
            from PIL import Image, ImageDraw
            img = Image.frombytes("RGB", (self.w, self.h), bytes(self.px))
            dr = ImageDraw.Draw(img)
            for x, y, t, c in self.labels:
                dr.text((x + 1, y + 1), t, fill=(0, 0, 0))
                dr.text((x, y), t, fill=tuple(c))
            img.save(path)
            return
        except ImportError:
            pass
        raw = b"".join(b"\x00" + bytes(self.px[y * self.w * 3:(y + 1) * self.w * 3]) for y in range(self.h))

        def chunk(t, d):
            return struct.pack(">I", len(d)) + t + d + struct.pack(">I", zlib.crc32(t + d) & 0xFFFFFFFF)
        png = b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", self.w, self.h, 8, 2, 0, 0, 0))
        png += chunk(b"IDAT", zlib.compress(raw, 6)) + chunk(b"IEND", b"")
        with open(path, "wb") as f:
            f.write(png)


# ---------------------------------------------------------------- the screenshot
COL = {
    "player": (40, 255, 80), "enemy": (255, 50, 50), "enemy_dead": (120, 40, 40),
    "door": (60, 200, 255), "item": (255, 230, 40), "event": (255, 80, 255),
    "other": (200, 200, 200), "wall": (255, 140, 0), "path": (120, 255, 255),
}


def _aot_colour(kind):
    return COL["door"] if kind == "door" else COL["item"] if kind == "item" else \
        COL["event"] if kind in ("event", "message", "flag_chg", "auto") else COL["other"]


def _collision_outline(c):
    if c.shape in (6, 7, 8):
        cx, cz = c.x + c.w / 2, c.z + c.d / 2
        return [(cx + c.w / 2 * math.cos(a * math.pi / 8), cz + c.d / 2 * math.sin(a * math.pi / 8)) for a in range(16)]
    return [(c.x, c.z), (c.x + c.w, c.z), (c.x + c.w, c.z + c.d), (c.x, c.z + c.d)]


CAPTION_H = 64


def screenshot(game, mod=None, path="shot.png", scale=2, walls=True, labels=True, caption=None, clean=False):
    """Saves the current view (game camera + overlays) and a top-down map side by side.
    caption: optional list of text lines (recent speech) shown in a strip underneath,
    together with any on-screen message."""
    from .world import PLAYER_BLOCK_BIT
    room = game.room
    gamedata = game.lib.gamedata
    rc = getattr(game, "_roomcut", None)
    if rc is None:
        p = find_roomcut(gamedata)
        rc = RoomCut(p) if p else False
        game._roomcut = rc
    bg = rc.background(room.stage, room.room, game.cut) if rc else None
    view = Canvas(W, H, bg) if bg else Canvas(W, H, fill=(30, 30, 40))
    if not bg:
        view.label(8, 8, "no background image for this camera", (255, 255, 255))
    cam = room.cameras[game.cut] if game.cut < len(room.cameras) else None
    pl = game.player
    floor_y = pl.y
    pr = Projector(cam) if cam else None

    def P(x, z, y=None):
        return pr.project(x, floor_y if y is None else y, z) if pr else None

    aots = mod.v.aots() if mod else []
    hidden = set()
    if mod is not None and hasattr(mod, "_hidden"):
        try:
            hidden = {a.id for a in aots if mod._hidden(a)}
        except Exception:
            hidden = set()
    aots = [a for a in aots if a.id not in hidden]
    if clean:          # only what a sighted player would care about: exits and pick-ups
        aots = [a for a in aots if a.kind in ("door", "item")]

    # --- camera view
    if pr:
        if walls:
            for c in game.collisions:
                if c.on_floor(pl.floor) and (c.raw_flag & PLAYER_BLOCK_BIT) and not (c.raw_flag & 0x100) and c.shape not in (4, 5, 13, 14, 15):
                    view.poly([P(x, z) for x, z in _collision_outline(c)], COL["wall"])
        for a in aots:
            q = [P(x, z) for x, z in a.quad]
            if all(q):
                view.poly(q, _aot_colour(a.kind), 2)
                cx, cz = a.centre
                pc = P(cx, cz)
                if labels and pc:
                    view.label(pc[0] - 10, pc[1] - 12, _aot_label(a, mod), _aot_colour(a.kind))
        for e in game.enemies.values():
            if mod and not mod.v.in_play(e):
                continue
            feet = P(e.x, e.z, e.y); head = P(e.x, e.z, e.y - 1700)
            colour = COL["enemy"] if e.alive else COL["enemy_dead"]
            if feet:
                view.line(feet, head, colour, 3); view.ring(feet[0], feet[1], 6, colour)
                if labels:
                    view.label(feet[0] + 6, feet[1] - 20, e.name, colour)
        feet = P(pl.x, pl.z, pl.y); head = P(pl.x, pl.z, pl.y - 1750)
        if feet:
            view.line(feet, head, COL["player"], 3)
            fx, fz = facing_vector(pl.d)
            view.line(feet, P(pl.x + fx * 900, pl.z + fz * 900, pl.y), COL["player"], 2)
            view.disc(feet[0], feet[1], 3, COL["player"])
    # --- top-down map
    mp = Canvas(MAP_W, H, fill=(18, 18, 24))
    xs = [c.x for c in room.collisions] + [c.x + c.w for c in room.collisions] + [pl.x]
    zs = [c.z for c in room.collisions] + [c.z + c.d for c in room.collisions] + [pl.z]
    if cam:
        xs.append(cam.frm[0]); zs.append(cam.frm[2])
    minx, maxx, minz, maxz = min(xs) - 500, max(xs) + 500, min(zs) - 500, max(zs) + 500
    k = min((MAP_W - 16) / max(1, maxx - minx), (H - 16) / max(1, maxz - minz))

    def M(x, z):          # north (+z) is up on the map
        return (8 + (x - minx) * k, H - 8 - (z - minz) * k)

    for c in game.collisions:
        if c.on_floor(pl.floor) and (c.raw_flag & PLAYER_BLOCK_BIT) and not (c.raw_flag & 0x100) and c.shape not in (4, 5, 13, 14, 15):
            mp.poly([M(x, z) for x, z in _collision_outline(c)], COL["wall"])
    for a in aots:
        mp.poly([M(x, z) for x, z in a.quad], _aot_colour(a.kind), 1)
        if labels:
            cx, cz = M(*a.centre); mp.label(cx + 3, cz - 5, _aot_label(a, mod, short=True), _aot_colour(a.kind))
    for e in game.enemies.values():
        if mod and not mod.v.in_play(e):
            continue
        ex, ez = M(e.x, e.z); mp.disc(ex, ez, 3, COL["enemy"] if e.alive else COL["enemy_dead"])
    if cam:
        cx, cz = M(cam.frm[0], cam.frm[2]); tx, tz = M(cam.to[0], cam.to[2])
        mp.ring(cx, cz, 4, (255, 255, 255)); mp.line((cx, cz), (tx, tz), (150, 150, 150))
        if labels:
            mp.label(cx + 5, cz - 5, f"cam {game.cut}", (255, 255, 255))
    aw = getattr(mod, "autowalk", None)
    route = [w for w in (aw if isinstance(aw, list) else []) if isinstance(w, tuple) and len(w) == 2]
    if route:
        pts = [M(pl.x, pl.z)] + [M(x, z) for x, z in route]
        for i in range(len(pts) - 1):
            mp.line(pts[i], pts[i + 1], COL["path"])
    px_, pz_ = M(pl.x, pl.z)
    fx, fz = facing_vector(pl.d)
    mp.disc(px_, pz_, 4, COL["player"]); mp.line((px_, pz_), M(pl.x + fx * 1200, pl.z + fz * 1200), COL["player"], 2)
    if labels:
        mp.label(6, 4, f"room {room.rid}  camera {game.cut}  floor {pl.floor}", (230, 230, 230))
        mp.label(6, H - 14, "N up. green you, red enemy, cyan door, yellow item, pink event, orange wall", (170, 170, 170))

    lines = []
    if game.message is not None:
        lines.append(("On screen: " + (game.message.text or "(blank message)") + ("  [yes/no]" if game.message.yesno else ""))[:110])
    lines += [str(t)[:110] for t in (caption or [])][-(4 - min(len(lines), 1)):]
    ch = CAPTION_H if lines else 0
    out = Canvas(W + MAP_W, H + ch, fill=(10, 10, 14))
    out.paste(view, 0, 0); out.paste(mp, W, 0)
    for y in range(H):
        out.dot(W, y, (90, 90, 90))
    for i, t in enumerate(lines):
        out.label(6, H + 4 + i * 14, t, (255, 255, 160) if t.startswith("On screen") else (220, 220, 220))
    if scale > 1:
        out = out.scaled(scale)
    out.save_png(path)
    return path


def _aot_label(a, mod, short=False):
    name = a.kind
    if mod is not None:
        try:
            name = mod.describe_aot(a, short=True)
        except Exception:
            pass
    if short:
        return f"{a.id}"
    return f"{a.id} {name}"[:28]
