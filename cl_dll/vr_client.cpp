#include "hud.h"
#include "cl_util.h"
#include "ref_params.h"
#include "in_defs.h"
#include "util_vector.h"
#include "entity_types.h"
#include "vr_client.h"
#include "com_model.h"

#include <float.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define WEAPON_CROWBAR 1
#define WEAPON_CROSSBOW 6
#define WEAPON_HANDGRENADE 12
#define WEAPON_TRIPMINE 13
#define WEAPON_SATCHEL 14
#define WEAPON_SNARK 15
#define WEAPON_KNIFE 19
#define WEAPON_PENGUIN 21
#define WEAPON_PIPEWRENCH 22
#define WEAPON_SNIPERRIFLE 24

static vr_client_frame_t g_vrFrame;
static vr_client_engine_api_t g_vrEngineAPI;
static long g_vrCurrentWeapon;
static int g_vrInitialised;
static int g_vrActiveLogged;
static float g_vrNextPoseSendTime;
static int g_vrScopeButtonDown;
static int g_vrScopeEngaged;
static int g_vrScopeAnglesValid;
static Vector g_vrScopeAngles;
static cl_entity_t g_vrTorchEntity;
static struct model_s *g_vrTorchModel;
static int g_vrTorchModelIndex;
static cl_entity_t g_vrHandEntities[VR_CLIENT_MAX_HANDS];
static struct model_s *g_vrHandModels[VR_CLIENT_MAX_HANDS];
static int g_vrHandModelIndices[VR_CLIENT_MAX_HANDS];

typedef struct vr_weapon_pose_s
{
	Vector weaponOffset;
	Vector weaponAngles;
	Vector weaponVelocity;
	Vector supportOffset;
	Vector supportAngles;
	int stabilised;
} vr_weapon_pose_t;

static vr_weapon_pose_t g_vrCachedWeaponPose;
static uint64_t g_vrCachedPoseFrameId;
static float g_vrCachedPoseYaw;
static int g_vrCachedWeaponPoseValid;

#define VR_POSE_SEND_INTERVAL 0.05f

void VectorAngles( const float *forward, float *angles );

static void VR_ClearFrame()
{
	memset( &g_vrFrame, 0, sizeof( g_vrFrame ) );
	g_vrNextPoseSendTime = 0.0f;
	g_vrScopeEngaged = 0;
	g_vrScopeAnglesValid = 0;
	g_vrCachedWeaponPoseValid = 0;
	gEngfuncs.Cvar_SetValue( "vr_weapon_stabilised", 0.0f );
	gEngfuncs.Cvar_SetValue( "vr_scope_engaged", 0.0f );
}

static void VR_ReleaseScopeButton()
{
	if( g_vrScopeButtonDown && gEngfuncs.pfnClientCmd )
		gEngfuncs.pfnClientCmd( "-alt1" );
	g_vrScopeButtonDown = 0;
}

static void VR_CLIENT_CALL VR_ClientFrame( const vr_client_frame_t *frame )
{
	if( !frame || frame->version != VR_CLIENT_FRAME_VERSION || frame->struct_size < sizeof( g_vrFrame ) )
	{
		VR_ClearFrame();
		return;
	}

	memcpy( &g_vrFrame, frame, sizeof( g_vrFrame ) );
	g_vrCachedWeaponPoseValid = 0;
	if( !( g_vrFrame.flags & VR_CLIENT_FRAME_ACTIVE ) )
		gEngfuncs.Cvar_SetValue( "vr_weapon_stabilised", 0.0f );
	if( !g_vrActiveLogged && ( g_vrFrame.flags & VR_CLIENT_FRAME_ACTIVE ) )
	{
		gEngfuncs.Con_Printf( "VR client: first active tracked frame received\n" );
		g_vrActiveLogged = 1;
	}
}

static void VR_CLIENT_CALL VR_ClientShutdown()
{
	VR_ReleaseScopeButton();
	VR_ClearFrame();
	memset( &g_vrEngineAPI, 0, sizeof( g_vrEngineAPI ) );
	g_vrActiveLogged = 0;
}

static Vector VR_RotateYaw( const float vector[3], float bodyYaw )
{
	const float radians = bodyYaw * (float)M_PI / 180.0f;
	const float sine = sinf( radians );
	const float cosine = cosf( radians );

	return Vector( vector[0] * cosine - vector[1] * sine,
		vector[0] * sine + vector[1] * cosine, vector[2] );
}

static void VR_BasisAngles( const Vector &forward, const Vector &right, const Vector &up, Vector &angles )
{
	float pitch = -asinf( fmaxf( -1.0f, fminf( 1.0f, forward.z ) ) );
	float cpitch = cosf( pitch );
	float yaw, roll;

	if( fabsf( cpitch ) > 0.000001f )
	{
		cpitch = 1.0f / cpitch;
		pitch = pitch * 180.0f / (float)M_PI;
		yaw = atan2f( forward.y * cpitch, forward.x * cpitch ) * 180.0f / (float)M_PI;
		roll = atan2f( -right.z * cpitch, up.z * cpitch ) * 180.0f / (float)M_PI;
	}
	else
	{
		pitch = forward.z > 0.0f ? -90.0f : 90.0f;
		yaw = atan2f( right.x, -right.y ) * 180.0f / (float)M_PI;
		roll = 180.0f;
	}
	angles = Vector( pitch, yaw, roll );
}

static void VR_TransformPose( const vr_client_pose_t &pose, float bodyYaw, Vector &position, Vector &angles )
{
	const Vector forward = VR_RotateYaw( pose.forward, bodyYaw );
	const Vector right = VR_RotateYaw( pose.right, bodyYaw );
	const Vector up = VR_RotateYaw( pose.up, bodyYaw );

	position = VR_RotateYaw( pose.position, bodyYaw );
	VR_BasisAngles( forward, right, up, angles );
}

/* Angles of a pose tilted nose-down by downDegrees about its own right axis. The grip correction
 * used to be added to the Euler pitch, which near straight up/down (gimbal lock: yaw and roll
 * become the same rotation, yaw jumps with tiny hand moves) tilted the gun towards a random
 * direction - aiming steeply up went all over the place. */
static void VR_TransformPoseTilted( const vr_client_pose_t &pose, float bodyYaw, float downDegrees, Vector &angles )
{
	const float a = downDegrees * (float)M_PI / 180.0f;
	const Vector forward = VR_RotateYaw( pose.forward, bodyYaw );
	const Vector right = VR_RotateYaw( pose.right, bodyYaw );
	const Vector up = VR_RotateYaw( pose.up, bodyYaw );

	VR_BasisAngles( forward * cosf( a ) - up * sinf( a ), right, up * cosf( a ) + forward * sinf( a ), angles );
}

/* Studio models keep the Quake pitch convention (positive = nose up) and the
 * renderer negates it again (StudioSetUpTransform), like view.cpp does for the
 * stock viewmodel. VR poses are view angles, so flip pitch for model entities. */
static Vector VR_ModelAngles( const Vector &viewAngles )
{
	return Vector( -viewAngles[PITCH], viewAngles[YAW], viewAngles[ROLL] );
}

/* The usercmd sidecar packs angles as int16 at 1/128 degree, i.e. +-256 degrees. VectorAngles
 * returns 0..360 (the two-handed aim), so pitch just below level (~350) or yaw past 256 was
 * clamped and the server fired somewhere else than the gun pointed. Wrap to -180..180 first. */
static Vector VR_WrapAngles( const Vector &angles )
{
	Vector wrapped;
	for( int index = 0; index < 3; ++index )
	{
		float value = fmodf( angles[index] + 180.0f, 360.0f );
		if( value < 0.0f )
			value += 360.0f;
		wrapped[index] = value - 180.0f;
	}
	return wrapped;
}

static int VR_IsFiniteVector( const Vector &vector )
{
	return vector.x == vector.x && vector.x >= -FLT_MAX && vector.x <= FLT_MAX &&
		vector.y == vector.y && vector.y >= -FLT_MAX && vector.y <= FLT_MAX &&
		vector.z == vector.z && vector.z >= -FLT_MAX && vector.z <= FLT_MAX;
}

static float VR_Clamp( float value, float minimum, float maximum )
{
	return value < minimum ? minimum : ( value > maximum ? maximum : value );
}

static float VR_AngleDelta( float value, float reference )
{
	float delta = value - reference;
	while( delta > 180.0f ) delta -= 360.0f;
	while( delta < -180.0f ) delta += 360.0f;
	return delta;
}

static Vector VR_LerpAngles( const Vector &from, const Vector &to, float fraction )
{
	return Vector( from.x + VR_AngleDelta( to.x, from.x ) * fraction,
		from.y + VR_AngleDelta( to.y, from.y ) * fraction,
		from.z + VR_AngleDelta( to.z, from.z ) * fraction );
}

static void VR_SendPoseUpdate( ref_params_s *pparams, const Vector &weaponPosition,
	const Vector &weaponAngles, const Vector &weaponVelocity, const Vector &offhandPosition,
	const Vector &offhandAngles )
{
	char command[256];
	int commandLength;

	if( g_vrEngineAPI.struct_size >= VR_CLIENT_ENGINE_API_NEGOTIATION_SIZE &&
		g_vrEngineAPI.IsUsercmdSidecarNegotiated && g_vrEngineAPI.IsUsercmdSidecarNegotiated() )
		return;
	if( !pparams || !gEngfuncs.pfnClientCmd || pparams->time < g_vrNextPoseSendTime ||
		!VR_IsFiniteVector( weaponPosition ) || !VR_IsFiniteVector( weaponAngles ) ||
		!VR_IsFiniteVector( weaponVelocity ) || !VR_IsFiniteVector( offhandPosition ) ||
		!VR_IsFiniteVector( offhandAngles ) )
		return;

	commandLength = snprintf( command, sizeof( command ),
		"updatevr %.3f %.3f %.3f %.3f %.3f %.3f %.3f %.3f %.3f %.3f %.3f %.3f %.3f %.3f %.3f",
		weaponPosition.x, weaponPosition.y, weaponPosition.z,
		weaponAngles.x, weaponAngles.y, weaponAngles.z,
		weaponVelocity.x, weaponVelocity.y, weaponVelocity.z,
		offhandPosition.x, offhandPosition.y, offhandPosition.z,
		offhandAngles.x, offhandAngles.y, offhandAngles.z );
	if( commandLength < 0 || commandLength >= (int)sizeof( command ) )
		return;
	command[sizeof( command ) - 1] = '\0';
	gEngfuncs.pfnClientCmd( command );
	g_vrNextPoseSendTime = pparams->time + VR_POSE_SEND_INTERVAL;
}

static int VR_IsThrowable( long weapon )
{
	return weapon == WEAPON_HANDGRENADE || weapon == WEAPON_TRIPMINE ||
		weapon == WEAPON_SATCHEL || weapon == WEAPON_SNARK || weapon == WEAPON_PENGUIN;
}

static int VR_ParseCommandFloat( const char *text, float &value )
{
	char *end;
	double parsed;

	if( !text || !text[0] )
		return 0;
	parsed = strtod( text, &end );
	if( end == text || *end || parsed != parsed || parsed < -FLT_MAX || parsed > FLT_MAX )
		return 0;
	value = (float)parsed;
	return value == value;
}

static int VR_GetWeaponPose( float bodyYaw, float frametime, vr_weapon_pose_t *pose );
void VR_RefreshLaserSight( const float *origin, const float *viewAngles ); /* hl/hl_weapons.cpp */

/* Debug: gun pose vs hand and viewmodel animation, for "gun sits wrong after a weapon switch". */
static void VR_WeaponInfoCommand()
{
	cl_entity_t *view = gEngfuncs.GetViewModel();
	cl_entity_t *player = gEngfuncs.GetLocalPlayer();
	const int dominant = VR_GetDominantHand();
	vr_weapon_pose_t pose;

	gEngfuncs.Con_Printf( "weapon id %ld, model %s, sequence %d, frame %.1f, animtime %.2f (now %.2f)\n",
		g_vrCurrentWeapon, view && view->model ? view->model->name : "-", view ? view->curstate.sequence : -1,
		view ? view->curstate.frame : 0.0f, view ? view->curstate.animtime : 0.0f, gEngfuncs.GetClientTime() );
	if( !player || !VR_GetWeaponPose( player->angles[YAW], 0.0f, &pose ))
	{
		gEngfuncs.Con_Printf( "no VR weapon pose\n" );
		return;
	}
	gEngfuncs.Con_Printf( "aim hand %d offset %.1f %.1f %.1f angles %.1f %.1f %.1f stabilised %d\n", dominant,
		pose.weaponOffset.x, pose.weaponOffset.y, pose.weaponOffset.z,
		pose.weaponAngles.x, pose.weaponAngles.y, pose.weaponAngles.z, pose.stabilised );
	if( view )
		gEngfuncs.Con_Printf( "viewmodel origin %.1f %.1f %.1f angles %.1f %.1f %.1f\n",
			view->origin[0], view->origin[1], view->origin[2], view->angles[0], view->angles[1], view->angles[2] );
}

static void VR_ServerHapticCommand()
{
	float duration, handValue, amplitude;
	int hand;

	if( gEngfuncs.Cmd_Argc() != 4 ||
		!VR_ParseCommandFloat( gEngfuncs.Cmd_Argv( 1 ), duration ) ||
		!VR_ParseCommandFloat( gEngfuncs.Cmd_Argv( 2 ), handValue ) ||
		!VR_ParseCommandFloat( gEngfuncs.Cmd_Argv( 3 ), amplitude ) )
		return;

	hand = (int)handValue;
	if( handValue != (float)hand || hand < -2 || hand >= VR_CLIENT_MAX_HANDS )
		return;
	if( hand == -1 ) hand = VR_GetDominantHand();
	else if( hand == -2 ) hand = VR_GetDominantHand() == VR_CLIENT_HAND_LEFT ?
		VR_CLIENT_HAND_RIGHT : VR_CLIENT_HAND_LEFT;
	VR_Haptic( duration == -1.0f ? -1.0f : VR_Clamp( duration, 0.0f, 2000.0f ), (unsigned int)hand,
		VR_Clamp( amplitude, 0.0f, 1.0f ) );
}

void VR_Init()
{
	if( g_vrInitialised )
		return;

	CVAR_CREATE( "vr_weapon_pitchadjust", "-20", FCVAR_ARCHIVE );
	CVAR_CREATE( "vr_crowbar_pitchadjust", "-25", FCVAR_ARCHIVE );
	CVAR_CREATE( "vr_weapon_recoil", "0", FCVAR_ARCHIVE );
	CVAR_CREATE( "vr_weapon_stabilised", "0", 0 );
	CVAR_CREATE( "vr_scope_engaged", "0", 0 );
	CVAR_CREATE( "vr_scope_stabilise", "1", FCVAR_ARCHIVE );
	CVAR_CREATE( "vr_flashlight_model", "1", FCVAR_ARCHIVE );
	CVAR_CREATE( "vr_hand_model", "1", FCVAR_ARCHIVE );
	CVAR_CREATE( "vr_mirror_weapons", "0", FCVAR_ARCHIVE );
	CVAR_CREATE( "vr_laser_width", "0.3", FCVAR_ARCHIVE ); // gun laser beam width (was a fixed 0.75)
	CVAR_CREATE( "vr_laser_dot_scale", "0.12", FCVAR_ARCHIVE ); // laser dot sprite scale (was a fixed 0.5)
	CVAR_CREATE( "vr_hud_health_rect", "", 0 ); // published by VR_HudBoxPublish, read by the renderer
	CVAR_CREATE( "vr_hud_suit_rect", "", 0 );
	CVAR_CREATE( "vr_hud_ammo_rect", "", 0 );
	CVAR_CREATE( "vr_weapon_backface_culling", "0", FCVAR_ARCHIVE );
	CVAR_CREATE( "vr_headtorch", "0", FCVAR_ARCHIVE );
	CVAR_CREATE( "vr_reversetorch", "0", FCVAR_ARCHIVE );
	CVAR_CREATE( "hand", "0", FCVAR_ARCHIVE );
	gEngfuncs.pfnAddCommand( "vr_haptic", VR_ServerHapticCommand );
	gEngfuncs.pfnAddCommand( "vr_weaponinfo", VR_WeaponInfoCommand );
	g_vrInitialised = 1;
}

void VR_Reset()
{
	VR_ReleaseScopeButton();
	VR_ClearFrame();
}

void VR_UpdateCurrentWeapon( long weapon )
{
	g_vrCurrentWeapon = weapon;
	g_vrCachedWeaponPoseValid = 0;
}

int VR_IsActive()
{
	return ( g_vrFrame.flags & VR_CLIENT_FRAME_ACTIVE ) != 0;
}

static struct
{
	int x0, y0, x1, y1;
	bool valid;
	char published[64];
} g_vrHudBoxes[VR_HUD_BOX_COUNT];
static const char *const g_vrHudBoxCvars[VR_HUD_BOX_COUNT] = { "vr_hud_health_rect", "vr_hud_suit_rect", "vr_hud_ammo_rect" };

int VR_HandHudActive()
{
	return VR_IsActive() && gEngfuncs.pfnGetCvarFloat( "vr_hud_hands" ) != 0.0f;
}

void VR_HudBoxBegin()
{
	for( int i = 0; i < VR_HUD_BOX_COUNT; ++i )
		g_vrHudBoxes[i].valid = false;
}

void VR_HudBoxAdd( int box, int x, int y, int width, int height )
{
	if( box < 0 || box >= VR_HUD_BOX_COUNT || width <= 0 || height <= 0 )
		return;
	if( !g_vrHudBoxes[box].valid )
	{
		g_vrHudBoxes[box].x0 = x;
		g_vrHudBoxes[box].y0 = y;
		g_vrHudBoxes[box].x1 = x + width;
		g_vrHudBoxes[box].y1 = y + height;
		g_vrHudBoxes[box].valid = true;
		return;
	}
	g_vrHudBoxes[box].x0 = Q_min( g_vrHudBoxes[box].x0, x );
	g_vrHudBoxes[box].y0 = Q_min( g_vrHudBoxes[box].y0, y );
	g_vrHudBoxes[box].x1 = Q_max( g_vrHudBoxes[box].x1, x + width );
	g_vrHudBoxes[box].y1 = Q_max( g_vrHudBoxes[box].y1, y + height );
}

/* "x y width height screenwidth screenheight" in HUD pixels, or "" when nothing was drawn (or the hand HUD is
 * off). Only written when it changes. */
void VR_HudBoxPublish()
{
	const int active = VR_HandHudActive();

	for( int i = 0; i < VR_HUD_BOX_COUNT; ++i )
	{
		char value[64] = "";

		if( active && g_vrHudBoxes[i].valid )
			snprintf( value, sizeof( value ), "%d %d %d %d %d %d", g_vrHudBoxes[i].x0, g_vrHudBoxes[i].y0,
				g_vrHudBoxes[i].x1 - g_vrHudBoxes[i].x0, g_vrHudBoxes[i].y1 - g_vrHudBoxes[i].y0, ScreenWidth, ScreenHeight );
		if( strcmp( value, g_vrHudBoxes[i].published ))
		{
			gEngfuncs.Cvar_Set( g_vrHudBoxCvars[i], value );
			snprintf( g_vrHudBoxes[i].published, sizeof( g_vrHudBoxes[i].published ), "%s", value );
		}
	}
}

int VR_GetDominantHand()
{
	return CVAR_GET_FLOAT( "hand" ) >= 0.5f ? VR_CLIENT_HAND_LEFT : VR_CLIENT_HAND_RIGHT;
}

int VR_IsWeaponStabilised()
{
	return VR_IsActive() && gEngfuncs.pfnGetCvarFloat( "vr_weapon_stabilised" ) != 0.0f;
}

int VR_ShouldApplyWeaponRecoil()
{
	return !VR_IsActive() || CVAR_GET_FLOAT( "vr_weapon_recoil" ) != 0.0f || g_vrScopeEngaged;
}

void VR_Haptic( float durationMilliseconds, unsigned int hand, float amplitude )
{
	if( VR_IsActive() && g_vrEngineAPI.Haptic && hand < VR_CLIENT_MAX_HANDS )
		g_vrEngineAPI.Haptic( hand, durationMilliseconds / 1000.0f, 0.0f, amplitude );
}

static int VR_IsMeleeWeapon( long weapon )
{
	return weapon == WEAPON_CROWBAR || weapon == WEAPON_KNIFE || weapon == WEAPON_PIPEWRENCH;
}

static int VR_IsScopeWeapon( long weapon )
{
	return weapon == WEAPON_CROSSBOW || weapon == WEAPON_SNIPERRIFLE;
}

static void VR_UpdateScope( float bodyYaw, const Vector &weaponOffset, int stabilised )
{
	Vector headOffset;
	Vector unusedAngles;
	float minimumScale;
	int closeToHead;
	int scopeRequested;
	int scopeActive;

	if( !VR_IsScopeWeapon( g_vrCurrentWeapon ) )
	{
		VR_ReleaseScopeButton();
		g_vrScopeEngaged = 0;
		g_vrScopeAnglesValid = 0;
		gEngfuncs.Cvar_SetValue( "vr_scope_engaged", 0.0f );
		return;
	}

	if( g_vrFrame.flags & VR_CLIENT_FRAME_HEAD_VALID )
		VR_TransformPose( g_vrFrame.head, bodyYaw, headOffset, unusedAngles );
	else
		headOffset = Vector( 0.0f, 0.0f, 0.0f );

	minimumScale = g_vrFrame.world_scale > 0.0f ? g_vrFrame.world_scale : 40.0f;
	closeToHead = ( weaponOffset - headOffset ).Length() < minimumScale * 0.28f;
	scopeRequested = stabilised && closeToHead;
	if( scopeRequested && !g_vrScopeButtonDown && gEngfuncs.pfnClientCmd )
	{
		gEngfuncs.pfnClientCmd( "+alt1" );
		g_vrScopeButtonDown = 1;
	}
	else if( !scopeRequested )
	{
		VR_ReleaseScopeButton();
	}

	/* The server's normal FOV message remains authoritative for actual zoom. */
	scopeActive = gHUD.m_iFOV > 0 &&
		gHUD.m_iFOV < (int)CVAR_GET_FLOAT( "default_fov" );
	g_vrScopeEngaged = scopeActive;
	if( !scopeActive )
		g_vrScopeAnglesValid = 0;
	gEngfuncs.Cvar_SetValue( "vr_scope_engaged", scopeActive ? 1.0f : 0.0f );
}

static int VR_ResolveWeaponPose( float bodyYaw, float frametime, vr_weapon_pose_t *pose )
{
	const int dominant = VR_GetDominantHand();
	const int offhand = dominant == VR_CLIENT_HAND_LEFT ? VR_CLIENT_HAND_RIGHT : VR_CLIENT_HAND_LEFT;
	const vr_client_hand_t *weapon = &g_vrFrame.hands[dominant];
	const vr_client_hand_t *support = &g_vrFrame.hands[offhand];
	Vector separation;
	float separationMinimum, separationMaximum, separationLength;

	if( !pose || !VR_IsActive() || !( weapon->flags & VR_CLIENT_HAND_AIM_VALID ) )
	{
		gEngfuncs.Cvar_SetValue( "vr_weapon_stabilised", 0.0f );
		VR_ReleaseScopeButton();
		g_vrScopeEngaged = 0;
		g_vrScopeAnglesValid = 0;
		gEngfuncs.Cvar_SetValue( "vr_scope_engaged", 0.0f );
		return 0;
	}

	VR_TransformPose( weapon->aim, bodyYaw, pose->weaponOffset, pose->weaponAngles );
	pose->supportOffset = pose->weaponOffset;
	pose->supportAngles = pose->weaponAngles;
	if( support->flags & VR_CLIENT_HAND_AIM_VALID )
		VR_TransformPose( support->aim, bodyYaw, pose->supportOffset, pose->supportAngles );

	separation = pose->supportOffset - pose->weaponOffset;
	separationMinimum = g_vrFrame.world_scale > 0.0f ? g_vrFrame.world_scale * 0.15f : 1.0f;
	separationMaximum = g_vrFrame.world_scale > 0.0f ? g_vrFrame.world_scale * 0.50f : 20.0f;
	separationLength = separation.Length();
	pose->stabilised = ( support->flags & VR_CLIENT_HAND_AIM_VALID ) &&
		( support->flags & VR_CLIENT_HAND_SQUEEZE_PRESSED ) &&
		separationLength > separationMinimum && separationLength < separationMaximum;
	gEngfuncs.Cvar_SetValue( "vr_weapon_stabilised", pose->stabilised ? 1.0f : 0.0f );
	if( pose->stabilised )
	{
		/* VectorAngles returns model-convention pitch (positive = up); these are view angles */
		VectorAngles( separation, pose->weaponAngles );
		pose->weaponAngles[PITCH] = -pose->weaponAngles[PITCH];
	}

	/* The pitch adjust cvars use Lambda1VR's model convention (negative = tilt
	 * down); weaponAngles are view angles here (positive = down), hence -=. */
	/* The adjust corrects the one-handed grip (the controller's aim ray points up relative to a
	 * pistol grip). Two-handed, the gun follows the line between the hands, which already is the
	 * barrel direction - applying it there aimed the gun ~20 degrees low. */
	{
		float adjust = 0.0f;
		if( pose->stabilised )
			;
		else if( VR_IsMeleeWeapon( g_vrCurrentWeapon ))
			adjust = CVAR_GET_FLOAT( "vr_crowbar_pitchadjust" );
		else if( !VR_IsThrowable( g_vrCurrentWeapon ) )
			adjust = CVAR_GET_FLOAT( "vr_weapon_pitchadjust" );
		/* negative adjust = nose down; applied about the controller's own right axis */
		if( adjust != 0.0f )
			VR_TransformPoseTilted( weapon->aim, bodyYaw, -adjust, pose->weaponAngles );
	}

	VR_UpdateScope( bodyYaw, pose->weaponOffset, pose->stabilised );
	if( g_vrScopeEngaged && CVAR_GET_FLOAT( "vr_scope_stabilise" ) != 0.0f )
	{
		if( g_vrScopeAnglesValid )
			pose->weaponAngles = VR_LerpAngles( g_vrScopeAngles, pose->weaponAngles, VR_Clamp( frametime * 8.0f, 0.0f, 1.0f ) );
		g_vrScopeAngles = pose->weaponAngles;
		g_vrScopeAnglesValid = 1;
	}

	pose->weaponVelocity = VR_RotateYaw( weapon->linear_velocity, bodyYaw );
	return VR_IsFiniteVector( pose->weaponOffset ) && VR_IsFiniteVector( pose->weaponAngles ) &&
		VR_IsFiniteVector( pose->weaponVelocity ) && VR_IsFiniteVector( pose->supportOffset ) &&
		VR_IsFiniteVector( pose->supportAngles );
}

static int VR_GetWeaponPose( float bodyYaw, float frametime, vr_weapon_pose_t *pose )
{
	if( g_vrCachedWeaponPoseValid && g_vrCachedPoseFrameId == g_vrFrame.frame_id &&
		g_vrCachedPoseYaw == bodyYaw )
	{
		*pose = g_vrCachedWeaponPose;
		return 1;
	}
	if( !VR_ResolveWeaponPose( bodyYaw, frametime, pose ) )
		return 0;
	g_vrCachedWeaponPose = *pose;
	g_vrCachedPoseFrameId = g_vrFrame.frame_id;
	g_vrCachedPoseYaw = bodyYaw;
	g_vrCachedWeaponPoseValid = 1;
	return 1;
}

static void VR_AddTorchModel( const Vector &origin, const Vector &angles )
{
	if( !gHUD.m_Flash.HasFlashlight() || !CVAR_GET_FLOAT( "vr_flashlight_model" ) ||
		!gEngfuncs.CL_LoadModel || !gEngfuncs.CL_CreateVisibleEntity )
		return;

	if( !g_vrTorchModel )
		g_vrTorchModel = gEngfuncs.CL_LoadModel( "models/v_torch.mdl", &g_vrTorchModelIndex );
	if( !g_vrTorchModel )
		return;

	memset( &g_vrTorchEntity, 0, sizeof( g_vrTorchEntity ) );
	g_vrTorchEntity.model = g_vrTorchModel;
	g_vrTorchEntity.curstate.modelindex = g_vrTorchModelIndex;
	g_vrTorchEntity.curstate.movetype = MOVETYPE_NONE;
	g_vrTorchEntity.curstate.rendermode = kRenderNormal;
	VectorCopy( origin, g_vrTorchEntity.origin );
	VectorCopy( origin, g_vrTorchEntity.curstate.origin );
	VectorCopy( VR_ModelAngles( angles ), g_vrTorchEntity.angles );
	VectorCopy( VR_ModelAngles( angles ), g_vrTorchEntity.curstate.angles );
	gEngfuncs.CL_CreateVisibleEntity( ET_NORMAL, &g_vrTorchEntity );
}

static void VR_AddHandModel( int hand, const Vector &origin, const Vector &angles )
{
	cl_entity_t *entity;
	Vector handOrigin;
	vec3_t forward;

	if( hand < 0 || hand >= VR_CLIENT_MAX_HANDS || !CVAR_GET_FLOAT( "vr_hand_model" ) ||
		!gEngfuncs.CL_LoadModel || !gEngfuncs.CL_CreateVisibleEntity || !gEngfuncs.pfnAngleVectors )
		return;

	if( !g_vrHandModels[hand] )
		g_vrHandModels[hand] = gEngfuncs.CL_LoadModel( "models/v_hand.mdl", &g_vrHandModelIndices[hand] );
	if( !g_vrHandModels[hand] )
		return;

	gEngfuncs.pfnAngleVectors( angles, forward, NULL, NULL );
	handOrigin = origin - Vector( forward ) * 5.0f;
	entity = &g_vrHandEntities[hand];
	memset( entity, 0, sizeof( *entity ) );
	entity->model = g_vrHandModels[hand];
	entity->curstate.modelindex = g_vrHandModelIndices[hand];
	entity->curstate.movetype = MOVETYPE_NONE;
	entity->curstate.rendermode = kRenderGlow;
	entity->curstate.renderfx = kRenderFxNoDissipation;
	entity->curstate.renderamt = 192;
	VectorCopy( handOrigin, entity->origin );
	VectorCopy( handOrigin, entity->curstate.origin );
	VectorCopy( VR_ModelAngles( angles ), entity->angles );
	VectorCopy( VR_ModelAngles( angles ), entity->curstate.angles );
	gEngfuncs.CL_CreateVisibleEntity( ET_NORMAL, entity );
}

/* v_hand.mdl is a right hand only; the studio renderer mirrors the left one. */
int VR_IsMirroredHandEntity( const struct cl_entity_s *entity )
{
	return entity && entity == &g_vrHandEntities[VR_CLIENT_HAND_LEFT];
}

static int VR_CanRenderPresentation( const ref_params_s *pparams )
{
	return pparams && !g_vrScopeEngaged && !pparams->intermission && !pparams->spectator && !pparams->onlyClientDraw &&
		pparams->health > 0 && pparams->viewentity == pparams->playernum + 1 &&
		!gEngfuncs.IsSpectateOnly();
}

void VR_UpdateViewmodel( struct ref_params_s *pparams )
{
	cl_entity_t *viewmodel;
	const int dominant = VR_GetDominantHand();
	const int offhand = dominant == VR_CLIENT_HAND_LEFT ? VR_CLIENT_HAND_RIGHT : VR_CLIENT_HAND_LEFT;
	const vr_client_hand_t *support = &g_vrFrame.hands[offhand];
	vr_weapon_pose_t pose;

	if( !pparams || !VR_IsActive() )
	{
		VR_ReleaseScopeButton();
		gEngfuncs.Cvar_SetValue( "vr_scope_engaged", 0.0f );
		return;
	}

	viewmodel = gEngfuncs.GetViewModel();
	if( !viewmodel )
	{
		gEngfuncs.Cvar_SetValue( "vr_weapon_stabilised", 0.0f );
		VR_ReleaseScopeButton();
		g_vrScopeEngaged = 0;
		g_vrScopeAnglesValid = 0;
		gEngfuncs.Cvar_SetValue( "vr_scope_engaged", 0.0f );
		return;
	}
	if( !VR_GetWeaponPose( pparams->cl_viewangles[YAW], pparams->frametime, &pose ) )
	{
		return;
	}

	/* Keep the server pose fresh when off-hand tracking is unavailable. */
	/* Server contract: weapon angles in model convention (GetWeaponViewAngles
	 * negates pitch), off-hand angles in view convention (used as-is). */
	VR_SendPoseUpdate( pparams, Vector( pparams->vieworg ) + pose.weaponOffset, VR_ModelAngles( pose.weaponAngles ), pose.weaponVelocity,
		Vector( pparams->vieworg ) + pose.supportOffset, pose.supportAngles );

	pose.weaponOffset = Vector( pparams->vieworg ) + pose.weaponOffset;
	VectorCopy( pose.weaponOffset, viewmodel->origin );
	VectorCopy( pose.weaponOffset, viewmodel->curstate.origin );
	VectorCopy( pose.weaponOffset, viewmodel->latched.prevorigin );
	VectorCopy( VR_ModelAngles( pose.weaponAngles ), viewmodel->angles );
	VectorCopy( VR_ModelAngles( pose.weaponAngles ), viewmodel->curstate.angles );
	VectorCopy( VR_ModelAngles( pose.weaponAngles ), viewmodel->latched.prevangles );
	VR_RefreshLaserSight( pose.weaponOffset, pose.weaponAngles ); /* keep the laser on the gun this frame */
	VectorCopy( pose.weaponVelocity, viewmodel->curstate.velocity );

	if( VR_CanRenderPresentation( pparams ) )
	{
		const int hasFlashlight = gHUD.m_Flash.HasFlashlight();

		/* Match Lambda1VR: the off-hand is empty only when it is neither
		 * carrying the flashlight nor stabilising a weapon. */
		if( ( support->flags & VR_CLIENT_HAND_AIM_VALID ) && !hasFlashlight && !pose.stabilised )
			VR_AddHandModel( offhand, Vector( pparams->vieworg ) + pose.supportOffset, pose.supportAngles );
		if( !viewmodel->model )
			VR_AddHandModel( dominant, pose.weaponOffset, pose.weaponAngles );
		if( ( support->flags & VR_CLIENT_HAND_AIM_VALID ) && hasFlashlight && !pose.stabilised )
		{
			if( CVAR_GET_FLOAT( "vr_headtorch" ) == 0.0f )
			{
				if( CVAR_GET_FLOAT( "vr_reversetorch" ) != 0.0f )
				{
					vec3_t forward;
					gEngfuncs.pfnAngleVectors( pose.supportAngles, forward, NULL, NULL );
					VectorAngles( -Vector( forward ), pose.supportAngles );
					pose.supportAngles[PITCH] = -pose.supportAngles[PITCH]; /* model -> view convention */
				}
				VR_AddTorchModel( Vector( pparams->vieworg ) + pose.supportOffset, pose.supportAngles );
			}
		}
	}
}

static int32_t VR_CLIENT_CALL VR_BuildUsercmdSidecar( const vr_client_usercmd_t *cmd,
	vr_usercmd_sidecar_t *sample )
{
	vr_weapon_pose_t pose;
	int index;

	if( !cmd || !sample || cmd->struct_size < VR_CLIENT_USERCMD_MIN_SIZE )
		return 0;
	memset( sample, 0, sizeof( *sample ) );
	if( !VR_GetWeaponPose( cmd->viewangles[YAW], cmd->frametime, &pose ) )
		return 0;
	pose.weaponAngles = VR_WrapAngles( VR_ModelAngles( pose.weaponAngles )); /* see VR_UpdateViewmodel */
	pose.supportAngles = VR_WrapAngles( pose.supportAngles );
	for( index = 0; index < 3; ++index )
	{
		sample->weapon_position[index] = VR_UsercmdSidecarQuantize( pose.weaponOffset[index], 8.0f );
		sample->weapon_angles[index] = VR_UsercmdSidecarQuantize( pose.weaponAngles[index], 128.0f );
		sample->weapon_velocity[index] = VR_UsercmdSidecarQuantize( pose.weaponVelocity[index], 8.0f );
		sample->offhand_position[index] = VR_UsercmdSidecarQuantize( pose.supportOffset[index], 8.0f );
		sample->offhand_angles[index] = VR_UsercmdSidecarQuantize( pose.supportAngles[index], 128.0f );
	}
	sample->version = VR_USERCMD_SIDECAR_VERSION;
	sample->flags = VR_USERCMD_SIDECAR_POSE_VALID;
	return 1;
}

extern "C" int VR_CLIENT_CALL DLLEXPORT HUD_GetVRClientAPI( const vr_client_engine_api_t *enginefuncs,
	vr_client_api_t *callbacks )
{
	uint32_t writableSize;
	uint32_t engineSize;

	if( !enginefuncs || !callbacks || enginefuncs->version != VR_CLIENT_API_VERSION ||
		enginefuncs->struct_size < VR_CLIENT_ENGINE_API_MIN_SIZE )
		return 0;
	writableSize = callbacks->struct_size ? callbacks->struct_size : VR_CLIENT_API_MIN_SIZE;
	if( writableSize < VR_CLIENT_API_MIN_SIZE )
		return 0;
	if( writableSize > sizeof( *callbacks ) )
		writableSize = sizeof( *callbacks );

	memset( &g_vrEngineAPI, 0, sizeof( g_vrEngineAPI ) );
	engineSize = enginefuncs->struct_size;
	if( engineSize > sizeof( g_vrEngineAPI ) )
		engineSize = sizeof( g_vrEngineAPI );
	memcpy( &g_vrEngineAPI, enginefuncs, engineSize );
	memset( callbacks, 0, writableSize );
	callbacks->version = VR_CLIENT_API_VERSION;
	callbacks->struct_size = writableSize;
	callbacks->Frame = VR_ClientFrame;
	callbacks->Shutdown = VR_ClientShutdown;
	if( writableSize >= VR_CLIENT_API_SIDECAR_SIZE )
		callbacks->BuildUsercmdSidecar = VR_BuildUsercmdSidecar;
	return 1;
}
