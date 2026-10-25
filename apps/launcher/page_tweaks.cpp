// SPDX-License-Identifier: GPL-3.0-or-later
// Tweaks tab: optional looks ([tweaks] in controls.ini, play_tweaks.h), off by default. S44: the glow colour, three 0..255 sliders with a
// colour swatch and a Reset button. A part with its own colour in the file (ball_glow / gun_glow = R,G,B) keeps it; the page only switches a
// part between following the glow colour and the game's own.
#include <algorithm>
#include <memory>
#include <string>

#include "pages.h"

namespace mprl {

namespace {

using nxrt::play::GlowMode;
using nxrt::play::GlowRgb;
using nxrt::play::TweakSettings;

constexpr const wchar_t* kSwatchClass = L"MprlGlowSwatch";

// A plain rectangle in one colour (GWLP_USERDATA holds the COLORREF).
LRESULT CALLBACK swatch_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_PAINT) {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(hwnd, &ps);
        RECT rc;
        GetClientRect(hwnd, &rc);
        HBRUSH b = CreateSolidBrush(static_cast<COLORREF>(GetWindowLongPtrW(hwnd, GWLP_USERDATA)));
        FillRect(dc, &rc, b);
        DeleteObject(b);
        FrameRect(dc, &rc, GetSysColorBrush(COLOR_WINDOWFRAME));
        EndPaint(hwnd, &ps);
        return 0;
    }
    if (msg == WM_ERASEBKGND) return 1;
    return DefWindowProcW(hwnd, msg, wp, lp);
}

void register_swatch(HINSTANCE inst) {
    static bool done = false;
    if (done) return;
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof wc;
    wc.lpfnWndProc = &swatch_proc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.lpszClassName = kSwatchClass;
    done = RegisterClassExW(&wc) != 0;
}

std::wstring byte_text(int v) { return std::to_wstring(v); }

// The part's drop-down: 0 follows the glow colour, 1 the game's own; a part with its own colour (only in the file) shows as 0 and is kept.
int part_index(GlowMode m) { return m == GlowMode::Off ? 1 : 0; }
void set_part(nxrt::play::GlowPart* p, int i) {
    if (i == 1) p->mode = GlowMode::Off;
    else if (p->mode == GlowMode::Off) p->mode = GlowMode::Auto;
}

class TweaksPage : public Page {
public:
    const wchar_t* title() const override { return L"Tweaks"; }

    void build(Form& f) override {
        register_swatch(host()->env().inst);
        f.heading(L"Glow colour");
        f.checkbox(
            L"Use a custom glow colour", [](const Settings& s) { return s.tweaks.glow_on; }, [](Settings& s, bool v) { s.tweaks.glow_on = v; },
            [this]() { update_swatch(); });
        f.hint(L"Off: the game's own colours. On: the morph ball's glow (stripes, inner glow, boost glow, trail, light), the arm cannon's charge glow and the suit's lights take this colour, live in the game.", 2);
        r_ = f.slider(
            L"Red", 0, 255, [](const Settings& s) { return int(s.tweaks.glow.r); }, [](Settings& s, int v) { s.tweaks.glow.r = uint8_t(v); }, byte_text,
            [this]() { update_swatch(); });
        g_ = f.slider(
            L"Green", 0, 255, [](const Settings& s) { return int(s.tweaks.glow.g); }, [](Settings& s, int v) { s.tweaks.glow.g = uint8_t(v); }, byte_text,
            [this]() { update_swatch(); });
        b_ = f.slider(
            L"Blue", 0, 255, [](const Settings& s) { return int(s.tweaks.glow.b); }, [](Settings& s, int v) { s.tweaks.glow.b = uint8_t(v); }, byte_text,
            [this]() { update_swatch(); });
        const RECT sw = f.custom_row(L"Colour", 28);
        const int swatch_w = std::min<int>(sw.right - sw.left, host()->env().S(160));
        swatch_ = f.create(kSwatchClass, L"", WS_CHILD | WS_VISIBLE, sw.left, sw.top, swatch_w, sw.bottom - sw.top);
        reset_id_ = f.button(L"Reset to the game's colours", 200);
        f.hint(L"The colour multiplies the game's glow: the ball's white glow takes it exactly, the yellow charge mixes with it, and a darker colour gives a dimmer glow. The cannon's own lit panels keep their colour.", 2);

        f.heading(L"Per part");
        f.combo(
            L"Suit", {L"Use the glow colour", L"The game's own colour"}, [](const Settings& s) { return part_index(s.tweaks.suit.mode); },
            [](Settings& s, int i) { set_part(&s.tweaks.suit, i); });
        f.combo(
            L"Morph ball", {L"Use the glow colour", L"The game's own colour"}, [](const Settings& s) { return part_index(s.tweaks.ball.mode); },
            [](Settings& s, int i) { set_part(&s.tweaks.ball, i); });
        f.combo(
            L"Arm cannon", {L"Use the glow colour", L"The game's own colour"}, [](const Settings& s) { return part_index(s.tweaks.gun.mode); },
            [](Settings& s, int i) { set_part(&s.tweaks.gun, i); });
        f.hint(L"A part can also get its own colour in controls.ini: [tweaks] suit_glow, ball_glow or gun_glow = R,G,B.", 1);
    }

    void load(const Settings& s) override {
        loaded_ = s;
        Page::load(s);
        update_swatch();
    }

    bool command(int id, int code, HWND) override {
        if (id != reset_id_ || code != BN_CLICKED) return false;
        Settings s = loaded_;
        store(&s);
        s.tweaks = TweakSettings{};
        Page::load(s);
        update_swatch();
        host()->changed();
        return true;
    }

    void rebuilt() override { update_swatch(); }

private:
    void update_swatch() {
        if (!swatch_) return;
        auto pos = [](HWND h) { return h ? static_cast<int>(SendMessageW(h, TBM_GETPOS, 0, 0)) : 255; };
        const COLORREF c = RGB(pos(r_), pos(g_), pos(b_));
        SetWindowLongPtrW(swatch_, GWLP_USERDATA, static_cast<LONG_PTR>(c));
        InvalidateRect(swatch_, nullptr, FALSE);
    }

    HWND r_ = nullptr, g_ = nullptr, b_ = nullptr, swatch_ = nullptr;
    int reset_id_ = -1;
    Settings loaded_;
};

}  // namespace

std::unique_ptr<Page> make_tweaks_page() { return std::make_unique<TweaksPage>(); }

}  // namespace mprl
