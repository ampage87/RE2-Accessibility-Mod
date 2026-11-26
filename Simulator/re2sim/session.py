"""A play session: game + mod wired together, a command interpreter and a transcript.

Commands (one per line; '#' starts a comment):

  Game controls (what the player physically presses)
    up S | run S | back S        hold forward / run forward / back for S seconds (default 1)
    left S | right S             hold turn for S seconds (default 0.25)
    turn DEG                     turn by degrees (+ right, - left) using the turn keys
    action | a                   press the action button (examine, open, pick up, confirm)
    yes | no | ok                answer / dismiss the on-screen message
    aim [S] | fire [N]           hold aim (auto-aims) / fire N shots while aiming
    use ITEM | equip ITEM        use or equip an inventory item (name or number)
    wait S                       let time pass

  Mod hotkeys (the accessibility layer)
    next | prev | where | go | explore | face | target | compass | room | enemies | status | inv | scan | repeat | filter
    goto [N]                     select point N (1-based) of the current list and auto-walk to it

  Simulator / debugging
    map                          ASCII map of the room (P = you, D door, I item, ...)
    info                         raw state dump        gaps      list simulator limitations hit so far
    teleport RID [from RID2 | x z | x y z [dir]]   jump to a room, e.g. 'teleport 200 from 103' arrives
                                 through the door from room 103
    flag G I [V]                 read or set a script flag
    skip                         release a stuck cutscene lock
    events on|off                print raw game events in the transcript
    shot [NAME] [clean]          save a screenshot: the real background for the current camera with
                                 the player, enemies, doors, items, triggers and walls drawn in, plus a
                                 top-down map and the latest speech. 'clean' leaves out walls and
                                 trigger labels. Saved to the shots folder; the path is printed.
"""
from __future__ import annotations

import math
import shlex

from .game import Game, FPS
from .mod import AccessibilityMod
from .view import GameView
from .world import dir_towards, angle_diff, DIR_FULL, facing_vector
from .scd import item_name, ITEM_IDS

QUIET_EVENTS = {"frame", "sim_gap"}


class Session:
    def __init__(self, gamedata, character=0, scenario=0, start=None, echo=True, show_events=False,
                 auto_dismiss=False, seed=1, shot_dir=None, auto_shots=False, mod_impl=None,
                 show_mod_log=False):
        self.lines = []
        import os as _os
        self.shot_dir = shot_dir or _os.path.join(_os.path.dirname(_os.path.dirname(_os.path.abspath(__file__))), "shots")
        self.auto_shots = auto_shots       # also save a screenshot at every camera or room change
        self._shot_n = 0
        self._shot_due = False
        self.echo = echo
        self.show_events = show_events
        self.auto_dismiss = auto_dismiss
        self.game = Game(gamedata, character, scenario, seed=seed)
        self.view = GameView(self.game)
        self.show_mod_log = show_mod_log
        import os as _os2
        self.cpp = (mod_impl or _os2.environ.get("RE2SIM_MOD", "python")) == "cpp"
        if self.cpp:
            from .cosim import CppMod
            self.mod = CppMod(self)
        else:
            self.mod = AccessibilityMod(self.view, self._speak, self._cue)
        self.game.listeners.append(self._on_event)
        self.user_keys = set()
        self._msg_pending_since = None
        self.results = []          # (passed, description)
        self._mark = 0             # transcript index at the start of the previous command
        start = start or ("100" if character == 0 else "104")
        self.game.enter_room(int(start[0], 16), int(start[1:], 16))
        self.mod.on_frame()

    # ---------------------------------------------------------------- transcript
    def out(self, text):
        self.lines.append(text)
        if self.echo:
            print(text, flush=True)

    def _speak(self, text, interrupt):
        # lines that cut off what was being said carry a marker, so tests can check it
        self.out(f"SPEECH: {text}" + ("  [interrupt]" if interrupt and getattr(self, "cpp", False) else ""))

    def _cue(self, name, **kw):
        extra = " ".join(f"{k}={v}" for k, v in kw.items())
        self.out(f"[cue: {name}{(' ' + extra) if extra else ''}]")

    def _on_event(self, kind, data):
        if self.auto_shots and kind in ("camera", "room_enter"):
            self._shot_due = True
        if kind == "sim_gap":
            self.out(f"[sim note] {data['detail']}")
            return
        if self.show_events and kind not in QUIET_EVENTS:
            self.out(f"  (event {kind} {data})")

    # ---------------------------------------------------------------- frame loop
    def _sync_menu(self):
        """Hands the open menu model's state to the C++ mod (memory values and drawn text)."""
        m = self.mod
        mm = getattr(self, "menu", None)
        # the mod presses cancel to leave the game's map screen (travel picked on its map)
        if mm is not None and getattr(mm, "type", 0) in (3, 4) and (m.buttons & 128):
            mm.closed = True
        if mm is not None and mm.closed:
            self.menu = mm = None
            if m.ui:
                m.ui.pop("status", None)
                m.ui.pop("save", None)
                m.ui.pop("terminal", None)
                m.ui.pop("options", None)
            m.screen_text = None
        if mm is None:
            return False
        from .menus import StatusModel, TerminalModel
        ui = m.ui if m.ui is not None else {"in_game": True}
        if mm.closed:
            ui.pop("terminal", None)
        if isinstance(mm, TerminalModel):
            mm.step()
            ui["terminal"] = {"grid": mm.export(), "state": mm.state}
            m.screen_text = None
        elif type(mm).__name__ == "OptionsModel":
            ui["options"] = mm.export()
            m.screen_text = None
        elif isinstance(mm, (StatusModel,)) or type(mm).__name__ == "FileReaderModel":
            ui["status"] = mm.export()
            m.screen_text = None
        else:
            ui["save"] = mm.export()
            m.screen_text = mm.texts()
        m.ui = ui
        return True

    def tick(self, frames=1):
        for _ in range(frames):
            if self.game.dead:
                return
            pt = getattr(self.game, "pending_terminal", None)
            if pt and getattr(self, "menu", None) is None:
                self.game.pending_terminal = None
                from .menus import DoorLockTerminal
                if pt == "doorlock":
                    self.menu = DoorLockTerminal(self.game)
            if self.cpp and self._sync_menu():
                self.mod.steer()                  # the game is paused under a menu
                continue
            steer = self.mod.steer() if (self.cpp or self.mod.autowalk) else None
            if steer and "action" in steer:
                steer = set(steer) - {"action"}
                self.mod.on_input("action")
                self.game.press_action()
            self.game.controls = set(self.user_keys) | (steer or set())
            self.game.step(1)
            self.mod.on_frame()
            if self._shot_due:
                self._shot_due = False
                self.screenshot()
            if self.auto_dismiss and self.game.message and not self.game.message.yesno:
                self.game.answer(True)
                self.mod.on_frame()

    def screenshot(self, name=None, clean=False):
        """Saves a PNG of what the player would see and prints its path."""
        import os
        from .render import screenshot
        os.makedirs(self.shot_dir, exist_ok=True)
        self._shot_n += 1
        g = self.game
        fname = f"{self._shot_n:03d}_room{g.room.rid}_cam{g.cut}" + (f"_{name}" if name else "") + ".png"
        path = os.path.join(self.shot_dir, fname)
        speech = ["Said: " + l[8:] for l in self.lines if l.startswith("SPEECH: ")][-4:]
        screenshot(g, self.mod, path, walls=not clean, labels=True, caption=speech, clean=clean)
        self.out(f"[screenshot] {path}")
        return path

    def _ui_command(self, args):
        """Menu model for exercising the C++ mod's menu reading (values at the real addresses):
        ui title [N] | ui screen N | ui step N | ui rtn A [B] | ui result N|clear | ui poke ADDR VAL [SIZE] | ui sys HEX | ui sub4 N | ui resultdata R H M S SAVES | ui tofu 0/1 | ui selmode N | ui hardok 0/1 | ui option N | ui claire 0/1 | ui highlight SPRITE | ui game | ui status | ui close | ui cursor N | ui submenu N |
        ui file F P (F = position in the file list, P = page) | ui itembox N | ui save | ui load |
        ui slot N | ui yesno N (yes/no cursor of the current message)"""
        if not self.cpp:
            self.out("(ui commands need --mod cpp)")
            return
        m = self.mod
        ui = m.ui if m.ui is not None else {"in_game": True}
        sub = args[0].lower() if args else ""
        n = int(args[1]) if len(args) > 1 and args[1].lstrip("-").isdigit() else 0
        if sub == "title":
            # main menu: title routine 2/1, menu screen 0, accepting input (step 1)
            ui = {"in_game": False, "title": {"main_rtn": 2, "sub_rtn0": 1, "screen": 0, "step": 1, "cursor": n}}
        elif sub == "screen" and "title" in ui:
            ui["title"]["screen"] = n
            ui["title"]["cursor"] = 0
        elif sub == "poke":
            # 'ui poke ADDR VALUE [SIZE]' writes game memory the simulator does not model
            # (hex or decimal; SIZE 1, 2 or 4 bytes, default 1)
            a = int(args[1], 0); v = int(args[2], 0); size = int(args[3]) if len(args) > 3 else 1
            ui.setdefault("poke", {})[a] = (v & ((1 << (8 * size)) - 1)).to_bytes(size, "little")
        elif sub == "sys":
            ui["sys_or"] = int(args[1], 16) if len(args) > 1 else 0
        elif sub == "sub4" and "title" in ui:
            ui["title"]["sub4"] = n
        elif sub == "result":
            # results screens after an ending: 'ui result N' shows table entry N (bio2.exe
            # 0x52D178); 'ui resultdata RANK H M S SAVES' sets the values; 'ui result clear'
            r = ui.setdefault("results", {"shown": set()})
            if len(args) > 1 and args[1] == "clear":
                r["shown"] = set()
            else:
                r["shown"].add(n)
        elif sub == "resultdata":
            r = ui.setdefault("results", {"shown": set()})
            v = [int(a) for a in args[1:6]] + [0] * 5
            r["rank"], r["time"], r["saves"] = v[0], (v[1], v[2], v[3]), v[4]
        elif sub == "rtn" and "title" in ui:
            # title routine and sub-routine (title work +0/+1), e.g. 'ui rtn 4 1' = the
            # 4th Survivor title taking input
            ui["title"]["main_rtn"] = n
            ui["title"]["sub_rtn0"] = int(args[2]) if len(args) > 2 else 0
        elif sub == "tofu" and "title" in ui:
            ui["title"]["tofu"] = bool(n)
        elif sub == "step" and "title" in ui:
            ui["title"]["step"] = n
        elif sub == "selmode" and "title" in ui:
            ui["title"]["sel_mode"] = n
        elif sub == "hardok" and "title" in ui:
            ui["title"]["hard_ok"] = bool(n)
        elif sub == "option" and "title" in ui:
            ui["title"]["option"] = n
        elif sub == "claire" and "title" in ui:
            ui["title"]["claire"] = bool(n)
        elif sub == "highlight" and "title" in ui:
            ui["title"]["sprites"] = {i: 0x20 for i in range(40)}
            ui["title"]["sprites"][n] = 0x40
        elif sub == "game":
            ui = {"in_game": True}
        elif sub == "status":
            ui["status"] = {"main_rtn": 1, "type": 0, "cursor": 0, "menu0": 0, "file_sel": -1}
        elif sub == "itembox":
            ui["status"] = {"main_rtn": 1, "type": 1, "cursor": 0, "ibox_sel": n}
        elif sub == "close":
            ui.pop("status", None)
            ui.pop("save_screen", None)
            ui.pop("load_screen", None)
        elif sub == "cursor":
            if "title" in ui and not ui.get("in_game"):
                ui["title"]["cursor"] = n
            elif "status" in ui:
                ui["status"]["cursor"] = n
        elif sub == "submenu" and "status" in ui:
            ui["status"]["menu0"] = n
        elif sub == "file" and "status" in ui:
            ui["status"]["file_sel"] = n
            ui["status"]["file_page"] = int(args[2]) if len(args) > 2 else 0
        elif sub == "files":
            ui["file_flags"] = int(args[1], 0) if len(args) > 1 else 0
        elif sub in ("save", "load"):
            ui["save_screen" if sub == "save" else "load_screen"] = True
            ui["save_cursor"] = 0
        elif sub == "slot":
            ui["save_cursor"] = n
        elif sub == "yesno" and self.game.message is not None:
            self.game.message.cursor = n
        else:
            self.out(self._ui_command.__doc__)
            return
        m.ui = ui
        self.tick(2)

    def hold(self, keys, seconds):
        self.user_keys = set(keys)
        self.tick(max(1, int(seconds * FPS)))
        self.user_keys = set()
        self.game.controls = set()
        self.tick(1)

    def walk_until_arrived(self, max_seconds=60):
        frames = 0
        while self.mod.autowalk and frames < max_seconds * FPS and not self.game.dead:
            self.tick(1)
            frames += 1
            if self.game.message and self.game.message.text.strip():
                break

    def face(self, want, max_frames=120):
        for _ in range(max_frames):
            diff = angle_diff(self.game.player.d, want)
            if abs(diff) <= 40:
                break
            self.user_keys = {"right" if diff > 0 else "left"}
            self.tick(1)
        self.user_keys = set()

    # ---------------------------------------------------------------- commands
    def run(self, line):
        line = line.split("#", 1)[0].strip()
        if not line:
            return
        parts = line.split()
        cmd, args = parts[0].lower(), parts[1:]
        if cmd.startswith("expect"):
            self._expect(cmd, " ".join(args))
            return
        self.out(f"> {line}")
        self._mark = len(self.lines) - 1
        self._ev_mark = len(self.game.events)
        num = lambda i, d: float(args[i]) if len(args) > i else d
        g = self.game
        try:
            if cmd in ("up", "forward", "walk"):
                self.hold({"up"}, num(0, 1))
            elif cmd == "run":
                self.hold({"up", "run"}, num(0, 1))
            elif cmd in ("back", "down"):
                self.hold({"down"}, num(0, 1))
            elif cmd in ("left", "right"):
                self.hold({cmd}, num(0, 0.25))
            elif cmd == "turn":
                deg = num(0, 90)
                self.face((g.player.d + int(deg / 360 * DIR_FULL)) % DIR_FULL)
            elif cmd in ("action", "a", "x"):
                self.mod.on_input("action")
                g.press_action()
                self.tick(2)
            elif cmd in ("choose", "left", "right") and g.message is not None and g.message.yesno and \
                    (cmd == "choose" or getattr(g.message, "choices", 2) > 2):
                # a question with more choices (20B's key pad: 1-6): move the cursor; choose N
                # picks choice N (1-based) and confirms
                n = getattr(g.message, "choices", 2)
                cur = getattr(g.message, "cursor", 0)
                if cmd == "choose":
                    while g.message is not None and g.message.page < g.message.pages - 1:
                        self.tick(4)
                        g.answer(True)
                        self.tick(1)
                    self.tick(4)
                    target = max(0, min(n - 1, int(num(0, 1)) - 1))
                    while g.message is not None and getattr(g.message, "cursor", 0) != target:
                        g.message.cursor = (getattr(g.message, "cursor", 0) + 1) % n
                        self.tick(6)
                    g.answer(True)
                    self.tick(2)
                else:
                    g.message.cursor = (cur + (1 if cmd == "right" else -1)) % n
                    self.tick(4)
            elif cmd in ("yes", "ok", "y", "confirm", "no", "n", "cancel"):
                yes = cmd in ("yes", "ok", "y", "confirm")
                # page through to the last page, where the question is asked
                while g.message is not None and g.message.page < g.message.pages - 1:
                    self.tick(4)
                    g.answer(True)
                    self.tick(1)
                if g.message is not None and g.message.yesno:
                    self.tick(4)                   # the choices appear once the page is typed
                    if not yes:
                        g.message.cursor = 1       # move to No, then confirm
                        self.tick(2)
                    else:
                        g.message.cursor = 0
                g.answer(yes)
                self.tick(2)
            elif cmd == "page":
                self.tick(4)                       # let the page finish typing
                if g.message is not None:
                    g.answer(True)
                self.tick(4)
            elif cmd == "aim":
                self.user_keys = {"aim"}
                self.tick(max(2, int(num(0, 0.5) * FPS)))
            elif cmd == "fire":
                # "fire N up": aiming up (up held with aim)
                g.aim_up = "up" in args
                args = [a for a in args if a != "up"]
                self.user_keys = {"aim", "up"} if g.aim_up else {"aim"}
                self.tick(2)
                for _ in range(int(num(0, 1))):
                    self.mod.on_input("fire")
                    g.fire()
                    self.tick(20)
                self.user_keys = set()
                g.aim_up = False
                self.tick(2)
            elif cmd in ("use", "equip"):
                item = self._item_arg(" ".join(args))
                if item is None:
                    self.out("(no such item in inventory)")
                else:
                    g.use_item(item)
                    self.tick(2)
            elif cmd == "wait":
                self.tick(int(num(0, 1) * FPS))
            elif cmd in ("next", "prev", "where", "compass", "room", "enemies", "status", "inv", "scan",
                         "repeat", "filter", "cat", "catprev", "help", "version", "diag", "health", "mapkey", "mapmode", "timer"):
                self.mod.hotkey(cmd)
            elif self.cpp and cmd in ("health-assist", "ammo-assist", "godmode"):
                self.mod.hotkey(cmd)
            elif self.cpp and cmd in ("target", "targetnext", "face", "go", "explore", "getaway"):
                self.mod.hotkey(cmd)
                self.walk_until_arrived(num(0, 60))
                self.tick(3)
            elif cmd == "target":
                r = self.mod.hotkey("target")
                if r:
                    self.face(r[1])
            elif cmd == "face":
                r = self.mod.hotkey("face")
                if r:
                    self.face(r[1])
                    self.mod.say("Facing it.")
            elif cmd == "go":
                if self.mod.hotkey("go"):
                    self.walk_until_arrived(num(0, 60))
            elif cmd == "explore":
                if self.mod.hotkey("explore"):
                    self.walk_until_arrived(num(0, 60))
            elif cmd == "goto":
                pts = self.mod.points()
                n = int(num(0, 1)) - 1
                if 0 <= n < len(pts):
                    self.mod.selected = pts[n].id
                    if self.mod.hotkey("go"):
                        self.walk_until_arrived()
                else:
                    self.out("(no such point)")
            elif cmd == "ui":
                self._ui_command(args)
            elif cmd == "menu" and self.cpp:
                from .menus import StatusModel, SaveModel
                kind = args[0].lower() if args else ""
                if kind == "status":
                    self.menu = StatusModel(self.game, "items")
                elif kind == "statusmenu":
                    self.menu = StatusModel(self.game, "menu")
                elif kind == "map":
                    self.menu = StatusModel(self.game, "map")
                elif kind == "options":
                    from .menus import OptionsModel
                    self.menu = OptionsModel(self.game)
                elif kind in ("save", "load"):
                    self.menu = SaveModel(self.game, int(args[1]) if len(args) > 1 else 3, kind)
                elif kind == "itembox":
                    from .menus import ItemBoxModel
                    self.menu = ItemBoxModel(self.game)
                elif kind == "readfile":
                    from .menus import FileReaderModel
                    self.menu = FileReaderModel(self.game, int(args[1]) if len(args) > 1 else 3)
                elif kind == "computer":
                    from .menus import TerminalModel
                    self.menu = TerminalModel(self.game)
                elif kind == "close" and getattr(self, "menu", None):
                    self.menu.closed = True
                else:
                    self.out("menu status | statusmenu | save N | load N | close")
                self.tick(3)
            elif cmd == "press":
                key = args[0].lower() if args else ""
                times = int(args[1]) if len(args) > 1 else 1
                for _ in range(times):
                    if getattr(self, "menu", None):
                        self.menu.press(key)
                    self.tick(3)
            elif cmd == "find":
                # press next until the catalog names something containing TEXT
                want = " ".join(args).lower()
                found = False
                for _cat in range(6):            # in this category, then the next ones
                    first = None
                    for _ in range(60):
                        self.mod.hotkey("next")
                        said = [l for l in self.lines if l.startswith("SPEECH")]
                        last = said[-1] if said else ""
                        if want in last.lower():
                            found = True
                            break
                        if " here." in last or last == first:
                            break
                        first = first or last
                    if found:
                        break
                    self.mod.hotkey("cat")
                if not found:
                    self.out(f"(find: nothing called {want})")
            elif cmd == "type" and getattr(self, "menu", None) is not None and hasattr(self.menu, "type_text"):
                for _ in self.menu.type_text(" ".join(args)):
                    self.tick(4)
                self.tick(15)
            elif cmd == "menushot":
                from .menus import render_menu
                import os
                if getattr(self, "menu", None):
                    os.makedirs(self.shot_dir, exist_ok=True)
                    self._shot_n += 1
                    said = [l[8:] for l in self.lines if l.startswith("SPEECH")]
                    name = args[0] if args else "menu"
                    path = os.path.join(self.shot_dir, f"{self._shot_n:03d}_{name}.png")
                    render_menu(path, self.menu, said)
                    self.out(f"[menu screenshot] {path}")
                    for row in self.menu.describe():
                        self.out("  screen: " + (row[1] if len(row) == 2 else row[3]))
            elif cmd == "shot":
                clean = "clean" in args
                names = [a for a in args if a != "clean"]
                self.screenshot(names[0] if names else None, clean=clean)
            elif cmd == "map":
                self.out(self.ascii_map(int(num(0, 64))))
            elif cmd == "info":
                self.out(self.info())
            elif cmd == "gaps":
                for gp in g.gaps:
                    self.out(f"- {gp}")
                if not g.gaps:
                    self.out("(none)")
            elif cmd == "teleport":
                # a cutscene cut short by the teleport would leave the player frozen
                g.message = None
                g.set_flag(2, 7, 0)
                g.set_flag(1, 27, 0)
                rid = args[0].upper()
                st, rm = int(rid[0], 16), int(rid[1:], 16)
                if len(args) >= 3 and args[1].lower() == "from":
                    from .mapdata import door_entries
                    if not hasattr(self, "_entries"):
                        self._entries = door_entries(g.lib, g.player.character)
                    ents = [e for e in self._entries.get(rid, []) if e["frm"] == args[2].upper()]
                    if not ents:
                        self.out(f"(no door from {args[2]} into {rid})")
                        return
                    e = ents[0]
                    g.enter_room(st, rm, e["x"], e["y"], e["z"], e["d"], e["cut"])
                elif len(args) >= 3:
                    nums = [float(v) for v in args[1:]]
                    if len(nums) == 2:
                        g.enter_room(st, rm, nums[0], 0, nums[1])
                    else:
                        g.enter_room(st, rm, nums[0], nums[1], nums[2], int(nums[3]) if len(nums) > 3 else None)
                else:
                    g.enter_room(st, rm)
                self.tick(1)
            elif cmd == "var":
                # var N [V]: show or set script variable N (35 is the escape countdown's seconds)
                if len(args) > 1:
                    g.setv(int(args[0]), int(args[1]))
                self.out(f"var {int(args[0])} = {g.getv(int(args[0]))}")
            elif cmd == "flag":
                grp, idx = int(args[0]), int(args[1])
                if len(args) > 2:
                    g.set_flag(grp, idx, int(args[2]))
                self.out(f"flag {grp}.{idx} = {g.get_flag(grp, idx)}")
            elif cmd == "skip":
                g.release_lock()
                self.tick(1)
            elif cmd == "events":
                self.show_events = (args[0] if args else "on") == "on"
            elif cmd == "heal":
                g.player.hp = g.player.max_hp
            elif cmd == "hp" and args:
                g.player.hp = int(args[0])              # set the player's life (testing)
            elif cmd == "poison" and args:
                g.poison = 600 if args[0] != "0" else 0  # the game's poison timer (testing)
                g.dead = False
            elif cmd == "enemypos":
                # [cheat] move an enemy: enemypos ID X Y Z (stands in for a scripted entrance)
                e = g.enemies.get(int(args[0]))
                if e is not None:
                    e.x, e.y, e.z = float(args[1]), float(args[2]), float(args[3])
                    if e.boss is not None and hasattr(e.boss, "started"):
                        e.boss.started = True
                    self.out(f"(enemy {e.id} moved)")
            elif cmd == "killall":
                # [cheat] remove the room's enemies (when testing navigation, not combat)
                for e in g.enemies.values():
                    e.alive = False
                g.dead = False
                self.out("(enemies removed)")
            elif cmd == "store":
                # [cheat] put an inventory item in the item box from anywhere (both slots of a big weapon)
                item = self._item_arg(" ".join(args))
                if item is None:
                    self.out("(no such item in inventory)")
                else:
                    slots = [x for x in g.inventory if x[0] == item]
                    for x in slots:
                        g.inventory.remove(x)
                    g.item_box = [b for b in g.item_box if b[0]] + [(item, slots[0][1])]
                    g.item_box += [(0, 0)] * (64 - len(g.item_box))
                    if g.equipped == item:
                        g.equipped = None
                    self.out(f"(stored {len(slots)} slot(s))")
            elif cmd == "combine":
                # [cheat] combine two inventory items (stands in for the status screen's Combine)
                names = " ".join(args).split("+")
                ids = [self._item_arg(n.strip()) for n in names]
                RESULT = {frozenset((65, 64)): 66, frozenset((56, 57)): 55, frozenset((38, 39)): 42,
                          frozenset((38, 40)): 43, frozenset((38,)): 41}
                if len(ids) != 2 or None in ids:
                    self.out("(combine: name two inventory items as A + B)")
                else:
                    res = RESULT.get(frozenset(ids))
                    if res is None:
                        self.out("(those don't combine)")
                    else:
                        g.remove_item(ids[0], all_=True)
                        g.remove_item(ids[1], all_=True)
                        g.inventory.append([res, 1])
                        self.out(f"(combined into {item_name(res)})")
            elif cmd in ("snap", "restore"):
                # [tool] save / restore the whole game state (saves replaying a long route)
                import pickle, os as _os
                path = _os.path.join(_os.path.dirname(_os.path.abspath(__file__)), "..", "snaps", (args[0] if args else "default") + ".pkl")
                _os.makedirs(_os.path.dirname(path), exist_ok=True)
                if cmd == "snap":
                    lst = g.listeners
                    g.listeners = []
                    try:
                        with open(path, "wb") as f:
                            pickle.dump(g, f)
                        self.out(f"(snapshot saved: {args[0] if args else 'default'})")
                    finally:
                        g.listeners = lst
                else:
                    with open(path, "rb") as f:
                        ng = pickle.load(f)
                    ng.listeners = g.listeners
                    self.game = ng
                    self.view._g = ng
                    self.out("(snapshot restored)")
                    self.tick(2)
            elif cmd == "retrieve":
                # [cheat] take an item out of the item box from anywhere (stands in for the box screen)
                want = " ".join(args).lower()
                hit = next((i for i, b in enumerate(g.item_box) if b[0] and item_name(b[0]).lower() == want), None)
                if hit is None:
                    hit = next((i for i, b in enumerate(g.item_box) if b[0] and item_name(b[0]).lower().startswith(want)), None)
                if hit is None:
                    self.out("(no such item in the box)")
                elif len(g.inventory) >= g.inventory_size:
                    self.out("(inventory full)")
                else:
                    iid, n = g.item_box[hit]
                    g.item_box[hit] = (0, 0)
                    g.inventory.append([iid, n])
                    self.out(f"(took {item_name(iid)} from the box)")
            elif cmd == "loadsave":
                from . import savefile
                sv = savefile.load(" ".join(args))
                for grp, idxs in sv["flags"].items():
                    g.flags[grp] = set(idxs)
                # status flags (group 1): which character, scenario A or B
                g.flags[1] = set(sv["status_flags"])
                g.player.character = 1 if 0 in sv["status_flags"] else 0
                g.scenario = 1 if 1 in sv["status_flags"] else 0
                g.inventory = [list(x) for x in sv["inventory"] if not (104 <= x[0] <= 128)]
                g.file_flags = sv["file_flags"]
                g.item_box = [tuple(x) for x in sv["item_box"]]
                weapons = [it for it, n in g.inventory if 2 <= it <= 19]
                g.equipped = weapons[0] if weapons else None
                if self.cpp:
                    ui = self.mod.ui if self.mod.ui is not None else {"in_game": True}
                    ui["file_flags"] = sv["file_flags"]
                    self.mod.ui = ui
                g.enter_room(sv["stage"] + 1, sv["room"], sv["x"], sv["y"], sv["z"], sv["dir"], sv["cut"])
                self.tick(1)
                self.out(f"loaded save: room {sv['stage'] + 1:X}{sv['room']:02X}, {len(g.inventory)} items")
            elif cmd == "give":
                qty = 1
                if len(args) > 1 and args[-1].isdigit():
                    qty = int(args[-1])
                    args = args[:-1]
                item = self._item_id(" ".join(args))
                if item is None:
                    self.out(f"(unknown item {' '.join(args)})")
                else:
                    g.add_item(item, qty, source="debug")
            else:
                self.out(f"(unknown command {cmd})")
        except Exception as e:     # keep the session alive; report the failure
            import traceback
            self.out(f"[sim error] {type(e).__name__}: {e}")
            self.out(traceback.format_exc(limit=3))

    def _expect(self, cmd, arg):
        said = [l[8:] for l in self.lines[self._mark:] if l.startswith("SPEECH: ")]
        if cmd == "expect":
            ok = any(arg.lower() in t.lower() for t in said)
        elif cmd == "expect-not":
            ok = not any(arg.lower() in t.lower() for t in said)
        elif cmd == "expect-room":
            ok = self.game.room.rid.upper() == arg.upper()
        elif cmd == "expect-message":
            ok = any(k == "message" and d.get("text", "").strip() for f, k, d in self.game.events[self._ev_mark:])
        elif cmd == "expect-item":
            ok = any(arg.lower() in item_name(i).lower() for i, q in self.game.inventory)
        else:
            ok = False
        self.results.append((ok, f"{cmd} {arg}"))
        self.out(f"[{'PASS' if ok else 'FAIL'}] {cmd} {arg}")

    def _item_id(self, s):
        if s.isdigit():
            return int(s)
        s2 = s.lower().replace(" ", "")
        for i, n in enumerate(ITEM_IDS):
            if n.lower() == s2 or item_name(i).lower().replace(" ", "") == s2:
                return i
        return None

    def _item_arg(self, s):
        if not s:
            return None
        if s.isdigit():
            n = int(s)
            inv = self.game.inventory
            return inv[n - 1][0] if 1 <= n <= len(inv) else None
        s2 = s.lower()
        for i, q in self.game.inventory:
            if s2 in item_name(i).lower() or s2 in ITEM_IDS[i].lower():
                return i
        return None

    # ---------------------------------------------------------------- debugging views
    def info(self):
        g = self.game
        p = g.player
        lines = [f"room {g.room.rid} cut {g.cut} frame {g.frame}",
                 f"player x={p.x:.0f} y={p.y:.0f} z={p.z:.0f} dir={p.d} floor={p.floor} hp={p.hp} locked={g.player_locked}",
                 f"inventory {[(item_name(i), q) for i, q in g.inventory]} equipped={item_name(g.equipped) if g.equipped else None}",
                 "aots:"]
        for a in g.aots.values():
            extra = a.door and f"-> {a.door['stage']:X}{a.door['room']:02X} lock={a.door['lock_type']}" or \
                (a.item and f"item {item_name(a.item['item'])} x{a.item['amount']}") or f"data={a.data}"
            cx, cz = a.centre
            lines.append(f"  {a.id:2d} {a.kind:9s} sat={a.sat:#04x} at ({cx:.0f},{cz:.0f}) {extra}")
        lines.append("enemies:")
        for e in g.enemies.values():
            lines.append(f"  {e.id} {e.name} ({e.x:.0f},{e.z:.0f}) hp={e.hp} alive={e.alive} active={e.active}")
        lines.append(f"threads: {[(t.origin, [(f.proc, hex(f.pc)) for f in t.frames], t.wait, getattr(t, 'waiting_message', False)) for t in g.threads]}")
        return "\n".join(lines)

    def ascii_map(self, width=64):
        g = self.game
        r = g.room
        p = g.player
        pts = [(c.x, c.z) for c in g.collisions] + [(c.x + c.w, c.z + c.d) for c in g.collisions]
        pts += [(p.x, p.z)] + [a.centre for a in g.aots.values()]
        xs = [q[0] for q in pts if -32000 < q[0] < 32000]
        zs = [q[1] for q in pts if -32000 < q[1] < 32000]
        minx, maxx, minz, maxz = min(xs), max(xs), min(zs), max(zs)
        scale = (maxx - minx) / width or 1
        height = int((maxz - minz) / scale / 2) + 1
        grid = [[" "] * (width + 1) for _ in range(height + 1)]
        from .world import blocking_at
        for j in range(height + 1):
            for i in range(width + 1):
                x = minx + i * scale
                z = maxz - j * scale * 2
                if blocking_at(g.collisions, p.floor, x, z, radius=0):
                    grid[j][i] = "#"

        def put(x, z, ch):
            i = int((x - minx) / scale)
            j = int((maxz - z) / scale / 2)
            if 0 <= j <= height and 0 <= i <= width:
                grid[j][i] = ch
        sym = {"door": "D", "item": "I", "message": "?", "event": "E", "save": "S", "itembox": "B", "flag_chg": "U"}
        for a in g.aots.values():
            if a.player_triggers and a.kind in sym:
                put(*a.centre, sym[a.kind])
        for e in g.enemies.values():
            if e.alive and e.hostile:
                put(e.x, e.z, "Z")
        put(p.x, p.z, "P")
        fx, fz = facing_vector(p.d)
        legend = f"Room {r.rid}: P you (facing {['E','SE','S','SW','W','NW','N','NE'][int(((p.d % 4096) / 512) + 0.5) % 8]}), "
        legend += "D exit, I item, ? examine, E event, S typewriter, B item box, U item-use spot, Z enemy, # wall. North is up."
        return legend + "\n" + "\n".join("".join(row) for row in grid)
