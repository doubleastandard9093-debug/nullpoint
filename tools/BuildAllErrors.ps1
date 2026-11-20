# Builds HeliosDriftEditor one file at a time (no unity batching) so the compiler
# reports errors from every file instead of stopping at the first big batch.
# All output goes to build-log.txt; just the error lines go to build-errors.txt.
#
# Usage (from the repo folder in PowerShell):
#   powershell -ExecutionPolicy Bypass -File tools\BuildAllErrors.ps1
# If Unreal is somewhere else:
#   powershell -ExecutionPolicy Bypass -File tools\BuildAllErrors.ps1 -Engine "D:\UE_5.8"

param(
    [string]$Engine = "E:\My Games Designs\UE_5.8"
)

$Repo    = Split-Path -Parent $PSScriptRoot
$Project = Join-Path $Repo "HeliosDrift.uproject"
$Build   = Join-Path $Engine "Engine\Build\BatchFiles\Build.bat"
$Log     = Join-Path $Repo "build-log.txt"
$Errors  = Join-Path $Repo "build-errors.txt"

if (-not (Test-Path $Build))   { Write-Host "Can't find Build.bat at $Build. Pass -Engine with your Unreal folder." -ForegroundColor Red; exit 1 }
if (-not (Test-Path $Project)) { Write-Host "Can't find $Project" -ForegroundColor Red; exit 1 }

Write-Host "Building every file separately. This is slower than a normal build (10-20 min the first time)..." -ForegroundColor Cyan

& $Build HeliosDriftEditor Win64 Development "-Project=$Project" -WaitMutex -DisableUnity -NoPCH -NoSharedPCH 2>&1 | Tee-Object -FilePath $Log

# Collect unique error lines (compiler, linker, UHT, UBT)
$found = Select-String -Path $Log -Pattern ': (fatal )?error ', 'error LNK', 'Error:' |
         ForEach-Object { $_.Line.Trim() } | Sort-Object -Unique
$found | Set-Content $Errors

$count = ($found | Measure-Object).Count
if ($count -eq 0) {
    Write-Host "`nNo errors found." -ForegroundColor Green
} else {
    Write-Host "`n$count error lines saved to build-errors.txt. Send that file (or paste it) to the bot." -ForegroundColor Yellow
    notepad $Errors
}
