# NBA 2K18 (Xbox 360) static recompilation

Work-in-progress setup for recompiling the Xbox 360 version of NBA 2K18 to
Windows with [XenonRecomp](https://github.com/hedge-dev/XenonRecomp).

**This repository contains no game code or assets.** You need your own copy of
the game. Everything taken from the disc, and all C++ generated from it, lives
in `private/` and `ppc/`, which are git-ignored and never leave your PC.

## How it works

1. **GitHub Actions builds the tools.** The `Build Xenon tools` workflow
   compiles xdvdfs, XenonAnalyse and XenonRecomp for Windows x64 and publishes
   them as the `tools-latest` release (`xenon-tools-win64.zip`). Nothing needs
   to be installed locally.
2. **You run them locally.** `scripts/setup.ps1` downloads that zip, extracts
   the ISO, fingerprints `default.xex`, runs the jump-table analysis and writes
   a starter config.

## Usage

```powershell
# from the repo folder, in a normal PowerShell window
powershell -ExecutionPolicy Bypass -File scripts\setup.ps1 -Repo <owner>/<repo>

# private repo: download xenon-tools-win64.zip from Releases, then
powershell -ExecutionPolicy Bypass -File scripts\setup.ps1 -ToolsZip C:\path\to\xenon-tools-win64.zip

# different ISO location
... -Iso "D:\path\to\NBA 2K18 (USA).iso"
```

To rebuild the tools against a different XenonRecomp commit: Actions →
Build Xenon tools → Run workflow → enter a branch, tag or commit.

## Layout

| Path | In git | Contents |
|---|---|---|
| `.github/workflows/` | yes | Tool build |
| `scripts/` | yes | Local setup |
| `config/` | yes | Recompiler config and jump tables (addresses only) |
| `tools/` | no | Downloaded prebuilt tools |
| `private/game/` | no | Extracted disc contents |
| `ppc/` | no | Generated C++ |
| `notes/` | no | Build fingerprint, file list, analysis log |

## Status

- [x] Tool build in CI
- [x] Extraction and first analysis pass
- [ ] Register save/restore helper addresses in `config/NBA2K18.toml`
- [ ] Jump tables and function boundaries the analyzer misses
- [ ] First clean XenonRecomp run
- [ ] Runtime: kernel/XAM, file system, input, audio, Xenos GPU → D3D12/Vulkan
