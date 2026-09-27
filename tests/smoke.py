#!/usr/bin/env python3
"""Exercise real process startup, one-instance behavior, shutdown, and recovery."""
import json
import os
from pathlib import Path
import signal
import subprocess
import tempfile
import time

root = Path(__file__).resolve().parent.parent
binary = root / "build/app/omadraft"
environment = dict(os.environ, QT_QPA_PLATFORM="offscreen", QT_QPA_PLATFORMTHEME="", QT_STYLE_OVERRIDE="Fusion")
environment.pop("HYPRLAND_INSTANCE_SIGNATURE", None)


def wait_for(predicate, process, timeout=5):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        if process.poll() is not None:
            raise AssertionError(f"App exited early: {process.stderr.read().decode()}")
        if predicate():
            return
        time.sleep(0.01)
    raise AssertionError("Timed out waiting for the app")


with tempfile.TemporaryDirectory(prefix="omadraft-process-") as temporary:
    data = Path(temporary) / "notes"
    data.mkdir()
    session_path = data / "session.json"
    original = {
        "version": 1,
        "active": 1,
        "notes": [
            {"id": "first", "text": "# Keep this\nUnicode: åäö 🌿", "cursor": 5, "anchor": 5, "scroll": 0},
            {"id": "second", "text": "**A second note**", "cursor": 8, "anchor": 3, "scroll": 0},
        ],
    }
    session_path.write_text(json.dumps(original))
    command = [str(binary), "--data-dir", str(data)]
    process = None
    try:
        before = session_path.stat().st_mtime_ns
        started = time.monotonic()
        process = subprocess.Popen(command, env=environment, stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)
        wait_for(lambda: session_path.stat().st_mtime_ns != before, process)
        startup_ms = (time.monotonic() - started) * 1000
        second = subprocess.run(command, env=environment, capture_output=True, timeout=5)
        assert second.returncode == 0, second.stderr.decode()
        assert process.poll() is None, "The first instance must keep running"
        saved = json.loads(session_path.read_text())
        assert saved["active"] == 1
        assert saved["notes"] == original["notes"]
        process.send_signal(signal.SIGTERM)
        assert process.wait(timeout=5) == 0
        assert not (data / "session.lock").exists()
        assert json.loads(session_path.read_text())["notes"] == original["notes"]

        # A hard kill leaves a stale process lock; the next launch must recover it.
        process = subprocess.Popen(command, env=environment, stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)
        wait_for(lambda: (data / "session.lock").exists(), process)
        time.sleep(0.1)
        process.kill()
        process.wait(timeout=5)
        process = subprocess.Popen(command, env=environment, stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)
        wait_for(lambda: (data / "session.lock").exists() and
                 (data / "session.lock").read_text().splitlines()[0] == str(process.pid), process)
        second = subprocess.run(command, env=environment, capture_output=True, timeout=5)
        assert second.returncode == 0, second.stderr.decode()
        process.send_signal(signal.SIGTERM)
        assert process.wait(timeout=5) == 0
        assert json.loads(session_path.read_text())["notes"] == original["notes"]
        # Race two launches before either has a socket. Exactly one must stay open.
        first = subprocess.Popen(command, env=environment, stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)
        second_process = subprocess.Popen(command, env=environment, stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)
        deadline = time.monotonic() + 5
        while first.poll() is None and second_process.poll() is None and time.monotonic() < deadline:
            time.sleep(0.02)
        alive = [candidate for candidate in (first, second_process) if candidate.poll() is None]
        if len(alive) != 1:
            for candidate in (first, second_process):
                if candidate.poll() is None:
                    candidate.kill()
                    candidate.wait(timeout=5)
            raise AssertionError("Two simultaneous launches must leave exactly one instance")
        process = alive[0]
        exited = second_process if process is first else first
        assert exited.returncode == 0, exited.stderr.read().decode()
        process.send_signal(signal.SIGTERM)
        assert process.wait(timeout=5) == 0
        print(f"PASS: one instance, graceful shutdown, hard-kill restart, and persisted notes. Startup to first saved window state: {startup_ms:.0f} ms (offscreen, warm system).")
    finally:
        if process and process.poll() is None:
            process.kill()
            process.wait(timeout=5)
        if process:
            diagnostics = process.stderr.read().decode()
            if diagnostics:
                print(diagnostics)
