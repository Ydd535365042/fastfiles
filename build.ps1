param([ValidateSet('Release', 'Debug')][string]$Config = 'Release', [ValidateSet('x64', 'x86')][string]$Arch = 'x64')
$ErrorActionPreference = 'Stop'
$root = $PSScriptRoot

$env:Path = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer;$env:Path"
Import-Module 'C:\Program Files\Microsoft Visual Studio\18\Community\Common7\Tools\Microsoft.VisualStudio.DevShell.dll'
Enter-VsDevShell -VsInstallPath 'C:\Program Files\Microsoft Visual Studio\18\Community' -SkipAutomaticLocation -DevCmdArguments "-arch=$Arch -host_arch=x64" | Out-Null

# x64 keeps build\<Config> (the tests run that exe); x86 builds into build\<Config>-x86
$build = Join-Path $root $(if ($Arch -eq 'x64') { "build\$Config" } else { "build\$Config-$Arch" })
cmake -S $root -B $build -G Ninja "-DCMAKE_BUILD_TYPE=$Config" | Out-Null
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
cmake --build $build
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
# Release: the installer (our own Setup.exe, see setup/) goes next to the project folder
if ($Config -eq 'Release') { Get-ChildItem $build -Filter "FastFiles_*_${Arch}_Setup.exe" | Copy-Item -Destination (Split-Path $root) -Force }
exit 0
