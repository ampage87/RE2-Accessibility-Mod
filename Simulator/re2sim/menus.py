"""Models of the game's menus that the accessibility mod reads, driven by the same buttons
the player presses, so scenarios exercise the mod the way the game would.

Both models follow the game code (bio2.exe, Sourcenext 1.10), not guesses:

StatusModel   the status (inventory) screen. Top menu 0x4F8050, item list and command menu
              0x4F8160 / 0x4F83C3, combine 0x4F88B0. Values are exported at the real
              addresses by memexport (status work 0x691F60).
SaveModel     the PC save / load screens. Input 0x4C5A40, list drawing 0x4315D0, dialogs
              0x4C6E30. The strings they draw are reported to the mod the way the Windows
              host's text hook reports them (x 0 = centred, colour 3 = highlighted).

render_menu() draws either one as a picture (words laid out as the game lays them out), so a
session can compare what is on screen with what the mod says.
"""
from __future__ import annotations

TABS = ["FILE", "MAP", "ITEM", "EXIT"]
# command menu words by cursor value (0x53F210 table; cursor 3 is the top entry)
CMD_WORDS = {3: ("EQUIP", "USE"), 2: ("CHECK", "CHECK"), 1: ("COMBN", "COMBN"), 0: ("USE", "EQUIP")}


class StatusModel:
    """sub: 2 top menu, 3 items, 4 map, 5 files. item_state: 0 slots, 1 command menu,
    2 carrying out command item_cmd (1 = combine: choose the second item with cursor_sub)."""

    def __init__(self, game, opened_by="items"):
        self.g = game
        self.main = 1
        self.sub = 3 if opened_by == "items" else 2
        self.type = 3 if opened_by == "map" else 0      # 3: the map key's map on its own
        self.menu0 = 2                      # top menu starts on ITEM
        self.cursor = 0
        self.cursor_sub = 0
        self.item_state = 0
        self.item_cmd = 0
        self.item_step = 0
        self.menu1 = 3
        self.weapon_menu = False
        self.closed = False

    # --- helpers -------------------------------------------------------------------------
    def slots(self):
        return 8

    def item_at(self, c):
        inv = self.g.inventory
        if c == 10:
            return 0
        return inv[c][0] if c < len(inv) else 0

    def _grid(self, c, key):
        """Slot movement: two columns, 10 is the box above the list (0x4F8177)."""
        n = self.slots()
        if key == "right" and c < n - 1 and c != 10:
            c += 1
        elif key == "left" and 0 < c and c != 10:
            c -= 1
        elif key == "down":
            if c == 10:
                c = 1
            elif c < n - 2:
                c += 2
        elif key == "up":
            if c == 10:
                return None                 # leaves for the top menu
            c = c - 2 if c > 1 else 10
        return c

    # --- input ---------------------------------------------------------------------------
    def press(self, key):
        if self.sub == 2:                                    # top menu
            if key == "right" and self.menu0 < 3:
                self.menu0 += 1
            elif key == "left" and self.menu0 > 0:
                self.menu0 -= 1
            elif key == "action":
                if self.menu0 == 3:
                    self.close()
                else:
                    self.sub = 5 - self.menu0                # FILE 5, MAP 4, ITEM 3
                    self.item_state = 0
            elif key == "down":
                self.sub = 3
            elif key == "cancel":
                self.close()
            return
        if self.sub in (4, 5):
            if key == "cancel":
                self.sub = 2
            return
        # items
        if self.item_state == 0:
            if key in ("up", "down", "left", "right"):
                c = self._grid(self.cursor, key)
                if c is None:
                    self.sub = 2
                else:
                    self.cursor = c
            elif key == "action" and self.item_at(self.cursor):
                iid = self.item_at(self.cursor)
                self.weapon_menu = iid == 4
                self.item_state = 1
                self.menu1 = 3
            elif key == "cancel":
                self.sub = 2
        elif self.item_state == 1:
            lo = 0 if self.weapon_menu else 1
            if key == "down":
                self.menu1 = self.menu1 - 1 if self.menu1 > lo else 3
            elif key == "up":
                self.menu1 = self.menu1 + 1 if self.menu1 < 3 else lo
            elif key == "action":
                self.item_state, self.item_cmd, self.item_step = 2, self.menu1, 0
                self.cursor_sub = self.cursor
                if self.menu1 != 1:                          # equip / use / check finish at once here
                    if self.menu1 == 3 and self.item_at(self.cursor) and self.item_at(self.cursor) < 0x14:
                        self.g.equipped = self.item_at(self.cursor)
                    elif self.menu1 == 3 and self.item_at(self.cursor):
                        # Use: the screen closes and the game tries the item where the player stands
                        item = self.item_at(self.cursor)
                        self.close()
                        self.g.use_item(item)
                    self.item_state = 0
            elif key == "cancel":
                self.item_state = 0
        elif self.item_state == 2 and self.item_cmd == 1:    # combine: choose the second item
            if key in ("up", "down", "left", "right"):
                c = self._grid(self.cursor_sub, key)
                if c is not None:
                    self.cursor_sub = c
            elif key in ("action", "cancel"):
                self.item_state = 0

    def close(self):
        self.main, self.sub = 2, 1
        self.closed = True

    def export(self):
        return {"main_rtn": self.main, "sub_rtn0": self.sub, "item_state": self.item_state,
                "item_cmd": self.item_cmd, "item_step": self.item_step, "cursor": self.cursor,
                "cursor_sub": self.cursor_sub, "menu0": self.menu0, "menu1": self.menu1, "type": self.type,
                "weapon_menu": self.weapon_menu, "file_sel": -1}

    # --- picture -------------------------------------------------------------------------
    def describe(self):
        """What is on screen, line by line, with the highlighted part marked [ ]."""
        from .scd import item_name
        lines = []
        tabs = "  ".join(f"[{t}]" if (self.sub == 2 and i == self.menu0) else t for i, t in enumerate(TABS))
        lines.append(("tabs", tabs))
        names = []
        for c in range(self.slots()):
            iid = self.item_at(c)
            n = item_name(iid) if iid else "-"
            if iid and iid < len(self.g.inventory) + 999:
                cnt = self.g.inventory[c][1] if c < len(self.g.inventory) else 0
                if iid and (cnt > 1 or 2 <= iid <= 19):
                    n += f" {cnt}"
            sel = self.sub == 3 and ((self.item_state == 0 and c == self.cursor) or
                                     (self.item_state == 2 and self.item_cmd == 1 and c == self.cursor_sub))
            names.append(f"[{n}]" if sel else n)
        top = "[top box]" if (self.sub == 3 and self.item_state == 0 and self.cursor == 10) else "top box"
        lines.append(("top", top))
        for r in range(0, self.slots(), 2):
            lines.append(("row", f"{names[r]:<28}{names[r + 1]}"))
        if self.sub == 3 and self.item_state == 1:
            weapon = 0 < self.item_at(self.cursor) < 0x14
            words = []
            lo = 0 if self.weapon_menu else 1
            for v in range(3, lo - 1, -1):
                w = CMD_WORDS[v][0 if weapon else 1]
                words.append(f"[{w}]" if v == self.menu1 else w)
            lines.append(("cmd", "  /  ".join(words)))
        if self.sub == 3 and self.item_state == 2 and self.item_cmd == 1:
            lines.append(("cmd", "COMBINE WITH ..."))
        if self.sub == 4:
            lines.append(("cmd", "MAP"))
        if self.sub == 5:
            lines.append(("cmd", "FILE"))
        return lines


class SaveModel:
    """The PC save / load screen: a list (title, Create New, saved games, Exit) with a window of
    `rows` rows (top row SV_TOP_ROW, highlighted row SV_ROW_SEL), and confirmation boxes."""

    def __init__(self, game, saves=3, mode="save", hires=True):
        self.g = game
        self.mode = mode
        self.hires = hires
        self.entries = (["Create New"] if mode == "save" else []) + \
            [f"{i + 1:02d}  LEON  RPD Main Hall  Time 00:{10 + i:02d}:00" for i in range(saves)] + ["Exit"]
        self.rows = 10
        self.top = 0
        self.sel = 0
        self.dialog = None          # (question, [choices]) while a box is open
        self.dsel = 0
        self.closed = False
        self.result = None

    def press(self, key):
        if self.dialog:
            q, ch = self.dialog
            if key in ("left", "up"):
                self.dsel = max(0, self.dsel - 1)
            elif key in ("right", "down"):
                self.dsel = min(len(ch) - 1, self.dsel + 1)
            elif key == "action":
                pick = ch[self.dsel]
                self.dialog = None
                if pick in ("Overwrite", "Load", "Yes"):
                    self.result = pick
                    self.closed = True
            elif key == "cancel":
                self.dialog = None
            return
        n = len(self.entries)
        cur = self.top + self.sel
        if key == "down" and cur < n - 1:
            if self.sel < self.rows - 1 and self.sel < n - 1:
                self.sel += 1
            else:
                self.top += 1
        elif key == "up" and cur > 0:
            if self.sel > 0:
                self.sel -= 1
            else:
                self.top -= 1
        elif key == "action":
            e = self.entries[cur]
            if e == "Exit":
                self.dialog = ("Leave without saving?" if self.mode == "save" else "Do not load", ["Yes", "No"])
                self.dialog = ("Leave without saving?", ["Yes", "No"])
                self.dsel = 1
            elif e == "Create New":
                self.result = "Create New"
                self.closed = True
            elif self.mode == "save":
                self.dialog = ("Proceed to overwrite this save file?", ["Overwrite", "Cancel"])
                self.dsel = 0
            else:
                self.dialog = ("Load this file?", ["Load", "Do not load"])
                self.dsel = 0
        elif key == "cancel":
            self.dialog = ("Leave without saving?", ["Yes", "No"])
            self.dsel = 1

    def texts(self):
        """The strings the game draws this frame, in drawing order: (x, y, colour, text)."""
        out = []
        base, step = (0x3C, 0x1E) if self.hires else (0x1E, 0x0F)
        out.append((0, base - step, 0, "savedata" if self.mode == "save" else "Load"))
        for i in range(self.rows):
            k = self.top + i
            if k >= len(self.entries):
                break
            out.append((0, base + step * i, 0, self.entries[k]))
        out.append((0, 0x1A0 if self.hires else 0xD0, 0, "Up/Down: select  Enter: confirm  Esc: back"))
        if self.dialog:
            q, ch = self.dialog
            out.append((0, 0xB8, 0, q))
            for i, c in enumerate(ch):
                out.append((0x64 + 0x64 * i, 0xD3, 3 if i == self.dsel else 0, c))
        return out

    def export(self):
        return {"save_top": self.top, "save_rows": self.rows, "save_sel": self.sel, "save_dsel": self.dsel}

    def describe(self):
        lines = []
        for x, y, c, t in self.texts():
            mark = ""
            if x == 0 and y not in (0x1A0, 0xD0) and t in self.entries:
                idx = self.entries.index(t)
                if idx == self.top + self.sel and not self.dialog:
                    mark = "highlight"
            if c == 3:
                mark = "highlight"
            lines.append((x, y, c, f"[{t}]" if mark else t))
        return lines


def render_menu(path, model, said=()):
    """Draws the menu as the game lays it out (words, highlight boxes) with what the mod said
    underneath, and saves a PNG. Needs Pillow."""
    from PIL import Image, ImageDraw
    W, H = 640, 480
    im = Image.new("RGB", (W, H + 120), (12, 14, 20))
    d = ImageDraw.Draw(im)
    if isinstance(model, FileReaderModel):
        for i, (_, line) in enumerate(model.describe()):
            d.text((40, 60 + i * 24), line, fill=(230, 230, 230))
    elif isinstance(model, TerminalModel):
        d.rectangle((60, 40, 580, 360), outline=(60, 160, 60))
        for i, (_, line) in enumerate(model.describe()):
            d.text((80, 60 + i * 24), line, fill=(120, 255, 120))
    elif isinstance(model, StatusModel):
        y = 20
        for kind, line in model.describe():
            col = (230, 230, 230)
            if kind == "tabs":
                col = (200, 200, 120)
            elif kind == "cmd":
                col = (140, 220, 140)
            d.text((24, y), line, fill=col)
            y += 26 if kind != "row" else 22
    else:
        for x, y, c, t in model.describe():
            px = 40 if x == 0 else x * 2
            py = y if model.hires else y * 2
            col = (255, 220, 120) if t.startswith("[") else (220, 220, 220)
            d.text((px, py), t, fill=col)
    d.line((0, H, W, H), fill=(80, 80, 80))
    for i, s in enumerate(list(said)[-5:]):
        d.text((10, H + 8 + i * 22), "Said: " + s, fill=(200, 200, 255))
    im.save(path)
    return path


class TerminalModel:
    """A computer terminal (bio2.exe 0x4D1280): the game types lines into a 12 x 30 grid a
    character at a time; at the name prompt ">" the player types on the keyboard."""

    WORK = 0x13800000            # where the simulator puts the terminal work area

    def __init__(self, game, script=None):
        self.g = game
        self.grid = [[" "] * 30 for _ in range(12)]
        self.row = 0
        self.col = 0
        self.queue = list(script or [
            "DOOR LOCK SERVICE",
            "------------------------------",
            "Hall side doors:      LOCKED",
            "The doors can be unlocked",
            "by a CARD KEY.",
            "",
            "Enter your user name.",
            ">"])
        self.typing = ""
        self.kcol, self.krow = 1, 0          # bio2.exe 0x4D21D0 starts the cursor on A
        self.blink = 0x40
        self.entered = None
        self.state = 1
        self.closed = False
        self.waiting = False

    def step(self):
        """One frame: the game types one character."""
        if self.waiting or not self.queue:
            self.waiting = True
            return
        line = self.queue[0]
        if self.typing == line:
            self.queue.pop(0)
            self.typing = ""
            self.row += 1
            self.col = 0
            return
        self.typing += line[len(self.typing)]
        self.grid[self.row][len(self.typing) - 1] = self.typing[-1]

    # The on-screen keyboard (bio2.exe 0x4D2240): 10 columns x 3 rows; work +2 is the column,
    # +3 the row. Key k = row * 10 + column gives code k + 1 (k < 9), k (10..18) or k - 1
    # (20..28); column 9 is Backspace (row 0) and Enter (rows 1 and 2, one tall key). A code's
    # letter is code + 0x3F, so the first row reads @ A B C D E F G H.
    @staticmethod
    def key_code(col, row):
        k = row * 10 + col
        if col == 9:
            return 0x1D if row == 0 else 0x1C
        if k < 9:
            return k + 1
        if k < 19:
            return k
        return k - 1

    @classmethod
    def key_of(cls, ch):
        for row in range(3):
            for col in range(10):
                c = cls.key_code(col, row)
                if c < 0x1C and chr(c + 0x3F) == ch:
                    return col, row
        return None

    def _input_line(self):
        r = max(0, self.row - 1)
        return r, "".join(self.grid[r]).rstrip()

    def press(self, key):
        if key == "cancel":
            self.closed = True
            return
        if not self.waiting:
            return
        if key == "up":
            self.krow -= 1
            if self.kcol == 9 and self.krow == 1:
                self.krow = 0
            if self.krow < 0:
                self.krow = 2
        elif key == "down":
            self.krow = 0 if self.krow >= 2 else self.krow + 1
        elif key == "left":
            self.kcol = 9 if self.kcol <= 0 else self.kcol - 1
        elif key == "right":
            self.kcol = 0 if self.kcol >= 9 else self.kcol + 1
        elif key == "action":
            code = self.key_code(self.kcol, self.krow)
            r, line = self._input_line()
            start = line.find(">") + 1
            if code == 0x1C:
                self.entered = line[start:]
                self.grid[r + 1 if r + 1 < 12 else r][:] = list(("ACCEPTED " + self.entered).ljust(30)[:30])
            elif code == 0x1D:
                if len(line) > start:
                    self.grid[r][len(line) - 1] = " "
            elif len(line) - start < 8 and len(line) < 30:
                self.grid[r][len(line)] = chr(code + 0x3F)

    def type_text(self, text):
        """Types with the on-screen keyboard: moves the cursor to each letter and presses action,
        then Enter."""
        for ch in list(text.upper()) + ["\n"]:
            target = (9, 1) if ch == "\n" else self.key_of(ch)
            if target is None:
                continue
            while self.kcol != target[0]:
                self.press("right")
                yield
            while self.krow != target[1] and not (target[0] == 9 and ch == "\n" and self.krow >= 1):
                self.press("down")
                yield
            self.press("action")
            yield

    def export(self):
        b = bytearray(0x320)
        b[0], b[1], b[2], b[3] = self.col & 0xFF, self.row & 0xFF, self.kcol & 0xFF, self.krow & 0xFF
        b[4:4 + 360] = bytes(ord(c) for row in self.grid for c in row)
        if self.waiting:
            self.blink = (self.blink + 2) & 0xFF
        b[0x2DA] = self.blink
        return bytes(b)

    def describe(self):
        return [("row", "".join(r).rstrip()) for r in self.grid if "".join(r).strip()]


class DoorLockTerminal(TerminalModel):
    """The main hall's door-lock computer (bio2.exe 0x4D1280, sce 14 in room 200): the screen
    says the hall side doors are locked; after a key press it checks for the Blue card key
    (item 0x35), takes it, sets flag 4.70 (the room script then unlocks the doors) and says the
    lock is released. No typing."""

    def __init__(self, game):
        released = game.get_flag(4, 70)
        head = ["DOOR LOCK SERVICE", "------------------------------"]
        if released:
            lines = head + ["Hall side doors:      RELEASED"]
        else:
            lines = head + ["Hall side doors:      LOCKED", "The doors can be unlocked", "by a CARD KEY.", "",
                            "[press any key]>"]
        super().__init__(game, script=lines)
        self.stage = 2 if released else 0

    def press(self, key):
        if key == "cancel" and self.stage != 1:
            self.closed = True
            return
        if not self.waiting or key not in ("action", "cancel"):
            return
        if self.stage == 0:
            g = self.g
            self.waiting = False
            if g.has_item(0x35):
                g.remove_item(0x35)
                g.set_flag(4, 70, 1)
                self.queue = ["Checking up ID CARD....", "OK!", "Hall side doors lock released."]
            else:
                self.queue = ["Checking up ID CARD....", ""]
            self.stage = 2
        else:
            self.closed = True

    def type_text(self, text):
        return iter(())


class FileReaderModel:
    """Reading a document as it is picked up: the status screen in 'get item' mode (type 2)
    showing the file (screen 4). Left / right turn pages (ST_F_PAGE_NO)."""

    def __init__(self, game, pages=3):
        self.g = game
        self.page = 0
        self.pages = pages
        self.closed = False

    def press(self, key):
        if key == "right" and self.page < self.pages - 1:
            self.page += 1
        elif key == "left" and self.page > 0:
            self.page -= 1
        elif key in ("action", "cancel") and self.page >= self.pages - 1:
            self.closed = True

    def export(self):
        return {"main_rtn": 1, "sub_rtn0": 4, "type": 2, "file_page": self.page, "file_sel": 0}

    def describe(self):
        return [("row", f"[document page {self.page + 1} of {self.pages}]")]


class ItemBoxModel(StatusModel):
    """The item box (status type 1, bio2.exe 0x4FC110): state 0 = your slots, action -> state 1
    = the box list (64 places, a window of five; the middle one, ibox + 2, is the one you
    swap with); up / down scroll (states 2 / 3 while moving); action swaps (state 4)."""

    def __init__(self, game):
        super().__init__(game, "items")
        self.type = 1
        self.ibox = 62                      # so the middle of the window is box place 1 (index 0)

    def box(self):
        b = list(self.g.item_box) + [[0, 0]] * 64
        return b[:64]

    def press(self, key):
        if self.item_state == 0:
            if key in ("up", "down", "left", "right"):
                c = self._grid(self.cursor, key)
                if c is not None:
                    self.cursor = c
            elif key == "action":
                self.item_state = 1
            elif key == "cancel":
                self.close()
        elif self.item_state == 1:
            if key == "up":
                self.ibox = (self.ibox - 1) & 63
            elif key == "down":
                self.ibox = (self.ibox + 1) & 63
            elif key == "action":                 # swap your slot with the middle box place
                sel = (self.ibox + 2) & 63
                box = self.box()
                inv = self.g.inventory
                while len(inv) <= self.cursor:
                    inv.append([0, 0])
                mine = list(inv[self.cursor])
                inv[self.cursor] = list(box[sel])
                box[sel] = mine
                self.g.item_box = [list(x) for x in box]
                self.item_state = 0
            elif key == "cancel":
                self.item_state = 0

    def export(self):
        d = super().export()
        d["ibox_sel"] = self.ibox
        d["type"] = 1
        return d

    def describe(self):
        from .scd import item_name
        rows = [("tabs", "ITEM BOX")]
        box = self.box()
        for k in range(5):
            idx = (self.ibox + k) & 63
            iid, n = box[idx][0], box[idx][1]
            t = f"{idx + 1:2d}: " + (item_name(iid) if iid else "-")
            if k == 2 and self.item_state >= 1:
                t = f"[{t}]"
            rows.append(("cmd", t))
        rows.append(("row", "your items:"))
        for c in range(self.slots()):
            iid = self.item_at(c)
            t = item_name(iid) if iid else "-"
            rows.append(("row", f"[{t}]" if (self.item_state == 0 and c == self.cursor) else t))
        return rows


class OptionsModel:
    """The PC options menu (Ctrl in the game, Option on the title; bio2.exe 0x44A0B0).
    state: 1 top menu (top 0 key config, 1 brightness, 2 sound, 3 exit), 11 key rows (row 0..10),
    12 waiting for a key, 15 'keep the new keys?', 7 brightness picture, 4 sound rows (0..4)."""

    ROW_KEY = [0, 1, 2, 3, 10, 11, 5, 13, 12, 7]
    DEFAULT_KEYS = [0x68, 0x62, 0x64, 0x66, 0, 0x56, 0, 0x41, 0, 0, 0x58, 0x43, 0x11, 0x5A] + [0] * 11 + \
                   [0x26, 0x28, 0x25, 0x27, 0x0D, 0x20, 0x1B]

    def __init__(self, game):
        self.g = game
        self.state = 1
        self.top = 0
        self.row = 0
        self.keys = list(self.DEFAULT_KEYS)
        self.saved = list(self.keys)
        self.choice = 0
        self.speaker = 2
        self.bits, self.rate = 16, 22050
        self.bgm, self.se = 100, 100
        self.closed = False

    def press(self, key):
        s = self.state
        if s == 1:
            if key == "up" and self.top > 0:
                self.top -= 1
            elif key == "down" and self.top < 3:
                self.top += 1
            elif key == "cancel":
                self.closed = True
            elif key == "action":
                self.row = 0
                self.choice = 0
                self.state = {0: 11, 1: 7, 2: 4}.get(self.top, 0)
                if self.top == 3:
                    self.closed = True
        elif s == 11:
            if key == "up":
                self.row = 10 if self.row == 0 else self.row - 1
            elif key == "down":
                self.row = 0 if self.row == 10 else self.row + 1
            elif key in ("left", "right") and self.row == 10:
                self.choice ^= 1
            elif key == "action":
                if self.row < 10:
                    self.state = 12
                elif self.choice == 1:
                    self.keys = list(self.DEFAULT_KEYS)
                elif self.keys != self.saved:
                    self.state, self.choice = 15, 0
                else:
                    self.state, self.row = 1, 0
        elif s == 12:
            if key.startswith("vk:"):
                vk = int(key[3:], 16)
                idx = self.ROW_KEY[self.row]
                if vk in self.keys:                 # the key's old function gets this row's old key
                    self.keys[self.keys.index(vk)] = self.keys[idx]
                self.keys[idx] = vk
                self.state = 11
        elif s == 15:
            if key in ("left", "right"):
                self.choice ^= 1
            elif key == "action":
                if self.choice == 0:
                    self.saved = list(self.keys)
                self.state, self.row, self.choice = 1, 0, 0
        elif s == 7:
            if key in ("action", "cancel"):
                self.state = 1
        elif s == 4:
            if key == "up" and self.row > 0:
                self.row -= 1
            elif key == "down" and self.row < 4:
                self.row += 1
            elif key == "cancel" or (key == "action" and self.row == 4):
                self.state, self.row = 1, 0
            elif key in ("left", "right"):
                d = 1 if key == "right" else -1
                if self.row == 0:
                    self.speaker = 1 if self.speaker == 2 else 2
                elif self.row == 1:
                    self.bits, self.rate = (16, 44100) if self.rate == 22050 else (16, 22050)
                elif self.row == 2:
                    self.bgm = max(1, min(100, self.bgm + d))
                elif self.row == 3:
                    self.se = max(1, min(100, self.se + d))

    def export(self):
        return {"state": self.state, "top": self.top, "row": self.row, "keys": self.keys,
                "choice": self.choice, "speaker": self.speaker, "bits": self.bits, "rate": self.rate,
                "bgm": self.bgm, "se": self.se}

    def describe(self):
        return [("row", f"options state {self.state} top {self.top} row {self.row}")]
