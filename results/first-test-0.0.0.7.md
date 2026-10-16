# LuzVDA first test: 0.0.0.7 on a real PC

The computer's name is redacted as `<computer>`, including in the certificate's subject. The thumbprint is unchanged.

Run by Claude Code on the PC (elevated), following `handoff.md`; the owner answered the visual checks. 2026-10-16.

## The setup

| | |
|---|---|
| Windows | 11 Pro N, build **26200** |
| GPU | **NVIDIA GeForce RTX 5080** (driver 617.42). The handoff assumed an RTX 4080. |
| Secure Boot | **Off** (`Confirm-SecureBootUEFI`: `False`); left as it was |
| CI run | **7** (id 38931149350), commit `75f35c6a3e3505be29e2712af4d4c5b5384f736c`, artifact `LuzVDA-0.0.0.7-ci` |
| `<ver>` | **0.0.0.7** |
| Certificate | `CN=LuzVDA driver signing (<computer>)`, thumbprint **`F9B9A1D427D86DAE49CA012F7B73C45B84107215`**, valid to 2031-10-16 |
| Signed zip | `LuzVDA-0.0.0.7.zip`, SHA256 **`42a928ac906f9697383b2d970f771ad141396189399b6ba9d0a96ba097b75844`** |
| Alongside | SudoVDA 1.10.9.289 (`ROOT\DISPLAY\0000`), used by Apollo 0.4.8.32 (SudoMaker's build); untouched throughout |
| LuzVDA device | `ROOT\DISPLAY\0001`, installed as `oem19.inf` |

## Results

| Part / test | Result |
|---|---|
| 0. Preflight | Pass. Restore point #8 "Before LuzVDA" created. |
| 1. Download | Pass. The unsigned zip matches `SHA256SUMS.txt`. |
| 2. Sign | Pass. Provenance verified; all three signatures `Valid`. |
| 3. Install | **Pass, with findings:** it asked for a restart; Windows tried to remove the SudoVDA device; a non-admin open reports error 0, not 5. |
| T1. MacBook Pro, HDR | **Pass.** Windows reads the reported brightness and P3 primaries; HDR on at 10 bits. |
| T2. Changing modes | **Finding:** Windows *accepted* a mode added by `update-modes`. |
| T3. Saved settings | **Pass:** brightness isn't part of how Windows recognizes the display. |
| T4. SDR display | **Pass, with a finding:** no HDR toggle, but advanced color reports `supported 1`. |
| T5. Long names | **Finding:** 13 characters by default; the full 26 with `edidSplitName`. |
| T6. Watchdog, repeats | **Pass.** |

## Surprises

1. **The install asked for a restart, and touched SudoVDA.** `install` printed `Installed the LuzVDA driver; restart Windows to finish`. At the same moment the System log recorded Kernel-PnP 225: *"The application \Device\HarddiskVolume6\Program Files\Apollo\sunshine.exe with process id 1772 stopped the removal or ejection for the device ROOT\DISPLAY\0000"*, which is the **SudoVDA** device. The LuzVDA device was started and working, but flagged as needing a restart (`DEVPKEY_Device_IsRebootRequired` `True`, DevNodeStatus `0x0180210B`, with DN_NEED_RESTART). SudoVDA kept its own driver and wasn't restarted. After a Windows restart the flag was gone (DevNodeStatus `0x0180200B`) and everything behaved normally.
   - Unconfirmed guess: both drivers use the `WUDFRd` service and the `IndirectKmd` upper filter, so installing one makes PnP try to restart the other, and an app holding SudoVDA open vetoes it. Apollo always holds it, so this will likely happen on every install next to SudoVDA.
2. **A refused open reports error 0.** The device's security is right (`DEVPKEY_Device_SecuritySDS`: `D:P(A;;GA;;;SY)(A;;GA;;;BA)`), and a non-admin `luzvda-ctl ping` was refused. But it printed `Opening LuzVDA (installed? elevated prompt?) failed: 0 The operation completed successfully.` instead of error 5. It looks like the error code is read after another call reset it.
   - This was before the restart. The owner didn't get to repeat it after the restart.
   - A second try came from a window that turned out to be elevated (its token was checked), so its silent success doesn't count.
3. **T2: Windows accepts modes added later.** After `update-modes â€¦ --mode 2560x1600@120 --mode 3024x1964@120`, `set-mode â€¦ 2560x1600@120` returned `SetDisplayConfig: 0 (success)` / `Now 2560x1600 at 120.000 Hz`. Also:
   - Windows didn't switch modes by itself after `update-modes`. The display stayed at 3024x1890, which wasn't in the new list.
   - The trace logged `ModesUpdated Count=3` while the tool printed `Updated: 2 mode(s)`. That may mean the driver keeps the current mode, which would explain why Windows stayed at 3024x1890.
4. **T4: `supported 1` on a display without HDR.** Windows offers no "Use HDR", but the advanced-color query reports `supported 1, enabled 0, wide color enforced 1`. That's probably Windows' automatic color management for non-HDR displays (unconfirmed).
5. **An unasked-for scale entry.** While "SDR test" was present, a new PerMonitorSettings key `LZV4C56LV1CA83E36_01_07EA_25^F81588â€¦` appeared with `DpiValue 0x3`. Nobody changed that display's scale.

## Details

### Part 0â€“2
- Build 26200; elevated; signtool `C:\Program Files (x86)\Windows Kits\10\bin\10.0.26100.0\x64\signtool.exe`; gh 2.102.0, logged in.
- `gh attestation verify`: 1 attestation, `https://slsa.dev/provenance/v1`, digest `sha256:dcc371fc312943f616f95ec5015849a74c1a76fbf89bb5f923bf2c347b896059`, built by `.github/workflows/build.yml@refs/heads/claude/luzvda-foundation`, run `38931149350/attempts/1`.
- Signatures: `LuzVDA.dll`, `luzvda.cat`, `luzvda-ctl.exe` are all `Valid`, signed by `CN=LuzVDA driver signing (<computer>)`, timestamped by `DigiCert SHA256 RSA4096 Timestamp Responder 2026 1`.

### Part 3
```
Created the LuzVDA device
Installed the LuzVDA driver; restart Windows to finish
LuzVDA 0.0.0.7, protocol 1.0; this tool speaks 1.0
Watchdog: 3 s (3 left)
Rendering on NVIDIA GeForce RTX 5080
0 display(s)
```
Device: `Status OK`, `CM_PROB_NONE`, `LuzVDA Virtual Display Adapter`, `ROOT\DISPLAY\0001`. The same outputs after the restart, with no reboot flag.

### T1 (`10-macbook.txt`)
```
Added: adapter 00000000:0001bf09, target 256
\\.\DISPLAY15: 3024x1964 at 120.000 Hz, position 4480,0
monitor name: "MacBook Pro" (11 characters)
rendered by: NVIDIA GeForce RTX 5080 (15979 MB dedicated)
-- after hdr on: HDR on: 0 (success)
advanced color: supported 1, enabled 1, wide color enforced 0, 10 bits per channel, encoding 0
SDR white level: 240 nits
DXGI: color space 12, 10 bits per color
DXGI: luminance min 0.0061, max 1600.0, max full frame 993.5 nits
DXGI: red 0.6797,0.3203 green 0.2646,0.6904 blue 0.1504,0.0596 white 0.3125,0.3291
```
- Min 0.0061 instead of 0.005 is the nearest value the CTA-861.3 min-luminance code can hold with max 1600: code 5 gives 1600Ã—(5/255)Â²/100 = 0.00615. Max 1600 (code 160) and max full frame 993.5 (code 138) are exact.
- Before HDR was on, the SDR white level was 80 nits.
- **Owner:** "Use HDR" is on, and the resolution is 3024 Ã— 1964.

### T2 (`11-modes.txt`)
```
set-mode 3024x1890@120  -> SetDisplayConfig: 0 (success) / Now 3024x1890 at 120.000 Hz
update-modes            -> Updated: 2 mode(s)
diag                    -> \\.\DISPLAY15: 3024x1890 at 120.000 Hz (preferred 3024x1964 at 120.000 Hz)
set-mode 2560x1600@120  -> SetDisplayConfig: 0 (success) / Now 2560x1600 at 120.000 Hz
```
HDR stayed on (color space 12, 10 bits) through every change.

### T3 (`12-saved-settings.txt`)
- Before T3, I set the display back to 3024x1964@120, the mode the re-add uses, so scale wasn't compared across resolutions.
- Before the scale change: only `SAM7489HNTY607772_19_07E9_72^B3C9â€¦` (`DpiValue 0x0`).
- After the owner set 175%: a new key `LZV4C56LV2642820C_01_07EA_1C^265567BDB645412D2E448C5BC979DC13`, `DpiValue 0xffffffff`.
- After remove, a 5 s wait, and a re-add with `--hdr 1000,600,0.05`:
  - the same key and value, with no new key for it;
  - luminance `min 0.0494, max 993.5, max full frame 603.7`;
  - HDR came back already on;
  - effective DPI 168 (175%), from `GetDpiForMonitor`.
- **Owner:** the scale is still 175%.

### T4 (`13-sdr.txt`)
```
\\.\DISPLAY16: 1920x1080 at 60.000 Hz
monitor name: "SDR test" (8 characters)
advanced color: supported 1, enabled 0, wide color enforced 1, 8 bits per channel, encoding 0
DXGI: color space 0, 8 bits per color
DXGI: luminance min 0.5000, max 270.0, max full frame 270.0 nits
DXGI: red 0.6396,0.3301 green 0.2998,0.5996 blue 0.1504,0.0596 white 0.3125,0.3291
```
- The "MacBook Pro" display was still present during T4, so both could be checked in one visit to Settings.
- **Owner:** no "Use HDR" toggle for "SDR test".

### T5 (`14-names.txt`)
- **Experiment off:** `monitor name: "Anyo's MacBoo" (13 characters)`.
  - The owner looked in the display arrangement, which shows only numbers, and didn't read this one from Advanced display.
- **`edidSplitName` = 1, then a device restart** (disable / enable; the device came back `OK`; `set-adapter auto` again):
  - `monitor name: "Anyo's MacBook Pro 14-inch" (26 characters)`.
  - **Owner:** Settings â†’ Display â†’ Advanced display shows **"Anyo's MacBook Pro 14-inch"**.
- The value was deleted afterwards and the device restarted. It came back `OK` with `0 display(s)`.
- The adapter LUID changes with each device restart: `0001bf09` â†’ `00c33100` â†’ `00e3d449`.

### T6 (`15-robustness.txt`)
- **Watchdog:** the keepalive stopped at 15:35:49. At 15:35:54, `list` printed `0 display(s)`.
- **Repeats:** 10 Ã— add and remove of "SDR test" all exited 0, and `list` ended at `0 display(s)`. The trace has a `DisplayRemoved Status=0x0` for each one.
- **Logs:** no System errors or warnings, and no Application errors about WUDFHost, LuzVDA or dwm, from 15:21 (the start of the tests) to the end.
- **Host process:** LuzVDA's `WUDFHost.exe` was running at the end, with a 27 MB working set. It was started at the last device restart (15:35:42).
- **Trace** (`luzvda-tests.etl/.xml`): 39 LuzVDA events. Every `Status` is `0x0`.

## For context
- System log errors that predate LuzVDA, and recur at boot:
  - Kernel-PnP 219 `\Driver\WUDFRd failed to load. Device: ROOT\LIBVIRTUALHID\0000` (Sunshine's virtual HID)
  - TPM-WMI 1040
  - `AMDRyzenMasterDriverV28` service 7000
  - DCOM 10016
  - and, earlier, ApolloService 7023 and SOLIDWORKS DCOM 10000
- Test-harness notes:
  - Claude's shell sets `NoDefaultCurrentDirectoryInExePath=1`, so `luzvda-ctl.exe` must be called by full path. The first two install attempts never reached `luzvda-ctl` because of this; their logs are kept as `03-install-attempt1/2-*.txt`.
  - In T1, the `timeout /t 3` waits didn't run, because there was no console. The display was up anyway.
- Logs are in `C:\LuzVDA\logs`:
  - `00`â€“`04`, `10`â€“`15`
  - `03b-install-diagnostics.txt`
  - `keepalive.txt`
  - the traces `luzvda-install.*` and `luzvda-tests.*`
- LuzVDA is still installed, with 0 displays. The test registry value has been removed.
