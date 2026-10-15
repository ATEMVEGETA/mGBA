param([string]$BuildDirectory = (Join-Path $PSScriptRoot '..\..\build-wireless'), [string]$Compiler = 'c++.exe', [string]$QtPlatformPluginPath = '')
$ErrorActionPreference = 'Stop'
$Compiler = (Get-Command $Compiler -ErrorAction Stop).Source
if (-not $QtPlatformPluginPath) {
    $prefix = Split-Path -Parent (Split-Path -Parent $Compiler)
    $QtPlatformPluginPath = Join-Path $prefix 'share/qt5/plugins/platforms'
}
$BuildDirectory = [IO.Path]::GetFullPath($BuildDirectory)
$ninja = [IO.File]::ReadAllLines((Join-Path $BuildDirectory 'build.ninja'))
function Get-NinjaStanza([string]$pattern) {
    $index = -1
    for ($i = 0; $i -lt $ninja.Length; ++$i) { if ($ninja[$i] -match $pattern) { $index = $i; break } }
    if ($index -lt 0) { throw "Missing Ninja target: $pattern" }
    $result = @{ Header = $ninja[$index] }
    for ($i = $index + 1; $i -lt $ninja.Length -and $ninja[$i] -match '^  ([A-Z_]+) = (.*)$'; ++$i) { $result[$Matches[1]] = $Matches[2] }
    return $result
}
$compile = Get-NinjaStanza '^build qt/CMakeFiles/mgba-qt.dir/main.cpp.obj:'
$link = Get-NinjaStanza '^build mGBA.exe:'
$runDirectory = Join-Path $BuildDirectory ('qt-wireless-smoke-' + [Guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $runDirectory | Out-Null
$object = (Join-Path $runDirectory 'smoke.obj').Replace('\', '/')
$executable = (Join-Path $runDirectory 'smoke.exe').Replace('\', '/')
$source = (Join-Path $PSScriptRoot 'smoke.cpp').Replace('\', '/')
$compileResponse = Join-Path $runDirectory 'compile.rsp'
$linkResponse = Join-Path $runDirectory 'link.rsp'
$utf8 = [Text.UTF8Encoding]::new($false)
[IO.File]::WriteAllText($compileResponse, ($compile.DEFINES + ' ' + $compile.FLAGS + ' ' + $compile.INCLUDES + ' -c "' + $source + '" -o "' + $object + '"'), $utf8)
$objectList = (($link.Header -split ': CXX_EXECUTABLE_LINKER[^ ]+ ', 2)[1] -split ' \|', 2)[0]
$objectList = ($objectList -split ' ' | Where-Object { $_ -and $_ -ne 'qt/CMakeFiles/mgba-qt.dir/main.cpp.obj' }) -join ' '
$libraries = $link.LINK_LIBRARIES -replace '(^|\s)-municode(\s|$)', ' '
[IO.File]::WriteAllText($linkResponse, ($objectList + ' "' + $object + '" -o "' + $executable + '" ' + $libraries), $utf8)
# These generated artifacts are confined to the fresh build fixture directory.
[IO.File]::WriteAllText((Join-Path $runDirectory 'portable.ini'), "[portable]`n", $utf8)
$savedPath = $env:PATH
$savedPlatform = $env:QT_QPA_PLATFORM
$savedPlugins = $env:QT_QPA_PLATFORM_PLUGIN_PATH
$savedVideo = $env:SDL_VIDEODRIVER
$savedAudio = $env:SDL_AUDIODRIVER
try {
    $env:PATH = "$(Split-Path -Parent $Compiler);C:\Windows\System32;C:\Windows;$savedPath"
    $env:QT_QPA_PLATFORM = 'offscreen'
    $env:QT_QPA_PLATFORM_PLUGIN_PATH = $QtPlatformPluginPath
    $env:SDL_VIDEODRIVER = 'dummy'
    $env:SDL_AUDIODRIVER = 'dummy'
    Push-Location $BuildDirectory
    try {
        & $Compiler ('@' + $compileResponse)
        if ($LASTEXITCODE -ne 0) { throw "Qt smoke compilation failed ($LASTEXITCODE)" }
        & $Compiler ('@' + $linkResponse)
        if ($LASTEXITCODE -ne 0) { throw "Qt smoke linking failed ($LASTEXITCODE)" }
    } finally { Pop-Location }
    & $executable $runDirectory
    if ($LASTEXITCODE -ne 0) { throw "Qt smoke failed ($LASTEXITCODE)" }
    Write-Output "Isolated fixture retained at $runDirectory"
} finally {
    $env:PATH = $savedPath
    $env:QT_QPA_PLATFORM = $savedPlatform
    $env:QT_QPA_PLATFORM_PLUGIN_PATH = $savedPlugins
    $env:SDL_VIDEODRIVER = $savedVideo
    $env:SDL_AUDIODRIVER = $savedAudio
}
