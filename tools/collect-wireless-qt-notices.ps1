# Download exact-version Qt bundled notices; no app or user configuration writes.
param([string]$Destination = '')
$ErrorActionPreference = 'Stop'
$repository = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$expected = [IO.Path]::GetFullPath((Join-Path $repository 'build-compact-deps/qt-third-party-notices'))
if (-not $Destination) { $Destination = $expected }
$Destination = [IO.Path]::GetFullPath($Destination)
if ($Destination -ne $expected) {
    throw 'This pinned collector only writes within build-compact-deps/qt-third-party-notices.'
}
$utf8 = [Text.UTF8Encoding]::new($false)
$manifest = [Collections.Generic.List[object]]::new()
$downloaded = @{}
$previous = @{}
$previousManifest = Join-Path $Destination 'download-manifest.json'
if (Test-Path -LiteralPath $previousManifest) {
    foreach ($entry in @(Get-Content -Raw -Encoding UTF8 -LiteralPath $previousManifest | ConvertFrom-Json)) {
        $previous[$entry.File] = $entry
    }
}
$release = 'v5.15.19-lts-lgpl'
$superCommit = 'dc2ac680fa9d0ef7b0d9520859593d13951bedea'
$modules = [ordered]@{
    qtbase = 'c68297f66533200c9ff626b056d096c78b4b94db'
    qtimageformats = 'ba9056b12830318729e88d7444ea6a13e3f38e43'
    qtsvg = '62c4a8da3114174ea5b2c4ea46d2ab848066d534'
    qtmultimedia = '54f54ab798ddfd16bfc2babaf94efb4a824d2379'
}
function Save-Download([string]$RelativePath, [string]$Url, [string]$Purpose) {
    if ($downloaded.ContainsKey($RelativePath)) { return (Join-Path $Destination $RelativePath) }
    $path = [IO.Path]::GetFullPath((Join-Path $Destination $RelativePath))
    if (-not $path.StartsWith($Destination + [IO.Path]::DirectorySeparatorChar, [StringComparison]::OrdinalIgnoreCase)) {
        throw "Unsafe destination: $RelativePath"
    }
    New-Item -ItemType Directory -Force -Path (Split-Path -Parent $path) | Out-Null
    $old = $previous[$RelativePath.Replace('\', '/')]
    $reusable = $old -and $old.Url -eq $Url -and (Test-Path -LiteralPath $path -PathType Leaf) -and
        (Get-FileHash -Algorithm SHA256 -LiteralPath $path).Hash.ToLowerInvariant() -eq $old.SHA256
    if (-not $reusable) { Invoke-WebRequest -Uri $Url -OutFile $path }
    $manifest.Add([pscustomobject]@{
        File = $RelativePath.Replace('\', '/')
        Url = $Url
        SHA256 = (Get-FileHash -Algorithm SHA256 -LiteralPath $path).Hash.ToLowerInvariant()
        Purpose = $Purpose
    })
    $downloaded[$RelativePath] = $true
    return $path
}
function Save-QtFile([string]$Module, [string]$Path, [string]$Purpose) {
    $url = 'https://raw.githubusercontent.com/qt/' + $Module + '/' + $modules[$Module] + '/' + $Path
    return Save-Download ($Module + '/' + $Path) $url $Purpose
}

# Verify official Qt's annotated release tag and module gitlinks, not just a
# moving 5.15 branch. Preserve the API evidence alongside the downloaded text.
$tagPath = Save-Download 'provenance/qt5-release-tag.json' 'https://api.github.com/repos/qt/qt5/git/tags/26a60f4d8accfc287789ed73be100f66b244ddd3' 'Official annotated Qt 5.15.19 release tag'
$tag = Get-Content -Raw -Encoding UTF8 -LiteralPath $tagPath | ConvertFrom-Json
if ($tag.tag -ne $release -or $tag.object.sha -ne $superCommit) { throw 'Qt release tag mismatch' }
$treePath = Save-Download 'provenance/qt5-release-tree.json' ('https://api.github.com/repos/qt/qt5/git/trees/' + $superCommit) 'Qt release module revisions'
$tree = Get-Content -Raw -Encoding UTF8 -LiteralPath $treePath | ConvertFrom-Json
foreach ($module in $modules.Keys) {
    $entry = $tree.tree | Where-Object { $_.path -eq $module }
    if ($entry.sha -ne $modules[$module]) { throw "Qt module pin mismatch: $module" }
}
$pkgbuild = Save-Download 'provenance/msys2-qt5-static-PKGBUILD' 'https://raw.githubusercontent.com/msys2/MINGW-packages/d0aab16fc62279e70b50b9f8f657872e86ee1dee/mingw-w64-qt5-static/PKGBUILD' 'Matching MSYS2 static Qt package build recipe'
$pkgHash = (Get-FileHash -Algorithm SHA256 -LiteralPath $pkgbuild).Hash.ToLowerInvariant()
if ($pkgHash -ne '0fe05984a674cb1f320d260bc7490192f733142fd93a67b540733edd7f805c8c') {
    throw 'MSYS2 PKGBUILD differs from the exact .BUILDINFO of the extracted binary package'
}

# These are the Windows QtCore/QtGui portions used by the frontend and its
# default Windows, style, audio and image plugins. Excluded are Android/macOS,
# XCB, DBus, tests, examples, QML and unused development-tool attributions.
$attributions = [ordered]@{
    qtbase = @(
        'src/3rdparty/double-conversion/qt_attribution.json',
        'src/3rdparty/easing/qt_attribution.json',
        'src/3rdparty/freetype/qt_attribution.json',
        'src/3rdparty/harfbuzz-ng/qt_attribution.json',
        'src/3rdparty/harfbuzz/qt_attribution.json',
        'src/3rdparty/iaccessible2/qt_attribution.json',
        'src/3rdparty/icc/qt_attribution.json',
        'src/3rdparty/libjpeg/qt_attribution.json',
        'src/3rdparty/libpng/qt_attribution.json',
        'src/3rdparty/md4/qt_attribution.json',
        'src/3rdparty/md4c/qt_attribution.json',
        'src/3rdparty/md5/qt_attribution.json',
        'src/3rdparty/pcre2/qt_attribution.json',
        'src/3rdparty/pixman/qt_attribution.json',
        'src/3rdparty/rfc6234/qt_attribution.json',
        'src/3rdparty/sha1/qt_attribution.json',
        'src/3rdparty/sha3/qt_attribution.json',
        'src/3rdparty/tinycbor/qt_attribution.json',
        'src/3rdparty/wintab/qt_attribution.json',
        'src/corelib/codecs/qt_attribution.json',
        'src/corelib/io/qt_attribution.json',
        'src/corelib/mimetypes/3rdparty/qt_attribution.json',
        'src/corelib/text/qt_attribution.json',
        'src/gui/opengl/qt_attribution.json',
        'src/gui/painting/qt_attribution.json',
        'src/gui/text/qt_attribution.json',
        'src/gui/vulkan/qt_attribution.json'
    )
    qtimageformats = @(
        'src/3rdparty/libtiff/qt_attribution.json',
        'src/3rdparty/libwebp/qt_attribution.json'
    )
    qtsvg = @('src/svg/qt_attribution.json')
}
$components = [Collections.Generic.List[object]]::new()
foreach ($module in $attributions.Keys) {
    foreach ($attribution in $attributions[$module]) {
        $file = Save-QtFile $module $attribution 'Official bundled component attribution, including version and copyright'
        $records = @(Get-Content -Raw -Encoding UTF8 -LiteralPath $file | ConvertFrom-Json)
        foreach ($record in $records) {
            $components.Add([pscustomobject]@{
                Module = $module
                Revision = $modules[$module]
                Attribution = $attribution
                Component = $record
            })
            $files = @()
            if ($record.LicenseFile) { $files += $record.LicenseFile }
            if ($record.LicenseFiles) { $files += $record.LicenseFiles }
            foreach ($license in $files) {
                $base = Split-Path -Parent $attribution
                $resolved = [IO.Path]::GetFullPath((Join-Path (Join-Path $Destination $module) (Join-Path $base $license)))
                $moduleRoot = (Join-Path $Destination $module) + [IO.Path]::DirectorySeparatorChar
                if (-not $resolved.StartsWith($moduleRoot, [StringComparison]::OrdinalIgnoreCase)) { throw "Invalid attribution path: $license" }
                $path = $resolved.Substring($moduleRoot.Length).Replace('\', '/')
                Save-QtFile $module $path 'Exact license text referenced by the bundled attribution' | Out-Null
            }
        }
    }
    Write-Output "Collected pinned $module notices"
}
# Supplement referenced notices with FreeType's individual license choices,
# JPEG's full copyright inventory, WebP's patent grant, and the Apache-2.0
# license used by the embedded MIME definition table.
foreach ($path in @('src/3rdparty/freetype/docs/FTL.TXT', 'src/3rdparty/freetype/docs/GPLv2.TXT', 'src/3rdparty/libjpeg/COPYRIGHT.txt', 'LICENSE.LGPLv3', 'LICENSE.GPL3')) {
    Save-QtFile 'qtbase' $path 'Supplementary Qt/third-party license or copyright text' | Out-Null
}
Save-QtFile 'qtimageformats' 'src/3rdparty/libwebp/PATENTS' 'WebP patent grant' | Out-Null
Save-QtFile 'qtbase' 'src/corelib/mimetypes/3rdparty/tika-mimetypes.xml' 'Exact embedded MIME database, including upstream Apache license header' | Out-Null
Save-Download 'common/LICENSE-Apache-2.0.txt' 'https://www.apache.org/licenses/LICENSE-2.0.txt' 'Apache-2.0 license text for the bundled Tika MIME database' | Out-Null

$allNoticeText = [Text.StringBuilder]::new()
[void]$allNoticeText.AppendLine('Qt 5.15.19 bundled third-party notices')
[void]$allNoticeText.AppendLine('Pinned official release: ' + $release + ' / ' + $superCommit)
[void]$allNoticeText.AppendLine('Each component below retains the exact upstream attribution and referenced license text.')
[void]$allNoticeText.AppendLine('The source manifest records exact URLs and SHA-256 hashes. This is a notice collection, not complete corresponding source or relink material.')
foreach ($component in $components) {
    [void]$allNoticeText.AppendLine("`n============================================================")
    [void]$allNoticeText.AppendLine($component.Component.Name + ' [' + $component.Module + ' ' + $component.Revision + ']')
    [void]$allNoticeText.AppendLine(($component.Component | ConvertTo-Json -Depth 10))
}
foreach ($entry in $manifest | Where-Object { $_.Purpose -match 'license|copyright|patent' -and $_.File -notmatch 'qt_attribution\.json|tika-mimetypes\.xml' }) {
    [void]$allNoticeText.AppendLine("`n============================================================")
    [void]$allNoticeText.AppendLine($entry.File)
    [void]$allNoticeText.AppendLine($entry.Url)
    [void]$allNoticeText.AppendLine((Get-Content -Raw -Encoding UTF8 -LiteralPath (Join-Path $Destination $entry.File)))
}
[IO.File]::WriteAllText((Join-Path $Destination 'THIRD-PARTY-NOTICES-Qt-5.15.19.txt'), $allNoticeText.ToString(), $utf8)
[IO.File]::WriteAllText((Join-Path $Destination 'download-manifest.json'), ($manifest | ConvertTo-Json -Depth 6), $utf8)
[IO.File]::WriteAllText((Join-Path $Destination 'components.json'), ($components | ConvertTo-Json -Depth 12), $utf8)
Write-Output ("Collected {0} components; {1} exact source files; consolidated notices: {2} bytes" -f $components.Count, $manifest.Count, (Get-Item (Join-Path $Destination 'THIRD-PARTY-NOTICES-Qt-5.15.19.txt')).Length)
