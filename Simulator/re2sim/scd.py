"""RE2 SCD bytecode: decoding, disassembly and opcode metadata.

Opcode sizes and signatures come from BioRand's Bio2ConstantTable.
Signature characters: upper-case letters U, T, I, L, '@', '~' are 16-bit fields
(I signed, the others unsigned except '~' which is a signed relative offset);
everything else is one byte.
"""
from __future__ import annotations

import struct
from dataclasses import dataclass

from ._tables_raw import SIZES, SIGS, ITEM_IDS, ENEMY_IDS

WIDE = set("UTIL@~")
SIGNED = set("I~")

NAMES = [s.split(":")[0] if s else f"op_{i:02X}" for i, s in enumerate(SIGS)]
OP = {n: i for i, n in enumerate(NAMES)}

FLAG_GROUPS = ["FG_SYSTEM", "FG_STATUS", "FG_STOP", "FG_SCENARIO", "FG_COMMON", "FG_ROOM", "FG_ENEMY",
               "FG_ENEMY_2", "FG_ITEM", "FG_MAP", "FG_USE", "FG_MESSAGE", "FG_ROOM_ENEMY"] + \
              [f"FG_PBF{i:02X}" for i in range(16)] + ["FG_ZAPPING", "FG_RBJ_SET", "FG_KEY", "FG_MAP_C",
                                                       "FG_MAP_I", "FG_ITEM_2"]
FG = {n: i for i, n in enumerate(FLAG_GROUPS)}

SCE_NAMES = ["auto", "door", "item", "normal", "message", "event", "flag_chg", "water", "move", "save",
             "itembox", "damage", "status", "hikidashi", "windows"]
SAT_PL, SAT_EM, SAT_SPL, SAT_OB, SAT_MANUAL, SAT_FRONT, SAT_UNDER = 1, 2, 4, 8, 0x10, 0x20, 0x40


@dataclass
class Instr:
    pc: int
    op: int
    size: int
    args: tuple
    raw: bytes

    @property
    def name(self) -> str:
        return NAMES[self.op] if self.op < len(NAMES) else f"op_{self.op:02X}"

    def __str__(self):
        return f"{self.pc:04X}: {self.name}({', '.join(str(a) for a in self.args)})"


def decode_args(op: int, raw: bytes) -> tuple:
    sig = SIGS[op] if op < len(SIGS) else ""
    if ":" not in sig:
        return ()
    fmt = sig.split(":", 1)[1]
    args = []
    p = 1
    for ch in fmt:
        if ch in WIDE:
            if p + 2 > len(raw):
                break
            args.append(struct.unpack_from("<h" if ch in SIGNED else "<H", raw, p)[0])
            p += 2
        else:
            if p + 1 > len(raw):
                break
            args.append(raw[p])
            p += 1
    return tuple(args)


def decode(proc: bytes) -> list:
    """Decode a procedure into instructions; stops at the first invalid opcode
    (procedures are often followed by padding)."""
    out = []
    pc = 0
    while pc < len(proc):
        op = proc[pc]
        if op >= len(SIZES) or SIZES[op] == 0:
            break
        size = SIZES[op]
        raw = proc[pc:pc + size]
        if len(raw) < size:
            break
        out.append(Instr(pc, op, size, decode_args(op, raw), raw))
        pc += size
    return out


def item_name(i: int) -> str:
    if 0 <= i < len(ITEM_IDS) and ITEM_IDS[i]:
        return ITEM_DISPLAY.get(ITEM_IDS[i], ITEM_IDS[i])
    return f"item {i}"


def enemy_name(i: int) -> str:
    if 0 <= i < len(ENEMY_IDS) and ENEMY_IDS[i]:
        return ENEMY_IDS[i]
    return f"enemy type {i}"


# Friendlier spoken names for item identifiers.
ITEM_DISPLAY = {
    "HandgunLeon": "Handgun", "HandgunClaire": "Browning handgun", "CustomHandgun": "Custom handgun",
    "CustomMagnum": "Custom magnum", "CustomShotgun": "Custom shotgun",
    "GrenadeLauncherExplosive": "Grenade launcher", "GrenadeLauncherFlame": "Grenade launcher, flame rounds",
    "GrenadeLauncherAcid": "Grenade launcher, acid rounds", "ColtSAA": "Colt S.A.A.", "SMG": "Sub machine gun",
    "RocketLauncher": "Rocket launcher", "GatlingGun": "Gatling gun", "HandgunAmmo": "Handgun bullets",
    "ShotgunAmmo": "Shotgun shells", "MagnumAmmo": "Magnum rounds", "FuelTank": "Fuel tank",
    "ExplosiveRounds": "Grenade rounds", "FlameRounds": "Flame rounds", "AcidRounds": "Acid rounds",
    "SMGAmmo": "Machine gun bullets", "SparkshotAmmo": "Sparkshot battery", "BowgunAmmo": "Bowgun bolts",
    "InkRibbon": "Ink ribbon", "SmallKey": "Small key", "HandgunParts": "Handgun parts",
    "MagnumParts": "Magnum parts", "ShotgunParts": "Shotgun parts", "FAidSpray": "First aid spray",
    "AntivirusBomb": "Antivirus bomb", "ChemicalACw32": "Chemical AC-w32", "HerbG": "Green herb",
    "HerbR": "Red herb", "HerbB": "Blue herb", "HerbGG": "Mixed herbs, green and green",
    "HerbGR": "Mixed herbs, green and red", "HerbGB": "Mixed herbs, green and blue",
    "HerbGGG": "Mixed herbs, three green", "HerbGGB": "Mixed herbs, green green blue",
    "HerbGRB": "Mixed herbs, green red blue", "PhotoSherry": "Sherry's photo", "ValveHandle": "Valve handle",
    "RedJewel": "Red jewel", "RedCard": "Red card key", "BlueCard": "Blue card key",
    "SerpentStone": "Serpent stone", "JaguarStone": "Jaguar stone", "JaguarStoneL": "Jaguar stone, left half",
    "JaguarStoneR": "Jaguar stone, right half", "EagleStone": "Eagle stone", "BishopPlug": "Bishop plug",
    "RookPlug": "Rook plug", "KnightPlug": "Knight plug", "KingPlug": "King plug",
    "WeaponBoxKey": "Weapon box key", "C4Explosive": "C4 plastic explosive", "C4Detonator": "C4 with detonator",
    "FilmA": "Film A", "FilmB": "Film B", "FilmC": "Film C", "FilmD": "Film D", "UnicornMedal": "Unicorn medal",
    "EagleMedal": "Eagle medal", "WolfMedal": "Wolf medal", "Cog": "Cog wheel", "ManholeOpener": "Manhole opener",
    "MainFuse": "Main fuse", "FuseCase": "Fuse case", "VaccineCart": "Vaccine cartridge",
    "VaccineBase": "Vaccine base", "GVirus": "G-virus", "SpecialKey": "Special key",
    "JointPlugBlue": "Joint plug, blue", "JointPlugRed": "Joint plug, red", "PhotoAda": "Ada's photo",
    "CabinKey": "Cabin key", "SpadeKey": "Spade key", "DiamondKey": "Diamond key", "HeartKey": "Heart key",
    "ClubKey": "Club key", "DownKey": "Down key", "UpKey": "Up key", "PowerRoomKey": "Power room key",
    "MODisk": "M.O. disk", "UmbrellaKeyCard": "Lab card key", "MasterKey": "Master key",
    "PlatformKey": "Platform key", "ChrisDiary": "Chris's diary", "FederalPoliceReport": "Federal police report",
    "MemotoLeon": "Memo to Leon", "PoliceMemorandum": "Police memorandum", "OperationReport1": "Operation report 1",
    "MailToTheChief1": "Mail to the chief", "MailToTheChief2": "Mail to the chief", "SecretaryDiaryA": "Secretary's diary A",
    "SecretaryDiaryB": "Secretary's diary B", "OperationReport2": "Operation report 2",
    "UserRegistration": "User registration", "DevelopedFilmA": "Film A, developed", "DevelopedFilmB": "Film B, developed",
    "DevelopedFilmC": "Film C, developed", "PatrolReport": "Patrol report", "WatchmanDiary": "Watchman's diary",
    "ChiefDiary": "Chief's diary", "SewerManagerDiary": "Sewer manager diary", "SewerManagerFax": "Sewer manager fax",
    "DevelopedFilmD": "Film D, developed", "VaccineSynthesis": "Vaccine synthesis", "LabSecurityManual": "Lab security manual",
    "PEpsilonReport": "P-epsilon report", "HintFiles1": "Hint files", "HintFiles2": "Hint files",
    "FAid": "First aid spray",
}

WEAPON_IDS = set(range(1, 20))
AMMO_FOR = {2: 20, 3: 20, 4: 20, 19: 20, 5: 22, 6: 22, 7: 21, 8: 21, 9: 24, 10: 25, 11: 26, 12: 29,
            14: 28, 15: 27, 16: 23}


def disassemble(proc: bytes) -> str:
    return "\n".join(str(i) for i in decode(proc))
