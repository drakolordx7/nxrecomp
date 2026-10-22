# Tools
- hangdump.ps1: runs `nxrt_tests.exe <group>` repeatedly until a run stalls, then dumps every thread's stack with cdb. `pwsh .weaver\hangdump.ps1 kernel_sync -Out hang.txt`
- runcls.ps1: runs `nxrt_tests.exe <arg>` N times at a chosen process priority class (optional NXRT_SCHED_CONSOLE_CORES), prints exit code, time and failed checks. `pwsh .weaver\runcls.ps1 -Arg kernel_sync -Class Normal -N 20`
