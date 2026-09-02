/*
vr_client_api.h - optional VR client-DLL ABI
Copyright (C) 2026 Xash3D FWGS contributors

This program is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.
*/

#ifndef VR_CLIENT_API_H
#define VR_CLIENT_API_H

#include <stddef.h>
#include <stdint.h>

typedef struct vr_usercmd_sidecar_s vr_usercmd_sidecar_t;

#ifdef __cplusplus
extern "C"
{
#endif

#define VR_CLIENT_API_VERSION 2u
#define VR_CLIENT_FRAME_VERSION 2u
#define VR_CLIENT_API_EXPORT "HUD_GetVRClientAPI"

#if defined( _WIN32 )
#define VR_CLIENT_CALL __cdecl
#else
#define VR_CLIENT_CALL
#endif

#define VR_CLIENT_HAND_LEFT  0u
#define VR_CLIENT_HAND_RIGHT 1u
#define VR_CLIENT_MAX_HANDS  2u

/* vr_client_frame_t flags */
#define VR_CLIENT_FRAME_ACTIVE      ( 1u << 0 )
#define VR_CLIENT_FRAME_FOCUSED     ( 1u << 1 )
#define VR_CLIENT_FRAME_HEAD_VALID  ( 1u << 2 )

/* vr_client_hand_t flags */
#define VR_CLIENT_HAND_GRIP_VALID ( 1u << 0 )
#define VR_CLIENT_HAND_AIM_VALID  ( 1u << 1 )

/* vr_client_hand_t buttons */
#define VR_CLIENT_BUTTON_PRIMARY   ( 1u << 0 )
#define VR_CLIENT_BUTTON_SECONDARY ( 1u << 1 )
#define VR_CLIENT_BUTTON_MENU      ( 1u << 2 )
#define VR_CLIENT_BUTTON_STICK     ( 1u << 3 )

/* Position is in game units and the orthonormal basis is dimensionless. Both
 * use Xash coordinates and are expressed relative to the recenter transform.
 * Velocities use the same recentered basis. */
typedef struct vr_client_pose_s
{
	float position[3];
	float forward[3];
	float right[3];
	float up[3];
} vr_client_pose_t;

typedef struct vr_client_hand_s
{
	uint32_t flags;
	uint32_t buttons;
	float trigger;
	float squeeze;
	float stick[2];
	vr_client_pose_t grip;
	vr_client_pose_t aim;
	float linear_velocity[3];
	float angular_velocity[3];
} vr_client_hand_t;

typedef struct vr_client_frame_s
{
	uint32_t version;
	uint32_t struct_size;
	uint64_t frame_id;
	uint32_t flags;
	float world_scale;
	vr_client_pose_t head;
	vr_client_hand_t hands[VR_CLIENT_MAX_HANDS];
} vr_client_frame_t;

typedef int32_t (VR_CLIENT_CALL *vr_client_haptic_t)( uint32_t hand, float duration, float frequency, float amplitude );

typedef struct vr_client_engine_api_s
{
	uint32_t version;
	uint32_t struct_size;
	vr_client_haptic_t Haptic;
} vr_client_engine_api_t;

typedef void (VR_CLIENT_CALL *vr_client_frame_callback_t)( const vr_client_frame_t *frame );
typedef void (VR_CLIENT_CALL *vr_client_shutdown_t)( void );

/* This deliberately contains only the final usercmd values required to make a
 * sidecar. It is not usercmd_t and does not expose an engine ABI. */
typedef struct vr_client_usercmd_s
{
	uint32_t struct_size;
	float viewangles[3];
	float frametime;
} vr_client_usercmd_t;

/* Return nonzero only after filling the pose fields and setting POSE_VALID.
 * The engine owns the ladder fields and preserves them around this callback. */
typedef int32_t (VR_CLIENT_CALL *vr_client_build_usercmd_sidecar_t)(
	const vr_client_usercmd_t *cmd, vr_usercmd_sidecar_t *sample );

typedef struct vr_client_api_s
{
	uint32_t version;
	uint32_t struct_size;
	vr_client_frame_callback_t Frame;
	vr_client_shutdown_t Shutdown;
	/* Optional v2 tail; available only when struct_size reaches this field. */
	vr_client_build_usercmd_sidecar_t BuildUsercmdSidecar;
} vr_client_api_t;

/* Return nonzero only after writing a compatible vr_client_api_t. */
typedef int32_t (VR_CLIENT_CALL *vr_client_get_api_t)( const vr_client_engine_api_t *engine, vr_client_api_t *client );

#define VR_CLIENT_ENGINE_API_MIN_SIZE ( offsetof( vr_client_engine_api_t, Haptic ) + sizeof( vr_client_haptic_t ))
#define VR_CLIENT_API_MIN_SIZE ( offsetof( vr_client_api_t, Shutdown ) + sizeof( vr_client_shutdown_t ))
#define VR_CLIENT_API_SIDECAR_SIZE ( offsetof( vr_client_api_t, BuildUsercmdSidecar ) + sizeof( vr_client_build_usercmd_sidecar_t ))
#define VR_CLIENT_USERCMD_MIN_SIZE ( offsetof( vr_client_usercmd_t, frametime ) + sizeof( float ))

/* The public ABI is intentionally independent of engine, renderer, and
 * OpenXR headers. Keep these i386 Linux checks when extending v2. */
#if defined( __linux__ ) && UINTPTR_MAX == UINT32_MAX
#define VR_CLIENT_STATIC_ASSERT( name, expression ) typedef char vr_client_static_assert_ ## name[( expression ) ? 1 : -1]
VR_CLIENT_STATIC_ASSERT( pose_size, sizeof( vr_client_pose_t ) == 48 );
VR_CLIENT_STATIC_ASSERT( hand_size, sizeof( vr_client_hand_t ) == 144 );
VR_CLIENT_STATIC_ASSERT( frame_size, sizeof( vr_client_frame_t ) == 360 );
VR_CLIENT_STATIC_ASSERT( frame_hands_offset, offsetof( vr_client_frame_t, hands ) == 72 );
VR_CLIENT_STATIC_ASSERT( engine_api_size, sizeof( vr_client_engine_api_t ) == 12 );
VR_CLIENT_STATIC_ASSERT( engine_api_haptic_offset, offsetof( vr_client_engine_api_t, Haptic ) == 8 );
VR_CLIENT_STATIC_ASSERT( client_api_size, sizeof( vr_client_api_t ) == 20 );
VR_CLIENT_STATIC_ASSERT( client_api_frame_offset, offsetof( vr_client_api_t, Frame ) == 8 );
VR_CLIENT_STATIC_ASSERT( client_api_sidecar_offset, offsetof( vr_client_api_t, BuildUsercmdSidecar ) == 16 );
VR_CLIENT_STATIC_ASSERT( client_usercmd_size, sizeof( vr_client_usercmd_t ) == 20 );
#undef VR_CLIENT_STATIC_ASSERT
#endif

#ifdef __cplusplus
}
#endif

#endif /* VR_CLIENT_API_H */
