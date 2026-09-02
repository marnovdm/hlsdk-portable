#pragma once

#include "vr_client_api.h"
#include "vr_usercmd_sidecar.h"

struct ref_params_s;

void VR_Init();
void VR_Reset();
void VR_UpdateCurrentWeapon( long weapon );
void VR_UpdateViewmodel( struct ref_params_s *pparams );
int VR_IsActive();
int VR_GetDominantHand();
int VR_IsWeaponStabilised();
int VR_ShouldApplyWeaponRecoil();
void VR_Haptic( float durationMilliseconds, unsigned int hand, float amplitude );
