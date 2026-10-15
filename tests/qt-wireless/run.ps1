param(
    [string]$BuildDirectory = (Join-Path $PSScriptRoot '..\..\build-wireless'),
    [string]$Compiler = 'c++.exe',
    [string]$QtPlatformPluginPath = '',
    [string]$QtStaticPrefix = '',
    [string]$QtOffscreenPluginArchive = ''
)
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
$staticQt = $link.LINK_LIBRARIES -match '(?:^|[\\/])libQt5Core\.a(?:["\s]|$)'
$extraLibraries = ''
if ($staticQt) {
    if (-not $QtStaticPrefix) {
        # Resolve the actual configured Qt package, not the compiler's shared
        # Qt prefix. Static dependency bundles can live outside the toolchain.
        $cache = [IO.File]::ReadAllLines((Join-Path $BuildDirectory 'CMakeCache.txt'))
        $qtPackage = $cache | Where-Object { $_ -match '^Qt5Core_DIR:[^=]+=(.+)$' } | Select-Object -First 1
        if ($qtPackage -match '^Qt5Core_DIR:[^=]+=(.+)$') {
            $QtStaticPrefix = [IO.Path]::GetFullPath((Join-Path $Matches[1] '../../..'))
        } elseif ($link.LINK_LIBRARIES -match '(?:"(?<quoted>[^"]*[\\/]libQt5Core\.a)"|(?<plain>[^\s"]*[\\/]libQt5Core\.a))') {
            $coreArchive = if ($Matches.quoted) { $Matches.quoted } else { $Matches.plain }
            $QtStaticPrefix = Split-Path -Parent (Split-Path -Parent $coreArchive)
        } else {
            throw 'Cannot derive the static Qt prefix; pass -QtStaticPrefix explicitly.'
        }
    }
    $QtStaticPrefix = [IO.Path]::GetFullPath($QtStaticPrefix)
    if (-not $QtOffscreenPluginArchive) {
        foreach ($candidate in @('share/qt5/plugins/platforms/libqoffscreen.a', 'plugins/platforms/libqoffscreen.a')) {
            $path = Join-Path $QtStaticPrefix $candidate
            if (Test-Path -LiteralPath $path -PathType Leaf) { $QtOffscreenPluginArchive = $path; break }
        }
    }
    if (-not $QtOffscreenPluginArchive -or -not (Test-Path -LiteralPath $QtOffscreenPluginArchive -PathType Leaf)) {
        throw 'Static Qt smoke requires libqoffscreen.a; pass -QtOffscreenPluginArchive for this matching Qt build.'
    }
    $QtPlatformPluginPath = Split-Path -Parent ([IO.Path]::GetFullPath($QtOffscreenPluginArchive))
    if ($link.LINK_LIBRARIES -notmatch '(?:^|[\\/])libqoffscreen\.a(?:["\s]|$)') {
        $archive = [IO.Path]::GetFullPath($QtOffscreenPluginArchive).Replace('\', '/')
        $extraLibraries = '"' + $archive + '" '
        # Preserve the plugin's own static dependencies from the same Qt
        # installation. The main program may only link the Windows plugin.
        $prlPath = Join-Path (Split-Path -Parent $archive) 'qoffscreen.prl'
        if (-not (Test-Path -LiteralPath $prlPath -PathType Leaf)) { throw "Missing static offscreen dependency metadata: $prlPath" }
        $prl = [IO.File]::ReadAllLines($prlPath) | Where-Object { $_ -match '^QMAKE_PRL_LIBS_FOR_CMAKE\s*=\s*(.*)$' } | Select-Object -First 1
        if ($prl -notmatch '^QMAKE_PRL_LIBS_FOR_CMAKE\s*=\s*(.*)$') { throw "Missing QMAKE_PRL_LIBS_FOR_CMAKE in $prlPath" }
        $qtLib = (Join-Path $QtStaticPrefix 'lib').Replace('\', '/')
        $dependencies = foreach ($entry in ($Matches[1] -split ';')) {
            $entry = $entry.Trim().Replace('$$[QT_INSTALL_LIBS]', $qtLib)
            if ($entry) { '"' + $entry + '"' }
        }
        $extraLibraries += ($dependencies -join ' ') + ' '
    }
    Write-Output "Qt smoke: static Qt; offscreen archive $QtOffscreenPluginArchive"
} else {
    Write-Output "Qt smoke: shared Qt; platform plugin directory $QtPlatformPluginPath"
}
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
$linkFlags = (($link.FLAGS + ' ' + $link.LINK_FLAGS) -replace '(^|\s)-municode(\s|$)', ' ')
# Retain the real target's optimization/static/runtime/linker flags. Only the
# entry-point subsystem is overridden: this fixture has a console main(), not
# the application's Unicode WinMain, and its check output must be observable.
[IO.File]::WriteAllText($linkResponse, ($linkFlags + ' -mconsole ' + $objectList + ' "' + $object + '" -o "' + $executable + '" ' + $link.LINK_PATH + ' ' + $extraLibraries + $libraries), $utf8)
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
