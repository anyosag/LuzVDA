# Handoff: retest of resumes and launches, run by two Claude sessions

One Claude Code session runs this on the **Windows PC** (role **PC**), another on the **MacBook** (role **Mac**). They coordinate through files on a GitHub branch, so the owner only starts them.

## For the owner: what to do

1. **On the PC:** open an elevated terminal (Run as administrator), start Claude Code, and paste:
   > Follow https://github.com/anyosag/LuzVDA/blob/claude/luzvda-foundation/handoff-retest.md in the role PC. I approve every step marked ⚠ in it.
2. **On the Mac:** open Terminal, start Claude Code, and paste:
   > Follow https://github.com/anyosag/LuzVDA/blob/claude/luzvda-foundation/handoff-retest.md in the role Mac. I approve every step marked ⚠ in it.
3. **For about 20 minutes:**
   - Keep the Mac awake, plugged in, and on the same network as the PC.
   - Don't use the PC; the test streams its "Desktop" app.
4. **Only if asked:**
   - approve a one-time macOS permission (screen recording, for screenshots); or
   - sign in to GitHub on the Mac with `gh auth login`.

Results land in [`results/retest-<date>.md`](https://github.com/anyosag/LuzVDA/tree/test-results/retest) on the `test-results/retest` branch. Pass that link to the cloud session.

## What this retests

The stream test (Apollo 0.4.8.75) found:
- **R4:** a resume to a resolution the display was given afterwards (`UPDATE_MODES`) re-created the display.
- **After that re-creation:** the PC's monitors came back on, windows moved, and the cursor was lost.
- **Every resume** cost 2 mode changes and 2 HDR switches.
- **Encoder restarts** mid-stream had no logged reason.

Apollo commit `1e4af45` on `claude/luzvda` fixes the first three and logs the reason for the fourth. **Pass means:**

| | Pass when |
|---|---|
| Resumes to listed modes (R1, R2, R6) | 1 mode change, 0 HDR switches, no "re-creating" |
| Resumes to new modes (R3 2560×1600, R5 1512×982) | "gave … modes", then the change works with **no "re-creating"** |
| HDR off (R4) | 0 mode changes, at most 1 HDR switch |
| Launches (L1–L3) | as before: 0 mode changes, ready in under 1 s |
| Every stream | **exactly one active display** on the PC: the virtual one (`dd_configuration_option = ensure_only_display`) |
| Any "Capture restarts:" line | recorded with its reason |

## Rules for both roles

1. **⚠ steps are pre-approved** by the owner's start message. Ask the owner only where this file says to, or when something unexpected happens.
2. **Stop and write the problem to the bus** (below) if something breaks:
   - the install fails;
   - no stream starts;
   - a crash;
   - the PC shows more than one active display during a stream **twice in a row**.
3. **Don't:**
   - change Apollo's settings beyond the one this file names;
   - touch the signing key, Secure Boot or test signing;
   - push anywhere but the `test-results/retest` branch.
4. **Times:** record every time in **UTC**: whole seconds on the Mac (`date -u +%H:%M:%S`; macOS's `date` has no milliseconds), and milliseconds on the PC (`(Get-Date).ToUniversalTime().ToString('HH:mm:ss.fff')`). The two clocks agreed to within 1–2 s in the last test.
5. **Redact everything pushed:** IP addresses become `<ip>`, UUIDs `<uuid>`, the PC's name `<computer>`.

## The bus: branch `test-results/retest` in anyosag/LuzVDA

Both roles clone the repository:

```bash
git clone https://github.com/anyosag/LuzVDA retest-bus && cd retest-bus
```

- **The PC creates the branch:** `git switch -c test-results/retest origin/claude/luzvda-foundation`, with its first push.
- **The Mac** switches to it once it exists: `git fetch origin && git switch test-results/retest`.

| File | Written by | Means |
|---|---|---|
| `retest/pc-ready.md` | PC | Apollo is installed and set up; the Mac may start. Holds versions and the PC's UTC offset. |
| `retest/mac-done.md` | Mac | All streams are over. Holds the Mac's timeline and findings. |
| `retest/problem.md` | either | Something broke. The other role stops and reports. |
| `results/retest-<date>.md` | PC | The final report |

- **To send:** commit with `git add retest/<file> && git commit -m "<file>"`, then `git pull --rebase` (once the branch is on GitHub) and `git push -u origin test-results/retest`.
- **To wait:** every 20 seconds, run `git fetch origin`, then `git show origin/test-results/retest:retest/<file>` (and the same for `problem.md`), for at most the time given.

If git can't push from the Mac, try `gh auth login` once (ask the owner). If that isn't possible, print the file's content and ask the owner to paste it into the PC session. That's the only fallback that needs them.

---

## Role PC

### P1. Preflight

1. Check you're elevated, using the same check as in the earlier handoffs. If you aren't, ask the owner to restart Claude Code as administrator.
2. Record:
   - the Windows build;
   - `Get-Service ApolloService`;
   - the Apollo version;
   - `Get-PnpDevice -Class Display`;
   - your UTC offset.

### P2. Install Apollo `1e4af45`

1. Find its build:
   ```bash
   gh run list -R anyosag/Apollo -b claude/luzvda -w "Build Windows" -L 3 --json databaseId,headSha,status,conclusion
   ```
   - Use the run whose `headSha` starts with `1e4af45`, or a later one.
   - If it's still running, check every 2 minutes for up to 30.
   - If it failed, write `retest/problem.md` and stop.
2. Download and verify it as in the stream test:
   ```bash
   gh run download <id> -R anyosag/Apollo -D C:/LuzVDA/retest/apollo
   ```
   Check the hashes in `SHA256SUMS.txt`, then run `gh attestation verify Apollo-windows-installer.exe -R anyosag/Apollo`.
3. ⚠ Back up `C:\Program Files\Apollo\config` to `C:\LuzVDA\retest\config-backup`. Then install silently:
   ```powershell
   Start-Process -Wait -FilePath '<path>\Apollo-windows-installer.exe' -ArgumentList '/S'
   ```
4. **Check:**
   - LuzVDA is `OK`, with no reboot flag;
   - `ApolloService` is running;
   - `sunshine.log` has `vdisplay: LuzVDA 0.0.0.7, protocol 1.0`.

### P3. Let resumes resume

1. In `C:\Program Files\Apollo\config\apps.json`, find the app named `Desktop`. If it has `"terminate-on-pause": true`:
   1. ⚠ Copy `apps.json` to `C:\LuzVDA\retest\apps.json.orig` and record its SHA256.
   2. Set the value to `false`, changing nothing else.
   3. `Restart-Service ApolloService`.

   Otherwise, note that it's already off.
2. Confirm `dd_configuration_option = ensure_only_display` in `sunshine.conf`, and record what it is.

### P4. Start the samplers and signal ready

1. Note the line count of `sunshine.log`.
2. Start a background **display sampler** that every 2 seconds appends one line to `C:\LuzVDA\retest\displays.csv`: the UTC time, then the active displays as `name WxH@x,y primary`.
   ```powershell
   Add-Type -AssemblyName System.Windows.Forms
   while ($true) {
     $t = (Get-Date).ToUniversalTime().ToString('HH:mm:ss.fff')
     $d = [System.Windows.Forms.Screen]::AllScreens | ForEach-Object { "$($_.DeviceName) $($_.Bounds.Width)x$($_.Bounds.Height)@$($_.Bounds.X),$($_.Bounds.Y)$(if ($_.Primary) {' primary'})" }
     "$t,$($d.Count),$($d -join ';')" | Add-Content C:\LuzVDA\retest\displays.csv
     Start-Sleep 2
   }
   ```
3. Push `retest/pc-ready.md` with:
   - the Apollo version and commit;
   - LuzVDA 0.0.0.7;
   - the UTC offset;
   - whether `terminate-on-pause` was changed;
   - the time.

### P5. Wait for the Mac

Wait for `retest/mac-done.md`, or `retest/problem.md`, for at most 45 minutes. While waiting, don't touch the PC's displays.

### P6. Collect and analyze

1. Stop the sampler.
2. From the new lines of `sunshine.log`, keep those matching:
   `vdisplay:|streams to|Capture restarts|re-creating|display_device|New streaming session|Client dynamicRange|Error|Warning`.
   Convert their local times to UTC with your offset.
3. Using the Mac's timeline in `mac-done.md`, assign the log lines and the `displays.csv` rows to each test: L1–L3 and R1–R6.
4. **For each test, record:**
   - **Launches:**
     - added ms and ready ms;
     - mode changes;
     - HDR set ms and HDR switches.
   - **Resumes:**
     - asked;
     - "gave … modes";
     - any "runs … after the change" retries;
     - "re-creating";
     - handled ms;
     - mode changes;
     - HDR switches.
   - **During the stream:** the active-display counts from `displays.csv` (min/max), and the names when the count isn't 1.
   - **Mac's verdicts:** from `mac-done.md`.
5. List every `Capture restarts:` line with its time and test.
6. Collect System-log errors and warnings for the test window:
   ```powershell
   Get-WinEvent -FilterHashtable @{LogName='System'; StartTime=<start>}
   ```

### P7. Restore and report

1. If you changed `apps.json`: stop the service, copy `apps.json.orig` back, check the SHA256 matches, and start the service.
2. Leave the new Apollo installed.
3. Write `results/retest-<date>.md`:
   - versions;
   - the pass table above, with **pass / fail** per row;
   - per-test details;
   - the capture restarts;
   - surprises;
   - redacted log excerpts for any failure.
4. Push it to `test-results/retest`.
5. Tell the owner, in one short message, the results link and the pass/fail summary.

---

## Role Mac

### M1. Preflight

1. Find LuzLunar's executable: `ls /Applications/LuzLunar.app/Contents/MacOS/`. Call it `LL` below.
2. Check its version: the newest `/tmp/Moonlight-*.log` or "This Mac" shows `6.2.0-fork.ffbb73c`.
3. **Quit LuzLunar's window if it's open:** `osascript -e 'quit app "LuzLunar"'`. The command line needs it closed.
4. **Find the paired PC's name** as LuzLunar knows it. Look in `defaults read com.moonlight-stream.Moonlight` for the hosts' names, then check it with `"$LL" list "<host>"`, which should list `Desktop`. Ask the owner only if this fails.
5. Keep the Mac awake for the whole run: `caffeinate -dimsu &`.
6. **Try one screenshot:** `screencapture -x /tmp/retest-check.png`.
   - If macOS asks for screen-recording permission, ask the owner to allow it once.
   - If screenshots still fail, go on without them.

### M2. Wait for the PC

Wait for `retest/pc-ready.md`, or `retest/problem.md`, for at most 40 minutes.

### M3. The streams

**One stream:** run each test as a single command, so it finishes within one tool call (about 45 seconds).

```bash
start=$(date -u +%H:%M:%S)
"$LL" stream "<host>" "Desktop" --resolution <WxH> --fps <fps> <--hdr|--no-hdr> <--quit-after|--no-quit-after> --display-mode fullscreen &
pid=$!
sleep 18; screencapture -x /tmp/retest-<test>.png
sleep 4; kill -INT $pid
for i in $(seq 15); do kill -0 $pid 2>/dev/null || break; sleep 1; done
kill -TERM $pid 2>/dev/null
end=$(date -u +%H:%M:%S)
echo "<test> $start $end"
sleep 8
```

- `kill -INT` ends the stream as closing its window does. The loop waits up to 15 seconds for LuzLunar to exit, then makes sure it has. The app keeps running on the PC unless `--quit-after` was given.
- **If a stream doesn't start within 30 seconds:** for example, the process exits early, or the newest log has an error. Record it and go on to the next test. **After two such failures in a row,** write `retest/problem.md` and stop.

**The tests, in order:**

| Test | Resolution | FPS | HDR | Quit after | What it checks |
|---|---|---|---|---|---|
| Before | – | – | – | – | Run `"$LL" quit "<host>"`, so the first launch is fresh. It's fine if nothing is running. |
| L1 | 3024x1964 | 120 | on | yes | a fresh launch |
| L2 | 3024x1964 | 120 | on | yes | a fresh launch |
| L3 | 3024x1964 | 120 | on | **no** | a fresh launch; the app keeps running for the resumes |
| R1 | 3024x1890 | 120 | on | no | below the notch: a listed mode |
| R2 | 3024x1964 | 60 | on | no | 60 Hz: a listed mode |
| R3 | 2560x1600 | 120 | on | no | **a new mode, from a 60 Hz one**: the old R4 |
| R4 | 3024x1964 | 120 | **off** | no | HDR off |
| R5 | 1512x982 | 120 | on | no | another new mode |
| R6 | 3024x1964 | 120 | on | **yes** | back to the screen's mode; quits the app at the end |

**After each stream:**
1. **The screenshot:**
   - Does the picture fill the screen (or the area below the notch for R1)?
   - Any black bars?
   - Does the Windows desktop look like a single display? Look for the taskbar where you'd expect it, and no half-visible second screen.
   - Note the cursor, if you can see it.
2. **From the newest `/tmp/Moonlight-*.log`:** the `[fork]` lines and any error.

### M4. Report

1. Push `retest/mac-done.md` with:
   - the build;
   - the host name, redacted if it's the PC's computer name;
   - the timeline: test, start and end in UTC, with the exact command options;
   - each test's screenshot verdict;
   - the `[fork] Describing` line, from one launch;
   - any errors.

   Don't push the screenshots.
2. Stop `caffeinate`.
3. Tell the owner the Mac's part is done.
