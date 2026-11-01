<#
Builds a Release ClipDeck, deploys the Qt runtime next to it and wraps it in an
Inno Setup installer (windows/installer/clipdeck.iss).

  powershell -ExecutionPolicy Bypass -File windows\scripts\package-windows.ps1 `
    -QtDir D:\Qt\6.12.0\mingw_64 -SodiumRoot D:\libsodium-win64 -InnoSetupDir D:\InnoSetup6

The MinGW toolchain (g++) must be on PATH, or passed with -MinGWBin. Output:
<BuildDir>\installer\ClipDeck-<version>-windows-x64-setup.exe
#>
param(
    [Parameter(Mandatory)] [string] $QtDir,
    [Parameter(Mandatory)] [string] $SodiumRoot,
    [string] $InnoSetupDir = "${env:LOCALAPPDATA}\Programs\Inno Setup 6",
    [string] $MinGWBin = "",
    [string] $BuildDir = ""
)
$ErrorActionPreference = 'Stop'

$repo = (Resolve-Path "$PSScriptRoot\..\..").Path
if (-not $BuildDir) { $BuildDir = Join-Path $repo 'windows\.build\release' }
if ($MinGWBin) { $env:PATH = "$MinGWBin;$env:PATH" }
$env:PATH = "$QtDir\bin;$env:PATH"

function Invoke-Checked([string] $exe, [string[]] $arguments) {
    & $exe @arguments
    if ($LASTEXITCODE -ne 0) { throw "$exe failed with exit code $LASTEXITCODE" }
}

# The installer version follows project(... VERSION x.y.z) in windows/CMakeLists.txt.
$cmakeLists = Get-Content (Join-Path $repo 'windows\CMakeLists.txt') -Raw
if ($cmakeLists -notmatch 'project\(ClipDeckWindows VERSION ([0-9.]+)') { throw 'No project version in windows/CMakeLists.txt' }
$version = $Matches[1]

Invoke-Checked cmake @('-S', (Join-Path $repo 'windows'), '-B', $BuildDir, '-G', 'Ninja',
    '-DCMAKE_BUILD_TYPE=Release', "-DCMAKE_PREFIX_PATH=$QtDir", "-DSODIUM_ROOT=$SodiumRoot")
Invoke-Checked cmake @('--build', $BuildDir, '--target', 'clipdeck')

$stage = Join-Path $BuildDir 'package\ClipDeck'
if (Test-Path $stage) { Remove-Item -Recurse -Force $stage }
New-Item -ItemType Directory -Force $stage | Out-Null
Copy-Item (Join-Path $BuildDir 'clipdeck.exe') $stage

# The software OpenGL fallback and D3D compiler are not needed: Qt Quick uses D3D11.
Invoke-Checked windeployqt @('--release', '--qmldir', (Join-Path $repo 'windows\qml'), '--no-translations',
    '--no-opengl-sw', '--no-system-d3d-compiler', '--compiler-runtime', (Join-Path $stage 'clipdeck.exe'))

# windeployqt only finds the MinGW runtime when g++ is on PATH; check it is there.
foreach ($dll in 'libgcc_s_seh-1.dll', 'libstdc++-6.dll', 'libwinpthread-1.dll') {
    if (-not (Test-Path (Join-Path $stage $dll))) {
        $gpp = (Get-Command g++ -ErrorAction SilentlyContinue).Source
        if (-not $gpp) { throw "Missing $dll and no g++ on PATH (pass -MinGWBin)" }
        Copy-Item (Join-Path (Split-Path $gpp) $dll) $stage
    }
}

$iscc = Join-Path $InnoSetupDir 'ISCC.exe'
if (-not (Test-Path $iscc)) { throw "Inno Setup 6 not found at $InnoSetupDir (pass -InnoSetupDir)" }
$output = Join-Path $BuildDir 'installer'
Invoke-Checked $iscc @('/Qp', "/DAppVersion=$version", "/DStageDir=$stage", "/DOutputDir=$output",
    (Join-Path $repo 'windows\installer\clipdeck.iss'))

$setup = Join-Path $output "ClipDeck-$version-windows-x64-setup.exe"
Write-Host "Installer: $setup ($([math]::Round((Get-Item $setup).Length / 1MB, 1)) MB)"
