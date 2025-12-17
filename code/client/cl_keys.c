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

/*

key up events are sent even if in console mode

*/

/*
===================
CL_KeyDownEvent

Called by CL_KeyEvent to handle a keypress
===================
*/
static void CL_KeyDownEvent( int key, unsigned time )
{
	keys[key].down = qtrue;
	keys[key].bound = qfalse;
	keys[key].repeats++;

	if ( keys[key].repeats == 1 ) anykeydown++;

	// hardcoded screenshot key
	if ( key == K_PRINT ) {
		if ( keys[K_SHIFT].down ) {
			Cbuf_ExecuteText( EXEC_APPEND, "screenshotBMP\n" );
		} else {
			Cbuf_ExecuteText( EXEC_APPEND, "screenshotBMP clipboard\n" );
		}
		return;
	}

	// escape is always handled special
	if ( key == K_ESCAPE ) {
		// escape always gets out of CGAME stuff
		if (Key_GetCatcher( ) & KEYCATCH_CGAME) {
			Key_SetCatcher( Key_GetCatcher( ) & ~KEYCATCH_CGAME );
			return;
		}

		if ( !( Key_GetCatcher( ) & KEYCATCH_UI ) ) {
			if ( cls.state == CA_ACTIVE && !clc.demoplaying ) {
				VM_Call( uivm, 1, UI_SET_ACTIVE_MENU, UIMENU_INGAME );
			}
			else if ( cls.state != CA_DISCONNECTED ) {
				Cmd_Clear();
				Cvar_Set( "com_errorMessage", "" );
				if ( !CL_Disconnect( qfalse ) ) { // restart client if not done already
					CL_FlushMemory();
				}
				VM_Call( uivm, 1, UI_SET_ACTIVE_MENU, UIMENU_MAIN );
			}
			return;
		}

		VM_Call( uivm, 2, UI_KEY_EVENT, key, qtrue );
		return;
	}

	// distribute the key down event to the appropriate handler
	if ( Key_GetCatcher( ) & KEYCATCH_UI ) {
		if ( uivm ) VM_Call( uivm, 2, UI_KEY_EVENT, key, qtrue );
	} else {
		Key_ParseBinding( key, qtrue, time );
	}
}


/*
===================
CL_KeyUpEvent

Called by CL_KeyEvent to handle a keyrelease
===================
*/
static void CL_KeyUpEvent( int key, unsigned time )
{
	const qboolean bound = keys[key].bound;

	keys[key].repeats = 0;
	keys[key].down = qfalse;
	keys[key].bound = qfalse;

	if ( --anykeydown < 0 ) anykeydown = 0;

	// hardcoded screenshot key
	if ( key == K_PRINT ) return;

	//
	// key up events only perform actions if the game key binding is
	// a button command (leading + sign).  These will be processed even in
	// console mode and menu mode, to keep the character from continuing
	// an action started before a mode switch.
	//
	if ( cls.state != CA_DISCONNECTED ) {
		if ( bound || ( Key_GetCatcher() & KEYCATCH_CGAME ) ) {
			Key_ParseBinding( key, qfalse, time );
		}
	}

	if ( Key_GetCatcher() & KEYCATCH_UI ) {
		if ( uivm ) VM_Call( uivm, 2, UI_KEY_EVENT, key, qfalse );
	}
}


/*
===================
CL_KeyEvent

Called by the system for both key up and key down events
===================
*/
void CL_KeyEvent( int key, qboolean down, unsigned time )
{
	if ( down )
		CL_KeyDownEvent( key, time );
	else
		CL_KeyUpEvent( key, time );
}


/*
===================
CL_CharEvent

Normal keyboard characters, already shifted / capslocked / etc
===================
*/
void CL_CharEvent( int key )
{
	// delete is not a printable character and is
	// otherwise handled by Field_KeyDownEvent
	if ( key == 127 )
		return;


    if ( Key_GetCatcher( ) & KEYCATCH_UI ) {
		VM_Call( uivm, 2, UI_KEY_EVENT, key | K_CHAR_FLAG, qtrue );
	}
}


/*
===================
Key_ClearStates
===================
*/
void Key_ClearStates( void )
{
	int		i;

	anykeydown = 0;

	for ( i = 0 ; i < MAX_KEYS ; i++ )
	{
		if ( keys[i].down )
			CL_KeyEvent( i, qfalse, 0 );

		keys[i].down = qfalse;
		keys[i].repeats = 0;
	}
}


static int keyCatchers = 0;

/*
====================
Key_GetCatcher
====================
*/
int Key_GetCatcher( void )
{
	return keyCatchers;
}


/*
====================
Key_SetCatcher
====================
*/
void Key_SetCatcher( int catcher )
{
	// If the catcher state is changing, clear all key states
	if ( catcher != keyCatchers )
		Key_ClearStates();

	keyCatchers = catcher;
}
