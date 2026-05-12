/**
 * f_op_camera_mng.cpp
 * Camera Process Manager
 */

#include "f_op/f_op_camera_mng.h"
#include "f_pc/f_pc_manager.h"
#include "dusk/logging.h"

static fpc_ProcID l_fopCamM_id[4];

u32 fopCamM_GetParam(camera_class* i_this) {
    // The process-parameters field is initialized from the procname's profile
    // and is the same (0) for every camera instance of fpcNm_CAMERA_e. That
    // breaks splitscreen because both cameras would self-identify as slot 0.
    // Resolve the slot by reverse-lookup against the l_fopCamM_id table: it
    // maps cameraIdx → proc_id at creation time.
    const fpc_ProcID my_id = fpcM_GetID(i_this);
    if (my_id != fpcM_ERROR_PROCESS_ID_e) {
        for (int i = 0; i < 4; i++) {
            if (l_fopCamM_id[i] == my_id) {
                return (u32)i;
            }
        }
    }
    // Fallback to legacy behaviour: process-parameters field.
    return fpcM_GetParam(i_this);
}

void dummy(fpc_ProcID i_procName) {
    fpcM_SearchByID(i_procName);
}

fpc_ProcID fopCamM_Create(int i_cameraIdx, s16 i_procName, void* i_append) {
    l_fopCamM_id[i_cameraIdx] = fpcM_Create(i_procName, NULL, i_append);
    return l_fopCamM_id[i_cameraIdx];
}

void fopCamM_Management() {}

void fopCamM_Init() {}
