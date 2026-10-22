# midimix

Goal: make an Akai **Midimix** a proper control surface for the owner's **MPC Key 37**
(the one from `../torpedo`): a translation layer running on the MPC, between the
MPC app and the Midimix, both ways. Above all, the Midimix's mute / solo / rec-arm
(and bank) LEDs must follow what happens on the MPC, including changes made on the
MPC's screen and pads. Not a VST plugin.

## Status (2026-10-22)

- Recon done on the Key 37 (MPC 3.9.1.2), see below.
- Midimix LEDs answer note on vel 127 on the same note (checked with `seqtool`, 2026-10-22).
- Decided: the bridge daemon below ("Plan"). Next: milestone 1.
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
  Installed today: `remote` (the MPC MCP server, seq client "MPC Remote", HTTP 6720).
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

- ALSA cards: 0 "MPC Key 37" (internal surface, USB bus 1 dev 2), 1 ACVR (audio), 2 f_midi
  (USB gadget to a computer), 3 "MIDI Mix" (USB bus 3; it also exposes `/dev/hidraw0`).
- The app opens its surface with raw MIDI (card 0, subdevices 0 Public / 1 Private / 2 Port A) and
  every other port through the ALSA sequencer as client 129 "MPC" (JUCE also makes client 128).
  It subscribes to the Midimix (client 28) both ways and has sent it nothing. A separate process
  can subscribe to client 28 too (sequencer ports are shared).
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

## Plan: the bridge, `midimixd` (decided with the user 2026-10-22)

The Lua scripting could not be reached (above), so: a separate C daemon on the MPC that owns the
LED state and corrects it from what MPC shows on its own surface. No change to the MPC process.
Reading MPC's internal state (reverse engineering, an addin) stays a later research track.

- **Process:** static musl like `seqtool` (kernel seq ioctls), started by systemd before `acvs`
  (ask before installing the unit). It finds the Midimix by name, not client number.
- **MIDI path:** the bridge subscribes to the Midimix and creates a seq port "Midimix Bridge" that
  MPC lists as an input. In MPC (Preferences > MIDI / Sync) the Midimix input's Control goes off
  and the bridge port's Control on, so MPC only gets translated messages.
- **Banks:** strips follow tracks 1–8, bank left / right page by 8 (user's choice). Strip n of bank b
  is track t = 8b + n. The bridge sends one fixed message per track and function (draft: mute =
  ch 1 note t−1, solo = ch 2, rec arm = ch 3, fader = ch 4 CC t−1; knobs and master to decide).
- **MPC side:** MIDI Learn does the muting (user's choice): learned once per track and function.
  Learn one by hand, save it, read the file and generate the rest if the format allows. If MIDI
  Learn takes absolute values (127 on / 0 off), use them: a press then always does what the LED
  shows, and a drifted LED heals on the first press. Otherwise toggles.
- **State per track:** mute, solo, arm, each known or unknown. Sources: the bridge's own presses;
  usbmon on bus 1 (MPC's surface): a mirror of the 16 pad colours kept from MPC's start, the Track
  Mute mode LED (CC 43) and the pad bank LEDs (CC 35…), so in Track Mute mode every visible track's
  mute / solo is known; the selected track's CC 76 / 77 / 74 once we can tell which track that is.
- **LEDs:** mute lit = muted (default, could be a setting: Ableton lights active tracks), solo
  notes = soloed, arm = armed, bank left lit when b > 0.
- **Known gaps:** mutes made on the screen outside Track Mute mode, other tracks' rec arm, project
  loads: fixed by the next Track Mute visit or the next press. Faders jump when changing bank
  (no pickup: MPC sends no volume back).
- **To check first:** pad → track order and pad banks in Track Mute mode; what MIDI Learn offers
  for track mute / solo / arm / volume (absolute or toggle) and how it stores mappings;
  `TrackMuteUseTrackColours` (pads in track colours) changes the colour rules.
- **Milestones:** 1. seq in / out, bank paging, own-state LEDs, bridge port (no MPC change but MIDI
  prefs and learn). 2. usbmon reader and Track Mute mirror. 3. systemd unit and install / uninstall.

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
  libasound) and copies it to `/tmp/seqtool` on the MPC: `seqtool ports`, `seqtool send 28:0 90 01
  7f` (note / CC only, never SysEx), `seqtool dump 28:0 [secs]`. The Midimix is client 28 today.
- `uv run python tools/usbmon.py decode captures/<name>.log [--all]`: one line per message,
  pad-colour bursts on one line. `captures/2026-10-22-map-trackmute.log` came from the text API
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
