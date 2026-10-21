# Third-party notices

The nxrt files below name SuperMarioWonderRecomp as a source in their header comments, and the provenance table in README.md lists them: include/nxrt/diag.h, include/nxrt/dispatch.h, include/nxrt/ipc.h, include/nxrt/kernel.h, include/nxrt/memory.h, include/nxrt/module.h, include/nxrt/svc.h, src/core/dispatch.cpp, src/diag/memguard.cpp, src/diag/watchdog.cpp, src/ipc/ipc.cpp, src/ipc/sm.cpp, src/kernel/handles.cpp, src/kernel/kernel_sync.cpp, src/kernel/svc.cpp, src/kernel/svc_info.cpp, src/kernel/svc_memory.cpp, src/kernel/svc_thread.cpp, src/kernel/thread.cpp, src/kernel/time.cpp, src/loader/module.cpp.

## SuperMarioWonderRecomp (MIT)

Source: `refs/SuperMarioWonderRecomp/LICENSE`, reproduced verbatim:

```text
MIT License

Copyright (c) 2026 BelmanteGu

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
```

## smo-recomp, ScarletNX and ssbu-recomp (GPL-3.0)

The nxrt parts that come from smo-recomp and ScarletNX are under GPL-3.0, like nxrt itself (GPL-3.0-or-later). The same holds for the ssbu-recomp reference in `include/nxrt/diag.h` and `src/core/abi_glue.cpp`. Upstream licence files: smo-recomp `LICENSE` (GPL-3.0), ScarletNX `LICENSE` (GPL-3.0; its source headers say GPL-3.0-or-later), ssbu-recomp `LICENSE` (GPL-3.0). The GPL-3.0 text for nxrt is the `LICENSE` file at the repository root.
