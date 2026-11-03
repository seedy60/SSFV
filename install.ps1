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
if (-not (Test-Path "xinput1_3.dll")) {
    throw "xinput1_3.dll is missing. Run .\build.ps1 first."
}

# A DLL already there is not the game's: back it up rather than overwrite
# someone else's proxy (SF5DInput uses this exact name for DirectInput). The
# marker means the file is this mod's own, from an earlier version.
$existing = Join-Path $target "xinput1_3.dll"
$marker = Join-Path $target "xinput1_3.dll.sfvaccess"
if ((Test-Path $existing) -and -not (Test-Path $marker)) {
    $backup = "$existing.before-sfvaccess"
    if (-not (Test-Path $backup)) {
        Copy-Item $existing $backup
        Write-Host "Saved the existing xinput1_3.dll as xinput1_3.dll.before-sfvaccess"
    }
}

Copy-Item "xinput1_3.dll" $target -Force
Copy-Item "sfvaccess.ini" $target -Force
Copy-Item "nvdaControllerClient64.dll" $target -Force
Copy-Item "prism.dll" $target -Force
New-Item -ItemType File -Force $marker | Out-Null
Write-Host "Installed into $target"
Write-Host "Start Street Fighter V normally. Log: $target\sfvaccess-native.log"
Write-Host "Uninstall: delete $target\xinput1_3.dll, $target\nvdaControllerClient64.dll, $target\sfvaccess.ini and $target\xinput1_3.dll.sfvaccess"
