# A-GPS (assisted GPS) for the ESP32-C5 — design

**Status: design agreed 2026-10-06 (grill session), nothing implemented.** No roadmap phase
assigned yet. Step 1 is a hardware spike (below) that can kill the whole feature; do not build
past it until it passes.

## Goal

Cut cold-start time-to-first-fix on the **C5's ATGM336H** (CASIC AT6558, mask-ROM `URANUS5`
firmware; see [hardware/olimex-mod-esp32-c5/README.md](hardware/olimex-mod-esp32-c5/README.md))
by loading offline-predicted ephemeris, valid for **7 days**, from the Flipper. C5 only — not
C6 or Heltec.

## Why we generate the data ourselves

- The chip speaks CASIC (`BA CE` framed binary), not u-blox UBX, so AssistNow Offline is not an
  option.
- The only known AT6558 feed is `api.smawatch.cn/epo/ble_epo_offline.bin` (Espruino/Bangle.js 2):
  ~2.7 KB (57-byte text header + 2650 B), **GPS only**, 32 `MSG-GPSEPH` (0x08 0x07) + 1
  `MSG-GPSION` (0x08 0x06) — **no GPSUTC message** (checked 2026-10-06; an earlier version of
  this doc wrongly listed one), **valid ~4 h**. Not a multi-day source. Unofficial, unlicensed.
  **The feed is also dead:** on 2026-10-06 every EPH still had `toe` = 2026-09-26 00:00 GPS time
  (server Last-Modified 2026-09-25), ~10 days stale, so it can test message *acceptance* only,
  not TTFF. Must be fetched over plain http (https fails TLS); espruino.com/agps/casic.base64 is
  a byte-identical mirror.
- MediaTek EPO (28 days) is a different, incompatible format.
- CASIC's own AGNSS server needs an account (public free-trial login is rate-limited).
- So: predict orbits ourselves (Global Locate "LTO" approach) and encode as CASIC messages.
  The CASIC protocol spec (v4.2, public) documents the message layouts; field packing still has
  to be read from it.

References: Espruino discussion #7026 (sending A-GNSS to AT6558; send whole messages, wait for
ACK each), wardriver_rev3 issue #175 (only 2 of 33 blocks ACKed — the risk), CASIC spec v4.2
(`espruino.com/forumattachments/68b597874e0a617692ebebc6a878032f76b34272.pdf`).

## Decisions

| # | Decision |
| --- | --- |
| Scope | Full predicted-ephemeris database, 7-day horizon. File carries its own validity windows; Flipper is not hard-coded to a duration. If measured prediction error is too high, shorten the horizon (tool emits fewer windows). |
| Constellations | **GPS only for v1.** BeiDou, GLONASS → BACKLOG. |
| Trigger | **Manual only for v1** ("Send A-GPS" on the GPS screen). Auto-push on GPS start and push-only-when-cold → BACKLOG. |
| Generation | **Host-side.** Download broadcast nav data (open source, e.g. BKG BRDC — no account), propagate orbits, fit a broadcast-style ephemeris per 4 h window, encode as CASIC `MSG-GPSEPH` + ION + UTC. ~42 windows × ~2.4 KB ≈ 100 KB. |
| Generator form | Small **compiled C#/C CLI tool**, built once, hosted on GitHub Releases, downloaded by the Sync script at run time. |
| Tool trust | **Pinned release tag + SHA-256 in the script**; refuse on mismatch; cached locally by hash. Runs as a separate process that never receives the wdgwars API key. Authenticode signing → BACKLOG (optional). |
| Delivery to Flipper | The existing host script pushes `assist.bin` to the SD card over CLI `storage write_chunk` (same channel as [WARDRIVING_PUBLISH.md](WARDRIVING_PUBLISH.md) §2). No Python install (user decision 2026-09-17 stands). |
| UI rename | Publish screen becomes **Sync** with three options: **Sync all**, **Publish Wardriving Logs**, **Sync A-GPS**. |
| Time hint | Sync script **sets the Flipper RTC from the PC's UTC** at the start of each sync. Time sent with every push. |
| Position hint | Last wardriving fix persisted on SD, sent if present, omitted if not. **Not in the first spike** (time + ephemeris only): the spike must isolate whether the chip accepts our EPH and whether TTFF improves, so it takes the fewest variables. Without a position the chip searches the whole sky, so the spike may understate the benefit; if the speedup is marginal, adding position (one more field in the time-injection message, no new protocol work) is the first thing to try. Needs a persisted last-fix feature that does not exist yet. |
| BLE transfer | Flipper sends the **whole window (~2.4 KB)** via a new authenticated CBOR command (`agps.load`, shape to be added to PROTOCOL.md/CAPABILITIES.md). **C5 buffers it, parses CASIC message boundaries, feeds the chip one message at a time waiting for ACK/NACK**, replies `{sent, acked, nacked}`. Whole messages only; never arbitrary byte chunks (causes FIFO errors). |
| Flipper UI | "Send A-GPS" action on the **GPS screen**. Checks the file's windows against RTC and shows one of: `No A-GPS file on SD`, `A-GPS data expired (last window ended <date>)`, or `A-GPS: 31/34 accepted`. **Never sends expired ephemeris** (worse than none). Result also logged. |

## Build contract (decided 2026-10-06, "do the big build")

**Why we are building despite the spike.** The spike (stationary, clear sky) showed only ~10 s
gain, but the user's real problem is a 15-minute cold start in a car with the GPS on the dash
behind a **heated (likely metal-coated) windshield**, which attenuates GNSS and prevents the 30 s
uninterrupted ephemeris decode. Pre-loaded ephemeris is expected to help most in that case.
Unproven; an in-car test with C/N0 logging is the validation (see "Validation"). Antenna
relocation (uncoated window patch / external roof antenna) is a likely bigger fix and is
independent of this feature.

**Spike results (2026-10-06, stationary clear sky, software cold restart via `$PCAS10,2`,
TTFF mean):** baseline 44.9 s (n=6); time+position 43.9 s (n=3); time+EPH 40.0 s (n=2);
time+position+EPH 34.9 s (n=3). All 34 assist messages ACKed every run. Module keeps time and
position through power loss (backup supply) — it is only missing ephemeris (>4 h old). The
spike counted `q=6` (estimated) as a fix; the module can output `q=6` with no satellites in
view, so use `q>=1` for real fixes in future tests. Scratch code lives in the untracked
`esp32c5_agps_spike/` (delete when done).

**Prediction study (host, partial 2026-10-06):** GPS 7-day position error, numerical
integration (J2 + luni-solar, empirical terms) from 1–2 days of broadcast data: median ~180–250 m,
p95 ~520–750 m; plain extrapolation of the broadcast model is ~3.3 km median at 7 days (not
usable). A few SVs (G15, G25) are off by hundreds of km (manoeuvre/unhealthy): skip SVs whose
recent broadcasts are unhealthy/inconsistent. Python propagator is slow (minutes per SV) — the
production generator must be compiled.

**Generator toolchain:** C# built with the Windows in-box `csc.exe` (.NET Framework 4.x; no
.NET SDK or C compiler is installed on the dev PC). Output exe is released on GitHub Releases;
the Sync script downloads it with a pinned tag + SHA-256 and runs it with no access to the
wdgwars API key.

### SD file: `/ext/apps_data/flipper_esp32_over_ble/agps.bin` (little-endian)

- Header (16 B): magic `"FAGP"`, `u8 version=1`, `u8 constellation_mask` (bit0 = GPS),
  `u16 window_count`, `u32 generated_unix`, `u32 reserved=0`.
- Window table, `window_count` entries (16 B each): `u32 valid_from_unix`, `u32 valid_to_unix`,
  `u32 offset` (from file start), `u16 length`, `u16 reserved=0`. Sorted by `valid_from`.
- Window payloads: back-to-back whole CASIC frames (`BA CE`, checksum included — exactly what is
  written to the GPS UART): up to 32 × `MSG-GPSEPH` (08/07) + one `MSG-GPSION` (08/06). Windows are
  4 h wide (`toe` at the centre, `valid_from = toe-2h`, `valid_to = toe+2h`), ~2.7 KB each; 7 days
  = 42 windows ≈ 115 KB. Max window length 4096 B.
- Flipper selects the window with `valid_from <= RTC_utc < valid_to`; if none: show
  "No A-GPS file" / "A-GPS data expired (last window ended <date>)" and never send.

### BLE command (authenticated CBOR; exact envelope/tags to follow PROTOCOL.md conventions)

`agps.load` (new capability `agps`): request carries `utc` (unix s, from the Flipper RTC),
optional `lat`/`lon`/`alt` (omitted in v1), and the window blob as sequential chunks
(`total`, `off`, `data`; last chunk marks completion). The C5 buffers the blob (≤ 4096 B),
validates every `BA CE` frame boundary and checksum before touching the GPS, then writes
AID-INI (time, flags 0x02, or 0x03 if position given) followed by each frame **one at a time**,
waiting for the CASIC ACK/NACK (1500 ms timeout, 20 s whole-feed budget) between frames. Reply: `{sent, acked, nacked,
timeout}`. Never send `$PCAS10,x` or any module-config command from this path. C5 board only;
C6/Heltec report the capability absent.

### Host script / UI

- Publish screen is renamed **Sync** with three items: **Sync all**, **Publish Wardriving Logs**,
  **Sync A-GPS**. The FAP launches the existing host script with a mode:
  `publish_wardriving.ps1 -Mode All|Publish|Agps`. `Agps` mode: set the Flipper RTC from PC UTC,
  ensure the generator exe (pinned tag + SHA-256, cached by hash), generate 7 days, push
  `agps.bin` via the CLI `storage write_chunk` path, write `agps_sync_result.txt`
  (the existing `wardriving_publish_result.txt` contract is unchanged for Publish).
- GPS screen gets a "Send A-GPS" action (manual only in v1) showing the result on screen.

### Validation

1. Offline: generator output decodes with the spike's decoder; windows round-trip; accuracy vs
   next-day broadcast ephemeris (study numbers above).
2. Hardware: ACK summary on the C5 (`acked == sent`), then an in-car dash test: baseline vs
   assisted cold start with a long timeout and per-satellite C/N0 logged (GSV), `q>=1` fix.
   User-supervised; flashing only on explicit request.

## SD file format (superseded sketch)

See "SD file: `agps.bin`" above.

## Risks — front-load these

1. **Spike first.** Hand-build one `MSG-GPSEPH` from the spec, send it to the ATGM336H, confirm
   ACK and that the chip uses it. wardriver_rev3 saw 2/33 ACKs. If the chip rejects our
   messages, stop.
2. **Measure TTFF before/after** (same location, antenna, power-cycled the same way). The SMA
   file reportedly gave only ~1 min; datasheet cold start is ~35 s. If assistance does not beat
   unassisted cold start, the feature is not worth shipping. Note `$PCAS06`-style status
   gives no ephemeris-state report, so judge by time to first valid RMC/GGA.
3. **VBAT backup.** Datasheet hot start (~1 s) needs VBAT to survive power-off. Not yet checked
   whether the C5 wiring provides it; affects how much assistance matters.
4. **Prediction accuracy.** Measure predicted ephemeris against the next day's real broadcast
   to find the honest usable horizon (expect tens–hundreds of m at 7 days from broadcast data;
   JPL predicted orbits claim <10 m median at 7 days). Only needs to be good enough to find
   satellites quickly; the chip replaces it with live data after ~30 s.
5. **Flipper FAP memory** is tight (see [SOURCE_SPLIT.md](SOURCE_SPLIT.md) size/stack gates).
   Keep SD read + chunked send minimal; all CASIC knowledge lives on the C5.
6. **RTC trust.** A wrong time hint can make assistance useless or harmful; hence the
   Sync-sets-RTC rule.
7. Never send `$PCAS10,x` restarts from routine code (existing rule).

## Suggested order

1. Spike: one hand-built EPH + `AID`/time message to the chip from a throwaway C5 build; log
   ACK/NACK. (Needs explicit user OK per the hardware-safety rule — it writes to the GPS, not
   to flash, but prod firmware currently never writes to the GPS.)
2. Generator tool: RINEX BRDC parse → propagate → fit → CASIC encode; test against the SMA file
   (our output for the same epoch should match its structure) and against live broadcast.
3. C5 `agps.load` + CASIC feeder; protocol docs for both firmwares.
4. Flipper: SD file reader, window selection, GPS-screen action, Sync menu rename.
5. Sync script: RTC set, tool download + hash check, `assist.bin` push.
6. TTFF measurement; update USER_GUIDE.md in the same pass as the UI change.

## C5 implementation (2026-10-06, build + host-test verified, hardware pending)

- Wire contract: [PROTOCOL.md](PROTOCOL.md) "`agps` command and status payloads" (capability `agps`,
  `arguments.action = "load"`, <=256 B chunks, per-chunk `partial`, unsolicited `complete`).
- Code: `components/feb_app_core/feb_cap_agps.c` (command, feed task, completion poll),
  `feb_agps_casic.c` (frame validator, AID-INI, ACK parser, feeder), `feb_agps_blob.c` (chunk
  state machine), `feb_agps_cmd.c` (argument codec); compiled in only where `board_config.h` sets
  `FEB_HAS_AGPS 1` (C5). Host tests: `tests/esp32/test_agps.c`.
- UART sharing: `esp32c5/main/location.c`'s NMEA task takes a mutex around each read; the feed
  holds it for its duration (`location_agps.h`). NMEA state goes stale for the ~3-5 s feed.
- Feed completion is delivered from the existing 1 s reassembly-check callout (no new NimBLE
  callout), so the `complete` status can lag the feed by up to 1 s.
