param(
    [string]$BuildDirectory = 'build-wireless',
    [Parameter(Mandatory = $true)][string]$RuntimePrefix,
    [Parameter(Mandatory = $true)][string]$SourceArchive,
    [string]$OutputDirectory = 'build-wireless/dist/mGBA-Wireless-Adapter-test-win64'
)
$ErrorActionPreference = 'Stop'
$root = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
if (-not [IO.Path]::IsPathRooted($BuildDirectory)) { $BuildDirectory = Join-Path $root $BuildDirectory }
if (-not [IO.Path]::IsPathRooted($OutputDirectory)) { $OutputDirectory = Join-Path $root $OutputDirectory }
$BuildDirectory = (Resolve-Path -LiteralPath $BuildDirectory).Path
$RuntimePrefix = (Resolve-Path -LiteralPath $RuntimePrefix).Path
$OutputDirectory = [IO.Path]::GetFullPath($OutputDirectory).TrimEnd([char[]]'\/')
if ($OutputDirectory -eq [IO.Path]::GetPathRoot($OutputDirectory).TrimEnd([char[]]'\/')) {
    throw 'The package output must not be a filesystem root.'
}
$SourceArchive = (Resolve-Path -LiteralPath $SourceArchive).Path
$zipPath = "$OutputDirectory.zip"
$binary = Join-Path $BuildDirectory 'mGBA.exe'
$objdump = Join-Path $RuntimePrefix 'bin/objdump.exe'
if (-not (Test-Path -LiteralPath $binary) -or -not (Test-Path -LiteralPath $objdump)) {
    throw 'Build mGBA.exe first, and supply its matching MinGW runtime prefix.'
}
$commit = & git -C $root rev-parse HEAD
if ($LASTEXITCODE -ne 0) { throw 'Cannot record source commit.' }
if (& git -C $root status --porcelain --untracked-files=all) {
    throw 'Commit or preserve outstanding source changes before packaging.'
}
$versionSource = [IO.File]::ReadAllText((Join-Path $BuildDirectory 'version.c'))
$version = [regex]::Match($versionSource, 'projectVersion = "([^"]+)"').Groups[1].Value
if (-not $version -or $version.EndsWith('-dirty') -or
    $versionSource -notmatch ('gitCommit = "' + [regex]::Escape($commit) + '"')) {
    throw 'Reconfigure and rebuild from the clean source commit before packaging.'
}
$binaryText = [Text.Encoding]::ASCII.GetString([IO.File]::ReadAllBytes($binary))
if (-not $binaryText.Contains($commit) -or -not $binaryText.Contains($version)) {
    throw 'Executable version does not match the recorded source commit.'
}
if ((Test-Path -LiteralPath $OutputDirectory) -or (Test-Path -LiteralPath $zipPath)) {
    throw 'Output already exists. Choose a new output path; existing packages are never removed.'
}
New-Item -ItemType Directory -Path $OutputDirectory -Force | Out-Null
Copy-Item -LiteralPath $binary -Destination $OutputDirectory
foreach ($name in @('portable.ini', 'qt.conf', 'DEPENDENCY-NOTICES.md')) {
    Copy-Item -LiteralPath (Join-Path $PSScriptRoot "wireless-portable/$name") -Destination $OutputDirectory
}
Copy-Item -LiteralPath (Join-Path $root 'LICENSE') -Destination (Join-Path $OutputDirectory 'LICENSE.txt')
foreach ($name in @('WIRELESS-ADAPTER.md', 'WIRELESS-RELEASE-NOTES.md', 'WIRELESS-BUILD.md')) {
    Copy-Item -LiteralPath (Join-Path $root "doc/$name") -Destination $OutputDirectory
}
$plugins = Join-Path $RuntimePrefix 'share/qt5/plugins'
foreach ($name in @('audio', 'imageformats', 'mediaservice', 'playlistformats', 'styles')) {
    $source = Join-Path $plugins $name
    if (Test-Path -LiteralPath $source) { Copy-Item -LiteralPath $source -Destination $OutputDirectory -Recurse }
}
$platforms = Join-Path $OutputDirectory 'platforms'
New-Item -ItemType Directory -Path $platforms | Out-Null
foreach ($name in @('qwindows.dll', 'qoffscreen.dll')) {
    Copy-Item -LiteralPath (Join-Path $plugins "platforms/$name") -Destination $platforms
}

# Upstream's deploy-win.sh filters paths containing "mingw". UCRT runtimes do
# not have that path component, so resolve the full PE import closure directly.
$queue = [Collections.Generic.Queue[string]]::new()
Get-ChildItem -LiteralPath $OutputDirectory -Recurse -File | Where-Object { $_.Extension -in '.dll', '.exe' } |
    ForEach-Object { $queue.Enqueue($_.FullName) }
$seen = [Collections.Generic.HashSet[string]]::new([StringComparer]::OrdinalIgnoreCase)
$imports = [Collections.Generic.SortedSet[string]]::new([StringComparer]::OrdinalIgnoreCase)
$unresolved = [Collections.Generic.HashSet[string]]::new([StringComparer]::OrdinalIgnoreCase)
# Explicit Windows components, not arbitrary DLLs installed in System32.
# This local test package targets Windows x64; older Windows is not validated.
$windowsModules = [Collections.Generic.HashSet[string]]::new([StringComparer]::OrdinalIgnoreCase)
@'
advapi32.dll avrt.dll authz.dll bcrypt.dll bcryptprimitives.dll cabinet.dll cfgmgr32.dll
clbcatq.dll combase.dll comctl32.dll comdlg32.dll credui.dll crypt32.dll cryptbase.dll cryptsp.dll
d2d1.dll d3d9.dll d3d11.dll d3d12.dll d3dcompiler_47.dll dbghelp.dll devobj.dll dinput8.dll dnsapi.dll
dwmapi.dll dwrite.dll dxgi.dll dxva2.dll evr.dll gdi32.dll gdi32full.dll gdiplus.dll glu32.dll hid.dll iertutil.dll imm32.dll
iphlpapi.dll kernel32.dll kernelbase.dll mf.dll mfplat.dll mfreadwrite.dll mfsrcsnk.dll ncrypt.dll
mmdevapi.dll mpr.dll msacm32.dll msasn1.dll msdmo.dll msimg32.dll msvcp_win.dll msvcrt.dll
mswsock.dll netapi32.dll netutils.dll normaliz.dll nsi.dll ntdll.dll ole32.dll oleaut32.dll opengl32.dll
pdh.dll powrprof.dll profapi.dll propsys.dll psapi.dll quartz.dll rasapi32.dll rasman.dll rpcrt4.dll
secur32.dll setupapi.dll shell32.dll shlwapi.dll sspicli.dll ucrtbase.dll user32.dll userenv.dll
usp10.dll uxtheme.dll version.dll win32u.dll windowscodecs.dll winhttp.dll wininet.dll winmm.dll
winnsi.dll winspool.drv wintrust.dll wldap32.dll wlanapi.dll ws2_32.dll wsock32.dll wtsapi32.dll
'@ -split '\s+' | Where-Object { $_ } | ForEach-Object { [void]$windowsModules.Add($_) }
while ($queue.Count) {
    $file = $queue.Dequeue()
    if (-not $seen.Add($file)) { continue }
    $details = & $objdump -p $file 2>&1
    if ($LASTEXITCODE -ne 0) { throw "Cannot inspect PE imports: $file" }
    foreach ($line in $details) {
        if ($line -notmatch '^\s*DLL Name:\s*(.+?)\s*$') { continue }
        $name = $Matches[1]
        if ($name -notmatch '^[A-Za-z0-9_.+-]+\.(dll|drv)$') { throw "Invalid imported module name: $name" }
        $relative = $file.Substring($OutputDirectory.Length + 1).Replace('\', '/')
        $copy = Join-Path $OutputDirectory $name
        $runtime = Join-Path $RuntimePrefix "bin/$name"
        if (Test-Path -LiteralPath $copy) {
            $queue.Enqueue($copy)
            [void]$imports.Add("$relative -> $name [bundled]")
        } elseif (Test-Path -LiteralPath $runtime) {
            Copy-Item -LiteralPath $runtime -Destination $copy
            $queue.Enqueue($copy)
            [void]$imports.Add("$relative -> $name [bundled]")
        } elseif ($name -match '^(api-ms-|ext-ms-)' -or $windowsModules.Contains($name)) {
            [void]$imports.Add("$relative -> $name [Windows]")
        } else {
            [void]$unresolved.Add($name)
        }
    }
}
if ($unresolved.Count) { throw "Unresolved runtime imports: $($unresolved -join ', ')" }
[IO.File]::WriteAllLines((Join-Path $OutputDirectory 'RUNTIME-DEPENDENCIES.txt'), [string[]]$imports)
$licenses = Join-Path $OutputDirectory 'licenses'
New-Item -ItemType Directory -Path $licenses | Out-Null
Copy-Item -LiteralPath (Join-Path $RuntimePrefix 'share/licenses') -Destination (Join-Path $licenses 'runtime') -Recurse
$common = Join-Path (Split-Path -Parent $RuntimePrefix) 'usr/share/licenses/common'
if (Test-Path -LiteralPath $common) { Copy-Item -LiteralPath $common -Destination $licenses -Recurse }
[IO.File]::WriteAllLines((Join-Path $OutputDirectory 'BUILD-INFO.txt'), @(
    'Unofficial mGBA Wireless Adapter local test candidate; not published.',
    "Source commit: $commit",
    "Version: $version",
    "Source archive: $([IO.Path]::GetFileName($SourceArchive))",
    "Source archive SHA-256: $((Get-FileHash -LiteralPath $SourceArchive -Algorithm SHA256).Hash)",
    'Official upstream base: 143adf5aa2a60c6036810b0cd72445e13d74b0e6',
    'Platform: Windows x64 / Qt 5 / MinGW-w64 UCRT',
    'Gameplay testing of this exact package is pending.',
    'No games, ROMs, BIOS, saves, personal controls, or diagnostic capture included.'
))
$manifest = foreach ($file in Get-ChildItem -LiteralPath $OutputDirectory -Recurse -File | Sort-Object FullName) {
    $relative = $file.FullName.Substring($OutputDirectory.Length + 1).Replace('\', '/')
    "$( (Get-FileHash -LiteralPath $file.FullName -Algorithm SHA256).Hash.ToLowerInvariant() )  $relative"
}
[IO.File]::WriteAllLines((Join-Path $OutputDirectory 'SHA256SUMS.txt'), [string[]]$manifest)
Add-Type -AssemblyName System.IO.Compression.FileSystem
[IO.Compression.ZipFile]::CreateFromDirectory($OutputDirectory, $zipPath,
    [IO.Compression.CompressionLevel]::Optimal, $true)
Get-FileHash -LiteralPath $binary, $zipPath -Algorithm SHA256
Write-Output "Packaged $($seen.Count) inspected PE files; no unresolved imports."
