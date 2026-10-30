"""A fake native API for midimixd's busy path: one plugin track; set_volume requests 2-4 are
refused with QUEUE_FULL and 6 with RETENTION_CAPACITY (nothing applied), the others applied and
watched. Everything else is refused (NOT_QUALIFIED).

On the host, then forwarded to the MPC, where a test build of midimixd uses it (stop the service
first; it holds the Midimix):

    uv run python tools/fake_api_busy.py /tmp/fake-api.sock 60 &
    ssh -N -R /tmp/fake-api.sock:/tmp/fake-api.sock root@mpc-key-37.lan.arpa &
    zig cc ... '-DAPI_SOCKET="/tmp/fake-api.sock"' '-DLOCK_FILE="/tmp/midimixd-busy.lock"' \
        '-DCTL_SOCKET="/tmp/midimixd-busy.sock"' (the arm target's flags and sources, mpc/Makefile)

The remote socket must not exist yet (the MPC's sshd doesn't unlink it). Fader 1 picks up at CC 64.
"""
import asyncio, json, os, sys, time

path = sys.argv[1]
inst = "run_" + "f" * 32
t0 = time.time()
track = {"uiIndex": 0, "ref": {"trackId": "7"}, "name": "Fake 1", "type": "plugin",
         "requested": {"muted": False, "soloed": False, "recordArmed": False}, "silenced": False,
         "mixer": {"generation": "5", "volume": 0.5, "pan": 0.5, "sends": [], "inserts": []},
         "setup": {"colour": 0x22ff00}, "instrument": {"status": "available", "productId": "MPC:Hype"}}
watchers, rev, n_vol = [], 1, 0
REFUSE = {2: "QUEUE_FULL", 3: "QUEUE_FULL", 4: "QUEUE_FULL", 6: "RETENTION_CAPACITY"}


def log(msg):
    print(f"{time.time() - t0:7.3f} {msg}", flush=True)


def frame(kind):
    global rev
    rev += 1
    f = {"apiVersion": "0.1", "type": kind, "instanceId": inst, "cursor": f"{inst}:{rev}", "revision": str(rev)}
    body = {"instanceId": inst, "projectId": "1", "tracks": [track], "muteContext": {"scope": "global"},
            "selectedTrackId": "7"}
    if kind == "snapshot":
        f["state"] = body
    else:
        f.update(body)
    return (json.dumps(f) + "\n").encode()


async def serve(reader, writer):
    global n_vol
    req = json.loads(await reader.readline())
    if req["op"] == "watch":
        log("watch")
        writer.write((json.dumps({"apiVersion": "0.1", "type": "ack", "instanceId": inst, "resumed": False}) + "\n").encode())
        writer.write(frame("snapshot"))
        watchers.append(writer)
        try:
            while True:
                await asyncio.sleep(1)
                writer.write((json.dumps({"apiVersion": "0.1", "type": "heartbeat", "instanceId": inst}) + "\n").encode())
                await writer.drain()
        except (ConnectionError, OSError):
            watchers.remove(writer)
        return
    if req["op"] == "set_volume":
        n_vol += 1
        error = REFUSE.get(n_vol)
        log(f"set_volume #{n_vol} {req['requestId']} value {req['value']:.4f} continuous {req.get('continuous')}"
            f" -> {error or 'applied'}")
        if error:
            reply = {"apiVersion": "0.1", "requestId": req["requestId"], "error": {"code": error}}
        else:
            await asyncio.sleep(0.03)
            track["mixer"]["volume"] = req["value"]
            reply = {"apiVersion": "0.1", "requestId": req["requestId"], "status": "applied",
                     "retention": "supersedable", "timing": {"totalMs": 30}}
            for w in watchers:
                w.write(frame("change"))
    else:
        log(f"{req['op']} (not faked)")
        reply = {"apiVersion": "0.1", "error": {"code": "NOT_QUALIFIED"}}
    writer.write((json.dumps(reply) + "\n").encode())
    await writer.drain()
    writer.close()


async def main():
    if os.path.exists(path):
        os.unlink(path)
    server = await asyncio.start_unix_server(serve, path)
    log(f"listening on {path}")
    async with server:
        await asyncio.sleep(float(sys.argv[2]) if len(sys.argv) > 2 else 60)


asyncio.run(main())
