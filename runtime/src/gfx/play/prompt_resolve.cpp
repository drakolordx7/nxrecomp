// SPDX-License-Identifier: GPL-3.0-or-later
#include "prompt_resolve.h"

#include <cctype>
#include <vector>

#include "nxrt/input_source.h"

namespace nxrt::play {

namespace {

struct IconInfo {
    Btn btn;
    bool pressed;
    const char* asset;
    const char* uuid;
    const char* pak;
    const char* front_uuid;   // the front end (title, menus) has its own red copy of most icons in PreloadFrontEndMPT.pak; nullptr when it has none
};

constexpr const char* kGui = "Preload/MPR1/GuiSysMP1.pak";
constexpr const char* kFront = "Preload/MPRT/PreloadFrontEndMPT.pak";

// Asset ids of the icon textures in the 1.0.0 paks (the ids tools/survey lists for TXTR_ButtonA ... TXTR_DPadIdle).
const IconInfo kIcons[kIconCount] = {
    {Btn::A, false, "TXTR_ButtonA", "eeb91bc0-dd56-49cd-87d9-3f3699a80289", kGui, "03b1ab95-b60d-4f71-8f35-a1b4ae72a562"},
    {Btn::A, true, "TXTR_ButtonAPressed", "fe1e5fa1-8db4-4e2b-8865-d1f5213719e6", kGui, "22d9d912-04c0-492a-9ffd-b51881f3d5ed"},
    {Btn::B, false, "TXTR_ButtonB", "b036b3e6-9fa1-47e3-a802-2d0166435bdc", kGui, "926d82b5-5929-4461-9908-6cac36c6d355"},
    {Btn::B, true, "TXTR_ButtonBPressed", "a6f902c6-c94f-4d7c-8a27-9c008797f258", kGui, "47e952a0-d4b0-4639-93af-f1cbebad3f49"},
    {Btn::X, false, "TXTR_ButtonX", "521a2683-2184-46fe-876c-17307bce348c", kGui, "8ff083b7-137b-4712-8bf2-4f2300926773"},
    {Btn::Y, false, "TXTR_ButtonY", "d53e206f-33c0-4615-92fd-9594f20ab8d9", kGui, "26afa6c0-7569-42eb-88a0-b0c9e3e9fd64"},
    {Btn::L, false, "TXTR_ButtonL", "5fd8b51b-df74-43ee-94ee-9346176d4ea2", kGui, "5dd346b9-c4b2-4cdb-9b6c-55a0c6c3815b"},
    {Btn::R, false, "TXTR_ButtonR", "5419cfac-a848-4caf-989a-dd278e2eb517", kGui, "73f6fa70-812d-457d-b86a-71b552aa6e57"},
    {Btn::R, true, "TXTR_ButtonRPressed", "2f02c1c4-e692-4f1c-96e9-999b9d3cc114", kGui, "c955bbd2-b2ca-49b0-9aec-f23d17fd9687"},
    {Btn::ZL, false, "TXTR_ButtonZL", "ce21fb91-8ef1-48af-80df-ea8e517987e4", kGui, "199ea232-54b4-40d5-81cc-ec31fb2e7f72"},
    {Btn::ZL, true, "TXTR_ButtonZLPressed", "812a3d75-e3ab-4530-b786-68644863f92c", kGui, "11d7237a-1f3b-4010-9286-8ea79c440032"},
    {Btn::ZR, false, "TXTR_ButtonZR", "a071b7ef-6ec7-451e-81f5-78f87530710e", kGui, "2de60d21-ed30-49f9-ad08-4e38db62138a"},
    {Btn::ZR, true, "TXTR_ButtonZRPressed", "cba3caf3-a9c7-400b-9b98-1cdd988d9486", kGui, "a08a0123-9e40-4f02-9e50-6fe77c2dab80"},
    {Btn::Plus, false, "TXTR_ButtonPlus", "289b5028-2757-42dd-bcf6-a16dd6240ad4", kGui, "5b483219-e915-418f-bd7a-193a4813a1fb"},
    {Btn::Plus, true, "TXTR_ButtonPlusPressed", "c6e47769-3e3c-4d82-adb1-f1f384642301", kGui, "289fe674-12d9-4ae7-8d56-8fba49c5ff65"},
    {Btn::Minus, false, "TXTR_ButtonMinus", "dd9a75d4-db58-41f9-be73-b2934b696997", kGui, "7b9ad0b2-83c1-40fe-bfa7-23308c05f51e"},
    {Btn::DPad, false, "TXTR_DPadIdle", "d78d38fa-a230-4363-a12b-1dc6eea1ad3a", kFront, nullptr},
};

std::string lower_ascii(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

// The Switch button bit an icon stands for; the d-pad icon stands for "up" (any direction will do for the glyph of the whole cross).
uint64_t button_bit(Btn b) {
    using namespace hid;
    switch (b) {
        case Btn::A: return btn(kBtnA);
        case Btn::B: return btn(kBtnB);
        case Btn::X: return btn(kBtnX);
        case Btn::Y: return btn(kBtnY);
        case Btn::L: return btn(kBtnL);
        case Btn::R: return btn(kBtnR);
        case Btn::ZL: return btn(kBtnZL);
        case Btn::ZR: return btn(kBtnZR);
        case Btn::Plus: return btn(kBtnPlus);
        case Btn::Minus: return btn(kBtnMinus);
        case Btn::DPad: return btn(kBtnUp);
        default: return 0;
    }
}

// Pad icons: the Xbox or PlayStation button that presses the Switch button.
const char* pad_name(PromptStyle style, bool label_abxy, Btn b) {
    const bool xbox = style == PromptStyle::Xbox;
    // Face buttons: by label A = Switch A, B = B, X = X, Y = Y (PlayStation: cross, circle, square, triangle). By position the east
    // button is Switch A, the south button B, the west button Y and the north button X (pad_map.h).
    static const char* const kXbox[4] = {"xbox/a", "xbox/b", "xbox/x", "xbox/y"};   // by label: A B X Y
    static const char* const kPs[4] = {"ps/cross", "ps/circle", "ps/square", "ps/triangle"};
    const char* const* f = xbox ? kXbox : kPs;
    switch (b) {
        case Btn::A: return label_abxy ? f[0] : f[1];     // by position Switch A is the east button: Xbox B, circle
        case Btn::B: return label_abxy ? f[1] : f[0];     // south: Xbox A, cross
        case Btn::X: return label_abxy ? f[2] : f[3];     // north: Xbox Y, triangle
        case Btn::Y: return label_abxy ? f[3] : f[2];     // west: Xbox X, square
        case Btn::L: return xbox ? "xbox/lb" : "ps/l1";
        case Btn::R: return xbox ? "xbox/rb" : "ps/r1";
        case Btn::ZL: return xbox ? "xbox/lt" : "ps/l2";
        case Btn::ZR: return xbox ? "xbox/rt" : "ps/r2";
        case Btn::Plus: return xbox ? "xbox/menu" : "ps/options";
        case Btn::Minus: return xbox ? "xbox/view" : "ps/create";
        case Btn::DPad: return xbox ? "xbox/dpad" : "ps/dpad";
        default: return "";
    }
}

// Does binding b apply in context c? A binding of c applies; an `all` binding applies unless the same input has a binding of c itself.
bool binding_applies(const Settings& s, const Binding& b, Ctx c) {
    if (b.act == Act::None) return false;
    if (c == Ctx::All) return b.ctx == Ctx::All;
    if (b.ctx == c) return true;
    if (b.ctx != Ctx::All) return false;
    for (const Binding& o : s.bindings)
        if (o.ctx == c && o.in == b.in) return false;
    return true;
}

}  // namespace

Btn icon_button(Icon i) { return kIcons[static_cast<int>(i)].btn; }
bool icon_pressed(Icon i) { return kIcons[static_cast<int>(i)].pressed; }
const char* icon_asset_name(Icon i) { return kIcons[static_cast<int>(i)].asset; }
const char* icon_asset_uuid(Icon i) { return kIcons[static_cast<int>(i)].uuid; }
const char* icon_pak_file(Icon i) { return kIcons[static_cast<int>(i)].pak; }
const char* icon_front_uuid(Icon i) { return kIcons[static_cast<int>(i)].front_uuid; }

std::string kbm_pack_name(const InputId& id) {
    const std::string n = lower_ascii(input_name(id));
    return n.empty() ? std::string() : "kbm/" + n;
}

bool input_for_button(const Settings& s, Ctx ctx, Btn b, InputId* out) {
    const uint64_t bit = button_bit(b);
    if (!bit) return false;
    // actions that do exactly this button: the named game actions first, the raw btn_ ones last
    std::vector<Act> cands;
    for (int pass = 0; pass < 2; ++pass)
        for (int a = 1; a < kActCount; ++a) {
            const bool raw = a >= static_cast<int>(Act::BtnA);
            if ((pass == 0) == raw) continue;
            const Output& o = s.out[a];
            if (o.buttons == bit && o.lx == 0 && o.ly == 0 && o.rx == 0 && o.ry == 0) cands.push_back(static_cast<Act>(a));
        }
    // the current context first, then the others (a menu button is still worth showing while playing)
    const Ctx order[] = {ctx, Ctx::Menu, Ctx::Gameplay, Ctx::Morph, Ctx::Map, Ctx::Scan, Ctx::All};
    for (Ctx c : order)
        for (Act a : cands)
            for (const Binding& bd : s.bindings)
                if (bd.act == a && binding_applies(s, bd, c)) {
                    *out = bd.in;
                    return true;
                }
    return false;
}

std::string resolve_icon(const ResolveIn& in, Icon icon) {
    const Btn b = icon_button(icon);
    std::string base;
    switch (in.style) {
        case PromptStyle::Xbox:
        case PromptStyle::PlayStation:
            base = pad_name(in.style, in.label_abxy, b);
            break;
        case PromptStyle::Keyboard: {
            if (!in.settings) return {};
            InputId id;
            if (!input_for_button(*in.settings, in.ctx, b, &id)) return {};
            base = kbm_pack_name(id);
            break;
        }
        default:
            return {};
    }
    if (base.empty()) return {};
    return icon_pressed(icon) ? base + "_pressed" : base;
}

PromptStyle auto_style(LastDevice last, int last_pad_kind, int connected_pad_kind) {
    auto from_kind = [](int k) {
        switch (k) {
            case 2: return PromptStyle::PlayStation;
            case 3: return PromptStyle::Switch;
            default: return PromptStyle::Xbox;     // Xbox pads and every pad that is neither a PlayStation nor a Switch pad
        }
    };
    switch (last) {
        case LastDevice::KeyboardMouse: return PromptStyle::Keyboard;
        case LastDevice::Pad: return from_kind(last_pad_kind);
        default: return connected_pad_kind >= 0 ? from_kind(connected_pad_kind) : PromptStyle::Keyboard;
    }
}

}  // namespace nxrt::play
