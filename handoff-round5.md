# Handoff: round 5. Round 4's checks, the encoder benchmark, frame-loss logging, then sleep

The same three Claude sessions take part as in rounds 3 and 4, talking through Remote Control messages. Each message starts with the word in capitals below.

| Role | Session | Runs on |
|---|---|---|
| **Cloud** | `session_014Gjk2MvrNtPDUetWZRBLU6` | the cloud; reads the results |
| **PC** | `session_01XyvV4TjBosBRLArmozHhmS` | the Windows PC, elevated |
| **Mac** | `session_016fcTdns2gRRa8g5evrZ1CS` | the MacBook |

The owner approves once in the PC session and once in the Mac session. That approval covers every ⚠ step for that role here, the sleep at the end included.

**About 1 h 45 min:**
- the Mac's part takes about 30 minutes;
- the PC's offline encodes then take about an hour.

At the end, **each machine goes to sleep, at the owner's request.**

This round replaces `handoff-round4.md`: Part A below is round 4. Read round 4's pass table there for Part A's rows.

## What's being tested

Apollo `claude/luzvda` at `44544ba` or later:

| Commit | What it adds |
|---|---|
| `e1ff09d` | HDR is set during the launch, with new arrival and HDR log lines (round 4, H1–H3 and H5) |
| `08f78c0` | `dd_wa_virtual_display_hdr_toggle` is off by default (round 4, H4) |
| `dc56203` | **Frame-loss logging.** The client's per-frame FEC reports are logged as `fork: frame loss for client [<device>] in the last 10.0 s, …` while there are losses, and `… over <time>, …` at the end of each stream, also when nothing was lost. |
| `44544ba` | **The encoder benchmark,** `scripts/fork/encoder-bench.ps1` (read `scripts/fork/encoder-bench.md` next to it) |

## What this round answers

| | Question | Pass when |
|---|---|---|
| **A** | Round 4's H1–H5 | As in `handoff-round4.md`. |
| **L1** | Does Apollo log the client's losses? | Every stream ends with a `fork: frame loss … over …` line. Any `in the last 10.0 s` lines are listed with their times. |
| **B1** | Do the clips record? | `text-lossless.mkv` and `motion-lossless.mkv` exist. Their capture logs say exit 0 and the stream's size (3024x1964), with few or no dropped frames. The Mac's screenshot during each capture shows the page full screen. |
| **B2** | What do NVENC's settings buy in picture? | `results.csv` and `results.md` cover the `core` matrix. A failed encode is listed with its reason, for example AV1 4:4:4 if the GPU can't do it. |
| **B3** | What do they cost in delay? | For each `Bench P*` stream, the Mac's host processing latency (min/avg/max), decode time, frame drops and keyframe requests, plus the codec actually used. |
| **Sleep** | | Both machines asleep at the end, after everything is pushed and sent. |

## Rules for PC and Mac

1. **Approval.** Before your first step, ask the owner once, in two lines:
   > Round 5, role <PC|Mac>: <your ⚠ steps in a few words, including the sleep at the end>. Approve all?
2. **Stop and send `PROBLEM: <what>`** to Cloud and the other role on:
   - an install failure, a crash, or no stream starting twice in a row;
   - more than one active display during a stream, twice in a row.
3. **Settings:** don't change Apollo's settings beyond the three named here:
   - Desktop's `terminate-on-pause`;
   - `dd_wa_virtual_display_hdr_toggle`;
   - the `Bench …` apps in `apps.json`.

   All are put back at the end, checked by hash. The Mac changes none of LuzLunar's settings beyond the HDR peak override (R1/R2): codec, 4:4:4 and bitrate are command-line options.
4. **Never:**
   - touch the signing key, Secure Boot or test signing;
   - push anywhere but `test-results/round5` in anyosag/LuzVDA;
   - **lock** the PC or the Mac. Sleep only at the very end, as below. If either OS locks on its own when it wakes, that's its normal setting.
5. **Times and redaction:** times are UTC. Redact what you push: `<ip>`, `<uuid>`, `<computer>`.
6. **Folders:** use `C:\LuzVDA\r5\` on the PC and `/tmp/r5/` on the Mac.

---

## Role PC

### P1. Preflight

1. The PC must be unlocked (`Get-Process LogonUI` returns nothing), with nobody streaming (`C:\LuzVDA\r3\ctl\luzvda-ctl.exe list` shows 0 displays) and ApolloService running.
2. **Find the Apollo build:** the newest green `Build Windows` run on `anyosag/Apollo` branch `claude/luzvda` whose head is `44544ba` or later.
3. **Free disk:** `C:` needs about 25 GB free. Say how much there is.

### P2. ⚠ Install Apollo

As in round 3's P2: download, check the hashes and attestation, back up `config` to `C:\LuzVDA\r5\config-backup`, and install with `/S`.

**Check:**
- `vdisplay: LuzVDA 0.0.0.9, protocol 1.0` in `sunshine.log`;
- no `dd_wa_virtual_display_hdr_toggle` line in `sunshine.conf`.

### P3. ⚠ ffmpeg and the benchmark script

1. **ffmpeg.** Download a Windows ffmpeg 7 build with `hevc_nvenc`, `av1_nvenc`, `libvmaf` and `ddagrab`, and check its SHA256 against the checksum its release publishes. For example, BtbN's `ffmpeg-n7.1-latest-win64-gpl-7.1.zip` from https://github.com/BtbN/FFmpeg-Builds/releases, with that release's `checksums.sha256`. Extract it to `C:\LuzVDA\r5\ffmpeg\`.
2. **The script.** Fetch `scripts/fork/encoder-bench.ps1` from `anyosag/Apollo` at the installed build's commit to `C:\LuzVDA\r5\encoder-bench.ps1`.
3. **Check:**
   ```powershell
   C:\LuzVDA\r5\encoder-bench.ps1 -Mode Check -Ffmpeg C:\LuzVDA\r5\ffmpeg\bin\ffmpeg.exe
   ```
   It must say `CHECK OK`. If `libvmaf` or `ddagrab` is missing, try another build, such as gyan.dev's full build, checked the same way.

### P4. ⚠ apps.json and the Desktop setting

1. Back up `apps.json` to `C:\LuzVDA\r5\apps.json.orig`, with its hash.
2. Set Desktop's `terminate-on-pause` to `false`, as in round 3.
3. **Bench apps.** Let the signed-in user write the folder, since Apollo starts the apps as that user:
   ```powershell
   icacls C:\LuzVDA\r5 /grant "$($env:USERDOMAIN)\$($env:USERNAME):(OI)(CI)M"
   ```
   Then add the apps:
   ```powershell
   C:\LuzVDA\r5\encoder-bench.ps1 -Mode AddApps -Ffmpeg C:\LuzVDA\r5\ffmpeg\bin\ffmpeg.exe -OutDir C:\LuzVDA\r5\bench
   ```
   It adds six `Bench …` apps (listed in `encoder-bench.md`).
4. Restart ApolloService. Check that the log shows no warning about `apps.json`.

### P5. Recording

1. Start round 3's display sampler, writing to `C:\LuzVDA\r5\displays.csv`.
2. Start the driver trace:
   ```powershell
   logman start luzvda-r5 -p "{fdbe20c6-12e0-4cf1-907d-e0b99b08b5ce}" -o C:\LuzVDA\r5\trace.etl -ets
   ```
3. Note `sunshine.log`'s line count.
4. Send `PC READY` to the Mac, with:
   - Apollo's version and commit;
   - your UTC offset;
   - the note "Part A phase 1 and Part B: defaults".

### P6. During Part B: check the clips

After the Mac's `CAPTURED text`, and again after `CAPTURED motion`, read `C:\LuzVDA\r5\bench\clips\<clip>-capture.txt` and `<clip>-ffmpeg.txt`. Check:
- exit 0;
- the output size (3024x1964);
- the frames written, and any `dup=`/`drop=`;
- that `<clip>-lossless.mkv` exists.

When both are good, send `CAPTURES OK` to the Mac. If one failed, send `RECAPTURE <clip>: <why>` (once per clip); after a second failure, go on without it and say so in the report.

### P7. Between Part B and Part A phase 2

When `PART B DONE` arrives:
1. Save the log so far, since an Apollo restart starts a new one.
2. ⚠ Copy `sunshine.conf` to `C:\LuzVDA\r5\sunshine.conf.orig`, with its hash, and add `dd_wa_virtual_display_hdr_toggle = enabled`.
3. Restart ApolloService. Check for `config: 'dd_wa_virtual_display_hdr_toggle' = enabled` and no warning.
4. Send `PHASE A2 READY` to the Mac.

### P8. Restore, encode, analyze, report

When `MAC DONE` arrives:
1. **Stop the recording:** stop the sampler, then stop and decode the trace.
2. **⚠ Restore** `sunshine.conf` and `apps.json` from the `.orig` files, each checked by hash, and restart ApolloService. Leave the new Apollo installed.
3. **Encode.** Use the bitrate the Mac reported. If its Office and Gaming profiles use other bitrates, add them to `-BitrateKbps`, at most 3 in all. This takes about an hour per bitrate.
   ```powershell
   C:\LuzVDA\r5\encoder-bench.ps1 -Mode Encode -Ffmpeg C:\LuzVDA\r5\ffmpeg\bin\ffmpeg.exe -OutDir C:\LuzVDA\r5\bench -BitrateKbps <kbps>
   ```
   Then delete `C:\LuzVDA\r5\bench\work\*.yuv` (about 16 GB). Keep the clips.
4. **Analyze `sunshine.log`.**
   - **Part A:** as in round 4's P6 step 3, for L1–L7 and R1/R2.
   - **Every stream (L1):** each `fork: frame loss` line, with its time and the stream it belongs to.
   - **Every `Bench P*` stream:**
     - `fork: encoder overrides for app [...]`;
     - the codec and resolution Apollo encoded;
     - any `Capture restarts:` line.
5. **After restoring,** with no stream running: list the displays Windows has active, and whether both of the PC's monitors are active. Don't change anything if one isn't.
6. **Report.** Write `results/round5-<date>.md` on a new branch `test-results/round5` from `origin/claude/luzvda-foundation`. Copy into `results/round5-bench/`:
   - `results.csv`;
   - `results.md`;
   - both `*-capture.txt` and `*-ffmpeg.txt`;
   - `runs\*-encode.txt` for any failed encode.

   Not the clips. Push it. Include:
   - Part A's pass table (round 4's H1–H5);
   - L1 and B1–B3;
   - the Mac's latency table;
   - `results.md`'s tables;
   - the bitrate(s) used;
   - surprises.
7. Send `ROUND5 DONE` to Cloud with the link and a line per row.
8. **⚠ Sleep the PC.** Only after the push and `ROUND5 DONE` have gone through, and nothing else is running (the trace and sampler are stopped):
   ```powershell
   Add-Type -AssemblyName System.Windows.Forms
   [System.Windows.Forms.Application]::SetSuspendState([System.Windows.Forms.PowerState]::Suspend, $false, $false)
   ```
   This session goes offline with the PC. If the call returns and the PC is still awake (for example, if sleep is disabled), say so in a last message to Cloud, and don't try hibernate, shutdown or locking instead.

---

## Role Mac

### M1. Preflight

1. As in round 3:
   - find `LL` and the host;
   - close LuzLunar's window;
   - check that a screenshot works;
   - record whether the HDR peak override (`fork.hdrPeakOverride`) is set, and the settings hash.
2. **Note the usual stream settings:**
   - the global `bitrate` (kbps), resolution, fps and codec in LuzLunar's settings;
   - any per-app stream profiles and their bitrates.

   Send them to the PC as `MAC SETTINGS`. The PC encodes at that bitrate.
3. `caffeinate -dimsu &` until the end, at most 2 hours.
4. Wait for `PC READY`, for at most 60 minutes.

### M2. Part A phase 1 (defaults)

Run round 4's M2: Before, L1–L4, R1, R2, with its screenshots and measurements, into `/tmp/r5/`.

### M3. Part B: the benchmark (defaults)

All in SDR, at 3024x1964 and 120 fps, at the usual bitrate (`--bitrate <kbps>`). After each stream, `SIGINT`, wait for LuzLunar to exit, and run `"$LL" quit "<host>"`.

**Captures.** Each app records its page from about +5 s to +10 s after it starts.

| Test | App | Options | Run for |
|---|---|---|---|
| B1 | `Bench capture text` | `--no-hdr --video-codec HEVC --yuv444` | 20 s; screenshot at +8 s |
| B2 | `Bench capture motion` | `--no-hdr --video-codec HEVC --no-yuv444` | 20 s; screenshot at +8 s |

1. After each one, send `CAPTURED <clip>` to the PC, with the screenshot's verdict: is the page full screen and moving?
2. Then wait for `CAPTURES OK`, or `RECAPTURE <clip>` (run it again once), for at most 5 minutes.

**Delay.** Run each for 40 s, then read the stream's log.

| Test | App | Options |
|---|---|---|
| B3 | `Bench P1` | `--no-hdr --video-codec HEVC --no-yuv444` |
| B4 | `Bench P4` | the same |
| B5 | `Bench P7` | the same |
| B6 | `Bench P7 full-res` | the same |
| B7 | `Bench P1` | `--no-hdr --video-codec AV1 --no-yuv444` |
| B8 | `Bench P7` | the same as B7 |

**For each B3–B8, record from the log:**
- the codec actually used (B7 and B8 show whether this Mac decodes AV1 in hardware);
- host processing latency (min/avg/max);
- decode time;
- network latency;
- frames dropped by the network, by jitter and by the decoder;
- any `Video decode unit queue overflow` or IDR request, with its time.

Then send `PART B DONE` to the PC with that table. Wait for `PHASE A2 READY`, for at most 20 minutes.

### M4. Part A phase 2 (the workaround on)

Run round 4's M3: L5–L7, with measurements.

### M5. Report and sleep

1. Send `MAC DONE` to the PC, and a copy to Cloud, with:
   - the timeline (test, start and end in UTC, exact options);
   - picture times;
   - Part A's screenshot measurements, against round 3's;
   - B1/B2's screenshot verdicts;
   - the B3–B8 table;
   - the `[fork] Describing` lines;
   - the usual settings from M1;
   - any errors.
2. Check that the override and the settings hash are as you found them.
3. Stop `caffeinate`.
4. **⚠ Sleep the Mac:** `pmset sleepnow`. This session goes offline with it. Don't lock it.
