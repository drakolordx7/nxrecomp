// SPDX-License-Identifier: GPL-3.0-or-later
// Mouse pointer in menus, the game side (S28). See menu_mouse_game.h (the hooks) and src/gfx/play/menu_mouse.h (pointer and math) for the design and docs/S28.md for what was found in the game.
#include "menu_mouse_game.h"

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <algorithm>
#include <cstring>
#include <cmath>
#include <string>
#include <vector>

#include "nxrt/abi.h"
#include "nxrt/dispatch.h"
#include "nxrt/hooks.h"
#include "nxrt/log.h"
#include "play_aim.h"

namespace nxrt::play::mm {

namespace {

constexpr const char* kTag = "menumouse";

// Offsets in `main` (docs/mpr/hooks.md; the names are from the game's own symbols).
constexpr uint64_t kFnWidgetProcessInput = 0x88cb74;       // CUIWidgetGOC::ProcessInput(CStateManager&, float): x0 widget, x1 state manager; returns flags in w0
constexpr uint64_t kFnGetTouchPosition = 0x35322c;         // CFinalInput::GetTouchScreenPosition(CTransform4f const*): the UI position in s0, s1
constexpr uint64_t kFnCheckTouchScreen = 0x88d7d4;         // CUIWidgetGOC::CheckTouchScreen(CStateManager&, CVector2f const&): the button's box test
constexpr uint64_t kFnTouchInput = 0x88d1b8;               // CUIWidgetGOC::ProcessWidgetTouchScreenInput(CStateManager&, CFinalInput const&)
constexpr uint64_t kFnComponentById = 0xb082fc;            // CStateManagerObject::ComponentById(id): this = state manager + 0x30
constexpr uint64_t kFnWidgetCastToPtr = 0x88ae38;          // CUIWidgetGOC::CastToPtr(CGameObjectComponent*)
constexpr uint64_t kFnMenuBeforeThink = 0x888608;          // CUIMenuGOC::BeforeThink(CStateManager&, float): x0 menu, x1 state manager; runs the widgets' ProcessInput
constexpr uint64_t kMenuFocus = 213, kMenuCount = 192, kMenuIds = 200;   // CUIMenuGOC: has the focus; widget count and array of component ids
constexpr uint64_t kFnTextPaneCastToPtr = 0x85dfe4;        // CTextPaneGOC::CastToPtr(CGameObjectComponent*)
constexpr uint64_t kFnTextPaneBounds = 0x861b98;           // CTextPaneGOC::GetBounds(CAABox&): the pane's box in UI units
constexpr uint64_t kFnRenderCastToPtr = 0x80a030;          // CRenderGOC::CastToPtr(CGameObjectComponent*)
constexpr uint64_t kFnRenderTextureBounds = 0xa330f0;      // CRenderTexture::GetBounds(CAABox&)
constexpr uint64_t kBtnPaneCount = 128, kBtnPanes = 136;   // CUIButtonGOC: the linked text panes / render components (ids)
constexpr uint64_t kFnSliderCastToPtr = 0x889d64;          // CUISliderGOC::CastToPtr(CGameObjectComponent*)
constexpr uint64_t kFnSliderSetTick = 0x88a8bc;            // CUISliderGOC::SetTickCount(CStateManager&, float tick, bool send_value): x0 slider, x1 state manager, s0 tick
// A slider row of the front end's option pages (HUD Opacity, SFX, Music): the knob travels from kSliderX0 (0) to kSliderX1 (max) in UI units. The slider
// widget has no button and so no box; its component is the one its "button" field names.
constexpr float kSliderX0 = -3.02f, kSliderX1 = 1.28f, kSliderGrab = 0.35f;
constexpr uint64_t kFnButtonCastToPtr = 0x883b64;          // CUIButtonGOC::CastToPtr(CGameObjectComponent*)
constexpr uint64_t kWButton = 0x90;                        // id of the widget's CUIButtonGOC
constexpr uint64_t kBtnBox = 176;                          // the button's CAABox2f: min x, min y, max x, max y
constexpr uint64_t kStateManagerFinalInput = 0x4e0320;     // CFinalInput inside CStateManager (the one ProcessInput hands to the widgets)
constexpr float kUiHalfWidth = 8.0f, kUiHalfHeight = 4.5f;   // the UI camera's extent in UI units

// The older GUI frames (pause screen, options, inventory, log book, map, message boxes): CGuiFrame holds a flat list of widgets
constexpr uint64_t kFnGuiFrameInput = 0xbd45e0;            // CGuiFrame::ProcessControllerInput(NGuiUtils::SWidgetInput const&): x0 frame, x1 input
constexpr uint64_t kFnGuiWorldPos = 0xbd6b40;              // CGuiObject::GetWorldPosition(): CVector3f in s0..s2
constexpr uint64_t kFnGuiToScreen = 0xbd53bc;              // CGuiCamera::ConvertToScreenSpace(CVector3f const&): CVector3f in s0..s2
constexpr uint64_t kFnGuiWorker = 0xbe2b1c;                // CGuiCompoundWidget::GetWorkerWidget(int)
constexpr uint64_t kFnTableSelectRow = 0xbe25e8;           // CGuiTableGroup::MAF_SelectRowAtIndex(int): selects the row and tells the screen
constexpr uint64_t kTableRows = 272, kTableCurrent = 280;
constexpr uint64_t kGuiTextPaneDims = 396;                 // CGuiPane: width, height in the frame's units
constexpr uint64_t kFnPauseManagerInput = 0xe65bec;        // CPauseScreenManagerMP1::ProcessControllerInput(CStateManager const&, CFinalInput const&): x0 the manager
// The pause screen manager keeps the text panes of its tab and button labels (FRME_PauseScreenInstructions); a click on one is that button.
// The pane holds the button's icon; the word beside or under it (OPTIONS, LOG BOOK, BACK, EXIT, SELECT) is part of the same label, so the click zone reaches over it (ndc units).
struct PauseLabel { uint64_t off; Act act; float left, right, below, above; };
const PauseLabel kPauseLabels[] = {{104, Act::BtnL, 0.05f, 0.42f, 0.05f, 0.05f}, {112, Act::BtnR, 0.42f, 0.05f, 0.05f, 0.05f}, {136, Act::BtnMinus, 0.07f, 0.07f, 0.06f, 0.05f},
                                   {144, Act::Confirm, 0.07f, 0.07f, 0.06f, 0.05f}, {152, Act::Back, 0.07f, 0.07f, 0.06f, 0.05f}};
constexpr uint64_t kFnGuiIsActive = 0xbd681c, kFnGuiIsVisible = 0xbd6810, kFnGuiIsEnabled = 0xbd686c;
constexpr uint64_t kFrameWidgetCount = 56, kFrameWidgets = 64, kFrameCamera = 48;
constexpr uint64_t kGuiVtable[][2] = {
    {0x20daa68, 0}, {0x20dae40, 1}, {0x20db108, 2}, {0x20db4b8, 3}, {0x20db548, 4}, {0x20db5e0, 5}, {0x20db678, 6}, {0x20db838, 7}, {0x20db8d0, 8}, {0x20db960, 9}, {0x20db428, 10}};
const char* const kGuiTypeName[] = {"Widget", "TextPane", "Pane", "Head", "TableGroup", "Compound", "SliderGroup", "Group", "Light", "Model", "MapWindow"};

// CUIWidgetGOC fields
constexpr uint64_t kWState = 128;           // EUIWidgetState: 0 hidden, 1 idle, 2 pressed, 3 released, 4 selected, 5 idle but selectable
constexpr uint64_t kWKind = 140;
constexpr uint64_t kWFlags = 40;            // activation bits; the widget works when (~flags & 0x78) == 0
constexpr uint64_t kWProps = 56;
constexpr uint64_t kWId = 24;
constexpr uint64_t kWDeferred = 336;        // id of the widget the menu selects after this frame's input (ProcessDeferredSelection)
constexpr uint64_t kPropTouchMode = 40;       // what a touch does: 0 act at once, 1 select only, 2 select and act (CUIWidgetGOC::ProcessWidgetTouchScreenInput)
constexpr uint64_t kPropKindOk = 77;

struct State {
    uint64_t base = 0x7100000000ull;
    uint32_t hook_widget = 0, hook_pos = 0;
    Snapshot snap;
    std::atomic<uint64_t> hovers{0}, clicks{0}, widget_calls{0};
    bool dump = false;                        // NXRT_MENUMOUSE_DUMP: log every GUI frame's widgets once
    uint32_t hook_frame = 0, hook_menu = 0, hook_pause = 0;
    uint64_t last_menu = 0;                   // the menu that had the focus at the last pointer pass: a new one is looked at once even if the pointer did not move
    bool hover_now = false;                   // this tick selects what is under the pointer (the pointer moved, or the menu changed under it)
    uint64_t drag_slider = 0;                 // the slider group the left button is dragging
    bool over_value = false;                  // the pointer is over a slider: the wheel steps its value
    uint64_t pause_mgr = 0;                   // the pause screen manager seen last (its label panes)
    Act pending_act = Act::None;              // a button the pointer pressed (a pause screen label)
    uint64_t drag_goc = 0;                    // the front end's slider component the left button is dragging (0: none)
    float drag_max = 0;                       // its tick count
    uint32_t rehover = 0;                     // frames in which the pointer is treated as moving, after it switched lists
    uint32_t focus_cooldown = 0;              // frames before the pointer may ask again to move between two lists (A / B)
    uint64_t chosen = 0;                      // the widget a click goes to
    uint64_t hover_w = 0;                     // the widget a hover selects (an arrow button's row label)
    bool over_value_goc = false;              // the pointer is over an option's arrow buttons: the wheel steps the value                      // the widget the pointer means this tick (set by the menu hook)
    uint32_t capable = 0;                     // widgets with a usable box seen while a click was pending: a screen without any takes the click as a plain confirm
    int log = 0;                             // NXRT_MENUMOUSE_LOG: 1 = the widgets the pointer hits, 2 = every widget it looks at
    bool force_pos = false;                  // GetTouchScreenPosition answers `pos` while the game's touch routine runs
    float pos[2] = {0, 0};
};

State& S() {
    static State s;
    return s;
}

thread_local bool t_in_widget = false;

// Calls a guest function from inside a hook: the registers are saved and restored, x0 comes back.
uint64_t call_guest(::Ctx* c, uint64_t fn, uint64_t a0, uint64_t a1 = 0, uint64_t a2 = 0) {
    ::Ctx saved = *c;
    c->x[0] = a0;
    c->x[1] = a1;
    c->x[2] = a2;
    dispatch_call(c, S().base + fn);
    const uint64_t r = c->x[0];
    *c = saved;
    return r;
}

// The pointer in UI units. The front end's UI camera is orthographic and authored for 16:9: x runs -8..8 and y -4.5..4.5 (up +); the button boxes of
// CUIButtonGOC are in the same units (the main menu's PLAY box is (-6.72, -0.98)-(-5.93, -0.68)). The game's own touch transform
// (CGameInstanceState + 0x70) is the identity on the PC, so the normalised position is scaled by hand.
void ui_point(const Snapshot& snap, float out[2]) {
    out[0] = snap.nx * kUiHalfWidth;
    out[1] = snap.ny * kUiHalfHeight;
}

// CUISliderGOC::SetTickCount with a float argument: the same call the pad's left and right make through OnAction_NextTick / PrevTick.
void slider_set_tick(::Ctx* c, uint64_t goc, uint64_t sm, float tick) {
    ::Ctx saved = *c;
    c->x[0] = goc;
    c->x[1] = sm;
    c->x[2] = 1;
    c->v[0].f32[0] = tick;
    dispatch_call(c, S().base + kFnSliderSetTick);
    *c = saved;
}

// Debug (NXRT_MENUMOUSE_LOG=3): the pieces a button's box is made of, asked directly.
void log_button_parts(::Ctx* c, uint64_t btn, uint64_t sm) {
    uint32_t n = 0;
    uint64_t ids = 0;
    play::guest_read(btn + kBtnPaneCount, &n, 4);
    play::guest_read(btn + kBtnPanes, &ids, 8);
    for (uint32_t i = 0; i < n && i < 8; ++i) {
        uint32_t cid = 0;
        if (!play::guest_read(ids + 4ull * i, &cid, 4)) continue;
        const uint64_t comp = call_guest(c, kFnComponentById, sm + 0x30, cid);
        if (!comp) { NXLOG_I(kTag, "      part %08x: no component", cid); continue; }
        uint8_t fl = 0;
        play::guest_read(comp + 40, &fl, 1);
        const uint64_t tp = call_guest(c, kFnTextPaneCastToPtr, comp);
        float b[6] = {0, 0, 0, 0, 0, 0};
        if (tp) {
            alignas(16) float out[6] = {};
            call_guest(c, kFnTextPaneBounds, tp, reinterpret_cast<uint64_t>(out));
            std::memcpy(b, out, sizeof b);
            NXLOG_I(kTag, "      part %08x: text pane flags=%02x bounds=(%.3f %.3f %.3f)-(%.3f %.3f %.3f)", cid, fl, b[0], b[1], b[2], b[3], b[4], b[5]);
        } else {
            NXLOG_I(kTag, "      part %08x: other component flags=%02x render=%llx", cid, fl, static_cast<unsigned long long>(call_guest(c, kFnRenderCastToPtr, comp)));
        }
    }
}

// The box of a button, worked out again. The game builds it (CUIButtonGOC::UpdateBoundingShape) from the bounds of the text panes and render components
// the button links, but takes the bounds of the inactive ones too, which come back as zeros: on the PC the main menu's boxes run to (0, 0) or into the
// next row. Here only the active parts count. A box that is just the size of the text gets the room of the bar it sits on (at least kMinBoxW x kMinBoxH,
// left aligned at the text), so a row is as easy to hit as it looks.
constexpr float kMinBoxW = 3.3f, kMinBoxH = 0.75f, kBoxPadX = 0.25f;

bool part_bounds(::Ctx* c, uint64_t comp, float b[4]) {
    uint8_t fl = 0;
    if (!play::guest_read(comp + 40, &fl, 1) || (~fl & 0x78) != 0) return false;           // an inactive part has no box
    alignas(16) float out[6] = {};
    if (const uint64_t tp = call_guest(c, kFnTextPaneCastToPtr, comp)) {
        call_guest(c, kFnTextPaneBounds, tp, reinterpret_cast<uint64_t>(out));
    } else if (const uint64_t rg = call_guest(c, kFnRenderCastToPtr, comp)) {
        uint64_t p = 0;
        if (!play::guest_read(rg + 200, &p, 8) || !p) return false;
        uint64_t vt = 0, fn = 0;
        if (!play::guest_read(p, &vt, 8) || !play::guest_read(vt + 224, &fn, 8) || !fn) return false;
        ::Ctx saved = *c;                                      // CRenderTexture* = p->vtable[28]()
        c->x[0] = p;
        dispatch_call(c, fn);
        const uint64_t rt = c->x[0];
        *c = saved;
        if (!rt) return false;
        call_guest(c, kFnRenderTextureBounds, rt, reinterpret_cast<uint64_t>(out));
    } else {
        return false;
    }
    b[0] = out[0]; b[1] = out[1]; b[2] = out[3]; b[3] = out[4];
    return b[2] > b[0] && b[3] > b[1];
}

bool button_box(::Ctx* c, uint64_t btn, uint64_t sm, float box[4]) {
    uint32_t n = 0;
    uint64_t ids = 0;
    if (!play::guest_read(btn + kBtnPaneCount, &n, 4) || !play::guest_read(btn + kBtnPanes, &ids, 8) || !ids) return false;
    bool any = false;
    for (uint32_t i = 0; i < n && i < 16; ++i) {
        uint32_t cid = 0;
        if (!play::guest_read(ids + 4ull * i, &cid, 4)) continue;
        const uint64_t comp = call_guest(c, kFnComponentById, sm + 0x30, cid);
        float b[4];
        if (!comp || !part_bounds(c, comp, b)) continue;
        if (!any) { std::memcpy(box, b, sizeof b); any = true; }
        else { box[0] = std::min(box[0], b[0]); box[1] = std::min(box[1], b[1]); box[2] = std::max(box[2], b[2]); box[3] = std::max(box[3], b[3]); }
    }
    if (!any) return false;
    if (box[2] - box[0] < kMinBoxW) box[2] = std::max(box[2] + kBoxPadX, box[0] - kBoxPadX + kMinBoxW);
    box[0] -= kBoxPadX;
    if (box[3] - box[1] < kMinBoxH) {
        const float mid = 0.5f * (box[1] + box[3]);
        box[1] = mid - 0.5f * kMinBoxH;
        box[3] = mid + 0.5f * kMinBoxH;
    }
    return true;
}

// What the pointer may do with a widget: the same guards as CUIWidgetGOC::ShouldProcessInput; a widget that is being pressed (2) or has just been (3) is left alone.
struct WidgetInfo { uint32_t state = 0, kind = 0, id = 0, touch_mode = 0, button = 0; uint64_t props = 0; bool ok = false; };

WidgetInfo widget_info(uint64_t w) {
    WidgetInfo r;
    uint8_t flags = 0, kind_ok = 1;
    if (!play::guest_read(w + kWState, &r.state, 4) || !play::guest_read(w + kWKind, &r.kind, 4) || !play::guest_read(w + kWFlags, &flags, 1) ||
        !play::guest_read(w + kWProps, &r.props, 8) || !r.props || !play::guest_read(w + kWId, &r.id, 4))
        return r;
    play::guest_read(r.props + kPropKindOk, &kind_ok, 1);
    play::guest_read(r.props + kPropTouchMode, &r.touch_mode, 4);
    play::guest_read(w + kWButton, &r.button, 4);
    r.ok = r.state != 0 && r.state != 2 && r.state != 3 && !(r.kind == 1 && kind_ok == 0) && ((~flags) & 0x78) == 0;
    return r;
}

// The menu runs its widgets' input one by one; widgets of a menu overlap (a button's box is the union of its text panes), so the pointer first picks the
// one widget it means (the smallest box under it) and the per-widget hook acts on that one only.
int menu_hook(::Ctx* c, uint64_t, void*) {
    State& s = S();
    s.chosen = 0;
    s.hover_w = 0;
    if (!s.snap.active || !s.snap.inside) return 0;
    const uint64_t menu = c->x[0], sm = c->x[1];
    uint8_t focus = 0;
    int32_t n = 0;
    uint64_t ids = 0;
    const bool got = play::guest_read(menu + kMenuFocus, &focus, 1) && play::guest_read(menu + kMenuCount, &n, 4) && play::guest_read(menu + kMenuIds, &ids, 8);
    if (!got || !focus || n <= 0 || !ids) return 0;
    const bool changed = menu != s.last_menu;
    s.hover_now = s.snap.hover || (changed && s.snap.shown);
    if (changed) s.over_value_goc = false;
    s.last_menu = menu;
    if (!s.snap.held) s.drag_goc = 0;
    if (!(s.hover_now || s.snap.click || s.drag_goc)) return 0;
    if (s.log >= 2) NXLOG_I(kTag, "menu %llx focus=%u widgets=%d ids=%llx", static_cast<unsigned long long>(menu), focus, n, static_cast<unsigned long long>(ids));
    float pt[2];
    ui_point(s.snap, pt);
    struct Cand { uint64_t w; uint32_t mode, state; float box[4]; bool hit; float dist; uint64_t btn; };
    std::vector<Cand> cands;
    struct Slider { uint64_t goc; float ticks; };
    std::vector<Slider> sliders;                                   // in the order of the menu's widgets: top row first
    for (int32_t i = 0; i < n && i < 64; ++i) {
        uint32_t cid = 0;
        if (!play::guest_read(ids + 4ull * i, &cid, 4)) continue;
        const uint64_t comp = call_guest(c, kFnComponentById, sm + 0x30, cid);
        const uint64_t w = comp ? call_guest(c, kFnWidgetCastToPtr, comp) : 0;
        if (s.log >= 4 && comp) {
            const uint64_t sg = call_guest(c, kFnSliderCastToPtr, comp);
            if (sg) {
                float tk[3] = {0, 0, 0};
                uint64_t pr = 0;
                play::guest_read(sg + 0x38, &pr, 8);
                if (pr) play::guest_read(pr + 0xc, &tk[0], 4);
                play::guest_read(sg + 0x44, &tk[1], 4);
                NXLOG_I(kTag, "  menu id %08x is a slider component %llx: max ticks %.1f, current %.1f", cid, static_cast<unsigned long long>(sg), tk[0], tk[1]);
            }
        }
        if (!w) continue;
        const WidgetInfo wi = widget_info(w);
        if (!wi.ok) continue;
        Cand k{w, wi.touch_mode, wi.state, {0, 0, 0, 0}, false, 0, 0};
        uint64_t btn = 0;
        {
            const uint64_t bc = wi.button ? call_guest(c, kFnComponentById, sm + 0x30, wi.button) : 0;
            btn = bc ? call_guest(c, kFnButtonCastToPtr, bc) : 0;
            if (!btn && bc) {                                      // a slider's widget names its slider component where a button's names the button
                const uint64_t sg = call_guest(c, kFnSliderCastToPtr, bc);
                uint64_t pr = 0;
                float ticks = 0;
                if (sg && play::guest_read(sg + 0x38, &pr, 8) && pr && play::guest_read(pr + 0xc, &ticks, 4) && ticks >= 1.f) sliders.push_back({sg, ticks});
            }
            if (btn) {
                uint32_t shape = 0;
                uint64_t bprops = 0;
                if (play::guest_read(btn + 56, &bprops, 8) && bprops) play::guest_read(bprops + 40, &shape, 4);
                if (shape == 1 && button_box(c, btn, sm, k.box)) play::guest_write(btn + kBtnBox, k.box, sizeof k.box);   // the game's own touch routine then tests this box
                else play::guest_read(btn + kBtnBox, k.box, sizeof k.box);
            }
        }
        if (s.log >= 3 && btn) log_button_parts(c, btn, sm);
        if (s.log >= 4) {   // debug: which of the widget's fields name a slider component?
            for (uint64_t off = 0; off < 0x200; off += 4) {
                uint32_t v = 0;
                if (!play::guest_read(w + off, &v, 4) || v < 0x10000 || v > 0x1ffff) continue;
                const uint64_t cc = call_guest(c, kFnComponentById, sm + 0x30, v);
                const uint64_t sg = cc ? call_guest(c, kFnSliderCastToPtr, cc) : 0;
                if (sg) NXLOG_I(kTag, "    widget %08x: field +%llu = %08x is a slider component %llx", wi.id, static_cast<unsigned long long>(off), v, static_cast<unsigned long long>(sg));
            }
        }
        k.btn = btn;
        const bool valid = btn && k.box[2] > k.box[0] && k.box[3] > k.box[1];
        k.hit = valid && pt[0] >= k.box[0] && pt[0] <= k.box[2] && pt[1] >= k.box[1] && pt[1] <= k.box[3];
        const float cx = 0.5f * (k.box[0] + k.box[2]), cy = 0.5f * (k.box[1] + k.box[3]);
        k.dist = (pt[0] - cx) * (pt[0] - cx) * 0.01f + (pt[1] - cy) * (pt[1] - cy);   // rows: the vertical distance decides
        if (valid) { s.capable++; }
        if (s.log >= 2 || (s.log && k.hit))
            NXLOG_I(kTag, "  menu %llx widget %08x state=%u mode=%u box=(%.3f %.3f)-(%.3f %.3f) hit=%d", static_cast<unsigned long long>(menu), wi.id, wi.state, wi.touch_mode, k.box[0], k.box[1],
                    k.box[2], k.box[3], k.hit ? 1 : 0);
        if (valid) cands.push_back(k);
    }
    // the two arrow buttons of an option row leave the value text between them: split the gap at its middle so the whole value area belongs to one of them
    for (Cand& l : cands) {
        if (l.mode != 0) continue;
        for (Cand& r : cands) {
            if (&l == &r || r.mode != 0 || r.box[0] <= l.box[2] || std::fabs(0.5f * (l.box[1] + l.box[3]) - 0.5f * (r.box[1] + r.box[3])) > 0.05f) continue;
            const float mid = 0.5f * (l.box[2] + r.box[0]);
            l.box[2] = r.box[0] = mid;
            play::guest_write(l.btn + kBtnBox, l.box, sizeof l.box);          // the game's touch routine tests the button's own box
            play::guest_write(r.btn + kBtnBox, r.box, sizeof r.box);
            break;
        }
    }
    for (Cand& k : cands) {
        k.hit = pt[0] >= k.box[0] && pt[0] <= k.box[2] && pt[1] >= k.box[1] && pt[1] <= k.box[3];
        const float cx = 0.5f * (k.box[0] + k.box[2]), cy = 0.5f * (k.box[1] + k.box[3]);
        k.dist = (pt[0] - cx) * (pt[0] - cx) * 0.01f + (pt[1] - cy) * (pt[1] - cy);
    }
    const Cand* best = nullptr;
    for (const Cand& k : cands)
        if (k.hit && (!best || k.dist < best->dist)) best = &k;
    // Slider rows: the label rows that have no pair of arrow buttons beside them, top to bottom, are the rows of the sliders found above, in order.
    // The track is a picture with no box: a pointer over the knob's travel on that row selects the row, a click or a drag sets the value.
    if (!sliders.empty()) {
        std::vector<const Cand*> rows;
        for (const Cand& k : cands) {
            if (k.mode != 1 && k.mode != 2) continue;
            const float cy = 0.5f * (k.box[1] + k.box[3]);
            bool arrows = false;
            for (const Cand& a : cands)
                if (a.mode == 0 && std::fabs(0.5f * (a.box[1] + a.box[3]) - cy) < 0.1f) arrows = true;
            if (!arrows) rows.push_back(&k);
        }
        std::sort(rows.begin(), rows.end(), [](const Cand* a, const Cand* b) { return a->box[1] > b->box[1]; });
        const Slider* dragging = nullptr;
        for (const Slider& sl : sliders)
            if (sl.goc == s.drag_goc) dragging = &sl;
        for (size_t i = 0; i < rows.size() && i < sliders.size(); ++i) {
            const Cand& row = *rows[i];
            const Slider& sl = sliders[i];
            const bool over = pt[0] >= kSliderX0 - kSliderGrab && pt[0] <= kSliderX1 + kSliderGrab && pt[1] >= row.box[1] && pt[1] <= row.box[3];
            if (!over && dragging != &sl) continue;
            uint8_t fl = 0;
            if (!play::guest_read(sl.goc + 0x28, &fl, 1) || (~fl & 0x78) != 0) continue;     // the slider is not active
            s.chosen = 0;
            s.hover_w = row.w;
            s.over_value_goc = true;
            if (s.snap.click && over) { s.drag_goc = sl.goc; pointer().click_used(); s.clicks++; dragging = &sl; }
            if (dragging == &sl && (s.snap.click || s.snap.held)) {
                const float f = std::min(1.f, std::max(0.f, (pt[0] - kSliderX0) / (kSliderX1 - kSliderX0)));
                const float tick = std::floor(f * sl.ticks + 0.5f);
                float cur = -1;
                play::guest_read(sl.goc + 0x44, &cur, 4);
                if (tick != cur) slider_set_tick(c, sl.goc, sm, tick);
                if (s.log) NXLOG_I(kTag, "slider row %llx: pointer %.3f -> tick %.0f of %.0f (was %.0f)", static_cast<unsigned long long>(row.w), pt[0], tick, sl.ticks, cur);
            }
            return 0;
        }
    }
    s.chosen = best ? best->w : 0;
    s.hover_w = 0;
    s.over_value_goc = false;
    if (best) {
        if (best->mode == 1 || best->mode == 2) {
            s.hover_w = best->w;
        } else if (best->mode == 0) {
            // an arrow button of an option row (act at once): hovering it selects the row, that is the label widget on the same line
            s.over_value_goc = true;
            const float cy = 0.5f * (best->box[1] + best->box[3]);
            float best_dy = 0.2f;
            for (const Cand& k : cands) {
                if ((k.mode != 1 && k.mode != 2) || &k == best) continue;
                const float dy = std::fabs(0.5f * (k.box[1] + k.box[3]) - cy);
                if (dy < best_dy) { best_dy = dy; s.hover_w = k.w; }
            }
        }
    }
    if (s.log && best) NXLOG_I(kTag, "pointer (%.3f, %.3f) hover=%d click=%d -> widget at %llx (mode %u), selects %llx", pt[0], pt[1], s.hover_now ? 1 : 0, s.snap.click ? 1 : 0, static_cast<unsigned long long>(best->w), best->mode,
                               static_cast<unsigned long long>(s.hover_w));
    return 0;
}

uint64_t pointer_widget(::Ctx* c, uint64_t w, uint64_t sm) {
    State& s = S();
    const WidgetInfo wi = widget_info(w);
    if (!wi.ok) return 0;
    float pt[2];
    ui_point(s.snap, pt);
    if (s.snap.click && w == s.chosen) {
        // the game's own touch routine: selects (and, depending on the widget, activates) at the position the position hook answers
        s.pos[0] = pt[0];
        s.pos[1] = pt[1];
        s.force_pos = true;
        // A touch on a select-only widget (mode 1) first selects it and acts at the next touch. A mouse click acts at once, as on any PC menu: run the
        // game's routine with the select-and-act behaviour (mode 2) for this call.
        const bool lift = wi.touch_mode == 1;
        if (lift) { const uint32_t two = 2; play::guest_write(wi.props + kPropTouchMode, &two, 4); }
        const uint64_t r = call_guest(c, kFnTouchInput, w, sm, sm + kStateManagerFinalInput) & 0xff;
        if (lift) play::guest_write(wi.props + kPropTouchMode, &wi.touch_mode, 4);
        s.force_pos = false;
        if (s.log) {
            uint32_t ty = 0, st2 = 0;
            play::guest_read(wi.props + 36, &ty, 4);
            play::guest_read(w + kWState, &st2, 4);
            NXLOG_I(kTag, "click on %08x: props+36=%u touch mode=%u state %u -> %u, game routine returned %llu", wi.id, ty, wi.touch_mode, wi.state, st2, static_cast<unsigned long long>(r));
        }
        if (r) {
            pointer().click_used();
            s.clicks++;
            return r;
        }
    }
    if (s.hover_now && w == s.hover_w && wi.state != 4 && (wi.touch_mode == 1 || wi.touch_mode == 2)) {
        // what a touch on a select-only widget does: this widget is the next selection; the menu applies it after the widgets have run
        play::guest_write(w + kWDeferred, &wi.id, 4);
        s.hovers++;
        return 4;
    }
    return 0;
}

// A guest function that returns a CVector3f (s0, s1, s2).
void call_guest_vec3(::Ctx* c, uint64_t fn, uint64_t a0, uint64_t a1, float out[3]) {
    ::Ctx saved = *c;
    c->x[0] = a0;
    c->x[1] = a1;
    dispatch_call(c, S().base + fn);
    out[0] = c->v[0].f32[0];
    out[1] = c->v[1].f32[0];
    out[2] = c->v[2].f32[0];
    *c = saved;
}

const char* gui_type(uint64_t vptr) {
    State& s = S();
    for (const auto& e : kGuiVtable)
        if (vptr == s.base + e[0] + 16) return kGuiTypeName[e[1]];
    return "?";
}

void dump_widget(::Ctx* c, uint64_t w, uint64_t cam, int depth, const char* tag);

// A widget's children: first child at +112, the next sibling at +120.
void dump_children(::Ctx* c, uint64_t w, uint64_t cam, int depth) {
    uint64_t ch = 0;
    play::guest_read(w + 112, &ch, 8);
    for (int k = 0; ch && k < 32; ++k) {
        dump_widget(c, ch, cam, depth, "child");
        uint64_t nx = 0;
        if (!play::guest_read(ch + 120, &nx, 8)) break;
        ch = nx;
    }
}

void dump_widget(::Ctx* c, uint64_t w, uint64_t cam, int depth, const char* tag) {
    uint64_t vp = 0;
    if (!play::guest_read(w, &vp, 8)) return;
    uint32_t id = 0, wi = 0;
    float dims[2] = {0, 0}, pos[3] = {0, 0, 0}, ndc[3] = {0, 0, 0};
    play::guest_read(w + 132, &id, 4);
    play::guest_read(w + 140, &wi, 4);
    play::guest_read(w + 396, dims, 8);
    call_guest_vec3(c, kFnGuiWorldPos, w, 0, pos);
    if (cam) call_guest_vec3(c, kFnGuiToScreen, cam, reinterpret_cast<uint64_t>(pos), ndc);
    const unsigned vis = call_guest(c, kFnGuiIsVisible, w) & 1, act = call_guest(c, kFnGuiIsActive, w) & 1;
    NXLOG_I(kTag, "    %*s%s %llx %s id=%u wi=%u pos=(%.2f %.2f) dims=(%.2f %.2f) ndc=(%.3f %.3f) act=%u vis=%u", depth * 2, "", tag, static_cast<unsigned long long>(w), gui_type(vp), id, wi, pos[0], pos[1],
            dims[0], dims[1], ndc[0], ndc[1], act, vis);
    if (depth < 3) dump_children(c, w, cam, depth + 1);
}

// Debug dump of one GUI frame: every widget with its type, id, position, size, visibility and where the camera puts it on the screen (-1..1).
void dump_frame(::Ctx* c, uint64_t frame) {
    uint32_t n = 0;
    uint64_t list = 0, cam = 0;
    play::guest_read(frame + kFrameWidgetCount, &n, 4);
    play::guest_read(frame + kFrameWidgets, &list, 8);
    play::guest_read(frame + kFrameCamera, &cam, 8);
    NXLOG_I(kTag, "gui frame %llx: %u widgets, camera %llx", static_cast<unsigned long long>(frame), n, static_cast<unsigned long long>(cam));
    for (uint32_t i = 0; i < n && i < 400; ++i) {
        uint64_t w = 0, vp = 0;
        if (!play::guest_read(list + 8ull * i, &w, 8) || !w || !play::guest_read(w, &vp, 8)) continue;
        uint32_t id = 0, wi = 0;
        float dims[2] = {0, 0}, pos[3] = {0, 0, 0}, ndc[3] = {0, 0, 0};
        play::guest_read(w + 132, &id, 4);
        play::guest_read(w + 140, &wi, 4);
        play::guest_read(w + 396, dims, 8);
        call_guest_vec3(c, kFnGuiWorldPos, w, 0, pos);
        if (cam) call_guest_vec3(c, kFnGuiToScreen, cam, reinterpret_cast<uint64_t>(pos), ndc);
        const unsigned act = call_guest(c, kFnGuiIsActive, w) & 1, vis = call_guest(c, kFnGuiIsVisible, w) & 1, ena = call_guest(c, kFnGuiIsEnabled, w) & 1;
        char name[48] = {};
        uint64_t np = 0;
        if (play::guest_read(w + 144, &np, 8) && np) play::guest_read(np, name, sizeof name - 1);
        for (char& ch : name) if (ch && (ch < 32 || ch > 126)) ch = '.';
        NXLOG_I(kTag, "  [%u] %llx %s id=%u wi=%u pos=(%.2f %.2f %.2f) dims=(%.2f %.2f) ndc=(%.3f %.3f) act=%u vis=%u ena=%u '%s'", i, static_cast<unsigned long long>(w), gui_type(vp), id, wi,
                pos[0], pos[1], pos[2], dims[0], dims[1], ndc[0], ndc[1], act, vis, ena, name);
        if (std::strcmp(gui_type(vp), "SliderGroup") == 0) {
            float sv[8] = {};
            play::guest_read(w + 264, sv, sizeof sv);
            uint64_t ends[2] = {};
            play::guest_read(w + 296, ends, 16);
            NXLOG_I(kTag, "    slider min=%.2f max=%.2f cur=%.2f incr=%.2f target=%.2f knob=%llx end=%llx", sv[0], sv[1], sv[2], sv[3], sv[4], static_cast<unsigned long long>(ends[0]), static_cast<unsigned long long>(ends[1]));
            dump_children(c, w, cam, 1);
            if (ends[0]) dump_widget(c, ends[0], cam, 1, "knob");
            if (ends[1]) dump_widget(c, ends[1], cam, 1, "end");
        }
        if (std::strcmp(gui_type(vp), "TableGroup") == 0) {
            int32_t rows = 0, cur = 0;
            play::guest_read(w + 272, &rows, 4);
            play::guest_read(w + 280, &cur, 4);
            NXLOG_I(kTag, "    table rows=%d current=%d", rows, cur);
            for (int32_t r = 0; r < rows && r < 12; ++r) {
                const uint64_t row = call_guest(c, kFnGuiWorker, w, static_cast<uint64_t>(r));
                if (row) dump_widget(c, row, cam, 1, "row");
            }
        }
    }
}

struct NdcRect { float x0 = 1e9f, y0 = 1e9f, x1 = -1e9f, y1 = -1e9f; bool valid() const { return x1 >= x0 && y1 >= y0; } };

void ndc_add(NdcRect* r, const float p[2]) {
    r->x0 = std::min(r->x0, p[0]); r->x1 = std::max(r->x1, p[0]);
    r->y0 = std::min(r->y0, p[1]); r->y1 = std::max(r->y1, p[1]);
}

// The screen rectangle of the text panes under a widget: each pane's four corners (position plus or minus half its size) through the frame's camera.
void gui_pane_rect(::Ctx* c, uint64_t w, uint64_t cam, NdcRect* out, int depth) {
    uint64_t vp = 0;
    if (!play::guest_read(w, &vp, 8)) return;
    if (std::strcmp(gui_type(vp), "TextPane") == 0 && (call_guest(c, kFnGuiIsVisible, w) & 1)) {
        float pos[3], dims[2] = {0, 0};
        call_guest_vec3(c, kFnGuiWorldPos, w, 0, pos);
        play::guest_read(w + kGuiTextPaneDims, dims, 8);
        if (dims[0] > 0 && dims[1] > 0) {
            for (int k = 0; k < 4; ++k) {
                float corner[3] = {pos[0] + ((k & 1) ? 0.5f : -0.5f) * dims[0], pos[1] + ((k & 2) ? 0.5f : -0.5f) * dims[1], pos[2]};
                float n[3];
                call_guest_vec3(c, kFnGuiToScreen, cam, reinterpret_cast<uint64_t>(corner), n);
                ndc_add(out, n);
            }
        }
    }
    if (depth >= 3) return;
    uint64_t ch = 0;
    play::guest_read(w + 112, &ch, 8);
    for (int k = 0; ch && k < 16; ++k) {
        gui_pane_rect(c, ch, cam, out, depth + 1);
        uint64_t nx = 0;
        if (!play::guest_read(ch + 120, &nx, 8)) break;
        ch = nx;
    }
}

// The row of a table under the pointer, or -1. The rows are laid out along one axis: a row owns the stretch between the midpoints to its neighbours on
// that axis, and a little more than its text across.
int table_row_at(::Ctx* c, uint64_t table, uint64_t cam, float gx, float gy, int32_t rows) {
    std::vector<NdcRect> r(static_cast<size_t>(rows));
    float cxs0 = 1e9f, cxs1 = -1e9f, cys0 = 1e9f, cys1 = -1e9f;
    for (int32_t i = 0; i < rows; ++i) {
        const uint64_t row = call_guest(c, kFnGuiWorker, table, static_cast<uint64_t>(i));
        if (row) gui_pane_rect(c, row, cam, &r[static_cast<size_t>(i)], 0);
        const NdcRect& q = r[static_cast<size_t>(i)];
        if (!q.valid()) continue;
        const float cx = 0.5f * (q.x0 + q.x1), cy = 0.5f * (q.y0 + q.y1);
        cxs0 = std::min(cxs0, cx); cxs1 = std::max(cxs1, cx); cys0 = std::min(cys0, cy); cys1 = std::max(cys1, cy);
    }
    const bool vertical = (cys1 - cys0) >= (cxs1 - cxs0);
    int best = -1;
    float best_d = 1e9f;
    for (int32_t i = 0; i < rows; ++i) {
        const NdcRect& q = r[static_cast<size_t>(i)];
        if (!q.valid()) continue;
        float pitch = 1e9f;                                        // distance to the nearest other row on the main axis
        const float ci = vertical ? 0.5f * (q.y0 + q.y1) : 0.5f * (q.x0 + q.x1);
        for (int32_t j = 0; j < rows; ++j) {
            if (j == i || !r[static_cast<size_t>(j)].valid()) continue;
            const NdcRect& o = r[static_cast<size_t>(j)];
            pitch = std::min(pitch, std::fabs((vertical ? 0.5f * (o.y0 + o.y1) : 0.5f * (o.x0 + o.x1)) - ci));
        }
        const float hw = 0.5f * (q.x1 - q.x0), hh = 0.5f * (q.y1 - q.y0);
        float ex = hw * 1.5f, ey = hh * 1.5f;                       // across the main axis: a little more than the text
        if (pitch < 1e8f) { if (vertical) ey = 0.5f * pitch; else ex = 0.5f * pitch; }
        const float cx = 0.5f * (q.x0 + q.x1), cy = 0.5f * (q.y0 + q.y1);
        const float dx = std::fabs(gx - cx), dy = std::fabs(gy - cy);
        if (dx > ex || dy > ey) continue;
        const float d = vertical ? dy : dx;
        if (d < best_d) { best_d = d; best = i; }
    }
    return best;
}

// The horizontal middle of the rows of a table on the screen (ndc), or false when no row has a text pane to measure.
bool table_center_x(::Ctx* c, uint64_t table, uint64_t cam, int32_t rows, float* out) {
    float sum = 0;
    int n = 0;
    for (int32_t i = 0; i < rows; ++i) {
        const uint64_t row = call_guest(c, kFnGuiWorker, table, static_cast<uint64_t>(i));
        NdcRect q;
        if (row) gui_pane_rect(c, row, cam, &q, 0);
        if (!q.valid()) continue;
        sum += 0.5f * (q.x0 + q.x1);
        ++n;
    }
    if (!n) return false;
    *out = sum / static_cast<float>(n);
    return true;
}

thread_local bool t_in_frame = false;
uint64_t g_dumped_frame = 0, g_last_gui_frame = 0;
uint32_t g_dumped_count = 0;

// The older GUI frames: tables of rows (pause screen lists, the options pages, yes/no boxes). The pointer selects the row under it and a click is the
// accept key (the pad's A), the same two things the pad does.
void gui_pointer(::Ctx* c, uint64_t frame) {
    State& s = S();
    uint32_t n = 0, status = 1;
    uint64_t list = 0, cam = 0, input = c->x[1];
    if (!play::guest_read(input, &status, 4) || status != 0) return;
    play::guest_read(frame + kFrameWidgetCount, &n, 4);
    play::guest_read(frame + kFrameWidgets, &list, 8);
    play::guest_read(frame + kFrameCamera, &cam, 8);
    if (!n || !list || !cam) return;
    const bool changed = frame != g_last_gui_frame;
    g_last_gui_frame = frame;
    bool hover = s.snap.hover || (changed && s.snap.shown);
    if (s.rehover > 0) { --s.rehover; hover = true; }       // a list was just made active for the pointer: select the row under it once it has taken over
    if (!s.snap.in_picture) return;
    // Two lists side by side (Inventory and Log Book: the categories on the left, the items or entries on the right) are tables of which only one
    // is active; the pad moves between them with A (into the right list) and B (back). Tables that are visible and enabled but not active are kept
    // here, so a pointer over one of them can ask for that move.
    struct Idle { uint64_t w; int32_t rows; };
    std::vector<Idle> idle;
    float active_cx = 0;
    bool have_active = false;
    if (s.focus_cooldown > 0) --s.focus_cooldown;
    for (uint32_t i = 0; i < n && i < 400 && (hover || s.snap.click); ++i) {
        uint64_t w = 0, vp = 0;
        if (!play::guest_read(list + 8ull * i, &w, 8) || !w || !play::guest_read(w, &vp, 8)) continue;
        if (std::strcmp(gui_type(vp), "TableGroup") != 0) continue;
        if (!(call_guest(c, kFnGuiIsVisible, w) & 1) || !(call_guest(c, kFnGuiIsEnabled, w) & 1)) continue;
        if (!(call_guest(c, kFnGuiIsActive, w) & 1)) {
            int32_t r2 = 0;
            play::guest_read(w + kTableRows, &r2, 4);
            if (r2 > 0 && r2 <= 32) idle.push_back({w, r2});
            continue;
        }
        int32_t rows = 0, cur = 0;
        play::guest_read(w + kTableRows, &rows, 4);
        play::guest_read(w + kTableCurrent, &cur, 4);
        if (rows <= 0 || rows > 32) continue;
        s.capable += static_cast<uint32_t>(rows);
        if (!have_active) have_active = table_center_x(c, w, cam, rows, &active_cx);
        const int hit = table_row_at(c, w, cam, s.snap.gx, s.snap.gy, rows);
        if (s.log) NXLOG_I(kTag, "gui table %llx rows=%d current=%d pointer=(%.3f, %.3f) -> row %d", static_cast<unsigned long long>(w), rows, cur, s.snap.gx, s.snap.gy, hit);
        if (hit < 0) continue;
        if (hit != cur) { call_guest(c, kFnTableSelectRow, w, static_cast<uint64_t>(hit)); s.hovers++; }
        if (s.snap.click) {
            // accept is the pad's A (some screens read it themselves, like the Quit Game row of the options, and not through the table's callback)
            s.pending_act = Act::Confirm;
            pointer().click_used();
            s.clicks++;
            return;
        }
    }
    // the pointer is over a row of the other list: ask the pad's way across (A into the list on the right, B back to the one on the left). The list
    // becomes active in the game's own way, and its row under the pointer is selected on the next frames like any other.
    if (have_active && !idle.empty() && s.focus_cooldown == 0) {
        for (const Idle& t : idle) {
            const int hit = table_row_at(c, t.w, cam, s.snap.gx, s.snap.gy, t.rows);
            float cx = 0;
            if (hit < 0 || !table_center_x(c, t.w, cam, t.rows, &cx) || std::fabs(cx - active_cx) < 0.1f) continue;
            s.pending_act = cx > active_cx ? Act::Confirm : Act::Back;
            s.focus_cooldown = 20;
            s.rehover = 25;
            if (s.snap.click) { pointer().click_used(); s.clicks++; } else { s.hovers++; }
            if (s.log) NXLOG_I(kTag, "gui table %llx (row %d) is not active: %s", static_cast<unsigned long long>(t.w), hit, cx > active_cx ? "A (into the right list)" : "B (back to the left list)");
            return;
        }
    }
    // sliders: a click on the track (or a drag that started on it) sets the value the pointer is at; the slider's own Update snaps it to its increment and
    // tells the screen, as it does when the pad moves it
    if (!s.snap.held) s.drag_slider = 0;
    s.over_value = false;
    for (uint32_t i = 0; i < n && i < 400; ++i) {
        uint64_t w = 0, vp = 0;
        if (!play::guest_read(list + 8ull * i, &w, 8) || !w || !play::guest_read(w, &vp, 8)) continue;
        if (std::strcmp(gui_type(vp), "SliderGroup") != 0) continue;
        if (!(call_guest(c, kFnGuiIsActive, w) & 1) || !(call_guest(c, kFnGuiIsVisible, w) & 1)) continue;
        float v[3];                                              // min, max, current
        uint64_t ends[2] = {}, bar = 0;
        if (!play::guest_read(w + 264, v, 8) || !play::guest_read(w + 296, ends, 16) || !play::guest_read(w + 112, &bar, 8) || !ends[1] || !bar) continue;
        float e[3], cb[3], st[3], se[3], ee[3];
        call_guest_vec3(c, kFnGuiWorldPos, ends[1], 0, e);
        call_guest_vec3(c, kFnGuiWorldPos, bar, 0, cb);
        st[0] = 2.f * cb[0] - e[0]; st[1] = 2.f * cb[1] - e[1]; st[2] = 2.f * cb[2] - e[2];   // the bar is centred between the two ends
        call_guest_vec3(c, kFnGuiToScreen, cam, reinterpret_cast<uint64_t>(st), se);
        call_guest_vec3(c, kFnGuiToScreen, cam, reinterpret_cast<uint64_t>(e), ee);
        const float x0 = std::min(se[0], ee[0]), x1 = std::max(se[0], ee[0]);
        if (!(x1 - x0 > 0.02f) || !(v[1] > v[0])) continue;
        const bool near_track = s.snap.gx >= x0 - 0.04f && s.snap.gx <= x1 + 0.04f && std::fabs(s.snap.gy - se[1]) <= 0.07f;
        if (near_track) s.over_value = true;
        if ((s.snap.click && near_track) || (s.snap.held && s.drag_slider == w)) {
            if (s.snap.click) { s.drag_slider = w; pointer().click_used(); s.clicks++; }
            const float f = std::min(1.f, std::max(0.f, (s.snap.gx - se[0]) / (ee[0] - se[0])));
            const float target = v[0] + f * (v[1] - v[0]);
            play::guest_write(w + 280, &target, 4);
            if (s.log) NXLOG_I(kTag, "slider %llx track %.3f..%.3f pointer %.3f -> %.1f of %.0f..%.0f", static_cast<unsigned long long>(w), x0, x1, s.snap.gx, target, v[0], v[1]);
            return;
        }
    }
    if (s.snap.click && s.pause_mgr) {
        for (const PauseLabel& l : kPauseLabels) {
            uint64_t lw = 0;
            if (!play::guest_read(s.pause_mgr + l.off, &lw, 8) || !lw) continue;
            NdcRect r;
            gui_pane_rect(c, lw, cam, &r, 3);
            if (!r.valid()) continue;
            (void)0;
            if (s.log) NXLOG_I(kTag, "pause label +%llu rect (%.3f %.3f)-(%.3f %.3f) pointer (%.3f %.3f)", static_cast<unsigned long long>(l.off), r.x0, r.y0, r.x1, r.y1, s.snap.gx, s.snap.gy);
            if (s.snap.gx >= r.x0 - l.left && s.snap.gx <= r.x1 + l.right && s.snap.gy >= r.y0 - l.below && s.snap.gy <= r.y1 + l.above) {
                s.pending_act = l.act;
                pointer().click_used();
                s.clicks++;
                return;
            }
        }
    }
}

int pause_hook(::Ctx* c, uint64_t, void*) {
    S().pause_mgr = c->x[0];
    return 0;
}

int frame_hook(::Ctx* c, uint64_t, void*) {
    State& s = S();
    if (t_in_frame) return 0;
    t_in_frame = true;
    const uint64_t frame = c->x[0];
    if (s.dump) {
        uint32_t n = 0;
        play::guest_read(frame + kFrameWidgetCount, &n, 4);
        if (frame != g_dumped_frame || n != g_dumped_count) {
            g_dumped_frame = frame;
            g_dumped_count = n;
            dump_frame(c, frame);
        }
    }
    if (s.snap.active) gui_pointer(c, frame);
    t_in_frame = false;
    return 0;
}

int widget_hook(::Ctx* c, uint64_t addr, void*) {
    if (t_in_widget) return 0;                              // the nested call that runs the game's own function
    State& s = S();
    if (!s.snap.active || (!s.chosen && !s.hover_w) || (c->x[0] != s.chosen && c->x[0] != s.hover_w)) return 0;
    s.widget_calls++;
    const uint64_t w = c->x[0], sm = c->x[1];
    ::Ctx saved = *c;
    t_in_widget = true;
    dispatch_call(c, addr);
    t_in_widget = false;
    uint64_t r = c->x[0] & 0xff;
    *c = saved;
    if (r == 0) r = pointer_widget(c, w, sm);               // nothing came from the pad or keys for this widget: act on the pointer
    if (w == s.chosen) s.chosen = 0;
    if (w == s.hover_w) s.hover_w = 0;
    c->x[0] = r;
    return 1;
}

int position_hook(::Ctx* c, uint64_t, void*) {
    State& s = S();
    if (!s.force_pos) return 0;
    c->v[0].f32[0] = s.pos[0];
    c->v[1].f32[0] = s.pos[1];
    return 1;
}

}  // namespace

void game_install(uint64_t module_base) {
    State& s = S();
    if (s.hook_widget) return;
    s.base = module_base;
    s.log = std::getenv("NXRT_MENUMOUSE_LOG") ? std::atoi(std::getenv("NXRT_MENUMOUSE_LOG")) : 0;
    s.hook_widget = hook_add_entry(module_base + kFnWidgetProcessInput, &widget_hook);
    s.hook_pos = hook_add_entry(module_base + kFnGetTouchPosition, &position_hook);
    s.dump = std::getenv("NXRT_MENUMOUSE_DUMP") != nullptr;
    s.hook_frame = hook_add_entry(module_base + kFnGuiFrameInput, &frame_hook);
    s.hook_menu = hook_add_entry(module_base + kFnMenuBeforeThink, &menu_hook);
    s.hook_pause = hook_add_entry(module_base + kFnPauseManagerInput, &pause_hook);
}

void game_remove() {
    State& s = S();
    if (s.hook_widget) hook_remove(s.hook_widget);
    if (s.hook_pos) hook_remove(s.hook_pos);
    if (s.hook_frame) hook_remove(s.hook_frame);
    if (s.hook_menu) hook_remove(s.hook_menu);
    if (s.hook_pause) hook_remove(s.hook_pause);
    s.hook_widget = s.hook_pos = s.hook_frame = s.hook_menu = s.hook_pause = 0;
}

Act game_frame(bool menu_up, const Geometry& g) {
    State& s = S();
    s.snap = pointer().begin_frame(menu_up, g);
    Act out = s.pending_act;
    s.pending_act = Act::None;
    if (out == Act::None && s.snap.click_unclaimed && s.capable == 0) out = Act::Confirm;   // nothing on this screen takes a click (the title screen, a message): the click is the confirm key
    if (!s.snap.click) s.capable = 0;
    return out;
}

const Snapshot& game_snapshot() { return S().snap; }

bool game_over_value() { return S().over_value || S().over_value_goc; }

uint64_t game_hook_calls() {
    State& s = S();
    return (s.hook_widget ? hook_calls(s.hook_widget) : 0) + (s.hook_pos ? hook_calls(s.hook_pos) : 0);
}

GameStats game_stats() {
    State& s = S();
    return GameStats{s.hovers.load(), s.clicks.load(), s.widget_calls.load()};
}

}  // namespace nxrt::play::mm
