# Resident Evil 2 Accessibility Mod (RE2Access)

A free mod that makes the original Resident Evil 2 (1998) on PC playable for blind and low-vision players.

## What it does

- Speaks through your screen reader (JAWS, NVDA and others, via the Prism library), or through Windows SAPI when no screen reader is running.
- Reads menus, inventory, files, messages, questions and the results screens.
- Audio description for all of the game's pre-rendered movies and the narrated introduction.
- A catalog of what's in each room (exits, items, objects, enemies), with step-by-step directions and auto-walk.
- A full map of every room, with travel from room to room.
- Help with the puzzles, enemy alerts and boss fight guidance.
- Optional unlimited health and unlimited ammo, each turned on or off with one key.

The full list of keys, settings and changes is in [Mod/README.md](Mod/README.md).

## What you need

- Resident Evil 2, the Sourcenext PC release, with Classic REbirth. The game is not included in this repository.
- Windows.
- Visual Studio is optional. Without it, Deploy uses a prebuilt copy of the mod.

## Install

1. Run `Install Sourcenext and Classic REbirth.cmd` to set up the game, Classic REbirth and the ASI loader. It writes `Game Install Info.txt`, which Deploy uses to find the game.
2. Run `Deploy.vbs`. It builds the mod (or uses the prebuilt copy) and copies it into the game folder. A message box says whether it worked.
3. Start the game. You should hear "Resident Evil 2 accessibility mod loaded."

If setup goes wrong, the scripts in `Tools\Setup troubleshooting` can diagnose it.

## Folders

- `Mod`: the mod itself. `src` holds the C++ source, `data` holds the room names, texts and audio description recordings, and `third_party\prism` holds the Prism screen reader library.
- `Simulator`: a Python simulator of the game used to test the mod without running the game. The game data it needs is copied from your own install with the scripts in `Tools` and is not included here.
- `Tools`: helper scripts for copying game files, movies and HD text images from your install.

## Reporting bugs and requesting features

Please use [Issues](https://github.com/ampage87/RE2-Accessibility-Mod/issues). If something isn't read or goes wrong, press F11 while it's on screen and attach `Logs\RE2Access.log`.

## Watch it played

The Blind Page on YouTube has a full playthrough with this mod: [Resident Evil 2 Accessibility Mod playlist](https://www.youtube.com/playlist?list=PLGBbGPD-H74w)

## Credits

- Prism screen reader library, https://github.com/ethindp/prism, under the Mozilla Public License 2.0 (see `Mod/third_party/prism/LICENSE-Prism.txt`).
- Resident Evil 2 is a trademark of Capcom. This project is not affiliated with Capcom and includes none of the game's files.
