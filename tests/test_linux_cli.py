"""Exercise public CLI contracts without opening or configuring a controller."""
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile

binary = Path(sys.argv[1]).resolve()


def run(*args):
    return subprocess.run([str(binary), *args], text=True, capture_output=True, timeout=10)


for args in [
    ("bridge", "--gyro-strength", "401"),
    ("bridge", "--gyro-strength", "-1"),
    ("bridge", "--gyro-yaw", "-1"),
    ("bridge", "--rumble-strength", "201"),
    ("test-rumble", "--left", "256"),
    ("test-triggers", "--mode", "invalid"),
    ("test-triggers", "--side", "invalid"),
    ("test-triggers", "--level", "5"),
    ("test-rgb", "--color", "12345z"),
    ("test-gyro", "--seconds", "31"),
    ("bridge", "--apex-profile", "5"),
    ("bridge", "--touchpad-profile", "invalid"),
    ("identify", "--telemetry-json", "unused.json"),
    ("bridge", "--trace-hid", "same.json", "--telemetry-json", "./same.json"),
    ("bridge", "--seconds", "86401"),
    ("--device",),
]:
    result = run(*args)
    assert result.returncode == 1 and "Error:" in result.stderr, (args, result)
    assert "No matching" not in result.stderr and "Cannot open" not in result.stderr, (args, result)

assert "Linux v9" in run("--version").stdout
assert "--touchpad-profile" in run("--help").stdout
assert "--verbose" in run("--help").stdout
read_fd, write_fd = os.pipe()
os.close(read_fd)
try:
    closed_pipe = subprocess.run([str(binary), "--help"], stdout=write_fd, stderr=subprocess.PIPE, timeout=10)
    assert closed_pipe.returncode == 0  # SIGPIPE follows the graceful-stop path.
finally:
    os.close(write_fd)
listed = run("list", "--json", "--verbose")
assert listed.returncode == 0, listed
assert isinstance(json.loads(listed.stdout), list)
diagnostic = run("diagnose", "--json")
assert diagnostic.returncode == 0, diagnostic
info = json.loads(diagnostic.stdout)
assert isinstance(info["uhid_read_write"], bool)
assert isinstance(info["devices"], list)

with tempfile.TemporaryDirectory(prefix="asb-cli-") as directory:
    status = Path(directory) / "status.json"
    # This path cannot match an enumerated /dev/hidrawN candidate. No HID opens.
    result = run("bridge", "--device", "/__asb_nonexistent_test_device__", "--telemetry-json", str(status))
    assert result.returncode == 1 and "No matching" in result.stderr, result
    snapshot = json.loads(status.read_text())
    assert snapshot["phase"] == "error" and "No matching" in snapshot["error"]
    assert snapshot["cleanup_ok"] is False and snapshot["build"] == "linux-v9"
    assert list(Path(directory).iterdir()) == [status]  # No partial snapshot left behind.
    bad_status = Path(directory) / "missing" / "status.json"
    result = run("bridge", "--device", "/__asb_nonexistent_test_device__", "--telemetry-json", str(bad_status))
    assert result.returncode == 1 and "telemetry" in result.stderr.lower()
    assert "No matching" not in result.stderr  # Fail before controller enumeration/settings.

print("Linux CLI aliases, validation, JSON diagnostics and atomic error snapshots passed")
