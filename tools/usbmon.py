"""Capture USB-MIDI traffic on the MPC with usbmon, and decode it.

usbmon sees every URB on a bus, so it shows what MPC sends to its own controller (bus 1)
and to the Midimix (bus 3) without touching MPC.

    uv run python tools/usbmon.py capture captures/<name>.log --secs 60 [--bus 1 3]
    uv run python tools/usbmon.py decode captures/<name>.log [--all]

The log holds usbmon text-API lines, each prefixed with its bus number, made from the binary
API (/dev/usbmon<bus>), which keeps whole transfers. Logs taken with the text API itself
(/sys/kernel/debug/usb/usbmon/<bus>u, before 2026-10-22 evening) stop at 32 bytes per URB:
decoding flags those "TRUNCATED". Decoding drops the blinking-LED keepalive MPC sends every
250 ms unless --all is given.
"""

import argparse
import struct
import subprocess
import sys

HOST = "root@mpc-key-37.lan.arpa"

# struct mon_bin_hdr as read() returns it (PKT_SZ_API0): id, type, xfer_type, epnum, devnum, busnum,
# flag_setup, flag_data, ts_sec, ts_usec, status, len_urb, len_cap, setup
MON_HDR = struct.Struct("<QBBBBHbbqiiII8s")

# (bus, device) -> name; ports are USB-MIDI cable numbers
DEVICES = {
    (1, 2): ("MPC", {0: "public", 1: "private", 2: "port A"}),
}
BUS_NAMES = {3: "Midimix"}

# USB-MIDI code index number -> payload length
CIN_LEN = {0x2: 2, 0x3: 3, 0x4: 3, 0x5: 1, 0x6: 2, 0x7: 3, 0x8: 3, 0x9: 3,
           0xA: 3, 0xB: 3, 0xC: 2, 0xD: 2, 0xE: 3, 0xF: 1}

# What MPC sends every 250 ms on its private port (a blinking button LED)
KEEPALIVE = {(0xB0, 0x35)}


def ssh(command, **kw):
    return subprocess.run(["ssh", "-o", "BatchMode=yes", HOST, command], check=True, **kw)


def bin_to_text(bus, raw):
    """usbmon binary events (read() API: 48-byte header, then len_cap bytes) as text-API lines."""
    lines, i = [], 0
    while i + MON_HDR.size <= len(raw):
        (tag, event, xfer, epnum, dev, _, _, _, sec, usec, status, length, cap, _) = \
            MON_HDR.unpack_from(raw, i)
        data = raw[i + MON_HDR.size:i + MON_HDR.size + cap]
        i += MON_HDR.size + cap
        kind = "ZICB"[xfer] + ("i" if epnum & 0x80 else "o")
        line = f"{bus} {tag:08x} {sec * 1000000 + usec} {chr(event)} {kind}:{bus}:{dev:03d}:{epnum & 0x7F} {status} {length}"
        if cap:
            line += " = " + " ".join(data[j:j + 4].hex() for j in range(0, len(data), 4))
        lines.append(line)
    return lines


def capture(path, secs, buses):
    # One cat per bus from the binary API (whole transfers; the text API stops at 32 bytes) into the
    # MPC's /tmp (tmpfs), killed by pid, then fetched and turned into text-API lines.
    start = " ".join(f"cat /dev/usbmon{b} > /tmp/usbmon{b}.bin & pids=\"$pids $!\";" for b in buses)
    print(f"capturing buses {buses} for {secs} s into {path}", file=sys.stderr)
    ssh(f"pids=; {start} sleep {secs}; kill $pids")
    lines = []
    for b in buses:
        lines += bin_to_text(b, ssh(f"cat /tmp/usbmon{b}.bin", capture_output=True).stdout)
    ssh("rm -f " + " ".join(f"/tmp/usbmon{b}.bin" for b in buses))
    lines.sort(key=lambda l: int(l.split()[2]))
    with open(path, "w", encoding="utf-8") as out:
        out.write("".join(l + "\n" for l in lines))


def name_of(bus, dev, cable):
    if (bus, dev) in DEVICES:
        name, cables = DEVICES[(bus, dev)]
        return f"{name} {cables.get(cable, f'cable {cable}')}"
    base = BUS_NAMES.get(bus, f"bus {bus} dev {dev}")
    return base if cable == 0 else f"{base} cable {cable}"


def describe(msg):
    s = msg[0]
    kind, ch = s & 0xF0, (s & 0x0F) + 1
    if s == 0xF0:
        return "sysex " + " ".join(f"{b:02x}" for b in msg)
    if kind == 0x90:
        return f"ch{ch} note on  {msg[1]:3d} (0x{msg[1]:02x}) vel {msg[2]}"
    if kind == 0x80:
        return f"ch{ch} note off {msg[1]:3d} (0x{msg[1]:02x}) vel {msg[2]}"
    if kind == 0xB0:
        return f"ch{ch} cc {msg[1]:3d} (0x{msg[1]:02x}) = {msg[2]}"
    if kind == 0xA0:
        return f"ch{ch} poly at {msg[1]:3d} (0x{msg[1]:02x}) = {msg[2]}"
    return " ".join(f"{b:02x}" for b in msg)


def pad_rgb(msg):
    """(pad, 'rrggbb') for MPC's set-pad-colour sysex (f0 47 7f <id> 65 00 04 pad r g b f7), else None."""
    if len(msg) == 12 and msg[:3] == b"\xf0\x47\x7f" and msg[4:7] == b"\x65\x00\x04":
        return msg[7], msg[8:11].hex()
    return None


def decode(path, show_all):
    pads = []  # pad colours of the current burst, printed as one line
    for line in decode_lines(path, show_all):
        stamp, text = line[:9], line[10:]
        prefix = "host -> MPC public: sysex "
        if text.startswith(prefix) and pad_rgb(bytes.fromhex(text[len(prefix):])):
            pad, rgb = pad_rgb(bytes.fromhex(text[len(prefix):]))
            if pads and pads[0][0] != stamp:
                print(f"{pads[0][0]} pads: " + " ".join(f"{p}={c}" for _, p, c in pads))
                pads = []
            pads.append((stamp, pad, rgb))
            continue
        if pads:
            print(f"{pads[0][0]} pads: " + " ".join(f"{p}={c}" for _, p, c in pads))
            pads = []
        print(line)
    if pads:
        print(f"{pads[0][0]} pads: " + " ".join(f"{p}={c}" for _, p, c in pads))


def decode_lines(path, show_all):
    sysex = {}  # (bus, dev, ep, cable) -> bytes so far
    t0 = None
    with open(path, encoding="utf-8") as f:
        for line in f:
            parts = line.split()
            # bus tag time event type:bus:dev:ep status length [= words...]
            if len(parts) < 7 or parts[3] not in ("S", "C"):
                continue
            bus, t, event, addr = int(parts[0]), int(parts[2]), parts[3], parts[4]
            kind, _, dev, ep = addr.split(":")
            # data rides on the submission for OUT, on the completion for IN
            if not ((kind.endswith("o") and event == "S") or (kind.endswith("i") and event == "C")):
                continue
            if "=" not in parts:
                continue
            length = int(parts[6])
            data = bytes.fromhex("".join(parts[parts.index("=") + 1:]))
            direction = "->" if kind.endswith("o") else "<-"
            t0 = t if t0 is None else t0
            stamp = f"{(t - t0) / 1e6:9.3f}"
            if len(data) < length:
                yield f"{stamp} bus {bus} dev {dev} {direction} TRUNCATED: {length} bytes, {len(data)} shown"
            for i in range(0, len(data) - 3, 4):
                pkt = data[i:i + 4]
                cable, cin = pkt[0] >> 4, pkt[0] & 0x0F
                if pkt == b"\0\0\0\0" or cin not in CIN_LEN:
                    continue
                key = (bus, dev, ep, cable)
                payload = pkt[1:1 + CIN_LEN[cin]]
                if cin == 0x4 or (cin in (0x5, 0x6, 0x7) and key in sysex):
                    sysex[key] = sysex.get(key, b"") + payload
                    if cin == 0x4:
                        continue
                    msg = sysex.pop(key)
                else:
                    msg = payload
                if not show_all and (msg[0], msg[1] if len(msg) > 1 else None) in KEEPALIVE:
                    continue
                who = name_of(bus, int(dev), cable)
                # host: the MPC's side of the USB bus (the MPC app, or midimixd on bus 3)
                arrow = f"host -> {who}" if direction == "->" else f"{who} -> host"
                yield f"{stamp} {arrow}: {describe(msg)}"
            if len(data) < length:  # the rest of a sysex cut here never arrives
                for key in [k for k in sysex if k[:3] == (bus, dev, ep)]:
                    yield (f"{stamp} {name_of(bus, int(dev), key[3])}: sysex cut: "
                          + " ".join(f"{b:02x}" for b in sysex.pop(key)) + " ...")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    c = sub.add_parser("capture")
    c.add_argument("log")
    c.add_argument("--secs", type=int, default=30)
    c.add_argument("--bus", type=int, nargs="+", default=[1, 3])
    d = sub.add_parser("decode")
    d.add_argument("log")
    d.add_argument("--all", action="store_true")
    a = ap.parse_args()
    if a.cmd == "capture":
        capture(a.log, a.secs, a.bus)
    else:
        decode(a.log, a.all)


if __name__ == "__main__":
    main()
