// SPDX-License-Identifier: GPL-3.0-or-later
// The setup window: plain Win32 with Common Controls v6. The player picks the game dump and the Switch keys, picks the install
// folder and presses Start. The pipeline runs on a worker thread and reports only by posting messages; every control is
// touched on the window thread.
//   mpr_setup.exe [--input <file>] [--keys <file>[,<file>]] [--out <dir>] [--quality N] [--auto-start] [--exit-when-done]
//                 [--no-message-box]
// --exit-when-done closes the window when the run ends (exit code 0 on success, else the Fail kind; 10 if the dump is not
// supported) and skips the success summary box. --no-message-box suppresses every MessageBox.

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <commctrl.h>
#include <objbase.h>
#include <shellapi.h>
#include <shobjidl.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <cwctype>
#include <filesystem>
#include <format>
#include <iterator>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "common.h"
#include "keys.h"
#include "log.h"
#include "pipeline.h"

namespace {

using mprs::Fail;
using mprs::LogLevel;
using mprs::narrow;
using mprs::Stage;
using mprs::u64;
using mprs::widen;

// Control ids. The close button uses IDCANCEL.
enum ControlId : int {
    IDC_DUMP_LABEL = 100, IDC_DUMP_EDIT, IDC_DUMP_BROWSE, IDC_DUMP_STATUS,
    IDC_KEYS_LABEL, IDC_KEYS_EDIT, IDC_KEYS_BROWSE, IDC_KEYS_HINT,
    IDC_OUT_LABEL, IDC_OUT_EDIT, IDC_OUT_BROWSE, IDC_OUT_DISK,
    IDC_QUALITY_LABEL, IDC_QUALITY, IDC_QUALITY_NOTE,
    IDC_START, IDC_OPEN_FOLDER, IDC_OPEN_LOG,
    IDC_STAGE_TITLE, IDC_STAGE_DETAIL, IDC_STAGE_BAR, IDC_OVERALL_TEXT, IDC_OVERALL_BAR, IDC_LOG,
};

// Window messages posted from worker threads. Payloads are heap objects owned by the handler (see post_owned).
constexpr UINT kMsgLog = WM_APP + 1;         // std::wstring*
constexpr UINT kMsgStageBegin = WM_APP + 2;  // StageBeginMsg*
constexpr UINT kMsgStageEnd = WM_APP + 3;    // StageEndMsg*
constexpr UINT kMsgRefresh = WM_APP + 4;     // no payload: re-read the progress atomics
constexpr UINT kMsgInspected = WM_APP + 5;   // InspectMsg*
constexpr UINT kMsgRunDone = WM_APP + 6;     // RunMsg*

constexpr UINT_PTR kTimerDebounce = 1;
constexpr UINT_PTR kTimerTick = 2;
constexpr int kMaxLogLines = 2000;
constexpr int kLogTrimLines = 500;
constexpr int kLogicalW = 644;  // client area in logical px (96 DPI)
constexpr int kLogicalH = 576;

constexpr COLORREF kGreen = RGB(0x0F, 0x6B, 0x2F);
constexpr COLORREF kRed = RGB(0xB3, 0x1B, 0x1B);
constexpr COLORREF kGray = RGB(0x5F, 0x5F, 0x5F);

constexpr wchar_t kClassName[] = L"MprSetupWindow";
constexpr wchar_t kWindowTitle[] = L"Metroid Prime Remastered PC - Setup";
constexpr wchar_t kDumpHint[] = L"Pick your own game dump (.nsp, .xci, .nsz or .xcz), or drag it onto this window.";
constexpr wchar_t kKeysHint[] =
    L"Browse to your prod.keys (dumped from your own Switch with Lockpick_RCM). Nothing is uploaded; the file is only read.";
constexpr wchar_t kIdleTitle[] = L"Choose your game dump and keys, then click Start setup.";
constexpr wchar_t kStopNote[] = L"Stopping after the current step. Progress is kept; run setup again to continue.";
constexpr wchar_t kStoppedTitle[] = L"Stopped. Progress is kept; run setup again to continue.";

struct StageBeginMsg {
    Stage stage;
    std::wstring title;
    u64 total;
    bool bytes;
    bool percent = false;   // unit is neither bytes nor textures (megapixels): show a percentage only
};
struct StageEndMsg {
    Stage stage;
    double seconds;
};
struct InspectMsg {
    std::string key;
    mprs::InputSummary summary;
};
struct RunMsg {
    bool ok = false;
    bool unexpected = false;
    Fail kind = Fail::Generic;
    std::string text;
    mprs::SetupReport report;
};

// Progress values the worker writes and the window reads. Lives in the App, which outlives the worker thread.
struct RunShared {
    std::atomic<bool> cancel{false};
    std::atomic<u64> done{0};
    std::atomic<u64> last_refresh{0};
};

struct CmdLine {
    std::wstring input;
    std::wstring out;
    std::vector<std::wstring> keys;
    int quality = -1;
    bool auto_start = false;
    bool exit_when_done = false;
    bool no_message_box = false;
};

struct ComRelease {
    void operator()(IUnknown* p) const noexcept {
        if (p) p->Release();
    }
};
template <class T>
using ComPtr = std::unique_ptr<T, ComRelease>;

// Posts a heap payload; if the window is gone the payload is freed here instead.
template <class T>
void post_owned(HWND hwnd, UINT msg, T* payload) {
    if (!PostMessageW(hwnd, msg, 0, reinterpret_cast<LPARAM>(payload))) delete payload;
}

void join_thread(std::thread& t) {
    if (t.joinable()) t.join();
}

std::wstring trim_path(std::wstring s) {
    auto ws = [](wchar_t c) { return c == L' ' || c == L'\t' || c == L'\r' || c == L'\n'; };
    while (!s.empty() && ws(s.front())) s.erase(s.begin());
    while (!s.empty() && ws(s.back())) s.pop_back();
    if (s.size() >= 2 && s.front() == L'"' && s.back() == L'"') s = s.substr(1, s.size() - 2);
    while (!s.empty() && ws(s.front())) s.erase(s.begin());
    while (!s.empty() && ws(s.back())) s.pop_back();
    return s;
}

std::wstring lower(std::wstring s) {
    for (auto& c : s) c = static_cast<wchar_t>(std::towlower(c));
    return s;
}

std::wstring exe_folder() {
    std::vector<wchar_t> buf(32768);
    const DWORD n = GetModuleFileNameW(nullptr, buf.data(), static_cast<DWORD>(buf.size()));
    return std::filesystem::path(std::wstring(buf.data(), n)).parent_path().wstring();
}

std::vector<std::wstring> split_commas(const std::wstring& s) {
    std::vector<std::wstring> out;
    std::wstring cur;
    for (wchar_t c : s) {
        if (c == L',') {
            if (!cur.empty()) out.push_back(cur);
            cur.clear();
        } else {
            cur.push_back(c);
        }
    }
    if (!cur.empty()) out.push_back(cur);
    return out;
}

CmdLine parse_command_line() {
    CmdLine c;
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (!argv) return c;
    for (int i = 1; i < argc; ++i) {
        const std::wstring a = argv[i];
        auto value = [&]() -> std::wstring { return i + 1 < argc ? std::wstring(argv[++i]) : std::wstring(); };
        if (a == L"--input") {
            c.input = trim_path(value());
        } else if (a == L"--out") {
            c.out = trim_path(value());
        } else if (a == L"--keys") {
            for (auto& k : split_commas(value())) c.keys.push_back(trim_path(k));
        } else if (a == L"--quality") {
            const int q = _wtoi(value().c_str());
            if (q >= 0 && q <= 6) c.quality = q;
        } else if (a == L"--auto-start") {
            c.auto_start = true;
        } else if (a == L"--exit-when-done") {
            c.exit_when_done = true;
        } else if (a == L"--no-message-box") {
            c.no_message_box = true;
        }
    }
    LocalFree(argv);
    for (auto* p : {&c.input, &c.out}) {
        if (!p->empty()) {
            std::error_code ec;
            *p = std::filesystem::absolute(*p, ec).wstring();
        }
    }
    for (auto& k : c.keys) {
        std::error_code ec;
        k = std::filesystem::absolute(k, ec).wstring();
    }
    return c;
}

// Text helpers for sizes and times.
std::wstring size_text(u64 bytes) {
    const double b = static_cast<double>(bytes);
    if (b >= 1e9) return std::format(L"{:.1f} GB", b / 1e9);
    return std::format(L"{:.0f} MB", b / 1e6);
}

std::wstring duration_text(double secs) {
    const long long total = std::llround(secs);
    const long long m = total / 60, s = total % 60;
    return m > 0 ? std::format(L"{} min {} s", m, s) : std::format(L"{} s", s);
}

std::wstring eta_text(double secs) {
    if (secs < 90) return std::format(L"about {:.0f} s left", secs);
    return std::format(L"about {:.0f} min left", secs / 60);
}

std::wstring clock_text(u64 ms) {
    const u64 s = ms / 1000;
    if (s >= 3600) return std::format(L"{}:{:02}:{:02}", s / 3600, (s / 60) % 60, s % 60);
    return std::format(L"{:02}:{:02}", s / 60, s % 60);
}

double stage_weight(Stage s) {
    switch (s) {
        case Stage::Exefs: return 1;
        case Stage::Romfs: return 25;
        case Stage::Textures: return 70;
        case Stage::ShaderCache: return 3;
        case Stage::Finish: return 1;
        default: return 0;
    }
}

const wchar_t* title_for(Fail k) {
    switch (k) {
        case Fail::Input: return L"This dump can't be used";
        case Fail::Keys: return L"Keys problem";
        case Fail::Disk: return L"Disk problem";
        case Fail::Cancelled: return L"Stopped";
        default: return L"Setup failed";
    }
}

// Picks a file or folder with IFileOpenDialog. Empty string when cancelled.
std::wstring pick_path(HWND owner, bool folder, const wchar_t* title, const COMDLG_FILTERSPEC* specs, UINT count) {
    IFileOpenDialog* raw = nullptr;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&raw)))) return {};
    ComPtr<IFileOpenDialog> dlg(raw);
    FILEOPENDIALOGOPTIONS opts = 0;
    dlg->GetOptions(&opts);
    opts |= FOS_FORCEFILESYSTEM | (folder ? FOS_PICKFOLDERS : FOS_FILEMUSTEXIST);
    dlg->SetOptions(opts);
    if (!folder && specs) dlg->SetFileTypes(count, specs);
    dlg->SetTitle(title);
    if (dlg->Show(owner) != S_OK) return {};
    IShellItem* item_raw = nullptr;
    if (FAILED(dlg->GetResult(&item_raw))) return {};
    ComPtr<IShellItem> item(item_raw);
    PWSTR path = nullptr;
    if (FAILED(item->GetDisplayName(SIGDN_FILESYSPATH, &path)) || !path) return {};
    std::wstring result = path;
    CoTaskMemFree(path);
    return result;
}

// Worker thread: opens the dump and reports the result. Never throws out of the thread.
void inspect_worker(HWND hwnd, std::string key, mprs::SetupOptions o) {
    std::unique_ptr<InspectMsg> msg;
    try {
        msg = std::make_unique<InspectMsg>();
        msg->key = std::move(key);
        try {
            msg->summary = mprs::inspect_input(o);
        } catch (const mprs::Error& e) {
            msg->summary.supported = false;
            msg->summary.problem = e.what();
        } catch (const std::exception& e) {
            msg->summary.supported = false;
            msg->summary.problem = std::string("Unexpected error: ") + e.what();
        } catch (...) {
            msg->summary.supported = false;
            msg->summary.problem = "Unexpected error: unknown exception.";
        }
    } catch (...) {
        return;
    }
    post_owned(hwnd, kMsgInspected, msg.release());
}

// Reports pipeline progress. Only posts to the window; the progress numbers are read from RunShared.
class GuiReporter final : public mprs::Reporter {
public:
    GuiReporter(HWND hwnd, RunShared& shared) : hwnd_(hwnd), shared_(shared) {}

    void stage_begin(Stage stage, const std::string& title, u64 total, const char* unit) override {
        shared_.done.store(0);
        const bool textures = unit && std::strcmp(unit, "textures") == 0;
        const bool percent = unit && std::strcmp(unit, "megapixels") == 0;
        post_owned(hwnd_, kMsgStageBegin, new StageBeginMsg{stage, widen(title), total, !textures && !percent, percent});
    }

    void stage_progress(u64 done) override {
        shared_.done.store(done, std::memory_order_relaxed);
        const u64 now = GetTickCount64();
        u64 last = shared_.last_refresh.load(std::memory_order_relaxed);
        if (now - last >= 100 && shared_.last_refresh.compare_exchange_strong(last, now)) {
            PostMessageW(hwnd_, kMsgRefresh, 0, 0);
        }
    }

    void stage_end(Stage stage, double seconds) override {
        post_owned(hwnd_, kMsgStageEnd, new StageEndMsg{stage, seconds});
    }

    void note(LogLevel level, const std::string& text) override {
        std::wstring line = (level == LogLevel::Warn || level == LogLevel::Error) ? L"Warning: " : L"";
        line += widen(text);
        post_owned(hwnd_, kMsgLog, new std::wstring(std::move(line)));
    }

    bool cancel_requested() override { return shared_.cancel.load(); }

private:
    HWND hwnd_;
    RunShared& shared_;
};

// Worker thread: runs the setup and posts the outcome. Never throws out of the thread.
void run_worker(HWND hwnd, mprs::SetupOptions o, RunShared& shared) {
    try {
        auto res = std::make_unique<RunMsg>();
        try {
            GuiReporter rep(hwnd, shared);
            res->report = mprs::run_setup(o, rep);
            res->ok = true;
        } catch (const mprs::Error& e) {
            res->kind = e.kind;
            res->text = e.what();
        } catch (const std::exception& e) {
            res->unexpected = true;
            res->text = std::string("Unexpected error: ") + e.what();
        } catch (...) {
            res->unexpected = true;
            res->text = "Unexpected error: unknown exception.";
        }
        post_owned(hwnd, kMsgRunDone, res.release());
    } catch (...) {
    }
}

class App {
public:
    App(HINSTANCE hinst, CmdLine cli) : hinst_(hinst), cli_(std::move(cli)) {}
    int run();

private:
    static LRESULT CALLBACK wndproc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);
    LRESULT on_message(UINT msg, WPARAM wp, LPARAM lp);

    void on_create();
    void on_destroy();
    void on_command(int id, int code);
    void on_drop(HDROP drop);
    void on_timer(UINT_PTR id);
    void on_inspected(InspectMsg* raw);
    void on_run_done(RunMsg* raw);
    void on_stage_begin(StageBeginMsg* raw);
    void on_stage_end(StageEndMsg* raw);
    void on_log(std::wstring* raw);

    void create_controls();
    void apply_fonts();
    void layout();
    int S(int logical) const { return MulDiv(logical, dpi_, 96); }
    HWND add(const wchar_t* cls, const wchar_t* text, DWORD style, int id, bool bold = false);
    HWND add_label(const wchar_t* text, int id, bool bold = false, bool gray = false, DWORD extra = 0);

    static std::wstring get_text(HWND h);
    static void set_text(HWND h, const std::wstring& s);
    void set_edit(HWND h, const std::wstring& s);
    void set_dump_status(const std::wstring& text, COLORREF color);
    void set_disk_line(const std::wstring& text);
    void set_bar(HWND bar, double fraction);

    std::wstring out_text() const { return trim_path(get_text(out_edit_)); }
    std::vector<std::wstring> keys_for_run() const;
    std::string current_key() const;
    mprs::SetupOptions make_options() const;
    int quality_value() const;
    bool can_start() const;

    void browse_dump();
    void browse_keys();
    void browse_folder();
    void set_dump(const std::wstring& path);
    void set_keys(const std::wstring& path, const std::wstring& source);
    void update_keys_hint();
    void request_inspect();
    void start_inspect(const std::string& key);
    void show_inspect_result();
    void start_run();
    void request_stop();
    void close_requested();
    void update_controls();
    void refresh_progress();
    void append_log(const std::wstring& line);
    void show_box(const wchar_t* title, const std::wstring& text, UINT icon);
    void open_path(const std::wstring& path);

    HINSTANCE hinst_;
    CmdLine cli_;
    HWND hwnd_ = nullptr;
    UINT dpi_ = 96;
    HFONT font_ = nullptr;
    HFONT bold_font_ = nullptr;
    std::vector<HWND> all_;
    std::vector<HWND> bold_;
    std::vector<HWND> gray_;

    HWND dump_label_ = nullptr, dump_edit_ = nullptr, dump_browse_ = nullptr, dump_status_ = nullptr;
    HWND keys_label_ = nullptr, keys_edit_ = nullptr, keys_browse_ = nullptr, keys_hint_ = nullptr;
    HWND out_label_ = nullptr, out_edit_ = nullptr, out_browse_ = nullptr, out_disk_ = nullptr;
    HWND quality_label_ = nullptr, quality_ = nullptr, quality_note_ = nullptr;
    HWND start_ = nullptr, open_folder_ = nullptr, open_log_ = nullptr, close_ = nullptr;
    HWND stage_title_ = nullptr, stage_detail_ = nullptr, stage_bar_ = nullptr;
    HWND overall_text_ = nullptr, overall_bar_ = nullptr, log_ = nullptr;
    std::vector<int> quality_values_;
    COLORREF status_color_ = kGray;

    // Inputs.
    std::wstring dump_path_;
    std::vector<std::wstring> extra_keys_;   // keys given on the command line after the first
    std::wstring auto_keys_source_;          // set when the keys were found automatically
    bool programmatic_ = false;              // true while the window sets an edit's text itself
    bool auto_start_pending_ = false;

    // Inspection.
    std::thread inspect_thread_;
    bool inspect_running_ = false;
    bool inspect_again_ = false;
    std::string applied_key_;                // inputs the current result belongs to
    mprs::InputSummary summary_;
    bool supported_ = false;

    // Run.
    std::thread run_thread_;
    RunShared shared_;
    bool running_ = false;
    bool stop_requested_ = false;
    bool close_after_run_ = false;
    bool run_success_ = false;
    Stage stage_ = Stage::Inspect;
    bool stage_active_ = false;
    u64 stage_total_ = 0;
    bool stage_bytes_ = true;
    bool stage_percent_ = false;
    u64 stage_started_ms_ = 0;
    u64 run_started_ms_ = 0;
    double completed_weight_ = 0;

    int exit_code_ = 0;
};

int App::run() {
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof wc;
    wc.lpfnWndProc = &App::wndproc;
    wc.hInstance = hinst_;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = reinterpret_cast<HBRUSH>(static_cast<INT_PTR>(COLOR_WINDOW + 1));
    wc.lpszClassName = kClassName;
    if (!RegisterClassExW(&wc)) return static_cast<int>(Fail::Generic);

    // Created at a placeholder size; WM_CREATE reads the window's own DPI, then the real size is set from that DPI.
    const DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX | WS_CLIPCHILDREN;
    CreateWindowExW(0, kClassName, kWindowTitle, style, CW_USEDEFAULT, CW_USEDEFAULT, 0, 0, nullptr, nullptr, hinst_, this);
    if (!hwnd_) return static_cast<int>(Fail::Generic);
    // The DPI at WM_CREATE can still be the placeholder; the window's DPI is only settled once it is placed.
    dpi_ = GetDpiForWindow(hwnd_);
    apply_fonts();
    RECT rc{0, 0, S(kLogicalW), S(kLogicalH)};
    AdjustWindowRectExForDpi(&rc, style, FALSE, 0, dpi_);
    const int w = rc.right - rc.left, h = rc.bottom - rc.top;
    RECT wa{};
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &wa, 0);
    const int x = wa.left + (wa.right - wa.left - w) / 2;
    const int y = wa.top + (wa.bottom - wa.top - h) / 2;
    SetWindowPos(hwnd_, nullptr, x, y, w, h, SWP_NOZORDER | SWP_NOACTIVATE);
    ShowWindow(hwnd_, SW_SHOW);
    UpdateWindow(hwnd_);

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        if (!IsDialogMessageW(hwnd_, &msg)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }
    return exit_code_;
}

LRESULT CALLBACK App::wndproc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    App* self = nullptr;
    if (msg == WM_NCCREATE) {
        self = static_cast<App*>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams);
        self->hwnd_ = hwnd;
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    } else {
        self = reinterpret_cast<App*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    }
    if (!self) return DefWindowProcW(hwnd, msg, wp, lp);
    return self->on_message(msg, wp, lp);
}

LRESULT App::on_message(UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_CREATE:
            on_create();
            return 0;
        case WM_SIZE:
            layout();
            return 0;
        case WM_DPICHANGED: {
            dpi_ = HIWORD(wp);
            const RECT* r = reinterpret_cast<const RECT*>(lp);
            SetWindowPos(hwnd_, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top, SWP_NOZORDER | SWP_NOACTIVATE);
            apply_fonts();
            layout();
            return 0;
        }
        case WM_COMMAND:
            on_command(LOWORD(wp), HIWORD(wp));
            return 0;
        case WM_DROPFILES:
            on_drop(reinterpret_cast<HDROP>(wp));
            return 0;
        case WM_TIMER:
            on_timer(static_cast<UINT_PTR>(wp));
            return 0;
        case WM_CTLCOLORSTATIC: {
            HDC dc = reinterpret_cast<HDC>(wp);
            HWND c = reinterpret_cast<HWND>(lp);
            COLORREF col = GetSysColor(COLOR_WINDOWTEXT);
            if (c == dump_status_) {
                col = status_color_;
            } else if (std::find(gray_.begin(), gray_.end(), c) != gray_.end()) {
                col = kGray;
            }
            SetBkMode(dc, TRANSPARENT);
            SetTextColor(dc, col);
            return reinterpret_cast<LRESULT>(GetSysColorBrush(COLOR_WINDOW));
        }
        case kMsgLog:
            on_log(reinterpret_cast<std::wstring*>(lp));
            return 0;
        case kMsgStageBegin:
            on_stage_begin(reinterpret_cast<StageBeginMsg*>(lp));
            return 0;
        case kMsgStageEnd:
            on_stage_end(reinterpret_cast<StageEndMsg*>(lp));
            return 0;
        case kMsgRefresh:
            refresh_progress();
            return 0;
        case kMsgInspected:
            on_inspected(reinterpret_cast<InspectMsg*>(lp));
            return 0;
        case kMsgRunDone:
            on_run_done(reinterpret_cast<RunMsg*>(lp));
            return 0;
        case WM_CLOSE:
            close_requested();
            return 0;
        case WM_DESTROY:
            on_destroy();
            return 0;
        default:
            return DefWindowProcW(hwnd_, msg, wp, lp);
    }
}

HWND App::add(const wchar_t* cls, const wchar_t* text, DWORD style, int id, bool bold) {
    HWND h = CreateWindowExW(0, cls, text, WS_CHILD | WS_VISIBLE | style, 0, 0, 0, 0, hwnd_,
                             reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), hinst_, nullptr);
    all_.push_back(h);
    if (bold) bold_.push_back(h);
    return h;
}

HWND App::add_label(const wchar_t* text, int id, bool bold, bool gray, DWORD extra) {
    HWND h = add(L"STATIC", text, SS_LEFT | SS_NOPREFIX | extra, id, bold);
    if (gray) gray_.push_back(h);
    return h;
}

void App::create_controls() {
    const DWORD edit = WS_BORDER | ES_AUTOHSCROLL | WS_TABSTOP;
    const DWORD button = BS_PUSHBUTTON | WS_TABSTOP;

    dump_label_ = add_label(L"Your game dump", IDC_DUMP_LABEL, true);
    dump_edit_ = add(L"EDIT", L"", edit | ES_READONLY, IDC_DUMP_EDIT);
    dump_browse_ = add(L"BUTTON", L"Browse...", button, IDC_DUMP_BROWSE);
    dump_status_ = add_label(L"", IDC_DUMP_STATUS);

    keys_label_ = add_label(L"Your Switch keys", IDC_KEYS_LABEL, true);
    keys_edit_ = add(L"EDIT", L"", edit, IDC_KEYS_EDIT);
    keys_browse_ = add(L"BUTTON", L"Browse...", button, IDC_KEYS_BROWSE);
    keys_hint_ = add_label(kKeysHint, IDC_KEYS_HINT, false, true);

    out_label_ = add_label(L"Install to", IDC_OUT_LABEL, true);
    out_edit_ = add(L"EDIT", L"", edit, IDC_OUT_EDIT);
    out_browse_ = add(L"BUTTON", L"Browse...", button, IDC_OUT_BROWSE);
    out_disk_ = add_label(L"", IDC_OUT_DISK, false, true);

    quality_label_ = add_label(L"Texture quality", IDC_QUALITY_LABEL, true);
    quality_ = add(L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP, IDC_QUALITY);
    quality_note_ = add_label(L"Converts the game's textures once so they load fast. About 3 to 12 minutes.", IDC_QUALITY_NOTE, false, true);

    start_ = add(L"BUTTON", L"Start setup", BS_DEFPUSHBUTTON | WS_TABSTOP, IDC_START);
    open_folder_ = add(L"BUTTON", L"Open folder", button, IDC_OPEN_FOLDER);
    open_log_ = add(L"BUTTON", L"Open log", button, IDC_OPEN_LOG);
    close_ = add(L"BUTTON", L"Close", button, IDCANCEL);
    ShowWindow(open_folder_, SW_HIDE);
    ShowWindow(open_log_, SW_HIDE);

    stage_title_ = add_label(kIdleTitle, IDC_STAGE_TITLE, true);
    stage_detail_ = add_label(L"", IDC_STAGE_DETAIL, false, true);
    stage_bar_ = add(PROGRESS_CLASSW, L"", 0, IDC_STAGE_BAR);
    overall_text_ = add_label(L"", IDC_OVERALL_TEXT, false, true);
    overall_bar_ = add(PROGRESS_CLASSW, L"", 0, IDC_OVERALL_BAR);
    log_ = add(L"EDIT", L"", ES_MULTILINE | ES_AUTOVSCROLL | ES_READONLY | WS_VSCROLL | WS_BORDER, IDC_LOG);

    SendMessageW(stage_bar_, PBM_SETRANGE32, 0, 1000);
    SendMessageW(overall_bar_, PBM_SETRANGE32, 0, 1000);

    // The combo's window is taller than its closed box (room for the drop list), so it overlaps the rows below it. Those rows
    // are created after it and therefore sit on top and receive the clicks.
    SendMessageW(quality_, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"Balanced (recommended)"));
    quality_values_.push_back(3);
    SendMessageW(quality_, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"Faster, lower quality"));
    quality_values_.push_back(1);
    SendMessageW(quality_, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"Best, slower"));
    quality_values_.push_back(5);
    SendMessageW(quality_, CB_SETCURSEL, 0, 0);
}

void App::apply_fonts() {
    NONCLIENTMETRICSW ncm{};
    ncm.cbSize = sizeof ncm;
    SystemParametersInfoForDpi(SPI_GETNONCLIENTMETRICS, sizeof ncm, &ncm, 0, dpi_);
    LOGFONTW lf = ncm.lfMessageFont;
    HFONT regular = CreateFontIndirectW(&lf);
    lf.lfWeight = FW_SEMIBOLD;
    HFONT bold = CreateFontIndirectW(&lf);
    for (HWND h : all_) {
        const bool is_bold = std::find(bold_.begin(), bold_.end(), h) != bold_.end();
        SendMessageW(h, WM_SETFONT, reinterpret_cast<WPARAM>(is_bold ? bold : regular), TRUE);
    }
    if (font_) DeleteObject(font_);
    if (bold_font_) DeleteObject(bold_font_);
    font_ = regular;
    bold_font_ = bold;
}

void App::layout() {
    if (!dump_edit_) return;
    RECT rc{};
    GetClientRect(hwnd_, &rc);
    const int cw = rc.right - rc.left;
    const int x0 = S(16);
    const int w = cw - 2 * x0;
    const int bw = S(110), gap = S(8);
    const int ew = w - bw - gap;
    const int xb = x0 + w - bw;
    auto put = [](HWND h, int x, int y, int cx, int cy) { MoveWindow(h, x, y, cx, cy, TRUE); };

    int y = S(12);
    put(dump_label_, x0, y, w, S(18));
    y += S(22);
    put(dump_edit_, x0, y, ew, S(26));
    put(dump_browse_, xb, y, bw, S(26));
    y += S(32);
    put(dump_status_, x0, y, w, S(30));
    y += S(34);

    put(keys_label_, x0, y, w, S(18));
    y += S(22);
    put(keys_edit_, x0, y, ew, S(26));
    put(keys_browse_, xb, y, bw, S(26));
    y += S(32);
    put(keys_hint_, x0, y, w, S(30));
    y += S(34);

    put(out_label_, x0, y, w, S(18));
    y += S(22);
    put(out_edit_, x0, y, ew, S(26));
    put(out_browse_, xb, y, bw, S(26));
    y += S(32);
    put(out_disk_, x0, y, w, S(18));
    y += S(26);

    put(quality_label_, x0, y, w, S(18));
    y += S(22);
    const int qy = y;
    put(quality_, x0, qy, S(260), S(96));
    put(quality_note_, x0 + S(272), qy + S(2), w - S(272), S(30));
    y = qy + S(36);

    put(start_, x0, y, S(150), S(28));
    put(open_folder_, x0 + S(160), y, S(130), S(28));
    put(open_log_, x0 + S(300), y, S(110), S(28));
    put(close_, x0 + w - S(110), y, S(110), S(28));
    y += S(38);

    put(stage_title_, x0, y, w, S(18));
    y += S(20);
    put(stage_detail_, x0, y, w, S(18));
    y += S(20);
    put(stage_bar_, x0, y, w, S(14));
    y += S(20);
    put(overall_text_, x0, y, w, S(18));
    y += S(20);
    put(overall_bar_, x0, y, w, S(12));
    y += S(18);
    put(log_, x0, y, w, std::max(S(60), static_cast<int>(rc.bottom) - S(16) - y));
}

void App::on_create() {
    dpi_ = GetDpiForWindow(hwnd_);
    create_controls();
    apply_fonts();
    layout();
    DragAcceptFiles(hwnd_, TRUE);
    SetTimer(hwnd_, kTimerTick, 250, nullptr);
    mprs::log_set_sink(
        [hwnd = hwnd_](LogLevel, const std::string& text) { post_owned(hwnd, kMsgLog, new std::wstring(widen(text))); },
        LogLevel::Info);

    set_edit(out_edit_, cli_.out.empty() ? exe_folder() + L"\\game" : cli_.out);
    dump_path_ = trim_path(cli_.input);
    set_text(dump_edit_, dump_path_);
    if (!cli_.keys.empty()) {
        set_edit(keys_edit_, cli_.keys[0]);
        extra_keys_.assign(cli_.keys.begin() + 1, cli_.keys.end());
        update_keys_hint();
    } else {
        const auto found = mprs::find_key_files();
        if (!found.empty()) set_keys(widen(found[0].path), widen(found[0].source));
    }
    if (cli_.quality >= 0) {
        int index = -1;
        for (size_t i = 0; i < quality_values_.size(); ++i) {
            if (quality_values_[i] == cli_.quality) index = static_cast<int>(i);
        }
        if (index < 0) {
            SendMessageW(quality_, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(std::format(L"Custom (quality {})", cli_.quality).c_str()));
            quality_values_.push_back(cli_.quality);
            index = static_cast<int>(quality_values_.size()) - 1;
        }
        SendMessageW(quality_, CB_SETCURSEL, index, 0);
    }
    auto_start_pending_ = cli_.auto_start;
    update_controls();
    request_inspect();
}

void App::on_destroy() {
    mprs::log_set_sink({}, LogLevel::Info);
    KillTimer(hwnd_, kTimerTick);
    KillTimer(hwnd_, kTimerDebounce);
    join_thread(inspect_thread_);
    join_thread(run_thread_);
    mprs::log_close();
    if (font_) DeleteObject(font_);
    if (bold_font_) DeleteObject(bold_font_);
    font_ = bold_font_ = nullptr;
    PostQuitMessage(0);
}

void App::on_command(int id, int code) {
    switch (id) {
        case IDC_DUMP_BROWSE:
            if (code == BN_CLICKED) browse_dump();
            break;
        case IDC_KEYS_BROWSE:
            if (code == BN_CLICKED) browse_keys();
            break;
        case IDC_OUT_BROWSE:
            if (code == BN_CLICKED) browse_folder();
            break;
        case IDC_KEYS_EDIT:
            if (code == EN_CHANGE && !programmatic_) {
                extra_keys_.clear();
                auto_keys_source_.clear();
                update_keys_hint();
                SetTimer(hwnd_, kTimerDebounce, 400, nullptr);
                update_controls();
            }
            break;
        case IDC_OUT_EDIT:
            if (code == EN_CHANGE && !programmatic_) {
                SetTimer(hwnd_, kTimerDebounce, 400, nullptr);
                update_controls();
            }
            break;
        case IDC_START:
            if (code == BN_CLICKED) {
                if (running_) {
                    request_stop();
                } else {
                    start_run();
                }
            }
            break;
        case IDC_OPEN_FOLDER:
            if (code == BN_CLICKED) open_path(out_text());
            break;
        case IDC_OPEN_LOG:
            if (code == BN_CLICKED) open_path(out_text() + L"\\setup.log");
            break;
        case IDCANCEL:
            if (code == BN_CLICKED) close_requested();
            break;
        default:
            break;
    }
}

void App::on_drop(HDROP drop) {
    std::wstring path;
    const UINT len = DragQueryFileW(drop, 0, nullptr, 0);
    if (len > 0) {
        std::wstring buf(len + 1, L'\0');
        DragQueryFileW(drop, 0, buf.data(), len + 1);
        buf.resize(len);
        path = buf;
    }
    DragFinish(drop);
    if (running_ || path.empty()) return;
    if (lower(std::filesystem::path(path).extension().wstring()) == L".keys") {
        set_keys(path, L"");
        request_inspect();
    } else {
        set_dump(path);
    }
}

void App::on_timer(UINT_PTR id) {
    if (id == kTimerDebounce) {
        KillTimer(hwnd_, kTimerDebounce);
        request_inspect();
    } else if (id == kTimerTick && running_) {
        refresh_progress();
    }
}

void App::on_inspected(InspectMsg* raw) {
    std::unique_ptr<InspectMsg> msg(raw);
    join_thread(inspect_thread_);
    inspect_running_ = false;
    const bool current = msg->key == current_key();
    if (current) {
        applied_key_ = msg->key;
        summary_ = msg->summary;
        supported_ = summary_.supported;
        show_inspect_result();
    }
    if (inspect_again_ || !current) {
        inspect_again_ = false;
        request_inspect();
    }
    if (!current) return;
    if (cli_.exit_when_done && !running_ && !supported_) {
        exit_code_ = 10;
        DestroyWindow(hwnd_);
        return;
    }
    if (auto_start_pending_ && can_start()) {
        auto_start_pending_ = false;
        start_run();
    }
    update_controls();
}

void App::on_run_done(RunMsg* raw) {
    std::unique_ptr<RunMsg> r(raw);
    join_thread(run_thread_);
    running_ = false;
    stop_requested_ = false;
    stage_active_ = false;
    const u64 elapsed_ms = GetTickCount64() - run_started_ms_;

    if (r->ok) {
        run_success_ = true;
        const double secs = r->report.total_seconds > 0 ? r->report.total_seconds : elapsed_ms / 1000.0;
        set_text(stage_title_, std::format(L"Setup finished in {}. Your game folder is ready:", duration_text(secs)));
        set_text(stage_detail_, L"");
        SendMessageW(stage_bar_, PBM_SETPOS, 1000, 0);
        SendMessageW(overall_bar_, PBM_SETPOS, 1000, 0);
        set_text(overall_text_, std::format(L"Overall 100%     Elapsed {}", clock_text(elapsed_ms)));
        MPRS_LOGI("Setup finished in %s", narrow(duration_text(secs)).c_str());
        if (!cli_.no_message_box && !cli_.exit_when_done) {
            const auto& rep = r->report;
            show_box(L"Setup finished",
                     std::format(L"Setup finished in {}.\n\nFiles written: {}\nFiles already in place: {}\n"
                                 L"Textures converted: {}, skipped: {}, failed: {}",
                                 duration_text(secs), rep.files_written, rep.files_skipped, rep.textures_converted,
                                 rep.textures_skipped, rep.textures_failed),
                     MB_ICONINFORMATION);
        }
        exit_code_ = 0;
    } else {
        const bool cancelled = !r->unexpected && r->kind == Fail::Cancelled;
        set_text(stage_title_, cancelled ? kStoppedTitle : L"Setup did not finish. See the log below.");
        set_text(stage_detail_, L"");
        const std::wstring text = widen(r->text);
        MPRS_LOGE("Setup stopped: %s", r->text.c_str());
        if (!(cli_.no_message_box || (close_after_run_ && cancelled))) {
            show_box(r->unexpected ? L"Setup failed" : title_for(r->kind), text,
                     cancelled ? MB_ICONINFORMATION : MB_ICONERROR);
        }
        exit_code_ = r->unexpected ? static_cast<int>(Fail::Generic) : static_cast<int>(r->kind);
    }
    update_controls();
    if (close_after_run_ || cli_.exit_when_done) DestroyWindow(hwnd_);
}

void App::on_stage_begin(StageBeginMsg* raw) {
    std::unique_ptr<StageBeginMsg> m(raw);
    stage_ = m->stage;
    stage_active_ = true;
    stage_total_ = m->total;
    stage_bytes_ = m->bytes;
    stage_percent_ = m->percent;
    stage_started_ms_ = GetTickCount64();
    set_text(stage_title_, m->title);
    refresh_progress();
}

void App::on_stage_end(StageEndMsg* raw) {
    std::unique_ptr<StageEndMsg> m(raw);
    completed_weight_ += stage_weight(m->stage);
    stage_active_ = false;
    refresh_progress();
}

void App::on_log(std::wstring* raw) {
    std::unique_ptr<std::wstring> line(raw);
    append_log(*line);
}

void App::refresh_progress() {
    if (!running_) return;
    const u64 done = shared_.done.load();
    double frac = 0;
    std::wstring detail;
    if (stage_active_ && stage_total_ > 0) {
        frac = std::min(1.0, static_cast<double>(done) / static_cast<double>(stage_total_));
        detail = stage_percent_ ? std::format(L"{}% done", static_cast<int>(frac * 100))
                 : stage_bytes_ ? std::format(L"{} of {}", size_text(done), size_text(stage_total_))
                                : std::format(L"{} of {} textures", done, stage_total_);
        const double stage_secs = (GetTickCount64() - stage_started_ms_) / 1000.0;
        if (done > 0 && done < stage_total_ && stage_secs >= 5) {
            const double rate = static_cast<double>(done) / stage_secs;
            const double left = static_cast<double>(stage_total_ - done) / rate;
            detail += L"   -   " + eta_text(left);
        }
    }
    set_bar(stage_bar_, frac);
    set_text(stage_detail_, detail);
    const double overall = completed_weight_ + (stage_active_ ? stage_weight(stage_) * frac : 0.0);
    set_bar(overall_bar_, overall / 100.0);
    set_text(overall_text_, std::format(L"Overall {:.0f}%     Elapsed {}", overall, clock_text(GetTickCount64() - run_started_ms_)));
}

void App::append_log(const std::wstring& line) {
    const int end = GetWindowTextLengthW(log_);
    SendMessageW(log_, EM_SETSEL, static_cast<WPARAM>(end), static_cast<LPARAM>(end));
    const std::wstring text = line + L"\r\n";
    SendMessageW(log_, EM_REPLACESEL, FALSE, reinterpret_cast<LPARAM>(text.c_str()));
    if (SendMessageW(log_, EM_GETLINECOUNT, 0, 0) > kMaxLogLines) {
        const LRESULT cut = SendMessageW(log_, EM_LINEINDEX, kLogTrimLines, 0);
        if (cut > 0) {
            SendMessageW(log_, EM_SETSEL, 0, cut);
            SendMessageW(log_, EM_REPLACESEL, FALSE, reinterpret_cast<LPARAM>(L""));
        }
    }
    SendMessageW(log_, EM_SCROLLCARET, 0, 0);
}

void App::show_box(const wchar_t* title, const std::wstring& text, UINT icon) {
    if (cli_.no_message_box) return;
    MessageBoxW(hwnd_, text.c_str(), title, MB_OK | icon);
}

void App::open_path(const std::wstring& path) {
    ShellExecuteW(hwnd_, L"open", path.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

std::wstring App::get_text(HWND h) {
    const int n = GetWindowTextLengthW(h);
    std::wstring s(static_cast<size_t>(n), L'\0');
    if (n > 0) GetWindowTextW(h, s.data(), n + 1);
    return s;
}

void App::set_text(HWND h, const std::wstring& s) {
    SetWindowTextW(h, s.c_str());
}

void App::set_edit(HWND h, const std::wstring& s) {
    programmatic_ = true;
    SetWindowTextW(h, s.c_str());
    programmatic_ = false;
}

void App::set_dump_status(const std::wstring& text, COLORREF color) {
    status_color_ = color;
    set_text(dump_status_, text);
}

void App::set_disk_line(const std::wstring& text) {
    set_text(out_disk_, text);
}

void App::set_bar(HWND bar, double fraction) {
    const double f = std::clamp(fraction, 0.0, 1.0);
    SendMessageW(bar, PBM_SETPOS, static_cast<WPARAM>(std::lround(f * 1000.0)), 0);
}

std::vector<std::wstring> App::keys_for_run() const {
    std::vector<std::wstring> v;
    const std::wstring first = trim_path(get_text(keys_edit_));
    if (first.empty()) return v;
    v.push_back(first);
    for (const auto& e : extra_keys_) v.push_back(e);
    if (extra_keys_.empty()) {
        const std::string title = mprs::sibling_title_keys(narrow(first));
        if (!title.empty()) v.push_back(widen(title));
    }
    return v;
}

std::string App::current_key() const {
    std::string k = narrow(dump_path_) + "\n";
    for (const auto& p : keys_for_run()) k += narrow(p) + "\n";
    k += narrow(out_text());
    return k;
}

mprs::SetupOptions App::make_options() const {
    mprs::SetupOptions o;
    o.input_path = narrow(dump_path_);
    for (const auto& k : keys_for_run()) o.key_files.push_back(narrow(k));
    o.out_dir = narrow(out_text());
    o.bc7_quality = quality_value();
    return o;
}

int App::quality_value() const {
    const int i = static_cast<int>(SendMessageW(quality_, CB_GETCURSEL, 0, 0));
    if (i < 0 || i >= static_cast<int>(quality_values_.size())) return 3;
    return quality_values_[static_cast<size_t>(i)];
}

bool App::can_start() const {
    return !running_ && supported_ && !dump_path_.empty() && !get_text(keys_edit_).empty() && applied_key_ == current_key();
}

void App::update_controls() {
    if (!start_) return;
    const bool idle = !running_;
    for (HWND h : {dump_browse_, keys_edit_, keys_browse_, out_edit_, out_browse_, quality_}) EnableWindow(h, idle);
    if (running_) {
        set_text(start_, stop_requested_ ? L"Stopping..." : L"Stop");
        EnableWindow(start_, !stop_requested_);
    } else {
        set_text(start_, L"Start setup");
        EnableWindow(start_, can_start());
    }
    ShowWindow(open_folder_, run_success_ ? SW_SHOW : SW_HIDE);
    ShowWindow(open_log_, run_success_ ? SW_SHOW : SW_HIDE);
}

void App::browse_dump() {
    static const COMDLG_FILTERSPEC specs[] = {
        {L"Switch game dumps (*.nsp;*.xci;*.nsz;*.xcz)", L"*.nsp;*.xci;*.nsz;*.xcz"},
        {L"All files (*.*)", L"*.*"},
    };
    const std::wstring p = pick_path(hwnd_, false, L"Choose your game dump", specs, 2);
    if (!p.empty()) set_dump(p);
}

void App::browse_keys() {
    static const COMDLG_FILTERSPEC specs[] = {
        {L"Switch keys (*.keys)", L"*.keys"},
        {L"All files (*.*)", L"*.*"},
    };
    const std::wstring p = pick_path(hwnd_, false, L"Choose your prod.keys", specs, 2);
    if (!p.empty()) {
        set_keys(p, L"");
        request_inspect();
    }
}

void App::browse_folder() {
    const std::wstring p = pick_path(hwnd_, true, L"Choose the folder to install the game to", nullptr, 0);
    if (!p.empty()) {
        set_edit(out_edit_, p);
        request_inspect();
    }
}

void App::set_dump(const std::wstring& path) {
    dump_path_ = trim_path(path);
    set_text(dump_edit_, dump_path_);
    request_inspect();
}

void App::set_keys(const std::wstring& path, const std::wstring& source) {
    extra_keys_.clear();
    auto_keys_source_ = source;
    set_edit(keys_edit_, trim_path(path));
    update_keys_hint();
}

void App::update_keys_hint() {
    set_text(keys_hint_, auto_keys_source_.empty() ? std::wstring(kKeysHint) : L"Found automatically: " + auto_keys_source_);
}

void App::request_inspect() {
    if (!hwnd_) return;
    if (dump_path_.empty()) {
        applied_key_.clear();
        supported_ = false;
        set_dump_status(kDumpHint, kGray);
        set_disk_line(L"");
        update_controls();
        return;
    }
    const std::string key = current_key();
    if (key == applied_key_) {
        update_controls();
        return;
    }
    if (inspect_running_) {
        inspect_again_ = true;
        supported_ = false;
        set_dump_status(L"Checking the dump...", kGray);
        update_controls();
        return;
    }
    start_inspect(key);
}

void App::start_inspect(const std::string& key) {
    inspect_running_ = true;
    inspect_again_ = false;
    supported_ = false;
    set_dump_status(L"Checking the dump...", kGray);
    update_controls();
    try {
        inspect_thread_ = std::thread(inspect_worker, hwnd_, key, make_options());
    } catch (const std::exception& e) {
        inspect_running_ = false;
        set_dump_status(widen(std::string("Could not start the check: ") + e.what()), kRed);
        update_controls();
    }
}

void App::show_inspect_result() {
    if (summary_.supported) {
        set_dump_status(std::format(L"Metroid Prime Remastered found ({}). Needs {}, {} free.", widen(summary_.container),
                                    size_text(summary_.disk_needed), size_text(summary_.disk_free)),
                        kGreen);
        set_disk_line(std::format(L"This folder needs about {} (free: {}).", size_text(summary_.disk_needed),
                                  size_text(summary_.disk_free)));
    } else {
        std::wstring problem = widen(summary_.problem);
        if (problem.empty()) problem = L"This is not a dump this setup can use.";
        set_dump_status(problem, kRed);
        set_disk_line(L"");
    }
}

void App::start_run() {
    if (!can_start()) return;
    const std::wstring out = out_text();
    std::error_code ec;
    std::filesystem::create_directories(out, ec);
    if (ec) {
        show_box(title_for(Fail::Disk), L"Can't create the install folder:\n" + out + L"\n" + widen(ec.message()), MB_ICONERROR);
        return;
    }
    mprs::log_open(narrow(out) + "\\setup.log");
    mprs::SetupOptions o = make_options();
    MPRS_LOGI("Setup started: input %s, folder %s, texture quality %d", o.input_path.c_str(), o.out_dir.c_str(), o.bc7_quality);

    shared_.cancel = false;
    shared_.done = 0;
    shared_.last_refresh = 0;
    running_ = true;
    stop_requested_ = false;
    close_after_run_ = false;
    run_success_ = false;
    stage_active_ = false;
    stage_total_ = 0;
    completed_weight_ = 0;
    run_started_ms_ = GetTickCount64();
    set_text(stage_title_, L"Starting...");
    set_text(stage_detail_, L"");
    SendMessageW(stage_bar_, PBM_SETPOS, 0, 0);
    SendMessageW(overall_bar_, PBM_SETPOS, 0, 0);
    set_text(overall_text_, L"Overall 0%     Elapsed 00:00");
    update_controls();
    try {
        run_thread_ = std::thread([hwnd = hwnd_, o = std::move(o), shared = &shared_]() { run_worker(hwnd, o, *shared); });
    } catch (const std::exception& e) {
        running_ = false;
        set_text(stage_title_, widen(std::string("Could not start the setup: ") + e.what()));
        update_controls();
    }
}

void App::request_stop() {
    if (!running_ || stop_requested_) return;
    shared_.cancel = true;
    stop_requested_ = true;
    set_text(stage_title_, kStopNote);
    update_controls();
}

void App::close_requested() {
    if (running_) {
        request_stop();
        close_after_run_ = true;
        return;
    }
    DestroyWindow(hwnd_);
}

}  // namespace

int WINAPI wWinMain(HINSTANCE hinst, HINSTANCE, LPWSTR, int) {
    const HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    struct ComInit {
        HRESULT hr;
        ~ComInit() {
            if (SUCCEEDED(hr)) CoUninitialize();
        }
    } com{hr};

    INITCOMMONCONTROLSEX icc{sizeof icc, ICC_PROGRESS_CLASS | ICC_STANDARD_CLASSES};
    InitCommonControlsEx(&icc);

    App app(hinst, parse_command_line());
    return app.run();
}
