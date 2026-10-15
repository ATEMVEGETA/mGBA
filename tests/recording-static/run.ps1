param(
    [string]$FFmpegPrefix = (Join-Path $PSScriptRoot '../../build-compact-deps/ffmpeg-static'),
    [string]$BuildDirectory = (Join-Path $PSScriptRoot '../../build-compact-static'),
    [string]$Compiler = 'C:/msys64/ucrt64/bin/c++.exe',
    [string]$PkgConfig = 'C:/msys64/ucrt64/bin/pkg-config.exe',
    [string]$Objdump = 'C:/msys64/ucrt64/bin/objdump.exe',
    [switch]$SkipSyntheticGif
)
$ErrorActionPreference = 'Stop'
$FFmpegPrefix = [IO.Path]::GetFullPath($FFmpegPrefix)
$BuildDirectory = [IO.Path]::GetFullPath($BuildDirectory)
$Compiler = (Get-Command $Compiler -ErrorAction Stop).Source
$PkgConfig = (Get-Command $PkgConfig -ErrorAction Stop).Source
$Objdump = (Get-Command $Objdump -ErrorAction Stop).Source
$toolchain = Split-Path -Parent (Split-Path -Parent $Compiler)
$pcDirectory = Join-Path $FFmpegPrefix 'lib/pkgconfig'
foreach ($name in @('avcodec', 'avformat', 'avfilter', 'avutil', 'swscale', 'swresample')) {
    $archive = Join-Path $FFmpegPrefix "lib/lib$name.a"
    if (-not (Test-Path -LiteralPath $archive -PathType Leaf)) { throw "Static dependency not ready: $archive" }
}
$runDirectory = Join-Path $BuildDirectory ('recording-static-smoke-' + [Guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $runDirectory | Out-Null
$executable = Join-Path $runDirectory 'recording-smoke.exe'
$source = (Join-Path $PSScriptRoot 'smoke.cpp').Replace('\', '/')
$response = Join-Path $runDirectory 'compile-link.rsp'
$utf8 = [Text.UTF8Encoding]::new($false)
$savedPath = $env:PATH
$savedPkgPath = $env:PKG_CONFIG_PATH
$savedPkgLibdir = $env:PKG_CONFIG_LIBDIR
function Normalize-MsysDrivePath([string]$value) {
    return [regex]::Replace($value, '(?<![A-Za-z0-9_])/([A-Za-z])/', { param($match) $match.Groups[1].Value.ToUpperInvariant() + ':/' })
}
try {
    $env:PATH = "$(Split-Path -Parent $Compiler);C:/Windows/System32;C:/Windows"
    # The isolated FFmpeg metadata must take precedence; its codec dependencies
    # come from the matching native toolchain rather than an unrelated FFmpeg.
    $env:PKG_CONFIG_PATH = $pcDirectory
    $env:PKG_CONFIG_LIBDIR = "$pcDirectory;$(Join-Path $toolchain 'lib/pkgconfig');$(Join-Path $toolchain 'share/pkgconfig')"
    $selected = & $PkgConfig --variable=libdir libavcodec | Out-String
    if ($LASTEXITCODE -ne 0) { throw 'Cannot resolve the isolated libavcodec package.' }
    $selected = [IO.Path]::GetFullPath((Normalize-MsysDrivePath $selected.Trim()))
    $expected = [IO.Path]::GetFullPath((Join-Path $FFmpegPrefix 'lib'))
    if ($selected.TrimEnd('\', '/') -ine $expected.TrimEnd('\', '/')) { throw "Wrong FFmpeg package selected: $selected" }
    $packages = @('libavfilter', 'libavformat', 'libavcodec', 'libswscale', 'libswresample', 'libavutil')
    $cflags = & $PkgConfig --cflags @packages | Out-String
    if ($LASTEXITCODE -ne 0) { throw 'Static recording pkg-config cflags failed.' }
    $libraries = & $PkgConfig --static --libs @packages | Out-String
    if ($LASTEXITCODE -ne 0) { throw 'Static recording pkg-config libraries failed.' }
    # Some native codec .pc files explicitly request the shared GCC unwind
    # runtime. Use the driver's static runtime closure instead, as requested
    # above; do not allow that explicit import to defeat -static-libgcc.
    $libraries = $libraries -replace '(?<!\S)-lgcc_s(?!\S)', ''
    $arguments = '-std=c++17 -O1 -g -Wall -Wextra -static -static-libgcc -static-libstdc++ -mconsole ' +
        (Normalize-MsysDrivePath $cflags.Trim()) + ' "' + $source + '" -o "' + $executable.Replace('\', '/') + '" ' +
        '-Wl,--start-group ' + (Normalize-MsysDrivePath $libraries.Trim()) + ' -Wl,--end-group'
    [IO.File]::WriteAllText($response, $arguments, $utf8)
    & $Compiler ('@' + $response)
    if ($LASTEXITCODE -ne 0) { throw "Static recording smoke compilation/linking failed ($LASTEXITCODE)." }
    $imports = & $Objdump -p $executable | Out-String
    if ($LASTEXITCODE -ne 0) { throw 'Recording smoke import audit failed.' }
    [IO.File]::WriteAllText((Join-Path $runDirectory 'imports.txt'), $imports, $utf8)
    $systemDlls = @(
        'advapi32.dll', 'bcrypt.dll', 'comdlg32.dll', 'crypt32.dll', 'dnsapi.dll', 'gdi32.dll',
        'iphlpapi.dll', 'kernel32.dll', 'msvcrt.dll', 'ncrypt.dll', 'netapi32.dll', 'ntdll.dll',
        'ole32.dll', 'oleaut32.dll', 'secur32.dll', 'setupapi.dll', 'shell32.dll', 'shlwapi.dll',
        'ucrtbase.dll', 'user32.dll', 'userenv.dll', 'version.dll', 'winmm.dll', 'winspool.drv',
        'ws2_32.dll', 'wtsapi32.dll'
    )
    $dlls = [regex]::Matches($imports, 'DLL Name:\s*(\S+)') | ForEach-Object { $_.Groups[1].Value.ToLowerInvariant() }
    if (-not $dlls) { throw 'No PE imports were parsed; static dependency audit was not meaningful.' }
    foreach ($dll in $dlls) {
        if ($dll -notin $systemDlls -and $dll -notmatch '^(api|ext)-ms-win-') { throw "Unexpected third-party DLL import in static recording smoke: $dll" }
    }
    Write-Output ('Static PE import audit: PASS (Windows system DLLs only: ' + ($dlls -join ', ') + ')')
    # Deliberately remove the compiler/dependency bin folders while running.
    $env:PATH = 'C:/Windows/System32;C:/Windows;C:/Windows/System32/downlevel'
    $output = if ($SkipSyntheticGif) { '--list-only' } else { Join-Path $runDirectory 'synthetic.gif' }
    & $executable $output
    if ($LASTEXITCODE -ne 0) { throw "Static recording smoke failed ($LASTEXITCODE)." }
    Write-Output "Isolated recording fixture retained at $runDirectory"
} finally {
    $env:PATH = $savedPath
    $env:PKG_CONFIG_PATH = $savedPkgPath
    $env:PKG_CONFIG_LIBDIR = $savedPkgLibdir
}
