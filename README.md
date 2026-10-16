# LuzVDA

A virtual display driver for [Apollo](https://github.com/anyosag/Apollo) and [LuzLunar](https://github.com/anyosag/LuzLunar). Apollo adds a display when a client starts streaming and removes it when the stream ends. Each display describes the screen it stands in for: the client's physical size, color primaries, HDR brightness and exact modes. Windows then picks a sensible scale and tone-maps HDR for that screen, not a generic one.

It's a fork of [SudoVDA](https://github.com/SudoMaker/SudoVDA), built on Microsoft's Indirect Display Driver sample (IddCx). See [NOTICE.md](NOTICE.md) for where each part comes from and its license.

## How it differs from SudoVDA

| | SudoVDA | LuzVDA |
|---|---|---|
| Who can add or remove displays | Any program (the device was open to Everyone) | SYSTEM and Administrators |
| What a display reports | One fixed EDID: 70×39 cm, about 3,800 nits, zero color primaries; only the name and serial change | An EDID built from the caller's description, checked with `edid-decode` |
| Modes | 47 built-in modes plus scaled copies of the requested one | Exactly the caller's modes (up to 32), plus 1920×1080 at 60 Hz |
| HDR | Offered on every display | Only on displays described as HDR |
| Changing modes | Not possible without re-adding the display | `UPDATE_MODES` replaces the target modes |
| Requests | No size or string checks | Every field checked (`src/common/validate.h`, tested) |
| Signing | SudoMaker's self-signed certificate, added to Trusted Root by Apollo's installer | Yours, made on your PC by `scripts/new-signing-cert.ps1` |
| Windows | 10 and 11 | 11 24H2 (build 26100) and later only |

It also fixes SudoVDA bugs: monitor contexts, EDIDs and a named event handle leaked on every display; lists were read without the lock; a full connector list blocked re-adding an existing display; a failed arrival left a half-made display behind; an invalid render GPU could abandon swap-chains forever; the EDID serial used only part of the GUID; and the manufacturer ID SMK belongs to SMK Corporation.

## Requirements

- Windows 11 24H2 or later, x64.
- A build signed with a certificate the PC trusts (see Signing).

## Control interface

`include/luzvda/luzvda-ioctl.h` (MIT) defines protocol 1.0: get version, ping, get watchdog, add, remove, update modes, set render adapter, and list. Callers open the device interface `{15d7851f-629d-4e2a-bffd-9c5f43ddd57a}` for reading and writing.

The watchdog removes every display when no request has arrived for its timeout (3 s by default), so a crashed caller doesn't leave displays behind. Callers ping at a third of the timeout.

## Settings

Under `HKLM\SOFTWARE\LuzVDA`. They're read when the driver loads, so restart the device after changing them.

| Value | Type | Default | Meaning |
|---|---|---|---|
| `gpuName` | REG_SZ | unset | The GPU that renders the displays, by its name; unset leaves it to the caller or Windows |
| `watchdog` | DWORD | 3 | Seconds without a request before displays are removed; 0 turns it off; at most 60 |
| `maxMonitors` | DWORD | 10 | 1 to 16 |
| `sdrBits` | DWORD | 8 | 10 for 10-bit SDR only |
| `hdrBits` | DWORD | 10 | 12 for 12-bit HDR only |
| `edidSplitName` | DWORD | 0 | Experiment: spread names longer than 13 characters over two EDID name descriptors |

## Building

CI builds every push (`.github/workflows/build.yml`) and publishes an **unsigned** zip with its SHA256 and build provenance. A tag `vX.Y.Z` builds version `X.Y.Z.<run number>`.

Locally, with Visual Studio 2022 or later:

```powershell
nuget restore .\packages.config -PackagesDirectory .\packages\
msbuild LuzVDA.sln -p:Configuration=Release -p:Platform=x64
```

Tests that don't need Windows:

```sh
g++ -std=c++17 -Iinclude -Isrc tests/validate_test.cpp -o validate_test && ./validate_test
g++ -std=c++17 -Iinclude -Isrc tests/edid_builder_test.cpp -o edid_builder_test
./edid_builder_test out && for f in out/*.bin; do edid-decode --check "$f"; done
```

## Signing

Builds are signed by whoever installs them, with a key that never leaves their PC. You need the Windows SDK's signing tools (`signtool.exe`) and the GitHub CLI (`winget install GitHub.cli`, then `gh auth login`). On the PC, in an elevated PowerShell:

1. Download the `LuzVDA-<version>-ci` artifact from a CI run and extract it. It holds `LuzVDA-<version>-unsigned.zip` and `SHA256SUMS.txt`; extract the scripts from that zip too.
2. Let this PowerShell window run the scripts: `Set-ExecutionPolicy -Scope Process Bypass`.
3. Once: `.\new-signing-cert.ps1`. It creates a non-exportable code-signing key in the machine's certificate store, trusts it for drivers on this PC, and writes the public `luzvda.cer`.
4. For each release: `.\sign-package.ps1 -Zip .\LuzVDA-<version>-unsigned.zip`. It checks the hash and the build provenance (`gh attestation verify`), signs with `signtool`, and writes `LuzVDA-<version>.zip`.

Any PC that trusts your certificate will install drivers and run software you sign. Install it only on your own machines.

## Installing and trying it

Apollo's installer does this. By hand, from the signed zip, in an elevated prompt:

```bat
luzvda-ctl remove-legacy
luzvda-ctl install LuzVDA.inf
luzvda-ctl add 6a0b9c1e-3f4d-4c2a-9e7b-1d2c3b4a5f60 --name "MacBook Pro" --mode 3024x1964@120 --mode 3024x1890@120 --size 302x196 --p3 --hdr 1600,1000,0.005
```

`add` keeps the display until you press Enter. Meanwhile, in another prompt:

- `luzvda-ctl diag` shows what Windows and DXGI report for it: mode, monitor name, HDR state, luminance, primaries, and the rendering GPU.
- `luzvda-ctl set-mode <guid> 3024x1890@60` asks Windows to switch modes.
- `luzvda-ctl hdr <guid> on` switches HDR.
