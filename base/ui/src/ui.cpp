// SPDX-License-Identifier: GPL-3.0-or-later
#include "nxui/ui.h"

#include <SDL3/SDL.h>
#include <backends/imgui_impl_sdl3.h>
#include <backends/imgui_impl_sdlrenderer3.h>
#include <misc/cpp/imgui_stdlib.h>

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <filesystem>
#include <memory>
#include <mutex>

// The DejaVu Sans font of the repository (runtime/assets/fonts), turned into a byte array by nxui.cmake.
extern const unsigned char nxui_font_dejavu[];
extern const unsigned long nxui_font_dejavu_size;

namespace nxui {
namespace {

constexpr float kFontPx = 14.f;

// A system font with CJK glyphs, merged behind DejaVu Sans when one is installed (the game's own language names are shown in their script).
// Nothing is bundled for it: a missing font shows those names with a replacement glyph, and the Latin part still reads.
void add_system_fonts(ImGuiIO& io) {
    namespace fs = std::filesystem;
    static const char* const kPaths[] = {
        // Windows
        "C:/Windows/Fonts/YuGothR.ttc", "C:/Windows/Fonts/meiryo.ttc", "C:/Windows/Fonts/msyh.ttc", "C:/Windows/Fonts/malgun.ttf",
        // Linux
        "/usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc", "/usr/share/fonts/noto-cjk/NotoSansCJK-Regular.ttc",
        "/usr/share/fonts/truetype/noto/NotoSansCJK-Regular.ttc", "/usr/share/fonts/google-noto-cjk/NotoSansCJK-Regular.ttc",
        "/usr/share/fonts/OTF/NotoSansCJK-Regular.ttc", "/usr/share/fonts/truetype/droid/DroidSansFallbackFull.ttf",
        "/usr/share/fonts/truetype/wqy/wqy-microhei.ttc",
    };
    std::string win;
    if (const char* r = std::getenv("SystemRoot")) win = std::string(r) + "/Fonts/";
    bool linux_done = false;
    for (const char* p : kPaths) {
        std::string path = p;
        if (!win.empty() && path.rfind("C:/Windows/Fonts/", 0) == 0) path = win + path.substr(17);
        std::error_code ec;
        if (!fs::exists(path, ec)) continue;
        if (path.rfind("/usr", 0) == 0) {
            if (linux_done) continue;
            linux_done = true;
        }
        ImFontConfig cfg;
        cfg.MergeMode = true;
        io.Fonts->AddFontFromFileTTF(path.c_str(), 0.f, &cfg);
    }
}

}  // namespace

// ---- state that does not belong in the header --------------------------------------------------------------------------------------

struct PickRequest;

struct App::Impl {
    Config cfg;
    ImGuiStyle base_style;
    std::atomic<bool> quit{false};
    std::atomic<int> exit_code{0};
    Uint32 wake_event = 0;
    int frames_left = 3;
    bool imgui_ready = false;
    bool renderer_ready = false;

    // message boxes
    std::deque<MessageSpec> messages;
    bool message_shown = false;

    // pickers
    struct Result {
        PickRequest* req;
        std::vector<std::string> paths;
        bool error;
        std::string error_text;
    };
    std::mutex mu;
    std::deque<Result> results;

    // the typed-path fallback of a picker
    struct PathEntry {
        bool open = false;
        bool opened = false;
        std::string title, note, text;
        bool folder = false, many = false;
        std::function<void(std::vector<std::string>)> done;
    } entry;

    std::string shot_path;
    int shot_frames = 12;
    int frame_no = 0;
};

struct PickRequest {
    App::Impl* d;
    bool folder;
    bool many;
    std::string title;
    std::function<void(std::vector<std::string>)> done;
    std::vector<std::string> filter_names, filter_patterns;
    std::vector<SDL_DialogFileFilter> filters;
};

namespace {

void SDLCALL pick_callback(void* userdata, const char* const* list, int) {
    std::unique_ptr<PickRequest> req(static_cast<PickRequest*>(userdata));
    App::Impl::Result r{req.get(), {}, false, {}};
    if (!list) {
        r.error = true;
        r.error_text = SDL_GetError();
    } else {
        for (; *list; ++list) r.paths.emplace_back(*list);
    }
    App::Impl* d = req->d;
    {
        std::lock_guard<std::mutex> lk(d->mu);
        // the request lives on in the result until the UI thread has called `done`
        r.req = req.release();
        d->results.push_back(std::move(r));
    }
    SDL_Event ev{};
    ev.type = d->wake_event;
    SDL_PushEvent(&ev);
}

void apply_theme(ImGuiStyle& base) {
    ImGui::StyleColorsLight(&base);
    ImVec4* c = base.Colors;
    c[ImGuiCol_WindowBg] = col::rgb(0xFFFFFF);
    c[ImGuiCol_ChildBg] = col::rgb(0xFFFFFF);
    c[ImGuiCol_PopupBg] = col::rgb(0xFFFFFF);
    c[ImGuiCol_Text] = col::rgb(0x1B1B1B);
    c[ImGuiCol_TextDisabled] = col::rgb(0x8A8A8A);
    c[ImGuiCol_Border] = col::rgb(0xC9CDD2);
    c[ImGuiCol_FrameBg] = col::rgb(0xF3F4F6);
    c[ImGuiCol_FrameBgHovered] = col::rgb(0xE8EBEF);
    c[ImGuiCol_FrameBgActive] = col::rgb(0xDCE1E7);
    c[ImGuiCol_Button] = col::rgb(0xE9ECEF);
    c[ImGuiCol_ButtonHovered] = col::rgb(0xDDE2E8);
    c[ImGuiCol_ButtonActive] = col::rgb(0xCDD4DC);
    c[ImGuiCol_Header] = col::rgb(0xE4E9F0);
    c[ImGuiCol_HeaderHovered] = col::rgb(0xD6E2F2);
    c[ImGuiCol_HeaderActive] = col::rgb(0xC5D8F0);
    c[ImGuiCol_Tab] = col::rgb(0xF0F2F5);
    c[ImGuiCol_TabHovered] = col::rgb(0xDCE8F6);
    c[ImGuiCol_TabSelected] = col::rgb(0xFFFFFF);
    c[ImGuiCol_TabSelectedOverline] = col::accent;
    c[ImGuiCol_TabDimmed] = col::rgb(0xF0F2F5);
    c[ImGuiCol_TabDimmedSelected] = col::rgb(0xFFFFFF);
    c[ImGuiCol_CheckMark] = col::accent;
    c[ImGuiCol_SliderGrab] = col::accent;
    c[ImGuiCol_SliderGrabActive] = col::rgb(0x084783);
    c[ImGuiCol_PlotHistogram] = col::accent;
    c[ImGuiCol_Separator] = col::rgb(0xD5D9DE);
    c[ImGuiCol_ModalWindowDimBg] = ImVec4(0.f, 0.f, 0.f, 0.30f);
    c[ImGuiCol_ScrollbarBg] = col::rgb(0xFFFFFF, 0.f);
    c[ImGuiCol_ScrollbarGrab] = col::rgb(0xC3C8CE);
    c[ImGuiCol_TextSelectedBg] = col::rgb(0x0B5CAD, 0.30f);
    base.FontSizeBase = kFontPx;
    base.WindowPadding = ImVec2(0, 0);
    base.FramePadding = ImVec2(8, 4);
    base.ItemSpacing = ImVec2(8, 6);
    base.ItemInnerSpacing = ImVec2(6, 4);
    base.FrameRounding = 3;
    base.PopupRounding = 4;
    base.WindowRounding = 0;
    base.ScrollbarRounding = 4;
    base.GrabRounding = 3;
    base.TabRounding = 3;
    base.FrameBorderSize = 1;
    base.WindowBorderSize = 0;
    base.ScrollbarSize = 12;
    base.GrabMinSize = 12;
    base.TabBarOverlineSize = 2;
}

void set_scale(App::Impl& d, float scale) {
    ImGuiStyle& s = ImGui::GetStyle();
    s = d.base_style;
    s.ScaleAllSizes(scale);
    s.FontScaleDpi = scale;
}

std::string file_url(const std::string& path) {
    std::string p = path;
    std::replace(p.begin(), p.end(), '\\', '/');
    std::string out = "file://";
    if (!p.empty() && p[0] != '/') out += '/';   // C:/x -> file:///C:/x
    for (unsigned char ch : p) {
        if (std::isalnum(ch) || std::strchr("/._~:-", ch)) {
            out += static_cast<char>(ch);
        } else {
            char b[4];
            std::snprintf(b, sizeof b, "%%%02X", ch);
            out += b;
        }
    }
    return out;
}

}  // namespace

// ---- App ----------------------------------------------------------------------------------------------------------------------------

App::App() : d_(new Impl) {}

App::~App() {
    if (d_->imgui_ready) {
        if (d_->renderer_ready) ImGui_ImplSDLRenderer3_Shutdown();
        ImGui_ImplSDL3_Shutdown();
        ImGui::DestroyContext();
    }
    if (renderer_) SDL_DestroyRenderer(renderer_);
    if (window_) SDL_DestroyWindow(window_);
    SDL_Quit();
    delete d_;
}

bool App::init(const Config& cfg) {
    d_->cfg = cfg;
    if (const char* s = std::getenv("NXUI_SHOT"); s && *s) {
        d_->shot_path = s;
        if (const char* n = std::getenv("NXUI_SHOT_FRAMES"); n && std::atoi(n) > 0) d_->shot_frames = std::atoi(n);
    }
    SDL_SetAppMetadata(cfg.title.c_str(), nullptr, cfg.app_id.empty() ? nullptr : cfg.app_id.c_str());
    SDL_SetHint(SDL_HINT_VIDEO_ALLOW_SCREENSAVER, "1");
    if (!SDL_Init(SDL_INIT_VIDEO)) {
        error_ = std::string("SDL could not start: ") + SDL_GetError();
        return false;
    }
    d_->wake_event = SDL_RegisterEvents(1);

    float scale = SDL_GetDisplayContentScale(SDL_GetPrimaryDisplay());
    if (!(scale >= 0.5f && scale <= 8.f)) scale = 1.f;
    scale_ = scale;
    SDL_WindowFlags flags = SDL_WINDOW_HIGH_PIXEL_DENSITY | (cfg.resizable ? SDL_WINDOW_RESIZABLE : 0);
    window_ = SDL_CreateWindow(cfg.title.c_str(), static_cast<int>(cfg.width * scale), static_cast<int>(cfg.height * scale), flags);
    if (!window_) {
        error_ = std::string("The window could not be created: ") + SDL_GetError();
        return false;
    }
    // the scale of the monitor the window really opened on
    if (float ws = SDL_GetWindowDisplayScale(window_); ws >= 0.5f && ws <= 8.f && ws != scale_) {
        scale_ = ws;
        SDL_SetWindowSize(window_, static_cast<int>(cfg.width * ws), static_cast<int>(cfg.height * ws));
    }
    SDL_SetWindowPosition(window_, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED);
    renderer_ = SDL_CreateRenderer(window_, nullptr);
    if (!renderer_) {
        error_ = std::string("No renderer could be created: ") + SDL_GetError();
        return false;
    }
    SDL_SetRenderVSync(renderer_, 1);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.LogFilename = nullptr;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    ImFontConfig fc;
    fc.FontDataOwnedByAtlas = false;
    io.Fonts->AddFontFromMemoryTTF(const_cast<unsigned char*>(nxui_font_dejavu), static_cast<int>(nxui_font_dejavu_size), 0.f, &fc);
    add_system_fonts(io);
    apply_theme(d_->base_style);
    set_scale(*d_, scale_);
    d_->imgui_ready = true;
    ImGui_ImplSDL3_InitForSDLRenderer(window_, renderer_);
    ImGui_ImplSDLRenderer3_Init(renderer_);
    d_->renderer_ready = true;
    return true;
}

void App::set_title(const std::string& title) {
    if (window_) SDL_SetWindowTitle(window_, title.c_str());
}

void App::wake() {
    SDL_Event ev{};
    ev.type = d_->wake_event;
    SDL_PushEvent(&ev);
}

void App::quit(int code) {
    d_->exit_code = code;
    d_->quit = true;
    wake();
}

void App::message(MessageSpec spec) {
    if (spec.buttons.empty()) spec.buttons.push_back("OK");
    d_->messages.push_back(std::move(spec));
    d_->frames_left = 3;
}

bool App::modal_open() const { return !d_->messages.empty() || d_->entry.open; }

void App::pick_files(const std::string& title, const std::vector<std::string>& filters, bool many, const std::string& start,
                     std::function<void(std::vector<std::string>)> done) {
    auto* req = new PickRequest{d_, false, many, title, std::move(done), {}, {}, {}};
    for (const std::string& f : filters) {
        const size_t bar = f.find('|');
        req->filter_names.push_back(bar == std::string::npos ? f : f.substr(0, bar));
        req->filter_patterns.push_back(bar == std::string::npos ? "*" : f.substr(bar + 1));
    }
    for (size_t i = 0; i < req->filter_names.size(); ++i) req->filters.push_back({req->filter_names[i].c_str(), req->filter_patterns[i].c_str()});
    SDL_PropertiesID p = SDL_CreateProperties();
    SDL_SetPointerProperty(p, SDL_PROP_FILE_DIALOG_FILTERS_POINTER, req->filters.data());
    SDL_SetNumberProperty(p, SDL_PROP_FILE_DIALOG_NFILTERS_NUMBER, static_cast<Sint64>(req->filters.size()));
    SDL_SetPointerProperty(p, SDL_PROP_FILE_DIALOG_WINDOW_POINTER, window_);
    if (!start.empty()) SDL_SetStringProperty(p, SDL_PROP_FILE_DIALOG_LOCATION_STRING, start.c_str());
    SDL_SetBooleanProperty(p, SDL_PROP_FILE_DIALOG_MANY_BOOLEAN, many);
    SDL_SetStringProperty(p, SDL_PROP_FILE_DIALOG_TITLE_STRING, req->title.c_str());
    SDL_ShowFileDialogWithProperties(SDL_FILEDIALOG_OPENFILE, pick_callback, req, p);
    SDL_DestroyProperties(p);
}

void App::pick_folder(const std::string& title, const std::string& start, std::function<void(std::string)> done) {
    auto* req = new PickRequest{d_, true, false, title, nullptr, {}, {}, {}};
    req->done = [done = std::move(done)](std::vector<std::string> v) { done(v.empty() ? std::string() : v[0]); };
    SDL_PropertiesID p = SDL_CreateProperties();
    SDL_SetPointerProperty(p, SDL_PROP_FILE_DIALOG_WINDOW_POINTER, window_);
    if (!start.empty()) SDL_SetStringProperty(p, SDL_PROP_FILE_DIALOG_LOCATION_STRING, start.c_str());
    SDL_SetStringProperty(p, SDL_PROP_FILE_DIALOG_TITLE_STRING, req->title.c_str());
    SDL_ShowFileDialogWithProperties(SDL_FILEDIALOG_OPENFOLDER, pick_callback, req, p);
    SDL_DestroyProperties(p);
}

bool App::open_path(const std::string& path) { return SDL_OpenURL(file_url(path).c_str()); }

// ---- the loop --------------------------------------------------------------------------------------------------------------------------

namespace {

void draw_message(App& app, App::Impl& d, MessageSpec& m, bool* finished, int* answer) {
    const std::string id = m.title + "###nxui_message";
    if (!d.message_shown) {
        ImGui::OpenPopup(id.c_str());
        d.message_shown = true;
    }
    const ImVec2 center = ImGui::GetMainViewport()->GetCenter();
    ImGui::SetNextWindowPos(center, ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSizeConstraints(ImVec2(app.S(360), 0), ImVec2(app.S(560), app.S(600)));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(app.S(18), app.S(14)));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 1.f);
    if (ImGui::BeginPopupModal(id.c_str(), nullptr, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings)) {
        ImGui::PushTextWrapPos(app.S(500));
        if (m.color.w > 0) ImGui::PushStyleColor(ImGuiCol_Text, m.color);
        ImGui::TextUnformatted(m.text.c_str());
        if (m.color.w > 0) ImGui::PopStyleColor();
        ImGui::PopTextWrapPos();
        ImGui::Dummy(ImVec2(0, app.S(8)));
        const float bw = app.S(96);
        const float total = m.buttons.size() * bw + (m.buttons.size() - 1) * ImGui::GetStyle().ItemSpacing.x;
        const float avail = ImGui::GetContentRegionAvail().x;
        if (avail > total) ImGui::SetCursorPosX(ImGui::GetCursorPosX() + avail - total);
        for (size_t i = 0; i < m.buttons.size(); ++i) {
            if (i) ImGui::SameLine();
            const bool first = i == 0;
            const bool pressed = first ? primary_button(m.buttons[i].c_str(), ImVec2(bw, 0)) : button(m.buttons[i].c_str(), ImVec2(bw, 0));
            if (pressed) {
                *finished = true;
                *answer = static_cast<int>(i);
            }
        }
        if (ImGui::IsKeyPressed(ImGuiKey_Escape)) {
            *finished = true;
            *answer = static_cast<int>(m.buttons.size()) - 1;
        }
        if (*finished) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
    ImGui::PopStyleVar(2);
}

void draw_path_entry(App& app, App::Impl& d) {
    auto& e = d.entry;
    const std::string id = e.title + "###nxui_path";
    if (!e.opened) {
        ImGui::OpenPopup(id.c_str());
        e.opened = true;
    }
    ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(app.S(520), 0));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(app.S(18), app.S(14)));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 1.f);
    if (ImGui::BeginPopupModal(id.c_str(), nullptr, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings)) {
        ImGui::PushTextWrapPos(app.S(480));
        ImGui::PushStyleColor(ImGuiCol_Text, col::gray);
        ImGui::TextUnformatted(e.note.c_str());
        ImGui::PopStyleColor();
        ImGui::PopTextWrapPos();
        ImGui::SetNextItemWidth(app.S(484));
        if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
        ImGui::InputText("##path", &e.text);
        bool ok = false, cancel = ImGui::IsKeyPressed(ImGuiKey_Escape);
        ImGui::Dummy(ImVec2(0, app.S(6)));
        if (primary_button("OK", ImVec2(app.S(96), 0))) ok = true;
        ImGui::SameLine();
        if (button("Cancel", ImVec2(app.S(96), 0))) cancel = true;
        if (ok || cancel) {
            ImGui::CloseCurrentPopup();
            auto done = std::move(e.done);
            std::vector<std::string> paths;
            if (ok && !e.text.empty()) paths.push_back(e.text);
            e = {};
            if (done) done(std::move(paths));
        }
        ImGui::EndPopup();
    }
    ImGui::PopStyleVar(2);
}

}  // namespace

int App::run(const Hooks& h) {
    Impl& d = *d_;
    bool done_loop = false;
    while (!done_loop && !d.quit) {
        // events: wait for the first, then take what else is queued
        const int timeout = (busy_ || d.frames_left > 0 || !d.shot_path.empty()) ? (d.frames_left > 0 ? 0 : 50) : 250;
        SDL_Event ev;
        bool got = SDL_WaitEventTimeout(&ev, timeout);
        while (got) {
            d.frames_left = 3;
            if (ev.type == SDL_EVENT_QUIT || ev.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED) {
                if (!h.can_close || h.can_close()) done_loop = true;
            } else if (ev.type == SDL_EVENT_WINDOW_DISPLAY_SCALE_CHANGED || ev.type == SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED) {
                const float ws = SDL_GetWindowDisplayScale(window_);
                if (ev.type == SDL_EVENT_WINDOW_DISPLAY_SCALE_CHANGED && ws >= 0.5f && ws <= 8.f && ws != scale_) {
                    scale_ = ws;
                    set_scale(d, scale_);
                    if (!d.cfg.resizable) SDL_SetWindowSize(window_, static_cast<int>(d.cfg.width * ws), static_cast<int>(d.cfg.height * ws));
                    if (h.on_scale_changed) h.on_scale_changed();
                }
            }
            if (!(h.on_event && h.on_event(ev))) ImGui_ImplSDL3_ProcessEvent(&ev);
            got = SDL_PollEvent(&ev);
        }
        if (done_loop || d.quit) break;
        if (d.frames_left > 0) --d.frames_left;

        // finished pickers
        for (;;) {
            Impl::Result r;
            {
                std::lock_guard<std::mutex> lk(d.mu);
                if (d.results.empty()) break;
                r = std::move(d.results.front());
                d.results.pop_front();
            }
            std::unique_ptr<PickRequest> req(r.req);
            if (r.error) {
                // the system has no dialog to offer: ask for the path in a box of our own
                d.entry = {};
                d.entry.open = true;
                d.entry.title = req->title;
                d.entry.note = std::string("The system's file dialog is not available here (") + r.error_text +
                               "). Type or paste the " + (req->folder ? "folder" : "file") + " path.";
                d.entry.folder = req->folder;
                d.entry.many = req->many;
                d.entry.done = std::move(req->done);
            } else if (req->done) {
                req->done(std::move(r.paths));
            }
            d.frames_left = 3;
        }

        ImGui_ImplSDLRenderer3_NewFrame();
        ImGui_ImplSDL3_NewFrame();
        ImGui::NewFrame();
        {
            ImGuiViewport* vp = ImGui::GetMainViewport();
            ImGui::SetNextWindowPos(vp->Pos);
            ImGui::SetNextWindowSize(vp->Size);
            ImGui::Begin("##nxui_root", nullptr,
                         ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                             ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoNavFocus | ImGuiWindowFlags_NoScrollWithMouse);
            if (h.frame) h.frame();
            ImGui::End();
            if (!d.messages.empty()) {
                bool finished = false;
                int answer = 0;
                draw_message(*this, d, d.messages.front(), &finished, &answer);
                if (finished) {
                    MessageSpec m = std::move(d.messages.front());
                    d.messages.pop_front();
                    d.message_shown = false;
                    d.frames_left = 3;
                    if (m.done) m.done(answer);
                }
            } else if (d.entry.open) {
                draw_path_entry(*this, d);
            }
        }
        ImGui::Render();
        const ImVec4 bg = col::rgb(0xFFFFFF);
        SDL_SetRenderDrawColorFloat(renderer_, bg.x, bg.y, bg.z, 1.f);
        SDL_RenderClear(renderer_);
        ImGui_ImplSDLRenderer3_RenderDrawData(ImGui::GetDrawData(), renderer_);
        if (!d.shot_path.empty() && ++d.frame_no == d.shot_frames) {
            if (SDL_Surface* s = SDL_RenderReadPixels(renderer_, nullptr)) {
                SDL_SaveBMP(s, d.shot_path.c_str());
                SDL_DestroySurface(s);
            }
            d.quit = true;
        }
        SDL_RenderPresent(renderer_);
    }
    return d.exit_code;
}

// ---- widgets -----------------------------------------------------------------------------------------------------------------------------

void heading(const char* text) {
    ImGui::PushStyleColor(ImGuiCol_Text, col::accent);
    ImGui::TextUnformatted(text);
    ImGui::PopStyleColor();
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const float w = ImGui::GetContentRegionAvail().x;
    ImGui::GetWindowDrawList()->AddLine(ImVec2(p.x, p.y), ImVec2(p.x + w, p.y), ImGui::GetColorU32(ImGuiCol_Separator));
    ImGui::Dummy(ImVec2(0, ImGui::GetStyle().ItemSpacing.y * 0.5f));
}

void hint(const std::string& text, float wrap_width) {
    ImGui::PushStyleColor(ImGuiCol_Text, col::gray);
    ImGui::PushTextWrapPos(wrap_width > 0 ? ImGui::GetCursorPosX() + wrap_width : 0.f);
    ImGui::TextUnformatted(text.c_str());
    ImGui::PopTextWrapPos();
    ImGui::PopStyleColor();
}

void colored_text(const ImVec4& c, const std::string& text, bool wrapped) {
    ImGui::PushStyleColor(ImGuiCol_Text, c);
    if (wrapped) {
        ImGui::PushTextWrapPos(0.f);
        ImGui::TextUnformatted(text.c_str());
        ImGui::PopTextWrapPos();
    } else {
        ImGui::TextUnformatted(text.c_str());
    }
    ImGui::PopStyleColor();
}

bool primary_button(const char* label, const ImVec2& size) {
    ImGui::PushStyleColor(ImGuiCol_Button, col::accent);
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, col::rgb(0x0A4F96));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, col::rgb(0x083F7A));
    ImGui::PushStyleColor(ImGuiCol_Text, col::rgb(0xFFFFFF));
    ImGui::PushStyleColor(ImGuiCol_Border, col::rgb(0x0B5CAD));
    const bool r = ImGui::Button(label, size);
    ImGui::PopStyleColor(5);
    return r;
}

bool button(const char* label, const ImVec2& size) { return ImGui::Button(label, size); }

void text_box(const char* id, const std::string& text, float width) {
    std::string copy = text;
    ImGui::SetNextItemWidth(width);
    ImGui::InputText(id, &copy, ImGuiInputTextFlags_ReadOnly);
}

bool input_text(const char* id, std::string* text, float width, const char* hint_text) {
    ImGui::SetNextItemWidth(width);
    if (hint_text) return ImGui::InputTextWithHint(id, hint_text, text);
    return ImGui::InputText(id, text);
}

std::string base_name(const std::string& path) {
    const size_t p = path.find_last_of("/\\");
    return p == std::string::npos ? path : path.substr(p + 1);
}

}  // namespace nxui
