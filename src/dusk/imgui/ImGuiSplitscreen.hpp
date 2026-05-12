#pragma once

namespace dusk {

class ImGuiSplitscreen {
public:
    ImGuiSplitscreen() = default;
    // Called from within ImGui::BeginMainMenuBar(). Renders the top-level
    // "Splitscreen" menu when DUSK_SPLITSCREEN is on; no-op otherwise.
    void draw();
};

}  // namespace dusk
