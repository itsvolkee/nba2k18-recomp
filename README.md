# NBA 2K18 (Xbox 360) static recompilation

Work-in-progress setup for recompiling the Xbox 360 version of NBA 2K18 to a
native Windows program with [XenonRecomp](https://github.com/hedge-dev/XenonRecomp).

**This repository contains no game code or assets.** You need your own copy of
the game. Everything taken from the disc, and all C++ generated from it, lives
in `private/` and `ppc/`, which are git-ignored and never leave your PC.

## How it works

1. **GitHub Actions builds the tools.** The `Build Xenon tools` workflow
   compiles xdvdfs, XenonAnalyse, XenonRecomp (with this project's patches)
   and XexDump for Windows x64, bundles the simde headers, and publishes them
   as the `tools-latest` release. A second job mirrors the latest llvm-mingw,
   CMake and Ninja releases into a `toolchain` release with SHA-256 hashes.
2. **Setup runs on your PC.** `scripts/setup.ps1` downloads the tools,
   extracts the ISO into `private/game` and fingerprints `default.xex`.
3. **The build runs on your PC.** `scripts/build.ps1` downloads the portable
   toolchain (checking every hash), writes the decrypted image with XexDump,
   generates the game's C++ into `ppc/` with XenonRecomp, and compiles it
   together with `runtime/` into `NBA2K18.exe`. No Visual Studio needed.

## Usage

Double-click, in order: `run-setup.cmd`, `build.cmd`, `run.cmd`.

Or from PowerShell in the repo folder:

```powershell
powershell -ExecutionPolicy Bypass -File scripts\setup.ps1 -Repo itsvolkee/nba2k18-recomp
powershell -ExecutionPolicy Bypass -File scripts\build.ps1
.\NBA2K18.exe private\game
```

`setup.ps1 -UpdateTools` re-downloads the latest tool build; `build.ps1 -Regenerate`
forces the C++ to be regenerated. Logs go to `notes\` and `nba2k18.log`.

## Layout

| Path | In git | Contents |
|---|---|---|
| `.github/workflows/` | yes | Tool and toolchain builds |
| `patches/` | yes | XenonRecomp changes: extra instructions, switch-aware analysis |
| `analysis/` | yes | Jump-table detector, pointer-reached function scanner, XexDump |
| `config/` | yes | Recompiler config, function list and jump tables (addresses only) |
| `runtime/` | yes | Host program: memory, loader, threads, kernel/XAM calls, files |
| `scripts/` | yes | Setup and build scripts |
| `tools/` | no | Downloaded tools and toolchain |
| `private/game/` | no | Extracted disc contents and decrypted image |
| `ppc/` | no | Generated C++ |
| `build/`, `notes/` | no | Build output and logs |

## Status

Recompiler:
- [x] Register save/restore helper addresses
- [x] 60 PowerPC opcodes added (`patches/`); 8 unrecognized remain, all `mullhw`,
      which the Xbox 360 CPU doesn't have (data words inside `.text`)
- [x] Jump tables: `analysis/jt_detect.py` finds 1,057 tables / 40,383 cases (stock XenonAnalyse finds 0)
- [x] Switch-aware function analysis: "switch case outside function" errors 21,424 → 0
- [x] Pointer-reached functions: `analysis/fn_ptrs.cpp` adds 2,640 functions only reached
      through data pointers or addresses built in code; 84,621 functions in total
- [ ] setjmp/longjmp addresses, if the game uses them
- [ ] Review the 668 jump tables that have no bounds check (sized from the table data)

Runtime (`runtime/`):
- [x] Guest memory, XEX loader, function table, guest threads with PCR/TLS
- [x] 111 of 318 imports: memory, sync objects, waits, threads, TLS, time, strings,
      printf family, file system; the rest log a warning when called
- [x] Windows build (`scripts/build.ps1`): NBA2K18.exe builds on the PC with llvm-mingw
- [x] Gets through startup on Windows: 10 threads, opens all data archives (0A-0H),
      queries controllers and video mode, starts GPU initialisation
- [x] Graphics stand-in (`runtime/gpu_stub.cpp`): 60 Hz vsync interrupts, ring buffer
      read-pointer writeback, video-mode queries, DPCs. The game stays alive and
      receives vsyncs
- [ ] Stalls after its first 31 dwords of GPU commands, 0 frames: it most likely
      waits for a fence or command-processor interrupt in those commands. Next: a
      minimal PM4 command processor (memory/event writes, interrupts, indirect buffers)
- [ ] Graphics (Xenos → D3D12/Vulkan), audio, input, XAM/profiles, networking
