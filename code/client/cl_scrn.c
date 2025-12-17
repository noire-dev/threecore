/*
===========================================================================
Copyright (C) 1999-2005 Id Software, Inc.

This file is part of Quake III Arena source code.

Quake III Arena source code is free software; you can redistribute it
and/or modify it under the terms of the GNU General Public License as
published by the Free Software Foundation; either version 2 of the License,
or (at your option) any later version.

Quake III Arena source code is distributed in the hope that it will be
useful, but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with Quake III Arena source code; if not, write to the Free Software
Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
===========================================================================
*/

#include "client.h"

static qboolean	scr_initialized;		// ready to draw

void SCR_Init( void ) { scr_initialized = qtrue; }

void SCR_Done( void ) { scr_initialized = qfalse; }

static void SCR_DrawScreenField( void ) {
	qboolean uiFullscreen;

	re.BeginFrame();

	uiFullscreen = (uivm && VM_Call( uivm, 0, UI_IS_FULLSCREEN ));

	if ( uivm && !uiFullscreen ) {
		switch( cls.state ) {
		default:
			Com_Error( ERR_FATAL, "SCR_DrawScreenField: bad cls.state" );
			break;
		case CA_DISCONNECTED:
			VM_Call( uivm, 1, UI_SET_ACTIVE_MENU, UIMENU_MAIN );
			break;
		case CA_CONNECTING:
		case CA_CHALLENGING:
		case CA_CONNECTED:
			VM_Call( uivm, 1, UI_REFRESH, cls.realtime );
			VM_Call( uivm, 1, UI_DRAW_CONNECT_SCREEN, qfalse );
			break;
		case CA_LOADING:
		case CA_PRIMED:
			if ( cgvm ) CL_CGameRendering();
			VM_Call( uivm, 1, UI_REFRESH, cls.realtime );
			VM_Call( uivm, 1, UI_DRAW_CONNECT_SCREEN, qtrue );
			break;
		case CA_ACTIVE:
			CL_CGameRendering();
			break;
		}
	}

	if ( Key_GetCatcher( ) & KEYCATCH_UI && uivm ) VM_Call( uivm, 1, UI_REFRESH, cls.realtime );
}

void SCR_UpdateScreen( void ) {
	static int framecount;
	static int next_frametime;

	if(!scr_initialized) return;

	if(framecount == cls.framecount) {
		int ms = Sys_Milliseconds();
		if ( next_frametime && ms - next_frametime < 0 ) re.ThrottleBackend();
		else next_frametime = ms + 16;
	} else {
		next_frametime = 0;
		framecount = cls.framecount;
	}

	if (uivm) {
		SCR_DrawScreenField();
		re.EndFrame();
	}
}
