param(
    [string]$RuntimePrefix = 'C:/msys64/ucrt64',
    [string]$BuildDirectory = 'build-compact-static',
    [string]$DependencyDirectory = '',
    [int]$Jobs = 4
)
$ErrorActionPreference = 'Stop'
$root = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
if (-not [IO.Path]::IsPathRooted($BuildDirectory)) { $BuildDirectory = Join-Path $root $BuildDirectory }
if ($DependencyDirectory -and -not [IO.Path]::IsPathRooted($DependencyDirectory)) { $DependencyDirectory = Join-Path $root $DependencyDirectory }
$deps = if ($DependencyDirectory) { (Resolve-Path -LiteralPath $DependencyDirectory).Path } else { Join-Path $root 'build-compact-deps' }
$qt = Join-Path $deps 'ucrt64/qt5-static'
$ffmpeg = Join-Path $deps 'ffmpeg-static'
$epoxy = Join-Path $deps 'epoxy-static'
if (-not (Test-Path (Join-Path $qt 'lib/libQt5Core.a')) -or
    -not (Test-Path (Join-Path $ffmpeg 'lib/pkgconfig/libavcodec.pc')) -or
    -not (Test-Path (Join-Path $epoxy 'lib/libepoxy.a'))) {
    throw 'Prepare the matching static Qt, recording-only FFmpeg and static-TLS epoxy prefixes first; see doc/WIRELESS-BUILD.md.'
}
$RuntimePrefix = (Resolve-Path -LiteralPath $RuntimePrefix).Path.Replace('\', '/')
$epoxySymbols = & "$RuntimePrefix/bin/nm.exe" --defined-only (Join-Path $epoxy 'lib/libepoxy.a') | Out-String
if ($LASTEXITCODE -ne 0 -or $epoxySymbols -notmatch '(?m)\b[DdRr]\s+dllmain_callback\s*$') {
    throw "The static epoxy archive must contain the official fork's PE TLS callback; a stock static archive is unsafe here."
}
$savedPath = $env:PATH
$savedPkg = $env:PKG_CONFIG_PATH
$savedPkgLib = $env:PKG_CONFIG_LIBDIR
try {
    $env:PATH = "$RuntimePrefix/bin;C:/Program Files/Git/cmd;$savedPath"
    $env:PKG_CONFIG_PATH = "$ffmpeg/lib/pkgconfig;$RuntimePrefix/lib/pkgconfig"
    $env:PKG_CONFIG_LIBDIR = "$ffmpeg/lib/pkgconfig;$RuntimePrefix/lib/pkgconfig;$RuntimePrefix/share/pkgconfig"
    $resolved = & "$RuntimePrefix/bin/pkg-config.exe" --variable=libdir libavcodec
    if ($LASTEXITCODE -ne 0 -or [IO.Path]::GetFullPath($resolved) -ne [IO.Path]::GetFullPath((Join-Path $ffmpeg 'lib'))) {
        throw 'pkg-config must select the isolated recording-only FFmpeg, not the shared system build.'
    }
    $arguments = @(
        '-S', $root, '-B', $BuildDirectory, '-G', 'Ninja',
        '-U', '*libav*', '-U', '*LIBAV*', '-U', '*libsw*', '-U', '*LIBSW*',
        '-DCMAKE_BUILD_TYPE=Release', "-DCMAKE_PREFIX_PATH=$ffmpeg;$qt;$epoxy;$RuntimePrefix",
        "-DEPOXY_LIBRARIES=$epoxy/lib/libepoxy.a", "-DEPOXY_INCLUDE_DIRS=$epoxy/include",
        "-DCMAKE_PROJECT_INCLUDE=$PSScriptRoot/compact-win64.cmake",
        "-DCMAKE_MAKE_PROGRAM=$RuntimePrefix/bin/ninja.exe",
        "-DCMAKE_C_COMPILER=$RuntimePrefix/bin/cc.exe", "-DCMAKE_CXX_COMPILER=$RuntimePrefix/bin/c++.exe",
        '-DGIT:FILEPATH=C:/Program Files/Git/cmd/git.exe',
        '-DCMAKE_EXE_LINKER_FLAGS=-static -static-libgcc -static-libstdc++ -s',
        '-DBUILD_STATIC=ON', '-DBUILD_SHARED=OFF', '-DBUILD_QT=ON', '-DBUILD_SDL=ON',
        '-DSDL_VERSION=2', '-DFORCE_QT_VERSION=5', '-DBUILD_LTO=OFF', '-Wno-dev'
    )
    & "$RuntimePrefix/bin/cmake.exe" @arguments
    if ($LASTEXITCODE -ne 0) { throw 'Compact configuration failed.' }
    & "$RuntimePrefix/bin/cmake.exe" --build $BuildDirectory --target mgba mgba-qt mgba-sdl -j $Jobs
    if ($LASTEXITCODE -ne 0) { throw 'Compact build failed.' }
} finally {
    $env:PATH = $savedPath
    $env:PKG_CONFIG_PATH = $savedPkg
    $env:PKG_CONFIG_LIBDIR = $savedPkgLib
}
