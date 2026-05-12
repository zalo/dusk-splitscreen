#include "dusk/splitscreen.hpp"

#ifdef DUSK_SPLITSCREEN

#include "m_Do/m_Do_graphic.h"
#include "f_op/f_op_camera_mng.h"

namespace dusk_ss {

// Splitscreen viewport math.
//
// Layout: vertical split. Left half = eye 0 (P1), right half = eye 1 (P2).
// During the 12-frame drop-in animation we lerp:
//   eye 0 from full-screen to left-half
//   eye 1 from left edge (zero width) to right-half
// Drop-out plays the inverse via GetTransitionT() going 1 → 0.

ViewportRect GetEyeViewport(int eye) {
    const float fbw = static_cast<float>(FB_WIDTH);
    const float fbh = static_cast<float>(FB_HEIGHT);

    if (!IsActive() || eye < 0 || eye > 1) {
        return { 0.0f, 0.0f, fbw, fbh };
    }

    const float t    = GetTransitionT();          // 0=full, 1=split
    const float half = fbw * 0.5f;

    if (eye == 0) {
        // Eye 0 starts at full width, shrinks to the left half.
        const float w = fbw - half * t;
        return { 0.0f, 0.0f, w, fbh };
    } else {
        // Eye 1 grows from zero width on the right.
        const float w = half * t;
        return { fbw - w, 0.0f, w, fbh };
    }
}

void RenderEye(int eye) {
    SetActiveEye(eye);
    // The per-eye GXSetViewport/Scissor is handled inline at the m_Do_graphic
    // two-pass loop (search "GetEyeViewport" in src/m_Do/m_Do_graphic.cpp).
    // This stub exists so callers outside that file can still tag the active
    // eye explicitly if needed in future passes.
    (void)eye;
}

}  // namespace dusk_ss

#endif  // DUSK_SPLITSCREEN
