"""Classic REbirth / Sourcenext save files (*.BIOHAZARD2, 2048 bytes).

The file is a copy of the game's save work at 0x98E79C (bio2.exe Sourcenext 1.10): file offset =
address - 0x98E79C. Layout found by comparing saves with the game's memory map:
  0x220  s16 ?, s16 x, s16 y, s16 z, s16 direction  (where the player stood when saving)
  0x214  u32 status flags (group 1: bit 0 Claire, bit 1 scenario B)
  0x378  s16 stage (0-based), s16 room, s16 camera cut
  0x584  u32 file flags
  0x598  inventory, 11 slots of (item, count, part, 0); a two-slot weapon fills two slots (part 1, 2)
  0x5C4  item box, 64 slots of (item, count, 0, 0)
  script flag groups at the addresses of the exe's flag table 0x53B218 that fall inside the save
"""
import struct

SAVE_BASE = 0x98E79C
# flag group -> address (bio2.exe 0x53B218); groups outside the save work are not saved
FLAG_ADDR = {3: 0x98EB2C, 4: 0x98EB4C, 5: 0x98EB6C, 6: 0x98EB74, 7: 0x98EB94, 8: 0x98EBB4, 9: 0x98EC04,
             13: 0x98EC20, 14: 0x98EC30, 15: 0x98EC40, 16: 0x98EC50, 17: 0x98EC60, 18: 0x98EC70, 19: 0x98EC80,
             20: 0x98EC90, 21: 0x98ECA0, 22: 0x98ECB0, 23: 0x98ECC0, 24: 0x98ECD0, 25: 0x98ECE0, 26: 0x98ECF0,
             27: 0x98ED00, 28: 0x98ED10, 29: 0x98ED24, 31: 0x98ED2C, 32: 0x98EC18, 33: 0x98EC1C, 34: 0x98EBD4}


def load(path):
    d = open(path, "rb").read()
    if len(d) < 0x800:
        raise ValueError("not a 2048-byte RE2 save")
    out = {}
    out["stage"], out["room"], out["cut"] = struct.unpack_from("<hhh", d, 0x378)
    _, x, y, z, dr = struct.unpack_from("<hhhhh", d, 0x220)
    out["x"], out["y"], out["z"], out["dir"] = x, y, z, dr & 0xFFF
    out["file_flags"] = struct.unpack_from("<I", d, 0x584)[0]
    # 0x214: the status flags (script flag group 1, 0x989ED0): 1.0 Claire, 1.1 scenario B
    st = struct.unpack_from("<I", d, 0x214)[0]
    out["status_flags"] = {b for b in range(32) if st & (0x80000000 >> b)}
    out["inventory"] = [[d[0x598 + i * 4], d[0x599 + i * 4]] for i in range(11) if d[0x598 + i * 4]]
    out["item_box"] = [(d[0x5C4 + i * 4], d[0x5C5 + i * 4]) for i in range(64)]
    addrs = sorted(FLAG_ADDR.items(), key=lambda kv: kv[1])
    flags = {}
    for k, (grp, a) in enumerate(addrs):
        nxt = addrs[k + 1][1] if k + 1 < len(addrs) else a + 32
        words = max(1, min(8, (nxt - a) // 4))
        s = set()
        for w in range(words):
            o = a - SAVE_BASE + w * 4
            if o + 4 > len(d):
                break
            v = struct.unpack_from("<I", d, o)[0]
            for b in range(32):
                if v & (0x80000000 >> b):
                    s.add(w * 32 + b)
        flags[grp] = s
    out["flags"] = flags
    return out
