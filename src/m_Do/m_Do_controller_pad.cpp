/**
 * m_Do_controller_pad.cpp
 * JUTGamePad Wrapper and Conversion
 */

#include "m_Do/m_Do_controller_pad.h"
#include "JSystem/JAWExtSystem/JAWExtSystem.h"
#include "SSystem/SComponent/c_lib.h"
#include "d/d_com_inf_game.h"
#include "dusk/netcoop.hpp"
#include "dusk/test_autoboot.hpp"
#include "f_ap/f_ap_game.h"
#include "m_Do/m_Do_Reset.h"
#include "m_Do/m_Do_main.h"
#include "tracy/Tracy.hpp"
#include <cmath>
#include <cstring>

JUTGamePad* mDoCPd_c::m_gamePad[4];

interface_of_controller_pad mDoCPd_c::m_cpadInfo[4];
interface_of_controller_pad mDoCPd_c::m_debugCpadInfo[4];

void mDoCPd_c::create() {
    #if PLATFORM_GCN || PLATFORM_SHIELD
    m_gamePad[0] = JKR_NEW JUTGamePad(JUTGamePad::EPort1);
    #endif

    if (DEBUG || mDoMain::developmentMode != 0) {
        #if PLATFORM_WII
        m_gamePad[0] = JKR_NEW JUTGamePad(JUTGamePad::EPort1);
        #endif

        m_gamePad[1] = JKR_NEW JUTGamePad(JUTGamePad::EPort2);
        m_gamePad[2] = JKR_NEW JUTGamePad(JUTGamePad::EPort3);
        m_gamePad[3] = JKR_NEW JUTGamePad(JUTGamePad::EPort4);
    } else {
        #if PLATFORM_WII
        m_gamePad[0] = NULL;
        #endif

        m_gamePad[1] = NULL;
        m_gamePad[2] = NULL;
        m_gamePad[3] = NULL;
    }

    #if PLATFORM_GCN || PLATFORM_SHIELD
    if (!mDoRst::isReset()) {
        JUTGamePad::clearResetOccurred();
        JUTGamePad::setResetCallback(mDoRst_resetCallBack, NULL);
    }
    #endif
    JUTGamePad::setAnalogMode(3);

    interface_of_controller_pad* cpad = &m_cpadInfo[0];
    for (int i = 0; i < 4; i++) {
        cpad->mHoldLockL = cpad->mTrigLockL = false;
        cpad->mHoldLockR = cpad->mTrigLockR = false;
        cpad++;
    }
}

void mDoCPd_c::read() {
    ZoneScoped;
    JUTGamePad::read();

    if (!mDoRst::isReset() && mDoRst::is3ButtonReset()) {
        if (!JUTGamePad::getGamePad(mDoRst::get3ButtonResetPort())->isPushing3ButtonReset()) {
            mDoRst::off3ButtonReset();
        }
    }

#if DEBUG
    if (m_gamePad[3]) {
        JAWExtSystem::padProc(*m_gamePad[3]);
    }
#endif

    JUTGamePad** pad = m_gamePad;
    interface_of_controller_pad* interface = m_cpadInfo;
#if DEBUG
    interface_of_controller_pad* interface2 = m_debugCpadInfo;
    if (dComIfG_isDebugMode()) {
        interface_of_controller_pad* tmp = interface;
        interface = interface2;
        interface2 = tmp;
    }
#endif

    for (u32 i = 0; i < 4; i++) {
        if (*pad == NULL) {
            cLib_memSet(interface, 0, sizeof(interface_of_controller_pad));
        } else {
            convert(interface, *pad);
            LRlockCheck(interface);
        }
#if DEBUG
        cLib_memSet(interface2, 0, sizeof(interface_of_controller_pad));
#endif
        pad++;
        interface++;
#if DEBUG
        interface2++;
#endif
    }

#ifdef DUSK_NETCOOP
    // Netcoop role-derived controller filter: each instance only consumes
    // input from its assigned slot. The engine always reads port 0 for the
    // main player, so for the client (slot 1) we slot the assigned port's
    // payload into m_cpadInfo[0] and zero the rest. Solo runs (no peer)
    // skip the filter entirely so single-player input is unchanged.
    const int slot = ::dusk::netcoop::GetAssignedPadSlot();
    if (slot >= 0 && slot < 4) {
        if (slot != 0) {
            m_cpadInfo[0] = m_cpadInfo[slot];
        }
        for (int i = 1; i < 4; ++i) {
            std::memset(&m_cpadInfo[i], 0, sizeof(m_cpadInfo[i]));
        }
    }
#endif

    // Test-mode stick injection: applied AFTER the netcoop filter so it
    // survives whatever slot gating just zeroed. Lets the harness drive
    // Link to walk in a given direction without real controller input.
    if (::dusk::test_autoboot::stick_enabled()) {
        const float x = std::isnan(::dusk::test_autoboot::stick_x())
                            ? 0.0f
                            : ::dusk::test_autoboot::stick_x();
        const float y = std::isnan(::dusk::test_autoboot::stick_y())
                            ? 0.0f
                            : ::dusk::test_autoboot::stick_y();
        m_cpadInfo[0].mMainStickPosX  = x;
        m_cpadInfo[0].mMainStickPosY  = y;
        const float mag = std::sqrt(x * x + y * y);
        m_cpadInfo[0].mMainStickValue = mag > 1.0f ? 1.0f : mag;
        // GC stick-angle convention: 0 = north, positive = east, range
        // is the full s16 covering [-pi, pi). atan2(x, y) maps (north=0,
        // east=+pi/2) which matches.
        m_cpadInfo[0].mMainStickAngle =
            static_cast<s16>(std::atan2(x, y) * (32768.0f / 3.14159265f));
    }
}

void mDoCPd_c::convert(interface_of_controller_pad* pInterface, JUTGamePad* pPad) {
    pInterface->mButtonFlags = pPad->getButton();
    pInterface->mPressedButtonFlags = pPad->getTrigger();
    pInterface->mMainStickPosX = pPad->getMainStickX();
    pInterface->mMainStickPosY = pPad->getMainStickY();
    pInterface->mMainStickValue = pPad->getMainStickValue();
    pInterface->mMainStickAngle = pPad->getMainStickAngle();
    pInterface->mCStickPosX = pPad->getSubStickX();
    pInterface->mCStickPosY = pPad->getSubStickY();
    pInterface->mCStickValue = pPad->getSubStickValue();
    pInterface->mCStickAngle = pPad->getSubStickAngle();

    mDoCPd_ANALOG_CONV(pPad->getAnalogA(), pInterface->mAnalogA);
    mDoCPd_ANALOG_CONV(pPad->getAnalogB(), pInterface->mAnalogB);
    mDoCPd_TRIGGER_CONV(pPad->getAnalogL(), pInterface->mTriggerLeft);
    mDoCPd_TRIGGER_CONV(pPad->getAnalogR(), pInterface->mTriggerRight);

    pInterface->mGamepadErrorFlags = pPad->getErrorStatus();
}

void mDoCPd_c::LRlockCheck(interface_of_controller_pad* interface) {
    f32 trigger = interface->mTriggerLeft;
    interface->mTrigLockL = false;
    interface->mTrigLockR = false;

    if (trigger > fapGmHIO_getLROnValue()) {
        if (interface->mHoldLockL != true) {
            interface->mTrigLockL = true;
        }
        interface->mHoldLockL = true;
    } else if (trigger < fapGmHIO_getLROffValue()) {
        interface->mHoldLockL = false;
    }

    trigger = interface->mTriggerRight;
    if (trigger > fapGmHIO_getLROnValue()) {
        if (interface->mHoldLockR != true) {
            interface->mTrigLockR = true;
        }
        interface->mHoldLockR = true;
    } else if (trigger < fapGmHIO_getLROffValue()) {
        interface->mHoldLockR = false;
    }
}

void mDoCPd_c::recalibrate(void) {
    JUTGamePad::clearForReset();
    JUTGamePad::CRumble::setEnabled(PAD_CHAN3_BIT | PAD_CHAN2_BIT | PAD_CHAN1_BIT | PAD_CHAN0_BIT);
}
