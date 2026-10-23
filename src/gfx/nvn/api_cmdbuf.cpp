// SPDX-License-Identifier: GPL-3.0-or-later
// NVNcommandBuffer lifecycle: initialize/finalize, command and control memory bookkeeping, Begin/EndRecording, handles,
// Call/CopyCommands. The recorders of the commands themselves are api_cmd_state.cpp and api_cmd_work.cpp.
//
// Command memory model (idea from SuperMarioWonderRecomp docs/nvn/DEBUG.md #1, #26, DEBUG-2 #90: the code is new): the game
// hands over memory (AddCommandMemory / AddControlMemory) and looks at how much a recording used right after EndRecording (a
// list that used nothing is dropped) and at how much is free (to add memory in time). The back end records into host lists, so
// usage is an estimate: 16 bytes of command memory and 4 bytes of control memory per record, growing across recordings, given
// back when memory is added, never reaching the total. The memory callback is never needed and never called.
#include <algorithm>

#include "nvn_cmd.h"
#include "nvn_impl.h"

namespace nxrt::nvn {
using namespace be;
namespace {

constexpr uint64_t kCmdBytesPerRecord = 16, kCtlBytesPerRecord = 4, kReserve = 64;

CommandBuffer* cb_of(NVNcommandBuffer* c) { return host_of<CommandBuffer>(c); }
const CommandBuffer* cb_of(const NVNcommandBuffer* c) { return host_of<CommandBuffer>(c); }

NVNboolean nvnCommandBufferInitialize(NVNcommandBuffer* c, NVNdevice* device) {
    auto* cb = new CommandBuffer();
    cb->device = host_of<Device>(device);
    bind_guest(c, cb);
    return 1;
}

void nvnCommandBufferFinalize(NVNcommandBuffer* c) {
    CommandBuffer* cb = cb_of(c);
    if (!cb) return;
    {
        std::lock_guard<std::mutex> lk(cb->lock);
        for (uint64_t h : cb->handles) handle_release(h);
        cb->handles.clear();
    }
    unbind_guest(c);
}

void nvnCommandBufferSetDebugLabel(NVNcommandBuffer* c, const char* label) {
    if (CommandBuffer* cb = cb_of(c)) cb->label = label ? label : "";
}

void nvnCommandBufferSetMemoryCallback(NVNcommandBuffer* c, PFNNVNCOMMANDBUFFERMEMORYCALLBACKPROC cb_fn) {
    if (CommandBuffer* cb = cb_of(c)) cb->callback = reinterpret_cast<uint64_t>(cb_fn);
}
void nvnCommandBufferSetMemoryCallbackData(NVNcommandBuffer* c, void* data) {
    if (CommandBuffer* cb = cb_of(c)) cb->callback_data = reinterpret_cast<uint64_t>(data);
}
PFNNVNCOMMANDBUFFERMEMORYCALLBACKPROC nvnCommandBufferGetMemoryCallback(const NVNcommandBuffer* c) {
    const CommandBuffer* cb = cb_of(c);
    return cb ? reinterpret_cast<PFNNVNCOMMANDBUFFERMEMORYCALLBACKPROC>(cb->callback) : nullptr;
}
void* nvnCommandBufferGetMemoryCallbackData(const NVNcommandBuffer* c) {
    const CommandBuffer* cb = cb_of(c);
    return cb ? reinterpret_cast<void*>(cb->callback_data) : nullptr;
}

void nvnCommandBufferAddCommandMemory(NVNcommandBuffer* c, const NVNmemoryPool* pool, ptrdiff_t offset, size_t size) {
    CommandBuffer* cb = cb_of(c);
    if (!cb) return;
    const MemoryPool* mp = host_of<MemoryPool>(pool);
    std::lock_guard<std::mutex> lk(cb->lock);
    const uint64_t addr = mp ? mp->gpu_base + static_cast<uint64_t>(offset) : static_cast<uint64_t>(offset);
    cb->command_blocks.push_back({addr, static_cast<uint64_t>(offset), size});
    if (cb->command_blocks.size() > 8) cb->command_blocks.erase(cb->command_blocks.begin());
    cb->command_total = size;            // the block the next recording uses
    cb->command_used = 0;                // memory handed over again: its usage is given back
    cb->base_used = 0;
    cb->last_block_addr = addr;
}

void nvnCommandBufferAddControlMemory(NVNcommandBuffer* c, void* memory, size_t size) {
    CommandBuffer* cb = cb_of(c);
    if (!cb) return;
    std::lock_guard<std::mutex> lk(cb->lock);
    (void)memory;
    cb->control_total = size;
    cb->control_used = 0;
    cb->control_base = 0;
}

size_t nvnCommandBufferGetCommandMemorySize(const NVNcommandBuffer* c) { const CommandBuffer* cb = cb_of(c); return cb ? cb->command_total : 0; }
size_t nvnCommandBufferGetCommandMemoryUsed(const NVNcommandBuffer* c) { const CommandBuffer* cb = cb_of(c); return cb ? cb->command_used : 0; }
size_t nvnCommandBufferGetCommandMemoryFree(const NVNcommandBuffer* c) { const CommandBuffer* cb = cb_of(c); return cb ? cb->command_total - std::min(cb->command_used, cb->command_total) : 0; }
size_t nvnCommandBufferGetControlMemorySize(const NVNcommandBuffer* c) { const CommandBuffer* cb = cb_of(c); return cb ? cb->control_total : 0; }
size_t nvnCommandBufferGetControlMemoryUsed(const NVNcommandBuffer* c) { const CommandBuffer* cb = cb_of(c); return cb ? cb->control_used : 0; }
size_t nvnCommandBufferGetControlMemoryFree(const NVNcommandBuffer* c) { const CommandBuffer* cb = cb_of(c); return cb ? cb->control_total - std::min(cb->control_used, cb->control_total) : 0; }

NVNboolean nvnCommandBufferIsRecording(const NVNcommandBuffer* c) {
    const CommandBuffer* cb = cb_of(c);
    return cb && cb->recording ? 1 : 0;
}

void nvnCommandBufferBeginRecording(NVNcommandBuffer* c) {
    CommandBuffer* cb = cb_of(c);
    if (!cb) return;
    std::lock_guard<std::mutex> lk(cb->lock);
    cb->rec = Ref<CmdList>(new CmdList());
    cb->recording = true;
    cb->tpool = nullptr;
    cb->base_used = cb->command_used;
    cb->control_base = cb->control_used;
}

NVNcommandHandle nvnCommandBufferEndRecording(NVNcommandBuffer* c) {
    CommandBuffer* cb = cb_of(c);
    if (!cb) return 0;
    Ref<CmdList> list;
    {
        std::lock_guard<std::mutex> lk(cb->lock);
        if (!cb->recording || !cb->rec) return 0;
        list = std::move(cb->rec);
        cb->recording = false;
        cb->tpool = nullptr;
        const uint64_t cmd_cap = cb->command_total > kReserve ? cb->command_total - kReserve : 0;
        const uint64_t ctl_cap = cb->control_total > kReserve ? cb->control_total - kReserve : 0;
        cb->command_used = std::min<uint64_t>(cb->base_used + list->record_count * kCmdBytesPerRecord, cmd_cap);
        cb->control_used = std::min<uint64_t>(cb->control_base + list->record_count * kCtlBytesPerRecord, ctl_cap);
        if (cb->last_block_addr && cb->command_used > cb->base_used) {
            list->mem_start = cb->last_block_addr + cb->base_used;
            list->mem_end = cb->last_block_addr + cb->command_used;
        }
    }
    const uint64_t h = handle_register(list);
    std::lock_guard<std::mutex> lk(cb->lock);
    cb->handles.push_back(h);
    if (cb->handles.size() > 4096) cb->handles.erase(cb->handles.begin(), cb->handles.begin() + 2048);   // old ones are dropped by region anyway
    return h;
}

void nvnCommandBufferCallCommands(NVNcommandBuffer* c, int count, const NVNcommandHandle* handles) {
    CommandBuffer* cb = cb_of(c);
    if (!cb || !cb->recording || !cb->rec || count <= 0 || !handles) return;
    P_CallCommands p{static_cast<uint32_t>(count), 0};
    cb->rec->emit(Op::CallCommands, p, handles, static_cast<uint32_t>(count) * 8u);
}

// CopyCommands: the other lists' records are copied into this one now (the console copies the command words).
void nvnCommandBufferCopyCommands(NVNcommandBuffer* c, int count, const NVNcommandHandle* handles) {
    CommandBuffer* cb = cb_of(c);
    if (!cb || !cb->recording || !cb->rec || count <= 0 || !handles) return;
    for (int i = 0; i < count; ++i) {
        Ref<CmdList> src = handle_lookup(handles[i]);
        if (!src) continue;
        cb->rec->bytes.insert(cb->rec->bytes.end(), src->bytes.begin(), src->bytes.end());
        cb->rec->record_count += src->record_count;
        cb->rec->draw_count += src->draw_count;
        for (auto& h : src->held) cb->rec->held.push_back(h);
    }
}

void nvnCommandBufferSetCopyRowStride(NVNcommandBuffer* c, ptrdiff_t stride) { if (CommandBuffer* cb = cb_of(c)) cb->copy_row_stride = stride; }
void nvnCommandBufferSetCopyImageStride(NVNcommandBuffer* c, ptrdiff_t stride) { if (CommandBuffer* cb = cb_of(c)) cb->copy_image_stride = stride; }
ptrdiff_t nvnCommandBufferGetCopyRowStride(const NVNcommandBuffer* c) { const CommandBuffer* cb = cb_of(c); return cb ? static_cast<ptrdiff_t>(cb->copy_row_stride) : 0; }
ptrdiff_t nvnCommandBufferGetCopyImageStride(const NVNcommandBuffer* c) { const CommandBuffer* cb = cb_of(c); return cb ? static_cast<ptrdiff_t>(cb->copy_image_stride) : 0; }

}  // namespace

void register_api_cmdbuf() {
    NVN_REG(nvnCommandBufferInitialize);
    NVN_REG(nvnCommandBufferFinalize);
    NVN_REG(nvnCommandBufferSetDebugLabel);
    NVN_REG(nvnCommandBufferSetMemoryCallback);
    NVN_REG(nvnCommandBufferSetMemoryCallbackData);
    NVN_REG(nvnCommandBufferGetMemoryCallback);
    NVN_REG(nvnCommandBufferGetMemoryCallbackData);
    NVN_REG(nvnCommandBufferAddCommandMemory);
    NVN_REG(nvnCommandBufferAddControlMemory);
    NVN_REG(nvnCommandBufferGetCommandMemorySize);
    NVN_REG(nvnCommandBufferGetCommandMemoryUsed);
    NVN_REG(nvnCommandBufferGetCommandMemoryFree);
    NVN_REG(nvnCommandBufferGetControlMemorySize);
    NVN_REG(nvnCommandBufferGetControlMemoryUsed);
    NVN_REG(nvnCommandBufferGetControlMemoryFree);
    NVN_REG(nvnCommandBufferIsRecording);
    NVN_REG(nvnCommandBufferBeginRecording);
    NVN_REG(nvnCommandBufferEndRecording);
    NVN_REG(nvnCommandBufferCallCommands);
    NVN_REG(nvnCommandBufferCopyCommands);
    NVN_REG(nvnCommandBufferSetCopyRowStride);
    NVN_REG(nvnCommandBufferSetCopyImageStride);
    NVN_REG(nvnCommandBufferGetCopyRowStride);
    NVN_REG(nvnCommandBufferGetCopyImageStride);
}

}  // namespace nxrt::nvn
