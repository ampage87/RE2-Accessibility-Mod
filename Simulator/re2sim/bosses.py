"""Boss behaviour for the simulator, from bio2.exe (Sourcenext 1.10). The generic enemy model in
game.py walks at the player and hits; bosses add what matters for playing them blind: how much
damage puts them down, what they do then, and the scripted parts of the fight.

  Tyrant 1 ("Mister X", type 0x2A, move routine 0x47D330): life is set to 20000 plus a
    per-encounter amount (0x528F64 table: 250-450; 0x47D4EB..0x47D764). Below 20000 the
    routine 0x480370 puts him down (state 3) and nothing raises his life again, so he stays
    down for the rest of the visit. Attacks (0x47E310 calls): 20-80 damage, two strengths
    per attack.

  Birkin 1 ("G", type 0x30, move routine 0x48A320; init 0x48A540): life 500, 400 in the B
    scenario (System_flg bit 0x20), 800 with 0x6805B1 set. A slow walk with a pipe; swings
    take 30 (45 with 0x6805B1) and 20 (15). A third attack calls 0x4DCA40(500): a kill when its
    hit box (0x4B25E0, 300-1100 in front) catches the player.
  The other G forms use the same walk-and-swing model here with their own numbers until
    their routines are read: 0x31 G2 0x4900E0, 0x33 G4 0x496930, 0x34 G5 0x49B5D0,
    0x36 0x4A4560, 0x37 0x4A8770.
"""
import math

from .world import blocking_at, dir_towards

TYRANT1 = 0x2A
TYRANT_DOWN_AT = 20000


class BossModel:
    """Default: no special behaviour (the generic model runs)."""
    def __init__(self, game, enemy):
        self.g, self.e = game, enemy

    def setup(self):
        pass

    def update(self, dist):
        """Return True when this model handled the enemy this frame."""
        return False

    def on_hit(self, damage):
        pass


class Tyrant1(BossModel):
    SPEED = 28            # [sim note] a steady walk, slower than the player runs
    REACH = 1100
    DAMAGE = 40           # the middle of 0x47E310's 20-80
    COOLDOWN = 80

    def setup(self):
        e = self.e
        extra = 300        # [sim note] 0x528F64's normal-difficulty amounts are 250-450 by encounter
        e.hp = e.max_hp = TYRANT_DOWN_AT + extra
        self.down = False

    def on_hit(self, damage):
        e = self.e
        if not self.down and e.hp < TYRANT_DOWN_AT:
            self.down = True
            self.g.emit("enemy_knocked_down", id=e.id, name=e.name)

    def update(self, dist):
        e, p, g = self.e, self.g.player, self.g
        if self.down:
            return True            # down for the rest of this visit (0x480370: state 3)
        # a scripted entrance: placed above the ceiling or behind a wall (20C: y -9000, inside
        # the corridor's wall) until the player comes near, then he breaks in
        if e.y < -3000 or blocking_at(g.collisions, e.floor, e.x, e.z, radius=0) is not None:
            if dist > 7000:
                return True
            # [sim note] the break-in animation is not modelled: he lands at the nearest open
            # spot toward the player
            best = None
            for k in range(1, 40):
                t = k / 40
                nx, nz = e.x + (p.x - e.x) * t, e.z + (p.z - e.z) * t
                if blocking_at(g.collisions, p.floor, nx, nz, radius=450) is None:
                    best = (nx, nz)
                    if math.hypot(nx - p.x, nz - p.z) <= 4000:
                        break
            if best is None:
                return True
            # a few metres away, not on top of the player
            nx, nz = best
            dd = math.hypot(nx - p.x, nz - p.z)
            if dd < 3000 and dd > 1:
                for back in range(10):
                    ox = nx + (nx - p.x) / dd * 300 * (back + 1)
                    oz = nz + (nz - p.z) / dd * 300 * (back + 1)
                    if blocking_at(g.collisions, p.floor, ox, oz, radius=450) is not None:
                        break
                    nx, nz = ox, oz
                    if math.hypot(nx - p.x, nz - p.z) >= 3000:
                        break
            e.x, e.z = nx, nz
            e.y = p.y
            e.floor = p.floor
            g.emit("enemy_appears", id=e.id, name=e.name)
        if not e.aware:
            e.aware = True
            g.emit("enemy_alert", id=e.id, name=e.name)
        if g.player_locked:
            return True
        if dist > self.REACH * 0.8:
            vx, vz = (p.x - e.x) / dist, (p.z - e.z) / dist
            nx, nz = e.x + vx * self.SPEED, e.z + vz * self.SPEED
            if blocking_at(g.collisions, e.floor, nx, nz, radius=400) is None:
                e.x, e.z = nx, nz
            e.d = dir_towards(e.x, e.z, p.x, p.z)
        if dist <= self.REACH:
            if e.attack_cd <= 0 and getattr(g, "grab_cd", 0) <= 0:
                p.hp -= self.DAMAGE
                e.attack_cd = self.COOLDOWN
                g.grab_cd = 30
                g.emit("player_hurt", by=e.name, damage=self.DAMAGE, hp=p.hp, condition=p.condition)
                if dist > 1:     # a punch knocks the player back
                    bx, bz = p.x + (p.x - e.x) / dist * 700, p.z + (p.z - e.z) / dist * 700
                    if blocking_at(g.collisions, p.floor, bx, bz) is None:
                        p.x, p.z = bx, bz
                if p.hp <= 0:
                    g.dead = True
                    g.emit("player_died", by=e.name)
            else:
                e.attack_cd -= 1
        return True


class Birkin(BossModel):
    """A boss that walks at the player and swings (the G forms)."""
    LIFE = 400
    SPEED = 22            # [sim note] a lumbering walk
    REACH = 1300
    DAMAGE = 30
    COOLDOWN = 75
    KNOCK = 600

    def setup(self):
        self.e.hp = self.e.max_hp = self.LIFE
        self.started = False

    def on_hit(self, damage):
        self.started = True

    def roof(self, dist):
        # [sim note] on a roof above the player (500: Birkin on the sky tram's roof, y -5000, the
        # script moves him over the player's spot every 45 frames): he reaches down through it
        # at whoever is right below; routine 0x4900E0's roof attack is not read yet
        e, p, g = self.e, self.g.player, self.g
        if g.player_locked:
            return True
        if e.attack_cd > 0:
            e.attack_cd -= 1
            return True
        if dist < 900 and getattr(g, "grab_cd", 0) <= 0:
            p.hp -= 20
            e.attack_cd = 90
            g.grab_cd = 30
            g.emit("player_hurt", by=e.name, damage=20, hp=p.hp, condition=p.condition)
            if p.hp <= 0:
                g.dead = True
                g.emit("player_died", by=e.name)
        return True

    def update(self, dist):
        e, p, g = self.e, self.g.player, self.g
        if p.y - e.y > 3000:
            return self.roof(dist)
        # [sim note] placed before his entrance scene (303: state 0 where the scene shows him):
        # he waits until a scene has played with him there, he is shot, or the player comes close
        if not self.started:
            if g.player_locked:
                self.seen_scene = True
                return True
            if getattr(self, "seen_scene", False) or dist < 3000:
                self.started = True
            else:
                return True
        if not e.aware:
            e.aware = True
            g.emit("enemy_alert", id=e.id, name=e.name)
        if g.player_locked:
            return True
        if dist > self.REACH * 0.8:
            vx, vz = (p.x - e.x) / dist, (p.z - e.z) / dist
            nx, nz = e.x + vx * self.SPEED, e.z + vz * self.SPEED
            if blocking_at(g.collisions, e.floor, nx, nz, radius=500) is None:
                e.x, e.z = nx, nz
            e.d = dir_towards(e.x, e.z, p.x, p.z)
        if dist <= self.REACH:
            if e.attack_cd <= 0 and getattr(g, "grab_cd", 0) <= 0:
                p.hp -= self.DAMAGE
                e.attack_cd = self.COOLDOWN
                g.grab_cd = 30
                g.emit("player_hurt", by=e.name, damage=self.DAMAGE, hp=p.hp, condition=p.condition)
                if dist > 1:
                    bx, bz = p.x + (p.x - e.x) / dist * self.KNOCK, p.z + (p.z - e.z) / dist * self.KNOCK
                    if blocking_at(g.collisions, p.floor, bx, bz) is None:
                        p.x, p.z = bx, bz
                if p.hp <= 0:
                    g.dead = True
                    g.emit("player_died", by=e.name)
            else:
                e.attack_cd -= 1
        return True


class Birkin1(Birkin):
    LIFE = 400


class Birkin2(Birkin):
    LIFE = 700            # [sim note] one of 700/600/1200 by scenario
    SPEED = 34
    DAMAGE = 40


class Birkin4(Birkin):
    LIFE = 1400
    SPEED = 55            # [sim note] the four-legged form charges
    REACH = 1100
    DAMAGE = 35
    COOLDOWN = 55


class Birkin5(Birkin):
    LIFE = 1300
    SPEED = 18
    REACH = 1600
    DAMAGE = 50


class Tyrant2(Birkin):
    """The Super Tyrant (type 0x2B), 702 in Leon's B scenario. Life 10000 here [sim note: the
    move routine 0x480900 is not read yet]. The room script (proc 11) waits for his members 25
    or 26 to become 1, then Ada throws the rocket launcher (item AOT 5). Here member 25 is set
    once he has taken 1500 damage, member 26 after 90 seconds of fighting."""
    LIFE = 10000
    SPEED = 40
    REACH = 1400
    DAMAGE = 60
    COOLDOWN = 80
    KNOCK = 800

    def setup(self):
        super().setup()
        self.e.members[25] = 0
        self.e.members[26] = 0
        self.frames = 0

    def on_hit(self, damage):
        super().on_hit(damage)
        if self.e.hp <= self.LIFE - 1500:
            self.e.members[25] = 1

    def update(self, dist):
        if self.e.y > 3000 and not self.started:      # below the floor until his entrance
            return True
        if self.started:
            self.frames += 1
            if self.frames > 90 * 30:
                self.e.members[26] = 1
        return super().update(dist)


MODELS = {TYRANT1: Tyrant1, 0x2B: Tyrant2, 0x30: Birkin1, 0x31: Birkin2, 0x33: Birkin4, 0x34: Birkin5, 0x36: Birkin5, 0x37: Birkin5}


def attach(game, enemy):
    cls = MODELS.get(enemy.type)
    if cls is None:
        return None
    m = cls(game, enemy)
    m.setup()
    return m
