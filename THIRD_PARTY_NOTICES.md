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

The nxrt parts that come from smo-recomp and ScarletNX are under GPL-3.0, like nxrt itself (GPL-3.0-or-later). The same holds for the ssbu-recomp references in `include/nxrt/diag.h`, `src/eh/*` (facts read from `runtime/eh_runtime.h` and `runtime/nncxxrt.c`: header layout of the SDK's libc++abi, the non-unique-name bit) and `src/core/hooks.cpp` (idea only). Upstream licence files: smo-recomp `LICENSE` (GPL-3.0), ScarletNX `LICENSE` (GPL-3.0; its source headers say GPL-3.0-or-later), ssbu-recomp `LICENSE` (GPL-3.0). The GPL-3.0 text for nxrt is the `LICENSE` file at the repository root.

## LLVM libc++abi (Apache-2.0 with LLVM exceptions)

`src/eh/typeinfo.cpp`, `src/eh/unwind.cpp`, `include/nxrt/eh.h` and `tests/lift/guest/rt2.h` follow the behaviour and data layouts of LLVM libc++abi (`libcxxabi/src/private_typeinfo.cpp`, `cxa_exception.h`, `cxa_exception.cpp`, `cxa_personality.cpp`), read from the llvm-project repository on 2026-10-21. The code was written from that behaviour and from the Itanium C++ ABI; no libc++abi source text was copied. Licence of the upstream files: Apache License v2.0 with LLVM Exceptions (https://llvm.org/LICENSE.txt), which is compatible with GPL-3.0-or-later for this use.

## recomp `oracle/e2e/rt/eh_rt.cpp` (GPL-3.0-or-later, S1)

`src/eh/unwind.cpp` and `src/eh/catch_call.cpp` rework the table walk and landing protocol of this test-only runtime (S1, same project, same licence). `tests/tools/guest_build.py` and `tests/tools/unicorn_expect.py` import S1's `build_prog.py` and `unicorn_side.py` from `../recomp/oracle/e2e` at build time without copying them.

## libnx (ISC)

`tests/tools/gen_hipc_vectors.py` re-implements the message packing of libnx `sf/hipc.h` and `sf/cmif.h` to produce independent test vectors. libnx is distributed under this licence:

```
Copyright 2017-2018 libnx Authors

Permission to use, copy, modify, and/or distribute this software for any purpose with or without fee is hereby granted, provided that the above copyright notice and this permission notice appear in all copies.

THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
```
