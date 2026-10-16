# Handoff: test the frame channel on the PC while the owner is away

This is for the **PC** Claude session (`session_01XyvV4TjBosBRLArmozHhmS`): Claude Code on the Windows PC, elevated. The **Cloud** session (`session_014Gjk2MvrNtPDUetWZRBLU6`) wrote the code and reads the results. There's no Mac in this test.

Messages between the sessions use Remote Control. Each starts with the word in capitals below.

## What this tests

LuzVDA protocol 1.1 adds a **frame channel**: the driver copies each frame of a display into shared GPU textures that Apollo could read directly, instead of capturing the display through Windows' Desktop Duplication, as it does today. This is Phase F's first step. Apollo only gets a frame-channel capture if this test shows it beats Desktop Duplication.

`luzvda-ctl capture-test` measures both on the same frames:
1. It adds a display.
2. A small window in its top-left corner draws the time each frame is presented.
3. Both paths read that time back, frame by frame.

**Nothing here changes Apollo.** Apollo never opens a channel, and the test driver is replaced by 0.0.0.9 at the end.

The questions:
1. **Does the channel work at all:**
   - Does it open?
   - Do its frames decode?
   - Which way did the objects open: by duplicating the driver's handles, or by name across session 0?
2. **Latency:** p50/p95 for each path, and the per-frame lead of one over the other on the same frames.
3. **Missed frames:** the share of presented frames each path delivered.
4. **Recovery:** restarts and the longest wait for a new frame, including during HDR switches.
5. **Cost:** frames the driver dropped for lack of a free slot.

## For the owner

1. In this PC session, approve once (the session asks).
2. **When you leave, leave the PC signed in and unlocked** (don't press Win+L).
   - Desktop Duplication can't capture the lock screen from here, and the test window can't draw on it.
   - The session keeps the PC awake while it works, and locks it when done.
3. Nothing else. Results reach the Cloud session, which reports to you there.

## Rules

1. **Approval.** Ask the owner once, in two lines:
   > Frame channel test: sign the test driver 0.0.0.N with your key (no release), stop Apollo, install it, run 7 capture tests (~15 min, a virtual display appears), put 0.0.0.9 back, restart Apollo, lock the PC. Start when you've left and the build is ready. Approve all?

   With a yes, don't ask again unless something unexpected happens.
2. **Start only when all of these hold:**
   - the owner approved;
   - `BUILD READY` arrived from Cloud;
   - the PC is unlocked: `Get-Process LogonUI -ErrorAction SilentlyContinue` returns nothing;
   - nobody is streaming: `luzvda-ctl list` (0.0.0.9's, in `C:\LuzVDA\r2\signed`) shows 0 displays.

   **If it's locked or streaming:** check again every 5 minutes for up to 2 hours, then send `PROBLEM: locked` or `PROBLEM: streaming` to Cloud and stop.
3. **Stop and send `PROBLEM: <what>`** to Cloud if:
   - signing or an install fails;
   - the device shows a problem code or needs a restart;
   - a crash, or a System-log error from LuzVDA.

   A capture test that reports `failed` for one path isn't a problem: record it and go on.
4. **Don't:**
   - publish a release;
   - export the signing key;
   - touch Secure Boot or test signing;
   - change Apollo's settings;
   - leave the test driver installed;
   - push anywhere but `test-results/frame-channel` in anyosag/LuzVDA.
5. **Logs and times.**
   - Put everything in `C:\LuzVDA\fc\`, with every command's output in `C:\LuzVDA\fc\logs\NN-name.txt`.
   - Times are UTC.
   - Redact what you push: `<ip>`, `<uuid>` (except the test GUID below), and `<computer>`.
6. The test GUID is `6a0b9c1e-3f4d-4c2a-9e7b-1d2c3b4a5f60`. The capture test adds and removes that display itself (`--add`).

## Steps

### F0. Keep the PC awake

Start a background PowerShell job that calls `SetThreadExecutionState(ES_CONTINUOUS | ES_SYSTEM_REQUIRED | ES_DISPLAY_REQUIRED)` and sleeps, until `C:\LuzVDA\fc\work\stop-awake` exists. Use `Add-Type` for the P/Invoke.

### F1. Download the build named in BUILD READY

1. Download it:
   ```bash
   gh run download <run id> -R anyosag/LuzVDA -n LuzVDA-0.0.0.<N>-ci -D C:/LuzVDA/fc/ci
   ```
2. Check that the run's `headSha` is the commit `BUILD READY` names.
3. Extract `LuzVDA-0.0.0.<N>-unsigned.zip` to `C:\LuzVDA\fc\unsigned`.

### F2. ⚠ Sign

Use the existing key, as in round 2. **Don't run `new-signing-cert.ps1`.**
```powershell
C:\LuzVDA\fc\unsigned\sign-package.ps1 -Zip C:\LuzVDA\fc\ci\LuzVDA-0.0.0.<N>-unsigned.zip
Expand-Archive C:\LuzVDA\fc\ci\LuzVDA-0.0.0.<N>.zip -DestinationPath C:\LuzVDA\fc\signed -Force
```
Check:
- `LuzVDA.dll`, `luzvda.cat` and `luzvda-ctl.exe` are `Valid`, signed by thumbprint `F9B9A1D427D86DAE49CA012F7B73C45B84107215`;
- the attestation passed.

### F3. ⚠ Install the test driver

1. Install it and restart the device:
   ```powershell
   Stop-Service ApolloService
   C:\LuzVDA\fc\signed\luzvda-ctl.exe install C:\LuzVDA\fc\signed\LuzVDA.inf
   Get-PnpDevice | Where-Object { $_.HardwareID -contains 'Root\LuzVDA' } | ForEach-Object { Disable-PnpDevice -InstanceId $_.InstanceId -Confirm:$false; Enable-PnpDevice -InstanceId $_.InstanceId -Confirm:$false }
   ```
2. Check:
   - `Status OK`, no reboot flag, `DEVPKEY_Device_DriverVersion` = `0.0.0.<N>`;
   - `luzvda-ctl.exe version` says protocol **1.1**.

Leave ApolloService stopped until F6.

### F4. Start the driver trace

```powershell
logman start luzvda-f -p "{fdbe20c6-12e0-4cf1-907d-e0b99b08b5ce}" -o C:\LuzVDA\fc\trace.etl -ets
```

### F5. The capture tests

Run each from `C:\LuzVDA\fc\signed` with the full path to `luzvda-ctl.exe`, saving the whole output.
- Between tests, wait 5 s and check that `luzvda-ctl list` shows 0 displays.
- If a test hangs past its time plus 60 s, end it, note it, and run `luzvda-ctl remove 6a0b9c1e-3f4d-4c2a-9e7b-1d2c3b4a5f60`.

| Test | Options after `capture-test 6a0b9c1e-3f4d-4c2a-9e7b-1d2c3b4a5f60 --add` | What it answers |
|---|---|---|
| T1 | `--hdr on --seconds 30` | both paths in HDR (FP16) |
| T2 | `--hdr off --seconds 30` | both paths in SDR (8-bit) |
| T3 | `--hdr on --seconds 40 --hdr-switch-every 8` | recovery from HDR switches |
| T4 | `--hdr on --seconds 20 --only channel` | the channel alone |
| T5 | `--hdr on --seconds 20 --only duplication` | Desktop Duplication alone |
| T6 | `--hdr on --seconds 20 --debug-privilege` | whether duplicating the driver's handles works when allowed |
| T7 | `--hdr on --seconds 30` | T1 again, for variance |

Each run ends with a `RESULT …` line. Keep the per-path blocks too: the setup line, latencies, the driver's lines, waits and restarts.

### F6. Apollo with the test driver

1. `Start-Service ApolloService`, wait 10 s.
2. Check `sunshine.log` for `vdisplay: LuzVDA 0.0.0.<N>, protocol 1.1`, with no warning after it.
3. `Stop-Service ApolloService`.

### F7. The trace

```powershell
logman stop luzvda-f -ets
tracerpt C:\LuzVDA\fc\trace.etl -o C:\LuzVDA\fc\trace.xml -of XML -y
```
List, with UTC times and fields:
- every `FrameChannelOpened`, `FrameTexturesMade` and `FrameTexturesFailed`;
- the `SwapChain*` events.

### F8. ⚠ Put 0.0.0.9 back

1. Reinstall it and restart the device:
   ```powershell
   C:\LuzVDA\r2\signed\luzvda-ctl.exe install C:\LuzVDA\r2\signed\LuzVDA.inf
   Get-PnpDevice | Where-Object { $_.HardwareID -contains 'Root\LuzVDA' } | ForEach-Object { Disable-PnpDevice -InstanceId $_.InstanceId -Confirm:$false; Enable-PnpDevice -InstanceId $_.InstanceId -Confirm:$false }
   Start-Service ApolloService
   ```
2. Check:
   - `DriverVersion` = `0.0.0.9`, `Status OK`, no reboot flag;
   - `sunshine.log` shows `vdisplay: LuzVDA 0.0.0.9, protocol 1.0`;
   - ApolloService is running.

### F9. Report

1. Write `results/frame-channel-<date>.md` on a new branch `test-results/frame-channel`, from `origin/claude/frame-channel`, and push it. Include:
   - versions: the driver's `0.0.0.<N>`, the run, the commit, Apollo's version;
   - for T1–T7, the full capture-test output (it has no secrets) and a table of the `RESULT` fields;
   - which way the channel's objects opened (the setup line), in T1 and in T6;
   - the trace events from F7;
   - F6's log line, and F8's checks;
   - anything unexpected.
2. Send `FRAMES DONE` to Cloud, with the link and one line per test.

### F10. Finish

1. Create `C:\LuzVDA\fc\work\stop-awake`.
2. ⚠ Lock the PC: `rundll32.exe user32.dll,LockWorkStation`.
