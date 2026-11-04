#!/usr/bin/env bash
set -euo pipefail
python3 - "$@" <<'PY'
import json
import os
from pathlib import Path
import re
import shutil
import signal
import subprocess
import sys
import tempfile
import time

binary, example, plugin = [str(Path(p).resolve()) for p in sys.argv[1:]]
def run(*args, ok=True):
    result = subprocess.run([binary, *args], capture_output=True, text=True, timeout=15)
    assert (result.returncode == 0) == ok, result.stdout + result.stderr
    return result.stdout + result.stderr

def verify_recording(output, field):
    counts = re.search(r'batches=(\d+) records=(\d+) errors=(\d+)', output)
    assert counts and int(counts[1]) > 0 and counts[3] == '0', output
    path = re.search(r' recording=(.+)', output)[1]
    lines = [json.loads(line) for line in Path(path).read_text().splitlines()]
    assert lines[0]['format'] == 'ilr' and lines[0]['version'] == 1
    assert len(lines) - 1 == int(counts[1]), output
    assert all(field in str(line) for line in lines[1:]), lines

with tempfile.TemporaryDirectory(dir=os.environ.get('TEST_TMPDIR')) as directory:
    root = Path(directory)
    output_dir = str(root / 'recordings with spaces ; literal')
    verify_recording(run('demo', '--config', example, '--output-dir', output_dir), 'value')
    assert 'sqlite' in run('storage')
    assert 'recording' in run('plugins')
    assert run('version').strip()
    run('run', '--source', 'missing', ok=False)
    run('run', ok=False)
    run('daemon', ok=False)
    run('demo', '--duration', 'abc', ok=False)
    run('demo', '--duration', '0', ok=False)
    run('demo', '--config', str(root / 'absent.yaml'), ok=False)
    invalid = root / 'invalid.yaml'
    invalid.write_text('engine: {channel: {size: invalid}}')
    run('demo', '--config', str(invalid), ok=False)
    blocked = root / 'not-a-directory'
    blocked.write_text('blocked')
    run('demo', '--output-dir', str(blocked), ok=False)
    # Validate SIGTERM while the timer, collection pool and sink pool are active.
    process = subprocess.Popen([binary, 'demo', '--duration', '30', '--output-dir', str(root / 'signal')],
                               stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
    try:
        deadline = time.monotonic() + 5
        while not list((root / 'signal').glob('*.ilr')):
            assert process.poll() is None, 'process exited before signal'
            assert time.monotonic() < deadline, 'recording did not start'
            time.sleep(0.02)
        time.sleep(0.15)
        process.send_signal(signal.SIGTERM)
        stdout, stderr = process.communicate(timeout=10)
        assert process.returncode == 0, stdout + stderr
        verify_recording(stdout, 'value')
    finally:
        if process.poll() is None:
            process.kill()
            process.communicate()
    # Load and run a genuine .so without linking Feature or Server.
    plugin_dir = root / 'plugins'
    plugin_dir.mkdir()
    shutil.copyfile(plugin, plugin_dir / 'source.so')
    config = root / 'core.yaml'
    config.write_text(f'global:\n  data_dir: {json.dumps(str(root / "so-recordings"))}\n'
                      f'  plugin_dirs: [{json.dumps(str(plugin_dir))}]\n'
                      'engine:\n  collect_pool_threads: 1\n  sink_pool_threads: 1\n'
                      '  channel:\n    size: small\n')
    verify_recording(run('run', '--source', 'test_so_source', '--config', str(config)), 'so_value')
PY
