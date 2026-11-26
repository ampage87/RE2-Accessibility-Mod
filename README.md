# Resident Evil 2 Accessibility Mod (RE2Access)

A free accessibility mod for the original **Resident Evil 2** (1998) on PC that makes the whole game playable for blind and low-vision players. It speaks through your screen reader, describes the movies, tells you what's around you, walks you there on request, and helps with the puzzles.

**Version:** 0.9.25 (active development, pre-release)

Watch the full playthrough with this mod on The Blind Page: [Resident Evil 2 Accessibility Mod playlist](https://www.youtube.com/playlist?list=PLGBbGPD-H74w)

---

## For Players

### What this mod does

- **Speech through your screen reader.** The mod talks through JAWS, NVDA or another running screen reader (using the Prism library), and braille works too. With no screen reader running it uses Windows SAPI. Shift+F1 switches between them.
- **Everything on screen is read.** Title menus, the status screen and inventory, item box, save and load screens, messages and Yes or No questions, documents you pick up, computer terminals, the options menu, the results screens and the bonus modes.
- **Audio description of every movie.** All 26 pre-rendered movies (the openings, the car crash, the Licker, Birkin, Mister X, the train escapes, the endings and more) have recorded descriptions that play in the gaps between the dialogue. The narrated introduction before a new game is described too.
- **A catalog of the room.** Browse the exits, items, objects and enemies around you, nearest first. Anything that appears later, like a document from the printer or a jewel a puzzle releases, is announced and added.
- **Directions and auto-walk.** Get step-by-step directions to anything in the catalog, or let the mod walk you there. Auto-walk runs on long stretches, takes stairs, ladders and ledges, steers around danger areas and awake enemies, and keeps a following partner like Sherry or Ada in range.
- **A full map of the game.** Every room in every area, which ones you've visited, their doors and locks, the items left in them and where the typewriters and item boxes are. Pick a room and the mod travels there for you, door by door.
- **Puzzle help.** The statue push puzzle, the archive room crank, the library shelves, the reserve power panel, Sherry's water tank, the faucets and more. Each one is explained, and where a puzzle is purely visual the mod can solve that part for you.
- **Combat awareness.** Enemies coming close, what you're aiming at and how far away, hits and kills, poison, your condition (Fine, Caution, Danger) the moment it changes, and guidance in boss fights. Shift+E gets you away from the nearest enemy.
- **Optional assists.** Unlimited health (F5) and unlimited ammo (F6), each off by default and remembered between sessions. Use them all the time, only for boss fights, or not at all.
- **Sound that stays clear.** Speech volume up to 200 percent, and the game's sound drops while the mod talks (ducking), so speech is easy to hear.

### Requirements

- **Resident Evil 2 (1998)** from Steam (app 4249110). The setup script converts it to the Sourcenext edition with Classic REbirth, which this mod is built for. The game isn't included in this repository.
- **Windows 10 or 11.**
- **About 12 GB of free space** for the setup.
- **A screen reader is optional.** The mod uses JAWS, NVDA or another screen reader when one is running, and Windows SAPI otherwise.
- **Visual Studio is optional.** Deploy builds the mod with it if it's installed, and uses the prebuilt copy if not.

### Installation

Every step works with the keyboard and a screen reader. There are no mouse-only or visual steps.

1. **Install Resident Evil 2 from Steam.** Make sure the game is closed afterwards.
2. **Download this mod.** Clone the repository, or use Code, then Download ZIP on this page and extract it.
3. **Run `Install Sourcenext and Classic REbirth.cmd`.** It finds the Steam game, downloads the open-source Resident Evil 2 Steam Enhancement Pack from Mulderland, and installs it silently, answering every pop-up for you. That gives you the Sourcenext edition, Classic REbirth, the 4 GB memory patch, the high-quality videos and the ASI loader the mod needs. It then turns off the Classic REbirth settings window, starts the game straight in English, adds desktop and Start menu shortcuts, and writes `Game Install Info.txt`.
4. **Run `Deploy.vbs`.** It builds or copies the mod into the game folder and checks every file. A message box says whether it worked, and the details are in `Logs\deploy_latest.log`. If Windows asks for permission, answer Yes.
5. **Start the game** from the "Resident Evil 2 Classic REbirth" shortcut. You should hear "Resident Evil 2 accessibility mod loaded."

If setup goes wrong, run the scripts in `Tools\Setup troubleshooting`. `Diagnose setup.cmd` reports what it finds.

### Main keys

The game's own controls are unchanged. A few of the mod's keys:

| Key | Action |
|---|---|
| `-` and `=` | Previous and next thing in the catalog |
| `[` and `]` | Catalog category: All, Exits, Items, Objects, Enemies |
| Backspace | Directions to the selected thing |
| `\` | Walk there automatically (Shift+`\` to turn and face it) |
| `.` | Explore: walk to the nearest place not visited yet |
| T | Turn to the nearest enemy (Shift+T for the next one) |
| Shift+E | Get away from the nearest enemy |
| E | Enemies nearby |
| `'` | Where am I |
| H | Condition |
| I | Inventory |
| M | Map of this area from where you are |
| Shift+M | Map of every room, with travel |
| Y | Time left on an escape countdown |
| `` ` `` | Repeat the last thing spoken |
| `/` | Key help |
| F5 / F6 | Unlimited health / unlimited ammo on or off |
| Shift+F1 | Speech output: Automatic, a screen reader, or SAPI |
| F11 | Write a diagnostic snapshot to the log |

The full key list, settings, puzzle help, everything spoken automatically and the changelog are in [Mod/README.md](Mod/README.md).

A tip: in the game, the Ctrl key opens the options menu, so don't use it to stop your screen reader talking while you play. The mod's own keys already interrupt speech.

### Known issues

- In-game documents aren't read in the version on GitHub yet. Their text is Capcom's, so it isn't published here. The mod still works fully otherwise, and documents say "Its text is not available." Building that text from your own copy of the game is planned in [issue #2](https://github.com/ampage87/RE2-Accessibility-Mod/issues/2).
- Some parts were built and tested in the simulator but haven't been checked in the real game yet. They're listed under "Known gaps" in [Mod/README.md](Mod/README.md).
- Everything else that's known is tracked in [Issues](https://github.com/ampage87/RE2-Accessibility-Mod/issues).

### Reporting a bug

Open an [issue](https://github.com/ampage87/RE2-Accessibility-Mod/issues). If something isn't read or goes wrong, press F11 while it's on screen, then attach `Logs\RE2Access.log` from the mod folder.

---

## Supporting This Project

The Resident Evil 2 Accessibility Mod is free and always will be. However, development takes significant time and effort, and the AI tools used in building it (such as Claude by Anthropic) have ongoing costs.

If you find this mod valuable, there are several ways you can help:

**Review the audio descriptions.** If you can see, watch the game's movies with the audio description on and report anything that's inaccurate, missing or out of step with the video. No technical skills needed.

**Report bugs and test.** Play through the game and report any issues you run into. Reports from parts of the game the playthrough hasn't reached yet are especially valuable.

**Make a donation.** If you're able and willing, you can sponsor this project through [GitHub Sponsors](https://github.com/sponsors/ampage87). Every contribution helps keep development going.

Thank you to everyone who has supported this project in any way.

---

## For Developers

### How it works

The mod is `RE2Access.asi`, a 32-bit DLL loaded into the Sourcenext `bio2.exe` by the Ultimate ASI Loader (`dsound.dll`). It reads the game's memory and room files to know where you are, what's in the room and what's on screen, and speaks through Prism or SAPI. Auto-walk follows the game's own collision rules, read from the room files, and steers by feeding the game's input. The audio description is a set of WAV recordings and cue files per movie, played on the mod's own sound stream and timed to the movie.

### Project structure

```
Deploy.vbs                    Builds the mod and installs it into the game
Install Sourcenext and Classic REbirth.cmd   One-step, screen-reader-friendly game setup
Mod/
  src/                        C++ source: win_main.cpp is the Windows host;
                              mod.cpp, state.cpp, room.cpp, map.cpp hold the shared logic
  src/deploy.ps1, deploy.bat  The build and install steps Deploy.vbs runs
  data/                       Room names, menu labels, and movies/ (audio description
                              cue files and WAV recordings for each movie)
  build/prebuilt/             A ready-built RE2Access.asi for when Visual Studio isn't installed
  tests/                      harness.cpp runs the real mod code inside the simulator
  tools/gen_tables.py         Regenerates src/tables.inc from the simulator's tables
  third_party/prism/          Prism screen reader library (MPL-2.0)
Simulator/                    Python simulator of the game for testing the mod
Tools/                        Scripts that copy game files, movies and HD text images
                              from your install for the simulator and mod tools
```

### Building

Install Visual Studio (or the Build Tools) with the C++ desktop workload, then run `Deploy.vbs`. It builds the 32-bit DLL, copies it, the data and Prism into the game folder, and verifies every file.

### The simulator

`Simulator` is a text-mode simulator of Resident Evil 2 written in plain Python 3, with no installs needed. It loads the real room files and scripts from your own copy of the game and runs them at 30 frames per second with real collision, doors, items and messages. The mod's logic is designed and tested there before it goes into the DLL, and `Mod/tests/harness.cpp` runs the real C++ mod code inside it.

The simulator needs game data from your install. Use the scripts in `Tools` to copy it, then see [Simulator/README.md](Simulator/README.md).

### Contributing

Contributions are welcome, especially:

- **Audio description review:** watch the movies and report inaccuracies. This is the easiest way to help.
- **Testing:** play through and report issues, especially in Leon's scenario and the B scenarios.
- **Code:** C++ for the mod and Python for the simulator. If you're comfortable with memory reading and game reverse engineering, there's plenty to do.

Please open an issue to discuss before starting work on a contribution.

---

## Acknowledgments

- **[Prism](https://github.com/ethindp/prism)**, the screen reader and braille library the mod speaks through, under the Mozilla Public License 2.0 (see `Mod/third_party/prism/LICENSE-Prism.txt` and `NOTICE-Prism.txt`).
- **[MulderLoad](https://github.com/Mulderland/MulderLoad)** by Mulderland, whose Resident Evil 2 Steam Enhancement Pack the setup script installs.
- **Classic REbirth**, which makes the original game run well on modern Windows.
- **Ultimate ASI Loader**, which loads the mod into the game.
- **The Seamless HD Project**, for the HD backgrounds and text images.
- The Resident Evil modding and research community, for years of work documenting the game's files and engine.

Resident Evil 2 is a trademark of Capcom. This project isn't affiliated with or endorsed by Capcom and includes none of the game's files.
