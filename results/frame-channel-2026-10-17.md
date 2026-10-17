# Frame channel test (LuzVDA protocol 1.1), 2026-10-17 UTC

Run on the Windows PC by the PC Claude session from `handoff-frame-channel.md` (commit `ecf12b4`), plus the Cloud session's addendum (T8, GPU, T9).
- The owner approved this round in the PC session as they left. Follow-up rounds weren't approved there, so the PC is locked at the end.
- All times are UTC. Redacted: `<computer>`. The test GUID is the handoff's.

## Versions
| | |
|---|---|
| Test driver | **LuzVDA 0.0.0.12, protocol 1.1**. CI run 38981537740 (#12), commit `df8469621e7476e4fca337b447aef13a25a18b8f` on `claude/frame-channel`. The unsigned zip matched `SHA256SUMS.txt` and BUILD READY (`f197b8acâ€¦f3cf`); the attestation passed. Signed with the existing key (thumbprint `F9B9A1D427D86DAE49CA012F7B73C45B84107215`); all 3 files `Valid`. **Not published.** |
| Installed for the test | 00:21:51â€“00:38:41, on `ROOT\DISPLAY\0001` (`OK`, no reboot flag, `DriverVersion 0.0.0.12`). Apollo was stopped throughout, except F6. |
| Apollo | 0.4.8.79 (`e76ed4e`), unchanged |
| Restored | **0.0.0.9** (`oem1.inf`), `OK`, no reboot flag. The staged 0.0.0.12 package was deleted from the driver store so Windows can't pick it later; 0.0.0.7 (`oem19.inf`) remains as before. |
| PC | Windows build 26200, RTX 5080. 3 physical/virtual outputs: the test display is `\\.\DISPLAY135` at 4480,0, extended next to the PC's two monitors. |

## Summary
- **The channel works.** It opened in every test that used it (7 Ã— `FrameChannelOpened Result=0x0`), its frames always decoded (`unreadable 0`), and the driver never dropped a frame for lack of a slot (`dropped 0` in every run, 0 `FrameTexturesFailed`).
- **The objects opened by handle,** header, event and textures alike, in T1 and in T6 (`--debug-privilege`) the same way. Duplicating the driver's handles works from an elevated process without the debug privilege; the name-across-session-0 fallback wasn't needed.
- **Latency is equal to Desktop Duplication, a hair better.**
  - Presented â†’ available, p50: channel 24.68â€“24.69 ms vs duplication 24.78â€“24.82 ms. p95 is within Â±0.4 ms.
  - On the same frames, the channel leads by about **0.14 ms at p50** (p95 âˆ’0.07 to âˆ’0.12 ms).
  - Its tail is a little longer: p99 +1.7 to +4.6 ms, max +6 to +14 ms.
- **Missed frames: the channel delivered 67â€“69% of presented frames; duplication 99.9â€“100%.**
  - This isn't loss. The test window presented **164.8 fps on a 120 Hz display**. The driver copies once per swap-chain frame: about 116/s, e.g. 3365 in 29 s for T1. 116 / 164.8 â‰ˆ 70%.
  - Desktop Duplication returned nearly every presented code (about 164/s). So **duplication saw frames the indirect display's swap chain never got**, or the window's presents outran the virtual vsync.
  - **For a fair "missed frames" number, the test window should present at or below the display's refresh,** or the metric should be counted per refresh.
- **Recovery (T3, an HDR switch every 8 s):**
  - Channel: 4 restarts, one per switch, each building a new texture generation (5 in all, alternating format 10 FP16 / 87 8-bit). Longest wait after a switch 348â€“364 ms.
  - Duplication: 12 restarts, about 3 per switch. Longest wait 280â€“288 ms.
  - So the channel restarts less but waits about 65 ms longer per HDR switch.
- **Stability (T8, 600 s, nothing else happening):** 0 restarts on both paths. Longest wait: channel 31.5 ms, duplication 25.4 ms. The driver copied 66,895 frames and dropped 0.
- **GPU cost** (nvidia-smi every 0.5 s; the test window drawing at about 165 fps is part of every test):

  | | GPU util mean (p95) | memory util mean | memory used mean |
  |---|---|---|---|
  | Baseline, no test (20 s) | 4.5% (5%) | 1.0% | 1,569 MiB |
  | T4, channel only | 9.6% (10%) | 3.0% | 1,975 MiB |
  | T5, duplication only | 8.8% (9%) | 1.6% | 1,900 MiB |
  | T8, both, 600 s | 12.3% (13%) | 3.0% | 2,019 MiB |

  The channel costs about +0.8 points of GPU, +1.4 points of memory bandwidth and about +75 MiB over duplication.
- **T9, access: PASS.** Under `runas /trustlevel:0x20000`, the token's `BUILTIN\Administrators` group is deny-only, and `luzvda-ctl version` printed `Opening LuzVDA (installed? elevated prompt?) failed: 5 Access is denied.` (exit 1). 0.0.0.12 reports the real error code; 0.0.0.7 printed `0`.
- **F6:** Apollo with the test driver logged `vdisplay: LuzVDA 0.0.0.12, protocol 1.1`, with no warning after it.
- **System log, 00:20 to 00:39:** no errors or warnings.

## RESULT table
| Test | Options | channel | duplication | presented | ch frames | dup frames | ch p50 | dup p50 | ch p95 | dup p95 | ch restarts | dup restarts | ch longest wait | dup longest wait | driver dropped |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| T1 | `--hdr on --seconds 30` | ok | ok | 4781 | 3253 (68.0%) | 4778 | 24.69 | 24.80 | 25.80 | 25.96 | 0 | 0 | 28.3 | 28.4 | 0 |
| T2 | `--hdr off --seconds 30` | ok | ok | 4781 | 3249 (68.0%) | 4778 | 24.68 | 24.80 | 25.28 | 25.43 | 0 | 0 | 27.3 | 27.8 | 0 |
| T3 | `--hdr on --seconds 40 --hdr-switch-every 8` | ok | ok | 6193 | 4141 (66.9%) | 6185 | 24.69 | 24.82 | 26.13 | 26.56 | 4 | 12 | 363.7 | 288.0 | 0 |
| T4 | `--hdr on --seconds 20 --only channel` | ok | skipped | 3132 | 2107 (67.3%) | â€“ | 24.69 | â€“ | 25.82 | â€“ | 0 | â€“ | 25.4 | â€“ | 0 |
| T5 | `--hdr on --seconds 20 --only duplication` | skipped | ok | 3132 | â€“ | 3129 | â€“ | 24.80 | â€“ | 25.41 | â€“ | 0 | â€“ | 25.7 | â€“ |
| T6 | `--hdr on --seconds 20 --debug-privilege` | ok | ok | 3132 | 2161 (69.0%) | 3129 | 24.68 | 24.81 | 25.29 | 25.46 | 0 | 0 | 26.2 | 26.4 | 0 |
| T7 | `--hdr on --seconds 30` | ok | ok | 4781 | 3256 (68.1%) | 4778 | 24.69 | 24.78 | 25.26 | 25.40 | 0 | 0 | 31.8 | 25.6 | 0 |
| T8 | `--hdr on --seconds 600` | ok | ok | 98733 | 66785 (67.6%) | 98730 | 24.69 | 24.80 | 25.44 | 25.43 | 0 | 0 | 31.5 | 25.4 | 0 |

Latencies are in ms (presented â†’ available). All tests used `capture-test 6a0b9c1e-3f4d-4c2a-9e7b-1d2c3b4a5f60 --add`. After every test, `luzvda-ctl list` showed 0 displays. None hung; T8 exited 0. The runner logged blank exit codes for T1â€“T7 (a PowerShell 5.1 quirk), so each test's `RESULT` line is the outcome.

## How the channel's objects opened
- **T1:** `Frame channel: header and event opened by handle, textures by handle`
- **T6** (`--debug-privilege`): `Frame channel: header and event opened by handle, textures by handle`. The same as T1.
- Desktop Duplication used `DuplicateOutput1 (FP16 or 8-bit)` in every test.

## Trace (F7), driver provider `LuzVDA`, decoded with tracerpt
- **125 events:**
  - `SwapChainAssigned` 30, `SwapChainEnded` 30, `SwapChainUnassigned` 23;
  - `FrameTexturesMade` 11;
  - `DisplayAdded` 8, `DisplayRemoved` 8, `RenderAdapterSet` 8;
  - `FrameChannelOpened` 7;
  - `FrameTexturesFailed` 0.
- The channel events:

```
00:22:51.565Z FrameChannelOpened Connector=0 Result=0x0              (T1)
00:22:51.575Z FrameTexturesMade Generation=1 Width=3024 Height=1964 Format=10 Names=0x107
00:23:30.198Z FrameChannelOpened Connector=0 Result=0x0              (T2)
00:23:30.199Z FrameTexturesMade Generation=1 Width=3024 Height=1964 Format=87 Names=0x107
00:24:08.758Z FrameChannelOpened Connector=0 Result=0x0              (T3)
00:24:08.758Z FrameTexturesMade Generation=1 Width=3024 Height=1964 Format=10 Names=0x107
00:24:17.134Z FrameTexturesMade Generation=2 Width=3024 Height=1964 Format=87 Names=0x107
00:24:25.140Z FrameTexturesMade Generation=3 Width=3024 Height=1964 Format=10 Names=0x107
00:24:33.150Z FrameTexturesMade Generation=4 Width=3024 Height=1964 Format=87 Names=0x107
00:24:41.144Z FrameTexturesMade Generation=5 Width=3024 Height=1964 Format=10 Names=0x107
00:24:56.976Z FrameChannelOpened Connector=0 Result=0x0              (T4)
00:24:56.982Z FrameTexturesMade Generation=1 Width=3024 Height=1964 Format=10 Names=0x107
00:25:53.347Z FrameChannelOpened Connector=0 Result=0x0              (T6)
00:25:53.348Z FrameTexturesMade Generation=1 Width=3024 Height=1964 Format=10 Names=0x107
00:26:21.569Z FrameChannelOpened Connector=0 Result=0x0              (T7)
00:26:21.578Z FrameTexturesMade Generation=1 Width=3024 Height=1964 Format=10 Names=0x107
00:27:25.961Z FrameChannelOpened Connector=0 Result=0x0              (T8)
00:27:25.969Z FrameTexturesMade Generation=1 Width=3024 Height=1964 Format=10 Names=0x107
```

The full event list, including the SwapChain events, is in `C:\LuzVDA\fc\logs\21-trace-utc.txt` on the PC; it's appended below.

## F6 and F8
- **F6:** `vdisplay: LuzVDA 0.0.0.12, protocol 1.1` and `vdisplay: watchdog timeout 3 s, countdown 3`, with no warning after them.
- **F8:**
  - `ROOT\DISPLAY\0001` `OK`, `DriverVersion 0.0.0.9`, `IsRebootRequired False`;
  - `luzvda-ctl version`: `LuzVDA 0.0.0.9, protocol 1.0`;
  - ApolloService running (0.4.8.79), with `vdisplay: LuzVDA 0.0.0.9, protocol 1.0`.

## Unexpected
1. **The window presents faster than the display refreshes** (164.8 fps on 120 Hz), so "missed frames" mostly measures that. Duplication delivering about 164 distinct frames per second from a 120 Hz indirect display is itself surprising: it suggests DWM composes the virtual output faster than the swap chain the driver sees. Either way, cap the window at the refresh rate for the next run.
2. **The channel recovers more slowly from an HDR switch** (about 350 ms vs 285 ms), even with fewer restarts.
3. **The channel's tail is longer.** p99 is about 1.7â€“4.6 ms behind duplication on the same frames; T8's max was 14.4 ms.
4. **A reboot was already pending** before the test, from the separate cleanup session's file deletions (`PendingFileRenameOperations`). It isn't related to LuzVDA, and the LuzVDA device never asked for a restart.
5. **Runner mistakes, on my side:** my first two runner scripts had syntax errors and never started; no test ran until the third. Exit codes were logged blank (see above).

## Full capture-test output

### T1

```
## T1: luzvda-ctl capture-test 6a0b9c1e-3f4d-4c2a-9e7b-1d2c3b4a5f60 --add --hdr on --seconds 30
## start UTC 00:22:48.039
LuzVDA 0.0.0.12, protocol 1.1
Rendering on NVIDIA GeForce RTX 5080
HDR on before measuring: ok
Display \\.\DISPLAY135: 3024x1964 at 4480,0, 120.000 Hz, HDR on

The window presented 4781 frames in 29.0 s after a 1 s warm-up (164.8 per second)

Frame channel: header and event opened by handle, textures by handle
  frames taken 3357, codes read 3253 after the warm-up (68.0% of the 4781 presented), unreadable 0
  presented -> available: p50 24.69, p95 25.80, p99 27.46, max 30.65 ms
  presented -> pixels on the CPU: p50 25.10, p95 26.23, p99 27.75, max 31.14 ms
  presented -> driver has it: p50 24.64, p95 25.75, p99 27.39, max 30.60 ms
  driver queues the copy: p50 0.02, p95 0.06, p99 0.08, max 0.14 ms
  copy queued -> consumer wakes: p50 0.02, p95 0.07, p99 0.07, max 0.27 ms
  longest wait for a new frame: 28.3 ms
  started over 0 times
  the driver copied 3365, dropped 0 (no free slot); generation 1, 3024x1964, DXGI format 10; last error 0x00000000

Desktop Duplication: DuplicateOutput1 (FP16 or 8-bit)
  frames taken 4944, codes read 4778 after the warm-up (99.9% of the 4781 presented), unreadable 0
  presented -> available: p50 24.80, p95 25.96, p99 26.43, max 27.33 ms
  presented -> pixels on the CPU: p50 25.18, p95 26.21, p99 26.72, max 27.72 ms
  presented -> Windows' LastPresentTime: p50 24.77, p95 25.92, p99 26.40, max 27.30 ms
  longest wait for a new frame: 28.4 ms
  started over 0 times

Frames both paths took: 3252. Frame channel minus Desktop Duplication, when each had the frame (negative: the channel first): p50 -0.14, p95 -0.08, p99 2.67, max 6.08 ms
RESULT channel=ok duplication=ok presented=4781 channel_frames=3253 duplication_frames=4778 channel_p50_ms=24.69 duplication_p50_ms=24.80 channel_p95_ms=25.80 duplication_p95_ms=25.96 channel_restarts=0 duplication_restarts=0
## end UTC 00:23:21.816, exit
```

### T2

```
## T2: luzvda-ctl capture-test 6a0b9c1e-3f4d-4c2a-9e7b-1d2c3b4a5f60 --add --hdr off --seconds 30
## start UTC 00:23:26.870
LuzVDA 0.0.0.12, protocol 1.1
Rendering on NVIDIA GeForce RTX 5080
HDR off before measuring: ok
Display \\.\DISPLAY135: 3024x1964 at 4480,0, 120.000 Hz, HDR off

The window presented 4781 frames in 29.0 s after a 1 s warm-up (164.8 per second)

Frame channel: header and event opened by handle, textures by handle
  frames taken 3354, codes read 3249 after the warm-up (68.0% of the 4781 presented), unreadable 0
  presented -> available: p50 24.68, p95 25.28, p99 29.19, max 31.18 ms
  presented -> pixels on the CPU: p50 25.05, p95 25.63, p99 29.55, max 31.52 ms
  presented -> driver has it: p50 24.64, p95 25.24, p99 29.15, max 31.13 ms
  driver queues the copy: p50 0.02, p95 0.02, p99 0.03, max 0.17 ms
  copy queued -> consumer wakes: p50 0.02, p95 0.03, p99 0.06, max 0.19 ms
  longest wait for a new frame: 27.3 ms
  started over 0 times
  the driver copied 3363, dropped 0 (no free slot); generation 1, 3024x1964, DXGI format 87; last error 0x00000000

Desktop Duplication: DuplicateOutput1 (FP16 or 8-bit)
  frames taken 4941, codes read 4778 after the warm-up (99.9% of the 4781 presented), unreadable 0
  presented -> available: p50 24.80, p95 25.43, p99 26.12, max 26.84 ms
  presented -> pixels on the CPU: p50 25.12, p95 25.70, p99 26.32, max 27.11 ms
  presented -> Windows' LastPresentTime: p50 24.76, p95 25.40, p99 26.09, max 26.81 ms
  longest wait for a new frame: 27.8 ms
  started over 0 times

Frames both paths took: 3248. Frame channel minus Desktop Duplication, when each had the frame (negative: the channel first): p50 -0.16, p95 -0.12, p99 4.55, max 6.62 ms
RESULT channel=ok duplication=ok presented=4781 channel_frames=3249 duplication_frames=4778 channel_p50_ms=24.68 duplication_p50_ms=24.80 channel_p95_ms=25.28 duplication_p95_ms=25.43 channel_restarts=0 duplication_restarts=0
## end UTC 00:24:00.405, exit
```

### T3

```
## T3: luzvda-ctl capture-test 6a0b9c1e-3f4d-4c2a-9e7b-1d2c3b4a5f60 --add --hdr on --seconds 40 --hdr-switch-every 8
## start UTC 00:24:05.450
LuzVDA 0.0.0.12, protocol 1.1
Rendering on NVIDIA GeForce RTX 5080
HDR on before measuring: ok
Display \\.\DISPLAY135: 3024x1964 at 4480,0, 120.000 Hz, HDR on
HDR off: ok
HDR on: ok
HDR off: ok
HDR on: ok

The window presented 6193 frames in 39.0 s after a 1 s warm-up (158.8 per second)

Frame channel: header and event opened by handle, textures by handle
  frames taken 4252, codes read 4141 after the warm-up (66.9% of the 6193 presented), unreadable 0
  presented -> available: p50 24.69, p95 26.13, p99 30.64, max 325.26 ms
  presented -> pixels on the CPU: p50 25.11, p95 26.53, p99 30.96, max 325.75 ms
  presented -> driver has it: p50 24.65, p95 26.08, p99 30.55, max 325.22 ms
  driver queues the copy: p50 0.02, p95 0.02, p99 0.08, max 0.22 ms
  copy queued -> consumer wakes: p50 0.02, p95 0.07, p99 0.09, max 2.23 ms
  longest wait for a new frame: 363.7 ms
  started over 4 times
  HDR switch 1: longest wait in the 5 s after it 348.7 ms
  HDR switch 2: longest wait in the 5 s after it 347.9 ms
  HDR switch 3: longest wait in the 5 s after it 363.7 ms
  HDR switch 4: longest wait in the 5 s after it 350.9 ms
  the driver copied 4263, dropped 0 (no free slot); generation 5, 3024x1964, DXGI format 10; last error 0x00000000

Desktop Duplication: DuplicateOutput1 (FP16 or 8-bit)
  frames taken 6359, codes read 6185 after the warm-up (99.9% of the 6193 presented), unreadable 0
  presented -> available: p50 24.82, p95 26.56, p99 32.67, max 325.50 ms
  presented -> pixels on the CPU: p50 25.17, p95 26.87, p99 33.14, max 325.79 ms
  presented -> Windows' LastPresentTime: p50 24.78, p95 26.52, p99 32.63, max 325.46 ms
  longest wait for a new frame: 288.0 ms
  started over 12 times
  HDR switch 1: longest wait in the 5 s after it 288.0 ms
  HDR switch 2: longest wait in the 5 s after it 284.3 ms
  HDR switch 3: longest wait in the 5 s after it 281.2 ms
  HDR switch 4: longest wait in the 5 s after it 279.6 ms

Frames both paths took: 4137. Frame channel minus Desktop Duplication, when each had the frame (negative: the channel first): p50 -0.15, p95 -0.08, p99 4.51, max 6.46 ms
RESULT channel=ok duplication=ok presented=6193 channel_frames=4141 duplication_frames=6185 channel_p50_ms=24.69 duplication_p50_ms=24.82 channel_p95_ms=26.13 duplication_p95_ms=26.56 channel_restarts=4 duplication_restarts=12
## end UTC 00:24:48.979, exit
```

### T4

```
## T4: luzvda-ctl capture-test 6a0b9c1e-3f4d-4c2a-9e7b-1d2c3b4a5f60 --add --hdr on --seconds 20 --only channel
## start UTC 00:24:54.024
LuzVDA 0.0.0.12, protocol 1.1
Rendering on NVIDIA GeForce RTX 5080
HDR on before measuring: ok
Display \\.\DISPLAY135: 3024x1964 at 4480,0, 120.000 Hz, HDR on

The window presented 3132 frames in 19.0 s after a 1 s warm-up (164.8 per second)

Frame channel: header and event opened by handle, textures by handle
  frames taken 2211, codes read 2107 after the warm-up (67.3% of the 3132 presented), unreadable 0
  presented -> available: p50 24.69, p95 25.82, p99 28.90, max 31.04 ms
  presented -> pixels on the CPU: p50 25.05, p95 26.17, p99 29.21, max 31.46 ms
  presented -> driver has it: p50 24.64, p95 25.78, p99 28.83, max 30.94 ms
  driver queues the copy: p50 0.02, p95 0.07, p99 0.10, max 0.22 ms
  copy queued -> consumer wakes: p50 0.02, p95 0.07, p99 0.11, max 0.51 ms
  longest wait for a new frame: 25.4 ms
  started over 0 times
  the driver copied 2219, dropped 0 (no free slot); generation 1, 3024x1964, DXGI format 10; last error 0x00000000

Desktop Duplication: 
  not run

Frames both paths took: 0. Frame channel minus Desktop Duplication, when each had the frame (negative: the channel first): none
RESULT channel=ok duplication=skipped presented=3132 channel_frames=2107 duplication_frames=0 channel_p50_ms=24.69 duplication_p50_ms=0.00 channel_p95_ms=25.82 duplication_p95_ms=0.00 channel_restarts=0 duplication_restarts=0
## end UTC 00:25:17.168, exit
```

### T5

```
## T5: luzvda-ctl capture-test 6a0b9c1e-3f4d-4c2a-9e7b-1d2c3b4a5f60 --add --hdr on --seconds 20 --only duplication
## start UTC 00:25:22.209
LuzVDA 0.0.0.12, protocol 1.1
Rendering on NVIDIA GeForce RTX 5080
HDR on before measuring: ok
Display \\.\DISPLAY135: 3024x1964 at 4480,0, 120.000 Hz, HDR on

The window presented 3132 frames in 19.0 s after a 1 s warm-up (164.8 per second)

Frame channel: 
  not run

Desktop Duplication: DuplicateOutput1 (FP16 or 8-bit)
  frames taken 3293, codes read 3129 after the warm-up (99.9% of the 3132 presented), unreadable 0
  presented -> available: p50 24.80, p95 25.41, p99 26.10, max 26.48 ms
  presented -> pixels on the CPU: p50 24.98, p95 25.59, p99 26.28, max 26.65 ms
  presented -> Windows' LastPresentTime: p50 24.76, p95 25.37, p99 26.05, max 26.44 ms
  longest wait for a new frame: 25.7 ms
  started over 0 times

Frames both paths took: 0. Frame channel minus Desktop Duplication, when each had the frame (negative: the channel first): none
RESULT channel=skipped duplication=ok presented=3132 channel_frames=0 duplication_frames=3129 channel_p50_ms=0.00 duplication_p50_ms=24.80 channel_p95_ms=0.00 duplication_p95_ms=25.41 channel_restarts=0 duplication_restarts=0
## end UTC 00:25:45.389, exit
```

### T6

```
## T6: luzvda-ctl capture-test 6a0b9c1e-3f4d-4c2a-9e7b-1d2c3b4a5f60 --add --hdr on --seconds 20 --debug-privilege
## start UTC 00:25:50.427
SeDebugPrivilege: enabled
LuzVDA 0.0.0.12, protocol 1.1
Rendering on NVIDIA GeForce RTX 5080
HDR on before measuring: ok
Display \\.\DISPLAY135: 3024x1964 at 4480,0, 120.000 Hz, HDR on

The window presented 3132 frames in 19.0 s after a 1 s warm-up (164.8 per second)

Frame channel: header and event opened by handle, textures by handle
  frames taken 2263, codes read 2161 after the warm-up (69.0% of the 3132 presented), unreadable 0
  presented -> available: p50 24.68, p95 25.29, p99 26.19, max 30.87 ms
  presented -> pixels on the CPU: p50 25.11, p95 25.70, p99 26.60, max 31.30 ms
  presented -> driver has it: p50 24.64, p95 25.26, p99 26.16, max 30.84 ms
  driver queues the copy: p50 0.02, p95 0.02, p99 0.02, max 0.07 ms
  copy queued -> consumer wakes: p50 0.02, p95 0.03, p99 0.05, max 0.17 ms
  longest wait for a new frame: 26.2 ms
  started over 0 times
  the driver copied 2271, dropped 0 (no free slot); generation 1, 3024x1964, DXGI format 10; last error 0x00000000

Desktop Duplication: DuplicateOutput1 (FP16 or 8-bit)
  frames taken 3293, codes read 3129 after the warm-up (99.9% of the 3132 presented), unreadable 0
  presented -> available: p50 24.81, p95 25.46, p99 26.12, max 26.45 ms
  presented -> pixels on the CPU: p50 25.19, p95 25.72, p99 26.37, max 26.97 ms
  presented -> Windows' LastPresentTime: p50 24.77, p95 25.42, p99 26.09, max 26.41 ms
  longest wait for a new frame: 26.4 ms
  started over 0 times

Frames both paths took: 2161. Frame channel minus Desktop Duplication, when each had the frame (negative: the channel first): p50 -0.16, p95 -0.12, p99 1.68, max 6.25 ms
RESULT channel=ok duplication=ok presented=3132 channel_frames=2161 duplication_frames=3129 channel_p50_ms=24.68 duplication_p50_ms=24.81 channel_p95_ms=25.29 duplication_p95_ms=25.46 channel_restarts=0 duplication_restarts=0
## end UTC 00:26:13.545, exit
```

### T7

```
## T7: luzvda-ctl capture-test 6a0b9c1e-3f4d-4c2a-9e7b-1d2c3b4a5f60 --add --hdr on --seconds 30
## start UTC 00:26:18.582
LuzVDA 0.0.0.12, protocol 1.1
Rendering on NVIDIA GeForce RTX 5080
HDR on before measuring: ok
Display \\.\DISPLAY135: 3024x1964 at 4480,0, 120.000 Hz, HDR on

The window presented 4781 frames in 29.0 s after a 1 s warm-up (164.8 per second)

Frame channel: header and event opened by handle, textures by handle
  frames taken 3362, codes read 3256 after the warm-up (68.1% of the 4781 presented), unreadable 0
  presented -> available: p50 24.69, p95 25.26, p99 27.87, max 30.98 ms
  presented -> pixels on the CPU: p50 25.10, p95 25.65, p99 28.22, max 31.52 ms
  presented -> driver has it: p50 24.63, p95 25.21, p99 27.83, max 30.82 ms
  driver queues the copy: p50 0.02, p95 0.06, p99 0.08, max 0.28 ms
  copy queued -> consumer wakes: p50 0.02, p95 0.07, p99 0.08, max 0.28 ms
  longest wait for a new frame: 31.8 ms
  started over 0 times
  the driver copied 3370, dropped 0 (no free slot); generation 1, 3024x1964, DXGI format 10; last error 0x00000000

Desktop Duplication: DuplicateOutput1 (FP16 or 8-bit)
  frames taken 4941, codes read 4778 after the warm-up (99.9% of the 4781 presented), unreadable 0
  presented -> available: p50 24.78, p95 25.40, p99 26.04, max 26.68 ms
  presented -> pixels on the CPU: p50 25.17, p95 25.68, p99 26.27, max 26.96 ms
  presented -> Windows' LastPresentTime: p50 24.75, p95 25.36, p99 26.01, max 26.65 ms
  longest wait for a new frame: 25.6 ms
  started over 0 times

Frames both paths took: 3256. Frame channel minus Desktop Duplication, when each had the frame (negative: the channel first): p50 -0.13, p95 -0.07, p99 3.25, max 6.35 ms
RESULT channel=ok duplication=ok presented=4781 channel_frames=3256 duplication_frames=4778 channel_p50_ms=24.69 duplication_p50_ms=24.78 channel_p95_ms=25.26 duplication_p95_ms=25.40 channel_restarts=0 duplication_restarts=0
## end UTC 00:26:51.777, exit
```

### T8

```
## T8: luzvda-ctl capture-test 6a0b9c1e-3f4d-4c2a-9e7b-1d2c3b4a5f60 --add --hdr on --seconds 600
## start UTC 00:27:22.980
LuzVDA 0.0.0.12, protocol 1.1
Rendering on NVIDIA GeForce RTX 5080
HDR on before measuring: ok
Display \\.\DISPLAY135: 3024x1964 at 4480,0, 120.000 Hz, HDR on

The window presented 98733 frames in 599.0 s after a 1 s warm-up (164.8 per second)

Frame channel: header and event opened by handle, textures by handle
  frames taken 66887, codes read 66785 after the warm-up (67.6% of the 98733 presented), unreadable 0
  presented -> available: p50 24.69, p95 25.44, p99 29.10, max 38.98 ms
  presented -> pixels on the CPU: p50 25.10, p95 25.85, p99 29.41, max 39.50 ms
  presented -> driver has it: p50 24.63, p95 25.40, p99 29.02, max 38.93 ms
  driver queues the copy: p50 0.02, p95 0.06, p99 0.08, max 0.31 ms
  copy queued -> consumer wakes: p50 0.02, p95 0.07, p99 0.09, max 0.33 ms
  longest wait for a new frame: 31.5 ms
  started over 0 times
  the driver copied 66895, dropped 0 (no free slot); generation 1, 3024x1964, DXGI format 10; last error 0x00000000

Desktop Duplication: DuplicateOutput1 (FP16 or 8-bit)
  frames taken 98894, codes read 98730 after the warm-up (100.0% of the 98733 presented), unreadable 0
  presented -> available: p50 24.80, p95 25.43, p99 26.11, max 34.26 ms
  presented -> pixels on the CPU: p50 25.18, p95 25.72, p99 26.34, max 34.46 ms
  presented -> Windows' LastPresentTime: p50 24.76, p95 25.40, p99 26.07, max 34.23 ms
  longest wait for a new frame: 25.4 ms
  started over 0 times

Frames both paths took: 66785. Frame channel minus Desktop Duplication, when each had the frame (negative: the channel first): p50 -0.14, p95 -0.07, p99 4.42, max 14.43 ms
RESULT channel=ok duplication=ok presented=98733 channel_frames=66785 duplication_frames=98730 channel_p50_ms=24.69 duplication_p50_ms=24.80 channel_p95_ms=25.44 duplication_p95_ms=25.43 channel_restarts=0 duplication_restarts=0
## end UTC 00:37:26.364, exit 0
```

### T9

```
## T9: runas /trustlevel:0x20000 (basic user) luzvda-ctl version - UTC 00:37:31.421
## T9 as: 
Mandatory Label\High Mandatory Level                          Label            S-1-16-12288                                                                                                                                                    
NT AUTHORITY\Local account and member of Administrators group Well-known group S-1-5-114                                                                                                     Mandatory group, Enabled by default, Enabled group
BUILTIN\Administrators                                        Alias            S-1-5-32-544                                                                                                  Group used for deny only                          
## C:\LuzVDA\fc\signed\luzvda-ctl.exe version 
Opening LuzVDA (installed? elevated prompt?) failed: 5 Access is denied.

exit=1
```

## GPU summary (nvidia-smi)

```
Baseline (no test)     n=  39  GPU util mean   4.5% p95   5% | mem util mean   1.0% | mem used mean  1,569 MiB max 1583 MiB
T4 channel only        n=  39  GPU util mean   9.6% p95  10% | mem util mean   3.0% | mem used mean  1,975 MiB max 1996 MiB
T5 duplication only    n=  39  GPU util mean   8.8% p95   9% | mem util mean   1.6% | mem used mean  1,900 MiB max 1916 MiB
T8 both, 600 s         n=1162  GPU util mean  12.3% p95  13% | mem util mean   3.0% | mem used mean  2,019 MiB max 2058 MiB
```

## All driver trace events (UTC)

```
00:22:48.096Z RenderAdapterSet LuidLow=69369 LuidHigh=0
00:22:48.096Z DisplayAdded Connector=0 TargetId=256 Width=3024 Height=1964 RefreshMilliHz=120000 Hdr=true
00:22:48.404Z SwapChainAssigned Connector=0 LuidLow=69369 LuidHigh=0
00:22:48.404Z SwapChainUnassigned Connector=0
00:22:48.404Z SwapChainEnded Result=0x887A0026
00:22:48.569Z SwapChainAssigned Connector=0 LuidLow=69369 LuidHigh=0
00:22:48.657Z SwapChainEnded Result=0x887A0026
00:22:48.834Z SwapChainAssigned Connector=0 LuidLow=69369 LuidHigh=0
00:22:48.834Z SwapChainUnassigned Connector=0
00:22:48.834Z SwapChainEnded Result=0x887A0026
00:22:48.994Z SwapChainAssigned Connector=0 LuidLow=69369 LuidHigh=0
00:22:51.565Z FrameChannelOpened Connector=0 Result=0x0
00:22:51.575Z FrameTexturesMade Generation=1 Width=3024 Height=1964 Format=10 Names=0x107
00:23:21.600Z SwapChainUnassigned Connector=0
00:23:21.600Z SwapChainEnded Result=0x0
00:23:21.612Z DisplayRemoved Status=0x0
00:23:26.892Z RenderAdapterSet LuidLow=69369 LuidHigh=0
00:23:26.893Z DisplayAdded Connector=0 TargetId=256 Width=3024 Height=1964 RefreshMilliHz=120000 Hdr=true
00:23:27.129Z SwapChainAssigned Connector=0 LuidLow=69369 LuidHigh=0
00:23:27.129Z SwapChainUnassigned Connector=0
00:23:27.129Z SwapChainEnded Result=0x887A0026
00:23:27.278Z SwapChainAssigned Connector=0 LuidLow=69369 LuidHigh=0
00:23:27.330Z SwapChainEnded Result=0x887A0026
00:23:27.496Z SwapChainAssigned Connector=0 LuidLow=69369 LuidHigh=0
00:23:27.496Z SwapChainUnassigned Connector=0
00:23:27.496Z SwapChainEnded Result=0x887A0026
00:23:27.660Z SwapChainAssigned Connector=0 LuidLow=69369 LuidHigh=0
00:23:30.198Z FrameChannelOpened Connector=0 Result=0x0
00:23:30.199Z FrameTexturesMade Generation=1 Width=3024 Height=1964 Format=87 Names=0x107
00:24:00.228Z SwapChainUnassigned Connector=0
00:24:00.228Z SwapChainEnded Result=0x0
00:24:00.240Z DisplayRemoved Status=0x0
00:24:05.474Z RenderAdapterSet LuidLow=69369 LuidHigh=0
00:24:05.475Z DisplayAdded Connector=0 TargetId=256 Width=3024 Height=1964 RefreshMilliHz=120000 Hdr=true
00:24:05.712Z SwapChainAssigned Connector=0 LuidLow=69369 LuidHigh=0
00:24:05.712Z SwapChainUnassigned Connector=0
00:24:05.712Z SwapChainEnded Result=0x887A0026
00:24:05.857Z SwapChainAssigned Connector=0 LuidLow=69369 LuidHigh=0
00:24:05.914Z SwapChainEnded Result=0x887A0026
00:24:06.073Z SwapChainAssigned Connector=0 LuidLow=69369 LuidHigh=0
00:24:06.073Z SwapChainUnassigned Connector=0
00:24:06.073Z SwapChainEnded Result=0x887A0026
00:24:06.232Z SwapChainAssigned Connector=0 LuidLow=69369 LuidHigh=0
00:24:08.758Z FrameChannelOpened Connector=0 Result=0x0
00:24:08.758Z FrameTexturesMade Generation=1 Width=3024 Height=1964 Format=10 Names=0x107
00:24:16.818Z SwapChainEnded Result=0x887A0026
00:24:16.987Z SwapChainAssigned Connector=0 LuidLow=69369 LuidHigh=0
00:24:16.987Z SwapChainUnassigned Connector=0
00:24:16.987Z SwapChainEnded Result=0x887A0026
00:24:17.133Z SwapChainAssigned Connector=0 LuidLow=69369 LuidHigh=0
00:24:17.134Z FrameTexturesMade Generation=2 Width=3024 Height=1964 Format=87 Names=0x107
00:24:24.826Z SwapChainEnded Result=0x887A0026
00:24:24.990Z SwapChainAssigned Connector=0 LuidLow=69369 LuidHigh=0
00:24:24.990Z SwapChainUnassigned Connector=0
00:24:24.990Z SwapChainEnded Result=0x887A0026
00:24:25.138Z SwapChainAssigned Connector=0 LuidLow=69369 LuidHigh=0
00:24:25.140Z FrameTexturesMade Generation=3 Width=3024 Height=1964 Format=10 Names=0x107
00:24:32.834Z SwapChainEnded Result=0x887A0026
00:24:33.000Z SwapChainAssigned Connector=0 LuidLow=69369 LuidHigh=0
00:24:33.001Z SwapChainUnassigned Connector=0
00:24:33.001Z SwapChainEnded Result=0x887A0026
00:24:33.149Z SwapChainAssigned Connector=0 LuidLow=69369 LuidHigh=0
00:24:33.150Z FrameTexturesMade Generation=4 Width=3024 Height=1964 Format=87 Names=0x107
00:24:40.830Z SwapChainEnded Result=0x887A0026
00:24:40.991Z SwapChainAssigned Connector=0 LuidLow=69369 LuidHigh=0
00:24:40.991Z SwapChainUnassigned Connector=0
00:24:40.991Z SwapChainEnded Result=0x887A0026
00:24:41.143Z SwapChainAssigned Connector=0 LuidLow=69369 LuidHigh=0
00:24:41.144Z FrameTexturesMade Generation=5 Width=3024 Height=1964 Format=10 Names=0x107
00:24:48.788Z SwapChainUnassigned Connector=0
00:24:48.789Z SwapChainEnded Result=0x0
00:24:48.801Z DisplayRemoved Status=0x0
00:24:54.050Z RenderAdapterSet LuidLow=69369 LuidHigh=0
00:24:54.050Z DisplayAdded Connector=0 TargetId=256 Width=3024 Height=1964 RefreshMilliHz=120000 Hdr=true
00:24:54.277Z SwapChainAssigned Connector=0 LuidLow=69369 LuidHigh=0
00:24:54.277Z SwapChainUnassigned Connector=0
00:24:54.277Z SwapChainEnded Result=0x887A0026
00:24:54.434Z SwapChainAssigned Connector=0 LuidLow=69369 LuidHigh=0
00:24:56.976Z FrameChannelOpened Connector=0 Result=0x0
00:24:56.982Z FrameTexturesMade Generation=1 Width=3024 Height=1964 Format=10 Names=0x107
00:25:16.995Z SwapChainUnassigned Connector=0
00:25:16.995Z SwapChainEnded Result=0x0
00:25:17.007Z DisplayRemoved Status=0x0
00:25:22.234Z RenderAdapterSet LuidLow=69369 LuidHigh=0
00:25:22.234Z DisplayAdded Connector=0 TargetId=256 Width=3024 Height=1964 RefreshMilliHz=120000 Hdr=true
00:25:22.460Z SwapChainAssigned Connector=0 LuidLow=69369 LuidHigh=0
00:25:22.460Z SwapChainUnassigned Connector=0
00:25:22.460Z SwapChainEnded Result=0x887A0026
00:25:22.605Z SwapChainAssigned Connector=0 LuidLow=69369 LuidHigh=0
00:25:45.200Z SwapChainUnassigned Connector=0
00:25:45.200Z SwapChainEnded Result=0x0
00:25:45.212Z DisplayRemoved Status=0x0
00:25:50.452Z RenderAdapterSet LuidLow=69369 LuidHigh=0
00:25:50.452Z DisplayAdded Connector=0 TargetId=256 Width=3024 Height=1964 RefreshMilliHz=120000 Hdr=true
00:25:50.654Z SwapChainAssigned Connector=0 LuidLow=69369 LuidHigh=0
00:25:50.654Z SwapChainUnassigned Connector=0
00:25:50.654Z SwapChainEnded Result=0x887A0026
00:25:50.799Z SwapChainAssigned Connector=0 LuidLow=69369 LuidHigh=0
00:25:53.347Z FrameChannelOpened Connector=0 Result=0x0
00:25:53.348Z FrameTexturesMade Generation=1 Width=3024 Height=1964 Format=10 Names=0x107
00:26:13.370Z SwapChainUnassigned Connector=0
00:26:13.370Z SwapChainEnded Result=0x0
00:26:13.382Z DisplayRemoved Status=0x0
00:26:18.602Z RenderAdapterSet LuidLow=69369 LuidHigh=0
00:26:18.602Z DisplayAdded Connector=0 TargetId=256 Width=3024 Height=1964 RefreshMilliHz=120000 Hdr=true
00:26:18.837Z SwapChainAssigned Connector=0 LuidLow=69369 LuidHigh=0
00:26:18.837Z SwapChainUnassigned Connector=0
00:26:18.838Z SwapChainEnded Result=0x887A0026
00:26:18.987Z SwapChainAssigned Connector=0 LuidLow=69369 LuidHigh=0
00:26:21.569Z FrameChannelOpened Connector=0 Result=0x0
00:26:21.578Z FrameTexturesMade Generation=1 Width=3024 Height=1964 Format=10 Names=0x107
00:26:51.597Z SwapChainUnassigned Connector=0
00:26:51.597Z SwapChainEnded Result=0x0
00:26:51.610Z DisplayRemoved Status=0x0
00:27:23.006Z RenderAdapterSet LuidLow=69369 LuidHigh=0
00:27:23.006Z DisplayAdded Connector=0 TargetId=256 Width=3024 Height=1964 RefreshMilliHz=120000 Hdr=true
00:27:23.238Z SwapChainAssigned Connector=0 LuidLow=69369 LuidHigh=0
00:27:23.238Z SwapChainUnassigned Connector=0
00:27:23.239Z SwapChainEnded Result=0x887A0026
00:27:23.385Z SwapChainAssigned Connector=0 LuidLow=69369 LuidHigh=0
00:27:25.961Z FrameChannelOpened Connector=0 Result=0x0
00:27:25.969Z FrameTexturesMade Generation=1 Width=3024 Height=1964 Format=10 Names=0x107
00:37:26.181Z SwapChainUnassigned Connector=0
00:37:26.181Z SwapChainEnded Result=0x0
00:37:26.194Z DisplayRemoved Status=0x0
```
