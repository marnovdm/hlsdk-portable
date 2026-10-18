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
//
// flashlight.cpp
//
// implementation of CHudFlashlight class
//

#include "hud.h"
#include "vr_client.h"
#include "cl_util.h"
#include "parsemsg.h"
#include "const.h"
#include "triangleapi.h"
#include "r_studioint.h"

extern engine_studio_api_t IEngineStudio;

#include <string.h>
#include <stdio.h>

DECLARE_MESSAGE( m_Nightvision, Nightvision )
DECLARE_MESSAGE( m_Nightvision, Flashlight )

#define NIGHTVISION_SPRITE1_NAME "sprites/of_nv.spr"
#define NIGHTVISION_SPRITE2_NAME "sprites/of_nv_a.spr"
#define NIGHTVISION_SPRITE3_NAME "sprites/of_nv_b.spr"
#define NIGHTVISION_SPRITE4_NAME "sprites/of_nv_int.spr"

int CHudNightvision::Init(void)
{
	m_fOn = 0;

	HOOK_MESSAGE(Nightvision);
	HOOK_MESSAGE(Flashlight);

	m_iFlags |= HUD_ACTIVE;

	gHUD.AddHudElem(this);

	return 1;
};

void CHudNightvision::Reset(void)
{
	m_fOn = 0;
}

int CHudNightvision::VidInit(void)
{
	m_hSprite1 = LoadSprite(NIGHTVISION_SPRITE1_NAME);
	m_hSprite2 = LoadSprite(NIGHTVISION_SPRITE2_NAME);
	m_hSprite3 = LoadSprite(NIGHTVISION_SPRITE3_NAME);
	m_hSprite4 = LoadSprite(NIGHTVISION_SPRITE4_NAME);
	m_hWhite = SPR_Load("sprites/white.spr");

	// Get the number of frames available in this sprite.
	m_nFrameCount = SPR_Frames(m_hSprite2);

	// current frame.
	m_iFrame = 0;

	return 1;
};


int CHudNightvision::MsgFunc_Nightvision(const char *pszName, int iSize, void *pbuf)
{
	BEGIN_READ(pbuf, iSize);
	m_fOn = READ_BYTE();

	return 1;
}

int CHudNightvision::MsgFunc_Flashlight( const char *pszName, int iSize, void *pbuf )
{
	BEGIN_READ( pbuf, iSize );
	m_fOn = READ_BYTE();

	return 1;
}

int CHudNightvision::Draw(float flTime)
{
	if (gHUD.m_iHideHUDDisplay & (HIDEHUD_FLASHLIGHT | HIDEHUD_ALL))
		return 1;

	int r, g, b, x, y, a;
	
	// Only display this if the player is equipped with the suit.
	if (!(gHUD.m_iWeaponBits & (1 << (WEAPON_SUIT))))
		return 1;

	if (m_fOn)
		a = 225;
	else
		a = MIN_ALPHA;

	// Get each color component from the main
	// hud color.
	UnpackRGB(r, g, b, RGB_YELLOWISH);

	ScaleColors(r, g, b, a);

	// Top left of the screen.
	x = y = 0;

	// Reset the number of frame if we are at last frame.
	if (m_iFrame >= m_nFrameCount)
		m_iFrame = 0;

	const int nvgSpriteWidth = SPR_Width(m_hSprite2, 0);
	const int nvgSpriteHeight = SPR_Height(m_hSprite2, 0);

	const int colCount = (int)ceil(ScreenWidth / (float)nvgSpriteWidth);
	const int rowCount = (int)ceil(ScreenHeight / (float)nvgSpriteHeight);

	// In VR the HUD is a floating panel (with the hand HUD cut out of it), so the
	// goggles are drawn over each eye instead: VR_DrawNightvision from the 3D pass.
	if (m_fOn && !VR_IsActive())
	{  
		//
		// draw nightvision scanlines sprite.
		//
		SPR_Set(m_hSprite2, r, g, b);

		int i, j;
		for (i = 0; i < rowCount; ++i) // height
		{
			for (j = 0; j < colCount; ++j) // width
			{
				SPR_DrawAdditive(m_iFrame, x + (j * nvgSpriteWidth), y + (i * nvgSpriteHeight), NULL);
			}
		}
	}

	// Increase sprite frame.
	m_iFrame++;

	return 1;
}

/* Night vision in VR: an additive green quad facing this eye, just past the engine's
 * near plane (4 units) and wider than the widest eye frustum (PS VR2: tangent 1.83 out,
 * 1.33 up/down), drawn in the transparent pass of each eye. A world-aligned box around
 * the eye was clipped by the near plane at the image edges. Nothing solid comes closer
 * than 4.5 units inside the view (the hull and the lean clamp keep walls >= 12 units
 * away). Strength: the desktop overlay's average (scanline sprite ~75% lit, HUD green,
 * alpha 225) times vr_nightvision_tint. */
void VR_DrawNightvision( void )
{
	if( !gHUD.m_Nightvision.m_fOn || !VR_IsActive() ||
		( gHUD.m_iHideHUDDisplay & ( HIDEHUD_FLASHLIGHT | HIDEHUD_ALL )) ||
		!( gHUD.m_iWeaponBits & ( 1 << WEAPON_SUIT )))
		return;

	const float scale = gEngfuncs.pfnGetCvarFloat( "vr_nightvision_tint" );
	if( scale <= 0.0f )
		return;

	int r, g, b;
	UnpackRGB( r, g, b, RGB_YELLOWISH );
	const float k = 0.75f * ( 225.0f / 255.0f ) * scale / 255.0f;

	vec3_t eye, up, right, forward;
	IEngineStudio.GetViewInfo( eye, up, right, forward ); // this eye's render origin and axes
	const float depth = 4.5f, extent = depth * 4.0f;
	vec3_t corner[4];
	for( int i = 0; i < 4; ++i )
	{
		const float sx = ( i == 1 || i == 2 ) ? extent : -extent;
		const float sy = ( i >= 2 ) ? extent : -extent;
		for( int c = 0; c < 3; ++c )
			corner[i][c] = eye[c] + forward[c] * depth + right[c] * sx + up[c] * sy;
	}

	const struct model_s *white = gEngfuncs.GetSpritePointer( gHUD.m_Nightvision.m_hWhite );
	if( !white )
		return;
	triangleapi_t *tri = gEngfuncs.pTriAPI;
	tri->RenderMode( kRenderTransAdd );
	tri->CullFace( TRI_NONE );
	tri->SpriteTexture( (struct model_s *)white, 0 );
	tri->Begin( TRI_QUADS );
	tri->Color4f( r * k, g * k, b * k, 1.0f );
	for( int v = 0; v < 4; ++v )
		tri->Vertex3fv( corner[v] );
	tri->End();
	tri->RenderMode( kRenderNormal );
	tri->CullFace( TRI_FRONT );
}
