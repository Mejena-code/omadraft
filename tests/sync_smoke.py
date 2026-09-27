#!/usr/bin/env python3
"""Exercise the setup dialog and live sync with two isolated local Syncthing nodes."""
import hashlib
import json
import os
from pathlib import Path
import shutil
import socket
import subprocess
import tempfile
import time
import urllib.request
import xml.etree.ElementTree as ET

ROOT = Path(__file__).resolve().parent.parent
TESTS = ROOT / "build/tests/omadraft-tests"
APP = ROOT / "build/app/omadraft"
if not shutil.which("syncthing"):
    raise SystemExit("Install Syncthing to run this optional integration test.")
ENV = dict(os.environ, QT_QPA_PLATFORM="offscreen", QT_QPA_PLATFORMTHEME="", QT_STYLE_OVERRIDE="Fusion")
for key in ("HYPRLAND_INSTANCE_SIGNATURE", "STCONFDIR", "STDATADIR", "STHOMEDIR", "STGUIADDRESS", "STGUIAPIKEY"):
    ENV.pop(key, None)


def port():
    with socket.socket() as handle:
        handle.bind(("127.0.0.1", 0))
        return handle.getsockname()[1]


def wait_for(predicate, label, timeout=30):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        try:
            if predicate():
                return
        except (OSError, ValueError):
            pass
        time.sleep(0.1)
    raise AssertionError(f"Timed out: {label}")


class Node:
    def __init__(self, home):
        self.home = home
        self.environment = dict(ENV, STHOMEDIR=str(home))
        home.mkdir()
        generated = subprocess.run(["syncthing", "generate", "--home", str(home)],
                                   env=self.environment, capture_output=True)
        if generated.returncode:
            raise AssertionError("Syncthing configuration failed: " + generated.stderr.decode())
        tree = ET.parse(home / "config.xml")
        root = tree.getroot()
        self.id = root.find("device").get("id")
        gui = root.find("gui")
        gui.set("tls", "false")
        self.api_port, self.sync_port = port(), port()
        gui.find("address").text = f"127.0.0.1:{self.api_port}"
        self.key = gui.findtext("apikey")
        for folder in root.findall("folder"):
            root.remove(folder)
        options = root.find("options")
        for name in ("globalAnnounceEnabled", "localAnnounceEnabled", "relaysEnabled", "natEnabled", "crashReportingEnabled"):
            element = options.find(name)
            if element is not None:
                element.text = "false"
        for address in options.findall("listenAddress"):
            options.remove(address)
        ET.SubElement(options, "listenAddress").text = f"tcp://127.0.0.1:{self.sync_port}"
        options.find("urAccepted").text = "-1"
        auto_upgrade = options.find("autoUpgradeIntervalH")
        if auto_upgrade is not None:
            auto_upgrade.text = "0"
        tree.write(home / "config.xml")
        self.data = home / "omadraft"
        self.drafts = self.data / "drafts"
        self.drafts.mkdir(parents=True)
        self.process = None
        self.app = None
        self.log = open(home / "test.log", "wb")

    def request(self, path, method="GET", body=None):
        data = None if body is None else json.dumps(body).encode()
        request = urllib.request.Request(f"http://127.0.0.1:{self.api_port}/rest/{path}", data=data, method=method,
                                         headers={"X-API-Key": self.key, "Content-Type": "application/json"})
        with urllib.request.urlopen(request, timeout=3) as response:
            raw = response.read()
            return json.loads(raw) if raw.strip() else None

    def start(self):
        self.process = subprocess.Popen(["syncthing", "serve", "--home", str(self.home), "--no-browser", "--no-restart", "--no-upgrade"],
                                        env=self.environment, stdout=self.log, stderr=self.log)
        wait_for(lambda: self.request("system/ping"), "Syncthing startup")

    def setup(self, peer):
        environment = dict(self.environment, OMADRAFT_TEST_SYNCTHING_HOME=str(self.home),
                           OMADRAFT_TEST_DRAFTS=str(self.drafts), OMADRAFT_TEST_PEER=peer.id)
        result = subprocess.run([str(TESTS), "syncthingSetupIntegration"], env=environment, capture_output=True, timeout=45)
        assert result.returncode == 0, result.stdout.decode() + result.stderr.decode()
        folders = self.request("config/folders")
        folder = next(item for item in folders if item["id"] == "omadraft-drafts-v1")
        assert Path(folder["path"]) == self.drafts
        assert {entry["deviceID"] for entry in folder["devices"]} == {self.id, peer.id}
        assert folder["maxConflicts"] == -1
        # Disable discovery in this test; connect only to the other loopback node.
        self.request(f"config/devices/{peer.id}", "PATCH", {"addresses": [f"tcp://127.0.0.1:{peer.sync_port}"]})

    def start_app(self):
        self.app = subprocess.Popen([str(APP), "--data-dir", str(self.data)], env=self.environment, stdout=self.log, stderr=self.log)
        wait_for(lambda: (self.data / "session.json").exists(), "Omadraft startup")

    def texts(self):
        return [note["text"] for note in json.loads((self.data / "session.json").read_text())["notes"]]

    def ids(self):
        return [note["id"] for note in json.loads((self.data / "session.json").read_text())["notes"]]

    def scan(self):
        self.request("db/scan?folder=omadraft-drafts-v1", "POST")

    def close(self):
        for process in (self.app, self.process):
            if process and process.poll() is None:
                process.terminate()
                try:
                    process.wait(timeout=10)
                except subprocess.TimeoutExpired:
                    process.kill()
                    process.wait(timeout=5)
        self.log.close()


with tempfile.TemporaryDirectory(prefix="omadraft-sync-") as temporary:
    nodes = []
    try:
        first = Node(Path(temporary) / "desktop")
        nodes.append(first)
        second = Node(Path(temporary) / "laptop")
        nodes.append(second)
        for node in nodes:
            node.start()
        first.setup(second)
        second.setup(first)
        first.setup(second)  # Repeating setup must not duplicate the folder or peer.
        assert len(first.request("config/folders")) == 1
        for node in nodes:
            node.start_app()
        text = "# A shared draft\nUnicode: åäö 🌿"
        draft = first.drafts / "integration-note.md"
        draft.write_text(text)
        first.scan()
        wait_for(lambda: text in first.texts() and text in second.texts(), "live draft sync")
        wait_for(lambda: first.ids() == second.ids(), "matching tab order on both computers")
        edited = "# Edited on the laptop\n**Both windows should update.**"
        (second.drafts / draft.name).write_text(edited)
        second.scan()
        wait_for(lambda: edited in first.texts() and edited in second.texts(), "reverse sync")
        # Deletion markers must also reach an open editor on the other device.
        marker = hashlib.sha256(edited.encode()).hexdigest() + "\n"
        (first.drafts / "integration-note.deleted").write_text(marker)
        first.scan()
        wait_for(lambda: edited not in first.texts() and edited not in second.texts(), "discard propagation")
        for node in nodes:
            assert node.app.poll() is None
        print("PASS: real setup dialog, new peer pairing, idempotent setup, two-way live sync, matching tab order, and discard propagation.")
    except Exception:
        for node in nodes:
            node.log.flush()
            print((node.home / "test.log").read_text(errors="replace")[-6000:])
        raise
    finally:
        for node in reversed(nodes):
            node.close()
