# Copies the built mod into the game's binary folder.
param(
    [string]$GamePath = "C:\Program Files (x86)\Steam\steamapps\common\StreetFighterV"
)
$ErrorActionPreference = "Stop"
Set-Location $PSScriptRoot

$target = Join-Path $GamePath "StreetFighterV\Binaries\Win64"
if (-not (Test-Path (Join-Path $target "StreetFighterV.exe"))) {
    throw "StreetFighterV.exe was not found in $target. Pass -GamePath with the game folder."
}
$source = if (Test-Path "build\xinput1_3.dll") { Join-Path $PSScriptRoot "build" } else { $PSScriptRoot }
if (-not (Test-Path (Join-Path $source "xinput1_3.dll"))) {
    throw "xinput1_3.dll is missing. Run .\build.ps1 first."
}

# A DLL already there is not the game's: back it up rather than overwrite
# someone else's proxy (SF5DInput uses this exact name for DirectInput). The
# marker means the file is this mod's own, from an earlier version. Accept the
# old marker so an SSFV update does not back up its own DLL.
$existing = Join-Path $target "xinput1_3.dll"
$marker = Join-Path $target "xinput1_3.dll.ssfv"
$legacyMarker = Join-Path $target "xinput1_3.dll.sfvaccess"
if ((Test-Path $existing) -and -not (Test-Path $marker) -and -not (Test-Path $legacyMarker)) {
    $backup = "$existing.before-ssfv"
    if (-not (Test-Path $backup)) {
        Copy-Item $existing $backup
        Write-Host "Saved the existing xinput1_3.dll as xinput1_3.dll.before-ssfv"
    }
}

# Rename the old settings file before installing so existing settings survive
# the project rename. A fresh install gets the new default file.
$config = Join-Path $target "ssfv.ini"
$legacyConfig = Join-Path $target "sfvaccess.ini"
if (Test-Path $legacyConfig) {
    if (-not (Test-Path $config)) {
        Move-Item $legacyConfig $config
    } else {
        Remove-Item $legacyConfig -Force
    }
}
if (-not (Test-Path $config)) {
    Copy-Item (Join-Path $source "ssfv.ini") $config -Force
}

$log = Join-Path $target "ssfv-native.log"
$legacyLog = Join-Path $target "sfvaccess-native.log"
if (Test-Path $legacyLog) {
    if (-not (Test-Path $log)) {
        Move-Item $legacyLog $log
    } else {
        Remove-Item $legacyLog -Force
    }
}

Copy-Item (Join-Path $source "xinput1_3.dll") $target -Force
Copy-Item (Join-Path $source "nvdaControllerClient64.dll") $target -Force
Copy-Item (Join-Path $source "prism.dll") $target -Force
New-Item -ItemType File -Force $marker | Out-Null
if (Test-Path $legacyMarker) { Remove-Item $legacyMarker -Force }
Write-Host "Installed into $target"
Write-Host "Start Street Fighter V normally. Log: $target\ssfv-native.log"
Write-Host "Uninstall: delete $target\xinput1_3.dll, $target\nvdaControllerClient64.dll, $target\ssfv.ini, $target\ssfv-native.log and $target\xinput1_3.dll.ssfv"
