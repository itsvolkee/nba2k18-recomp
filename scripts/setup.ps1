<#
  NBA 2K18 (Xbox 360) recompilation - local setup, no Visual Studio needed.

  Uses the tools that GitHub Actions builds for this repo (release "tools-latest"):
  xdvdfs, XenonAnalyse, XenonRecomp and ppc_context.h.

  Steps:
    1. Downloads the prebuilt tools (or uses a zip you downloaded yourself)
    2. Extracts the ISO into private\game   (git-ignored)
    3. Records the SHA-256 of default.xex and a file list in notes\
    4. Runs XenonAnalyse to produce config\NBA2K18_switch_tables.toml
    5. Writes config\NBA2K18.toml (if it doesn't exist yet) and recompile.bat

  Run from the repo folder in a normal PowerShell window:
      powershell -ExecutionPolicy Bypass -File scripts\setup.ps1 -Repo yourname/nba2k18-recomp

  If the repo is private, download xenon-tools-win64.zip from the repo's
  Releases page in your browser and pass it with -ToolsZip instead of -Repo.
#>

param(
    [string]$Iso      = "E:\Gamefiles\NBA 2K18 (USA)\NBA 2K18 (USA).iso",
    [string]$Repo     = "",   # owner/name of this repo on GitHub
    [string]$ToolsZip = "",   # path to an already-downloaded xenon-tools-win64.zip
    [switch]$UpdateTools      # re-download the tools even if they're already present
)

$ErrorActionPreference = "Stop"
$ProgressPreference    = "SilentlyContinue"   # makes Invoke-WebRequest much faster
function Step($msg) { Write-Host "`n==> $msg" -ForegroundColor Cyan }

$Root     = Split-Path -Parent $PSScriptRoot
$ToolsDir = Join-Path $Root "tools"
$GameDir  = Join-Path $Root "private\game"
foreach ($d in $ToolsDir, $GameDir, (Join-Path $Root "notes"), (Join-Path $Root "config"), (Join-Path $Root "ppc")) {
    New-Item -ItemType Directory -Force -Path $d | Out-Null
}
Start-Transcript -Path (Join-Path $Root "notes\setup.log") -Append | Out-Null

# ---------- 0. Preflight ----------
Step "Checking inputs"
if (-not (Test-Path -LiteralPath $Iso)) { throw "ISO not found: $Iso" }
$freeGB = [math]::Round((Get-PSDrive ($Root.Substring(0,1))).Free / 1GB, 1)
if ($freeGB -lt 10) { throw "Only $freeGB GB free on $($Root.Substring(0,2)) - need about 10 GB." }

# ---------- 1. Tools ----------
$xdvdfs  = Join-Path $ToolsDir "xdvdfs.exe"
$analyse = Join-Path $ToolsDir "XenonAnalyse.exe"
$recomp  = Join-Path $ToolsDir "XenonRecomp.exe"
$ppcCtx  = Join-Path $ToolsDir "ppc_context.h"

if ($UpdateTools) {
    Remove-Item -Force -ErrorAction SilentlyContinue $analyse, $recomp, $xdvdfs, $ppcCtx, (Join-Path $ToolsDir "xenon-tools-win64.zip")
}
if (-not (Test-Path $analyse)) {
    if (-not $ToolsZip) {
        if (-not $Repo) { throw "Pass -Repo owner/name (public repo) or -ToolsZip <path to xenon-tools-win64.zip>." }
        Step "Downloading prebuilt tools from github.com/$Repo"
        $ToolsZip = Join-Path $ToolsDir "xenon-tools-win64.zip"
        $url = "https://github.com/$Repo/releases/download/tools-latest/xenon-tools-win64.zip"
        try { Invoke-WebRequest -Uri $url -OutFile $ToolsZip -UseBasicParsing }
        catch { throw "Download failed ($url). If the repo is private, download the zip from its Releases page and use -ToolsZip." }
    }
    Step "Unpacking tools"
    Expand-Archive -LiteralPath $ToolsZip -DestinationPath $ToolsDir -Force
}
foreach ($f in $xdvdfs, $analyse, $recomp, $ppcCtx) {
    if (-not (Test-Path $f)) { throw "Missing from tools zip: $(Split-Path $f -Leaf)" }
}

# ---------- 2. Extract ISO ----------
if (Get-ChildItem -LiteralPath $GameDir -Recurse -Filter "default.xex" -ErrorAction SilentlyContinue) {
    Step "default.xex already extracted - skipping"
} else {
    Step "Extracting ISO (about 8.7 GB, takes several minutes)"
    & $xdvdfs unpack "$Iso" "$GameDir"
    if ($LASTEXITCODE -ne 0) { throw "xdvdfs failed (exit $LASTEXITCODE)." }
}
$xex = Get-ChildItem -LiteralPath $GameDir -Recurse -Filter "default.xex" | Select-Object -First 1
if (-not $xex) { throw "No default.xex found in $GameDir after extraction." }

# ---------- 3. Fingerprint ----------
Step "Hashing default.xex and listing files"
$hash = (Get-FileHash -LiteralPath $xex.FullName -Algorithm SHA256).Hash
@"
Source ISO : $Iso
default.xex: $($xex.FullName)
Size       : $($xex.Length) bytes
SHA-256    : $hash
Extracted  : $(Get-Date -Format s)
Tools      : $(Get-Content (Join-Path $ToolsDir 'BUILD-INFO.txt') -ErrorAction SilentlyContinue | Select-Object -First 1)
"@ | Set-Content -LiteralPath (Join-Path $Root "notes\build-info.txt") -Encoding UTF8
Get-ChildItem -LiteralPath $GameDir -Recurse -File |
    Select-Object @{n="Path";e={$_.FullName.Substring($GameDir.Length+1)}}, Length |
    Sort-Object Path | Export-Csv (Join-Path $Root "notes\file-list.csv") -NoTypeInformation

# ---------- 4. Jump-table analysis ----------
Step "Running XenonAnalyse"
$switchToml = Join-Path $Root "config\NBA2K18_switch_tables.toml"
& $analyse "$($xex.FullName)" "$switchToml" 2>&1 | Tee-Object -FilePath (Join-Path $Root "notes\xenonanalyse.log")
if ($LASTEXITCODE -ne 0) {
    Write-Host "XenonAnalyse exited with $LASTEXITCODE (see notes\xenonanalyse.log). Partial output is normal for games other than Sonic Unleashed." -ForegroundColor Yellow
}

# ---------- 5. Config + recompile.bat ----------
$configPath = Join-Path $Root "config\NBA2K18.toml"
$relXex = "../" + $xex.FullName.Substring($Root.TrimEnd('\').Length + 1).Replace('\','/')
if (Test-Path $configPath) {
    Step "config\NBA2K18.toml already exists - leaving it alone"
} else {
    Step "Writing config\NBA2K18.toml"
@"
# XenonRecomp config for NBA 2K18 (Xbox 360, USA, base disc)
# default.xex SHA-256: $hash
# Paths are relative to this file.

[main]
file_path = "$relXex"
out_directory_path = "../ppc"
switch_table_file_path = "NBA2K18_switch_tables.toml"

# Leave all optimizations off until the recompilation actually runs.
skip_lr = false
skip_msr = false
ctr_as_local = false
xer_as_local = false
reserved_as_local = false
cr_as_local = false
non_argument_as_local = false
non_volatile_as_local = false

# TODO: register save/restore helper addresses. XenonRecomp needs these
# before it will produce usable output. Find them in Ghidra (with an
# Xbox 360 XEX loader) by locating the __savegprlr_14 / __restgprlr_14,
# __savefpr_14 / __restfpr_14 and __savevmx_* / __restvmx_* sequences,
# then uncomment and fill in:
# restgprlr_14_address = 0x00000000
# savegprlr_14_address = 0x00000000
# restfpr_14_address   = 0x00000000
# savefpr_14_address   = 0x00000000
# restvmx_14_address   = 0x00000000
# savevmx_14_address   = 0x00000000
# restvmx_64_address   = 0x00000000
# savevmx_64_address   = 0x00000000

# Optional, if the game uses them:
# longjmp_address = 0x00000000
# setjmp_address  = 0x00000000

# Manual function boundaries for anything the analyzer misses:
# functions = [ { address = 0x00000000, size = 0x0 } ]
# invalid_instructions = [ { data = 0x00000000, size = 4 } ]
"@ | Set-Content -LiteralPath $configPath -Encoding UTF8
}

@"
@echo off
"%~dp0tools\XenonRecomp.exe" "%~dp0config\NBA2K18.toml" "%~dp0tools\ppc_context.h"
"@ | Set-Content -LiteralPath (Join-Path $Root "recompile.bat") -Encoding ASCII

Step "Done"
Write-Host @"
Game files : $GameDir
Executable : $($xex.FullName)
SHA-256    : $hash
Config     : $configPath
Jump tables: $switchToml
Next       : fill in the TODO addresses in config\NBA2K18.toml, then run recompile.bat
"@ -ForegroundColor Green
