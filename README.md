THIS PROJECT IS NOT RELATED TO SHADPS4. DO NOT SEND QUESTIONS ABOUT IT TO THE SHADPS4 SERVER.


# bbport for Windows — a native Windows port of Bloodborne

This is a Windows-focused fork of [bbport](https://github.com/deadinside28/bloodborne_pc) by
deadinside28. bbport runs the original *Bloodborne* executable for PlayStation 4 (CUSA03173,
game version 1.09) natively on an x86-64 PC:

- the game's x86-64 code runs directly on the CPU (no emulation, no instruction translation),
  and a small runtime written for this one game replaces the PS4 system libraries;
- the graphics are translated to Vulkan by a renderer derived from
  [shadPS4](https://github.com/shadps4-emu/shadPS4), extended with temporal upscaling
  (AMD FSR 3.1, FSR 4 and FSR 4.1.1), motion vectors computed for this game, and an
  unlocked frame rate.

Upstream bbport targets **Linux only** (GCC, Nix, GTK4, AppImage). This fork's goal is to
build and run the same port on **Windows 10/11 x64**, staying as close to upstream as possible
so new upstream releases can be merged in.

> **No game files are included.** You need your own decrypted dump of Bloodborne
> (CUSA03173, v1.09). This project is not affiliated with Sony Interactive Entertainment,
> FromSoftware or AMD.

## Status

**Work in progress — not playable on Windows yet.** The loader, runtime and renderer build into
`bb-probe.exe` with MSYS2 Clang, and the game boots into its own initialization on Windows
(its threads, about 5 GB of game memory, audio, shader compilation); it does not reach the first
frame yet. To play today you need Linux (or the Steam Deck) and
[upstream bbport](https://github.com/deadinside28/bloodborne_pc).

For the full feature list, settings, in-game menu, mods, patches and environment variables, see
the upstream README: [English](https://github.com/deadinside28/bloodborne_pc/blob/master/README.md)
· [Русский](README.ru.md). Everything there describes the Linux build.

## Porting plan

What upstream depends on, and what replaces it on Windows:

| Area | Upstream (Linux) | Windows |
|---|---|---|
| Toolchain | GCC, `build.sh`, Nix `shell.nix` | Clang (clang-cl or MinGW-w64 Clang) with CMake + Ninja. MSVC `cl.exe` cannot be used: the game calls the runtime with the System V ABI (`__attribute__((sysv_abi))`, `src/runtime.h`), which MSVC does not support. Dependencies via vcpkg or MSYS2. |
| Guest thread pointer (TLS) | The game's `mov rax, fs:[0]` is rewritten to `gs:[0]` (`scripts/link_modules.py`) and GS is pointed at the guest TCB with `arch_prctl` (`src/runtime_thread.c`) | Windows owns GS (the TEB) and has no user-mode way to set FS/GS. Rewrite the same 9-byte instruction to read a TEB TLS slot instead (`mov rax, gs:[0x1480 + 8*slot]`) and store the guest TCB in that slot per thread. |
| Guest memory | `mmap` with `MAP_FIXED`, `memfd_create` shared backing (`src/runtime_memory.c`), non-PIE loader at fixed addresses (`src/probe.c`) | `VirtualAlloc2` / `MapViewOfFile3` with placeholders (Windows 10 1803+) for fixed-address and aliased mappings; reserve the guest ranges early. |
| Write tracking / faults | `sigaction(SIGSEGV)` + `ucontext_t` | Vectored exception handler (`AddVectoredExceptionHandler`) + `CONTEXT`. |
| Threads and sync | pthreads, `futex`, `gettid`/`tgkill` | Win32 threads, SRW locks and condition variables, `WaitOnAddress` (or winpthreads for a first pass). |
| Window, input, audio | SDL3 (X11 required by `gpu/CMakeLists.txt`) | SDL3 works as is; drop the X11 requirement and use the Win32 Vulkan surface. |
| GPU library | `libbbgpu.so` from the vendored shadPS4 video core | `bbgpu.dll`. shadPS4 itself already builds on Windows, so most of the vendored core should port with small changes. |
| Run script | `run.sh` (bash) calling the Python preparation scripts in `scripts/` | A PowerShell or Python entry point; the scripts in `scripts/` are plain Python. |
| Launcher | GTK4 + libadwaita (Python) | To be decided: GTK4 via MSYS2, or a lightweight native launcher. |
| Packaging | AppImage, Nix package | A portable `.zip` (and possibly an installer later). |
| FSR 4.1.1 assets | Recorded from AMD's DX12 DLLs under GE-Proton / vkd3d-proton (`tools/fsr4cap`) | To be decided. FSR 3.1 needs no assets; FSR 4 v07 assets come from `tools/fetch_fsr4_assets.sh`. |

The game logic, the Python image preparation, the shader recompiler and the FSR integration are
platform-independent and should carry over unchanged.

## Requirements (target)

- Windows 10 (1803 or newer) or Windows 11, x64.
- A Vulkan 1.3 GPU with an up-to-date driver. FSR 4 / 4.1.1 need the extra shader features
  listed upstream; on other GPUs the port falls back to FSR 3.1.
- Your decrypted game dump: the `CUSA03173` folder (eboot.bin, sce_module, ...), version 1.09.
  The base game alone (1.00) crashes at start: copy the dumped 1.09 update over it.

## Building (developers)

1. Install [MSYS2](https://www.msys2.org) and, in its **CLANG64** shell, the toolchain and libraries:

   ```bash
   pacman -S --needed mingw-w64-clang-x86_64-{toolchain,cmake,ninja,pkgconf,python,sdl3,boost,fmt,robin-map,xxhash,vulkan-headers,vulkan-loader,vulkan-memory-allocator,glslang,spirv-cross,spirv-headers,spirv-tools,zydis,ffmpeg} git
   ```

2. Clone with submodules and build (magic_enum, miniz and xbyak are downloaded by CMake):

   ```bash
   git clone --recursive https://github.com/Phroster/bloodborne_pc_windows.git && cd bloodborne_pc_windows
   bash windows/build.sh            # out/bb-probe.exe
   ./out/bb-probe.exe --vulkan-only # Vulkan check, no game files needed
   ```

3. Check your dump before running anything: damaged archives stop the game while it loads.

   ```bash
   python windows/check_dump.py "/path/to/CUSA03173"
   ```

There is no Windows launcher or run script yet. **Do not use upstream's `run.sh` on Windows:**
its cleanup of the merged mod folder compares paths that Windows spells two ways and can delete
the game folder.

How the port works: [windows/compat](windows/compat) implements the Linux functions the code
uses (memory mapping with Windows placeholders, signals on a vectored exception handler, files,
threads), so upstream sources compile almost unchanged; the few Windows-specific edits in
upstream files are marked `bbport-windows:`.

**Red zones:** the game's leaf functions keep data in the 128 bytes below the stack pointer,
and some Windows builds write exception records there when a GPU write-tracking fault hits
such a function. At startup the loader probes whether this Windows does; if so, it reroutes
the game's faultable memory accesses through trampolines that step past the red zone
([windows/redzone.cpp](windows/redzone.cpp), ported from shadPS4's
[#4802](https://github.com/shadps4-emu/shadPS4/pull/4802)). It needs the function list from
`python windows/function_starts.py <game folder> --out out` (after the image scripts);
`BB_RED_ZONE=1` forces the patching, `=0` turns it off.

## Relationship to upstream

- Upstream: [deadinside28/bloodborne_pc](https://github.com/deadinside28/bloodborne_pc). All
  credit for bbport itself goes to its author; questions about the port in general go to the
  upstream project's Discord (linked in its README).
- This fork merges upstream releases and keeps Windows changes small and isolated, so they can
  be offered back upstream.
- To update a local clone from upstream:

  ```bash
  git remote add upstream https://github.com/deadinside28/bloodborne_pc.git
  git fetch upstream
  git merge upstream/master
  ```

  This README differs from upstream's, so it may conflict on merge; keep this fork's version.

## Repository layout

| Path | Contents |
|---|---|
| `src/` | Loader (`probe.c`) and the HLE runtime that replaces the PS4 system libraries |
| `scripts/` | Offline preparation of the game image, module linking, patch compiler (Python) |
| `gpu/` | Renderer library: vendored shadPS4 video core with this port's changes (`gpu/VENDOR.txt`), ImGui menu, FSR runtime |
| `launcher/`, `packaging/` | Upstream's GTK4 launcher; Nix package and AppImage |
| `patches/` | Community patches for Bloodborne |
| `tools/` | Developer tools: scripted runs, FSR helpers, `fsr4cap` |
| `tests/` | Loader, runtime, patch and renderer tests |
| `docs/` | Upstream design notes ([upscaler](docs/upscaler.md), [parallel GPU](docs/parallel_gpu.md), [motion vectors](docs/motion_vectors.md), [roadmap](docs/ROADMAP.md)) |

## Credits and licenses

Licensed under the **GNU GPL v2 or later** ([LICENSE](LICENSE)), as upstream bbport, which
contains code from shadPS4 (GPL-2.0-or-later).

- [bbport](https://github.com/deadinside28/bloodborne_pc) by deadinside28: the port this fork is
  based on.
- [shadPS4](https://github.com/shadps4-emu/shadPS4): video core and shader recompiler (GPL-2.0+),
  [sirit](https://github.com/shadps4-emu/sirit).
- [FSR-Vulkan](https://github.com/FireBurn/FSR-Vulkan) by FireBurn (MIT), AMD FidelityFX SDK (MIT),
  [LibAtrac9](https://github.com/Thealexbarney/LibAtrac9) (MIT),
  [Dear ImGui](https://github.com/ocornut/imgui) (MIT), [half](https://half.sourceforge.net/),
  DejaVu fonts, [dxil-spirv](https://github.com/HansKristian-Work/dxil-spirv) (MIT).
- Game patches by Kyo, Lance McDonald, auser1337, illusion, emoose and other community members
  (`patches/Bloodborne.xml`).

AMD's FSR 4 DLLs and model data are not distributed here.
