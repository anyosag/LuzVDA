# First test on a PC

LuzVDA hasn't run on a real PC yet. This installs it next to SudoVDA, so the Apollo you use today keeps working, and answers the questions Apollo's next steps depend on. Paste back everything marked **Paste**.

Before you start, make a restore point (Start → "Create a restore point"). A driver that misbehaves can be removed with `luzvda-ctl uninstall`, but a restore point costs nothing.

## 1. Sign a build

You need:
- the Windows SDK's signing tools (in the SDK installer, the "Signing Tools for Desktop Apps" feature is enough);
- the GitHub CLI, signed in: `winget install GitHub.cli`, then `gh auth login`.

Then:

1. On the latest green run of [Build](https://github.com/anyosag/LuzVDA/actions/workflows/build.yml), download the `LuzVDA-<version>-ci` artifact. Extract it into a new folder, for example `C:\LuzVDA`. Inside are `LuzVDA-<version>-unsigned.zip` and `SHA256SUMS.txt`.
2. Extract `LuzVDA-<version>-unsigned.zip` into `C:\LuzVDA\unsigned`. You need its two `.ps1` scripts.
3. Open PowerShell **as administrator**:
   ```powershell
   cd C:\LuzVDA
   Set-ExecutionPolicy -Scope Process Bypass
   .\unsigned\new-signing-cert.ps1
   .\unsigned\sign-package.ps1 -Zip .\LuzVDA-<version>-unsigned.zip
   ```
   The first script runs once per PC. It makes your signing key, which can't be exported, and trusts it on this PC.

   The second checks that the zip is what CI built, signs it, and writes `LuzVDA-<version>.zip`.

   **Paste:** the output of both scripts. It holds no secrets: the thumbprint is public.
4. Extract `LuzVDA-<version>.zip` into `C:\LuzVDA\signed`. Every command below runs in an **elevated** Command Prompt in that folder.

## 2. Install

Do **not** run `remove-legacy` yet: today's Apollo still uses SudoVDA.

```bat
luzvda-ctl install LuzVDA.inf
luzvda-ctl version
luzvda-ctl set-adapter auto
```

**Paste:** all three outputs. Then:
- Device Manager → Display adapters: is "LuzVDA" there without a warning sign?
- In a Command Prompt that is **not** elevated, run `luzvda-ctl ping`. It should be refused.

## 3. A MacBook Pro display

Prompt A (it keeps the display until you press Enter):

```bat
luzvda-ctl add 6a0b9c1e-3f4d-4c2a-9e7b-1d2c3b4a5f60 --name "MacBook Pro" --mode 3024x1964@120 --mode 3024x1890@120 --mode 3024x1964@60 --size 302x196 --p3 --hdr 1600,1000,0.005
```

Prompt B:

```bat
luzvda-ctl diag
luzvda-ctl hdr 6a0b9c1e-3f4d-4c2a-9e7b-1d2c3b4a5f60 on
luzvda-ctl diag
```

**Paste:** both `diag` outputs. The second should show HDR on, about 1,600 / 1,000 nits, P3 primaries, and the RTX 4080 as the render GPU.

## 4. Changing modes

Still in prompt B, with the display from step 3:

```bat
luzvda-ctl set-mode 6a0b9c1e-3f4d-4c2a-9e7b-1d2c3b4a5f60 3024x1890@120
luzvda-ctl update-modes 6a0b9c1e-3f4d-4c2a-9e7b-1d2c3b4a5f60 --mode 2560x1600@120 --mode 3024x1964@120
luzvda-ctl set-mode 6a0b9c1e-3f4d-4c2a-9e7b-1d2c3b4a5f60 2560x1600@120
luzvda-ctl diag
```

**Paste:** all four outputs. The first mode was there when the display arrived. The second wasn't; whether Windows takes it says if a display can gain modes without being re-created.

## 5. Saved settings

1. In Windows Settings → Display, set the "MacBook Pro" display's scale to 175%.
2. Press Enter in prompt A to remove it. Add it again with a different brightness:
   ```bat
   luzvda-ctl add 6a0b9c1e-3f4d-4c2a-9e7b-1d2c3b4a5f60 --name "MacBook Pro" --mode 3024x1964@120 --size 302x196 --p3 --hdr 1000,600,0.05
   ```

**Tell me:** is the scale still 175%? That says whether brightness is part of how Windows recognizes a display. Press Enter in prompt A when done.

## 6. A display without HDR

Prompt A:

```bat
luzvda-ctl add 1b2c3d4e-0000-4000-8000-000000000001 --name "SDR test" --mode 1920x1080@60
```

**Tell me:** in Settings → Display, does "Use HDR" appear for "SDR test"? It shouldn't: its EDID doesn't declare HDR. **Paste:** `luzvda-ctl diag`. Press Enter in prompt A when done.

## 7. Long names

Prompt A:

```bat
luzvda-ctl add 1b2c3d4e-0000-4000-8000-000000000002 --name "Anyo's MacBook Pro 14-inch" --mode 1920x1080@60
```

**Paste:** `luzvda-ctl diag`, and the name Settings → Display shows. Press Enter.

Then try the experimental two-part name:
1. Set it in the registry:
   ```bat
   reg add HKLM\SOFTWARE\LuzVDA /v edidSplitName /t REG_DWORD /d 1 /f
   ```
2. Restart the device: Device Manager → LuzVDA → Disable device, then Enable device.
3. Run the same `add` again.

**Paste:** `diag` and the name Settings shows. Then remove the setting:

```bat
reg delete HKLM\SOFTWARE\LuzVDA /v edidSplitName /f
```

Disable and enable the device once more.

## If something goes wrong

- `luzvda-ctl uninstall` removes the device and its driver packages.
- The driver logs through TraceLogging (provider name LuzVDA). If the install fails, **paste** the end of `C:\Windows\INF\setupapi.dev.log`: the section that mentions `LuzVDA`.
