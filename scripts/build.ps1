<#
  NBA 2K18 recompiled - build NBA2K18.exe on this PC (no Visual Studio needed).

  Run setup.ps1 first (it extracts the disc and downloads the Xenon tools).
  Then, from the repo folder:
      powershell -ExecutionPolicy Bypass -File scripts\build.ps1
  or double-click build.cmd.

  What it does:
    1. Downloads the portable toolchain (llvm-mingw, CMake, Ninja) from this
       repo's "toolchain" release, checks each SHA-256, unpacks into tools\
    2. Writes private\game\default.img (the decrypted executable) with XexDump
    3. Generates the game's C++ into ppc\ with XenonRecomp
    4. Compiles runtime\ + ppc\ into NBA2K18.exe (first build takes a while)

  The game's code and the generated C++ never leave this PC.
#>
param(
    [string]$Repo = "itsvolkee/nba2k18-recomp",
    [int]$Jobs = 0,              # parallel compile jobs (0 = number of CPU cores)
    [switch]$Regenerate          # force XenonRecomp to regenerate ppc\
)

$ErrorActionPreference = "Stop"
$ProgressPreference    = "SilentlyContinue"
function Step($msg) { Write-Host "`n==> $msg" -ForegroundColor Cyan }

$Root     = Split-Path -Parent $PSScriptRoot
$Tools    = Join-Path $Root "tools"
$GameDir  = Join-Path $Root "private\game"
$PpcDir   = Join-Path $Root "ppc"
$BuildDir = Join-Path $Root "build"
New-Item -ItemType Directory -Force -Path $Tools, (Join-Path $Root "notes") | Out-Null
Start-Transcript -Path (Join-Path $Root "notes\build.log") -Append | Out-Null

# ---------- 0. Preflight ----------
Step "Checking inputs"
$xex = Join-Path $GameDir "default.xex"
foreach ($f in $xex, (Join-Path $Tools "ppc_context.h")) {
    if (-not (Test-Path $f)) { throw "Missing $f. Run scripts\setup.ps1 first." }
}
if (-not (Test-Path (Join-Path $Tools "XenonRecomp.exe"))) {
    throw ("tools\XenonRecomp.exe is missing, but the other tools from the same download are there. " +
           "Antivirus software may have quarantined it: check Windows Security > Virus & threat protection > " +
           "Protection history, restore it if it's listed, then run build.cmd again. " +
           "Or run: scripts\setup.ps1 -UpdateTools to download it again.")
}
if (-not (Test-Path (Join-Path $Tools "XexDump.exe")) -or -not (Test-Path (Join-Path $Tools "simde\x86\sse.h"))) {
    Step "Tools are older than this script (no XexDump/simde): updating them"
    & powershell -NoProfile -ExecutionPolicy Bypass -File (Join-Path $PSScriptRoot "setup.ps1") -Repo $Repo -UpdateTools
    if ($LASTEXITCODE -ne 0) { throw "Updating the tools failed; see notes\setup.log" }
    if (-not (Test-Path (Join-Path $Tools "XexDump.exe"))) { throw "XexDump.exe still missing after updating the tools." }
}
$freeGB = [math]::Round((Get-PSDrive ($Root.Substring(0,1))).Free / 1GB, 1)
if ($freeGB -lt 8) { throw "Only $freeGB GB free on $($Root.Substring(0,2)) - the build needs about 8 GB." }
if ($Jobs -le 0) { $Jobs = [Environment]::ProcessorCount }

# ---------- 1. Toolchain ----------
$toolchain = @{
    "llvm-mingw.zip" = (Join-Path $Tools "llvm-mingw")
    "cmake.zip"      = (Join-Path $Tools "cmake")
    "ninja.zip"      = (Join-Path $Tools "ninja")
}
$base = "https://github.com/$Repo/releases/download/toolchain"
$needed = $toolchain.Keys | Where-Object { -not (Test-Path $toolchain[$_]) }
if ($needed) {
    Step "Downloading the portable toolchain (about 250 MB, once)"
    # Save the hash list to a file: GitHub serves it as binary, and Windows
    # PowerShell 5.1 then hands back bytes instead of text from .Content.
    $sumsFile = Join-Path $Tools "SHA256SUMS"
    Invoke-WebRequest -Uri "$base/SHA256SUMS" -OutFile $sumsFile -UseBasicParsing
    $sums = @{}
    Get-Content -LiteralPath $sumsFile | ForEach-Object {
        $parts = $_.Trim() -split "\s+"
        if ($parts.Count -eq 2) { $sums[$parts[1].TrimStart('*')] = $parts[0].ToUpper() }
    }
    if ($sums.Count -eq 0) { throw "Could not read any hashes from $base/SHA256SUMS." }
    foreach ($name in $needed) {
        $zip = Join-Path $Tools $name
        Write-Host "  $name"
        Invoke-WebRequest -Uri "$base/$name" -OutFile $zip -UseBasicParsing
        $hash = (Get-FileHash -LiteralPath $zip -Algorithm SHA256).Hash
        if (-not $sums.ContainsKey($name) -or $sums[$name] -ne $hash) {
            Remove-Item $zip -Force
            throw "$name failed its SHA-256 check (got $hash). Not using it."
        }
        $tmp = Join-Path $Tools ("_unpack_" + [IO.Path]::GetFileNameWithoutExtension($name))
        if (Test-Path $tmp) { Remove-Item -Recurse -Force $tmp }
        Expand-Archive -LiteralPath $zip -DestinationPath $tmp -Force
        # the zips contain a single top-level folder (except ninja); flatten it
        $inner = Get-ChildItem $tmp
        if ($inner.Count -eq 1 -and $inner[0].PSIsContainer) { Move-Item $inner[0].FullName $toolchain[$name] }
        else { Move-Item $tmp $toolchain[$name] }
        if (Test-Path $tmp) { Remove-Item -Recurse -Force $tmp }
        Remove-Item $zip -Force
    }
}
$clang = Join-Path $Tools "llvm-mingw\bin\clang++.exe"
$cmake = Join-Path $Tools "cmake\bin\cmake.exe"
$ninja = Join-Path $Tools "ninja\ninja.exe"
foreach ($f in $clang, $cmake, $ninja) { if (-not (Test-Path $f)) { throw "Toolchain incomplete: $f missing. Delete tools\llvm-mingw, tools\cmake and tools\ninja and run again." } }

# ---------- 2. Decrypted image ----------
$img = Join-Path $GameDir "default.img"
if (-not (Test-Path $img)) {
    Step "Writing the decrypted executable image"
    & (Join-Path $Tools "XexDump.exe") "$xex" "$img"
    if ($LASTEXITCODE -ne 0) { throw "XexDump failed." }
}

# ---------- 3. Generate C++ ----------
$config = Join-Path $Root "config\NBA2K18.toml"
$stamp  = Join-Path $PpcDir "ppc_recomp_shared.h"
$done   = Join-Path $PpcDir ".complete"       # written only after a verified, complete generation
# Fingerprint of everything the generated code depends on. Hashes, not file
# dates: the tools unpacked from the GitHub zip carry timestamps hours ahead.
$inputs = @($config, (Join-Path $Root "config\NBA2K18_switch_tables.toml"), (Join-Path $Tools "XenonRecomp.exe"), $xex)
$fingerprint = ($inputs | ForEach-Object { (Get-FileHash -LiteralPath $_ -Algorithm SHA256).Hash }) -join ","
$stale  = $Regenerate -or -not (Test-Path $stamp) -or -not (Test-Path $done) -or
          ((Get-Content -LiteralPath $done -TotalCount 1) -ne $fingerprint)
if ($stale) {
    Step "Generating the game's C++ (about a minute)"
    if (Test-Path $PpcDir) { Remove-Item -Recurse -Force $PpcDir }
    New-Item -ItemType Directory -Force -Path $PpcDir | Out-Null
    # Run it as a separate process with real output files: PowerShell 5.1's `*>`
    # redirection of a native program captured nothing and the output was cut short.
    $log = Join-Path $Root "notes\recompile.log"
    $err = Join-Path $Root "notes\recompile.err.log"
    $p = Start-Process -FilePath (Join-Path $Tools "XenonRecomp.exe") `
        -ArgumentList "`"$config`"", "`"$(Join-Path $Tools 'ppc_context.h')`"" `
        -WorkingDirectory $Root -NoNewWindow -Wait -PassThru `
        -RedirectStandardOutput $log -RedirectStandardError $err
    $out = (Get-Content -LiteralPath $log -Raw) + (Get-Content -LiteralPath $err -Raw)
    if ($p.ExitCode -ne 0) { throw "XenonRecomp exited with code $($p.ExitCode); see notes\recompile.log and notes\recompile.err.log" }
    if ($out -notmatch "100%") { throw "XenonRecomp stopped before finishing (no 100% in its output); see notes\recompile.log" }
    $files = (Get-ChildItem $PpcDir -Filter 'ppc_recomp.*.cpp').Count
    $unrec = ([regex]::Matches($out, "Unrecognized instruction")).Count
    $switchErr = ([regex]::Matches($out, "jump outside function")).Count
    Write-Host "  $files files, $unrec unrecognized instructions, $switchErr switch errors"
    if ($files -lt 200) { throw "Only $files generated files; expected about 330. See notes\recompile.log" }
    @($fingerprint, "$files files, $unrec unrecognized, $switchErr switch errors, $(Get-Date -Format s)") | Set-Content -LiteralPath $done
}

# ---------- 4. Compile ----------
Step "Compiling NBA2K18.exe with $Jobs jobs (the first build can take an hour or more)"
$env:PATH = (Join-Path $Tools "llvm-mingw\bin") + ";" + (Join-Path $Tools "ninja") + ";" + $env:PATH
& $cmake -S (Join-Path $Root "runtime") -B $BuildDir -G Ninja `
    "-DCMAKE_MAKE_PROGRAM=$ninja" "-DCMAKE_CXX_COMPILER=$clang" -DCMAKE_BUILD_TYPE=Release `
    "-DPPC_DIR=$PpcDir" "-DSIMDE_DIR=$(Join-Path $Tools 'simde')"
if ($LASTEXITCODE -ne 0) { throw "CMake configure failed." }
& $cmake --build $BuildDir -j $Jobs
if ($LASTEXITCODE -ne 0) { throw "Build failed; the error is above (also in notes\build.log)." }

Copy-Item (Join-Path $BuildDir "NBA2K18.exe") (Join-Path $Root "NBA2K18.exe") -Force
Step "Done"
Write-Host "NBA2K18.exe is in $Root. Double-click run.cmd to start it; it writes nba2k18.log next to it." -ForegroundColor Green
Stop-Transcript | Out-Null
