#include "ImGuiSplitscreen.hpp"

#include "imgui.h"

#include "dusk/main.h"
#include "dusk/splitscreen.hpp"
#include "m_Do/m_Do_controller_pad.h"

namespace dusk {

void ImGuiSplitscreen::draw() {
#ifdef DUSK_SPLITSCREEN
    if (!ImGui::BeginMenu("Splitscreen")) return;

    const bool game_running = dusk::IsGameLaunched;
    const bool active       = dusk_ss::IsActive();
    const bool transitioning = dusk_ss::IsTransitioning();

    // --- Status readout -----------------------------------------------------
    ImGui::TextDisabled("Status");
    ImGui::Separator();
    ImGui::Text("State:        %s", dusk_ss::GetStateName());
    ImGui::Text("Player count: %d", dusk_ss::GetPlayerCount());
    ImGui::Text("Transition:   %.0f%%", dusk_ss::GetTransitionT() * 100.0f);
    ImGui::Text("Pad-2 plugged:%s", mDoCPd_c::isConnect(PAD_2) ? " yes" : " no");
    ImGui::Spacing();

    // --- Actions ------------------------------------------------------------
    ImGui::TextDisabled("Actions");
    ImGui::Separator();

    if (!game_running) {
        ImGui::TextDisabled("(Launch the game first.)");
    } else {
        // Join / Leave is a single toggle button that mirrors current state.
        if (!active && !transitioning) {
            if (ImGui::Button("Join P2", ImVec2(180, 0))) {
                dusk_ss::RequestJoinP2();
            }
            ImGui::SameLine();
            ImGui::TextDisabled("(or Start on pad-2 / F9)");
        } else if (active) {
            if (ImGui::Button("Drop P2 out", ImVec2(180, 0))) {
                dusk_ss::RequestLeaveP2();
            }
            ImGui::SameLine();
            ImGui::TextDisabled("(or Start hold on pad-2 / F10)");
        } else {
            // Transitioning — show disabled-style indicator
            ImGui::BeginDisabled();
            ImGui::Button(transitioning ? "(transition in progress)" : "Join P2", ImVec2(180, 0));
            ImGui::EndDisabled();
        }

        ImGui::Spacing();

        const bool can_warp = active && dusk_ss::GetP2Actor() != nullptr;
        if (!can_warp) ImGui::BeginDisabled();
        if (ImGui::Button("Warp P2 to P1", ImVec2(180, 0))) {
            dusk_ss::WarpPlayer2ToPlayer1();
        }
        if (!can_warp) ImGui::EndDisabled();
        ImGui::SameLine();
        ImGui::TextDisabled("(or L+R+D-Up held / F11)");
    }

    ImGui::Spacing();

    // --- Settings -----------------------------------------------------------
    ImGui::TextDisabled("Settings");
    ImGui::Separator();
    bool auto_join = dusk_ss::IsAutoJoinOnConnect();
    if (ImGui::Checkbox("Auto-join when pad-2 connects", &auto_join)) {
        dusk_ss::SetAutoJoinOnConnect(auto_join);
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip(
            "When ON, plugging in a second controller while the game is\n"
            "running automatically drops P2 in. When OFF, you'll need to\n"
            "press Start on pad-2 (or F9) to join.");
    }

    ImGui::Spacing();

    // --- Diagnostics --------------------------------------------------------
    if (ImGui::TreeNode("Diagnostics")) {
        ImGui::TextDisabled("Env vars (set before launch):");
        ImGui::BulletText("DUSK_SS_AUTO_JOIN=<frame>");
        ImGui::BulletText("DUSK_SS_AUTO_LEAVE=<frame>");
        ImGui::BulletText("DUSK_SS_AUTO_WARP=<frame>");
        ImGui::BulletText("DUSK_SS_SKIP_INTRO=<frame>");
        ImGui::Spacing();
        ImGui::TextDisabled("Hotkeys:");
        ImGui::BulletText("F9   Join P2");
        ImGui::BulletText("F10  Drop P2 out");
        ImGui::BulletText("F11  Warp P2 to P1");
        ImGui::BulletText("F12  Skip intro");
        ImGui::TreePop();
    }

    ImGui::EndMenu();
#endif  // DUSK_SPLITSCREEN
}

}  // namespace dusk
