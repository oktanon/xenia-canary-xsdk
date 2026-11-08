# XSDK Xenia: compatibility analysis vs. Xenia Canary

Date: 2026-11-08. Fork base: `master` (Canary experimental `5ef05ba` + fork commits). Upstream
`canary_experimental` is 50 commits ahead of the fork base (not merged here).

## Method

* Rebuilt the fork in Release with the portable MSVC toolchain
  (`FXC_PATH` -> Windows SDK `fxc.exe`, `VULKAN_SDK` -> folder with `glslang.exe` are required for the shader
  steps; without them the build stops at the DXBC/SPIR-V stamps).
* Ran every project of the test list on **the fork build** and on a **stock Canary** (`xenia_canary.exe`, 2026-10-13),
  headless, same inputs, 45-90 s per run, comparing the emulator log and the title's own log.
* Ran the unit suites (`xenia-vfs/kernel/cpu/base-tests`) and the PPC instruction suite (`xenia-cpu-ppc-tests`,
  test binaries generated with `powerpc-linux-gnu` binutils in WSL; the 81 VMX128 suites cannot be assembled with it).
* Cross-checked the 148 kernel/XAM imports of `xsdk` (`imports/imports.txt`) against the exports registered in the fork.

## Results per title

| Title | Stock Canary | XSDK Xenia (this fork) |
|---|---|---|
| Super Mario All-Stars (`hito5e`) | Hangs: spins on `GPU: Read from unknown register (1928)` (DC_LUT_AUTOFILL) - hundreds of errors, and `undefined extern call GetTimeZoneInformation`; game dir is read-only (`Attempted to open the file/dir for create/write`) so no `.log`, `.ini`, saves | Boots, video 1280x720 ok, ROM loaded, writes `SuperMarioAllStars.log`, `smas_port.ini`, `saves/`, `achievements.ini` |
| CTR360 (`ctr360-xsdk`) | Same: stuck polling register `0x1928` (202+ errors), `GetTimeZoneInformation` undefined | Runs to the title menu, vblank pacing 16-17 ms, Vulkan renderer active |
| Mario Kart Wii (run-007, `DATA` found in `%APPDATA%\CT-MKWII\Recomp\Install\GameAssets\DATA`) | Access violation (exit `0xC0000005`) right after `undefined extern call GetTimeZoneInformation` and read-only `game:` | NAND created from scratch on `game:`, Aurora/null backend, passes scene 1 exit, title screen, ~60 fps `[vi] heartbeat` |

Functions that run in Canary but not in the fork: **none found** in these titles. Everything the fork changed is
additive, with the exception of the regression below.

## Bugs found and fixed

1. **`NtSetInformationFile(FileRenameInformation)` could not replace an existing file** (regression vs Canary).
   `Entry::Rename` returned `ACCESS_DENIED` whenever the target existed, ignoring `ReplaceIfExists`.
   MKWii's `NANDClose` failed with `rename('banner.bin.nandsafe.tmp' -> 'banner.bin') failed` (`0xC0000022`), i.e. every
   POSIX `rename()`-over-existing (the usual atomic-save pattern) failed. Stock Canary overwrote the file.
   Fix: honor `replace_existing`; without it return `STATUS_OBJECT_NAME_COLLISION`; never replace directories; rename onto
   itself is a no-op; the stale cached entry of the replaced file is dropped; exact `X_STATUS` propagated.
   Covered by a new vfs unit test. Verified in MKWii: `banner.bin` (29 344 B) and `rksys.dat` are now written.
   Commit `fix(vfs): honor ReplaceIfExists in NtSetInformationFile rename`.

## Coverage added

* `instr_lswi`, `instr_lswx`, `instr_stswi`, `instr_stswx`, `instr_mtfsbx` (+ XER byte-count round trip): partial
  words, `r31 -> r0` wrap, zero count, indexed offsets. All pass. (The README features had no tests.)
* vfs rename-replace test (above).

## Test status

| Suite | Result |
|---|---|
| `xenia-vfs-tests` | pass (4 cases, incl. new one) |
| `xenia-kernel-tests` | pass (33 cases) |
| `xenia-cpu-tests` | pass (267 cases) |
| `xenia-base-tests` | 1 failure: `physical_heap_test.cc:173` ("alloc with alignment larger than page_size is rejected"). Code not touched by the fork; not compared against upstream. |
| `xenia-cpu-ppc-tests` (487 suites) | All integer/branch/load/store/atomic suites pass (including `stwcx`/`stdcx` and the five new ones). Failures (28 320 of 170 105 cases) are all in generated FP/VMX suites (`fnmsub*`, `fmadd*`, `vnmsubfp`, `vmaddfp`, `vcmpbfp`, `vsl/vsr`, NaN payload and CR bits after `fcmp`); none relate to code the fork touched. A stock-Canary baseline of the same suite was not built, so this is not proven to be identical. |

## Observed, not changed (needs a decision / hardware to validate)

* **`D1MODE_V_COUNTER` is a constant (960)**, not a progressing raster counter as the README claimed (README corrected).
  `xsdk` `present.c` uses it to count "late" flips and `display.c` waits for it; with a constant, late-flip statistics are
  always 0 and `wait_for_scaler` returns at once ("reached 960 after 0 us"). Pacing on Xenia is therefore not
  representative.
* **`D1GRPH_CONTROL` tiling bit**: `GraphicsSystem::WriteRegister(0x183B)` computes `is_tiled = bit20` but passes it as the
  `is_linear` argument of `IssueDirectSwap`, which sets `fetch.tiled = is_linear ? 0 : 1`. The polarity is inverted
  relative to the variable name; the default register value (bit20 clear) yields a *tiled* fetch. Left as is because the
  tested xsdk titles render correctly; a direct-framebuffer homebrew that sets bit 20 would be read as linear.
* **Reservations are value-compare based** (`lwarx` caches address+value; `stwcx.` is a `lock cmpxchg`): ABA is
  indistinguishable from "unchanged", plain stores from other threads do not clear the reservation, and a stale
  reservation survives until the next `stwcx.`. Fine for GCC spin-locks/counters; can mis-succeed for pointer-based
  lock-free structures.
* `NtFlushBuffersFile` is still a stub (no host flush); `VdQueryVideoMode`, `VdSetDisplayMode`, `XamContentFlush`,
  `NetDll_XNetGetTitleXnAddr` etc. are Canary stubs. All 148 xsdk imports have a handler (the only "missing" names are
  exported variables).
* `addo`-style (OE=1) ALU forms and some SPRs in `mfspr/mtspr` are unimplemented upstream (`XEINSTRNOTIMPLEMENTED`).
* The first-boot `NtCreateFile ... 0xC0000034/0xC000003A` lines in MKWii are expected (files being created).
* MKWii title screen runs at ~1.4 fps in Xenia (software THP video decode in translated code); this matches the ~600 ms
  frame time already documented in `RUN.md` for Canary and is not a fork regression.
* Fork is 50 commits behind upstream `canary_experimental`; a rebase should re-run the suites above.

## Reproduce

```
# build (Release); needs FXC_PATH and VULKAN_SDK set for shader compilation
ninja -f build-Release.ninja
# unit tests
build/bin/Windows/Release/xenia-vfs-tests.exe
# PPC tests (generate bins with powerpc-linux-gnu-as/ld/nm, -a32 -be -mregnames, link at 0x80000000)
build/bin/Windows/Release/xenia-cpu-ppc-tests.exe instr_lswi instr_lswx instr_stswi instr_stswx instr_mtfsbx instr_stwcx
# game
xenia_canary.exe <title.xex> --headless=true --hid=nop --apu=nop --log_file=run.log
```
