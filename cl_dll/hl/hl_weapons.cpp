/***
*
*	Copyright (c) 1996-2002, Valve LLC. All rights reserved.
*	
*	This product contains software technology licensed from Id 
*	Software, Inc. ("Id Technology").  Id Technology (c) 1996 Id Software, Inc. 
*	All Rights Reserved.
*
*   Use, distribution, and modification of this source code and/or resulting
*   object code is restricted to non-commercial enhancements to products from
*   Valve LLC.  All other use, distribution, or modification is prohibited
*   without written permission from Valve LLC.
*
****/

#include "extdll.h"
#include "util.h"
#include "cbase.h"
#include "monsters.h"
#include "weapons.h"
#include "nodes.h"
#include "player.h"

#include "usercmd.h"
#include "entity_state.h"
#include "demo_api.h"
#include "pm_defs.h"
#include "event_api.h"
#include "r_efx.h"

#include "../hud_iface.h"
#include "../com_weapons.h"
#include "../demo.h"
#include "../vr_client.h"

extern globalvars_t *gpGlobals;
extern int g_iUser1;

// Pool of client side entities/entvars_t
static entvars_t ev[MAX_WEAPONS];
static int num_ents = 0;

// The entity we'll use to represent the local client
static CBasePlayer player;

// Local version of game .dll global variables ( time, etc. )
static globalvars_t Globals; 

static CBasePlayerWeapon *g_pWpns[MAX_WEAPONS];

float g_flApplyVel = 0.0;
int g_irunninggausspred = 0;

vec3_t previousorigin;

// HLDM Weapon placeholder entities.
CGlock g_Glock;
CCrowbar g_Crowbar;
CPython g_Python;
CMP5 g_Mp5;
CCrossbow g_Crossbow;
CShotgun g_Shotgun;
CRpg g_Rpg;
CGauss g_Gauss;
CEgon g_Egon;
CHgun g_HGun;
CHandGrenade g_HandGren;
CSatchel g_Satchel;
CTripmine g_Tripmine;
CSqueak g_Snark;

/*
======================
AlertMessage

Print debug messages to console
======================
*/
void AlertMessage( ALERT_TYPE atype, const char *szFmt, ... )
{
	va_list argptr;
	static char string[1024];

	va_start( argptr, szFmt );
	vsprintf( string, szFmt, argptr );
	va_end( argptr );

	gEngfuncs.Con_Printf( "cl:  " );
	gEngfuncs.Con_Printf( string );
}

//Returns if it's multiplayer.
//Mostly used by the client side weapons.
bool bIsMultiplayer( void )
{
	return gEngfuncs.GetMaxClients() == 1 ? 0 : 1;
}

//Just loads a v_ model.
void LoadVModel( const char *szViewModel, CBasePlayer *m_pPlayer )
{
	gEngfuncs.CL_LoadModel( szViewModel, &m_pPlayer->pev->viewmodel );
}

/*
=====================
HUD_PrepEntity

Links the raw entity to an entvars_s holder.  If a player is passed in as the owner, then
we set up the m_pPlayer field.
=====================
*/
void HUD_PrepEntity( CBaseEntity *pEntity, CBasePlayer *pWeaponOwner )
{
	memset( &ev[num_ents], 0, sizeof(entvars_t) );
	pEntity->pev = &ev[num_ents++];

	pEntity->Precache();
	pEntity->Spawn();

	if( pWeaponOwner )
	{
		ItemInfo info;

		( (CBasePlayerWeapon *)pEntity )->m_pPlayer = pWeaponOwner;

		( (CBasePlayerWeapon *)pEntity )->GetItemInfo( &info );

		g_pWpns[info.iId] = (CBasePlayerWeapon *)pEntity;
	}
}

/*
=====================
CBaseEntity::Killed

If weapons code "kills" an entity, just set its effects to EF_NODRAW
=====================
*/
void CBaseEntity::Killed( entvars_t *pevAttacker, int iGib )
{
	pev->effects |= EF_NODRAW;
}

/*
=====================
CBasePlayerWeapon::DefaultReload
=====================
*/
BOOL CBasePlayerWeapon::DefaultReload( int iClipSize, int iAnim, float fDelay, int body )
{
	if( m_pPlayer->m_rgAmmo[m_iPrimaryAmmoType] <= 0 )
		return FALSE;

	int j = Q_min( iClipSize - m_iClip, m_pPlayer->m_rgAmmo[m_iPrimaryAmmoType] );

	if( j == 0 )
		return FALSE;

	m_pPlayer->m_flNextAttack = UTIL_WeaponTimeBase() + fDelay;

	//!!UNDONE -- reload sound goes here !!!
	SendWeaponAnim( iAnim, UseDecrement(), body );

	m_fInReload = TRUE;

	m_flTimeWeaponIdle = UTIL_WeaponTimeBase() + 3.0f;
	return TRUE;
}

/*
=====================
CBasePlayerWeapon::CanDeploy
=====================
*/
BOOL CBasePlayerWeapon::CanDeploy( void ) 
{
	BOOL bHasAmmo = 0;

	if( !pszAmmo1() )
	{
		// this weapon doesn't use ammo, can always deploy.
		return TRUE;
	}

	if( pszAmmo1() )
	{
		bHasAmmo |= ( m_pPlayer->m_rgAmmo[m_iPrimaryAmmoType] != 0 );
	}
	if( pszAmmo2() )
	{
		bHasAmmo |= ( m_pPlayer->m_rgAmmo[m_iSecondaryAmmoType] != 0 );
	}
	if( m_iClip > 0 )
	{
		bHasAmmo |= 1;
	}
	if( !bHasAmmo )
	{
		return FALSE;
	}

	return TRUE;
}

/*
=====================
CBasePlayerWeapon::DefaultDeploy

=====================
*/
BOOL CBasePlayerWeapon::DefaultDeploy( const char *szViewModel, const char *szWeaponModel, int iAnim, const char *szAnimExt, int skiplocal, int body )
{
	if( !CanDeploy() )
		return FALSE;

	gEngfuncs.CL_LoadModel( szViewModel, &m_pPlayer->pev->viewmodel );

	SendWeaponAnim( iAnim, skiplocal, body );

	g_irunninggausspred = false;
	m_pPlayer->m_flNextAttack = 0.5f;
	m_flTimeWeaponIdle = 1.0f;
	return TRUE;
}

/*
=====================
CBasePlayerWeapon::PlayEmptySound

=====================
*/
BOOL CBasePlayerWeapon::PlayEmptySound( void )
{
	if( m_iPlayEmptySound )
	{
		HUD_PlaySound( "weapons/357_cock1.wav", 0.8f );
		m_iPlayEmptySound = 0;
		return 0;
	}
	return 0;
}

/*
=====================
CBasePlayerWeapon::ResetEmptySound

=====================
*/
void CBasePlayerWeapon::ResetEmptySound( void )
{
	m_iPlayEmptySound = 1;
}

/*
=====================
CBasePlayerWeapon::Holster

Put away weapon
=====================
*/
void CBasePlayerWeapon::Holster( int skiplocal /* = 0 */ )
{ 
	KillLaser();

	m_fInReload = FALSE; // cancel any reload in progress.
	g_irunninggausspred = false;
	m_pPlayer->pev->viewmodel = 0; 
}

// vr_lasersight is an archive cvar local to each client.  Keeping these effects
// client-side means a player's choice cannot affect any other player on a server.
static BEAM *g_pLaserSightBeam;
static TEMPENTITY *g_pLaserSightDot;
static float g_flLaserSightExpiry;
static float g_flLaserSightLastUpdate;

static void TraceLaserSight( const Vector &vecSrc, const Vector &vecEnd, pmtrace_t *pTrace )
{
	cl_entity_t *pLocalPlayer = gEngfuncs.GetLocalPlayer();
	Vector traceSrc = vecSrc;
	Vector traceEnd = vecEnd;

	memset( pTrace, 0, sizeof( *pTrace ) );
	pTrace->fraction = 1.0f;
	pTrace->endpos[0] = vecEnd.x;
	pTrace->endpos[1] = vecEnd.y;
	pTrace->endpos[2] = vecEnd.z;
	if( !pLocalPlayer )
		return;

	gEngfuncs.pEventAPI->EV_SetUpPlayerPrediction( false, true );
	gEngfuncs.pEventAPI->EV_PushPMStates();
	gEngfuncs.pEventAPI->EV_SetSolidPlayers( pLocalPlayer->index - 1 );
	gEngfuncs.pEventAPI->EV_SetTraceHull( 2 );
	gEngfuncs.pEventAPI->EV_PlayerTrace( traceSrc, traceEnd, PM_NORMAL, -1, pTrace );
	gEngfuncs.pEventAPI->EV_PopPMStates();
}

void CBasePlayerWeapon::KillLaser( void )
{
	const float flTime = gEngfuncs.GetClientTime();

	// Do not dereference an expired (or pre-map-change) temp-effect pointer.
	if( flTime >= g_flLaserSightLastUpdate && flTime < g_flLaserSightExpiry )
	{
		if( g_pLaserSightBeam )
			g_pLaserSightBeam->die = 0.0f;
		if( g_pLaserSightDot )
			g_pLaserSightDot->die = 0.0f;
	}

	g_pLaserSightBeam = NULL;
	g_pLaserSightDot = NULL;
	g_flLaserSightExpiry = 0.0f;
	g_flLaserSightLastUpdate = flTime;
}

// Laser sight geometry from a gun pose (view-convention angles): where the beam starts (just past the
// muzzle) and where it hits. The trace starts at the gun so the dot cannot pass through a wall.
static void VR_LaserGeometry( const Vector &vecSrc, const Vector &vecAngles, int iMode, Vector &vecBeamSrc, Vector &vecHit )
{
	// UTIL_MakeVectors was an empty stub on the client: use the engine directly
	Vector vecForward;
	gEngfuncs.pfnAngleVectors( vecAngles, vecForward, NULL, NULL );
	Vector vecEnd = vecSrc + vecForward * ( iMode == 1 ? 2048.0f : 8192.0f );
	pmtrace_t trace;
	TraceLaserSight( vecSrc, vecEnd, &trace );
	vecHit = trace.endpos;
	vecBeamSrc = vecSrc + vecForward * Q_min( 10.0f, ( vecHit - vecSrc ).Length() );
}

static float VR_LaserDotScale( void )
{
	return Q_max( 0.02f, gEngfuncs.pfnGetCvarFloat( "vr_laser_dot_scale" ));
}

// Called from VR_UpdateViewmodel with the gun pose of the frame being rendered. MakeLaser runs in
// weapon prediction, earlier in the frame and with the previous frame's gun pose, so on its own the
// beam lagged behind the gun when moving it, strafing or falling.
void VR_RefreshLaserSight( const float *origin, const float *viewAngles )
{
	const int iMode = (int)gEngfuncs.pfnGetCvarFloat( "vr_lasersight" );
	const float flTime = gEngfuncs.GetClientTime();
	Vector vecBeamSrc, vecHit;

	if( iMode < 1 || iMode > 2 || !( flTime >= g_flLaserSightLastUpdate && flTime < g_flLaserSightExpiry ))
		return;
	if( !g_pLaserSightBeam && !g_pLaserSightDot )
		return;
	VR_LaserGeometry( Vector( origin[0], origin[1], origin[2] ), Vector( viewAngles[0], viewAngles[1], viewAngles[2] ), iMode, vecBeamSrc, vecHit );
	if( g_pLaserSightBeam )
	{
		g_pLaserSightBeam->source = vecBeamSrc;
		g_pLaserSightBeam->target = vecHit;
		g_pLaserSightBeam->delta = g_pLaserSightBeam->target - g_pLaserSightBeam->source;
	}
	if( g_pLaserSightDot )
		g_pLaserSightDot->entity.origin = vecHit;
}

void CBasePlayerWeapon::MakeLaser( void )
{
	// client side: g_engfuncs is a stub table here, CVAR_GET_FLOAT would call NULL
	const int iMode = (int)gEngfuncs.pfnGetCvarFloat( "vr_lasersight" );
	const float flTime = gEngfuncs.GetClientTime();

	// The RPG owns its guidance dot (a server laser_spot the rockets steer to): it only gets the beam,
	// and only while that spot is on. The spot alone is a fixed-size sprite that shrinks to a speck at
	// range and lags the gun (server-updated), so far targets (Xen's Nihilanth crystals) were hard to aim at.
	const BOOL bRpg = m_iId == WEAPON_RPG;
	// No laser while reloading: it follows the hand, not the animated gun, so it pointed off on its own
	// while the reload animation swung the gun about.
	if( !m_pPlayer || iMode < 1 || iMode > 2 || m_fInReload || ( bRpg && ( iMode != 1 || !( (CRpg *)this )->m_fSpotActive )))
	{
		KillLaser();
		return;
	}

	// Effects must only be emitted once for a predicted command.
	if( !g_runfuncs )
		return;

	Vector vecBeamSrc, vecHit;
	VR_LaserGeometry( m_pPlayer->GetWeaponPosition(), m_pPlayer->GetWeaponViewAngles(), iMode, vecBeamSrc, vecHit );

	const int iBeamModel = gEngfuncs.pEventAPI->EV_FindModelIndex( "sprites/laserbeam.spr" );
	const int iDotModel = gEngfuncs.pEventAPI->EV_FindModelIndex( "sprites/laserdot.spr" );
	if( iDotModel <= 0 || ( iMode == 1 && iBeamModel <= 0 ))
	{
		KillLaser();
		return;
	}

	const float flLifetime = 0.1f;
	const BOOL bEffectIsCurrent = flTime >= g_flLaserSightLastUpdate && flTime < g_flLaserSightExpiry;
	const float flBeamWidth = Q_max( 0.05f, gEngfuncs.pfnGetCvarFloat( "vr_laser_width" ));

	// mode 1: beam with a dot where it hits; mode 2: dot only
	if( iMode == 1 )
	{
		if( !g_pLaserSightBeam || !bEffectIsCurrent )
		{
			g_pLaserSightBeam = gEngfuncs.pEfxAPI->R_BeamPoints( vecBeamSrc, vecHit,
				iBeamModel, flLifetime, flBeamWidth, 0.0f, 96.0f / 255.0f, 255.0f, 0, 0.0f,
				214.0f / 255.0f, 34.0f / 255.0f, 34.0f / 255.0f );
		}
		else
		{
			g_pLaserSightBeam->source = vecBeamSrc;
			g_pLaserSightBeam->target = vecHit;
			// point beams only compute delta at creation (gl_beams.c); without this the far end stays put
			g_pLaserSightBeam->delta = g_pLaserSightBeam->target - g_pLaserSightBeam->source;
			g_pLaserSightBeam->width = flBeamWidth;
			g_pLaserSightBeam->die = flTime + flLifetime;
		}
	}
	else
	{
		if( g_pLaserSightBeam && bEffectIsCurrent )
			g_pLaserSightBeam->die = 0.0f;
		g_pLaserSightBeam = NULL;
	}

	if( bRpg )
		g_pLaserSightDot = NULL; // the stock laser_spot is the RPG's dot
	else if( !g_pLaserSightDot || !bEffectIsCurrent )
	{
		Vector vecZero;
		g_pLaserSightDot = gEngfuncs.pEfxAPI->R_TempSprite( vecHit, vecZero,
			VR_LaserDotScale(), iDotModel, kRenderGlow, kRenderFxNoDissipation, 1.0f, flLifetime,
			FTENT_SPRCYCLE | FTENT_PERSIST );
	}
	else
	{
		g_pLaserSightDot->entity.origin = vecHit;
		g_pLaserSightDot->entity.curstate.scale = VR_LaserDotScale();
		g_pLaserSightDot->die = flTime + flLifetime;
	}

	g_flLaserSightExpiry = flTime + flLifetime;
	g_flLaserSightLastUpdate = flTime;
}

/*
=====================
CBasePlayerWeapon::SendWeaponAnim

Animate weapon model
=====================
*/
void CBasePlayerWeapon::SendWeaponAnim( int iAnim, int skiplocal, int body )
{
	m_pPlayer->pev->weaponanim = iAnim;

	HUD_SendWeaponAnim( iAnim, body, 0 );
}

/*
=====================
CBaseEntity::FireBulletsPlayer

Only produces random numbers to match the server ones.
=====================
*/
Vector CBaseEntity::FireBulletsPlayer ( ULONG cShots, Vector vecSrc, Vector vecDirShooting, Vector vecSpread, float flDistance, int iBulletType, int iTracerFreq, int iDamage, entvars_t *pevAttacker, int shared_rand )
{
	float x = 0.0f, y = 0.0f, z;

	for( ULONG iShot = 1; iShot <= cShots; iShot++ )
	{
		if( pevAttacker == NULL )
		{
			// get circular gaussian spread
			do {
					x = RANDOM_FLOAT( -0.5f, 0.5f ) + RANDOM_FLOAT( -0.5f, 0.5f );
					y = RANDOM_FLOAT( -0.5f, 0.5f ) + RANDOM_FLOAT( -0.5f, 0.5f );
					z = x * x + y * y;
			} while( z > 1 );
		}
		else
		{
			//Use player's random seed.
			// get circular gaussian spread
			x = UTIL_SharedRandomFloat( shared_rand + iShot, -0.5f, 0.5f ) + UTIL_SharedRandomFloat( shared_rand + ( 1 + iShot ) , -0.5f, 0.5f );
			y = UTIL_SharedRandomFloat( shared_rand + ( 2 + iShot ), -0.5f, 0.5f ) + UTIL_SharedRandomFloat( shared_rand + ( 3 + iShot ), -0.5f, 0.5f );
			// z = x * x + y * y;
		}			
	}

	return Vector( x * vecSpread.x, y * vecSpread.y, 0.0f );
}

/*
=====================
CBasePlayerWeapon::ItemPostFrame

Handles weapon firing, reloading, etc.
=====================
*/
void CBasePlayerWeapon::ItemPostFrame( void )
{
	if( ( m_fInReload ) && ( m_pPlayer->m_flNextAttack <= 0.0f ) )
	{
#if 1
		// complete the reload. 
		ItemInfo itemInfo;
		memset( &itemInfo, 0, sizeof( itemInfo ) );
		GetItemInfo( &itemInfo );

		int j = Q_min( itemInfo.iMaxClip - m_iClip, m_pPlayer->m_rgAmmo[m_iPrimaryAmmoType] );

		// Add them to the clip
		m_iClip += j;
		m_pPlayer->m_rgAmmo[m_iPrimaryAmmoType] -= j;
#else
		m_iClip += 10;
#endif
		m_fInReload = FALSE;
	}

	if( ( m_pPlayer->pev->button & IN_ATTACK2 ) && ( m_flNextSecondaryAttack <= 0.0f ) )
	{
		if( pszAmmo2() && !m_pPlayer->m_rgAmmo[SecondaryAmmoIndex()] )
		{
			m_fFireOnEmpty = TRUE;
		}

		SecondaryAttack();
		m_pPlayer->pev->button &= ~IN_ATTACK2;
	}
	else if( ( m_pPlayer->pev->button & IN_ATTACK ) && ( m_flNextPrimaryAttack <= 0.0f ) )
	{
		if( ( m_iClip == 0 && pszAmmo1() ) || ( iMaxClip() == -1 && !m_pPlayer->m_rgAmmo[PrimaryAmmoIndex()] ) )
		{
			m_fFireOnEmpty = TRUE;
		}

		PrimaryAttack();
	}
	else if( m_pPlayer->pev->button & IN_RELOAD && iMaxClip() != WEAPON_NOCLIP && !m_fInReload )
	{
		// reload when reload is pressed, or if no buttons are down and weapon is empty.
		Reload();
	}
	else if( !( m_pPlayer->pev->button & ( IN_ATTACK | IN_ATTACK2 ) ) )
	{
		// no fire buttons down
		m_fFireOnEmpty = FALSE;

		// weapon is useable. Reload if empty and weapon has waited as long as it has to after firing
		if( m_iClip == 0 && !( iFlags() & ITEM_FLAG_NOAUTORELOAD ) && m_flNextPrimaryAttack <= 0.0f )
		{
			Reload();
			return;
		}

		WeaponIdle( );
		return;
	}

	// catch all
	if( ShouldWeaponIdle() )
	{
		WeaponIdle();
	}
}

/*
=====================
CBasePlayer::SelectItem

  Switch weapons
=====================
*/
void CBasePlayer::SelectItem( const char *pstr )
{
	if( !pstr )
		return;

	CBasePlayerItem *pItem = NULL;

	if( !pItem )
		return;

	if( pItem == m_pActiveItem )
		return;

	if( m_pActiveItem )
		m_pActiveItem->Holster();

	m_pLastItem = m_pActiveItem;
	m_pActiveItem = pItem;

	if( m_pActiveItem )
	{
		m_pActiveItem->Deploy();
	}
}

/*
=====================
CBasePlayer::SelectLastItem

=====================
*/
void CBasePlayer::SelectLastItem( void )
{
	if( !m_pLastItem )
	{
		return;
	}

	if( m_pActiveItem && !m_pActiveItem->CanHolster() )
	{
		return;
	}

	if( m_pActiveItem )
		m_pActiveItem->Holster();

	CBasePlayerItem *pTemp = m_pActiveItem;
	m_pActiveItem = m_pLastItem;
	m_pLastItem = pTemp;
	m_pActiveItem->Deploy( );
}

/*
=====================
CBasePlayer::Killed

=====================
*/
void CBasePlayer::Killed( entvars_t *pevAttacker, int iGib )
{
	// Holster weapon immediately, to allow it to cleanup
	if( m_pActiveItem )
		 m_pActiveItem->Holster();

	g_irunninggausspred = false;
}

/*
=====================
CBasePlayer::Spawn

=====================
*/
void CBasePlayer::Spawn( void )
{
	if( m_pActiveItem )
		m_pActiveItem->Deploy( );

	g_irunninggausspred = false;
}

/*
=====================
UTIL_TraceLine

Don't actually trace, but act like the trace didn't hit anything.
=====================
*/
void UTIL_TraceLine( const Vector &vecStart, const Vector &vecEnd, IGNORE_MONSTERS igmon, edict_t *pentIgnore, TraceResult *ptr )
{
	memset( ptr, 0, sizeof(*ptr) );
	ptr->flFraction = 1.0f;
}

/*
=====================
UTIL_ParticleBox

For debugging, draw a box around a player made out of particles
=====================
*/
void UTIL_ParticleBox( CBasePlayer *player, float *mins, float *maxs, float life, unsigned char r, unsigned char g, unsigned char b )
{
	int i;
	vec3_t mmin, mmax;

	for( i = 0; i < 3; i++ )
	{
		mmin[i] = player->pev->origin[i] + mins[i];
		mmax[i] = player->pev->origin[i] + maxs[i];
	}

	gEngfuncs.pEfxAPI->R_ParticleBox( (float *)&mmin, (float *)&mmax, 5.0, 0, 255, 0 );
}

/*
=====================
UTIL_ParticleBoxes

For debugging, draw boxes for other collidable players
=====================
*/
void UTIL_ParticleBoxes( void )
{
	int idx;
	physent_t *pe;
	cl_entity_t *player;
	vec3_t mins, maxs;

	gEngfuncs.pEventAPI->EV_SetUpPlayerPrediction( false, true );

	// Store off the old count
	gEngfuncs.pEventAPI->EV_PushPMStates();

	player = gEngfuncs.GetLocalPlayer();
	// Now add in all of the players.
	gEngfuncs.pEventAPI->EV_SetSolidPlayers ( player->index - 1 );	

	for( idx = 1; idx < 100; idx++ )
	{
		pe = gEngfuncs.pEventAPI->EV_GetPhysent( idx );
		if( !pe )
			break;

		if( pe->info >= 1 && pe->info <= gEngfuncs.GetMaxClients() )
		{
			mins = pe->origin + pe->mins;
			maxs = pe->origin + pe->maxs;

			gEngfuncs.pEfxAPI->R_ParticleBox( (float *)&mins, (float *)&maxs, 0, 0, 255, 2.0 );
		}
	}

	gEngfuncs.pEventAPI->EV_PopPMStates();
}

/*
=====================
UTIL_ParticleLine

For debugging, draw a line made out of particles
=====================
*/
void UTIL_ParticleLine( CBasePlayer *player, float *start, float *end, float life, unsigned char r, unsigned char g, unsigned char b )
{
	gEngfuncs.pEfxAPI->R_ParticleLine( start, end, r, g, b, life );
}

/*
=====================
HUD_InitClientWeapons

Set up weapons, player and functions needed to run weapons code client-side.
=====================
*/
static void stub_ClientCommand( edict_t *pEdict, const char *szFmt, ... ) { }

void HUD_InitClientWeapons( void )
{
	static int initialized = 0;
	if( initialized )
		return;

	initialized = 1;

	// Set up pointer ( dummy object )
	gpGlobals = &Globals;

	// Fill in current time ( probably not needed )
	gpGlobals->time = gEngfuncs.GetClientTime();

	// Fake functions
	g_engfuncs.pfnPrecacheModel = stub_PrecacheModel;
	g_engfuncs.pfnPrecacheSound = stub_PrecacheSound;
	g_engfuncs.pfnPrecacheEvent = stub_PrecacheEvent;
	g_engfuncs.pfnNameForFunction = stub_NameForFunction;
	g_engfuncs.pfnSetModel = stub_SetModel;
	g_engfuncs.pfnSetClientMaxspeed = HUD_SetMaxSpeed;

	// Handled locally
	g_engfuncs.pfnPlaybackEvent = HUD_PlaybackEvent;
	g_engfuncs.pfnAlertMessage = AlertMessage;

	// Pass through to engine
	g_engfuncs.pfnPrecacheEvent = gEngfuncs.pfnPrecacheEvent;
	g_engfuncs.pfnRandomFloat = gEngfuncs.pfnRandomFloat;
	g_engfuncs.pfnRandomLong = gEngfuncs.pfnRandomLong;
	g_engfuncs.pfnCVarGetFloat = gEngfuncs.pfnGetCvarFloat;
	// Server-to-client commands (e.g. VR haptics) are sent by the real server; no-op here, not NULL
	g_engfuncs.pfnClientCommand = stub_ClientCommand;

	// Allocate a slot for the local player
	HUD_PrepEntity( &player, NULL );

	// Allocate slot(s) for each weapon that we are going to be predicting
	HUD_PrepEntity( &g_Glock, &player );
	HUD_PrepEntity( &g_Crowbar, &player );
	HUD_PrepEntity( &g_Python, &player );
	HUD_PrepEntity( &g_Mp5, &player );
	HUD_PrepEntity( &g_Crossbow, &player );
	HUD_PrepEntity( &g_Shotgun, &player );
	HUD_PrepEntity( &g_Rpg, &player );
	HUD_PrepEntity( &g_Gauss, &player );
	HUD_PrepEntity( &g_Egon, &player );
	HUD_PrepEntity( &g_HGun, &player );
	HUD_PrepEntity( &g_HandGren, &player );
	HUD_PrepEntity( &g_Satchel, &player );
	HUD_PrepEntity( &g_Tripmine, &player );
	HUD_PrepEntity( &g_Snark, &player );
}

/*
=====================
HUD_GetLastOrg

Retruns the last position that we stored for egon beam endpoint.
=====================
*/
void HUD_GetLastOrg( float *org )
{
	int i;

	// Return last origin
	for( i = 0; i < 3; i++ )
	{
		org[i] = previousorigin[i];
	}
}

/*
=====================
HUD_SetLastOrg

Remember our exact predicted origin so we can draw the egon to the right position.
=====================
*/
void HUD_SetLastOrg( void )
{
	int i;

	// Offset final origin by view_offset
	for( i = 0; i < 3; i++ )
	{
		previousorigin[i] = g_finalstate->playerstate.origin[i] + g_finalstate->client.view_ofs[i];
	}
}

/*
=====================
HUD_WeaponsPostThink

Run Weapon firing code on client
=====================
*/
void HUD_WeaponsPostThink( local_state_s *from, local_state_s *to, usercmd_t *cmd, double time, unsigned int random_seed )
{
	VR_UpdateCurrentWeapon( to->client.m_iId );

	int i;
	int buttonsChanged;
	CBasePlayerWeapon *pWeapon = NULL;
	CBasePlayerWeapon *pCurrent;
	weapon_data_t nulldata = {0}, *pfrom, *pto;
	static int lasthealth;

	HUD_InitClientWeapons();

	// Get current clock
	gpGlobals->time = time;

	// Fill in data based on selected weapon
	// FIXME, make this a method in each weapon?  where you pass in an entity_state_t *?
	switch( from->client.m_iId )
	{
		case WEAPON_CROWBAR:
			pWeapon = &g_Crowbar;
			break;
		case WEAPON_GLOCK:
			pWeapon = &g_Glock;
			break;
		case WEAPON_PYTHON:
			pWeapon = &g_Python;
			break;
		case WEAPON_MP5:
			pWeapon = &g_Mp5;
			break;
		case WEAPON_CROSSBOW:
			pWeapon = &g_Crossbow;
			break;
		case WEAPON_SHOTGUN:
			pWeapon = &g_Shotgun;
			break;
		case WEAPON_RPG:
			pWeapon = &g_Rpg;
			break;
		case WEAPON_GAUSS:
			pWeapon = &g_Gauss;
			break;
		case WEAPON_EGON:
			pWeapon = &g_Egon;
			break;
		case WEAPON_HORNETGUN:
			pWeapon = &g_HGun;
			break;
		case WEAPON_HANDGRENADE:
			pWeapon = &g_HandGren;
			break;
		case WEAPON_SATCHEL:
			pWeapon = &g_Satchel;
			break;
		case WEAPON_TRIPMINE:
			pWeapon = &g_Tripmine;
			break;
		case WEAPON_SNARK:
			pWeapon = &g_Snark;
			break;
	}

	// Store pointer to our destination entity_state_t so we can get our origin, etc. from it
	//  for setting up events on the client
	g_finalstate = to;

	// If we are running events/etc. go ahead and see if we
	//  managed to die between last frame and this one
	// If so, run the appropriate player killed or spawn function
	if( g_runfuncs )
	{
		if( to->client.health <= 0 && lasthealth > 0 )
		{
			player.Killed( NULL, 0 );
		}
		else if( to->client.health > 0 && lasthealth <= 0 )
		{
			player.Spawn();
		}

		lasthealth = to->client.health;
	}

	// We are not predicting the current weapon, just bow out here.
	if( !pWeapon )
		return;

	for( i = 0; i < MAX_WEAPONS; i++ )
	{
		pCurrent = g_pWpns[i];
		if( !pCurrent )
		{
			continue;
		}

		pfrom = &from->weapondata[i];

		pCurrent->m_fInReload = pfrom->m_fInReload;
		pCurrent->m_fInSpecialReload = pfrom->m_fInSpecialReload;
		//pCurrent->m_flPumpTime = pfrom->m_flPumpTime;
		pCurrent->m_iClip = pfrom->m_iClip;
		pCurrent->m_flNextPrimaryAttack	= pfrom->m_flNextPrimaryAttack;
		pCurrent->m_flNextSecondaryAttack = pfrom->m_flNextSecondaryAttack;
		pCurrent->m_flTimeWeaponIdle = pfrom->m_flTimeWeaponIdle;
		pCurrent->pev->fuser1 = pfrom->fuser1;
		pCurrent->m_flStartThrow = pfrom->fuser2;
		pCurrent->m_flReleaseThrow = pfrom->fuser3;
		pCurrent->m_chargeReady = pfrom->iuser1;
		pCurrent->m_fInAttack = pfrom->iuser2;
		pCurrent->m_fireState = pfrom->iuser3;

		pCurrent->m_iSecondaryAmmoType = (int)from->client.vuser3[2];
		pCurrent->m_iPrimaryAmmoType = (int)from->client.vuser4[0];
		player.m_rgAmmo[pCurrent->m_iPrimaryAmmoType] = (int)from->client.vuser4[1];
		player.m_rgAmmo[pCurrent->m_iSecondaryAmmoType] = (int)from->client.vuser4[2];
	}

	// For random weapon events, use this seed to seed random # generator
	player.random_seed = random_seed;

	// Get old buttons from previous state.
	player.m_afButtonLast = from->playerstate.oldbuttons;

	// Which buttsons chave changed
	buttonsChanged = ( player.m_afButtonLast ^ cmd->buttons );	// These buttons have changed this frame

	// Debounced button codes for pressed/released
	// The changed ones still down are "pressed"
	player.m_afButtonPressed =  buttonsChanged & cmd->buttons;	
	// The ones not down are "released"
	player.m_afButtonReleased = buttonsChanged & ( ~cmd->buttons );

	// Set player variables that weapons code might check/alter
	player.pev->button = cmd->buttons;

	player.pev->velocity = from->client.velocity;
	player.pev->flags = from->client.flags;

	player.pev->deadflag = from->client.deadflag;
	player.pev->waterlevel = from->client.waterlevel;
	player.pev->maxspeed = from->client.maxspeed;
	player.pev->fov = from->client.fov;
	player.pev->weaponanim = from->client.weaponanim;
	player.pev->viewmodel = from->client.viewmodel;
	player.m_flNextAttack = from->client.m_flNextAttack;
	player.m_flNextAmmoBurn = from->client.fuser2;
	player.m_flAmmoStartCharge = from->client.fuser3;

	//Stores all our ammo info, so the client side weapons can use them.
	player.ammo_9mm = (int)from->client.vuser1[0];
	player.ammo_357 = (int)from->client.vuser1[1];
	player.ammo_argrens = (int)from->client.vuser1[2];
	player.ammo_bolts = (int)from->client.ammo_nails; //is an int anyways...
	player.ammo_buckshot = (int)from->client.ammo_shells; 
	player.ammo_uranium = (int)from->client.ammo_cells;
	player.ammo_hornets = (int)from->client.vuser2[0];
	player.ammo_rockets = (int)from->client.ammo_rockets;

	// Point to current weapon object
	if( from->client.m_iId )
	{
		player.m_pActiveItem = g_pWpns[from->client.m_iId];
	}

	if( player.m_pActiveItem->m_iId == WEAPON_RPG )
	{
		( (CRpg *)player.m_pActiveItem )->m_fSpotActive = (int)from->client.vuser2[1];
		( (CRpg *)player.m_pActiveItem )->m_cActiveRockets = (int)from->client.vuser2[2];
	}

	// Don't go firing anything if we have died.
	// Or if we don't have a weapon model deployed
	if( ( player.pev->deadflag != ( DEAD_DISCARDBODY + 1 ) ) && 
		 !CL_IsDead() && player.pev->viewmodel && !g_iUser1 )
	{
		// every frame, not only when ItemPostFrame runs: it pauses while m_flNextAttack > 0
		// (deploy after a weapon switch, reloads), and the laser sight lapsed meanwhile
		pWeapon->MakeLaser();
		if( player.m_flNextAttack <= 0 )
		{
			pWeapon->ItemPostFrame();
		}
	}

	// Assume that we are not going to switch weapons
	to->client.m_iId = from->client.m_iId;

	// Now see if we issued a changeweapon command ( and we're not dead )
	if( cmd->weaponselect && ( player.pev->deadflag != ( DEAD_DISCARDBODY + 1 ) ) )
	{
		// Switched to a different weapon?
		if( from->weapondata[cmd->weaponselect].m_iId == cmd->weaponselect )
		{
			CBasePlayerWeapon *pNew = g_pWpns[cmd->weaponselect];
			if( pNew && ( pNew != pWeapon ) )
			{
				// Put away old weapon
				if( player.m_pActiveItem )
					player.m_pActiveItem->Holster();

				player.m_pLastItem = player.m_pActiveItem;
				player.m_pActiveItem = pNew;

				// Deploy new weapon
				if( player.m_pActiveItem )
				{
					player.m_pActiveItem->Deploy();
				}

				// Update weapon id so we can predict things correctly.
				to->client.m_iId = cmd->weaponselect;
			}
		}
	}

	// Copy in results of prediction code
	to->client.viewmodel = player.pev->viewmodel;
	to->client.fov = player.pev->fov;
	to->client.weaponanim = player.pev->weaponanim;
	to->client.m_flNextAttack = player.m_flNextAttack;
	to->client.fuser2 = player.m_flNextAmmoBurn;
	to->client.fuser3 = player.m_flAmmoStartCharge;
	to->client.maxspeed = player.pev->maxspeed;

	//HL Weapons
	to->client.vuser1[0] = player.ammo_9mm;
	to->client.vuser1[1] = player.ammo_357;
	to->client.vuser1[2] = player.ammo_argrens;

	to->client.ammo_nails = player.ammo_bolts;
	to->client.ammo_shells = player.ammo_buckshot;
	to->client.ammo_cells = player.ammo_uranium;
	to->client.vuser2[0] = player.ammo_hornets;
	to->client.ammo_rockets = player.ammo_rockets;

	if( player.m_pActiveItem->m_iId == WEAPON_RPG )
	{
		to->client.vuser2[1] = ( (CRpg *)player.m_pActiveItem)->m_fSpotActive;
		to->client.vuser2[2] = ( (CRpg *)player.m_pActiveItem)->m_cActiveRockets;
	}

	// Make sure that weapon animation matches what the game .dll is telling us
	//  over the wire ( fixes some animation glitches )
	if( g_runfuncs && ( HUD_GetWeaponAnim() != to->client.weaponanim ) )
	{
		int body = 0;

		//Show laser sight/scope combo
		if( pWeapon == &g_Python && bIsMultiplayer() )
			 body = 1;

		// Force a fixed anim down to viewmodel
		HUD_SendWeaponAnim( to->client.weaponanim, body, 1 );
	}

	for( i = 0; i < MAX_WEAPONS; i++ )
	{
		pCurrent = g_pWpns[i];

		pto = &to->weapondata[i];

		if( !pCurrent )
		{
			memset( pto, 0, sizeof(weapon_data_t) );
			continue;
		}

		pto->m_fInReload = pCurrent->m_fInReload;
		pto->m_fInSpecialReload = pCurrent->m_fInSpecialReload;
		//pto->m_flPumpTime = pCurrent->m_flPumpTime;
		pto->m_iClip = pCurrent->m_iClip; 
		pto->m_flNextPrimaryAttack = pCurrent->m_flNextPrimaryAttack;
		pto->m_flNextSecondaryAttack = pCurrent->m_flNextSecondaryAttack;
		pto->m_flTimeWeaponIdle = pCurrent->m_flTimeWeaponIdle;
		pto->fuser1 = pCurrent->pev->fuser1;
		pto->fuser2 = pCurrent->m_flStartThrow;
		pto->fuser3 = pCurrent->m_flReleaseThrow;
		pto->iuser1 = pCurrent->m_chargeReady;
		pto->iuser2 = pCurrent->m_fInAttack;
		pto->iuser3 = pCurrent->m_fireState;

		// Decrement weapon counters, server does this at same time ( during post think, after doing everything else )
		pto->m_flNextReload -= cmd->msec / 1000.0f;
		pto->m_fNextAimBonus -= cmd->msec / 1000.0f;
		pto->m_flNextPrimaryAttack -= cmd->msec / 1000.0f;
		pto->m_flNextSecondaryAttack -= cmd->msec / 1000.0f;
		pto->m_flTimeWeaponIdle -= cmd->msec / 1000.0f;
		pto->fuser1 -= cmd->msec / 1000.0f;

		to->client.vuser3[2] = pCurrent->m_iSecondaryAmmoType;
		to->client.vuser4[0] = pCurrent->m_iPrimaryAmmoType;
		to->client.vuser4[1] = player.m_rgAmmo[pCurrent->m_iPrimaryAmmoType];
		to->client.vuser4[2] = player.m_rgAmmo[pCurrent->m_iSecondaryAmmoType];

/*		if( pto->m_flPumpTime != -9999.0f )
		{
			pto->m_flPumpTime -= cmd->msec / 1000.0f;
			if( pto->m_flPumpTime < -0.001f )
				pto->m_flPumpTime = -0.001f;
		}*/

		if( pto->m_fNextAimBonus < -1.0f )
		{
			pto->m_fNextAimBonus = -1.0f;
		}

		if( pto->m_flNextPrimaryAttack < -1.0f )
		{
			pto->m_flNextPrimaryAttack = -1.0f;
		}

		if( pto->m_flNextSecondaryAttack < -0.001f )
		{
			pto->m_flNextSecondaryAttack = -0.001f;
		}

		if( pto->m_flTimeWeaponIdle < -0.001f )
		{
			pto->m_flTimeWeaponIdle = -0.001f;
		}

		if( pto->m_flNextReload < -0.001f )
		{
			pto->m_flNextReload = -0.001f;
		}

		if( pto->fuser1 < -0.001f )
		{
			pto->fuser1 = -0.001f;
		}
	}

	// m_flNextAttack is now part of the weapons, but is part of the player instead
	to->client.m_flNextAttack -= cmd->msec / 1000.0f;
	if( to->client.m_flNextAttack < -0.001f )
	{
		to->client.m_flNextAttack = -0.001f;
	}

	to->client.fuser2 -= cmd->msec / 1000.0f;
	if( to->client.fuser2 < -0.001f )
	{
		to->client.fuser2 = -0.001f;
	}

	to->client.fuser3 -= cmd->msec / 1000.0f;
	if( to->client.fuser3 < -0.001f )
	{
		to->client.fuser3 = -0.001f;
	}

	// Store off the last position from the predicted state.
	HUD_SetLastOrg();

	// Wipe it so we can't use it after this frame
	g_finalstate = NULL;
}

/*
=====================
HUD_PostRunCmd

Client calls this during prediction, after it has moved the player and updated any info changed into to->
time is the current client clock based on prediction
cmd is the command that caused the movement, etc
runfuncs is 1 if this is the first time we've predicted this command.  If so, sounds and effects should play, otherwise, they should
be ignored
=====================
*/
void _DLLEXPORT HUD_PostRunCmd( struct local_state_s *from, struct local_state_s *to, struct usercmd_s *cmd, int runfuncs, double time, unsigned int random_seed )
{
	g_runfuncs = runfuncs;

#if CLIENT_WEAPONS
	if( cl_lw && cl_lw->value )
	{
		HUD_WeaponsPostThink( from, to, cmd, time, random_seed );
	}
	else
#endif
	{
		to->client.fov = g_lastFOV;
	}

	if( g_irunninggausspred == 1 )
	{
		Vector forward;
		gEngfuncs.pfnAngleVectors( v_angles, forward, NULL, NULL );
		to->client.velocity = to->client.velocity - forward * g_flApplyVel * 5; 
		g_irunninggausspred = false;
	}

	// All games can use FOV state
	g_lastFOV = to->client.fov;
}
