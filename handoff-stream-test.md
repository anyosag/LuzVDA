# Handoff: stream from the Mac through Apollo and LuzVDA

This is for **Claude Code running locally on the Windows PC**, elevated, with permission prompts on. The owner is the user, and they also operate the Mac. A cloud Claude session wrote the code; you install it, watch the PC side while the user streams, collect the evidence, and hand it back. You don't change any code.

## Where things stand

- **[First test](https://github.com/anyosag/LuzVDA/blob/test-results/first-test/results/first-test-0.0.0.7.md) (LuzVDA 0.0.0.7):** the driver installed and worked on this PC (Windows 11 build 26200, RTX 5080, Secure Boot off). Windows read its HDR brightness and P3 colors, took new modes without the display being re-added, and showed long names. It was signed with the owner's key and published as release [`signed-0.0.0.7`](https://github.com/anyosag/LuzVDA/releases/tag/signed-0.0.0.7).
- **Apollo** (branch `claude/luzvda` in [anyosag/Apollo](https://github.com/anyosag/Apollo)) now installs that signed LuzVDA in place of SudoVDA, and describes the client's screen to it.
- **LuzLunar** (branch `claude/client-display` in [anyosag/LuzLunar](https://github.com/anyosag/LuzLunar)) sends the Mac's screen to Apollo with each launch and resume.

**This test answers:**
1. **Launch:** does the display come up as the Mac's screen (size, resolution, HDR at the right brightness), and how many mode changes and HDR switches does a launch cost?
2. **Resume:** does a resume at another resolution change the mode without re-creating the display?
3. **The Mac's side:** what does macOS report about the screen? The `[fork] Describing` line settles how LuzLunar recognizes the XDR panel.
4. **Whole system:** is everything else, like isolated layout, other clients, and stopping and starting Apollo, still fine?

## Rules

1. **Ask the user before each step marked ⚠**: a restore point, installing Apollo, running an installer script by hand, restoring the previous Apollo, publishing results. Say what the step does and wait for a yes.
2. **Never:**
   - edit `sunshine.conf`, `apps.json` or other Apollo settings unless the user asks;
   - export or copy the signing key;
   - enable test signing or change Secure Boot;
   - upload the config backup: it holds pairing certificates and Web UI credentials.
3. **Stop and report** on anything unexpected: an install error, no virtual display, a device with a problem code, a crash, a black stream. Don't improvise fixes.
4. **Log everything** to `C:\LuzVDA\stream\logs\NN-name.txt`.
5. **Elevation:** check it first (the same PowerShell check as in the first test). Apollo's log is under `C:\Program Files\Apollo\config`, which only SYSTEM and Administrators can read.
6. **Full paths:** your shell doesn't run programs from the current folder. Call `luzvda-ctl.exe` and the installer by their full paths. From Git Bash, run PowerShell through `.ps1` files in `C:\LuzVDA\stream\work`.
7. **Redact before anything leaves the PC:**
   - Apollo's log has client IP addresses, device names and unique IDs. Replace IPs with `<ip>` and UUIDs with `<uuid>`.
   - Keep the computer name as `<computer>`.

## Part 0: Preflight

1. Run the elevation check, then record:
   - the Windows build;
   - the current Apollo version: `(Get-Item 'C:\Program Files\Apollo\sunshine.exe').VersionInfo.ProductVersion`;
   - `Get-Service ApolloService`;
   - display devices: `Get-PnpDevice -Class Display | Format-Table Status, FriendlyName, InstanceId`. Expect SudoVDA and LuzVDA both present; LuzVDA is left from the first test.
2. ⚠ Make a restore point: `Checkpoint-Computer -Description "Before Apollo with LuzVDA" -RestorePointType MODIFY_SETTINGS`. Windows allows one per 24 hours; check `Get-ComputerRestorePoint`.
3. **Back up Apollo's settings.** Copy `C:\Program Files\Apollo\config` to `C:\LuzVDA\stream\config-backup` (`robocopy … /E`). It stays on this PC.
4. Ask the user:
   - How do they normally get a virtual display? Apollo's per-app "virtual display" setting, the client's option, or headless mode. Keep it as it is.
   - Do they use the isolated-display option?
   - Which app do they stream for testing ("Desktop" is fine)?

## Part 1: Download the Apollo build

1. Find the newest green Windows build of the branch:
   ```bash
   gh run list -R anyosag/Apollo -b claude/luzvda -w "Build Windows" --status success -L 1 --json databaseId,headSha,displayTitle,createdAt
   ```
   Its commit should be `b4ab8bb` ("Package LuzVDA, signed by the owner, in place of SudoVDA") or later. If the newest run isn't green, stop and tell the user.
2. Download it:
   ```bash
   gh run download <id> -R anyosag/Apollo -D C:/LuzVDA/stream/apollo
   ```
   Keep the `Apollo-windows-<version>` artifact. It holds `Apollo-windows-installer.exe`, `Apollo-windows-portable.zip` and `SHA256SUMS.txt`.
3. Verify it:
   - the hashes against `SHA256SUMS.txt`;
   - `gh attestation verify Apollo-windows-installer.exe -R anyosag/Apollo`.

## Part 2: Install Apollo

⚠ Ask first. The installer:
- replaces the Apollo on this PC and keeps its config;
- stops Apollo's service;
- **removes SudoVDA, and SudoMaker's certificate (`3C918FC7…77CC49`) from Trusted Root and Trusted Publishers**;
- trusts the LuzVDA certificate (`F9B9A1D4…7215`), turns on long display names, and installs LuzVDA 0.0.0.7;
- reinstalls and starts the service.

Run it silently and wait:

```powershell
Start-Process -Wait -FilePath 'C:\LuzVDA\stream\apollo\Apollo-windows-<version>\Apollo-windows-installer.exe' -ArgumentList '/S'
```

Then check and record (`01-install.txt`):

| Check | Expected |
|---|---|
| `Get-PnpDevice -Class Display` | LuzVDA `OK`; **no SudoVDA** device |
| The LuzVDA device's `DEVPKEY_Device_IsRebootRequired` | `False`. The first test needed a restart because the running Apollo held SudoVDA open; the installer now stops Apollo first. Report it either way. |
| `Test-Path Cert:\LocalMachine\Root\3C918FC73525AD8B1521B6DB26B71F694277CC49` and the same under `TrustedPublisher` | `False` |
| The same for `F9B9A1D427D86DAE49CA012F7B73C45B84107215` | `True` |
| `reg query HKLM\SOFTWARE\LuzVDA /v edidSplitName` | `0x1` |
| `& 'C:\Program Files\Apollo\drivers\luzvda\luzvda-ctl.exe' version` | `LuzVDA 0.0.0.7, protocol 1.0` |
| `Get-Service ApolloService` | `Running` |
| `C:\Program Files\Apollo\config\sunshine.log` | `vdisplay: LuzVDA 0.0.0.7, protocol 1.0` and `vdisplay: watchdog timeout 3 s`; no warning starting `vdisplay:` |

**If something's wrong:**
- **A reboot is required:** ask the user before restarting, then repeat the checks.
- **LuzVDA is missing or not `OK`:** ⚠ run `C:\Program Files\Apollo\drivers\luzvda\install.bat` from an elevated prompt, then record its output and the setupapi section as in the first test.
- **Anything else that's off:** stop and report.

Ask the user to look at the Web UI's Audio/Video tab: it should say "LuzVDA virtual display driver status: Ready".

## Part 3: The Mac (the user does this)

Give the user these steps:

1. **Download LuzLunar** while logged in to GitHub: **[LuzLunar-macOS-arm64-6.2.0-fork.ffbb73c](https://github.com/anyosag/LuzLunar/actions/runs/38919733047/artifacts/12054741080)**. It's from the [build of `claude/client-display`](https://github.com/anyosag/LuzLunar/actions/runs/38919733047) and is kept until 2027-01-14. Its SHA256 digest, as GitHub reports it, is `b7fe96c32d9dad078a64dbc8dd9ff13ed58e58124073b48187eef1208e800ab6`.
2. Unzip it, open the DMG, and drag LuzLunar to Applications, replacing any older LuzLunar. Delete `Moonlight.app` if it's there: both share settings, and paired PCs carry over.
3. It's ad-hoc signed, not notarized. Open it the first time with right-click → Open, or run:
   ```
   xattr -dr com.apple.quarantine /Applications/LuzLunar.app
   ```
4. In Settings:
   - "Describe this Mac's screen to Apollo hosts" should be **on** (the default).
   - Leave "HDR peak brightness" on automatic.
   - Resolution: the screen's native **3024 × 1964**. 120 FPS. **HDR on.**
5. Under "This Mac" in Settings, note what it shows for the built-in screen: size, gamut and HDR peak. Tell the Windows Claude.

The Mac's log is `/tmp/Moonlight-<time>.log`, one file per run of the app.

## Part 4: The streams

Before each test, record the line count of `sunshine.log`. After it, save the new lines that match:
`vdisplay:|streams to|Virtual Display|HDR|display_device|Error|Warning`.
Number the tests `S1`, `S2`, … and keep one file per test.

Ask the user the visual questions after each test. They can open Settings → System → Display **inside the stream**: the streamed desktop is the virtual display.

### L1–L5: Five fresh launches

Each launch starts the app, streams about 30 seconds, then **quits the app**, so the next launch is fresh. Use 3024×1964 @ 120 with HDR on.

**Expected in the log:**
- `Client [<device>] streams to 3024x1964 (3024x1890 below the notch), 302x196 mm, 120 Hz, P3, HDR 1600/1000 nits, EDR headroom …`
- `vdisplay: added \\.\DISPLAYn in … ms: 3024x1964x120000 mHz, one of … modes, 302x196 mm, own primaries, HDR`
- `vdisplay: HDR peak 1600 nits, full screen 1000 nits, black 0.005 nits`
- `vdisplay: ready in … ms after launch began, … mode changes`
- `vdisplay: HDR set … ms after launch began, … HDR switches`

Record, for each launch: ready ms, mode changes, HDR set ms, HDR switches, and any warning.

**Ask the user, at the first launch:**
- Does the picture fill the Mac's screen sharply, without being scaled?
- What does the display show in Settings → Display:
  - its name: expect the device name, up to 63 characters;
  - its resolution;
  - the **scale Windows chose**: with a 302 × 196 mm, 3024 × 1964 screen, expect a high value such as 200% or more;
  - whether "Use HDR" is on.
- How does HDR look: highlights, blacks, colors? Better or worse than with SudoVDA?
- How much did the screen flash or go black at launch, on the Mac and on the PC's own monitors? Roughly how many times?

### R1–R5: Five resumes

Launch once, then for each resume **disconnect without quitting the app** (close the stream window), change the setting below in LuzLunar, and resume:

| Resume | LuzLunar setting | Expected |
|---|---|---|
| R1 | 3024 × 1890 (below the notch), 120 | A mode change, no re-creation: `resume asks for 3024x1890x120000`, then `resume handled in … ms, 1 mode changes` |
| R2 | back to 3024 × 1964, 120 | The same |
| R3 | 3024 × 1964, **60 FPS** | A mode change to 60 Hz |
| R4 | a resolution **not** in the list, e.g. custom 2560 × 1600, 120 | `gave \\.\DISPLAYn … modes for the new one`, then a mode change. **It must not say "re-creating".** |
| R5 | 3024 × 1964, 120, **HDR off** | HDR goes off on the display, with no re-creation. This needs Apollo's display HDR option on automatic; note what it's set to. |

Record, for each resume: handled ms, mode changes, HDR switches, and whether the log says "re-creating".

**Ask the user:**
- Did the stream come back at the new size without black bars or scaling?
- Did app windows move during a resume? They shouldn't when the display isn't re-created.

### X1–X3: Odds and ends

These are optional; ask which the user wants.

- **X1: Isolated display**, only if the user uses the option. Launch once and confirm the virtual display ends up alone, as before.
- **X2: Another client.** If the user has plain Moonlight or Artemis on another device, launch from it.
  - There's no `streams to` line, and the display gets the default description: 700×390 mm, about 3,800 nits.
  - Confirm it still works.
- **X3: Restart Apollo** while no one is streaming: `Restart-Service ApolloService`. The log should show the driver opening again, with no displays left over.

## Part 5: The Mac's log

Ask the user to run this in Terminal on the Mac and paste the output to you:

```
grep -h '\[fork\]' /tmp/Moonlight-*.log | tail -60
```

The important line is `[fork] Describing <screen> to the host: …`. It shows the size, the panel and the area below the notch, refresh rates, gamut, and EDR values (potential, current, reference). Those decide the XDR threshold, which is currently a guess at 15.5.

## Part 6: Report

1. Write `C:\LuzVDA\stream\results.md` with:
   - versions: Apollo, LuzVDA, LuzLunar build `6.2.0-fork.ffbb73c`, and the Windows build;
   - the Part 2 checks;
   - a table for the launches (L1–L5) and one for the resumes (R1–R5);
   - the user's visual answers;
   - the Mac's `[fork] Describing` line;
   - the "This Mac" values;
   - any warnings or errors, from Apollo's log and from the System log for the test period:
     ```powershell
     Get-WinEvent -FilterHashtable @{LogName='System'; StartTime=<start>} | Where-Object { $_.LevelDisplayName -in 'Error','Warning' }
     ```
   - anything surprising.

   Redact per rule 7.
2. Show it to the user.
3. ⚠ Ask how to hand it back:
   - **(a)** paste it into the cloud session; or
   - **(b)** push it to the public LuzVDA repository as `results/stream-test-<date>.md` on a new branch `test-results/stream-test`. Clone `claude/luzvda-foundation` and branch from it; don't push to `claude/luzvda-foundation` itself.

## Going back

⚠ Only if the user asks:

1. **Reinstall the previous Apollo**, for example the newest green "Build Windows" run of `master`. Its installer puts SudoVDA and SudoMaker's certificate back.
2. **Restore `config`** from `C:\LuzVDA\stream\config-backup`, with the service stopped.
3. **Remove LuzVDA:** `& 'C:\Program Files\Apollo\drivers\luzvda\luzvda-ctl.exe' uninstall`, or Apollo's uninstaller's LuzVDA question. Removing it is optional; it does nothing when nobody asks it for a display.
4. **Last resort:** the restore point from Part 0.
