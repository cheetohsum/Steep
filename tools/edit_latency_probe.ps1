param(
    [string]$Label = 'baseline',
    [string]$Executable = 'C:\msys64\home\alexr\build-hw\Release\steep.exe',
    [int]$Steps = 100,
    [int]$IntervalMs = 30,
    [int]$Target = 0,
    [string]$Photo = '',
    [switch]$WithProfile,
    [switch]$DumpFrames
)
$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
$run = Join-Path $root ('test-output\edit-latency\' + $Label)
foreach ($dir in @($run, "$run\settings", "$run\cache", "$run\photos")) {
    New-Item -ItemType Directory -Path $dir -Force | Out-Null
}
$sample = "$run\photos\Sample.RAF"
if ($Photo) {
    $sample = (Resolve-Path -LiteralPath $Photo).Path
} elseif (!(Test-Path -LiteralPath $sample)) {
    Copy-Item -LiteralPath "$root\test-output\view-switch\photos\Sample-000.RAF" -Destination $sample
}
if ($WithProfile) {
    if ($Photo) { throw 'Use copied fixtures, not -WithProfile, with an explicit -Photo.' }
    Copy-Item -LiteralPath 'D:\Media\Photos\February 2026\DSCF8535.RAF.pp3' -Destination "$sample.pp3"
}
$options = Get-Content -LiteralPath "$root\test-output\gradient-popup\settings\options"
$options = $options | ForEach-Object {
    if ($_ -match '^StartupPath=') { 'StartupPath=' + (Split-Path $sample).Replace('\', '/') }
    elseif ($_ -match '^Verbose=') { 'Verbose=false' }
    else { $_ }
}
[System.IO.File]::WriteAllLines("$run\settings\options", $options)
$env:PATH = 'C:\msys64\mingw64\bin;C:\msys64\usr\bin;C:\Windows\System32;C:\Windows'
Get-ChildItem Env: | Where-Object { $_.Name -match '^STEEP_' } | ForEach-Object {
    Remove-Item -LiteralPath ('Env:' + $_.Name)
}
$env:RT_SETTINGS = "$run\settings"
$env:RT_CACHE = "$run\cache"
$env:USERPROFILE = $run
$env:STEEP_EDIT_TRACE = '1'
$env:STEEP_EDIT_TRACE_VERBOSE = '1'
$env:STEEP_EDIT_BENCH = "$Steps"
$env:STEEP_EDIT_BENCH_INTERVAL_MS = "$IntervalMs"
$env:STEEP_EDIT_BENCH_REALDRAG = '1'
$env:STEEP_EDIT_BENCH_TARGET = "$Target"
if ($DumpFrames) {
    New-Item -ItemType Directory -Path "$run\frames" -Force | Out-Null
    $env:STEEP_DUMP_CROP = "$run\frames"
}
$process = Start-Process -FilePath $Executable -ArgumentList @('-w', ('"' + $sample + '"')) `
    -WorkingDirectory (Split-Path $Executable) -RedirectStandardOutput "$run\stdout.log" `
    -RedirectStandardError "$run\stderr.log" -PassThru
[pscustomobject]@{Pid=$process.Id; Run=$run}
