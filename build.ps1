# Builds SysDVR-PS4 on Windows: compiles in WSL, then packages with the Windows PkgTool.
# Usage: powershell -ExecutionPolicy Bypass -File build.ps1
$ErrorActionPreference = "Stop"
$root = $PSScriptRoot
$toolchainWin = if ($env:OO_PS4_TOOLCHAIN_WIN) { $env:OO_PS4_TOOLCHAIN_WIN } else { "$env:USERPROFILE\ps4dev\OpenOrbis\PS4Toolchain" }
$distro = if ($env:WSL_DISTRO) { $env:WSL_DISTRO } else { "Debian" }

$wslPath = (wsl -d $distro wslpath -a ($root -replace '\\', '/')).Trim()
Write-Host "==> Compiling in WSL ($distro): $wslPath"
wsl -d $distro -- make -C "$wslPath" pkg.gp4
if ($LASTEXITCODE -ne 0) { throw "make failed" }

Write-Host "==> Building the .pkg"
# PkgTool.Core targets .NET Core 3.0: let it run on any newer installed runtime
$env:DOTNET_ROLL_FORWARD = "Major"
Push-Location $root
try {
    & "$toolchainWin\bin\windows\PkgTool.Core.exe" pkg_build pkg.gp4 .
    if ($LASTEXITCODE -ne 0) { throw "PkgTool failed" }
} finally {
    Pop-Location
}
Get-ChildItem "$root\*.pkg" | ForEach-Object { Write-Host "==> Done: $($_.FullName)" }
