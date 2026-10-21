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

## libnx (ISC)

`tests/tools/gen_hipc_vectors.py` re-implements the message packing of libnx `sf/hipc.h` and `sf/cmif.h` to produce independent test vectors. libnx is distributed under this licence:

```
Copyright 2017-2018 libnx Authors

Permission to use, copy, modify, and/or distribute this software for any purpose with or without fee is hereby granted, provided that the above copyright notice and this permission notice appear in all copies.

THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
```

## Graphics shell (S4b)

The graphics shell (`src/gfx`, `cmake/gfx*.cmake`) downloads the components below at configure time, each pinned by URL and SHA256 (see README.md, "Third-party code fetched at configure time"). The generated NVN tables take names, argument classes and object sizes from the headers named at the end of this section.

### SDL3 (zlib)

Source: SDL 3.4.18, `LICENSE.txt`:

```text
Copyright (C) 1997-2026 Sam Lantinga <slouken@libsdl.org>

This software is provided 'as-is', without any express or implied
warranty.  In no event will the authors be held liable for any damages
arising from the use of this software.

Permission is granted to anyone to use this software for any purpose,
including commercial applications, and to alter it and redistribute it
freely, subject to the following restrictions:

1. The origin of this software must not be misrepresented; you must not
   claim that you wrote the original software. If you use this software
   in a product, an acknowledgment in the product documentation would be
   appreciated but is not required.
2. Altered source versions must be plainly marked as such, and must not be
   misrepresented as being the original software.
3. This notice may not be removed or altered from any source distribution.
```

### volk (MIT)

Copyright (c) 2018-2026 Arseny Kapoulkine. MIT licence, same text as the Wonder notice above.

### VulkanMemoryAllocator (MIT)

Copyright (c) 2017-2026 Advanced Micro Devices, Inc. All rights reserved. MIT licence, same text as the Wonder notice above.

### Dear ImGui (MIT)

Copyright (c) 2014-2026 Omar Cornut. MIT licence, same text as the Wonder notice above.

### Vulkan-Headers (Apache-2.0 OR MIT)

Copyright 2015-2026 The Khronos Group Inc. Files fall under Apache-2.0 or MIT (SPDX `Apache-2.0 OR MIT`); nxrt uses them under MIT.

### NVN interface facts

`src/gfx/nvn/nvn_table.inc` and `nvn_sizes.inc` list NVN function names, argument classes and object sizes. They were read from `nvn.h` of the mp1r-practice-mod repository (MIT notice in the file: "Copyright (c) 2020 h1k421"; the repository as a whole is GPL-2.0) and checked against smo-recomp, ssbu-recomp (both GPL-3.0), SuperMarioWonderRecomp (MIT, Copyright (c) 2026 BelmanteGu) and LASRecomp (MIT, Copyright (c) 2026 Peiikai). No source text is copied into the tables.
