param(
    [string]$BuildDirectory = (Join-Path $PSScriptRoot '../../build-compact-static'),
    [string]$Compiler = 'C:/msys64/ucrt64/bin/c++.exe',
    [string]$Objcopy = 'C:/msys64/ucrt64/bin/objcopy.exe',
    [string]$Objdump = 'C:/msys64/ucrt64/bin/objdump.exe',
    [string]$Nm = 'C:/msys64/ucrt64/bin/nm.exe',
    [string]$ReproEpoxyArchive = '',
    [int]$TimeoutMs = 20000
)
$ErrorActionPreference = 'Stop'
if ($TimeoutMs -lt 5000 -or $TimeoutMs -gt 60000) { throw 'TimeoutMs must be between 5000 and 60000.' }
$BuildDirectory = [IO.Path]::GetFullPath($BuildDirectory)
$Compiler = (Get-Command $Compiler -ErrorAction Stop).Source
$Objcopy = (Get-Command $Objcopy -ErrorAction Stop).Source
$Objdump = (Get-Command $Objdump -ErrorAction Stop).Source
$Nm = (Get-Command $Nm -ErrorAction Stop).Source
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
if ($link.LINK_LIBRARIES -notmatch '(?:^|[\\/])libQt5Core\.a(?:["\s]|$)') { throw 'This startup fixture requires the compact static Qt Windows build.' }
$runDirectory = Join-Path $BuildDirectory ('qt-startup-' + [Guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $runDirectory | Out-Null
$mainObject = Join-Path $BuildDirectory 'qt/CMakeFiles/mgba-qt.dir/main.cpp.obj'
$renamedMain = (Join-Path $runDirectory 'production-main.obj').Replace('\', '/')
$wrapperObject = (Join-Path $runDirectory 'startup.obj').Replace('\', '/')
$executable = (Join-Path $runDirectory 'startup.exe').Replace('\', '/')
$source = (Join-Path $PSScriptRoot 'startup.cpp').Replace('\', '/')
$compileResponse = Join-Path $runDirectory 'compile.rsp'
$linkResponse = Join-Path $runDirectory 'link.rsp'
$utf8 = [Text.UTF8Encoding]::new($false)
[IO.File]::WriteAllText((Join-Path $runDirectory 'portable.ini'), "[portable]`n", $utf8)
[IO.File]::WriteAllText((Join-Path $runDirectory 'production-main.sha256'), (Get-FileHash -LiteralPath $mainObject -Algorithm SHA256).Hash, $utf8)
[IO.File]::WriteAllText($compileResponse, ($compile.DEFINES + ' ' + $compile.FLAGS + ' ' + $compile.INCLUDES + ' -c "' + $source + '" -o "' + $wrapperObject + '"'), $utf8)
$objects = (($link.Header -split ': CXX_EXECUTABLE_LINKER[^ ]+ ', 2)[1] -split ' \|', 2)[0]
$objects = ($objects -split ' ' | Where-Object { $_ -and $_ -ne 'qt/CMakeFiles/mgba-qt.dir/main.cpp.obj' }) -join ' '
$libraries = $link.LINK_LIBRARIES
if ($ReproEpoxyArchive) {
    # Explicit negative-control mode only: restore the previous epoxy archive
    # while retaining the same current app/Qt objects and default GL startup.
    $replacement = [IO.Path]::GetFullPath($ReproEpoxyArchive).Replace('\', '/')
    if (-not (Test-Path -LiteralPath $replacement -PathType Leaf)) { throw "Missing negative-control epoxy archive: $replacement" }
    $epoxyPattern = '(?:"[^"]*[\\/]libepoxy\.a"|[^\s"]*[\\/]libepoxy\.a)'
    if (-not [regex]::IsMatch($libraries, $epoxyPattern)) { throw 'No explicit static epoxy archive found in target libraries.' }
    $libraries = [regex]::Replace($libraries, $epoxyPattern, { '"' + $replacement + '"' })
    Write-Output "Negative-control epoxy override: $replacement"
}
# Preserve the Unicode Windows entry-point flags, every original library search
# path, and all static/runtime flags. The original app main object is retained
# with symbols renamed, not replaced by synthetic startup logic.
[IO.File]::WriteAllText($linkResponse, ($link.FLAGS + ' ' + $link.LINK_FLAGS + ' ' + $objects + ' "' + $renamedMain + '" "' + $wrapperObject + '" -o "' + $executable + '" ' + $link.LINK_PATH + ' ' + $libraries), $utf8)
$saved = @{}
$names = @('PATH', 'QT_QPA_PLATFORM', 'QT_QPA_PLATFORM_PLUGIN_PATH', 'QT_PLUGIN_PATH', 'SDL_VIDEODRIVER', 'SDL_AUDIODRIVER')
foreach ($name in $names) { $saved[$name] = [Environment]::GetEnvironmentVariable($name, 'Process') }
try {
    $env:PATH = "$(Split-Path -Parent $Compiler);C:/Windows/System32;C:/Windows"
    & $Objcopy --redefine-sym main=mgba_app_main --redefine-sym wmain=mgba_app_wmain $mainObject $renamedMain
    if ($LASTEXITCODE -ne 0) { throw "Production entry-point object rename failed ($LASTEXITCODE)." }
    $symbols = & $Nm --defined-only $renamedMain | Out-String
    if ($LASTEXITCODE -ne 0) { throw 'Cannot inspect renamed production entry symbols.' }
    if ($symbols -notmatch '(?m)\bmgba_app_main\s*$' -or $symbols -notmatch '(?m)\bmgba_app_wmain\s*$') { throw 'Renamed production main/wmain symbols are missing.' }
    Push-Location $BuildDirectory
    try {
        & $Compiler ('@' + $compileResponse)
        if ($LASTEXITCODE -ne 0) { throw "Startup wrapper compilation failed ($LASTEXITCODE)." }
        & $Compiler ('@' + $linkResponse)
        if ($LASTEXITCODE -ne 0) { throw "Startup fixture linking failed ($LASTEXITCODE)." }
    } finally { Pop-Location }
    $imports = & $Objdump -p $executable | Out-String
    if ($LASTEXITCODE -ne 0) { throw 'Startup fixture import inspection failed.' }
    [IO.File]::WriteAllText((Join-Path $runDirectory 'imports.txt'), $imports, $utf8)
    $systemDlls = @(
        'advapi32.dll', 'bcrypt.dll', 'd3d9.dll', 'dwmapi.dll', 'dwrite.dll', 'dxva2.dll', 'evr.dll',
        'gdi32.dll', 'imm32.dll', 'kernel32.dll', 'mf.dll', 'mfplat.dll', 'netapi32.dll',
        'ole32.dll', 'oleaut32.dll', 'opengl32.dll', 'rpcrt4.dll', 'setupapi.dll', 'shell32.dll', 'shlwapi.dll',
        'user32.dll', 'userenv.dll', 'usp10.dll', 'uxtheme.dll', 'version.dll', 'winmm.dll', 'ws2_32.dll',
        'crypt32.dll', 'iphlpapi.dll', 'wtsapi32.dll', 'ucrtbase.dll', 'msvcrt.dll'
    )
    $dlls = [regex]::Matches($imports, 'DLL Name:\s*(\S+)') | ForEach-Object { $_.Groups[1].Value.ToLowerInvariant() }
    if (-not $dlls) { throw 'No PE imports were parsed; startup import audit was not meaningful.' }
    foreach ($dll in $dlls) {
        if ($dll -notin $systemDlls -and $dll -notmatch '^(api|ext)-ms-win-') { throw "Unexpected third-party DLL import in compact startup fixture: $dll" }
    }
    Write-Output "Startup fixture import audit: PASS, Windows system DLLs only ($($dlls.Count) imports)."
    $env:PATH = 'C:/Windows/System32;C:/Windows;C:/Windows/System32/downlevel'
    $env:QT_QPA_PLATFORM = 'windows'
    $env:QT_QPA_PLATFORM_PLUGIN_PATH = ''
    $env:QT_PLUGIN_PATH = ''
    # Do not select SDL dummy or override the renderer: normal production
    # defaults must initialize. There is no loaded game/audio stream.
    $env:SDL_VIDEODRIVER = ''
    $env:SDL_AUDIODRIVER = ''
    $start = [Diagnostics.ProcessStartInfo]::new()
    $start.FileName = $executable
    $start.WorkingDirectory = $runDirectory
    $start.UseShellExecute = $false
    $start.CreateNoWindow = $true
    $start.WindowStyle = [Diagnostics.ProcessWindowStyle]::Hidden
    $start.RedirectStandardOutput = $true
    $start.RedirectStandardError = $true
    $process = [Diagnostics.Process]::Start($start)
    if (-not $process) { throw 'Startup fixture process did not start.' }
    $ownPid = $process.Id
    $stdout = $process.StandardOutput.ReadToEndAsync()
    $stderr = $process.StandardError.ReadToEndAsync()
    if (-not $process.WaitForExit($TimeoutMs)) {
        # This Process object is exclusively the child created above. Never
        # enumerate, close, or terminate another emulator or arbitrary PID.
        $process.Kill()
        $process.WaitForExit(5000) | Out-Null
        throw "Startup fixture own PID $ownPid exceeded ${TimeoutMs}ms and was terminated. Fixture: $runDirectory"
    }
    $process.WaitForExit()
    [IO.File]::WriteAllText((Join-Path $runDirectory 'stdout.txt'), $stdout.GetAwaiter().GetResult(), $utf8)
    [IO.File]::WriteAllText((Join-Path $runDirectory 'stderr.txt'), $stderr.GetAwaiter().GetResult(), $utf8)
    $reportPath = Join-Path $runDirectory 'startup-report.txt'
    $report = if (Test-Path -LiteralPath $reportPath) { [IO.File]::ReadAllText($reportPath) } else { '' }
    Write-Output $report.TrimEnd()
    $code = [BitConverter]::ToUInt32([BitConverter]::GetBytes([int]$process.ExitCode), 0)
    Write-Output ('Startup fixture own PID {0}: exit 0x{1:X8}; retained at {2}' -f $ownPid, $code, $runDirectory)
    if ($process.ExitCode -ne 0 -or $report -notmatch 'PASS production Windows startup') { throw 'Production Windows startup/default OpenGL/normal-close regression failed.' }
} finally {
    foreach ($name in $names) { [Environment]::SetEnvironmentVariable($name, $saved[$name], 'Process') }
}
