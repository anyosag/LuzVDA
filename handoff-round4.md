# Handoff: round 4. HDR during the launch, and the workaround off by default

The same three Claude sessions take part as in round 3, talking through Remote Control messages. Each message starts with the word in capitals below.

| Role | Session | Runs on |
|---|---|---|
| **Cloud** | `session_014Gjk2MvrNtPDUetWZRBLU6` | the cloud; reads the results |
| **PC** | `session_01XyvV4TjBosBRLArmozHhmS` | the Windows PC, elevated |
| **Mac** | `session_016fcTdns2gRRa8g5evrZ1CS` | the MacBook |

The owner approves once in the PC session and once in the Mac session. That approval covers every ⚠ step for that role here. About 25 minutes.

## What changed since round 3

Apollo `claude/luzvda`:
- **`e1ff09d`:** a launch sets the virtual display's HDR itself, after the display configuration and before the encoder probe and the app. Before, a detached thread did it after the app had started. New log lines:
  - `vdisplay: DXGI lists <display> after <n> ms; it arrived with HDR on|off`;
  - `vdisplay: HDR set <n> ms after launch began, <h> HDR switches; the display configuration left HDR on|off, now on|off`.

  `<h>` still counts Apollo's own switches only. display_device's switch shows as `Changing HDR states to:`.
- **`08f78c0`:** `dd_wa_virtual_display_hdr_toggle` is off by default.

## What this round answers

| | Question | Pass when |
|---|---|---|
| **H1** | With the defaults, do HDR launches still come out right? | L1, L3 and L4: `now on`, `Display is HDR: true`, Main10 on the Mac, and screenshots that measure like round 3's L1/L2 (background 28.5/28.5/28.5, sidebar 25.2/25.3/25.3). 0 Apollo HDR switches. |
| **H2** | Does a display that arrives with HDR off still come out right? | L3, which follows an SDR launch: `arrived with HDR off`, `Changing HDR states to: … Enabled`, `left HDR on, now on`, with the same screenshot measurements as L1. |
| **H3** | Is HDR now set during the launch? | For each launch, `HDR set` comes within 300 ms of `ready`. In round 3 the gap was 700 ms or more. There is no `Restoring HDR settings failed` or `Not getting current display` line, and no new warning. |
| **H4** | Does the workaround still work when turned on (phase 2)? | L6 and L7: 2 Apollo switches each, `now on`, Main10, the same screenshot measurements. Note the extra gap between `ready` and `HDR set`. |
| **H5** | Do rebuilds still work? | R1 and R2 as in round 3: `re-creating`, `added … in N ms` with N above 0, a device id, the new `DXGI lists` line, and one display from 6 s in. |
| **Timing** | | Each launch's `DXGI lists … after N ms` and the Mac's time to picture. Compare with round 3, where the picture was usually up at +3 s. |

## Rules for PC and Mac

1. **Approval.** Before your first step, ask the owner once, in two lines:
   > Round 4, role <PC|Mac>: <your ⚠ steps in a few words>. Approve all?
2. **Stop and send `PROBLEM: <what>`** to Cloud and the other role on:
   - an install failure, a crash, or no stream starting twice in a row;
   - more than one active display during a stream, twice in a row.
3. **Settings:** don't change Apollo's settings beyond the two named here: Desktop's `terminate-on-pause`, and `dd_wa_virtual_display_hdr_toggle`. Both are put back at the end, checked by hash.
4. **Never:**
   - touch the signing key, Secure Boot or test signing;
   - push anywhere but `test-results/round4` in anyosag/LuzVDA;
   - lock the PC or the Mac, unless the owner says so directly.
5. **Times and redaction:** times are UTC. Redact what you push: `<ip>`, `<uuid>`, `<computer>`.
6. **Folders:** use `C:\LuzVDA\r4\` on the PC and `/tmp/r4/` on the Mac.

---

## Role PC

### P1. Preflight

1. The PC must be unlocked (`Get-Process LogonUI` returns nothing), with nobody streaming (`C:\LuzVDA\r3\ctl\luzvda-ctl.exe list` shows 0 displays) and ApolloService running.
2. **Find the Apollo build:** the newest green `Build Windows` run on `anyosag/Apollo` branch `claude/luzvda` whose head is `08f78c0` or later.
   ```bash
   gh run list -R anyosag/Apollo -b claude/luzvda -w "Build Windows" -L 5 --json databaseId,headSha,conclusion
   ```
3. No driver changes: LuzVDA stays 0.0.0.9. Round 3's `luzvda-ctl.exe` in `C:\LuzVDA\r3\ctl\` is used for `list` only.

### P2. ⚠ Install Apollo

1. As in round 3's P2: download, check the hashes and attestation, back up `config` to `C:\LuzVDA\r4\config-backup`, and install with `/S`.
2. **Check:**
   - `vdisplay: LuzVDA 0.0.0.9, protocol 1.0` in `sunshine.log`;
   - `sunshine.conf` has no `dd_wa_virtual_display_hdr_toggle` line, so the new default (off) applies.

### P3. ⚠ Desktop's terminate-on-pause off

As in round 3:
1. Back up `apps.json` to `C:\LuzVDA\r4\apps.json.orig`, with its hash.
2. Set Desktop's `terminate-on-pause` to `false`.
3. Restart ApolloService.

### P4. Recording

1. Start round 3's display sampler, writing to `C:\LuzVDA\r4\displays.csv`.
2. Start the driver trace:
   ```powershell
   logman start luzvda-r4 -p "{fdbe20c6-12e0-4cf1-907d-e0b99b08b5ce}" -o C:\LuzVDA\r4\trace.etl -ets
   ```
3. Note `sunshine.log`'s line count.
4. Send `PC READY` to the Mac, with:
   - Apollo's version and commit;
   - your UTC offset;
   - the note "phase 1: defaults (toggle off)".

### P5. Between phase 1 and phase 2

When `PHASE 1 DONE` arrives:
1. Save phase 1's `sunshine.log`, since an Apollo restart starts a new one.
2. ⚠ Copy `sunshine.conf` to `C:\LuzVDA\r4\sunshine.conf.orig`, and record its hash.
3. Add the line `dd_wa_virtual_display_hdr_toggle = enabled`.
4. Restart ApolloService.
5. Check `sunshine.log` for `config: 'dd_wa_virtual_display_hdr_toggle' = enabled` and no warning.
6. Send `PHASE 2 READY` to the Mac.

### P6. Analyze, restore, report

When `MAC DONE` arrives:
1. **Stop the recording:** stop the sampler, then stop and decode the trace (`tracerpt`).
2. **⚠ Restore:**
   - `sunshine.conf` from `sunshine.conf.orig`, and `apps.json` from `apps.json.orig`, each checked by hash;
   - restart ApolloService.

   Leave the new Apollo installed.
3. **From the new `sunshine.log` lines** of both phases, using the Mac's timeline:
   - **For each launch (L1–L7):**
     - `vdisplay: added … in N ms`;
     - `DXGI lists … after N ms; it arrived with HDR …`;
     - `ready in …`;
     - `HDR set … ms after launch began, H HDR switches; the display configuration left HDR …, now …`;
     - the gap between `ready` and `HDR set`;
     - display_device's `Changing HDR states to:` / `back to:` with their JSON;
     - Apollo's `HDR enabled …` or `… without switching it off first`;
     - `Display is HDR`;
     - `HDR metadata for the client: …`;
     - any warning or error between launch and stream start.
   - **R1/R2:**
     - `re-creating`, then `added … in N ms`, and the `DXGI lists` line;
     - the configuration's `device_id`: say whether it's empty;
     - the available devices;
     - the sampler's counts;
     - every `Capture restarts:` line, with the swap-chain events within 2 s of it.
4. **After restoring,** with no stream running: list the displays Windows has active (friendly name, active or not, primary). Say whether both of the PC's monitors are active. Don't change anything if one isn't.
5. **Report.** Write `results/round4-<date>.md` on a new branch `test-results/round4` from `origin/claude/luzvda-foundation`, and push it. Include:
   - the pass table above, pass or fail per row, with the evidence;
   - a per-launch table;
   - surprises.
6. Send `ROUND4 DONE` to Cloud with the link and a line per row. Leave the PC unlocked.

---

## Role Mac

### M1. Preflight

1. As in round 3:
   - find `LL` and the host;
   - close LuzLunar's window;
   - check that a screenshot works;
   - record whether the HDR peak override (`fork.hdrPeakOverride`) is set, and the settings hash.
2. `caffeinate -dimsu &` until the end, at most 2 hours.
3. Wait for `PC READY`, for at most 60 minutes.

### M2. Phase 1: the defaults

Use round 3's stream block: screenshot at +18 s, `SIGINT` at +22 s. Record when the picture comes up. Screenshots go in `/tmp/r4/`.

| Test | Before it | Resolution | FPS | HDR | Quit after |
|---|---|---|---|---|---|
| Before | – | – | – | – | `"$LL" quit "<host>"` |
| L1 | – | 3024x1964 | 120 | on | yes |
| L2 | – | 3024x1964 | 120 | **off** | yes |
| L3 | – | 3024x1964 | 120 | on | yes |
| L4 | – | 3024x1964 | 120 | on | no |
| R1 | ⚠ set the HDR peak override to 1000 | 3024x1964 | 120 | on | no |
| R2 | ⚠ put the override back as found | 3024x1964 | 120 | on | **yes** |

L3 follows the SDR launch L2, so its display arrives with HDR off (H2).

**Measure each HDR screenshot** (L1, L3, L4, R2) with round 3's fixed regions (background, sidebar), and compare with round 3's L1/L2: 28.5/28.5/28.5 and 25.2/25.3/25.3. R1, described at 1,000 nits, should again be about 4% brighter (29.6/29.7/29.7).

Then send `PHASE 1 DONE` to the PC and wait for `PHASE 2 READY`, for at most 20 minutes.

### M3. Phase 2: the workaround on

| Test | Resolution | FPS | HDR | Quit after |
|---|---|---|---|---|
| L5 | 3024x1964 | 120 | **off** | yes |
| L6 | 3024x1964 | 120 | on | yes |
| L7 | 3024x1964 | 120 | on | yes |

Measure L6 and L7 as in M2.

### M4. Report

1. Send `MAC DONE` to the PC with:
   - the timeline (test, start and end in UTC, exact options);
   - the time to picture for each test;
   - the screenshot measurements, against round 3's;
   - the `[fork] Describing` lines;
   - any errors.
2. Check that the override and the settings hash are as you found them.
3. Stop `caffeinate`. Don't lock the Mac.
