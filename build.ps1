# Builds build\xinput1_3.dll with the Visual C++ toolchain that is installed.
# Prism is vendored; speech backends are chosen by Prism at runtime.
$ErrorActionPreference = "Stop"
Set-Location $PSScriptRoot

$vswhere = Join-Path ${env:ProgramFiles(x86)} "Microsoft Visual Studio\Installer\vswhere.exe"
if (-not (Test-Path $vswhere)) {
    throw "vswhere.exe not found. Install Visual Studio Build Tools with the C++ workload."
}
$vsPath = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $vsPath) { throw "No Visual Studio with the C++ toolchain was found." }
$vcvars = Join-Path $vsPath "VC\Auxiliary\Build\vcvars64.bat"
if (-not (Test-Path $vcvars)) { throw "vcvars64.bat not found at $vcvars" }

New-Item -ItemType Directory -Force build | Out-Null

$sources = @(
    "src\common.cpp",
    "src\config.cpp",
    "src\object_index.cpp",
    "src\keyconfig.cpp",
    "src\radar.cpp",
    "src\macro.cpp",
    "src\scaleform.cpp",
    "src\speech.cpp",
    "src\fight.cpp",
    "src\narrator.cpp",
    "src\hotkeys.cpp",
    "src\xinput_proxy.cpp",
    "src\dllmain.cpp"
)

# /MT so the mod has no runtime DLL of its own to depend on.
$compile = "call `"$vcvars`" >nul && cl /nologo /O2 /MT /EHsc /std:c++17 /W3 /DNDEBUG " +
    "/DWIN32_LEAN_AND_MEAN /Fo:build\\ " + ($sources -join " ") +
    " /link /DLL /DEF:src\\xinput.def /OUT:build\\xinput1_3.dll user32.lib ole32.lib oleaut32.lib winmm.lib"

cmd /c $compile
if ($LASTEXITCODE -ne 0) { throw "Build failed with exit code $LASTEXITCODE" }

Copy-Item config\ssfv.ini build\ssfv.ini -Force
# Prism decides which screen reader or voice backend to use. It is loaded
# beside the mod; SAPI is the built-in fallback.
Copy-Item vendor\prism.dll build\prism.dll -Force
Write-Host ""
Write-Host "Built build\xinput1_3.dll"
Write-Host "Install: .\install.ps1"
