# Handoff: round 3. HDR at stream start, the display-rebuild fix, and a long stream

Three Claude sessions take part, talking through Remote Control messages as in round 2. Each message starts with the word in capitals below.

| Role | Session | Runs on |
|---|---|---|
| **Cloud** | `session_014Gjk2MvrNtPDUetWZRBLU6` | the cloud; reads the results |
| **PC** | `session_01XyvV4TjBosBRLArmozHhmS` | the Windows PC, elevated |
| **Mac** | `session_016fcTdns2gRRa8g5evrZ1CS` | the MacBook |

The owner approves once in the PC session and once in the Mac session. That approval covers every ⚠ step for that role here. About 75 minutes, most of it the long stream.

## What this round answers

| | Question | Pass when |
|---|---|---|
| **S5** | Does a LuzVDA display need Apollo's HDR off-and-on at stream start? Apollo `f39b12b` adds `dd_wa_virtual_display_hdr_toggle` to turn it off. | With it **off** (phase 2), HDR launches show `Display is HDR: true`, Main10 on the Mac, and screenshots that look like phase 1's (same brightness and colors). Each launch costs at most 1 HDR switch. |
| **Rebuild** | Apollo `9e208ba` waits for Windows to drop a removed display before adding it again. | `re-creating`, then `added … in N ms` with N above 0. The re-applied configuration has a device id (not `""`), and no `display_device doesn't list` warning. Only the virtual display is active from 6 s in. |
| **Restarts** | What makes the encoder restart mid-stream? | Every `Capture restarts:` line in a 30-minute stream is listed with its time, the driver's swap-chain events within 2 s of it, and whether a second Desktop Duplication on the same display (`capture-test`) restarted too. |
| **B7** | Does the Mac get its own screen's brightness? Apollo `2b5ee3b` logs `HDR metadata for the client: …`. | Peak about 1600, full frame about 1000, black about 0.005 nits. |

## Rules for PC and Mac

1. **Approval.** Before your first step, ask the owner once, in two lines:
   > Round 3, role <PC|Mac>: <your ⚠ steps in a few words>. Approve all?
2. **Stop and send `PROBLEM: <what>`** to Cloud and the other role on:
   - an install failure, a crash, or no stream starting twice in a row;
   - more than one active display during a stream, twice in a row.
3. **Settings:** don't change Apollo's settings beyond the two named here: Desktop's `terminate-on-pause`, and `dd_wa_virtual_display_hdr_toggle`. Both are put back at the end, checked by hash.
4. **Never:**
   - touch the signing key, Secure Boot or test signing;
   - push anywhere but `test-results/round3` in anyosag/LuzVDA;
   - lock the PC or the Mac, unless the owner says so directly.
5. **Times and redaction:** times are UTC. Redact what you push: `<ip>`, `<uuid>`, `<computer>`.
6. **Folders:** use `C:\LuzVDA\r3\` on the PC and `/tmp/r3/` on the Mac.

---

## Role PC

### P1. Preflight

1. The PC must be unlocked (`Get-Process LogonUI` returns nothing), with nobody streaming (`luzvda-ctl list` shows 0 displays) and ApolloService running.
2. **Find the Apollo build:** the newest green `Build Windows` run on `anyosag/Apollo` branch `claude/luzvda` whose head is `2b5ee3b` or later.
   ```bash
   gh run list -R anyosag/Apollo -b claude/luzvda -w "Build Windows" -L 5 --json databaseId,headSha,conclusion
   ```
3. **Find `luzvda-ctl`:** the newest green `Build` run on `anyosag/LuzVDA` branch `claude/frame-channel` whose head is `f74c3ba` or later. Only its `luzvda-ctl.exe` is used, unsigned and run directly. No driver changes: LuzVDA stays 0.0.0.9.
   ```bash
   gh run list -R anyosag/LuzVDA -b claude/frame-channel -w Build -L 3 --json databaseId,number,headSha,conclusion
   ```
   - Download the run's `LuzVDA-0.0.0.<N>-ci` artifact and check its `SHA256SUMS.txt`.
   - Extract `luzvda-ctl.exe` from the unsigned zip to `C:\LuzVDA\r3\ctl\`.

### P2. ⚠ Install Apollo

1. Download, check the hashes and attestation, back up `config` to `C:\LuzVDA\r3\config-backup`, and install with `/S`, as in round 2's Part P.
2. **Check:**
   - `vdisplay: LuzVDA 0.0.0.9, protocol 1.0` in `sunshine.log`;
   - `sunshine.conf` has no `dd_wa_virtual_display_hdr_toggle` line, so the toggle is on, the default.

### P3. ⚠ Desktop's terminate-on-pause off

As in round 2:
1. Back up `apps.json` to `C:\LuzVDA\r3\apps.json.orig`, with its hash.
2. Set Desktop's `terminate-on-pause` to `false`.
3. Restart ApolloService.

### P4. Recording

1. Start the fresh-process display sampler from round 2, writing to `C:\LuzVDA\r3\displays.csv`.
2. Start the driver trace:
   ```powershell
   logman start luzvda-r3 -p "{fdbe20c6-12e0-4cf1-907d-e0b99b08b5ce}" -o C:\LuzVDA\r3\trace.etl -ets
   ```
3. Note `sunshine.log`'s line count.
4. Send `PC READY` to the Mac, with:
   - Apollo's version and commit;
   - your UTC offset;
   - the note "phase 1: toggle on".

### P5. Between phase 1 and phase 2

When `PHASE 1 DONE` arrives:
1. ⚠ Copy `sunshine.conf` to `C:\LuzVDA\r3\sunshine.conf.orig`, and record its hash.
2. Add the line `dd_wa_virtual_display_hdr_toggle = disabled`.
3. Restart ApolloService.
4. Check `sunshine.log` for a warning about the setting.
5. Send `PHASE 2 READY` to the Mac.

### P6. The long stream (phase 3)

When `STREAMING` arrives from the Mac:
1. `C:\LuzVDA\r3\ctl\luzvda-ctl.exe list` shows Apollo's display. Use its GUID.
2. In the background, run:
   ```powershell
   C:\LuzVDA\r3\ctl\luzvda-ctl.exe capture-test <guid> --only duplication --fps 120 --seconds 1680
   ```
   Save its whole output to `C:\LuzVDA\r3\logs\long-capture-test.txt`.
   - It draws a small, changing code in the display's top-left corner, so the stream has something moving.
   - It runs a second Desktop Duplication on the same display and prints the UTC time of each restart.
   - Without `--add`, it neither adds nor removes the display.
   - Don't pass `--hdr`.

### P7. Analyze, restore, report

When `MAC DONE` arrives:
1. **Stop the recording:** stop the sampler, then stop and decode the trace (`tracerpt`).
2. **⚠ Restore:**
   - `sunshine.conf` from `sunshine.conf.orig`, and `apps.json` from `apps.json.orig`, each checked by hash;
   - restart ApolloService.

   Leave the new Apollo installed.
3. **From the new `sunshine.log` lines,** using the Mac's timeline:
   - **Each launch (L1–L8):**
     - `vdisplay: ready in …`;
     - `HDR set … ms after launch began, H HDR switches`;
     - the HDR lines: `HDR enabled …`, or `… without switching it off first`;
     - `Client dynamicRange` / `Display is HDR`;
     - `HDR metadata for the client: …`.
   - **R1/R2:**
     - `re-creating`, and `added … in N ms`;
     - `Using the following configuration` with its `device_id`: say whether it's empty;
     - any `display_device doesn't list` warning;
     - the available devices;
     - the sampler's counts.
   - **The long stream:**
     - every `Capture restarts:` line, with its UTC time;
     - `capture-test`'s restart times;
     - the driver's `SwapChain*` events within 2 s of each restart.
     - Say for each restart whether it happened on both Apollo's and `capture-test`'s duplication (Windows' doing), or only Apollo's.
4. **Report.** Write `results/round3-<date>.md` on a new branch `test-results/round3` from `origin/claude/luzvda-foundation`, and push it. Include:
   - the pass table above, pass or fail per row, with the evidence;
   - the per-launch details;
   - the long stream's restart table;
   - surprises.
5. Send `ROUND3 DONE` to Cloud with the link and a line per row. Leave the PC unlocked.

---

## Role Mac

### M1. Preflight

1. As in round 2:
   - find `LL` and the host;
   - close LuzLunar's window;
   - check that a screenshot works;
   - find the HDR peak override key (`fork.hdrPeakOverride` in `com.moonlight-stream.Moonlight`), and record whether it's set.
2. `caffeinate -dimsu &` until the end, at most 3 hours.
3. Wait for `PC READY`, for at most 60 minutes.

### M2. Phase 1: the HDR toggle on

Use the single-command stream block from round 2: screenshot at +18 s, `SIGINT` at +22 s. Screenshots go in `/tmp/r3/`.

| Test | Resolution | FPS | HDR | Quit after |
|---|---|---|---|---|
| Before | – | – | – | `"$LL" quit "<host>"` |
| L1 | 3024x1964 | 120 | on | yes |
| L2 | 3024x1964 | 120 | on | yes |
| L3 | 3024x1964 | 120 | **off** | yes |

Then send `PHASE 1 DONE` to the PC and wait for `PHASE 2 READY`, for at most 20 minutes.

### M3. Phase 2: the HDR toggle off, and the rebuild

| Test | Before it | Resolution | FPS | HDR | Quit after |
|---|---|---|---|---|---|
| L4 | – | 3024x1964 | 120 | on | yes |
| L5 | – | 3024x1964 | 120 | on | yes |
| L6 | – | 3024x1964 | 120 | **off** | yes |
| L7 | – | 3024x1964 | 120 | on | no |
| R1 | ⚠ set the HDR peak override to 1000 | 3024x1964 | 120 | on | no |
| R2 | ⚠ put the override back as found | 3024x1964 | 120 | on | **yes** |

**Compare each phase 2 HDR screenshot (L4, L5, L7) with L1/L2:**
- same overall brightness?
- same colors?
- nothing washed out or too dark?

That comparison is the main S5 evidence. Describe any difference concretely.

Then send `PHASE 2 DONE` to the PC.

### M4. Phase 3: the long stream

1. Start L8: 3024x1964, 120 fps, HDR on, `--no-quit-after`, in the background.
2. As soon as the picture is up, send `STREAMING` to the PC.
3. Keep it running for **30 minutes**. Take a screenshot every 5 minutes, using as many tool calls as you need, each under 10 minutes.
4. Then send `SIGINT`, wait for LuzLunar to exit, and run `"$LL" quit "<host>"`.
5. From that stream's log, note:
   - any line about a decoder reset, a reconnect, an IDR or keyframe request, or frame loss;
   - the `[fork]` lines.

### M5. Report

1. Send `MAC DONE` to the PC with:
   - the timeline (test, start and end in UTC, exact options);
   - the screenshot verdicts, including the phase 1 against phase 2 comparison;
   - the `[fork] Describing` lines;
   - phase 3's log findings;
   - any errors.
2. Check that the override is as you found it.
3. Stop `caffeinate`.
