# RE2 Accessibility Mod Simulator

A text-mode simulator of the original Resident Evil 2 (1998 PC, Steam release, English data)
used to design and test the blind-accessibility mod before and alongside building the real DLL.

It loads the real room files (ROOMsrrp.RDT), runs the real room scripts, and plays the game
at 30 frames per second with real collision, doors, items, messages, flags and cutscene
logic. On top of it runs a prototype of the accessibility mod. That prototype sees the game
only through `GameView`, an interface limited to what the DLL can read from game memory or
the room files. Logic proven here can be ported to the DLL.

Pure Python 3 standard library, no installs.

## Quick start

```
cd Simulator
python -m re2sim                                 # interactive; type 'help'
python -m re2sim -c "room" -c "next" -c "go"     # one-shot commands
python -m re2sim --script tests/scenarios/02_leon_gunshop.txt
python tests/run_tests.py                        # regression scenarios (expect assertions)
python tests/sweep_rooms.py [--claire]           # every room: walk to and use every exit
python tests/autoplay.py --rooms 40              # plays forward using only mod hotkeys
python -m re2sim --auto-shots -c "next" -c "go"  # same, saving a screenshot at every camera change
```

## Screenshots

`shot` (or `--auto-shots` on the command line) saves a PNG to `Simulator/shots/` and prints
its path, so the session can be looked at as well as listened to. Each picture has:

- **Left: the game screen.** The real pre-rendered background for the current camera, from
  `Common/bin/ROOMCUT.BIN`, with the simulator's state projected through that camera: the
  player in green (a line for the body and the facing direction), enemies in red, doors in
  cyan, items in yellow, triggers and examine spots in pink, and walls in orange, each
  labelled.
- **Right: a top-down map** of the room with north up, showing the same things plus the
  camera position and the current auto-walk route.
- **Underneath:** any on-screen message and the last few things the mod said.

`shot clean` leaves out walls and trigger spots, keeping only exits, items, enemies and the
player. `shot NAME` adds NAME to the file name.

Limits: characters are markers, not 3D models, and foreground objects in the background
(pillars, railings) do not hide them. The camera projection follows OpenBiohazard2
(focal length = RID distance / 128), so markers can be a few pixels off. Inventory, files,
maps and close-up puzzle screens are separate game images and are not drawn yet. Backgrounds
need `ROOMCUT.BIN`: the simulator looks in the data folder's `Common/bin`, then in
`../Game Files/rebirth/Common/bin/roomcut.bin`. Decoded backgrounds are cached in
`Simulator/.cache/`. Labels need Pillow (`pip install pillow`); without it the pictures are
drawn without text.

The simulator has two copies of the game data:

- **rebirth** (default): the Sourcenext data that Classic REbirth actually runs, from the
  Steam install's `rebirth` folder. Stored as `Simulator/gamedata-rebirth.zip` and extracted
  to `Simulator/gamedata-rebirth/` on first run (fallback: `../Game Files/rebirth`). Rooms
  are in `PlN/Rdt`.
- **english**: the US PC data from the Steam `english` folder. Stored as
  `Simulator/gamedata.zip` (fallback: `../Game Files/4249110_Biohazard2/english`). Rooms are
  in `PlN/Rdu`.

Choose with `--data rebirth`, `--data english` or `--data DIR`, or set the `RE2SIM_DATA`
environment variable (the test scripts honour it, for example
`RE2SIM_DATA=english python tests/run_tests.py`).

To refresh the rebirth snapshot after the game changes, run
`Tools/Copy game files for simulator.cmd` in the mod folder (it only reads the Steam folder),
then rebuild `gamedata-rebirth.zip` from `Game Files/rebirth` (Pl0/Rdt, Pl1/Rdt,
Common/bin/ITEMDATA.BIN, bio2.exe, ddraw.dll, dsound.dll, config.ini).

Options: `--claire`, `--scenario B`, `--start 104`, `--events` (print raw game events),
`--auto-dismiss` (close plain messages automatically), `--out transcript.txt`. Also `--auto-shots` (screenshot at every camera or room change) and `--shots-dir DIR`.

## Commands

Transcript lines: `SPEECH:` is what the screen reader would say, and `[cue: ...]` is a sound
cue the DLL would play. `[sim note]` is a simulator limitation that was hit, and
`[PASS]`/`[FAIL]` are test assertions.

| Group | Commands |
|---|---|
| Game controls | `up S`, `run S`, `back S`, `left S`, `right S`, `turn DEG` (+ right), `action`, `yes`, `no`, `ok`, `aim`, `fire N`, `use ITEM`, `equip ITEM`, `wait S` |
| Mod hotkeys | `next`, `prev`, `where`, `go`, `explore`, `face`, `target`, `compass`, `room`, `enemies`, `status`, `inv`, `scan`, `repeat`, `filter`, `goto N` |
| Assertions | `expect TEXT`, `expect-not TEXT`, `expect-room RID`, `expect-item NAME`, `expect-message` |
| Debug | `shot [NAME] [clean]`, `map`, `info`, `gaps`, `teleport RID from RID2`, `teleport RID x y z [dir]`, `flag G I [V]`, `skip`, `give ITEM [N]`, `heal`, `events on` |

A scenario file can start with an options line, for example `#! start=101 claire auto-dismiss`.

## Files

| File | What it does |
|---|---|
| `re2sim/rdt.py` | RDT parser: cameras (RID), camera zones (RVD), collision (SCA), floor sounds, English messages, init/main scripts |
| `re2sim/scd.py` | Script opcode table (sizes and signatures from BioRand), decoder, item and enemy names |
| `re2sim/game.py` | Simulator core: script VM (threads, if/else/while/for/switch/gosub, sleeps), AOT triggers, doors and locks, items, inventory, typewriter, camera switching, player movement and collision, stairs, climbing, simplified enemies and combat, cutscene watchdog |
| `re2sim/view.py` | `GameView`, the mod's only access to the game. `MEMORY_TODO` lists the memory locations the DLL must find |
| `re2sim/mod.py` | The accessibility-mod prototype: room announcements, points-of-interest cycling, path-finding auto-walk, wall bumps, enemy alerts, aim feedback, ammo, health, stairs and level changes, climbing, scripted-door naming, explore |
| `re2sim/world.py` | Geometry: direction maths, collision shape tests, navigation grid with A* path-finding |
| `re2sim/session.py` | Command interpreter, frame loop, transcript, assertions |
| `re2sim/render.py` | Screenshots: background decoding (ADT/ROOMCUT.BIN), camera projection, overlays, top-down map, PNG writer |
| `re2sim/mapdata.py` | Static door graph: arrival points into every room |
| `re2sim/room_names.json` | Room id to spoken name. Mostly empty; fill in as rooms are confirmed |
| `tests/` | Scenarios, room sweep, auto-player |

## What the simulator learned about RE2 (verify each in the real game)

- **Facing direction.** Forward = (cos t, −sin t) with t = dir/4096·2π. This is calibrated
  from 281 door arrivals. North is taken as +Z, so increasing dir turns right; the
  handedness still needs a check in game.
- **Units and floors.** About 1000 units per metre. Height bands are 1800 units (y is
  negative going up); collision floor masks are per band. A storey is about 4 bands, for
  example the RPD 2F corridor (room 110) is at y = −7200. Stairs (shape 12) and slopes
  (shape 11) rise `(type >> 6) & 31` bands from their lowest masked band.
- **Collision flags (from bio2.exe).** An SCA entry pushes an object out when its flag word
  has one of the object's bits and not bit 0x0100 (0x4CC730 -> 0x4E03B0). The player uses bit
  0x8000 (0x4C0486), zombies 0x0400, other enemies 0x1000. The player's climb check (0x4CDE9A)
  uses 0x8000 too; the 0x1000/0x0800 one at 0x4CE179 is another character's.
  Entries without 0x8000 stop enemies but not the player (the top landing of the stairs in room
  10C is one). Boxes push a square body of half-width player+0x90 (0x1C2 = 450) out across the
  edge it came through (0x4E0A00), which is how the player slides along walls; the simulator's
  `push_out` copies that. Scripts toggle passages with `sca_id_set` (byte 1 = entry index,
  bytes 2-3 = new flag high bits; 0x0000 = open, 0xFC00 = solid). Shapes 4 and 5 and 13-15 are
  not understood yet and are treated as walkable.
- **Floors for navigation.** Above floor 0, a place is floor only where something on the floor
  below (or stairs or a slope) lies under it; otherwise the open air above a lower level looks
  walkable. This holds for 69 of the 71 door arrivals above floor 0.
- **Room objects.** OBJECT_WORK[32] at 0x98A61C, 0x1F8 bytes each (obj_model_set 0x4E56B0):
  +0 flags (bit 0 active), +0x38/+0x3C/+0x40 position, +0x76 direction, +0x106 floor, +0x0B the
  AOT the object stands in this frame (script member 9). Every frame the game checks AOTs with
  sat bit 0x04 against each object (0x4EA0D0), so a statue on its floor mark runs the mark's
  event. Script `work_set(4, n)` selects object n.
- **Camera zones (RVD).** From the current camera, check zones with cam0 = this camera and
  cam1 ≠ 0, plus the last cam1 = 0 zone when there are two or more. Scripts wait on `V_CUT`,
  so getting this wrong freezes events (the alley gate in room 102 depends on it).
- **Yes/no prompts.** Yes is stored as flag group 11 index 31 = 0.
- **Event and auto AOTs.** Event data is (255, 0, 0x18, proc): a gosub into a main-script
  procedure. `flag_chg` AOTs are item-use spots: they set flag (d0, d2) to d4 while the
  player is in them.
- **Hidden doors.** Scripts park doors they trigger themselves with `aot_on` at zero size,
  at (20000, 20000) or at (0, 0). Those are not real exits.
- **Re-typed doors.** Scripts often turn a door into an event (locked, "won't open",
  cutscene). The mod names these by looking up the room file's door list.
- **Parked enemies.** Enemies that will burst in later wait outside the room bounds
  (−32000 or 20000, 20000).
- **Subtitles.** The English PC cutscene subtitles are empty strings; the voice is audio.
- **`plc_dest`.** Mode 21 means turn to a direction; mode 20 means stop; other modes walk to
  (x, z). Arg 2 is the FG_ROOM flag set on arrival, and scripts wait on it.
- **Door locks.** Door data is (lock_id, lock_type): 0 = open, 254 = unlock from this side,
  255 = locked, any other value = key item id. Unlocked state is FG_KEY[lock_id].

## Sourcenext (rebirth) data compared with the English data

Checked on 2026-10-17 against the Classic REbirth install made by the setup script.

- **Same rooms, same layout.** All 252 story rooms (125 Leon, 127 Claire) exist in both.
  Doors are identical in every room. Collision differs in only 2 rooms, cameras in 4.
- **Items and enemies are almost identical.** Room 30C has one extra item in the English
  data that the Sourcenext data lacks. In room 605 the two enemies start at a different
  height and pose.
- **Scripts differ in detail.** 168 rooms have different main scripts, mostly background
  music tables (`sce_bgm_control`), procedures being renumbered, and a few
  `nop_8a`–`nop_8c` instructions. None of it changed the test or sweep results.
- **English text is present.** The Sourcenext room files carry English messages as well as
  Japanese, which is where Classic REbirth's English comes from.
- **A second room set, A–G.** The Sourcenext data has 250 more files named ROOMA000 to
  ROOMG..., one per story room (A = stage 1 ... G = stage 7). They have only 2 script
  procedures and no messages; most likely the Extreme Battle mini-game. The simulator's story
  logic ignores them, but the loader can open them.
- **Results on rebirth data are the same as on English:** 35/35 regression checks, room
  sweep Leon 206/212 and Claire 204/213, and the same autoplay route to room 202.
- **bio2.exe has no version resource.** Identify the build by SHA-256:
  `00DD7AAB0AE5D5F6AFEF1CB4A9071030C2E8D4EDD00E5A24302A70C800C3C232` (Sourcenext 1.1.0 with
  the 4 GB patch, as installed on 2026-10-17). Memory addresses found for the DLL apply to
  this build.

## Mod design findings from the simulator

1. Out-of-ammo needs speech: "Magazine empty" if spare ammo exists, otherwise "Out of ammo".
   There is also a 3-rounds-left warning.
2. Auto-aim only covers ±90°, so attackers behind the player go unanswered. A `target`
   hotkey turns toward the nearest enemy.
3. Some exits only open after an invisible trigger elsewhere in the room (room 102's gate).
   "No path" is not enough, so `go` walks as close as possible and says so, and `explore`
   walks to unvisited reachable areas.
4. Doors usually sit inside walls. Auto-walk must stop at the closest free spot, not the
   middle of the trigger.
5. Height bands are much smaller than storeys. Announce stairs as "Stairs" then "Top/Bottom
   of the stairs", not every band change.
6. Auto-walk must turn on the spot before moving and re-plan after stairs or climbing, and
   it must climb and drop crates (room 118).
7. "Thing in front of you" announcements are chatty while walking. Consider announcing only
   when stopped, or using a sound cue instead.
8. Room names are not in the game data, so `room_names.json` needs filling.

## Current coverage

- Room sweep: Leon 206 of 212 exits reached and used, Claire 204 of 213. The rest involve
  ladders, manholes, lifts or story-locked doors, or rooms 305 and 703 (scripted
  repositioning and multi-storey).
- Auto-player (Leon A): 100 → 101 → 102 → 118 → 119 → 11A → 11B → 103 → 200 → 202, using only
  mod hotkeys.
- Regression scenarios: 35 of 35 checks pass.

## Fidelity: what is approximated (reported as `[sim note]`)

- The new-game start position is chosen by the simulator.
- Enemy AI, damage numbers, enemy HP and hit detection are simplified. There is no
  animation, so cutscenes run on timers. A watchdog releases the player if scripts sit idle
  while the player is frozen.
- `work_copy` and indexed flag checks, `member_cmp` on unmodelled members, drawers
  (sce 13), windows (sce 14), water and the partner character are approximate or missing.
- Navigation is per floor band with re-planning; true multi-storey routing (ladders, lifts)
  is not done.
- Climb-up/drop-down geometry (shapes 9 and 10) is approximate.
- Messages with item-name insertion show `<item>`.

## Porting to the DLL

The mod reads `GameView`. For each property, find the RE2 memory equivalent (see
`MEMORY_TODO`) or parse the RDT on disk with the same logic as `rdt.py`. Keep the same
hotkey names so scenarios stay meaningful. When the DLL exists, scenarios can be replayed
against the real game to compare transcripts.

References used: IntelOrca's biohazard-utils / BioRand (opcode tables, RDT layout) and
OpenBiohazard2 (collision shapes, camera switches, script semantics, background image
decompression and camera projection; MIT licence, Samuel Yuan).

## Running the real mod in the simulator

`python -m re2sim --mod cpp` runs the actual mod code (the `Mod` folder next to this one) instead of the Python prototype. The simulator lays out the game's memory as in bio2.exe each frame and passes it to `Mod\tests\harness.exe`, which runs the mod and returns its speech and the buttons it holds.

- `python tests\run_tests.py --cpp` runs the mod scenarios in `tests\mod_scenarios` (61 checks).
- `--mod-log` shows the mod's log lines.
- Mod keys in the simulator: `next`, `prev`, `cat`, `catprev`, `where`, `go`, `face`, `target`, `targetnext`, `room`, `health`, `inv`, `enemies`, `scan`, `compass`, `explore`, `repeat`, `help`, `version`, `diag`.
- Menus: `ui title`, `ui status`, `ui itembox`, `ui files`, `ui file N`, `ui save`, `ui load`, `ui slot N`, `ui cursor N`, `ui submenu N`, `ui yesno N`, `ui close`.

## Menus driven like the game (C++ mod)

`re2sim/menus.py` models the game's menus from its code, and the C++ mod reads them at the real addresses:

- `menu status` (inventory key) or `menu statusmenu`, `menu save N`, `menu load N`, `menu computer`, `menu close`.
- `press up|down|left|right|action|cancel [times]` moves through the open menu the way the game does.
- `type TEXT` types at a computer's name prompt.
- `menushot [name]` saves a picture of the menu as laid out on screen, with what the mod said underneath, and lists the screen lines (highlighted parts in brackets) in the transcript, so the two can be compared.
- Scenarios `05_status_files`, `09_save_screen` and `10_computer` use them.

## Additions from the 2026-10-18 playthrough

- **Snapshots:** `snap NAME` / `restore NAME` pickle the whole game to `snaps/NAME.pkl`.
  Playthrough snapshots a35-a40 cover 30A after the G fight to room 400 (the sewers).
- **Cheats for skipped puzzles:** `retrieve ITEM` (from the item box), `combine A + B`.
- **Engine behaviour now modelled:** main procedure 1 every frame; script variable 0 (AOT
  used) and 2 (item used, cleared each frame except right after use); work_copy; AOTs attached
  to objects or enemies (sup 0x80/0x40) follow them; player speed_set/add_speed/add_aspeed;
  aot_reset on a door patches its destination; V_LAST_RDT = stage*0x100+room; the live SCA
  table exported for the mod; active stairs and stair undersides in collision.
- **Timeouts for engine-only values:** member_cmp on unmodelled members and cmp on script
  variable 1 give up after a second, so cutscenes that wait on animation or the engine's cut
  (30A's Sherry cutscene) finish.
- `info` threads show procedure numbers and program counters.
- Coverage: tests/mod_scenarios 187/187 (C++ mod), tests/scenarios 35/35.

## Additions from the 2026-10-19 session (mod 0.6.1)

- **Objects collide.** Room objects carry a collision box from obj_model_set (OBJECT_WORK
  +0x94/+0x96/+0x98 offset x/z/y, +0x9A/+0x9C/+0x9E sizes; bio2.exe 0x4CD610 builds the box
  every frame: centre = position + offset, half sizes, x and z swapped when the direction has
  bit 0x400). The sim's player collides with them, memexport writes them, and object speed
  slots 0 and 2 move objects across the floor (the library's sliding shelves, 900 units a place).
- **Climbable objects** (be_flg 0x100, the archive room's stepladder): action facing one climbs
  on top; action on top facing out gets down (approximate).
- **AOT floor check:** an AOT floor byte below 0x80 must equal the player's floor
  (bio2.exe 0x4E921B), as in the game.
- Tests: tests/mod_scenarios 203/203 (new 19_archive_ladder; 17 now plays the library the real
  way, through the weak floor).

## Additions from the 2026-10-20 session (mod 0.7.0)

- **Key flags:** a door's lock id byte carries bit 0x80; the unlocked flag is FG_KEY[id & 0x7F]
  (checked against a real save). Type 0xFF doors open once that flag is set (unlocked from the
  other side or by a script). The sim follows both.
- **FG_MAP:** entering a room sets FG_MAP bit (0, 30, 58, 72, 89, 99, 123)[stage] + room
  (bio2.exe 0x4E4040), the game map's "been here".
- **New commands:** `mapkey` (the mod's M) and `mapmode` (Shift+M) drive the map.
- Tests: tests/mod_scenarios 217/217 (new 20_water_tank, 21_unlocked_doors, 22_map).

## Stairs, climbing and ladders (2026-10-20, from bio2.exe and the user's play)

- **Stairs on a screen** (SCA shape 12, e.g. 116's fire escape, 112's library stairs) are
  walls: the collision push table at 0x53AD00 sends shape 12 to the box push 0x4E0A00, and the
  player's move (0x4CC730, include bit 0x8000) applies it whenever the player's floor is in the
  stair's mask. The player takes them by lining up at an end, facing along them, and pressing
  action; then the height follows the stair (0x4E2680) until they step off the far end. The
  trigger check 0x4E03B0 reports stairs as result bit 0x10 (entry kept at 0x695E7C); the feet
  check at 0x4C04C4 puts it in +0x112. Their mouths can be only a body wide (116: 569 units
  between the landing rail and the far rail), so the approach has to be straight and centred.
  The sim models this: stairs block until `stair_ahead()`/action, `p.stair` while on them.
- **Slopes** (shape 11) have their own push routine (0x4E24C0) and are walked.
- **Stairs between rooms** are doors (door AOTs, sometimes an event with "Will you go up the
  stairs?"): action at the door, a yes/no, then a room change. They are not collision stairs.
- **Climb boxes and drop ledges** (shapes 9 and 10, the player's bit 0x8000): action facing the
  edge (0x4CDE9A calls 0x4E03B0 with include 0x8000 and the player's floor bit, so only boxes
  on the player's own level count; shape 9 returns 3 = climb up, shape 10 returns 4 = drop).
  603's step up from the fallen crate has only 0x8000. Scripts can set an object's floor with
  member_set 17 (603's crate after its fall).
- **Ladders** are events (a question, then a scripted move or a door to another room).
- **Rise** of a stairway = highest minus lowest floor in its mask (116's long flight: 1 to 4).
- **The player's floor byte** (+0x106) is what AOT floor bytes are matched against but is not
  the level stood on near stairs; plan by height (floor = round(-y / 1800)).

## Options menu, map key, stair doors (2026-10-20, 0.7.9)

- **Ctrl = the PC options menu**, not a status page. The in-game check at bio2.exe 0x4C0302
  reads the raw pad (0x9885FE): raw 0x100 (Ctrl) sets System_flg 0x08000000 and the task at
  0x449FD0 runs the options menu (0x44A0B0 input, 0x44B180 drawing; the title's Option uses the
  same code from 0x505400). State byte 0x99D001: 1 top menu (cursor 0x99CF68: key config,
  brightness, sound, exit), 9-12 key config (row 0x68915C; rows map to KeyDef indices
  0,1,2,3,10,11,5,13,12,7 = up, down, left, right, aim, action, run/cancel, status, options, map;
  row 10 Exit/Default via 0x6893C8; the edited KeyDef is at 0x99CF40), 15 keep the keys?,
  3/7 the brightness test picture (common\data\color2.adt), 4/8 sound (speaker 0x669CFC
  1 mono / 2 stereo, quality from 0x669F40 bits and 0x669D9C rate, music volume 0x693471,
  effects volume 0x693C48). `menu options` in the sim drives a model of it (OptionsModel);
  `press vk:5A` presses a key while one is being assigned.
- **A = the map key**: raw 0x10 is game bit 0x4000 (remap table 0x5338D8 per key type); with
  it the status screen opens as type 3 (ST_TYPE 0x691F70 = 3, init 0x4FB5E0): the map alone.
  Type 4 is a script showing the map (0x4E8FE0). Z (raw 0x800) opens the normal status screen.
  `menu map` in the sim. The sim's `statusmenu` now uses type 0 (it wrongly used 3).
- **Stair doors:** a door AOT's animation byte (IN_DOOR_WORK +13, the 19th door_aot_se
  argument) is 4 for a flight up and 5 for a flight down (207 -> 10C, 10C -> 207, 200's stairs to
  2F, 112, 211, ...). The mod names these "Stairs up to ..." / "Stairs down to ...".
- **20D faucets:** the fireplace is an item-use AOT (8, the Lighter, item 47); flag 5.10 = lit.
  Faucets 12, 13, 11 are AOTs 4, 5, 6; var 4 counts them turned in order (0..3).
- Tests: tests/mod_scenarios 259/259 (new 25_faucets_options), tests/scenarios 35/35.
- **0.8.0: the game's map opens the mod's map.** Status type 3/4 (map key A, or a script) opens
  the mod's map (`open_map`); its keys work on that screen; backslash presses cancel (the sim's
  session closes a type 3/4 status model when the mod holds cancel) and travel starts back in
  the game. Tests: tests/mod_scenarios 267/267 (new 26_game_map).

## Second Sherry scene and the rest of the game (2026-10-21, 0.8.1-0.8.2)

- Played from the user's save (Claire A_07) with the C++ mod through the faucets, the clock
  tower, the stones in the Chief's office, the elevator, the secret room, Sherry's rescue and
  the sewers as far as the Garbage Dump (snapshots in snaps/: a07, cog, jag, stones, c21b,
  sherry, r400b, r401, r405, r407, valve, s408, r40a).
- **Sim fixes:** the status screen's Use on a non-weapon now uses the item where the player
  stands; a scripted walk (plc_dest) stops at walls and counts as arrived there (408's Annette
  scene walked the player out of the room); event AOTs set up by item_aot_set export their
  item data.
- **tests/sweep_cpp.py**: enters each room by each door that leads there, walks to every exit in
  the catalog with the C++ mod and tries it. Rooms 400-707 swept; the remaining failures are
  story gates (bridge, water, fan, keys, the ropeway and turntable) or need checking in the real
  game (616's corridor collisions).
- Tests: tests/mod_scenarios 282/282 (new 27_sewer_levels, 28_ladders_and_flights).

## Leon B playthrough and bosses (2026-10-25, mod 0.9.6)

- **Boss models** (`re2sim/bosses.py`, attached when an enemy spawns): Tyrant 1 (Mister X: life 20000 plus 300, down below 20000 for the rest of the visit, scripted entrance from above), Birkin forms 0x30/0x31/0x33/0x34/0x36/0x37 (walk and swing; Birkin 1's life 500/400 in B from 0x48A540, swings 30 and 20; the others' numbers are guesses until their routines are read), Tyrant 2 (Super Tyrant, 702: life 10000; sets his members 25 after 1500 damage and 26 after 90 seconds, which the room script waits for before Ada throws the rocket launcher). A G on a roof above the player (the sky tram, 500) reaches down only at whoever is right below. All `[sim note]`.
- **Commands:** `fire N up` aims up (an enemy high on a wall or a roof can be hit); `getaway` is the mod's Shift+E; `enemypos ID X Y Z` moves an enemy (stands in for a scripted entrance); `choose N` picks choice N of a multi-choice question (1-based). `retrieve` prefers an exact item name.
- **Fixes:** `aot_reset` of an item AOT now changes the item (111: the Knight plug instead of the Jaguar stone for Leon); several item-use spots overlapping (303's plug panel) give variable 0 the highest id, as the script expects; the player's stop flag (2.7) is cleared on entering a room (410's scene with Ada ends with it set); status flag 1.11 is set for 3 seconds by `xa_on` (scenes wait for a voice line to start, 409); a script waiting on an animation member gets the opposite answer after a second (both "while" and "if ... goto" waits end); enemies more than 2500 above the player do not melee (a spider high on a wall spits poison now and then).
- **Not modelled, so cheated past in the run:** the sky tram ride's end (teleported to 501), the turn table's descent after the G (teleported to 600), the lab's 600 walkway, the 601 elevator ride (flag 1.27 stays set), the 408/409 bridge state when arriving from the garbage dump (flag 4.30 set by hand).
