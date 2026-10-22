// SPDX-License-Identifier: GPL-3.0-or-later
// Dear ImGui overlay (declarations in include/nxrt/overlay.h). ImGui and its Vulkan backend are pinned in cmake/gfx_deps.cmake.
#include "nxrt/overlay.h"

#include <atomic>
#include <mutex>

#include <SDL3/SDL_keyboard.h>
#include <SDL3/SDL_scancode.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <vector>

#include <imgui.h>
#include <imgui_impl_vulkan.h>

#include "nxrt/config.h"
#include "nxrt/log.h"
#include "nxrt/nvn.h"
#include "nxrt/platform.h"
#include "extras/overlay_composite.h"

namespace nxrt::gfx {
namespace {
constexpr const char* kTag = "overlay";

ImGuiKey map_key(int sc) {
    if (sc >= SDL_SCANCODE_A && sc <= SDL_SCANCODE_Z) return static_cast<ImGuiKey>(ImGuiKey_A + (sc - SDL_SCANCODE_A));
    if (sc >= SDL_SCANCODE_1 && sc <= SDL_SCANCODE_9) return static_cast<ImGuiKey>(ImGuiKey_1 + (sc - SDL_SCANCODE_1));
    if (sc == SDL_SCANCODE_0) return ImGuiKey_0;
    if (sc >= SDL_SCANCODE_F1 && sc <= SDL_SCANCODE_F12) return static_cast<ImGuiKey>(ImGuiKey_F1 + (sc - SDL_SCANCODE_F1));
    switch (sc) {
        case SDL_SCANCODE_RETURN: return ImGuiKey_Enter;
        case SDL_SCANCODE_ESCAPE: return ImGuiKey_Escape;
        case SDL_SCANCODE_BACKSPACE: return ImGuiKey_Backspace;
        case SDL_SCANCODE_TAB: return ImGuiKey_Tab;
        case SDL_SCANCODE_SPACE: return ImGuiKey_Space;
        case SDL_SCANCODE_INSERT: return ImGuiKey_Insert;
        case SDL_SCANCODE_DELETE: return ImGuiKey_Delete;
        case SDL_SCANCODE_HOME: return ImGuiKey_Home;
        case SDL_SCANCODE_END: return ImGuiKey_End;
        case SDL_SCANCODE_PAGEUP: return ImGuiKey_PageUp;
        case SDL_SCANCODE_PAGEDOWN: return ImGuiKey_PageDown;
        case SDL_SCANCODE_RIGHT: return ImGuiKey_RightArrow;
        case SDL_SCANCODE_LEFT: return ImGuiKey_LeftArrow;
        case SDL_SCANCODE_DOWN: return ImGuiKey_DownArrow;
        case SDL_SCANCODE_UP: return ImGuiKey_UpArrow;
        case SDL_SCANCODE_LCTRL: return ImGuiKey_LeftCtrl;
        case SDL_SCANCODE_RCTRL: return ImGuiKey_RightCtrl;
        case SDL_SCANCODE_LSHIFT: return ImGuiKey_LeftShift;
        case SDL_SCANCODE_RSHIFT: return ImGuiKey_RightShift;
        case SDL_SCANCODE_LALT: return ImGuiKey_LeftAlt;
        case SDL_SCANCODE_RALT: return ImGuiKey_RightAlt;
        default: return ImGuiKey_None;
    }
}

const double kFpsChoices[] = {0, 30, 60, 90, 120, 144, 165, 240, 360};
}  // namespace

struct Overlay::Impl {
    Context ctx;
    ImGuiContext* imgui = nullptr;
    bool backend_ready = false;
    std::atomic<bool> visible{false};
    std::mutex mu;                    // guards the ImGui context: events arrive on the game's main thread, drawing happens on the presenting thread
    int toggle_sc = SDL_SCANCODE_F1;
    bool restore_relative = false;
    std::chrono::steady_clock::time_point last_frame = std::chrono::steady_clock::now();
    uint64_t drawn = 0;

    struct Panel { std::string title; std::function<void()> fn; };
    std::vector<Panel> panels;

    // menu state applied by apply_pending()
    bool want_mode = false;
    PresentMode new_mode = PresentMode::Fifo;
    bool want_fullscreen = false;
    platform::FullscreenMode new_fullscreen = platform::FullscreenMode::Windowed;

    float frame_ms[120] = {};
    int frame_ms_pos = 0;
    int fps_choice = 0;
    VkFormat color_format = VK_FORMAT_UNDEFINED;
    bool hdr = false;                 // HDR swapchain: the overlay is drawn into an RGBA8 target and composited (draw)
    extras::OverlayComposite composite;

    void build_ui();
};

void Overlay::Impl::build_ui() {
    Renderer& r = *ctx.renderer;
    ImGui::SetNextWindowPos(ImVec2(16, 16), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(420, 0), ImGuiCond_FirstUseEver);
    if (ImGui::Begin("nxrt graphics")) {
        const GpuInfo& g = ctx.device->gpu();
        ImGui::Text("GPU: %s", g.name.c_str());
        ImGui::Text("Vulkan 1.3, driver 0x%08x, validation %s%s", g.driver_version, ctx.device->validation_enabled() ? "on" : "off",
                    ctx.device->headless() ? ", headless" : "");
        const VkExtent2D e = r.extent();
        ImGui::Text("Target: %ux%u, %u images, %u frames in flight", e.width, e.height, r.image_count(), r.frames_in_flight());

        ImGui::SeparatorText("Presentation");
        const PresentMode modes[] = {PresentMode::Fifo, PresentMode::FifoRelaxed, PresentMode::Mailbox, PresentMode::Immediate};
        int cur = static_cast<int>(r.requested_present_mode());
        if (ImGui::BeginCombo("Present mode", present_mode_name(modes[cur]))) {
            for (int i = 0; i < 4; ++i) {
                const bool ok = r.present_mode_supported(modes[i]);
                ImGui::BeginDisabled(!ok);
                if (ImGui::Selectable(present_mode_name(modes[i]), i == cur)) { want_mode = true; new_mode = modes[i]; }
                ImGui::EndDisabled();
            }
            ImGui::EndCombo();
        }
        if (r.present_mode() != r.requested_present_mode()) ImGui::TextDisabled("in use: %s (requested mode unavailable)", present_mode_name(r.present_mode()));

        if (ctx.limiter) {
            char label[32];
            const double t = ctx.limiter->target_fps();
            std::snprintf(label, sizeof label, t > 0 ? "%.0f fps" : "unlimited", t);
            if (ImGui::BeginCombo("Frame cap", label)) {
                for (double v : kFpsChoices) {
                    std::snprintf(label, sizeof label, v > 0 ? "%.0f fps" : "unlimited", v);
                    if (ImGui::Selectable(label, v == t)) ctx.limiter->set_target_fps(v);
                }
                ImGui::EndCombo();
            }
            const float hz = platform::has_window() ? platform::display_refresh_hz() : 0.f;
            if (hz > 0) {
                ImGui::TextDisabled("display %.2f Hz; VRR-friendly cap %.1f fps", hz, suggest_vrr_cap(hz));
                if (ImGui::SmallButton("Use VRR cap")) ctx.limiter->set_target_fps(suggest_vrr_cap(hz));
            }
        }

        if (ctx.has_window && platform::has_window()) {
            ImGui::SeparatorText("Window");
            const auto fm = platform::fullscreen_mode();
            if (ImGui::RadioButton("Windowed", fm == platform::FullscreenMode::Windowed)) { want_fullscreen = true; new_fullscreen = platform::FullscreenMode::Windowed; }
            ImGui::SameLine();
            if (ImGui::RadioButton("Borderless", fm == platform::FullscreenMode::Borderless)) { want_fullscreen = true; new_fullscreen = platform::FullscreenMode::Borderless; }
            ImGui::SameLine();
            if (ImGui::RadioButton("Exclusive", fm == platform::FullscreenMode::Exclusive)) { want_fullscreen = true; new_fullscreen = platform::FullscreenMode::Exclusive; }
            bool rel = platform::relative_mouse();
            if (ImGui::Checkbox("Raw mouse (relative mode)", &rel)) platform::set_relative_mouse(rel);
            ImGui::SameLine();
            ImGui::TextDisabled(platform::relative_mouse_is_raw() ? "(unaccelerated)" : "(system scaling!)");
        }

        ImGui::SeparatorText("Timing");
        const RendererStats rs = r.stats();
        const float dt_ms = ImGui::GetIO().DeltaTime * 1000.f;
        frame_ms[frame_ms_pos] = dt_ms;
        frame_ms_pos = (frame_ms_pos + 1) % 120;
        ImGui::Text("CPU frame %.2f ms (%.0f fps)   GPU %.2f ms (avg %.2f)", dt_ms, dt_ms > 0 ? 1000.f / dt_ms : 0.f, rs.gpu_ms_last, rs.gpu_ms_avg);
        ImGui::PlotLines("##ft", frame_ms, 120, frame_ms_pos, nullptr, 0.f, 33.f, ImVec2(0, 48));
        ImGui::Text("frames %llu, swapchains %llu, out-of-date %llu, screenshots %llu", static_cast<unsigned long long>(rs.frames),
                    static_cast<unsigned long long>(rs.swapchain_creates), static_cast<unsigned long long>(rs.out_of_date),
                    static_cast<unsigned long long>(rs.screenshots));
        const ValidationStats vs = ctx.device->validation_stats();
        ImGui::Text("validation: %llu errors, %llu warnings", static_cast<unsigned long long>(vs.errors), static_cast<unsigned long long>(vs.warnings));

        ImGui::SeparatorText("NVN front end");
        ImGui::Text("%zu entry points, %zu implemented; stub calls %llu (%zu distinct)", nvn::entry_count(), nvn::implemented_count(),
                    static_cast<unsigned long long>(nvn::stub_calls_total()), nvn::stubs_called_distinct());
        if (ImGui::TreeNode("Most called stubs")) {
            auto calls = nvn::called_entries();
            int shown = 0;
            for (auto& c : calls) {
                if (c.implemented) continue;
                ImGui::Text("%8llu  %s", static_cast<unsigned long long>(c.calls), c.entry->name);
                if (++shown >= 12) break;
            }
            if (!shown) ImGui::TextDisabled("none called");
            ImGui::TreePop();
        }
    }
    ImGui::End();
    for (auto& p : panels) {
        if (ImGui::Begin(p.title.c_str())) p.fn();
        ImGui::End();
    }
}

std::unique_ptr<Overlay> Overlay::create(const Context& ctx, std::string* error) {
    if (!ctx.device || !ctx.renderer) {
        if (error) *error = "overlay needs a device and a renderer";
        return nullptr;
    }
    std::unique_ptr<Overlay> o(new Overlay());
    o->impl_ = new Impl();
    Impl& s = *o->impl_;
    s.ctx = ctx;
    s.color_format = ctx.renderer->format();
    if (ctx.renderer->output_kind() != extras::OutputKind::Sdr8) {
        // HDR swapchain: ImGui draws into an RGBA8 target that the composite maps onto the swapchain (see draw)
        if (s.composite.init(*ctx.device)) {
            s.hdr = true;
            s.color_format = VK_FORMAT_R8G8B8A8_UNORM;
        } else {
            NXLOG_W(kTag, "HDR overlay composite failed to initialise: the overlay is drawn straight into the HDR swapchain (colours will look wrong)");
        }
    }
    const std::string key = config_get_str("gfx", "overlay_key", "F1");
    if (const SDL_Scancode sc = SDL_GetScancodeFromName(key.c_str()); sc != SDL_SCANCODE_UNKNOWN) s.toggle_sc = sc;
    else NXLOG_W(kTag, "unknown [gfx] overlay_key '%s': using F1", key.c_str());

    IMGUI_CHECKVERSION();
    s.imgui = ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;     // no imgui.ini next to the exe
    io.LogFilename = nullptr;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    ImGui::StyleColorsDark();
    const VkExtent2D e = ctx.renderer->extent();
    io.DisplaySize = ImVec2(static_cast<float>(e.width), static_cast<float>(e.height));

    VkPipelineRenderingCreateInfoKHR rci = {VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO_KHR};
    rci.colorAttachmentCount = 1;
    rci.pColorAttachmentFormats = &s.color_format;
    ImGui_ImplVulkan_InitInfo ii = {};
    ii.ApiVersion = VK_API_VERSION_1_3;
    ii.Instance = ctx.device->instance();
    ii.PhysicalDevice = ctx.device->physical();
    ii.Device = ctx.device->device();
    ii.QueueFamily = ctx.device->queue_family();
    ii.Queue = ctx.device->queue();
    ii.DescriptorPoolSize = 16;
    ii.MinImageCount = 2;
    ii.ImageCount = std::max(2u, ctx.renderer->image_count());
    ii.UseDynamicRendering = true;
    ii.PipelineInfoMain.PipelineRenderingCreateInfo = rci;
    ii.CheckVkResultFn = [](VkResult r) { if (r != VK_SUCCESS) NXLOG_E(kTag, "ImGui Vulkan call failed (%d)", static_cast<int>(r)); };
    if (!ImGui_ImplVulkan_Init(&ii)) {
        if (error) *error = "ImGui_ImplVulkan_Init failed";
        ImGui::DestroyContext(s.imgui);
        s.imgui = nullptr;
        return nullptr;
    }
    s.backend_ready = true;
    NXLOG_I(kTag, "overlay ready (toggle key %s)", SDL_GetScancodeName(static_cast<SDL_Scancode>(s.toggle_sc)));
    return o;
}

Overlay::~Overlay() {
    if (!impl_) return;
    if (impl_->ctx.device) impl_->ctx.device->wait_idle();
    impl_->composite.shutdown();
    if (impl_->backend_ready) ImGui_ImplVulkan_Shutdown();
    if (impl_->imgui) ImGui::DestroyContext(impl_->imgui);
    delete impl_;
}

void Overlay::set_visible(bool v) {
    Impl& s = *impl_;
    if (s.visible == v) return;
    s.visible = v;
    if (v) {
        s.restore_relative = platform::has_window() && platform::relative_mouse();
        if (s.restore_relative) platform::set_relative_mouse(false);
    } else {
        if (s.restore_relative && platform::has_window()) platform::set_relative_mouse(true);
        s.restore_relative = false;
        std::lock_guard<std::mutex> lk(s.mu);
        ImGuiContext* prev = ImGui::GetCurrentContext();
        ImGui::SetCurrentContext(s.imgui);
        ImGuiIO& io = ImGui::GetIO();   // drop pressed keys and buttons so nothing stays latched
        io.ClearInputKeys();
        io.ClearInputMouse();
        ImGui::SetCurrentContext(prev);
    }
    NXLOG_I(kTag, "overlay %s", v ? "shown" : "hidden");
}

bool Overlay::visible() const { return impl_->visible; }
void Overlay::toggle() { set_visible(!impl_->visible); }
int Overlay::toggle_scancode() const { return impl_->toggle_sc; }
void Overlay::set_toggle_scancode(int sc) { impl_->toggle_sc = sc; }
uint64_t Overlay::frames_drawn() const { return impl_->drawn; }

namespace {
bool imgui_wants(std::mutex& mu, ImGuiContext* ctx, bool mouse, bool keyboard) {
    std::lock_guard<std::mutex> lk(mu);
    ImGuiContext* prev = ImGui::GetCurrentContext();
    ImGui::SetCurrentContext(ctx);
    const ImGuiIO& io = ImGui::GetIO();
    const bool w = (mouse && io.WantCaptureMouse) || (keyboard && io.WantCaptureKeyboard);
    ImGui::SetCurrentContext(prev);
    return w;
}
}  // namespace

bool Overlay::wants_input() const { return impl_->visible && imgui_wants(impl_->mu, impl_->imgui, true, true); }
bool Overlay::wants_mouse() const { return impl_->visible && imgui_wants(impl_->mu, impl_->imgui, true, false); }
bool Overlay::wants_keyboard() const { return impl_->visible && imgui_wants(impl_->mu, impl_->imgui, false, true); }

void Overlay::add_panel(const std::string& title, std::function<void()> fn) { impl_->panels.push_back({title, std::move(fn)}); }

bool Overlay::handle_event(const input::Event& e) {
    Impl& s = *impl_;
    using input::EventType;
    if ((e.type == EventType::KeyDown || e.type == EventType::KeyUp) && e.u.key.scancode == s.toggle_sc) {
        if (e.type == EventType::KeyDown && !(e.flags & input::kFlagRepeat)) toggle();
        return true;   // the toggle key never reaches the game (press, repeat and release)
    }
    if (!s.visible) return false;
    std::lock_guard<std::mutex> lk(s.mu);
    ImGuiContext* prev = ImGui::GetCurrentContext();
    ImGui::SetCurrentContext(s.imgui);
    ImGuiIO& io = ImGui::GetIO();
    bool consumed = false;
    switch (e.type) {
        case EventType::KeyDown:
        case EventType::KeyUp: {
            const ImGuiKey k = map_key(e.u.key.scancode);
            if (k != ImGuiKey_None) io.AddKeyEvent(k, e.type == EventType::KeyDown);
            consumed = io.WantCaptureKeyboard;
            break;
        }
        case EventType::Text:
            io.AddInputCharactersUTF8(e.u.text.utf8);
            consumed = io.WantTextInput;
            break;
        case EventType::MouseMotion:
            io.AddMousePosEvent(e.u.motion.x, e.u.motion.y);
            consumed = io.WantCaptureMouse;
            break;
        case EventType::MouseButtonDown:
        case EventType::MouseButtonUp: {
            const int b = e.u.button.button;   // SDL: 1 left, 2 middle, 3 right
            const int ib = b == 1 ? 0 : b == 3 ? 1 : b == 2 ? 2 : b - 1;
            io.AddMousePosEvent(e.u.button.x, e.u.button.y);
            io.AddMouseButtonEvent(ib, e.type == EventType::MouseButtonDown);
            consumed = io.WantCaptureMouse;
            break;
        }
        case EventType::MouseWheel:
            io.AddMouseWheelEvent(e.u.wheel.dx, e.u.wheel.dy);
            consumed = io.WantCaptureMouse;
            break;
        case EventType::WindowFocusLost: io.AddFocusEvent(false); break;
        case EventType::WindowFocusGained: io.AddFocusEvent(true); break;
        default: break;
    }
    ImGui::SetCurrentContext(prev);
    return consumed;
}

void Overlay::apply_pending() {
    apply_present_pending();
    apply_window_pending();
}

void Overlay::apply_present_pending() {
    Impl& s = *impl_;
    if (s.want_mode) {
        s.want_mode = false;
        s.ctx.renderer->set_present_mode(s.new_mode);
    }
}

void Overlay::apply_window_pending() {
    Impl& s = *impl_;
    if (s.want_fullscreen) {
        s.want_fullscreen = false;
        platform::set_fullscreen(s.new_fullscreen);
    }
}

void Overlay::draw(const FrameInfo& f) {
    Impl& s = *impl_;
    if (!s.visible) {
        s.last_frame = std::chrono::steady_clock::now();
        return;
    }
    const bool hdr_frame = s.hdr && f.hdr_output;
    if (s.hdr && !f.hdr_output) return;      // the display left HDR mode: the RGBA8 overlay pipeline does not match the SDR swapchain, so no overlay until the next start
    if (hdr_frame && !s.composite.ensure_target(f.extent)) return;
    std::lock_guard<std::mutex> lk(s.mu);
    ImGuiContext* prev = ImGui::GetCurrentContext();
    ImGui::SetCurrentContext(s.imgui);
    ImGuiIO& io = ImGui::GetIO();
    const auto now = std::chrono::steady_clock::now();
    io.DeltaTime = std::clamp(std::chrono::duration<float>(now - s.last_frame).count(), 1.0f / 1000.f, 0.25f);
    s.last_frame = now;
    io.DisplaySize = ImVec2(static_cast<float>(f.extent.width), static_cast<float>(f.extent.height));

    ImGui_ImplVulkan_NewFrame();
    ImGui::NewFrame();
    s.build_ui();
    ImGui::Render();

    // one dynamic-rendering pass that draws the ImGui draw data into view
    auto render_into = [&](VkImageView view, VkAttachmentLoadOp load) {
        VkRenderingAttachmentInfo att = {VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
        att.imageView = view;
        att.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        att.loadOp = load;
        att.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        att.clearValue.color = {{0.0f, 0.0f, 0.0f, 0.0f}};
        VkRenderingInfo ri = {VK_STRUCTURE_TYPE_RENDERING_INFO};
        ri.renderArea = {{0, 0}, f.extent};
        ri.layerCount = 1;
        ri.colorAttachmentCount = 1;
        ri.pColorAttachments = &att;
        vkCmdBeginRendering(f.cmd, &ri);
        ImGui_ImplVulkan_RenderDrawData(ImGui::GetDrawData(), f.cmd);
        vkCmdEndRendering(f.cmd);
    };
    if (hdr_frame) {
        // HDR: the overlay goes into the cleared RGBA8 target, then the composite maps it onto the swapchain image (UI white = paper white)
        const extras::HdrState hs = extras::hdr_state();
        s.composite.begin_overlay(f.cmd);
        render_into(s.composite.target_view(), VK_ATTACHMENT_LOAD_OP_CLEAR);
        s.composite.composite(f.cmd, f.view, f.format, f.extent, s.ctx.renderer->output_kind(), hs.paper_white_nits, hs.peak_nits);
    } else {
        render_into(f.view, VK_ATTACHMENT_LOAD_OP_LOAD);
    }
    s.drawn++;
    ImGui::SetCurrentContext(prev);
}

void Overlay::install_on_renderer() {
    impl_->ctx.renderer->set_overlay_hook([this](const FrameInfo& f) { draw(f); });
}

}  // namespace nxrt::gfx
