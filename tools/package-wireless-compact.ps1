param(
    [string]$BuildDirectory = 'build-compact-static',
    [Parameter(Mandatory = $true)][string]$RuntimePrefix,
    [Parameter(Mandatory = $true)][string]$StaticQtPrefix,
    [string]$StaticEpoxyPrefix = 'build-compact-deps/epoxy-static',
    [Parameter(Mandatory = $true)][string]$FFmpegSourceDirectory,
    [Parameter(Mandatory = $true)][string]$SourceArchive,
    [string]$OutputDirectory = 'build-compact-static/dist/mGBA-Wireless-Adapter-compact-win64'
)
$ErrorActionPreference = 'Stop'
$root = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
function Resolve-Input([string]$path) {
    if (-not [IO.Path]::IsPathRooted($path)) { $path = Join-Path $root $path }
    return (Resolve-Path -LiteralPath $path).Path
}
$BuildDirectory = Resolve-Input $BuildDirectory
$RuntimePrefix = Resolve-Input $RuntimePrefix
$StaticQtPrefix = Resolve-Input $StaticQtPrefix
$StaticEpoxyPrefix = Resolve-Input $StaticEpoxyPrefix
$FFmpegSourceDirectory = Resolve-Input $FFmpegSourceDirectory
$SourceArchive = Resolve-Input $SourceArchive
if (-not [IO.Path]::IsPathRooted($OutputDirectory)) { $OutputDirectory = Join-Path $root $OutputDirectory }
$OutputDirectory = [IO.Path]::GetFullPath($OutputDirectory).TrimEnd([char[]]'\/')
if ($OutputDirectory -eq [IO.Path]::GetPathRoot($OutputDirectory).TrimEnd([char[]]'\/')) { throw 'Output must not be a filesystem root.' }
$zipPath = "$OutputDirectory.zip"
if ((Test-Path -LiteralPath $OutputDirectory) -or (Test-Path -LiteralPath $zipPath)) { throw 'Output already exists; choose a new path.' }
$commit = & git -C $root rev-parse HEAD
if ($LASTEXITCODE -ne 0) { throw 'Cannot record source commit.' }
if (& git -C $root status --porcelain --untracked-files=all) { throw 'Commit outstanding source changes before packaging.' }
$versionSource = [IO.File]::ReadAllText((Join-Path $BuildDirectory 'version.c'))
$version = [regex]::Match($versionSource, 'projectVersion = "([^"]+)"').Groups[1].Value
if (-not $version -or $version.EndsWith('-dirty') -or $versionSource -notmatch ('gitCommit = "' + [regex]::Escape($commit) + '"')) {
    throw 'Reconfigure and rebuild from the clean source commit.'
}
$objdump = Join-Path $RuntimePrefix 'bin/objdump.exe'
$binaries = @('mGBA.exe', 'mgba-sdl.exe')
$windows = [Collections.Generic.HashSet[string]]::new([StringComparer]::OrdinalIgnoreCase)
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
'@ -split '\s+' | Where-Object { $_ } | ForEach-Object { [void]$windows.Add($_) }
$imports = [Collections.Generic.SortedSet[string]]::new([StringComparer]::OrdinalIgnoreCase)
foreach ($name in $binaries) {
    $file = Join-Path $BuildDirectory $name
    $binaryText = [Text.Encoding]::ASCII.GetString([IO.File]::ReadAllBytes($file))
    if (-not $binaryText.Contains($commit) -or -not $binaryText.Contains($version)) { throw "Source/version mismatch: $name" }
    $details = & $objdump -p $file 2>&1
    if ($LASTEXITCODE -ne 0) { throw "Cannot inspect $name" }
    foreach ($line in $details) {
        if ($line -notmatch '^\s*DLL Name:\s*(.+?)\s*$') { continue }
        $module = $Matches[1]
        if ($module -notmatch '^(api-ms-|ext-ms-)' -and -not $windows.Contains($module)) {
            throw "Compact binary still imports non-system DLL: $name -> $module"
        }
        [void]$imports.Add("$name -> $module [Windows]")
    }
}
New-Item -ItemType Directory -Path $OutputDirectory | Out-Null
foreach ($name in $binaries) { Copy-Item -LiteralPath (Join-Path $BuildDirectory $name) -Destination $OutputDirectory }
Copy-Item -LiteralPath (Join-Path $PSScriptRoot 'wireless-portable/portable.ini') -Destination $OutputDirectory
Copy-Item -LiteralPath (Join-Path $root 'LICENSE') -Destination (Join-Path $OutputDirectory 'LICENSE.txt')
Copy-Item -LiteralPath (Join-Path $root 'CHANGES') -Destination (Join-Path $OutputDirectory 'CHANGES.txt')
Copy-Item -LiteralPath (Join-Path $root 'res/nointro.dat') -Destination $OutputDirectory
Copy-Item -LiteralPath (Join-Path $root 'res/scripts') -Destination $OutputDirectory -Recurse
$shaderOutput = Join-Path $OutputDirectory 'shaders'
New-Item -ItemType Directory -Path $shaderOutput | Out-Null
Add-Type -AssemblyName System.IO.Compression.FileSystem
foreach ($shader in Get-ChildItem -LiteralPath (Join-Path $root 'res/shaders') -Directory -Filter '*.shader') {
    [IO.Compression.ZipFile]::CreateFromDirectory($shader.FullName, (Join-Path $shaderOutput $shader.Name))
}
$docs = Join-Path $OutputDirectory 'docs'
New-Item -ItemType Directory -Path $docs | Out-Null
foreach ($name in @('WIRELESS-ADAPTER.md', 'WIRELESS-RELEASE-NOTES.md', 'WIRELESS-BUILD.md')) {
    Copy-Item -LiteralPath (Join-Path $root "doc/$name") -Destination $docs
}
Copy-Item -LiteralPath (Join-Path $PSScriptRoot 'wireless-portable/COMPACT-README.md') -Destination (Join-Path $OutputDirectory 'README.md')
Copy-Item -LiteralPath (Join-Path $PSScriptRoot 'wireless-portable/COMPACT-NOTICES.md') -Destination $docs
[IO.File]::WriteAllLines((Join-Path $docs 'RUNTIME-DEPENDENCIES.txt'), [string[]]$imports)
$licenses = Join-Path $OutputDirectory 'licenses'
New-Item -ItemType Directory -Path $licenses | Out-Null
# Aggregate complete notices without hundreds of loose runtime notice files.
# This broad collection intentionally retains some unused toolchain notices.
$noticeRoots = @((Join-Path $RuntimePrefix 'share/licenses'), (Join-Path $StaticQtPrefix 'share/licenses'), (Join-Path $StaticEpoxyPrefix 'share/licenses'))
$common = Join-Path (Split-Path -Parent $RuntimePrefix) 'usr/share/licenses/common'
if (Test-Path -LiteralPath $common) { $noticeRoots += $common }
$builder = [Text.StringBuilder]::new()
foreach ($noticeRoot in $noticeRoots) {
    foreach ($file in Get-ChildItem -LiteralPath $noticeRoot -Recurse -File | Sort-Object FullName) {
        [void]$builder.AppendLine("=== $($file.FullName.Substring($noticeRoot.Length + 1).Replace('\', '/')) ===")
        [void]$builder.AppendLine([IO.File]::ReadAllText($file.FullName))
        [void]$builder.AppendLine()
    }
}
foreach ($relative in @('inih/LICENSE.txt', 'discord-rpc/LICENSE', 'discord-rpc/include/rapidjson/license.txt', 'discord-rpc/include/mingw-std-threads/LICENSE')) {
    [void]$builder.AppendLine("=== mGBA src/third-party/$relative ===")
    [void]$builder.AppendLine([IO.File]::ReadAllText((Join-Path $root "src/third-party/$relative")))
    [void]$builder.AppendLine()
}
$nvHeaders = Join-Path $root 'build-compact-deps/nv-codec-headers-c69278340ab1d5559c7d7bf0edf615dc33ddbba7/include/ffnvcodec'
foreach ($file in Get-ChildItem -LiteralPath $nvHeaders -File -Filter '*.h') {
    [void]$builder.AppendLine("=== NV codec headers c69278340: $($file.Name) ===")
    $notice = [regex]::Match([IO.File]::ReadAllText($file.FullName), '(?s)\A\s*/\*.*?\*/').Value
    if (-not $notice) { throw "Missing NV codec header notice: $($file.Name)" }
    [void]$builder.AppendLine($notice)
    [void]$builder.AppendLine()
}
[IO.File]::WriteAllText((Join-Path $licenses 'DEPENDENCY-LICENSES.txt'), $builder.ToString())
Copy-Item -LiteralPath (Join-Path $root 'build-compact-deps/libelf-COPYING.LIB') -Destination $licenses
Copy-Item -LiteralPath (Join-Path $root 'build-compact-deps/libelf-COPYING.LIB.provenance.json') -Destination $docs
foreach ($file in Get-ChildItem -LiteralPath $FFmpegSourceDirectory -File -Filter 'COPYING*') {
    Copy-Item -LiteralPath $file.FullName -Destination (Join-Path $licenses ('FFmpeg-' + $file.Name))
}
Copy-Item -LiteralPath (Join-Path $FFmpegSourceDirectory 'LICENSE.md') -Destination (Join-Path $licenses 'FFmpeg-LICENSE.md')
$qtNotices = Join-Path $root 'build-compact-deps/qt-third-party-notices'
if (-not (Test-Path -LiteralPath $qtNotices -PathType Container)) { throw 'Collect the matching Qt bundled third-party notices first.' }
Copy-Item -LiteralPath (Join-Path $qtNotices 'THIRD-PARTY-NOTICES-Qt-5.15.19.txt') -Destination $licenses
Copy-Item -LiteralPath (Join-Path $qtNotices 'download-manifest.json') -Destination (Join-Path $docs 'QT-NOTICE-PROVENANCE.json')
Copy-Item -LiteralPath (Join-Path $qtNotices 'README.md') -Destination (Join-Path $docs 'QT-NOTICES.md')
[IO.File]::WriteAllLines((Join-Path $docs 'BUILD-INFO.txt'), @(
    'Unofficial mGBA Wireless Adapter compact local test candidate; not published.',
    "Source commit: $commit", "Version: $version",
    "Source archive: $([IO.Path]::GetFileName($SourceArchive))",
    "Source archive SHA-256: $((Get-FileHash -LiteralPath $SourceArchive -Algorithm SHA256).Hash)",
    'Official upstream base: 143adf5aa2a60c6036810b0cd72445e13d74b0e6',
    'Platform: Windows x64 / static Qt 5 / MinGW-w64 UCRT',
    'OpenGL dispatch: mGBA libepoxy fork 8b7513e88c4f23b6b8d6757b306e431af79df1ff, built with static PE TLS callback.',
    'No third-party runtime DLLs or external Qt plugins are needed.',
    'Gameplay testing of this exact package is pending.',
    'No games, ROMs, BIOS, saves, personal controls, or diagnostic capture included.'
))
$manifest = foreach ($file in Get-ChildItem -LiteralPath $OutputDirectory -Recurse -File | Sort-Object FullName) {
    $relative = $file.FullName.Substring($OutputDirectory.Length + 1).Replace('\', '/')
    "$( (Get-FileHash -LiteralPath $file.FullName -Algorithm SHA256).Hash.ToLowerInvariant() )  $relative"
}
[IO.File]::WriteAllLines((Join-Path $docs 'SHA256SUMS.txt'), [string[]]$manifest)
[IO.Compression.ZipFile]::CreateFromDirectory($OutputDirectory, $zipPath, [IO.Compression.CompressionLevel]::Optimal, $true)
Get-FileHash -LiteralPath $zipPath -Algorithm SHA256
Write-Output 'Compact package imports only Windows system libraries.'
