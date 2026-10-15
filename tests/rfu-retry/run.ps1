param([string]$Compiler = 'gcc', [switch]$NativeMutex)
$ErrorActionPreference = 'Stop'
if (Split-Path -Parent $Compiler) {
    $env:PATH = "$(Split-Path -Parent $Compiler);$env:PATH"
}
$testDirectory = $PSScriptRoot
$sourceDirectory = [System.IO.Path]::GetFullPath((Join-Path $testDirectory '..\..'))
$executable = Join-Path $testDirectory $(if ($NativeMutex) { 'rfu-regression-native.exe' } else { 'rfu-regression.exe' })
$arguments = @(
    '-std=c11', '-O1', '-g', '-Wall', '-Wextra'
)
if (-not $NativeMutex) { $arguments += '-DDISABLE_THREADING' }
if ($NativeMutex) {
    $arguments += @('-include', (Join-Path $testDirectory 'native-mutex-check.h'))
}
$arguments += @(
    '-I', (Join-Path $sourceDirectory 'include'),
    (Join-Path $testDirectory 'rfu-regression.c'),
    (Join-Path $sourceDirectory 'src\gba\sio\rfu.c'),
    (Join-Path $sourceDirectory 'src\gba\sio.c'),
    (Join-Path $sourceDirectory 'src\core\timing.c'),
    '-o', $executable
)
& $Compiler @arguments
if ($LASTEXITCODE -ne 0) { throw "RFU regression compilation failed ($LASTEXITCODE)." }
& $executable
if ($LASTEXITCODE -ne 0) { throw "RFU regression failed ($LASTEXITCODE)." }
