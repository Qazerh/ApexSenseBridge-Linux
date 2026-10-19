#!/usr/bin/env python3
"""Run the read-only HID probe through Steam's selected Proton, then the game."""
from pathlib import Path
import subprocess
import sys


def probe_command(command, executable):
    # Steam's expanded %command% may include the Steam Runtime and wrappers.
    # Keep that entire prefix; replace only the game executable and its args.
    verbs = [i for i, arg in enumerate(command) if arg in ("waitforexitandrun", "run")]
    for verb in verbs:
        if verb == 0 or Path(command[verb - 1]).name != "proton":
            continue
        for i in range(verb + 1, len(command)):
            if command[i].lower().endswith(".exe"):
                return command[:i] + [str(executable)]
    raise ValueError("Steam did not supply a Proton game command. Use this wrapper before %command%.")


def main():
    root = Path(__file__).resolve().parent.parent
    executable = root / "bin/ProtonHidProbe.exe"
    native_log = root / "bin/proton-hid-probe-native.log"
    log = root / "proton-hid-probe.log"
    command = sys.argv[1:]
    if not command or not executable.is_file():
        print("Use this script before Steam's %command%; bin/ProtonHidProbe.exe must exist.", file=sys.stderr)
        return 1
    try:
        probe = probe_command(command, executable)
    except ValueError as error:
        print(error, file=sys.stderr)
        return 1
    native_log.unlink(missing_ok=True)
    with log.open("w", encoding="utf-8") as output:
        output.write("Starting native HID probe through the game's selected Proton.\n")
        output.flush()
        try:
            result = subprocess.run(probe, stdout=output, stderr=subprocess.STDOUT, timeout=120)
            output.write(f"\nProbe process exit code: {result.returncode}\n")
        except subprocess.TimeoutExpired:
            output.write("\nProbe launcher exceeded 120 seconds; proceeding to the game.\n")
        if native_log.is_file():
            output.write("\n--- Native HID checker report ---\n")
            output.write(native_log.read_text(encoding="utf-8", errors="replace"))
        else:
            output.write("\nERROR: No native HID checker report was produced. Launcher exit code alone does not verify that the checker ran.\n")
    print(f"HID diagnostic saved to {log}", flush=True)
    return subprocess.call(command)


if __name__ == "__main__":
    raise SystemExit(main())
