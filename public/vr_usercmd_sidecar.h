/*
vr_usercmd_sidecar.h - ABI-neutral VR usercmd extension
*/
#ifndef VR_USERCMD_SIDECAR_H
#define VR_USERCMD_SIDECAR_H

#include <stdint.h>

#define VR_USERCMD_SIDECAR_VERSION 1u

#define VR_USERCMD_SIDECAR_LADDER_VALID ( 1u << 0 )
#define VR_USERCMD_SIDECAR_POSE_VALID   ( 1u << 1 )
#define VR_USERCMD_SIDECAR_WIRE_BYTES 35u

/* This record is never embedded in usercmd_t or playermove_t.  Position
 * values are relative to the player's eye at 1/8 game-unit precision;
 * angles use degrees at 1/128 precision. */
typedef struct vr_usercmd_sidecar_s
{
	uint8_t version;
	uint8_t flags;
	int16_t ladder_angles[2]; /* pitch, yaw */
	int16_t weapon_position[3];
	int16_t weapon_angles[3];
	int16_t weapon_velocity[3];
	int16_t offhand_position[3];
	int16_t offhand_angles[3];
} vr_usercmd_sidecar_t;

#define VR_USERCMD_SIDECAR_BEGIN_PM_EXPORT "VR_BeginPMUsercmdSidecar"
#define VR_USERCMD_SIDECAR_END_PM_EXPORT "VR_EndPMUsercmdSidecar"
#define VR_USERCMD_SIDECAR_UPDATE_POSE_EXPORT "VR_UpdateUsercmdVRPose"

typedef void (*vr_usercmd_sidecar_begin_pm_t)( const vr_usercmd_sidecar_t *sample );
typedef void (*vr_usercmd_sidecar_end_pm_t)( void );
typedef void (*vr_usercmd_sidecar_update_pose_t)( void *player, const vr_usercmd_sidecar_t *sample );

#endif /* VR_USERCMD_SIDECAR_H */
