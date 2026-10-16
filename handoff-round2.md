# Handoff: round 2. Sign LuzVDA 0.0.0.9, then check Apollo's HDR fix and re-creation

Three Claude sessions take part:

| Role | Session | Runs on |
|---|---|---|
| **Cloud** | `session_014Gjk2MvrNtPDUetWZRBLU6` | the cloud; it updates Apollo between the PC's two parts |
| **PC** | `session_01XyvV4TjBosBRLArmozHhmS` | the Windows PC, elevated |
| **Mac** | `session_016fcTdns2gRRa8g5evrZ1CS` | the MacBook |

They talk through Remote Control messages, as in the retest. Each message starts with the word in capitals below.

The owner approves once in each of the PC and Mac sessions. That one approval covers every ⚠ step for that role in this file.

## What this round checks

The retest ([report](https://github.com/anyosag/LuzVDA/blob/test-results/retest/results/retest-2026-10-16.md)) found that **Apollo always read the virtual display's HDR as off**:
- a resume that kept HDR switched it off and back on;
- a resume to SDR left the display in HDR.

Apollo `c22efa5` fixes that.

The retest couldn't check two things:
- whether a **re-created** display gets "only this display" again, because nothing was re-created;
- why the **encoder restarts**, because none happened in 20-second streams.

This round also ships LuzVDA **0.0.0.9** (CI run 9, commit `3fd1e90`), which adds:
- traces when Windows gives the driver a swap chain or takes it away;
- luzvda-ctl's real error codes;
- long names by default.

| | Pass when |
|---|---|
| S: signing | `LuzVDA.dll`, `luzvda.cat` and `luzvda-ctl.exe` are `Valid`, signed by thumbprint `F9B9A1D427D86DAE49CA012F7B73C45B84107215` |
| S: 0.0.0.9 on its own | All of these hold: the device is `OK`, with no reboot flag; driver version `0.0.0.9`; a test display is added, listed (its mode labelled "preferred"), seen by the sampler, and removed; the trace shows `SwapChainAssigned` for it |
| L1 (held 75 s) | 0 mode changes; ready in under 1 s; every `Capture restarts:` line is recorded, with its reason and the swap-chain events near it |
| R1: 3024x1890, HDR | 1 mode change, **0 HDR switches**, `Display is HDR: true` |
| R2: 3024x1964, **SDR** | 1 mode change, **1 HDR switch**, `Display is HDR: false` |
| R3: 3024x1964, HDR | 0 mode changes, at most 2 HDR switches, `Display is HDR: true` |
| R4: the Mac reports 1000 nits | `re-creating`; `streams to` shows HDR 1000; display_device's "Using the following configuration" at R4; **only the virtual display active**; `Display is HDR: true` |
| R5: back to 1600 nits, quit after | `re-creating` again; only the virtual display active |
| Every stream | the sampler shows exactly one active display, the virtual one, from start + 6 s to end − 1 s |

## Rules for PC and Mac

1. **Approval.** Before your first step, ask the owner once, in two lines:
   > Round 2, role <PC|Mac>: <list your ⚠ steps in a few words>. Approve all?

   With a yes, don't ask again, except where this file says to or something unexpected happens.
2. **Stop and send `PROBLEM: <what>`** to the Cloud session and the other role if:
   - signing or an install fails;
   - the device shows a problem code;
   - no stream starts twice in a row;
   - a crash;
   - more than one active display during a stream, twice in a row.
3. **Don't:**
   - change Apollo's settings beyond the one named here;
   - touch the signing key beyond signing this build (never export it);
   - touch Secure Boot or test signing;
   - push anywhere but `test-results/round2` in anyosag/LuzVDA.
4. **Times are UTC:** whole seconds on the Mac, milliseconds on the PC. Redact everything you push: `<ip>`, `<uuid>`, `<computer>`.
5. Everything for this round goes in `C:\LuzVDA\r2\` (on the PC) or `/tmp/r2/` (on the Mac). Don't overwrite `C:\LuzVDA\signed`, which holds 0.0.0.7, the fallback.

---

## Role PC

### Part S: sign and publish LuzVDA 0.0.0.9

1. **Download** CI run `38969006760`, artifact `LuzVDA-0.0.0.9-ci`, to `C:\LuzVDA\r2\ci`:
   ```bash
   gh run download 38969006760 -R anyosag/LuzVDA -n LuzVDA-0.0.0.9-ci -D C:/LuzVDA/r2/ci
   ```
   Extract `LuzVDA-0.0.0.9-unsigned.zip` to `C:\LuzVDA\r2\unsigned`.
2. ⚠ **Sign** with the existing key. Don't run `new-signing-cert.ps1`.
   ```powershell
   C:\LuzVDA\r2\unsigned\sign-package.ps1 -Zip C:\LuzVDA\r2\ci\LuzVDA-0.0.0.9-unsigned.zip
   ```
   - Expand `LuzVDA-0.0.0.9.zip` to `C:\LuzVDA\r2\signed`.
   - Check that the three files are `Valid` and that `SignerCertificate.Thumbprint` is `F9B9A1D4…7215`.
3. **Fix the display sampler.**
   - The retest's sampler went stale: `Screen.AllScreens` caches its first reading.
   - Write `C:\LuzVDA\r2\work\sampler.ps1`. Every 2 s it starts a **fresh** `powershell -NoProfile` process that reads `[System.Windows.Forms.Screen]::AllScreens` once, then appends `UTC,count,name WxH@x,y[ primary];…` to `C:\LuzVDA\r2\displays.csv`.
   - It stops when `C:\LuzVDA\r2\work\stop-sampler` exists.
   - Start it now.
4. **Start a trace** of the driver's events (provider `LuzVDA`, `{fdbe20c6-12e0-4cf1-907d-e0b99b08b5ce}`):
   ```powershell
   logman start luzvda-s -p "{fdbe20c6-12e0-4cf1-907d-e0b99b08b5ce}" -o C:\LuzVDA\r2\trace-s.etl -ets
   ```
5. ⚠ **Install 0.0.0.9 over 0.0.0.7:**
   ```powershell
   Stop-Service ApolloService
   C:\LuzVDA\r2\signed\luzvda-ctl.exe install C:\LuzVDA\r2\signed\LuzVDA.inf
   Get-PnpDevice | Where-Object { $_.HardwareID -contains 'Root\LuzVDA' } | ForEach-Object { Disable-PnpDevice -InstanceId $_.InstanceId -Confirm:$false; Enable-PnpDevice -InstanceId $_.InstanceId -Confirm:$false }
   ```
   - This is the same device restart Apollo's `install.bat` does.
   - Check `Status OK`, no reboot flag, and `DEVPKEY_Device_DriverVersion` = `0.0.0.9`.
   - Then run `luzvda-ctl.exe version`.
6. **Smoke test.** Use the keepalive pattern from `handoff.md` Part 4 (`luzvda-ctl ping` every second).
   ```powershell
   C:\LuzVDA\r2\signed\luzvda-ctl.exe add 6a0b9c1e-3f4d-4c2a-9e7b-1d2c3b4a5f60 --name "MacBook Pro" --mode 3024x1964@120 --size 302x196 --p3 --hdr 1600,1000,0.005 --no-hold
   ```
   - Wait 6 s. Then `luzvda-ctl list`: expect "preferred" on its mode.
   - **Check the sampler shows the new display.** If it doesn't, fix the sampler before going on.
   - `luzvda-ctl remove 6a0b9c1e-3f4d-4c2a-9e7b-1d2c3b4a5f60`, then stop the keepalive.
7. **Stop the trace and decode it:**
   ```powershell
   logman stop luzvda-s -ets
   tracerpt C:\LuzVDA\r2\trace-s.etl -o C:\LuzVDA\r2\trace-s.xml -of XML -y
   ```
   - Expect `SwapChainAssigned` (Connector, LuidLow, LuidHigh), then `SwapChainUnassigned` and `SwapChainEnded` (Result 0) at the removal.
   - If tracerpt doesn't decode TraceLogging, try `Get-WinEvent -Path C:\LuzVDA\r2\trace-s.etl -Oldest`. Report which worked.
8. `Start-Service ApolloService`. Check that `sunshine.log` shows `vdisplay: LuzVDA 0.0.0.9, protocol 1.0`.
9. ⚠ **Publish** the signed build, as in `handoff.md` Part 6:
   ```bash
   gh release create signed-0.0.0.9 C:/LuzVDA/r2/ci/LuzVDA-0.0.0.9.zip C:/LuzVDA/r2/ci/LuzVDA-0.0.0.9.zip.sha256 \
     -R anyosag/LuzVDA --target 3fd1e9093a7f551b38fd62e6a1c72a115c894cb6 --prerelease \
     --title "LuzVDA 0.0.0.9 (signed)" \
     --notes "Signed on the owner's PC from CI run 38969006760 (provenance verified). Certificate thumbprint F9B9A1D427D86DAE49CA012F7B73C45B84107215. Smoke test: <one line>."
   ```
   Download the zip back from the release and check its SHA256.
10. **Send `SIGNED`** to the Cloud session with:
    - the release URL;
    - the zip's SHA256;
    - one line per pass-table row for S.

    Then wait for `APOLLO READY`, for at most 60 minutes.

### Part P: install Apollo and signal the Mac

1. ⚠ **Install the Apollo run named in `APOLLO READY`**, as in `handoff-retest.md` P2, into `C:\LuzVDA\r2\apollo`:
   - check the hashes and the attestation;
   - back up `config` to `C:\LuzVDA\r2\config-backup`;
   - install silently with `/S`.

   **Check:** LuzVDA `OK` at `0.0.0.9`, and `sunshine.log` shows `vdisplay: LuzVDA 0.0.0.9, protocol 1.0`.
2. ⚠ **Turn off `terminate-on-pause`** for Desktop, exactly as in `handoff-retest.md` P3. Back up `apps.json` to `C:\LuzVDA\r2\apps.json.orig`, with its hash.
3. Note the line count of `sunshine.log`. Confirm that the sampler is running and that it shows only the PC's monitors now.
4. **Start the trace:**
   ```powershell
   logman start luzvda-p -p "{fdbe20c6-12e0-4cf1-907d-e0b99b08b5ce}" -o C:\LuzVDA\r2\trace-p.etl -ets
   ```
5. **Send `PC READY`** to the Mac session, with:
   - the Apollo version and commit;
   - LuzVDA 0.0.0.9;
   - your UTC offset.

   Wait for `MAC DONE` or `PROBLEM`, for at most 40 minutes. Don't touch the PC's displays meanwhile.

### Part A: analyze, restore, report

1. Stop the sampler. Stop and decode the trace (`trace-p`).
2. From the new `sunshine.log` lines, keep those matching:
   `vdisplay:|streams to|Capture restarts|re-creating|display_device|Using the following configuration|New streaming session|Client dynamicRange|Error|Warning`
   - Convert the times to UTC.
   - Assign them, the sampler rows and the trace events to each test, using the Mac's timeline.
3. Fill in the pass table. For **L1**:
   - list every `Capture restarts:` line;
   - put the swap-chain events within 2 s of each one beside it;
   - say whether a restart had no swap-chain event nearby.
4. **Restore `apps.json`** and check its hash. Leave Apollo and LuzVDA 0.0.0.9 installed.
5. **Report.** Write `results/round2-2026-10-16.md` on a new branch, `test-results/round2`, from `origin/claude/luzvda-foundation`, and push it. Use the same layout as the retest report.
6. **Send `ROUND2 DONE`** to the Cloud session, with the link and a pass/fail line per row.

---

## Role Mac

### M1. Preflight

1. As in `handoff-retest.md` M1:
   - find `LL` and the host name;
   - close LuzLunar's window;
   - check that a screenshot works.
2. **Keep the Mac awake** until the end: `caffeinate -dimsu &`.
   - The wait for the PC can be about 45 minutes.
   - Ask the owner to keep it plugged in.
3. **Find how LuzLunar stores the HDR peak setting.** It's a Qt setting in `com.moonlight-stream.Moonlight`, key `fork/hdrPeakOverride`.
   - Look at how the existing fork keys appear: `defaults read com.moonlight-stream.Moonlight | grep -i fork`. Qt may write `/` as `.`.
   - Record whether `hdrPeakOverride` is already set, and to what. You put that value back at the end.
4. Wait for `PC READY` or `PROBLEM`, for at most 60 minutes.

### M2. The streams

Use the same single-command stream block as `handoff-retest.md` M3, with screenshots in `/tmp/r2/`. **L1 is held longer:** take its screenshot at +18 s and send `SIGINT` at **+75 s**. Everything else uses +18 s and +22 s as before.

| Test | Before it | Resolution | FPS | HDR | Quit after |
|---|---|---|---|---|---|
| Before | – | – | – | – | `"$LL" quit "<host>"` |
| L1 | – | 3024x1964 | 120 | on | no (held 75 s) |
| R1 | – | 3024x1890 | 120 | on | no |
| R2 | – | 3024x1964 | 120 | **off** | no |
| R3 | – | 3024x1964 | 120 | on | no |
| R4 | ⚠ set the HDR peak override to **1000** | 3024x1964 | 120 | on | no |
| R5 | ⚠ put the override back as found in M1 | 3024x1964 | 120 | on | **yes** |

- **R4 setting:** use `defaults write com.moonlight-stream.Moonlight <key as found> -int 1000`.
- **R4 check:** the newest log's `[fork] Describing … HDR 1000/…` line confirms the override took. If it still says 1600, the key is wrong: skip R4 and R5 (just put the override back) and report it.
- **After R5,** check with `defaults read` that the override is as you found it.

After each stream, note:
- the screenshot verdict: it fills the screen or the area below the notch; no bars; the taskbar is where expected;
- the `[fork]` lines and any errors from the newest `/tmp/Moonlight-*.log`.

### M3. Report

1. **Send `MAC DONE`** to the PC session, with:
   - the build;
   - the timeline (test, start and end in UTC, exact options);
   - each screenshot verdict;
   - the `[fork] Describing` lines from L1 and R4;
   - any errors.

   Don't send the screenshots.
2. Stop `caffeinate`.
