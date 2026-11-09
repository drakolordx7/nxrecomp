// SPDX-License-Identifier: GPL-3.0-or-later
// Button prompts, the Metroid Prime Remastered side (prompt_resolve.h PromptGame): the game's 19 button icon textures and how to read them from
// the player's own paks. The base (src/gfx/play/prompts.cpp) recognises them in the uploads by these bytes and swaps in the pack's icons.
#include <algorithm>
#include <cstring>
#include <filesystem>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "nxrt/log.h"
#include "pak.h"
#include "prompt_resolve.h"
#include "txtr.h"

namespace nxrt::play {

namespace {

constexpr const char* kTag = "prompts";
constexpr uint32_t kIconSize = 36;

constexpr const char* kGui = "Preload/MPR1/GuiSysMP1.pak";
constexpr const char* kFront = "Preload/MPRT/PreloadFrontEndMPT.pak";

// Asset ids of the icon textures in the 1.0.0 paks (the ids tools/survey lists for TXTR_ButtonA ... TXTR_DPadIdle).
const PromptIcon kIcons[kIconCount] = {
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
    {Btn::None, false, "TXTR_LStickIdle", "f72e5e94-6afe-4860-82b0-756714ce3e0a", kGui, "0f32e3c3-1893-4d29-9c53-fe2163e434a5"},
    {Btn::None, false, "TXTR_RStickIdle", "1dbb8f79-6e18-4d41-8586-65341d66fa2a", kGui, "875ca189-1d34-4d3c-970f-261eb0d13c21"},
};

// Reads the icon textures from the player's own paks. Every icon has up to two copies: the one of the in-game interface (GuiSysMP1.pak, blue)
// and the front end's own (PreloadFrontEndMPT.pak, red: the title screen and the menus draw those).
int scan(const std::string& romfs, void (*found_fn)(void* user, Icon icon, const uint8_t* tiled, size_t size), void* user) {
    std::map<std::string, std::unique_ptr<mprs::Pak>> paks;
    int found = 0;
    const auto add = [&](Icon ic, const std::string& rel, const std::string& uuid) {
        try {
            auto it = paks.find(rel);
            if (it == paks.end()) {
                const std::filesystem::path p = std::filesystem::path(romfs) / rel;
                it = paks.emplace(rel, mprs::Pak::open(mprs::open_file_storage(p.string()))).first;
            }
            const mprs::Pak& pak = *it->second;
            for (const mprs::PakEntry& e : pak.assets()) {
                if (std::strcmp(e.type, "TXTR") != 0 || e.id.str() != uuid) continue;
                const mprs::TxtrInfo info = mprs::txtr_read_info(pak, e);
                if (info.width != kIconSize || info.height != kIconSize || info.mips != 1) break;
                const std::vector<mprs::u8> buf = mprs::txtr_read_tiled(pak, e, info);
                found_fn(user, ic, buf.data(), buf.size());
                ++found;
                break;
            }
        } catch (const std::exception& ex) {
            NXLOG_W(kTag, "%s in %s: %s", kIcons[static_cast<int>(ic)].asset, rel.c_str(), ex.what());
        }
    };
    for (int i = 0; i < kIconCount; ++i) {
        const Icon ic = static_cast<Icon>(i);
        add(ic, kIcons[i].pak, kIcons[i].uuid);
        if (const char* front = kIcons[i].front_uuid) add(ic, kFront, front);
    }
    return found;
}

}  // namespace

const PromptGame& mpr_prompt_game() {
    static const PromptGame g = [] {
        PromptGame p;
        p.icons = kIcons;
        p.front_pak = kFront;
        p.icon_size = kIconSize;
        p.upside_down = true;      // the game stores its UI textures upside down (row 0 of the storage is the bottom row; the UI flips v when it draws)
        p.scan = &scan;
        return p;
    }();
    return g;
}

}  // namespace nxrt::play
