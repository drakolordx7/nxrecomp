# Runs nxrt_tests.exe <arg> N times at a given process priority class (and optional NXRT_SCHED_CONSOLE_CORES), prints rc, ms and failed checks.
#   pwsh .weaver\runcls.ps1 -Arg kernel_sync -Class AboveNormal -N 5 [-Cores 0]
param([string]$Arg = "kernel_sync", [string]$Class = "Normal", [int]$N = 5, [string]$Cores = "")
$exe = Join-Path (Split-Path -Parent $PSScriptRoot) "build\release\nxrt_tests.exe"
$bad = 0
1..$N | ForEach-Object {
    $sw = [Diagnostics.Stopwatch]::StartNew()
    $psi = New-Object Diagnostics.ProcessStartInfo $exe, $Arg
    $psi.UseShellExecute = $false; $psi.RedirectStandardOutput = $true; $psi.WorkingDirectory = Split-Path $exe
    if ($Cores -ne "") { $psi.Environment["NXRT_SCHED_CONSOLE_CORES"] = $Cores }
    $p = [Diagnostics.Process]::Start($psi); $p.PriorityClass = $Class
    $t = $p.StandardOutput.ReadToEndAsync(); $p.WaitForExit(); $o = $t.Result
    $f = ($o -split "`n" | Where-Object { $_ -match 'FAILED|WATCHDOG' } | ForEach-Object { $s = $_.Trim() -replace '.*test_kernel_sync.cpp:', 'L'; $s.Substring(0, [Math]::Min(70, $s.Length)) }) -join ' | '
    if ($p.ExitCode -ne 0) { $bad++ }
    "run $_ rc=$($p.ExitCode) $([int]$sw.Elapsed.TotalMilliseconds) ms $f"
}
"failed runs: $bad of $N"
