"""The editor plugin (mpc/plugin: engine + mpc-vst-plugins' wrapper) in a stand-in host (mpc/plugin/test_host.c,
ASan/UBSan), against a fake midimixd control socket (midimixd/ctl.h's protocol).

    uv run --with pytest pytest tests/
"""

import json
import os
import shutil
import socket
import subprocess
import sys
import tempfile
import threading
import time
from pathlib import Path

import pytest

MPC = Path(__file__).resolve().parent.parent / "mpc"
pytestmark = pytest.mark.skipif(
    not sys.platform.startswith("linux") or not shutil.which("make") or not shutil.which("cc")
    or not (MPC / "mpc-vst-plugins" / "wrapper" / "vst2_wrap.c").exists(),
    reason="needs Linux, make, a C compiler and the mpc-vst-plugins submodule")
PARAMS = json.loads((MPC / "plugin" / "params.json").read_text(encoding="utf-8"))["params"]
INDEX = {p["key"]: i for i, p in enumerate(PARAMS)}
DEFAULT = "top=none mid=none bottom=pan fader=volume mute=mute solo=solo arm=arm"


def norm(key: str, option: str) -> float:
    opts = PARAMS[INDEX[key]]["options"]
    return opts.index(option) / (len(opts) - 1)


class FakeMidimixd:
    """midimixd's control socket: answers get with `state`, keeps each connection's map, like ctl.c."""

    def __init__(self, path: str):
        self.path = path
        self.maps = []          # every map request, in order
        self.connections = 0    # open now
        self.touch = (0, "top", 1)
        self.bank = (1, 1)
        self.strips = [("ff0000", "drum", "Kick Kit"), ("11ff00", "plugin", "Plugin 001")]
        self.tracks = None      # the project's track count, when it isn't len(strips)
        self.lock = threading.Lock()
        self.srv = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        self.srv.bind(path)
        self.srv.listen(4)
        threading.Thread(target=self._accept, daemon=True).start()

    def state(self, mine: bool) -> str:
        with self.lock:
            seq, row, strip = self.touch
            lines = ["midimixd 1", "midimix 1", "api 1", "bank %d %d" % self.bank,
                     "tracks %d" % (self.tracks or len(self.strips)),
                     "map " + (self.maps[-1] if self.maps else DEFAULT), "mapfrom " + ("you" if mine else "default"),
                     "touch %d %s %d" % (seq, row, strip), "selected 1"]
            for n in range(8):
                s = self.strips[n] if n < len(self.strips) else ("-", "-", "")
                lines.append("strip %d %s %s %s" % (n + 1, *s))
        return "\n".join(lines) + "\n\n"

    def _accept(self):
        while True:
            try:
                conn, _ = self.srv.accept()
            except OSError:
                return
            threading.Thread(target=self._serve, args=(conn,), daemon=True).start()

    def _serve(self, conn):
        with self.lock:
            self.connections += 1
        mine, buf = False, b""
        try:
            while True:
                data = conn.recv(4096)
                if not data:
                    break
                buf += data
                while b"\n" in buf:
                    line, buf = buf.split(b"\n", 1)
                    req = line.decode()
                    if req == "get":
                        conn.sendall(self.state(mine).encode())
                    elif req.startswith("map "):
                        with self.lock:
                            self.maps.append(req[4:])
                        mine = True
                        conn.sendall(b"error no target \"bad\"\n\n" if "bad" in req else b"ok\n\n")
                    else:
                        conn.sendall(b"error unknown request\n\n")
        finally:
            with self.lock:
                self.connections -= 1
            conn.close()

    def close(self):
        self.srv.close()
        os.unlink(self.path)


class Host:
    def __init__(self, sock: str):
        self.p = subprocess.Popen([str(MPC / "build" / "plugin-host")], stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                  text=True, env={**os.environ, "MIDIMIXD_SOCKET": sock})

    def cmd(self, line: str) -> list[str]:
        self.p.stdin.write(line + "\n")
        self.p.stdin.flush()
        out = []
        while True:
            l = self.p.stdout.readline()
            assert l, "the host died"
            l = l.rstrip("\n")
            if l.startswith("error"):
                raise AssertionError(l)
            if l in ("ok", "end"):
                return out
            out.append(l)

    def text(self, key: str) -> str:
        line = [l for l in self.cmd("get %d" % INDEX[key]) if l.startswith("value ")][0]
        return line.split(" ", 3)[3] if line.count(" ") >= 3 else ""

    def value(self, key: str) -> float:
        line = [l for l in self.cmd("get %d" % INDEX[key]) if l.startswith("value ")][0]
        return float(line.split(" ")[2])

    def set(self, key: str, v: float):
        self.cmd("set %d %g" % (INDEX[key], v))

    def settle(self):
        """Blocks enough for the wrapper's 100 ms display poll, after mmd's own 100 ms."""
        time.sleep(0.25)
        self.cmd("run 40")

    def chunk(self) -> str:
        line = self.cmd("chunk")[-1]
        return line.split(" ", 2)[2]

    def close(self):
        self.p.stdin.write("quit\n")
        self.p.stdin.flush()
        assert self.p.wait(10) == 0


def wait_for(cond, secs=3.0):
    end = time.time() + secs
    while time.time() < end:
        if cond():
            return True
        time.sleep(0.02)
    return False


@pytest.fixture(scope="module", autouse=True)
def build():
    subprocess.run(["make", "-C", str(MPC), "plugin-host"], check=True, capture_output=True)


@pytest.fixture
def env():
    d = tempfile.mkdtemp()
    sock = os.path.join(d, "ctl.sock")
    fake = FakeMidimixd(sock)
    host = Host(sock)
    yield fake, host
    host.close()
    fake.close()
    shutil.rmtree(d)


def test_sends_the_default_and_shows_the_strips(env):
    fake, host = env
    assert wait_for(lambda: fake.maps == [DEFAULT])
    host.settle()
    assert host.text("status") == "BANK 1 OF 1: TRACKS 1-2 OF 2"
    assert host.text("hint") == "TOUCH A CONTROL ON THE MIDIMIX TO PICK ITS ROW"
    assert host.text("name_1") == "Kick Kit"
    assert host.text("name_2") == "Plugin 001"
    assert host.text("name_3") == ""
    assert host.text("colour_1") == "RED"
    assert host.text("colour_2") == "GREEN"
    assert host.text("colour_3") == "NONE"
    assert host.text("row_1") == "TOP KNOB: NONE"
    assert host.text("row_4") == "FADER: VOLUME"
    assert host.text("row_7") == "REC ARM: REC ARM"
    assert host.chunk() == DEFAULT


def test_a_bank_with_one_track(env):
    fake, host = env
    fake.bank, fake.tracks, fake.strips = (2, 2), 9, [("ffd500", "midi", "MIDI 004")]
    host.settle()
    assert host.text("status") == "BANK 2 OF 2: TRACK 9 OF 9"
    assert host.text("name_1") == "MIDI 004"
    assert host.text("colour_1") == "YELLOW"


def test_editing_a_row(env):
    fake, host = env
    host.set("row_1", 1)                       # tap TOP KNOB
    host.settle()
    assert host.text("edit_title") == "TOP KNOBS"
    assert host.text("rowkind") == "LEVEL"
    host.set("edit_level", norm("edit_level", "SEND 1"))
    assert wait_for(lambda: fake.maps[-1:] == ["top=send1 mid=none bottom=pan fader=volume mute=mute solo=solo arm=arm"])
    assert host.text("row_1") == "TOP KNOB: SEND 1"
    assert host.text("edit_help") == "SENDS ONLY MOVE WITH THE TRACK'S AUTOMATION OFF"
    host.set("edit_button", norm("edit_button", "SELECT"))   # a button target on a knob row: refused
    assert host.chunk() == "top=send1 mid=none bottom=pan fader=volume mute=mute solo=solo arm=arm"
    host.set("row_6", 1)                       # tap SOLO
    host.settle()
    assert host.text("rowkind") == "BUTTON"
    host.set("edit_button", norm("edit_button", "INSERT 2"))
    assert wait_for(lambda: fake.maps[-1:] == ["top=send1 mid=none bottom=pan fader=volume mute=mute solo=insert2 arm=arm"])
    assert host.chunk() == "top=send1 mid=none bottom=pan fader=volume mute=mute solo=insert2 arm=arm"


def test_a_project_brings_its_mapping(env):
    fake, host = env
    host.cmd("setchunk arm=select top=send2")
    want = "top=send2 mid=none bottom=pan fader=volume mute=mute solo=solo arm=select"
    assert wait_for(lambda: fake.maps[-1:] == [want])
    assert host.chunk() == want
    host.cmd("setchunk top=volume arm=nonsense")     # unreadable: kept as it was
    assert host.chunk() == want


def test_touching_a_control_picks_its_row(env):
    fake, host = env
    host.settle()
    assert host.text("edit_title") == "FADERS"
    fake.touch = (1, "arm", 3)
    host.settle()
    assert host.text("edit_title") == "REC ARM BUTTONS"
    assert host.value("row_7") == 1.0 or host.text("rowkind") == "BUTTON"
    host.set("row_2", 1)
    host.settle()
    assert host.text("edit_title") == "MIDDLE KNOBS"    # the same touch doesn't pull it back
    fake.touch = (2, "arm", 3)
    host.settle()
    assert host.text("edit_title") == "REC ARM BUTTONS"


def test_closing_the_instance_drops_its_connection(env):
    fake, host = env
    assert wait_for(lambda: fake.connections == 1)
    host.cmd("open")
    assert wait_for(lambda: fake.connections == 2)
    host.cmd("close")
    assert wait_for(lambda: fake.connections == 1)


def test_without_midimixd_then_with_it(tmp_path):
    sock = str(tmp_path / "ctl.sock")
    host = Host(sock)
    try:
        host.settle()
        assert host.text("status") == "MIDIMIXD IS NOT RUNNING"
        assert host.text("name_1") == ""
        host.set("row_4", 1)
        host.set("edit_level", norm("edit_level", "PAN"))   # editing still works
        fake = FakeMidimixd(sock)
        try:
            assert wait_for(lambda: fake.maps[-1:] == ["top=none mid=none bottom=pan fader=pan mute=mute solo=solo arm=arm"])
            host.settle()
            assert host.text("status") == "BANK 1 OF 1: TRACKS 1-2 OF 2"
        finally:
            fake.close()
    finally:
        host.close()
