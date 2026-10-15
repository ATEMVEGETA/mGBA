param([string]$Compiler = 'gcc')
$ErrorActionPreference = 'Stop'
if (Split-Path -Parent $Compiler) {
    $env:PATH = "$(Split-Path -Parent $Compiler);$env:PATH"
}
$sourceDirectory = [System.IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..\..'))
$executable = Join-Path $PSScriptRoot 'rfu-cooperative-native.exe'
$arguments = @(
    '-std=c11', '-O1', '-g', '-Wall', '-Wextra',
    '-include', (Join-Path $PSScriptRoot 'native-mutex-check.h'),
    '-I', (Join-Path $sourceDirectory 'include'),
    (Join-Path $PSScriptRoot 'rfu-cooperative-native.c'),
    (Join-Path $sourceDirectory 'src\gba\sio\rfu.c'),
    (Join-Path $sourceDirectory 'src\gba\sio.c'),
    (Join-Path $sourceDirectory 'src\core\timing.c'),
    '-o', $executable
)
& $Compiler @arguments
if ($LASTEXITCODE -ne 0) { throw "Native cooperative stress compilation failed ($LASTEXITCODE)." }
& $executable
if ($LASTEXITCODE -ne 0) { throw "Native cooperative stress failed ($LASTEXITCODE)." }
