# Runs `nxrt_tests.exe <group>` repeatedly until one run stalls (no new output for -StallSec), then dumps every thread's stack with cdb.
#   pwsh .weaver\hangdump.ps1 kernel_sync -Out C:\temp\hang.txt
param([string]$Group = "kernel_sync", [int]$StallSec = 10, [int]$Runs = 30, [string]$Out = "hang.txt")
$exe = Join-Path (Split-Path -Parent $PSScriptRoot) "build\release\nxrt_tests.exe"
$cdb = "C:\Program Files (x86)\Windows Kits\10\Debuggers\x64\cdb.exe"
for ($i = 1; $i -le $Runs; $i++) {
    $log = Join-Path $env:TEMP "hd_$i.txt"
    $p = Start-Process $exe -ArgumentList $Group -RedirectStandardOutput $log -NoNewWindow -PassThru -WorkingDirectory (Split-Path $exe)
    $last = ""; $since = Get-Date
    while (-not $p.HasExited) {
        Start-Sleep -Milliseconds 500
        $cur = (Get-Content $log -Raw -ErrorAction SilentlyContinue)
        if ($cur -ne $last) { $last = $cur; $since = Get-Date }
        elseif (((Get-Date) - $since).TotalSeconds -gt $StallSec) {
            "run $i stalled; dumping" 
            & $cdb -p $p.Id -c "~*kn 40;q" | Out-File $Out
            Stop-Process -Id $p.Id -Force -ErrorAction SilentlyContinue
            exit 0
        }
    }
    "run $i exit $($p.ExitCode)"
}
