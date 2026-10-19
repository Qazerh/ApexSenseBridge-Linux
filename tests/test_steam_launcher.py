"""Integration tests for the installed Steam wrapper using owned fake processes."""
import json
import os
from pathlib import Path
import signal
import subprocess
import sys
import tempfile
import time

launcher = Path(sys.argv[1]).resolve()
help_result = subprocess.run([sys.executable, str(launcher), "--help"], capture_output=True, text=True, timeout=4)
assert help_result.returncode == 0 and "%command%" in help_result.stdout and not help_result.stderr, help_result
with tempfile.TemporaryDirectory(prefix="asb-launcher-") as folder:
    root = Path(folder)
    bridge = root / "fake bridge"
    bridge.write_text('''#!/usr/bin/env python3
import json, os, pathlib, signal, sys, time
root = pathlib.Path(os.environ['ASB_TEST_ROOT'])
def stop(*_):
    (root / 'bridge-stop').touch()
    sys.exit(0)
signal.signal(signal.SIGTERM, stop)
signal.signal(signal.SIGINT, stop)
(root / 'bridge-start.json').write_text(json.dumps({'args':sys.argv[1:], 'preload':os.environ.get('LD_PRELOAD'), 'libs':os.environ.get('LD_LIBRARY_PATH')}))
print('bridge stdout', flush=True)
print('bridge stderr', file=sys.stderr, flush=True)
if os.environ.get('ASB_TEST_FAIL'):
    sys.exit(7)
if os.environ.get('ASB_TEST_CONNECT_DURING_GAME'):
    while not (root / 'game.json').exists(): time.sleep(0.01)
    time.sleep(0.1)
    (root / 'bridge-ready').touch()
while True: time.sleep(0.02)
''')
    bridge.chmod(0o755)
    game = root / "fake game.py"
    game.write_text('''import json, os, pathlib, sys, time
root = pathlib.Path(os.environ['ASB_TEST_ROOT'])
(root / 'game.json').write_text(json.dumps({'args':sys.argv[1:], 'preload':os.environ.get('LD_PRELOAD'), 'libs':os.environ.get('LD_LIBRARY_PATH'), 'bridge_ready':(root / 'bridge-ready').exists()}))
time.sleep(float(os.environ.get('ASB_TEST_GAME_SECONDS', '0.3')))
sys.exit(int(os.environ.get('ASB_TEST_GAME_EXIT', '0')))
''')
    env = os.environ.copy()
    env.update(ASB_TEST_ROOT=str(root), XDG_STATE_HOME=str(root / 'state'), SteamAppId='123')
    # A real, harmless existing library avoids noisy loader errors. The game
    # must retain both variables; only the bridge loses Steam's runtime env.
    env['LD_PRELOAD'] = 'libm.so.6'
    env['LD_LIBRARY_PATH'] = str(root / 'steam-runtime')
    base = [sys.executable, str(launcher), '--bridge', str(bridge)]
    command = [sys.executable, str(game), 'path with spaces', '$(literal)', '--flag']

    def reset():
        for name in ('bridge-stop', 'bridge-start.json', 'game.json', 'bridge-ready'):
            (root / name).unlink(missing_ok=True)

    result = subprocess.run([*base, '--strength', '75', '--', *command], env=env, capture_output=True, text=True, timeout=8)
    assert result.returncode == 0 and not result.stdout and not result.stderr, result
    started = json.loads((root / 'bridge-start.json').read_text())
    assert started['args'] == ['bridge', '--strength', '75']
    assert started['preload'] is None and started['libs'] is None
    played = json.loads((root / 'game.json').read_text())
    assert played['args'] == ['path with spaces', '$(literal)', '--flag']
    assert played['preload'] == env['LD_PRELOAD'] and played['libs'] == env['LD_LIBRARY_PATH']
    assert (root / 'bridge-stop').exists() and not played['bridge_ready']
    assert not (root / 'state').exists()  # No default log directory or file.
    reset()
    log = root / 'logs/explicit bridge.log'
    result = subprocess.run([*base, '--log', str(log), '--', *command], env=env, capture_output=True, text=True, timeout=8)
    assert result.returncode == 0 and not result.stdout and not result.stderr, result
    assert log.read_text() == 'bridge stdout\nbridge stderr\n'
    assert (root / 'bridge-stop').exists()
    reset()
    result = subprocess.run([*base, '--', *command], env={**env, 'ASB_TEST_GAME_EXIT':'9'}, capture_output=True, text=True, timeout=8)
    assert result.returncode == 9 and (root / 'bridge-stop').exists(), result
    reset()
    result = subprocess.run([*base, '--', *command], env={**env, 'ASB_TEST_FAIL':'1'}, capture_output=True, text=True, timeout=8)
    assert result.returncode == 0 and 'status 7' in result.stderr and (root / 'game.json').exists(), result
    reset()
    result = subprocess.run([*base, '--', *command], env={**env, 'ASB_TEST_CONNECT_DURING_GAME':'1', 'ASB_TEST_GAME_SECONDS':'0.6'}, capture_output=True, text=True, timeout=8)
    assert result.returncode == 0 and not json.loads((root / 'game.json').read_text())['bridge_ready'], result
    assert (root / 'bridge-ready').exists() and (root / 'bridge-stop').exists()
    reset()
    result = subprocess.run([sys.executable, str(launcher), '--bridge', str(root / 'missing-bridge'), '--', *command], env=env, capture_output=True, text=True, timeout=8)
    assert result.returncode == 0 and 'Starting the game anyway' in result.stderr and (root / 'game.json').exists(), result
    reset()
    result = subprocess.run([*base, '--log', str(root), '--', *command], env=env, capture_output=True, text=True, timeout=8)
    assert result.returncode == 0 and 'Starting the game anyway' in result.stderr and (root / 'game.json').exists(), result
    for running_game in (False, True):
        reset()
        child = subprocess.Popen([*base, '--', *command], env={**env, 'ASB_TEST_GAME_SECONDS':'30'}, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        try:
            ready = root / ('game.json' if running_game else 'bridge-start.json')
            deadline = time.monotonic() + 5
            while not ready.exists() and time.monotonic() < deadline:
                assert child.poll() is None
                time.sleep(0.02)
            assert ready.exists()
            time.sleep(0.05)  # Let fake bridge install its signal handler.
            child.send_signal(signal.SIGTERM)
            out, err = child.communicate(timeout=8)
            assert child.returncode == 128 + signal.SIGTERM and (root / 'bridge-stop').exists(), (out, err)
        finally:
            if child.poll() is None:
                child.kill()
                child.wait()
    reset()
    for args in ([], ['--', ''], ['--once', '--', 'true'], ['--seconds', '1', '--', 'true']):
        result = subprocess.run([*base, *args], env=env, capture_output=True, text=True, timeout=4)
        assert result.returncode != 0 and not (root / "bridge-start.json").exists(), (args, result)
print('Steam wrapper immediate launch without a controller, late readiness, bridge failures, opt-in logging, argv/environment and cleanup passed')
