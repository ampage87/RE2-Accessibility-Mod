"""Geometry helpers: directions, collision tests, AOT shapes and grid path-finding."""
from __future__ import annotations

import heapq
import math

DIR_FULL = 4096          # RE2 uses 0..4095 for a full turn
PLAYER_RADIUS = 450      # the player's collision half-width (bio2.exe sets +0x90/+0x92 = 0x1C2 for Leon and Claire)
UNITS_PER_METRE = 1000.0


def facing_vector(d: int) -> tuple:
    """Unit vector the character faces for RE2 direction d.
    Calibrated from 281 door transitions: forward = (cos t, -sin t), t = d/4096 * 2pi."""
    t = (d % DIR_FULL) / DIR_FULL * 2 * math.pi
    return math.cos(t), -math.sin(t)


def dir_towards(fx: float, fz: float, tx: float, tz: float) -> int:
    """RE2 direction value pointing from (fx,fz) to (tx,tz)."""
    t = math.atan2(-(tz - fz), tx - fx)
    return int(round(t / (2 * math.pi) * DIR_FULL)) % DIR_FULL


def angle_diff(a: int, b: int) -> int:
    """Signed smallest difference b - a in RE2 units (-2048..2047). Positive = clockwise (turn right)."""
    d = (b - a) % DIR_FULL
    if d >= DIR_FULL // 2:
        d -= DIR_FULL
    return d


def compass(d: int) -> str:
    """Compass name of a direction. North is +Z (map up), east is +X."""
    vx, vz = facing_vector(d)
    deg = (math.degrees(math.atan2(vx, vz)) + 360) % 360   # 0 = north (+z), 90 = east (+x)
    names = ["north", "north-east", "east", "south-east", "south", "south-west", "west", "north-west"]
    return names[int((deg + 22.5) // 45) % 8]


def relative_direction(player_dir: int, px: float, pz: float, tx: float, tz: float) -> str:
    """Spoken direction of a target relative to where the player faces."""
    want = dir_towards(px, pz, tx, tz)
    diff = angle_diff(player_dir, want)          # + = to the right
    deg = diff / DIR_FULL * 360
    a = abs(deg)
    if a <= 20:
        return "straight ahead"
    if a >= 160:
        return "behind you"
    side = "right" if deg > 0 else "left"
    if a <= 60:
        return f"ahead to your {side}"
    if a <= 120:
        return f"to your {side}"
    return f"behind you to the {side}"


def clock_position(player_dir: int, px: float, pz: float, tx: float, tz: float) -> int:
    want = dir_towards(px, pz, tx, tz)
    diff = angle_diff(player_dir, want) / DIR_FULL * 12
    h = int(round(diff)) % 12
    return 12 if h == 0 else h


def metres(dist_units: float) -> str:
    m = dist_units / UNITS_PER_METRE
    if m < 1:
        return "less than a metre"
    if m < 1.5:
        return "1 metre"
    return f"{int(round(m))} metres"


def point_in_quad(px, pz, quad) -> bool:
    """Point in convex/concave quad (ray casting)."""
    inside = False
    n = len(quad)
    j = n - 1
    for i in range(n):
        xi, zi = quad[i]
        xj, zj = quad[j]
        if ((zi > pz) != (zj > pz)) and (px < (xj - xi) * (pz - zi) / ((zj - zi) or 1e-9) + xi):
            inside = not inside
        j = i
    return inside


def quad_centre(quad):
    return sum(p[0] for p in quad) / len(quad), sum(p[1] for p in quad) / len(quad)


def rect_quad(x, z, w, d):
    # sizes are unsigned 16-bit in the game (704's aot 10 is 58400 wide, read signed as -7136)
    w, d = (w & 0xFFFF if w < 0 else w), (d & 0xFFFF if d < 0 else d)
    return [(x, z), (x + w, z), (x + w, z + d), (x, z + d)]


def contains_rounded(c, px, pz) -> bool:
    """Shapes 7/8 are boxes with rounded ends (capsules)."""
    x, z, w, d = c.x, c.z, c.w, c.d
    if c.shape == 7:      # rounded on the x axis: semicircles at left/right ends
        r = d / 2
        cz = z + r
        if x + r <= px <= x + w - r and z <= pz <= z + d:
            return True
        for cx in (x + r, x + w - r):
            if (px - cx) ** 2 + (pz - cz) ** 2 <= r * r:
                return True
        return False
    r = w / 2              # shape 8, rounded on the z axis
    cx = x + r
    if z + r <= pz <= z + d - r and x <= px <= x + w:
        return True
    for cz in (z + r, z + d - r):
        if (px - cx) ** 2 + (pz - cz) ** 2 <= r * r:
            return True
    return False


WALKABLE_SHAPES = {11, 12, 13, 14, 15}   # 11 slope, 12 stairs; 13-15 not understood yet (treated as walkable); 4 is the fourth half-box triangle, 5 a diamond (bio2.exe 0x4E14B0, 0x4E17B0)
INVERTED_SHAPES = set()                  # (tested: treating 4/5 as inverted room boundaries made navigation worse)
# Collision flag bits (bio2.exe 0x4CC730 / 0x4E03B0): an entry pushes an object out when its flag
# word has one of the object's "include" bits and none of the "exclude" bits. The player passes
# include 0x8000 (0x4C0486); zombies pass 0x400, other enemies 0x1000; everyone excludes 0x100.
PLAYER_BLOCK_BIT = 0x8000
EXCLUDE_BIT = 0x0100


def blocking_at(collisions, floor, px, pz, radius=PLAYER_RADIUS, extra=()):
    """Return the first collision entry that blocks a body of `radius` at (px,pz), else None.
    `floor` may be a tuple of floors (on stairs): then only collisions present on all of them block."""
    pts = [(px, pz)]
    for k in range(8):
        a = k * math.pi / 4
        pts.append((px + radius * math.cos(a), pz + radius * math.sin(a)))
    floors = floor if isinstance(floor, (tuple, list)) else (floor,)
    for c in list(collisions) + list(extra):
        if c.shape in WALKABLE_SHAPES:
            continue
        if not (c.raw_flag & PLAYER_BLOCK_BIT) or (c.raw_flag & EXCLUDE_BIT):
            continue
        if not all(c.on_floor(f) for f in floors):
            continue
        if stair_underside(c, collisions):
            continue
        if c.shape in INVERTED_SHAPES:
            for qx, qz in pts:
                if not _inside_boundary(c, qx, qz):
                    return c
            continue
        if px + radius < c.x or px - radius > c.x + c.w or pz + radius < c.z or pz - radius > c.z + c.d:
            continue
        for qx, qz in pts:
            inside = contains_rounded(c, qx, qz) if c.shape in (7, 8) else c.contains(qx, qz)
            if inside:
                return c
    return None


def stair_underside(c, collisions):
    """A box on one floor that covers an active stairway rising from that floor: the space under
    the stairs (room 111: the crank switches the stairs and this box on together). The game lets
    the player onto the stairs there, so it is not treated as a wall. (Needs real-game check.)"""
    if c.shape != 0 or not c.floor_mask or (c.floor_mask & (c.floor_mask - 1)):
        return False
    f = c.floor_mask.bit_length() - 1
    for st in collisions:
        if st.shape not in (11, 12) or not (st.raw_flag & PLAYER_BLOCK_BIT) or not st.floor_mask:
            continue
        low = (st.floor_mask & -st.floor_mask).bit_length() - 1
        if low != f:
            continue
        ox = min(c.x + c.w, st.x + st.w) - max(c.x, st.x)
        oz = min(c.z + c.d, st.z + st.d) - max(c.z, st.z)
        if ox > 0 and oz > 0 and ox * oz >= 0.9 * st.w * st.d:
            return True
    return False


def _blocks(c, floors, collisions=()):
    return (c.shape not in WALKABLE_SHAPES and (c.raw_flag & PLAYER_BLOCK_BIT) and not (c.raw_flag & EXCLUDE_BIT)
            and all(c.on_floor(f) for f in floors) and not stair_underside(c, collisions))


def push_out(collisions, floor, ox, oz, nx, nz, radius=PLAYER_RADIUS):
    """The game's collision response: move to (nx, nz), then push the body out of whatever it
    overlaps. Boxes (bio2.exe 0x4E0A00) treat the body as a square of half-width `radius` and
    push it back across the edge it came through, so walking into a wall at an angle slides
    along it. Round shapes push out radially. Returns the resolved position, or None when a
    shape that is not modelled (triangles) is in the way."""
    floors = floor if isinstance(floor, (tuple, list)) else (floor,)
    x, z = nx, nz
    r = radius
    for _ in range(3):
        moved = False
        for c in collisions:
            if not _blocks(c, floors, collisions):
                continue
            if x + r < c.x or x - r > c.x + c.w or z + r < c.z or z - r > c.z + c.d:
                continue
            if c.shape in (0, 9, 10):
                x0, x1, z0, z1 = c.x - r, c.x + c.w + r, c.z - r, c.z + c.d + r
                if not (x0 < x < x1 and z0 < z < z1):
                    continue
                out_x = ox <= x0 or ox >= x1
                out_z = oz <= z0 or oz >= z1
                if out_x and not out_z:
                    x = x0 if ox <= x0 else x1
                elif out_z and not out_x:
                    z = z0 if oz <= z0 else z1
                else:   # came in through a corner or was already inside: shortest way out
                    opts = [(x - x0, 'x', x0), (x1 - x, 'x', x1), (z - z0, 'z', z0), (z1 - z, 'z', z1)]
                    _, ax, v = min(opts)
                    if ax == 'x':
                        x = v
                    else:
                        z = v
                moved = True
            elif c.shape in (6, 7, 8):
                if c.shape == 6:
                    rr = c.w / 2
                    sx0 = sx1 = c.x + rr
                    sz0 = sz1 = c.z + rr
                elif c.shape == 7:
                    rr = c.d / 2
                    sx0, sx1 = c.x + rr, c.x + c.w - rr
                    sz0 = sz1 = c.z + rr
                else:
                    rr = c.w / 2
                    sz0, sz1 = c.z + rr, c.z + c.d - rr
                    sx0 = sx1 = c.x + rr
                if sx1 < sx0:
                    sx0 = sx1 = (sx0 + sx1) / 2
                if sz1 < sz0:
                    sz0 = sz1 = (sz0 + sz1) / 2
                px_, pz_ = min(max(x, sx0), sx1), min(max(z, sz0), sz1)
                dx, dz = x - px_, z - pz_
                dist = math.hypot(dx, dz)
                need = rr + r
                if dist >= need:
                    continue
                if dist < 1e-6:
                    dx, dz = ox - px_, oz - pz_
                    dist = math.hypot(dx, dz) or 1.0
                x, z = px_ + dx / dist * need, pz_ + dz / dist * need
                moved = True
            elif c.shape in (1, 2, 3, 4, 5):
                # triangles and diamonds (bio2.exe 0x4E14B0 / 0x4E17B0): push the body out across
                # the nearest edge; a push of more than 400 puts it back where it was.
                poly = _polygon(c)
                n = len(poly)
                ccw = _poly_area(poly) >= 0
                inside = True
                best = None          # (signed distance, nx, nz) of the least deep edge when inside
                close = None         # (distance, cx, cz) to the outline
                for k in range(n):
                    (ax_, az_), (bx_, bz_) = poly[k], poly[(k + 1) % n]
                    ex, ez = bx_ - ax_, bz_ - az_
                    ln = math.hypot(ex, ez)
                    if ln < 1e-6:
                        continue
                    nx_, nz_ = (ez / ln, -ex / ln) if ccw else (-ez / ln, ex / ln)
                    sd = (x - ax_) * nx_ + (z - az_) * nz_
                    if sd > 0:
                        inside = False
                    if best is None or sd > best[0]:
                        best = (sd, nx_, nz_)
                    t = max(0.0, min(1.0, ((x - ax_) * ex + (z - az_) * ez) / (ln * ln)))
                    cx_, cz_ = ax_ + ex * t, az_ + ez * t
                    dd = math.hypot(x - cx_, z - cz_)
                    if close is None or dd < close[0]:
                        close = (dd, cx_, cz_)
                if best is None:
                    continue
                if inside:
                    need = r - best[0]
                    dx_, dz_ = best[1], best[2]
                else:
                    if close[0] >= r:
                        continue
                    need = r - close[0]
                    dx_, dz_ = (x - close[1]) / close[0], (z - close[2]) / close[0]
                if need > 400 + r:
                    return ox, oz
                x, z = x + dx_ * need, z + dz_ * need
                moved = True
            else:
                if blocking_at([c], floors, x, z, radius):
                    return None
        if not moved:
            break
    return x, z


def _polygon(c):
    x, z, w, d = c.x, c.z, c.w, c.d
    if c.shape == 1:
        return [(x, z + d), (x + w, z + d), (x + w, z)]
    if c.shape == 2:
        return [(x, z), (x, z + d), (x + w, z + d)]
    if c.shape == 3:
        return [(x, z), (x + w, z + d), (x + w, z)]
    if c.shape == 4:
        return [(x, z), (x + w, z), (x, z + d)]
    return [(x + w / 2, z), (x + w, z + d / 2), (x + w / 2, z + d), (x, z + d / 2)]


def _poly_area(poly):
    return sum(poly[k][0] * poly[(k + 1) % len(poly)][1] - poly[(k + 1) % len(poly)][0] * poly[k][1]
               for k in range(len(poly)))


def _inside_boundary(c, px, pz):
    if c.shape == 4:
        return c.x <= px <= c.x + c.w and c.z <= pz <= c.z + c.d
    a, b = c.w / 2, c.d / 2
    if a <= 0 or b <= 0:
        return True
    return ((px - (c.x + a)) / a) ** 2 + ((pz - (c.z + b)) / b) ** 2 <= 1.0


class NavGrid:
    """Occupancy grid for one room/floor, used by the mod's auto-walk and the simulator's checks."""

    def __init__(self, collisions, floor, cell=300, radius=PLAYER_RADIUS, bounds=None):
        self.cell = cell
        xs = [c.x for c in collisions] + [c.x + c.w for c in collisions]
        zs = [c.z for c in collisions] + [c.z + c.d for c in collisions]
        if bounds:
            xs += [bounds[0], bounds[2]]
            zs += [bounds[1], bounds[3]]
        self.minx = max(-32768, (min(xs) if xs else -16000) - cell)
        self.minz = max(-32768, (min(zs) if zs else -16000) - cell)
        maxx = min(32767, (max(xs) if xs else 16000) + cell)
        maxz = min(32767, (max(zs) if zs else 16000) + cell)
        self.w = int((maxx - self.minx) // cell) + 1
        self.h = int((maxz - self.minz) // cell) + 1
        self.free = bytearray(self.w * self.h)
        cols = [c for c in collisions if c.on_floor(floor)]
        self.cols, self.floor, self.radius = cols, floor, radius
        for j in range(self.h):
            for i in range(self.w):
                x, z = self.to_world(i, j)
                self.free[j * self.w + i] = 0 if blocking_at(cols, floor, x, z, radius) else 1

    def to_world(self, i, j):
        return self.minx + i * self.cell + self.cell / 2, self.minz + j * self.cell + self.cell / 2

    def to_cell(self, x, z):
        return int((x - self.minx) // self.cell), int((z - self.minz) // self.cell)

    def is_free(self, i, j):
        return 0 <= i < self.w and 0 <= j < self.h and self.free[j * self.w + i] == 1

    def nearest_free(self, i, j, maxr=12):
        if self.is_free(i, j):
            return i, j
        for r in range(1, maxr):
            best = None
            for di in range(-r, r + 1):
                for dj in (-r, r):
                    for (a, b) in ((i + di, j + dj), (i + dj, j + di)):
                        if self.is_free(a, b):
                            d = (a - i) ** 2 + (b - j) ** 2
                            if best is None or d < best[0]:
                                best = (d, a, b)
            if best:
                return best[1], best[2]
        return None

    def find_path(self, sx, sz, tx, tz, goal_radius_cells=1):
        s = self.nearest_free(*self.to_cell(sx, sz))
        g = self.nearest_free(*self.to_cell(tx, tz))
        if s is None or g is None:
            return None
        openq = [(0, s)]
        came = {s: None}
        cost = {s: 0}
        while openq:
            _, cur = heapq.heappop(openq)
            if abs(cur[0] - g[0]) <= goal_radius_cells and abs(cur[1] - g[1]) <= goal_radius_cells:
                path = []
                while cur is not None:
                    path.append(self.to_world(*cur))
                    cur = came[cur]
                path.reverse()
                return self._smooth(path)
            for di, dj, c in ((1, 0, 1), (-1, 0, 1), (0, 1, 1), (0, -1, 1),
                              (1, 1, 1.414), (1, -1, 1.414), (-1, 1, 1.414), (-1, -1, 1.414)):
                n = (cur[0] + di, cur[1] + dj)
                if not self.is_free(*n):
                    continue
                if di and dj and not (self.is_free(cur[0] + di, cur[1]) and self.is_free(cur[0], cur[1] + dj)):
                    continue
                nc = cost[cur] + c
                if n not in cost or nc < cost[n]:
                    cost[n] = nc
                    came[n] = cur
                    h = math.hypot(n[0] - g[0], n[1] - g[1])
                    heapq.heappush(openq, (nc + h, n))
        return None

    def reachable(self, sx, sz):
        """Set of free cells connected to the start position."""
        import collections
        st = self.nearest_free(*self.to_cell(sx, sz))
        if st is None:
            return set()
        seen = {st}
        q = collections.deque([st])
        while q:
            c = q.popleft()
            for di, dj in ((1, 0), (-1, 0), (0, 1), (0, -1)):
                n = (c[0] + di, c[1] + dj)
                if n not in seen and self.is_free(*n):
                    seen.add(n)
                    q.append(n)
        return seen

    def _line_free(self, a, b):
        """Straight segment keeps full body clearance (checked against the real collision shapes)."""
        steps = int(math.hypot(b[0] - a[0], b[1] - a[1]) / 100) + 1
        for k in range(steps + 1):
            t = k / steps
            x, z = a[0] + (b[0] - a[0]) * t, a[1] + (b[1] - a[1]) * t
            if blocking_at(self.cols, self.floor, x, z, self.radius):
                return False
        return True

    def _smooth(self, path):
        if len(path) <= 2:
            return path
        out = [path[0]]
        i = 0
        while i < len(path) - 1:
            j = len(path) - 1
            while j > i + 1 and not self._line_free(path[i], path[j]):
                j -= 1
            out.append(path[j])
            i = j
        return out

