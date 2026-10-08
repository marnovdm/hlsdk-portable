#pragma once

#include "vr_client_api.h"
#include "vr_usercmd_sidecar.h"

struct ref_params_s;

void VR_Init();
void VR_Reset();
void VR_UpdateCurrentWeapon( long weapon );
void VR_UpdateViewmodel( struct ref_params_s *pparams );
int VR_IsMirroredHandEntity( const struct cl_entity_s *entity );
int VR_IsActive();
/* Hand-mounted HUD (vr_hud_hands): health/suit and ammo report the screen boxes they drew, the renderer
 * moves those parts of the HUD onto small panels: health above suit on the off hand, ammo on the weapon hand. */
/* VR_HUD_BOX_WEAPONS..+2: rows of the pointer weapon grid (+vr_weaponpointer), shown side by side on the off hand
 * whenever VR is active (a grid wider than the HUD is drawn wrapped and put back into one row by the renderer). */
/* VR_HUD_BOX_DAMAGE: the damage-type icons (drowning, freezing, ...), next to health/suit on the off hand.
 * VR_HUD_BOX_FLASHLIGHT: the flashlight icon and battery, between suit and the damage icons. */
enum { VR_HUD_BOX_HEALTH, VR_HUD_BOX_SUIT, VR_HUD_BOX_AMMO, VR_HUD_BOX_DAMAGE, VR_HUD_BOX_FLASHLIGHT, VR_HUD_BOX_WEAPONS, VR_HUD_BOX_WEAPONS_LAST = VR_HUD_BOX_WEAPONS + 2,
	VR_HUD_BOX_COUNT };
int VR_HandHudActive();
void VR_HudBoxBegin();
void VR_HudBoxAdd( int box, int x, int y, int width, int height );
void VR_HudBoxPublish();
int VR_GetDominantHand();
int VR_IsWeaponStabilised();
int VR_ShouldApplyWeaponRecoil();
void VR_Haptic( float durationMilliseconds, unsigned int hand, float amplitude );
