# Handoff: sign, install and test LuzVDA on this PC

> Run once, on 0.0.0.7: [results](https://github.com/anyosag/LuzVDA/blob/test-results/first-test/results/first-test-0.0.0.7.md). From the next version on, long names are on by default, so `edidSplitName` = 0 shows the 13-character form instead. When you run it, call `luzvda-ctl.exe` by its full path: some shells, Claude Code's among them, don't run programs from the current folder.

This is for **Claude Code running locally on the Windows PC** (NVIDIA GPU, Windows 11 24H2 or later). A cloud Claude session built the driver; you run what only this PC can run, collect the results, and hand them back. You don't change the driver's code.

## What this is

- **LuzVDA** ([anyosag/LuzVDA](https://github.com/anyosag/LuzVDA), branch `claude/luzvda-foundation`) is a virtual display driver, a fork of SudoVDA.
  - Apollo, a game-streaming host ([anyosag/Apollo](https://github.com/anyosag/Apollo)), adds a LuzVDA display when a Mac running LuzLunar starts streaming.
  - Each display describes the Mac's screen: its physical size, P3 colors, HDR brightness (1,600 / 1,000 nits), and exact resolutions.
- CI builds it unsigned. Windows installs only drivers signed by a certificate it trusts, so the owner signs each build with their own key. The key is made on this PC and can't be exported.
- The driver has **never run on a real PC**. This is its first test. Expect surprises, and record them rather than working around them.
- SudoVDA stays installed throughout. The Apollo running on this PC today still uses it.

## Rules

1. **Ask the user before each step marked ⚠.** These change the system: a restore point, creating and trusting a certificate, installing the driver, registry changes, restarting a device, publishing a release, installing Apollo. Say what the step does and wait for a yes.
2. **Never:**
   - export, copy, print or upload the private key;
   - pass `-SkipAttestation` without the user's OK;
   - enable test signing (`bcdedit /set testsigning`), disable Secure Boot, or change driver signature enforcement;
   - run `luzvda-ctl remove-legacy` or otherwise uninstall SudoVDA;
   - edit, rebuild or re-sign anything but the CI build. A local build wouldn't match its provenance.
3. **Stop and report** on anything unexpected: an install error, a device with a problem code, a display that doesn't appear, an error in the System event log. Don't improvise fixes to the driver or to Windows.
4. **Log everything.** Save each command's full output to `C:\LuzVDA\logs\NN-name.txt`, numbered in order. The results file at the end quotes them.
5. **Run as administrator.** The driver opens only for SYSTEM and Administrators, and installing needs it too. The user should start this Claude Code session from an **elevated** terminal, with permission prompts left on, so they approve each command. Check at the start:
   ```powershell
   ([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
   ```
   If that prints `False`, ask the user to restart Claude Code from an elevated terminal. Don't try to elevate yourself.
6. **PowerShell from Git Bash:** write the commands to a `.ps1` file in `C:\LuzVDA\work` and run `powershell -NoProfile -ExecutionPolicy Bypass -File <file>`. That avoids quoting problems.
7. Tell the user before step T1 that **displays will appear and disappear**. Windows extends the desktop onto each one, which can briefly rearrange or flicker their real monitors and move windows. They shouldn't be in the middle of anything.

## Folders

```
C:\LuzVDA\ci        the CI artifact (unsigned zip + SHA256SUMS.txt)
C:\LuzVDA\unsigned  the unsigned zip, extracted (holds the two scripts)
C:\LuzVDA\signed    the signed zip, extracted (what gets installed)
C:\LuzVDA\logs      every command's output
C:\LuzVDA\work      your scripts
C:\LuzVDA\src       optional: a read-only clone of the repo, for reference
```

---

## Part 0: Preflight

Read-only checks except the restore point. Log the results to `00-preflight.txt`.

1. **Windows build:** `(Get-CimInstance Win32_OperatingSystem).BuildNumber` must be **26100 or higher**. If it's lower, stop: the driver doesn't install there.
2. **Elevation:** rule 5 above.
3. **signtool:** `Get-ChildItem "${env:ProgramFiles(x86)}\Windows Kits\10\bin\*\x64\signtool.exe"`.
   - If it's missing, ask the user how to install it. Either the Windows SDK installer with only "Windows SDK Signing Tools for Desktop Apps" ticked, or the full SDK through winget: run `winget search "Windows SDK"` and pick 10.0.26100 or later. The package ID isn't confirmed here; check it.
4. **GitHub CLI:** run `gh --version` (the provenance check needs `gh attestation`, from 2.49 on), then `gh auth status`.
   - If gh is missing, it installs with `winget install GitHub.cli`.
   - If it isn't logged in, the user runs `gh auth login`.
5. **What's installed now**, for the record:
   - `Get-PnpDevice -Class Display | Format-Table Status, FriendlyName, InstanceId -AutoSize`: SudoVDA should be there.
   - `Get-Service ApolloService`.
   - Secure Boot: `Confirm-SecureBootUEFI`. Information only, don't change it.
6. ⚠ **Restore point:**
   ```powershell
   Checkpoint-Computer -Description "Before LuzVDA" -RestorePointType MODIFY_SETTINGS
   Get-ComputerRestorePoint | Select-Object -Last 1
   ```
   - It needs System Protection on for C:.
   - Windows also creates at most one restore point per 24 hours and may skip it silently, so check the list.
   - If there's no new restore point, tell the user and ask whether to go on.
7. **Optional driver trace**, useful if anything fails. Start it before installing:
   ```powershell
   logman start luzvda -p "{fdbe20c6-12e0-4cf1-907d-e0b99b08b5ce}" -o C:\LuzVDA\logs\luzvda.etl -ets
   ```
   Stop it at the end with `logman stop luzvda -ets`. Decode it with `tracerpt C:\LuzVDA\logs\luzvda.etl -o C:\LuzVDA\logs\luzvda.xml -of XML`; that should read TraceLogging events, but if it doesn't, keep the `.etl`.

## Part 1: Download the build

1. Find the newest green run on the branch:
   ```bash
   gh run list -R anyosag/LuzVDA -b claude/luzvda-foundation -w Build --status success -L 1 --json databaseId,headSha,displayTitle,createdAt
   ```
   At the time of writing that was run 7, artifact `LuzVDA-0.0.0.7-ci`. Run 6, `LuzVDA-0.0.0.6-ci`, has the same driver code; run 7 added only docs. The version is `0.0.0.<run number>`; call it `<ver>` below.
2. Download it:
   ```bash
   gh run download <run id> -R anyosag/LuzVDA -n LuzVDA-<ver>-ci -D C:/LuzVDA/ci
   ```
   You should get `C:\LuzVDA\ci\LuzVDA-<ver>-unsigned.zip` and `SHA256SUMS.txt`. If gh left a zip of the artifact instead, extract it there.
3. Extract `LuzVDA-<ver>-unsigned.zip` to `C:\LuzVDA\unsigned`. It holds `LuzVDA.inf`, `LuzVDA.dll`, `luzvda.cat`, `luzvda-ctl.exe`, `luzvda-ioctl.h`, `LICENSE`, `NOTICE.md`, `new-signing-cert.ps1` and `sign-package.ps1`.
4. Read both `.ps1` files before running them. They're short; tell the user in two lines what each does.

## Part 2: Sign

⚠ Ask first. `new-signing-cert.ps1` creates a non-exportable code-signing key in `LocalMachine\My` and adds its public certificate to `LocalMachine\Root` and `LocalMachine\TrustedPublisher`. From then on, this PC trusts anything signed with that key.

```powershell
cd C:\LuzVDA
Set-ExecutionPolicy -Scope Process Bypass
.\unsigned\new-signing-cert.ps1                                  # once per PC
.\unsigned\sign-package.ps1 -Zip .\ci\LuzVDA-<ver>-unsigned.zip
```

- **`new-signing-cert.ps1`** prints the subject (`CN=LuzVDA driver signing (<computer name>)`) and the thumbprint, and writes `C:\LuzVDA\unsigned\luzvda.cer`.
- **`sign-package.ps1`:**
  1. checks the zip against `SHA256SUMS.txt`;
  2. runs `gh attestation verify` against `anyosag/LuzVDA`;
  3. signs `LuzVDA.dll`, `luzvda-ctl.exe` and `luzvda.cat`, with a timestamp from DigiCert's server;
  4. checks that the catalog covers the DLL;
  5. writes `C:\LuzVDA\ci\LuzVDA-<ver>.zip` and `LuzVDA-<ver>.zip.sha256`, with `luzvda.cer` inside the zip.
- **If it fails:**
  - On the timestamp server: stop and report the error; don't drop the timestamp.
  - On the attestation: report the error, and ask the user whether to retry, or to use `-SkipAttestation`, which leaves only the hash check.
- **Then verify the signed files:**
  ```powershell
  Expand-Archive C:\LuzVDA\ci\LuzVDA-<ver>.zip -DestinationPath C:\LuzVDA\signed -Force
  Get-AuthenticodeSignature C:\LuzVDA\signed\LuzVDA.dll, C:\LuzVDA\signed\luzvda.cat, C:\LuzVDA\signed\luzvda-ctl.exe | Format-Table Status, StatusMessage, Path -AutoSize
  ```
  Each should be `Valid`. Log it all to `02-sign.txt`.

## Part 3: Install

⚠ Ask first. This creates a device `Root\LuzVDA` and installs the driver package. It doesn't touch SudoVDA.

Run in `C:\LuzVDA\signed`:

```bat
luzvda-ctl.exe install LuzVDA.inf
luzvda-ctl.exe version
luzvda-ctl.exe set-adapter auto
luzvda-ctl.exe list
```

**Expected:**
- `install` prints `Created the LuzVDA device`, then `Installed the LuzVDA driver`. If it adds `restart Windows to finish`, ask the user before restarting.
- `version` prints `LuzVDA 0.0.0.<run>, protocol 1.0; this tool speaks 1.0`, then `Watchdog: 3 s`.
- `set-adapter auto` prints `Rendering on NVIDIA GeForce …` (the PC's GPU).
- `list` prints `0 display(s)`.

**Device check:**

```powershell
Get-PnpDevice | Where-Object { $_.HardwareID -contains 'Root\LuzVDA' } | Format-List Status, Problem, FriendlyName, InstanceId
```

Status `OK` and no problem code. Note the InstanceId; T5 needs it.

**If the install fails, or the device shows a problem:**
- Save the LuzVDA section from the end of `C:\Windows\INF\setupapi.dev.log`. Search for `LuzVDA`; take from the last `>>>  [Device Install` section that mentions it to its `<<<`.
- Save the System log since you started:
  ```powershell
  Get-WinEvent -FilterHashtable @{LogName='System'; StartTime=(Get-Date).AddHours(-1)} | Where-Object { $_.LevelDisplayName -in 'Error','Warning' } | Format-List TimeCreated, ProviderName, Id, Message
  ```
- **Code 52** means Windows doesn't trust the signature. Check that the thumbprint is in `Cert:\LocalMachine\Root` and `Cert:\LocalMachine\TrustedPublisher`, and report. Don't change signing settings (rule 2).
- **Code 10 or 31** means the driver didn't start. Stop the trace (Part 0, step 7) and keep it.
- Then stop and report.

**Access check (the user does this):** ask them to open a **normal, non-elevated** Command Prompt and run `C:\LuzVDA\signed\luzvda-ctl.exe ping`. It should fail with an access-denied error (5). Record what it printed.

## Part 4: Tests

Each display stays only while something asks the driver for something. Its watchdog removes every display 3 seconds after the last request. `luzvda-ctl add` normally holds the display until Enter is pressed, which doesn't work without a console. Instead:

1. Start a **keepalive in the background** before adding a display, for example in Git Bash:
   ```bash
   while true; do /c/LuzVDA/signed/luzvda-ctl.exe ping >/dev/null 2>&1; sleep 1; done
   ```
2. Add each display with `--no-hold`.
3. Remove displays with `luzvda-ctl remove <guid>`. Stop the keepalive when a test is done.

The test GUIDs:
- `6a0b9c1e-3f4d-4c2a-9e7b-1d2c3b4a5f60`: "MacBook Pro"
- `1b2c3d4e-0000-4000-8000-000000000001`: "SDR test"
- `1b2c3d4e-0000-4000-8000-000000000002`: long name

### T1: A MacBook Pro display, and HDR (`10-macbook.txt`)

What it decides: does Windows read the brightness and colors the display reports? Which GPU renders it?

```bat
luzvda-ctl add 6a0b9c1e-3f4d-4c2a-9e7b-1d2c3b4a5f60 --name "MacBook Pro" --mode 3024x1964@120 --mode 3024x1890@120 --mode 3024x1964@60 --size 302x196 --p3 --hdr 1600,1000,0.005 --no-hold
luzvda-ctl diag
luzvda-ctl hdr 6a0b9c1e-3f4d-4c2a-9e7b-1d2c3b4a5f60 on
luzvda-ctl diag
```

**Expected:**
- `add` prints `Added: adapter …, target N`.
- `diag` lists the display:
  - with a GDI name like `\\.\DISPLAY5`, at `3024x1964 at 120.000 Hz`;
  - `monitor name: "MacBook Pro"`;
  - `rendered by: NVIDIA GeForce …` (the PC's GPU).
- After `hdr … on`, `advanced color: supported 1, enabled 1`. Then the DXGI lines:
  - color space 12 (PQ / BT.2020), 10 bits;
  - luminance about **min 0.005, max 1600, max full frame 994**: the EDID stores 1000 as the nearest code, 994;
  - primaries about **red 0.680,0.320, green 0.265,0.690, blue 0.150,0.060, white 0.313,0.329**: Display P3.

If the luminance or the primaries are something else (Windows' defaults are often 1499/799, or values matching sRGB), record exactly what it shows. That's the key finding of this test.

**Also ask the user:** in Settings → System → Display, select the "MacBook Pro" display. Is "Use HDR" on, and is the resolution 3024 × 1964?

### T2: Changing modes (`11-modes.txt`)

What it decides: can a resume change the resolution without removing and re-adding the display? Run this with the display from T1 still there.

```bat
luzvda-ctl set-mode 6a0b9c1e-3f4d-4c2a-9e7b-1d2c3b4a5f60 3024x1890@120
luzvda-ctl update-modes 6a0b9c1e-3f4d-4c2a-9e7b-1d2c3b4a5f60 --mode 2560x1600@120 --mode 3024x1964@120
luzvda-ctl diag
luzvda-ctl set-mode 6a0b9c1e-3f4d-4c2a-9e7b-1d2c3b4a5f60 2560x1600@120
luzvda-ctl diag
```

**Expected:**
- The first `set-mode` succeeds (`SetDisplayConfig: 0 (success)`, `Now 3024x1890 at 120.000 Hz`). That mode was in the display's list when it arrived.
- The second `set-mode` asks for a mode that wasn't. The guess is that Windows refuses it (a non-zero code, or `Now` unchanged), but this test exists to find out. Record exactly what happens, including any mode Windows switched to on its own after `update-modes`.

### T3: What Windows remembers a display by (`12-saved-settings.txt`)

What it decides: is brightness part of how Windows recognizes a display? If it is, a different brightness makes Windows forget that display's settings.

1. Record `reg query "HKCU\Control Panel\Desktop\PerMonitorSettings" /s`. The format of this key is undocumented, so treat it as supporting evidence only.
2. Ask the user to set the "MacBook Pro" display's **scale to 175%** in Settings → Display, then record the key again.
3. Remove the display, wait 5 seconds, and add it again, with the same GUID and a different brightness:
   ```bat
   luzvda-ctl remove 6a0b9c1e-3f4d-4c2a-9e7b-1d2c3b4a5f60
   luzvda-ctl add 6a0b9c1e-3f4d-4c2a-9e7b-1d2c3b4a5f60 --name "MacBook Pro" --mode 3024x1964@120 --size 302x196 --p3 --hdr 1000,600,0.05 --no-hold
   ```
4. **Ask the user:** is the scale still 175%? Record the registry key a third time.
5. Remove the display.

### T4: A display without HDR (`13-sdr.txt`)

What it decides: does Windows keep HDR off a display whose description has no HDR? Every connector can carry HDR, so only the display's EDID says it can't.

```bat
luzvda-ctl add 1b2c3d4e-0000-4000-8000-000000000001 --name "SDR test" --mode 1920x1080@60 --no-hold
luzvda-ctl diag
```

**Expected:** `advanced color: supported 0`. **Ask the user** whether "Use HDR" appears for "SDR test" in Settings. Then remove the display.

### T5: Long names (`14-names.txt`)

What it decides: how much of a display's name Windows shows. The standard EDID name field holds 13 characters.

1. Add a display with a long name and look at it:
   ```bat
   luzvda-ctl add 1b2c3d4e-0000-4000-8000-000000000002 --name "Anyo's MacBook Pro 14-inch" --mode 1920x1080@60 --no-hold
   luzvda-ctl diag
   ```
   Record the `monitor name` line and its character count. **Ask the user** what name Settings → Display shows. Then remove the display.
2. ⚠ Turn on the experiment, splitting the name over two EDID fields:
   ```bat
   reg add HKLM\SOFTWARE\LuzVDA /v edidSplitName /t REG_DWORD /d 1 /f
   ```
   Then restart the device (the InstanceId is from Part 3):
   ```powershell
   Disable-PnpDevice -InstanceId '<InstanceId>' -Confirm:$false; Start-Sleep 3; Enable-PnpDevice -InstanceId '<InstanceId>' -Confirm:$false
   ```
   Run `luzvda-ctl set-adapter auto` again, since the device lost the setting when it restarted.
3. Repeat step 1 and record the same things.
4. ⚠ Turn the experiment off again:
   ```bat
   reg delete HKLM\SOFTWARE\LuzVDA /v edidSplitName /f
   ```
   Restart the device the same way.

### T6: The watchdog, and adding and removing repeatedly (`15-robustness.txt`)

What it decides: whether the driver cleans up after a crashed caller, and whether its cleanup fixes hold up.

1. With the keepalive running, add the "SDR test" display. **Stop the keepalive**, wait 5 seconds, then run `luzvda-ctl list`. It should show `0 display(s)`: the watchdog removed it.
2. Restart the keepalive. Add and remove the "SDR test" display **10 times in a row**, then run `luzvda-ctl list`. Every `add` and `remove` should succeed, and the list should end empty.
3. Look for errors in the System log during the tests, using the query from Part 3. Also check that `WUDFHost.exe` is still running and how much memory it uses:
   ```powershell
   Get-Process WUDFHost | Format-Table Id, WorkingSet64, StartTime
   ```

### After the tests

- Remove any display left, stop the keepalive, and confirm `luzvda-ctl list` shows `0 display(s)`.
- Stop the trace, if you started one.
- **Leave LuzVDA installed**, unless the user wants it gone.

## Part 5: Report

1. Write `C:\LuzVDA\results.md`. For each part and test, give pass, fail or "finding", the key lines quoted exactly, and the user's answers to the visual questions. Include:
   - the Windows build, the run number and `<ver>`, the certificate's thumbprint and subject, and the SHA256 of `LuzVDA-<ver>.zip`;
   - anything that surprised you.
2. Show the user the file. It contains the computer's name, its GPU and monitor names, and the certificate's thumbprint, none of which is secret.
3. ⚠ **Ask how to hand it back:**
   - **(a)** The user pastes it into the cloud Claude session.
   - **(b)** Push it to a new branch in LuzVDA, which is a **public** repository, so the cloud session can read it. Redact the computer name first if the user wants. Don't push to `claude/luzvda-foundation`: the cloud session works on that branch.
     ```bash
     git clone --depth 1 -b claude/luzvda-foundation https://github.com/anyosag/LuzVDA C:/LuzVDA/src
     cd C:/LuzVDA/src && git switch -c test-results/first-test
     mkdir -p results && cp C:/LuzVDA/results.md results/first-test-<ver>.md
     git add results && git commit -m "First test of LuzVDA <ver> on the PC"
     git push -u origin test-results/first-test
     ```

## Part 6: Publish the signed build (only if Part 3 and T1 passed)

⚠ This is public and needs the user's yes. Apollo's installer will download and bundle this signed zip, and Apollo's CI checks that it's signed by the certificate inside it.

```bash
gh run view <run id> -R anyosag/LuzVDA --json headSha -q .headSha        # the commit CI built
gh release create signed-<ver> C:/LuzVDA/ci/LuzVDA-<ver>.zip C:/LuzVDA/ci/LuzVDA-<ver>.zip.sha256 \
  -R anyosag/LuzVDA --target <headSha> --prerelease \
  --title "LuzVDA <ver> (signed)" \
  --notes "Signed on the owner's PC from CI run <run id> (provenance verified). Certificate thumbprint <thumbprint>. First-test results: <one line>."
```

- The tag starts with `signed-`, not `v`, so it doesn't start a CI build.
- Report the release URL and the zip's SHA256. The cloud session needs both for Apollo's packaging.

## Part 7: Apollo with LuzVDA (optional; only if the user asks)

The Apollo branch `claude/luzvda` uses LuzVDA. Its installer still installs SudoVDA too, which is harmless.

With LuzVDA already installed from Part 3, an install of that branch's build would use it, and could test the whole chain from the Mac.

**The trade-off:** it replaces the Apollo the user streams with today. Going back means reinstalling a build from `master`.

⚠ Only if the user wants this now:

1. Find the newest green run of "Build Windows" on `claude/luzvda`:
   ```bash
   gh run list -R anyosag/Apollo -b claude/luzvda -w "Build Windows" --status success -L 1
   ```
   If there isn't one, stop.
2. Download its `Apollo-windows-<version>` artifact and check it:
   ```bash
   gh attestation verify Apollo-windows-installer.exe -R anyosag/Apollo
   ```
3. Back up `C:\Program Files\Apollo\config`.
4. Install as administrator.
5. The user streams from the Mac with LuzLunar: 5 launches and 5 resumes, at least one resume switching between the full screen and the below-notch resolution.
6. Collect the lines with `vdisplay:` or `streams to` from Apollo's log in `C:\Program Files\Apollo\config`. Run `Get-ChildItem *.log` there to find it.
7. Also note what the user sees: whether HDR is on and looks right, how much the screen flashes at launch, and the display's name.
8. Add all of it to `results.md`.

## Undoing everything

- **Driver:** `luzvda-ctl uninstall` removes the device and its driver packages.
- **Settings:** `reg delete HKLM\SOFTWARE\LuzVDA /f`, if the key exists.
- **Certificate:** only if the user asks.
  1. Remove its thumbprint from `Cert:\LocalMachine\Root` and `Cert:\LocalMachine\TrustedPublisher`.
  2. The key itself is in `Cert:\LocalMachine\My`. Removing it means future builds need a new key, and a new certificate in Apollo.
- **Whole system:** the restore point from Part 0.

## Reference

- In the repo: `README.md` covers the driver, its settings and the protocol. `docs/first-test.md` is the same tests, written for a person.
- Trace provider: `LuzVDA`, `{fdbe20c6-12e0-4cf1-907d-e0b99b08b5ce}`.
- Driver settings under `HKLM\SOFTWARE\LuzVDA`, read when the device starts:

  | Value | Meaning |
  |---|---|
  | `watchdog` | seconds; 0 turns it off |
  | `maxMonitors` | how many displays at once |
  | `gpuName` | the GPU that renders the displays |
  | `sdrBits`, `hdrBits` | bits per channel |
  | `edidSplitName` | the long-name experiment |

- `luzvda-ctl` with no arguments prints its usage.
