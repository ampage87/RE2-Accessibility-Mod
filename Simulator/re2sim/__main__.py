"""Command-line entry point.

  python -m re2sim                         interactive play (type commands; 'help' lists them)
  python -m re2sim --script tests/x.txt    run a command file, print the transcript
  python -m re2sim -c "next" -c "go"       run commands given on the command line

Options: --data rebirth|english|DIR (default rebirth = the Sourcenext data Classic REbirth runs, when present), --claire, --scenario B,
         --start RID, --out FILE (save transcript), --events (show raw game events),
         --auto-dismiss (close plain messages automatically), --auto-shots (screenshot at every
         camera or room change), --shots-dir DIR.
"""
import argparse
import os
import sys

from .session import Session, __doc__ as COMMANDS


def _extract(root, name):
    """Unpacks Simulator/<name>.zip to Simulator/<name>/ on first use."""
    zp = os.path.join(root, name + ".zip")
    if not os.path.isdir(os.path.join(root, name, "Pl0")) and os.path.exists(zp):
        import zipfile
        print(f"Extracting {name}.zip ...", file=sys.stderr)
        with zipfile.ZipFile(zp) as z:
            z.extractall(os.path.join(root, name))


def find_data(explicit=None):
    """Game data folder. explicit (or the RE2SIM_DATA environment variable) may be a folder,
    'english' (US PC data, rooms in PlN/Rdu) or 'rebirth' (the Sourcenext data that Classic
    REbirth runs, rooms in PlN/Rdt). Default: rebirth when its data is present, else english."""
    here = os.path.dirname(os.path.abspath(__file__))
    root = os.path.abspath(os.path.join(here, ".."))
    default = "rebirth" if (os.path.exists(os.path.join(root, "gamedata-rebirth.zip"))
                            or os.path.isdir(os.path.join(root, "gamedata-rebirth", "Pl0"))) else "english"
    choice = explicit or os.environ.get("RE2SIM_DATA") or default
    if choice.lower() in ("english", "rebirth"):
        name = "gamedata" if choice.lower() == "english" else "gamedata-rebirth"
        _extract(root, name)
        sub = "4249110_Biohazard2/english" if name == "gamedata" else "rebirth"
        candidates = [os.path.join(root, name), os.path.join(root, "..", "Game Files", sub)]
    else:
        candidates = [choice]
    for c in candidates:
        if os.path.isdir(os.path.join(c, "Pl0")):
            return os.path.abspath(c)
    sys.exit(f"Could not find game data ({choice}). Pass --data english, --data rebirth, or a folder "
             "that contains Pl0 and Pl1.")


def main(argv=None):
    ap = argparse.ArgumentParser(prog="re2sim", description="Resident Evil 2 accessibility-mod simulator")
    ap.add_argument("--data")
    ap.add_argument("--claire", action="store_true")
    ap.add_argument("--scenario", default="A", choices=["A", "B", "a", "b"])
    ap.add_argument("--start")
    ap.add_argument("--script")
    ap.add_argument("-c", "--cmd", action="append", default=[])
    ap.add_argument("--out")
    ap.add_argument("--events", action="store_true")
    ap.add_argument("--auto-dismiss", action="store_true")
    ap.add_argument("--seed", type=int, default=1)
    ap.add_argument("--mod", choices=["python", "cpp"], default=None,
                    help="which mod drives speech: the Python prototype or the real C++ mod (needs the harness)")
    ap.add_argument("--mod-log", action="store_true", help="show the C++ mod's log lines in the transcript")
    ap.add_argument("--auto-shots", action="store_true", help="save a screenshot at every camera or room change")
    ap.add_argument("--shots-dir", help="folder for screenshots (default Simulator/shots)")
    a = ap.parse_args(argv)

    s = Session(find_data(a.data), character=1 if a.claire else 0, scenario=0 if a.scenario.upper() == "A" else 1,
                start=a.start, show_events=a.events, auto_dismiss=a.auto_dismiss, seed=a.seed,
                shot_dir=a.shots_dir, auto_shots=a.auto_shots, mod_impl=a.mod, show_mod_log=a.mod_log)
    cmds = list(a.cmd)
    if a.script:
        with open(a.script, encoding="utf-8") as f:
            cmds += f.read().splitlines()
    if cmds:
        for c in cmds:
            s.run(c)
    else:
        print("RE2 simulator. Type 'help' for commands, 'quit' to exit.")
        while True:
            try:
                line = input("> ")
            except EOFError:
                break
            if line.strip() in ("quit", "exit"):
                break
            if line.strip() == "help":
                print(COMMANDS)
                continue
            s.echo = False
            s.run(line)
            # print only what this command produced (minus the echoed command)
            for l in s.lines[s._printed if hasattr(s, "_printed") else 0:]:
                if not l.startswith("> "):
                    print(l)
            s._printed = len(s.lines)
    if a.out:
        with open(a.out, "w", encoding="utf-8") as f:
            f.write("\n".join(s.lines) + "\n")


if __name__ == "__main__":
    main()
