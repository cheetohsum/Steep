param([string]$Build = 'C:\msys64\home\alexr\build-hw')
$ErrorActionPreference = 'Stop'
$source = Join-Path $Build 'Release'
$runtime = Join-Path (Split-Path $PSScriptRoot -Parent) 'test-output\view-switch\runtime'
New-Item -ItemType Directory -Path $runtime -Force | Out-Null
# Share immutable runtime assets; keep the executable separate from the live app.
Get-ChildItem -LiteralPath $source -File -Recurse | ForEach-Object {
    $relative = $_.FullName.Substring($source.Length + 1)
    if ($relative -ne 'steep.exe') {
        $target = Join-Path $runtime $relative
        if (!(Test-Path -LiteralPath $target)) {
            New-Item -ItemType Directory -Path (Split-Path $target -Parent) -Force | Out-Null
            New-Item -ItemType HardLink -Path $target -Target $_.FullName | Out-Null
        }
    }
}
Copy-Item -LiteralPath (Join-Path $Build 'rtgui\steep.exe') -Destination (Join-Path $runtime 'steep.exe')
Write-Output $runtime
