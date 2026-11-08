<p align="center">
    <a href="https://github.com/xenia-canary/xenia-canary">
        <img height="200px" src="https://raw.githubusercontent.com/xenia-canary/xenia/master/assets/icon/256.png" alt="XSDK Xenia Logo" />
    </a>
</p>

<h1 align="center">XSDK Xenia - Xbox 360 Emulator Fork</h1>

<p align="center">
    Compatibility and rapid debugging layer for open-source Xbox 360 ports and homebrew targeting <code>xsdk</code>, <code>vulkan4xenon</code>, and <code>SDL-xenon</code>.
</p>

---

## Important Notice

This is a **highly experimental** fork of [Xenia Canary](https://github.com/xenia-canary/xenia-canary).

Its performance is **not comparable to a physical Xbox 360 console**. This project is intended strictly as a **rapid debugging and development environment** on PC for games, homebrew software, and ports built with the open-source Xbox 360 toolchain ecosystem:

* Open-source Xbox 360 SDK: [xsdk](https://github.com/xenosworks/xsdk)
* Vulkan driver/translation layer for Xenon: [vulkan4xenon](https://github.com/xenosworks/vulkan4xenon)
* Multimedia, windowing, and input framework: [SDL-xenon](https://github.com/xenosworks/SDL-xenon)

---

## Overview and Purpose

When porting modern game engines and homebrew applications to the Xbox 360 architecture using open-source toolchains (`powerpc-xenon-gcc`, `newlib`, `pthread`, `vulkan4xenon`, and `SDL-xenon`), developers encounter roadblocks in standard emulators designed primarily for retail MSVC-compiled titles:

1. **GCC Atomic Deadlocks**: Upstream emulators assume Microsoft Visual C++ code generation patterns for atomic operations (`lwarx`/`stwcx.`). GCC-generated atomic loops (`__atomic_compare_exchange`, `pthread_mutex`, lockless single-producer single-consumer queues) frequently execute `lwarx`, compare values, and branch out without emitting `stwcx.`. In vanilla Xenia, abandoned reservations trigger debug breaks and permanently leak reservation bits in the 64 KB block lock, causing 100% CPU thread deadlocks.
2. **Missing PowerPC String Word Instructions**: Standard memory routines in `newlib` and `xsdk` optimize block transfers using PowerPC string instructions (`lswi`, `lswx`, `stswi`, `stswx`) and the byte count field in `XER[57:63]`, which were previously unimplemented.
3. **GPU Readback Desynchronization**: `vulkan4xenon` relies on bidirectional CPU/GPU transfers (`readback_resolve` and `readback_memexport`). Without explicit validation of CPU-modified ranges in shared memory, subsequent GPU reads reload stale cache lines.
4. **Unimplemented AVIVO Display Controller Registers**: Applications using `SDL-xenon` or direct framebuffer presentation poll AVIVO registers (`DC_LUT_AUTOFILL`, `D1MODE_V_COUNTER`, `D1CRTC_DOUBLE_BUFFER_CONTROL`) and GPU performance counters (`RBBM_PERFCOUNTER`), hanging indefinitely when these registers remain unhandled.

**XSDK Xenia** implements the required compatibility layers and diagnostics to eliminate these bottlenecks, enabling fast local turnaround times on PC without requiring physical hardware deployment during early port development.

---

## Implemented Features

### 1. PowerPC CPU and x64 JIT Backend

* **Non-Blocking GCC Reservation Management**:
  Refactored `EmitTryAcquireReservationHelper` and `EmitReservedStoreHelper` in `src/xenia/cpu/backend/x64/x64_backend.cc`. Reservations are now tracked per thread via `cached_reserve_bit` using the exact guest address. Abandoned `lwarx` instructions overwrite the calling thread's reservation cleanly without locking out other threads, and `stwcx.` / `stdcx.` execute atomic comparisons using host `lock cmpxchg`.
* **String Word Instruction Set**:
  Implemented `lswi`, `lswx`, `stswi`, and `stswx` in `src/xenia/cpu/ppc/ppc_emit_memory.cc` and `src/xenia/cpu/ppc/ppc_frontend.cc`. Added tracking for `XER[57:63]` (`xer_byte_count`) across `LoadXER` and `StoreXER` in `src/xenia/cpu/ppc/ppc_hir_builder.cc`.
* **FPU Control Instructions**:
  Implemented `mtfsb0x` and `mtfsb1x` for floating-point status register bit operations.
* **Deterministic ASLR-Invariant Address Translation**:
  Standardized `PPCContext::TranslateVirtual` and `PPCContext::HostToGuestVirtual` in `src/xenia/cpu/ppc/ppc_context.h` to compare against a fixed `0xE0000000` base, preventing host stack address collisions.

### 2. Xenos GPU, AVIVO Display, and Shared Memory

* **Bidirectional Readback Coherence for vulkan4xenon**:
  Exposed `SharedMemory::MakeRangeValid` in `src/xenia/gpu/shared_memory.h` and invoked it after CPU-side writes during `readback_resolve` and `readback_memexport` in both Direct3D 12 and Vulkan command processors. This ensures sequences such as `vkCmdCopyImageToBuffer` followed by `vkCmdCopyBufferToImage` read freshly synchronized data.
* **Optimized Default Debug Flags**:
  Default configuration tuned for open-source development: `readback_resolve = "full"`, `readback_memexport = true`, `occlusion_query_viz = true`, and `async_shader_compilation = false`.
* **AVIVO Display Controller Emulation**:
  * Emulated `DC_LUT_AUTOFILL` (`0x1928`) with bit 1 (`AUTOFILL_DONE`).
  * `D1MODE_V_COUNTER` (`0x194C`) reads as a constant `960` (always past vblank), so polling loops terminate immediately; it does not progress with the raster (see `docs/xsdk_compat_analysis.md`).
  * Added direct frontbuffer presentation support via `D1CRTC_DOUBLE_BUFFER_CONTROL` (`0x183B` -> `IssueDirectSwap`) for homebrew rendering directly to memory.
* **48-Bit 500 MHz GPU Performance Counter (`RBBM_PERFCOUNTER`)**:
  Implemented `RBBM_PERFCOUNTER1_LOW` (`0x0399`) and `RBBM_PERFCOUNTER1_HI` (`0x039A`) across both MMIO register access and PM4 `REG_TO_MEM` packets, used by `vulkan4xenon` for `vkGetQueryPoolResults` timestamps.
* **Cache Invalidation on Synchronization Events**:
  Triggered cache invalidations (`TracePlaybackWroteMemory`) when `SURFACE_SYNC` (`CP_COHER_CNTL`) packets are processed in `CommandProcessor::MakeCoherent`.

### 3. Kernel, VFS, and Port Diagnostics

* **POSIX VFS Overwrite Support**:
  In `src/xenia/vfs/devices/host_path_file.cc`, file handles are explicitly closed prior to deletion, allowing `rename()` overwrites (`NtSetInformationFile`) to succeed on Windows host filesystems.
* **Vectored Exception Handling**:
  Implemented `RtlSetVectoredExceptionHandler` and `RtlClearVectoredExceptionHandler` in `src/xenia/kernel/xboxkrnl/xboxkrnl_rtl.cc`.
* **Network and Sockets**:
  Implemented `NetDll_getpeername`, real DNS address resolution via `getaddrinfo` in `NetDll_XNetDnsLookup`, and socket family endian conversions in `src/xenia/kernel/xam/xam_net.cc`.
* **Automated Debugging Features**:
  * Default `allow_game_relative_writes = true`.
  * Removed modal startup message boxes (`MessageBoxW`) to support headless, continuous integration, and script-driven execution.
  * Added `--exit_on_crash` flag for automated test harnesses.
  * Enhanced crash exception handler with decoded registers (`LR`, `CTR`, `CR`, `FPSCR`), virtual/physical fault resolution, and a **symbolic PowerPC stack backtrace** walking guest stack frames.

---

## Supported Ecosystem

This fork is designed to work in conjunction with:

* **xsdk**: [https://github.com/xenosworks/xsdk](https://github.com/xenosworks/xsdk)
* **vulkan4xenon**: [https://github.com/xenosworks/vulkan4xenon](https://github.com/xenosworks/vulkan4xenon)
* **SDL-xenon**: [https://github.com/xenosworks/SDL-xenon](https://github.com/xenosworks/SDL-xenon)

---

## Building from Source

### Prerequisites (Windows x64)

* Visual Studio 2022 or Build Tools with C++ toolchain (MSVC x64)
* [CMake](https://cmake.org/) (3.25 or newer)
* [Ninja](https://ninja-build.org/)
* [Python](https://www.python.org/) (3.10 or newer)
* [Vulkan SDK](https://vulkan.lunarg.com/) (with `glslangValidator`, `spirv-tools`, and Windows SDK `fxc.exe`)

### Compilation Steps

```powershell
# Clone the repository
git clone https://github.com/<YOUR_USER>/<YOUR_FORK>.git
cd "XSDK Xenia"

# Build the Release executable (xenia-app)
python xenia-build.py build --config=Release --target=xenia-app

# Run the test suites
.\build\bin\Windows\Release\xenia-cpu-tests.exe
.\build\bin\Windows\Release\xenia-kernel-tests.exe
.\build\bin\Windows\Release\xenia-vfs-tests.exe
```

---

## Running and Debugging Ports

To launch an executable (`.xex`) compiled with `xsdk` or `vulkan4xenon`:

```powershell
.\build\bin\Windows\Release\xenia_canary.exe "path\to\your_game.xex" --exit_on_crash
```

For applications utilizing multi-threaded graphics (e.g. `BT3_GS=gpu` and `BT3_GS_THREAD=1`), XSDK Xenia handles inter-thread atomic queues and GPU memory export without deadlocks.

---

## GitHub Actions Continuous Integration

A GitHub Actions workflow is provided at `.github/workflows/release.yml`. It automatically builds Windows x64 Release binaries on pushes to `master`, `main`, or `canary_experimental`, packages the executable into `xsdk_xenia_windows_x64.zip`, and creates a GitHub Release with the attached archive.

---

## License

This project is licensed under the BSD 3-Clause License. See the [LICENSE](LICENSE) file for details.
All upstream copyrights remain with their respective authors.
