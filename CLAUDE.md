# midimix

Goal: make an Akai **Midimix** a proper control surface for the owner's **MPC Key 37**
(the one from `../torpedo`): a translation layer running on the MPC, between the
MPC app and the Midimix, both ways. Above all, the Midimix's mute / solo / rec-arm
(and bank) LEDs must follow what happens on the MPC, including changes made on the
MPC's screen and pads. Not a VST plugin.

## Status (2026-10-28)

- Recon done on the Key 37 (MPC 3.9.1.2), see below.
- Midimix LEDs answer note on vel 127 on the same note (checked with `seqtool`, 2026-10-22).
- The bridge (`mpc/midimixd`, below) runs on mpc-tests' **native API**, an addin inside MPC ("MPC's
  native API" below): LEDs from its watch stream, buttons, faders and pan knobs as its commands.
  Milestone 3, checked on the Key 37 2026-10-28 with presses sent to its port; not yet tried by hand
  on the hardware. Not checked yet: a Midimix replug. Next: milestone 4 (systemd unit, install /
  uninstall), the top two knobs.
- The memory reader of milestones 1–2 (`mpc/mpcread`, "Reading MPC's state from memory" below) is
  now only behind `mpcstate`, a diagnostic tool.
- MPC's hidden **Lua scripting** (below) could read and set every track's state but nothing
  reaches it in this firmware: no console, script folders never read. The flag file
  `/data/scriptingDefaultEnabled` is still on the unit (user's OK, 2026-10-22); not yet saved into
  MPC.settings.

## MPC app facts (MPC 3.9.1.2, OE scarthgap 5.0.17, kernel 6.18 RT)

- Rootfs is read-only ext4; `/etc` and `/var` are overlays kept on `/data` (rw), so systemd
  drop-ins survive reboots. `/data/mpc-addins/` and the shared drop-in
  `/etc/systemd/system/acvs.service.d/90-mpc-addins.conf` come from mpc-vst-plugins' **addin**
  framework (MIT, `../torpedo/mpc/mpc-vst-plugins/docs/ADDINS.md`): one `LD_PRELOAD` list shared
  by every addin, install / uninstall scripts, a reload unit for the first start after a boot.
  Installed: `remote` (the MPC MCP server, seq client "MPC Remote", HTTP 6720) and mpc-tests'
  `privtap` (logs the surface's rawmidi to `/tmp/privtap.log`; bytes written to the FIFO
  `/dev/midi_inject` reach MPC as if from the surface, which is how the MCP's `press_button` works),
  and mpc-tests' `mpc-native-api` (below).
- **SOLO on the Main screen (or the MPC X's SOLO, note 77) solos the selected track, and the solo
  follows the selection**: select another track and the solo moves to it (seen 2026-10-24).
- **New Project restarts the app** (seen twice 2026-10-24, once with no reader running): MPC exits
  cleanly (status 0), systemd starts it again, and it opens on the startup "Select Demo" screen.
  Whether stock MPC (without the two addins) does the same is untested.
- `acvs.service` runs `/usr/bin/az01-launch-MPC`; the app is `/usr/bin/MPC` (112 MB, JUCE, PIE).
- **No app symbols**: the 8000 exported symbols are bundled libraries only (CryptoPP, pugixml,
  Elastique, Superpowered, Ableton Link, Hermes, Yoga, Lua/sol2). RTTI type names are there
  (`TouchTrackMutePage`, `SceneControlSurface`, `ScriptableTrackPool` ...). The binary also embeds
  key material: analyse it in the scratchpad only, never commit or quote its strings wholesale.
- UI partly react-juce (JavaScript compiled at runtime by an embedded `hermesc` to `binary.hbc`).
- MIDI Learn maps (`/usr/share/Akai/SME0/Midi Learn/*.xmm`, ~75 controllers, no Midimix) are XML
  `pairing`s of `Target_control` with `Mapping_type/channel/data1/control/reverse`: input only,
  nothing describes output to the controller.

### Hidden Lua scripting (sol2, Lua 5.4)

From the binary's docstrings (`strings`, offsets ~78.2–78.7 MB):

- Gated by the setting `ScriptingEnabled` (read at 0x2766de0, written at 0x2766c84): its default,
  when MPC.settings has none, is `File("/data/scriptingDefaultEnabled").exists()`. Once MPC saves
  its settings the value sticks there, so **undoing it** means deleting the flag file *and* the
  `ScriptingEnabled` entry in `/media/az01-internal/Settings/MPC/MPC.settings` (MPC stopped), then
  restarting. Content folders: `/usr/share/Akai/SME0/Scripts` and `<storage>/MPC Documents/Scripts`
  (both absent; their path getters at 0x15a3248 / 0x15a7b74 have no direct callers). There is a
  scripting console (`TouchScriptingConsolePopup`, `ScriptingConsoleWindow`), an interaction
  recorder ("MPC Interaction Recording Files") and a QA API that presses MPC / Force buttons and
  pads (`SetTypeTo`, `Press`, `Hold`, `DoublePress`).
- Scripts load from a `LuaBundle` (`GetScript(path)`, `require` through a bundle searcher;
  `json-ordered`, `OrderedTable` built in). "Exposes the file system to the scripting framework;
  relative to the location of the script".
- **Scripted devices** ("Control surface mapper for MIDI devices"): `OnLoad` / `OnUnload` on
  device connect / disconnect; `mappings` with `pads`, `controlSurface`, `qlinks`,
  `tracks.mutes` (`AddNoteToTrackMute(channel, note, trackIndex)`), `script`
  (`AddNoteToScript`, `AddCCToScript` → the script's `HandleMIDINoteOn/Off`, `HandleMIDICC`) and
  **`midiOut`** (`SendMIDINoteOn/Off(…, outputIndex)`, `SendMIDICC`, `SendMIDISysex`).
- **Project / tracks** (by track name): `GetMute` / `SetMute`, `GetSolo` / `SetSolo`,
  `GetSoloMute` (silenced by another solo), `GetTrackRecordArmed` / `SetTrackRecordArmed`,
  `GetTrackVolume` / `SetTrackVolume`, send levels, `GetCurrentTrackName` / `SetCurrentTrack`,
  track counts, `CreateTrack` (avoid). Also `currentSequence`, song, step sequencer, clips.
- Tried 2026-10-22 and failed: `mpc/scripts/probe.lua` (logs `_G` to `/tmp/mpc-script-probe-*.txt`)
  as `MPC Documents/Scripts/{MIDI Mix, MIDI Mix MIDI 1, Midimix, MIDImix, AKAI MIDI Mix, init,
  main, autorun, startup}.lua`: never read (atime unchanged) at MPC start or on a Midimix replug.
  Removed afterwards.
- `OnLoad`, `OnUnload`, `HandleMIDI*` are registered as API functions with docstrings (around
  0x10675d0, the only references to those names). The touch "Script Console" component is built at
  0x3b21df0, which nothing calls directly. The same API is also registered for JavaScript
  (`Scripting::JSRegistrar`, JUCE's engine).
- Unknown: how a script is bound to a MIDI device, whether a script gets a timer or change
  callback (without one it only runs on incoming MIDI), and the root namespace's Lua name.
- Separate and not ours: a USB-drive autorun ("Script detected on … Do you want to run it?",
  `autorun`, runs `sh`). Don't use it.
- Reverse-engineering helpers live in the session scratchpad, not the repo (they read Akai's
  binary): ARM disassembly with Capstone through `uv run --with capstone`, a string-reference
  scanner (literal pool + `add Rd, pc, Rm`) and a branch-caller scanner.

### MIDI and USB on the Key 37

- ALSA cards (numbered differently from one boot to the next): "MPC Key 37" (internal surface, USB
  bus 1 dev 2), ACVR (audio), f_midi (USB gadget to a computer), "MIDI Mix" (USB bus 3; it also
  exposes `/dev/hidraw0`).
- The app opens its surface with raw MIDI (card 0, subdevices 0 Public / 1 Private / 2 Port A) and
  every other port through the ALSA sequencer as client 129 "MPC" (JUCE also makes client 128).
  It subscribes to the Midimix both ways and has sent it nothing. A separate process can subscribe
  to it too (sequencer ports are shared). The Midimix's client number changes (28 on 2026-10-22,
  20 on 2026-10-24): find it by its name, "MIDI Mix" (port 0 "MIDI Mix MIDI 1").
- MPC's Preferences > MIDI / Sync (2026-10-24): input "MIDI Mix MIDI 1" has CONTROL on, GLOBAL and
  TRACK off, so a Midimix press only reaches MIDI Learn (nothing learned yet); output "MIDI Mix
  MIDI 1" has TRACK on.
- `usbmon` is built in and `/dev/usbmon0..3` exist: `tools/usbmon.py` captures any bus passively
  (no MPC restart) and decodes USB-MIDI.

### What the app sends its own surface (captures/2026-10-22-*.log)

- Buttons → app: note on `<id>` vel 127 / 0 on Private. Pads → app: channel 10 notes on Private
  (pad 1 = 0x25, 2 = 0x24, 3 = 0x2A, 4 = 0x52, 5 = 0x28, as TheKikGen's `MPC_PAD_*`).
- Button LEDs: CC `<id>` on Private, value 1 dim, 3 lit (2 and 4 also seen). 43 = Track Mute mode
  button; 49 = Shift. **76 / 77 / 74 = mute / solo / rec arm of the selected track**, whatever
  the pad mode (they don't say which track is selected).
- Pad colours: `f0 47 7f 57 65 00 04 <pad 0-15> <r> <g> <b> f7` on Public, 7-bit RGB, **only
  when a pad changes** (entering a mode doesn't resend unchanged pads); blinking is the app
  resending at 2 Hz. CC 53 blinks every 250 ms; `52`/`53` sysex challenge / reply every ~4 s.
- Track Mute pad mode: `7f7f00` yellow = audible, `7f3b00` blinking orange = muted, `007f7f`
  blinking cyan = soloed (Shift + pad), `080000` dim red = silenced by a solo, `000000` = no track.
  Only while that mode is on the pads.

## Plan: the bridge, `midimixd` (decided with the user 2026-10-22, moved onto the native API 2026-10-28)

The Lua scripting could not be reached (above). The bridge is a separate C daemon on the MPC that
talks to mpc-tests' native API (below): its watch says every track's state, its commands change it.
No memory reading, no MIDI Learn, nothing sent to MPC as MIDI.

- **Process:** static musl like `seqtool` (kernel seq ioctls), to be started by systemd (milestone
  4: ask before installing the unit). It finds the Midimix by name, not client number, and the API
  by its socket; it follows the API's and MPC's restarts.
- **MIDI path:** the bridge subscribes to the Midimix from a private port ("midimixd:Midimix",
  write only, no SUBS capability, so MPC lists it neither as an input nor an output). MPC's own
  Midimix input (CONTROL on, nothing learned) stays harmless.
- **Banks:** strips follow tracks 1–8, bank left / right page by 8 (user's choice). Strip n of bank b
  is the track with `uiIndex` 8b + n: MPC's Track List order, audio tracks included.
- **LEDs** (decided with the user 2026-10-24): mute steady = muted, **blinking (2 Hz) = silenced by
  another track's solo**, off = audible; solo notes = soloed (the Midimix shows them on the mute
  row while SOLO is held, by itself: SOLO is a modifier); arm = armed; bank left / right lit when
  there are tracks that way. Strips without a track, and everything while the API can't be read or
  after the bridge exits: off.
- **Buttons:** a press asks for the opposite of what its LED shows, with that value as `expected`
  (refused if MPC changed it in between). The LED shows the press at once; a refused press flashes
  3 times.
- **Faders:** track volume with pickup (soft takeover), on the API's normalized scale (MPC's fader
  position, 1.0 = +6 dB): CC / 127. One volume or pan command at a time, at most 10 a second.
- **Pan:** each strip's bottom knob (the one above mute; user's choice 2026-10-28), with pickup like
  the faders: CC / 128, so that CC 64 is the centre (0.5), and CC 127 is hard right (1.0). The
  session's pans were all multiples of 1/128 (2026-10-28).
- **Top two knobs and master fader:** nothing yet. Sends need automation Off (below); the master
  channel can't be set through the API.
- **Milestones:** 1. reader prototype (memory, done 2026-10-24; now a diagnostic). 2. seq in / out,
  bank paging, LEDs (done 2026-10-24). 3. orders to MPC, rebuilt on the native API (done 2026-10-28).
  4. systemd unit and install / uninstall.
- Dropped: mirroring MPC's own surface through usbmon (pad colours in Track Mute mode, CC 76 /
  77 / 74), MIDI Learn for orders, a "Midimix Bridge" seq port. `tools/usbmon.py` stays a recon
  tool.

## MPC's native API (mpc-tests' `mpc-native-api` addin, 0.13.0, checked 2026-10-28)

Source and docs in the sibling repo `../mpc` (`native-api/README.md`, `native-api/WATCH.md`,
`findings/native-*.md`). An addin inside MPC (`/data/mpc-addins/mpc-native-api`), pinned to
3.9.1.2's build ID; `mpc-api host-status | host-stop | host-start` manages it without restarting MPC.

- **Socket:** `/tmp/mpc-native-api/api.sock` (directory 0700, socket 0600, same UID only: root).
  One JSON request and newline per connection, one JSON line back; `watch` streams lines instead.
- **Watch:** `{"apiVersion":"0.1","op":"watch"}` → `ack`, a full `snapshot` (154 KB for 8 tracks),
  then a `change` (~32 KB, every sampled track with its capability block) when something changed,
  a `heartbeat` every second, and a full snapshot every 5 s and after anyone's full read. Tracks are
  sampled every 100 ms, 16 per page. Resume after a drop with the last `cursor`; `resync_required`
  means start over without one. The API's stop / start makes a new instance: every reference
  changes. Measured cost: +26 ms/s on MPC's UI thread with 8 tracks, transport stopped.
- **Tracks:** every user track in `uiIndex` order (TrackPool +0x74). References are `instanceId` +
  `projectId` + `trackId`. Mute / solo / arm: plugin, drum, keygroup, audio, MIDI, CV tracks. Mixer
  (`mixer.volume` normalized 0–1, pan, sends, inserts): plugin, drum, keygroup, audio.
- **Commands:** `set_mute` / `set_solo` / `set_record_armed` (boolean, optional `expected`),
  `set_volume` and `set_pan` (0–1, pan centred at 0.5, with the track's `mixer.generation` as
  `mixerGeneration`). With per-sequence
  mute on (MPC's default) a mute names the active sequence: `muteScope: "sequence"` plus the
  `sequence` reference from `muteContext`. Allowed on Main, the Channel Mixer pages and Track Mute,
  stopped or playing a sequence, recording off, automation Off or Read (sends, inserts: Off only;
  pan under Read since 0.13.0); refused elsewhere before anything runs (`NOT_READY` on the Menu
  screen). Under Read, MPC's automation takes a volume or pan back on its next update (Drums and
  Bass in the user's session).
  Receipts in 25–42 ms (2026-10-28). A command costs MPC's UI thread about 13–50 ms (mpc-tests'
  measurements): hence the fader throttle.

## Reading MPC's state from memory (`mpc/mpcread`, checked on the Key 37 2026-10-24)

`mpcread` (behind `mpcstate`; `midimixd` used it until milestone 3) reads with `process_vm_readv`
only: 5 calls per poll, one per level of pointers.
Offsets are in its source, pinned to 3.9.1.2's GNU build ID (`8371199f…`); vtables are offsets
from the load address (0x400000).

- **Tracks:** the TrackPool holds several Track vectors (begin / end / capacity). **+0x74 is every
  user track in the UI's order**, audio included, so it's the one for banks. mpc-tests' "normal"
  vector (+0x44) leaves audio tracks out. The others are listed in `mpcread.c`. The Track Mute
  pads (T01…) and Project +0x15C (selected index, 0-based) follow the +0x74 order. The selected
  index lags the selected-track pointer by one poll.
- **Flags:** Program +0x78 mute, +0xC0 solo, +0x1AC silenced (by its own mute or another track's
  solo; a soloed muted track is not silenced); Track +0x494 rec arm. Checked on plugin, drum,
  audio, keygroup, MIDI and CV tracks, from MUTE / SOLO / REC ARM, from Track Mute pads, and on the
  screen. **Per-sequence mutes** ("Track Mute State Per Sequence", on by default): MPC copies the
  sequence's mutes into Program +0x78 within 1–2 polls of a sequence change, so nothing more to read.
- **Volume:** Program +0x500, a linear gain (1.0 = 0 dB, 0.397 = −8.02 dB, 1.058 = +0.49 dB),
  with copies at +0x504, +0x510, +0x51C, +0x520, +0x52C, +0x5AC. A MIDI track's fader is a CC
  value instead (0–127): not looked at.
- **Topology:** adding tracks, deleting one (flags follow their track), loading a demo (tracks
  appear one by one during the import) and MPC's restart were all followed without a bad read. The
  Project stayed at the same address through a project load; after a restart the watcher finds
  the new pid's Project about 4 s after MPC starts (the heap scan only finds it once the startup
  screen is up). The Project was always in `[heap]` (207–249 MB, scanned in 0.3–0.4 s).
  Reordering tracks: no UI for it found in 3.9 (the Arrange track menu has none).
- **Cost:** a full poll of 10 tracks takes ~100–120 µs (p99 < 0.5 ms). At 30 Hz the reader uses
  0.4% of a core, at 200 Hz 2.5%. During playback of the HipHop demo MPC used 45–46% of a core
  with or without 30 Hz polling, 48% at 200 Hz. MPC logged nothing about audio, but it may not
  log xruns: **a listening test is still to do**. The kernel has `PER_VMA_LOCK` and MPC's memory
  is all resident (RSS = size), so MPC's page faults don't wait on the reader's mmap lock.
- **Losing MPC:** a heap scan costs ~0.4 s of CPU, so the milestone 2 midimixd only rescanned when
  the Project was gone (MPC restarting), at once, then backed off while MPC had no Project yet (2 s
  doubling to 30 s, back to 2 s for a new pid). A bad read while the Project's vtables still match
  is MPC in the middle of a change: keep polling.

## midimixd (milestone 3, checked on the Key 37 2026-10-28)

`mpc/midimixd/`: `midimixd.c` (the Midimix, LEDs, buttons, faders), `api.c` (the native API client:
watch model, commands), `json.c` (a small in-place JSON reader). `midimixd [-v]` logs to stdout;
`-v` adds every LED message, the Midimix's input and the API traffic. Checked on the user's session
with presses sent to its port (`seqtool send <client>:0 90 07 7f`, faders `b0 1b <value>`) and its
log, everything restored afterwards:

- LEDs from the first snapshot ~40 ms after start. MPC's own MUTE button reached the LED through
  the watch.
- Mute, solo, rec arm on a selected plugin track: applied in 25–42 ms, LED at the press; while it
  was soloed the other tracks' mute LEDs blinked. On the Menu screen: refused (`NOT_READY`), the
  LED flashed.
- Fader: ignored below the track's level, picked up when crossing it, 8 quick moves sent as 2
  commands. A volume change on MPC's side let go of the fader; it picked up again when crossing.
- API `host-stop` / `host-start`: LEDs off after 2 s, back with the new instance's snapshot.
- Bank right refused with 8 tracks; knobs ignored. SIGTERM / SIGINT turn every LED off. 0.2% of a
  core, 568 kB RSS.
- Pan knobs (API 0.13.0, 2026-10-28, same way, on the Mixer Volume page): refused on the Menu screen
  (`NOT_READY`, the knob let go); a plugin and an audio track; 0, 64, 127 gave 0, 0.5, 1; a sweep of
  16 messages sent 3 commands; pickup from far below by crossing; a pan set through the API let go
  of the knob until it crossed again; fader and pan knob moved together alternated their commands
  (10 a second in all) and both reached their last position. Pans and volumes restored exactly.
- It followed MPC's restart for the API's 0.13.0 install (2026-10-28, from its log: LEDs not looked
  at). During MPC's startup the API sent ~30 watch lines without a `type` (logged as `unknown watch
  frame "?"`, not captured), then midimixd carried on by itself.
- Not checked yet: a Midimix replug, more than 8 tracks (bank 2, the API's 16-track pages), MIDI /
  CV / keygroup tracks, presses by hand on the hardware (the knobs' CCs other than 16 come from the
  Midimix's default map, not a capture).
- Milestone 2 (2026-10-24, memory reader) was checked with a usbmon capture of bus 3: every LED
  message reaches the Midimix; the seq path hasn't changed since.

## Recon

- [x] MPC OS 3.9.1.2; Midimix on the Key 37's USB host port.
- [x] ALSA inventory, how the app starts, mounts, libc (see above).
- [x] Binary: no app symbols; scripting found instead.
- [x] Midimix map confirmed (below).
- [x] Track Mute pad mode and the selected track's LEDs captured.
- [x] A note sent to the Midimix lights its LED; holding SOLO shows the solo LEDs.
- [x] Scripting enabled (flag file, user's OK). [ ] Find the console and how a device script is
      loaded.

## Midimix map (confirmed 2026-10-22, captures/2026-10-22-*.log)

All on MIDI channel 1; buttons send note on vel 127 and note off **vel 127**.

| Control | Message |
|---|---|
| Knobs, strips 1–8, top to bottom | CC 16–18, 20–22, 24–26, 28–30, 46–48, 50–52, 54–56, 58–60 (16 confirmed) |
| Faders 1–8, master | CC 19, 23, 27, 31, 49, 53, 57, 61; 62 (19 and 62 confirmed) |
| Mute 1–8 | notes 1, 4, 7, 10, 13, 16, 19, 22 |
| Solo 1–8 (mute row while SOLO is held) | notes 2, 5, 8, 11, 14, 17, 20, 23 |
| Rec arm 1–8 | notes 3, 6, 9, 12, 15, 18, 21, 24 |
| Bank left, bank right | notes 25, 26 |
| SOLO | note 27, on while held, off on release |

The Midimix lights nothing by itself. A note on with velocity 127 on the same note lights the
LED, velocity 0 turns it off (mute, solo, rec arm, bank checked 2026-10-22). It keeps the solo
LEDs (notes 2, 5 … 23) apart from the mute LEDs and shows them on the mute row while SOLO is held,
so the host just keeps both sets current. Knobs and faders are absolute (not endless, not
motorised).

## Device

- MPC Key 37: Gen1, Rockchip RK3288 (armv7, Cortex-A17), root SSH with no password, BusyBox
  userland (no `timeout`, `head -n` only), the app runs as root under `acvs` (`Restart=always`).
  Details and install paths in `../torpedo/CLAUDE.md`.
- Address: `mpc-key-37.lan.arpa` (static DHCP lease with a DNS entry, 192.168.138.100), so
  `ssh root@mpc-key-37.lan.arpa`. Not always powered on: if it doesn't answer, ask.
- The MPC MCP (the `remote` addin) is available in Claude sessions: screenshots, touches,
  hardware buttons, MIDI into MPC through seq client 130 "MPC Remote".

## Tools

- `uv run python tools/usbmon.py capture captures/<name>.log --secs N [--bus 1 3]`: runs on the
  MPC over SSH (`cat /dev/usbmon<bus>` into `/tmp`, killed by pid). Bus 1 = MPC surface, bus 3 =
  Midimix. Stopping it early leaves the remote `cat`s running: kill them and delete
  `/tmp/usbmon*.bin`.
- `make -C mpc deploy` builds `mpc/seqtool/seqtool.c` (static, kernel `/dev/snd/seq` ioctls, no
  libasound) and copies it to `/tmp/seqtool` on the MPC: `seqtool ports`, `seqtool send 20:0 90 01
  7f` (note / CC only, never SysEx), `seqtool dump 20:0 [secs]`. The Midimix's client number changes
  (20 on 2026-10-28): check `seqtool ports`.
- `make -C mpc deploy` also builds `mpc/midimixd` (above; run it with `setsid /tmp/midimixd >
  /tmp/midimixd.log 2>&1 < /dev/null &`, stop it with `kill $(pidof midimixd)`) and `mpc/mpcstate`
  (on `mpc/mpcread`, a diagnostic) into `/tmp`: `mpcstate show`
  (every track's mute / solo / arm / silenced / volume), `mpcstate watch [secs] [hz]` (prints changes, then
  timings; run it with `setsid … > /tmp/mpcstate-watch.log &` to watch while driving the MPC),
  `mpcstate dump project | track|program <n> | at <hex> <bytes>` (raw bytes, to diff on the host).
  Read-only, safe while MPC runs.
- `uv run python tools/usbmon.py decode captures/<name>.log [--all]`: one line per message,
  pad-colour bursts on one line. "host" is the MPC's side of the bus (the MPC app, or midimixd on
  bus 3). `captures/2026-10-22-map-trackmute.log` came from the text API
  and stops at 32 bytes per transfer (half the pads).

## Safety

- Ask before restarting the MPC app (`acvs`) or changing how it starts (systemd units,
  `LD_PRELOAD`), and before writing anywhere outside a scratch folder on the MPC (`/tmp`).
- Never commit Akai's files (the MPC binary, skins, MIDI Learn maps, firmware): analyse them in
  the scratchpad only.
- Don't send SysEx to the Midimix that hasn't been seen in a capture: its editor rewrites the
  unit's mapping over SysEx.
- TheKikGen's MPCLiveXplore-libs: LICENSE says CC0 but its sources carry CC BY-NC 4.0 headers.
  Read it, don't copy it.

## Conventions (same as torpedo)

- On-device code in C, cross-built for armv7 with Zig from PyPI (`uvx --from ziglang`);
  no Docker on this machine. A standalone program can be static musl; a library loaded
  into the MPC process must link against glibc like torpedo's VST did.
- Host-side tools in Python, managed with `uv`; one-off scripts in `tools/`, captures
  in `captures/`.
- Commit only when the user asks. Git identity: `yann-ygn <yann@ygn-effects.com>`.
