"""Runs the real C++ mod (re2mod) inside the simulator.

The mod is built as a small native program (re2mod/tests/harness.cpp) that hosts the exact
mod code used in the game. Each frame the simulator sends the game's memory, laid out as in
bio2.exe (memexport.py), plus any mod hotkeys; the mod answers with speech, cues, log lines
and the buttons it holds (auto-walk, turning).

CppMod mimics the parts of the Python prototype's interface that Session uses.
"""
from __future__ import annotations

import os
import subprocess

from . import memexport

HOTKEYS = {
    "next": 1, "prev": 2, "cat": 3, "filter": 3, "catprev": 4, "where": 5, "go": 6, "face": 7,
    "target": 8, "targetnext": 9, "room": 10, "status": 11, "health": 11, "inv": 12, "enemies": 13,
    "scan": 14, "compass": 15, "explore": 16, "repeat": 17, "help": 18, "version": 19, "diag": 20,
    "godmode": 21, "health-assist": 21, "ammo-assist": 22, "mapkey": 23, "mapmode": 24, "getaway": 25, "timer": 26,
}
BTN = {1: "up", 2: "down", 4: "left", 8: "right", 16: "action", 32: "run", 64: "aim", 128: "cancel"}


def default_harness():
    here = os.path.dirname(os.path.abspath(__file__))
    for cand in (os.path.join(here, "..", "..", "re2mod", "tests", "harness"),
                 os.path.join(here, "..", "..", "Mod", "tests", "harness"),
                 os.path.join(here, "..", "harness"),
                 os.path.join(here, "..", "..", "re2mod", "tests", "harness.exe"),
                 os.path.join(here, "..", "..", "Mod", "tests", "harness.exe")):
        if os.path.exists(cand):
            return os.path.abspath(cand)
    return None


def default_mod_data():
    here = os.path.dirname(os.path.abspath(__file__))
    for cand in (os.path.join(here, "..", "..", "re2mod", "data"), os.path.join(here, "..", "..", "Mod", "data")):
        if os.path.isdir(cand):
            return os.path.abspath(cand)
    return ""


class CppMod:
    def __init__(self, session, harness=None, mod_data=None):
        self.s = session
        exe = harness or default_harness()
        if not exe:
            raise RuntimeError("C++ mod harness not built: run 'make -C re2mod/tests' (or build tests/harness.cpp)")
        self.p = subprocess.Popen([exe, session.game.lib.gamedata, mod_data or default_mod_data()],
                                  stdin=subprocess.PIPE, stdout=subprocess.PIPE, bufsize=0)
        self.buttons = 0
        self.busy = False
        self.keys = []
        self.v = session.view            # the screenshot overlay uses the simulator's view
        self.autowalk = None             # truthy while the mod holds movement buttons
        self.log_lines = []
        self.writes = []
        self._quiet_frames = 0
        self._walk_requested = False
        self.ms = 0
        self._read_until("READY")
        self.ui = None                   # menu model state (title/status) when modelled
        self.screen_text = None          # strings the PC menus draw this frame (x, y, colour, text)

    # --- protocol ------------------------------------------------------------------------------
    def _read_until(self, marker):
        while True:
            line = self.p.stdout.readline()
            if not line:
                raise RuntimeError("mod harness exited")
            line = line.decode("utf-8", "replace").rstrip("\n")
            if line == marker:
                return
            self._handle(line)

    def _handle(self, line):
        if line.startswith("SAY "):
            self.s._speak(line[6:], line[4] == "1")
        elif line.startswith("CUE "):
            self.s._cue(line[4:])
        elif line.startswith("LOG "):
            self.log_lines.append(line[4:])
            if self.s.show_mod_log:
                self.s.out(f"  (mod log) {line[4:]}")
        elif line.startswith("BUTTONS "):
            self.buttons = int(line[8:])
        elif line.startswith("WRITE "):
            _, a, hexb = line.split(" ", 2)
            self._apply_write(int(a, 16), bytes.fromhex(hexb))
        elif line.startswith("BUSY "):
            self.busy = line[5:] == "1"

    def _apply_write(self, addr, data):
        """Mod memory writes (assist options, puzzle help) applied to the simulated game."""
        g = self.s.game
        from .memexport import MSG_CHUNK, OBJ_WORK, OBJ_SIZE
        chunk = getattr(g, "msg_chunk", None)
        if chunk is not None and MSG_CHUNK <= addr < MSG_CHUNK + len(chunk):
            o = addr - MSG_CHUNK
            n = min(len(data), len(chunk) - o)
            chunk[o:o + n] = data[:n]
            g.refresh_message_text()
            self.writes.append((addr, data))
            return
        if OBJ_WORK <= addr < OBJ_WORK + 32 * OBJ_SIZE:
            idx, off = divmod(addr - OBJ_WORK, OBJ_SIZE)
            ob = g.objects.get(idx)
            if ob is not None and len(data) == 4 and off in (0x38, 0x3C, 0x40):
                v = int.from_bytes(data, "little", signed=True)
                setattr(ob, {0x38: "x", 0x3C: "y", 0x40: "z"}[off], v)
            elif ob is not None and len(data) == 1 and off == 0x106:
                ob.floor = data[0]
            self.writes.append((addr, data))
            return
        if addr == 0x989EF0 + 0x156 and len(data) == 2:
            g.player.hp = int.from_bytes(data, "little", signed=True)
        elif 0x98ED34 <= addr < 0x98ED34 + 11 * 4 and (addr - 0x98ED34) % 4 == 1:
            i = (addr - 0x98ED34) // 4
            if i < len(g.inventory):
                g.inventory[i][1] = data[0]
        self.writes.append((addr, data))

    def exchange(self):
        g = self.s.game
        pad = memexport.pad_bits(g.controls)
        img = memexport.build(g, self.ui, pad)
        self.ms += 33
        # buttons the player holds (not the ones the mod is pressing for auto-walk)
        held = 0
        for bit, name in BTN.items():
            if name in g.controls:
                held |= bit
        held &= ~self.buttons
        chunks = [f"FRAME {self.ms} {held}\n".encode()]
        for addr, data in img.regions():
            chunks.append(f"MEM {addr:x} {len(data)}\n".encode())
            chunks.append(data)
        for x, y, c, t in (self.screen_text or []):
            chunks.append(f"TEXT {x} {y} {c} {t.encode('utf-8').hex()}\n".encode())
        for k in self.keys:
            chunks.append(f"KEY {k}\n".encode())
        self.keys = []
        chunks.append(b"END\n")
        self.p.stdin.write(b"".join(chunks))
        self.p.stdin.flush()
        self._read_until("DONE")
        # the mod reports whether it is auto-walking or turning
        self.autowalk = True if self.busy else None

    def close(self):
        try:
            self.p.stdin.write(b"QUIT\n")
            self.p.stdin.flush()
        except Exception:
            pass

    # --- Session-facing interface -------------------------------------------------------------
    def steer(self):
        self.exchange()
        keys = {BTN[b] for b in BTN if self.buttons & b}
        return keys

    def on_frame(self):
        pass

    def on_input(self, key):
        pass

    def hotkey(self, name):
        k = HOTKEYS.get(name)
        if k is None:
            self.s.out(f"(no C++ mod hotkey called {name})")
            return None
        self.keys.append(k)
        self.s.tick(1)
        return True

    def say(self, text, interrupt=False):
        self.s._speak(text, interrupt)

    def points(self):
        return []
