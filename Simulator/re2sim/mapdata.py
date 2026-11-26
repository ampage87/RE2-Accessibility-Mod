"""Static facts derived from all room files: which doors lead where, and arrival points."""
from .scd import decode


def door_entries(lib, character):
    """{target_rid: [ {x,y,z,d,cut,frm}, ... ]} - where a player arrives in a room through each door
    found in any room's scripts (both branches of conditionals are included)."""
    entries = {}
    for r in lib.all_rooms(character):
        for proc in r.scd_init + r.scd_main:
            for ins in decode(proc):
                if ins.name not in ("door_aot_se", "door_aot_set_4p"):
                    continue
                rest = ins.args[9:] if ins.name == "door_aot_se" else ins.args[13:]
                nx, ny, nz, nd, st, rm, cut = rest[:7]
                rid = f"{st + 1:X}{rm:02X}"
                entries.setdefault(rid, []).append(dict(x=nx, y=ny, z=nz, d=nd, cut=cut, frm=r.rid))
    return entries
