# Notices

LuzVDA is a fork of [SudoMaker/SudoVDA](https://github.com/SudoMaker/SudoVDA), which grew out of Microsoft's Indirect Display Driver sample and MTT's Virtual Display Driver. The git history from SudoVDA is kept.

| Part | From | License |
|---|---|---|
| The driver (`src/driver/`) | Microsoft's [IddSampleDriver](https://github.com/microsoft/Windows-driver-samples/tree/main/video/IndirectDisplay), changed by SudoVDA's contributors and then by LuzVDA | Microsoft Public License (MS-PL), in [LICENSE](LICENSE). SudoVDA's README offers its own changes as "MIT and CC0 or Public Domain"; Microsoft's sample is MS-PL, and MS-PL requires source that contains it to be distributed under MS-PL. |
| `src/driver/AdapterOption.h` | MTT's Virtual Display Driver, via SudoVDA | As above |
| `include/luzvda/luzvda-ioctl.h` | Written for LuzVDA, not derived from Microsoft's sample | MIT, as stated in the file, so programs under other licenses (Apollo is GPL-3.0) can include it |
| Everything else new in LuzVDA (`src/common/`, `src/edid/`, `tools/`, `tests/`, `scripts/`, workflows) | LuzVDA | MS-PL |

Microsoft's copyright notices in the driver sources are kept, as MS-PL section 3(C) requires.
