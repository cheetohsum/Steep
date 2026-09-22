param(
    [string]$Label = 'baseline',
    [int]$Count = 96,
    [int]$Cycles = 6,
    [int]$PeriodMs = 1500,
    [switch]$ColdCache,
    [switch]$Edit,
    [switch]$Advance,
    [switch]$WaitForExit,
    [string]$Executable = 'C:\msys64\home\alexr\build-hw\Release\steep.exe'
)
$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
$fixture = Join-Path $root 'test-output\view-switch'
$photos = Join-Path $fixture 'photos'
$settings = Join-Path $fixture 'settings'
$cache = Join-Path $fixture $(if ($ColdCache) { "cache-$Label" } else { 'cache' })
$run = Join-Path $fixture $Label
foreach ($dir in @($photos, $settings, $cache, $run)) {
    New-Item -ItemType Directory -Path $dir -Force | Out-Null
}
$sample = Join-Path $photos 'Sample-000.RAF'
if (!(Test-Path -LiteralPath $sample)) {
    Copy-Item -LiteralPath 'D:\Media\Photos\February 2026\DSCF8535.RAF' -Destination $sample
}
for ($i = 1; $i -lt $Count; $i++) {
    $link = Join-Path $photos ('Sample-{0:D3}.RAF' -f $i)
    if (!(Test-Path -LiteralPath $link)) {
        New-Item -ItemType HardLink -Path $link -Target $sample | Out-Null
    }
}
$sourceOptions = Join-Path $root 'test-output\gradient-popup\settings\options'
$options = Get-Content -LiteralPath $sourceOptions
$options = $options | ForEach-Object {
    if ($_ -match '^StartupPath=') { 'StartupPath=' + $photos.Replace('\', '/') }
    elseif ($_ -match '^Verbose=') { 'Verbose=false' }
    else { $_ }
}
[System.IO.File]::WriteAllLines((Join-Path $settings 'options'), $options)
$env:PATH = 'C:\msys64\mingw64\bin;C:\msys64\usr\bin;C:\Windows\System32;C:\Windows'
$env:RT_SETTINGS = $settings
$env:RT_CACHE = $cache
$env:USERPROFILE = $run
$env:STEEP_FILESEL_LOG = '1'
$env:STEEP_SWITCH_SELFTEST = "$Cycles"
$env:STEEP_SWITCH_SELFTEST_DELAY_MS = '5000'
$env:STEEP_SWITCH_SELFTEST_PERIOD_MS = "$PeriodMs"
Remove-Item Env:STEEP_SWITCH_SELFTEST_EDIT, Env:STEEP_SWITCH_SELFTEST_ADVANCE -ErrorAction SilentlyContinue
if ($Edit) {
    if ($PeriodMs -lt 4500) { throw 'Edit mode requires PeriodMs >= 4500 for the deferred exposure action.' }
    $env:STEEP_SWITCH_SELFTEST_EDIT = '1'
}
if ($Advance) { $env:STEEP_SWITCH_SELFTEST_ADVANCE = '1' }
$process = Start-Process -FilePath $Executable -ArgumentList '-w' -WorkingDirectory (Split-Path $Executable) `
    -RedirectStandardOutput (Join-Path $run 'stdout.log') -RedirectStandardError (Join-Path $run 'stderr.log') -PassThru -Wait:$WaitForExit
[pscustomobject]@{Pid=$process.Id; Run=$run; Cycles=$Cycles; PeriodMs=$PeriodMs}
if ($WaitForExit) {
    Write-Output "Steep test exit code: $($process.ExitCode)"
    exit $process.ExitCode
}
