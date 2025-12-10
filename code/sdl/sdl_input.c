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

#include <SDL.h>

#include "../client/client.h"
#include "sdl_glw.h"

static cvar_t *in_keyboardDebug;
static cvar_t *in_forceCharset;

static qboolean mouseAvailable = qfalse;
static qboolean mouseActive = qfalse;

static cvar_t *in_mouse;

#define Com_QueueEvent Sys_QueEvent

static cvar_t *cl_consoleKeys;

static int in_eventTime = 0;
static qboolean mouse_focus;

#define CTRL(a) ((a)-'a'+1)

/*
===============
IN_PrintKey
===============
*/
static void IN_PrintKey( const SDL_Keysym *keysym, keyNum_t key, qboolean down )
{
	if( down )
		Com_Printf( "+ " );
	else
		Com_Printf( "  " );

	Com_Printf( "Scancode: 0x%02x(%s) Sym: 0x%02x(%s)",
			keysym->scancode, SDL_GetScancodeName( keysym->scancode ),
			keysym->sym, SDL_GetKeyName( keysym->sym ) );

	if( keysym->mod & KMOD_LSHIFT )   Com_Printf( " KMOD_LSHIFT" );
	if( keysym->mod & KMOD_RSHIFT )   Com_Printf( " KMOD_RSHIFT" );
	if( keysym->mod & KMOD_LCTRL )    Com_Printf( " KMOD_LCTRL" );
	if( keysym->mod & KMOD_RCTRL )    Com_Printf( " KMOD_RCTRL" );
	if( keysym->mod & KMOD_LALT )     Com_Printf( " KMOD_LALT" );
	if( keysym->mod & KMOD_RALT )     Com_Printf( " KMOD_RALT" );
	if( keysym->mod & KMOD_LGUI )     Com_Printf( " KMOD_LGUI" );
	if( keysym->mod & KMOD_RGUI )     Com_Printf( " KMOD_RGUI" );
	if( keysym->mod & KMOD_NUM )      Com_Printf( " KMOD_NUM" );
	if( keysym->mod & KMOD_CAPS )     Com_Printf( " KMOD_CAPS" );
	if( keysym->mod & KMOD_MODE )     Com_Printf( " KMOD_MODE" );
	if( keysym->mod & KMOD_RESERVED ) Com_Printf( " KMOD_RESERVED" );

	Com_Printf( " Q:0x%02x(%s)\n", key, Key_KeynumToString( key ) );
}


#define MAX_CONSOLE_KEYS 16

/*
===============
IN_IsConsoleKey

TODO: If the SDL_Scancode situation improves, use it instead of
      both of these methods
===============
*/
static qboolean IN_IsConsoleKey( keyNum_t key, int character )
{
	typedef struct consoleKey_s
	{
		enum
		{
			QUAKE_KEY,
			CHARACTER
		} type;

		union
		{
			keyNum_t key;
			int character;
		} u;
	} consoleKey_t;

	static consoleKey_t consoleKeys[ MAX_CONSOLE_KEYS ];
	static int numConsoleKeys = 0;
	int i;

	// Only parse the variable when it changes
	if ( cl_consoleKeys->modified )
	{
		const char *text_p, *token;

		cl_consoleKeys->modified = qfalse;
		text_p = cl_consoleKeys->string;
		numConsoleKeys = 0;

		while( numConsoleKeys < MAX_CONSOLE_KEYS )
		{
			consoleKey_t *c = &consoleKeys[ numConsoleKeys ];
			int charCode = 0;

			token = COM_Parse( &text_p );
			if( !token[ 0 ] )
				break;

			charCode = Com_HexStrToInt( token );

			if( charCode > 0 )
			{
				c->type = CHARACTER;
				c->u.character = charCode;
			}
			else
			{
				c->type = QUAKE_KEY;
				c->u.key = Key_StringToKeynum( token );

				// 0 isn't a key
				if ( c->u.key <= 0 )
					continue;
			}

			numConsoleKeys++;
		}
	}

	// If the character is the same as the key, prefer the character
	if ( key == character )
		key = 0;

	for ( i = 0; i < numConsoleKeys; i++ )
	{
		consoleKey_t *c = &consoleKeys[ i ];

		switch ( c->type )
		{
			case QUAKE_KEY:
				if( key && c->u.key == key )
					return qtrue;
				break;

			case CHARACTER:
				if( c->u.character == character )
					return qtrue;
				break;
		}
	}

	return qfalse;
}


/*
===============
IN_TranslateSDLToQ3Key
===============
*/
static keyNum_t IN_TranslateSDLToQ3Key( SDL_Keysym *keysym, qboolean down )
{
	keyNum_t key = 0;

	if ( keysym->scancode >= SDL_SCANCODE_1 && keysym->scancode <= SDL_SCANCODE_0 )
	{
		// Always map the number keys as such even if they actually map
		// to other characters (eg, "1" is "&" on an AZERTY keyboard).
		// This is required for SDL before 2.0.6, except on Windows
		// which already had this behavior.
		if( keysym->scancode == SDL_SCANCODE_0 )
			key = '0';
		else
			key = '1' + keysym->scancode - SDL_SCANCODE_1;
	}
	else if ( in_forceCharset->integer > 0 )
	{
		if ( keysym->scancode >= SDL_SCANCODE_A && keysym->scancode <= SDL_SCANCODE_Z )
		{
			key = 'a' + keysym->scancode - SDL_SCANCODE_A;
		}
		else
		{
			switch ( keysym->scancode )
			{
				case SDL_SCANCODE_MINUS:        key = '-';  break;
				case SDL_SCANCODE_EQUALS:       key = '=';  break;
				case SDL_SCANCODE_LEFTBRACKET:  key = '[';  break;
				case SDL_SCANCODE_RIGHTBRACKET: key = ']';  break;
				case SDL_SCANCODE_NONUSBACKSLASH:
				case SDL_SCANCODE_BACKSLASH:    key = '\\'; break;
				case SDL_SCANCODE_SEMICOLON:    key = ';';  break;
				case SDL_SCANCODE_APOSTROPHE:   key = '\''; break;
				case SDL_SCANCODE_COMMA:        key = ',';  break;
				case SDL_SCANCODE_PERIOD:       key = '.';  break;
				case SDL_SCANCODE_SLASH:        key = '/';  break;
				default:
					/* key = 0 */
					break;
			}
		}
	}

	if( !key && keysym->sym >= SDLK_SPACE && keysym->sym < SDLK_DELETE )
	{
		// These happen to match the ASCII chars
		key = (int)keysym->sym;
	}
	else if( !key )
	{
		switch( keysym->sym )
		{
			case SDLK_PAGEUP:       key = K_PGUP;          break;
			case SDLK_KP_9:         key = K_KP_PGUP;       break;
			case SDLK_PAGEDOWN:     key = K_PGDN;          break;
			case SDLK_KP_3:         key = K_KP_PGDN;       break;
			case SDLK_KP_7:         key = K_KP_HOME;       break;
			case SDLK_HOME:         key = K_HOME;          break;
			case SDLK_KP_1:         key = K_KP_END;        break;
			case SDLK_END:          key = K_END;           break;
			case SDLK_KP_4:         key = K_KP_LEFTARROW;  break;
			case SDLK_LEFT:         key = K_LEFTARROW;     break;
			case SDLK_KP_6:         key = K_KP_RIGHTARROW; break;
			case SDLK_RIGHT:        key = K_RIGHTARROW;    break;
			case SDLK_KP_2:         key = K_KP_DOWNARROW;  break;
			case SDLK_DOWN:         key = K_DOWNARROW;     break;
			case SDLK_KP_8:         key = K_KP_UPARROW;    break;
			case SDLK_UP:           key = K_UPARROW;       break;
			case SDLK_ESCAPE:       key = K_ESCAPE;        break;
			case SDLK_KP_ENTER:     key = K_KP_ENTER;      break;
			case SDLK_RETURN:       key = K_ENTER;         break;
			case SDLK_TAB:          key = K_TAB;           break;
			case SDLK_F1:           key = K_F1;            break;
			case SDLK_F2:           key = K_F2;            break;
			case SDLK_F3:           key = K_F3;            break;
			case SDLK_F4:           key = K_F4;            break;
			case SDLK_F5:           key = K_F5;            break;
			case SDLK_F6:           key = K_F6;            break;
			case SDLK_F7:           key = K_F7;            break;
			case SDLK_F8:           key = K_F8;            break;
			case SDLK_F9:           key = K_F9;            break;
			case SDLK_F10:          key = K_F10;           break;
			case SDLK_F11:          key = K_F11;           break;
			case SDLK_F12:          key = K_F12;           break;
			case SDLK_F13:          key = K_F13;           break;
			case SDLK_F14:          key = K_F14;           break;
			case SDLK_F15:          key = K_F15;           break;

			case SDLK_BACKSPACE:    key = K_BACKSPACE;     break;
			case SDLK_KP_PERIOD:    key = K_KP_DEL;        break;
			case SDLK_DELETE:       key = K_DEL;           break;
			case SDLK_PAUSE:        key = K_PAUSE;         break;

			case SDLK_LSHIFT:
			case SDLK_RSHIFT:       key = K_SHIFT;         break;

			case SDLK_LCTRL:
			case SDLK_RCTRL:        key = K_CTRL;          break;

#ifdef __APPLE__
			case SDLK_RGUI:
			case SDLK_LGUI:         key = K_COMMAND;       break;
#else
			case SDLK_RGUI:
			case SDLK_LGUI:         key = K_SUPER;         break;
#endif

			case SDLK_RALT:
			case SDLK_LALT:         key = K_ALT;           break;

			case SDLK_KP_5:         key = K_KP_5;          break;
			case SDLK_INSERT:       key = K_INS;           break;
			case SDLK_KP_0:         key = K_KP_INS;        break;
			case SDLK_KP_MULTIPLY:  key = '*'; /*K_KP_STAR;*/ break;
			case SDLK_KP_PLUS:      key = K_KP_PLUS;       break;
			case SDLK_KP_MINUS:     key = K_KP_MINUS;      break;
			case SDLK_KP_DIVIDE:    key = K_KP_SLASH;      break;

			case SDLK_MODE:         key = K_MODE;          break;
			case SDLK_HELP:         key = K_HELP;          break;
			case SDLK_PRINTSCREEN:  key = K_PRINT;         break;
			case SDLK_SYSREQ:       key = K_SYSREQ;        break;
			case SDLK_MENU:         key = K_MENU;          break;
			case SDLK_APPLICATION:	key = K_MENU;          break;
			case SDLK_POWER:        key = K_POWER;         break;
			case SDLK_UNDO:         key = K_UNDO;          break;
			case SDLK_SCROLLLOCK:   key = K_SCROLLOCK;     break;
			case SDLK_NUMLOCKCLEAR: key = K_KP_NUMLOCK;    break;
			case SDLK_CAPSLOCK:     key = K_CAPSLOCK;      break;

			default:
#if 1
				key = 0;
#else
				if( !( keysym->sym & SDLK_SCANCODE_MASK ) && keysym->scancode <= 95 )
				{
					// Map Unicode characters to 95 world keys using the key's scan code.
					// FIXME: There aren't enough world keys to cover all the scancodes.
					// Maybe create a map of scancode to quake key at start up and on
					// key map change; allocate world key numbers as needed similar
					// to SDL 1.2.
					key = K_WORLD_0 + (int)keysym->scancode;
				}
#endif
				break;
		}
	}

	if ( in_keyboardDebug->integer )
		IN_PrintKey( keysym, key, down );

	if ( keysym->scancode == SDL_SCANCODE_GRAVE )
	{
		//SDL_Keycode translated = SDL_GetKeyFromScancode( SDL_SCANCODE_GRAVE );

		//if ( translated == SDLK_CARET )
		{
			// Console keys can't be bound or generate characters
			key = K_CONSOLE;
		}
	}
	else if ( IN_IsConsoleKey( key, 0 ) )
	{
		// Console keys can't be bound or generate characters
		key = K_CONSOLE;
	}

	return key;
}


/*
===============
IN_GobbleMotionEvents
===============
*/
static void IN_GobbleMouseEvents( void )
{
	SDL_Event dummy[ 1 ];
	int val = 0;

	// Gobble any mouse events
	SDL_PumpEvents();

	while( ( val = SDL_PeepEvents( dummy, ARRAY_LEN( dummy ), SDL_GETEVENT,
		SDL_MOUSEMOTION, SDL_MOUSEWHEEL ) ) > 0 ) { }

	if ( val < 0 )
		Com_Printf( "%s failed: %s\n", __func__, SDL_GetError() );
}


//#define DEBUG_EVENTS

/*
===============
IN_ActivateMouse
===============
*/
static void IN_ActivateMouse( void )
{
	if ( !mouseAvailable )
		return;

	if ( !mouseActive )
	{
		IN_GobbleMouseEvents();

		SDL_SetRelativeMouseMode( in_mouse->integer == 1 ? SDL_TRUE : SDL_FALSE );
		SDL_SetWindowGrab( SDL_window, SDL_TRUE );

		if ( glw_state.isFullscreen )
			SDL_ShowCursor( SDL_FALSE );

		SDL_WarpMouseInWindow( SDL_window, glw_state.window_width / 2, glw_state.window_height / 2 );

#ifdef DEBUG_EVENTS
		Com_Printf( "%4i %s\n", Sys_Milliseconds(), __func__ );
#endif
	}

	// in_nograb makes no sense in fullscreen mode
	if ( !glw_state.isFullscreen )
	{
		if ( in_nograb->modified || !mouseActive )
		{
			if ( in_nograb->integer ) {
				SDL_SetRelativeMouseMode( SDL_FALSE );
				SDL_SetWindowGrab( SDL_window, SDL_FALSE );
			} else {
				SDL_SetRelativeMouseMode( in_mouse->integer == 1 ? SDL_TRUE : SDL_FALSE );
				SDL_SetWindowGrab( SDL_window, SDL_TRUE );
			}

			in_nograb->modified = qfalse;
		}
	}

	mouseActive = qtrue;
}


/*
===============
IN_DeactivateMouse
===============
*/
static void IN_DeactivateMouse( void )
{
	if ( !mouseAvailable )
		return;

	if ( mouseActive )
	{
#ifdef DEBUG_EVENTS
		Com_Printf( "%4i %s\n", Sys_Milliseconds(), __func__ );
#endif
		IN_GobbleMouseEvents();

		SDL_SetWindowGrab( SDL_window, SDL_FALSE );
		SDL_SetRelativeMouseMode( SDL_FALSE );

		if ( gw_active )
			SDL_WarpMouseInWindow( SDL_window, glw_state.window_width / 2, glw_state.window_height / 2 );
		else
		{
			if ( glw_state.isFullscreen )
				SDL_ShowCursor( SDL_TRUE );

			SDL_WarpMouseGlobal( glw_state.desktop_width / 2, glw_state.desktop_height / 2 );
		}

		mouseActive = qfalse;
	}

	// Always show the cursor when the mouse is disabled,
	// but not when fullscreen
	if ( !glw_state.isFullscreen )
		SDL_ShowCursor( SDL_TRUE );
}

#ifdef DEBUG_EVENTS
static const char *eventName( SDL_WindowEventID event )
{
	static char buf[32];

	switch ( event )
	{
		case SDL_WINDOWEVENT_NONE: return "NONE";
		case SDL_WINDOWEVENT_SHOWN: return "SHOWN";
		case SDL_WINDOWEVENT_HIDDEN: return "HIDDEN";
		case SDL_WINDOWEVENT_EXPOSED: return "EXPOSED";
		case SDL_WINDOWEVENT_MOVED: return "MOVED";
		case SDL_WINDOWEVENT_RESIZED: return "RESIZED";
		case SDL_WINDOWEVENT_SIZE_CHANGED: return "SIZE_CHANGED";
		case SDL_WINDOWEVENT_MINIMIZED: return "MINIMIZED";
		case SDL_WINDOWEVENT_MAXIMIZED: return "MAXIMIZED";
		case SDL_WINDOWEVENT_RESTORED: return "RESTORED";
		case SDL_WINDOWEVENT_ENTER: return "ENTER";
		case SDL_WINDOWEVENT_LEAVE: return "LEAVE";
		case SDL_WINDOWEVENT_FOCUS_GAINED: return "FOCUS_GAINED";
		case SDL_WINDOWEVENT_FOCUS_LOST: return "FOCUS_LOST";
		case SDL_WINDOWEVENT_CLOSE: return "CLOSE";
		case SDL_WINDOWEVENT_TAKE_FOCUS: return "TAKE_FOCUS";
		case SDL_WINDOWEVENT_HIT_TEST: return "HIT_TEST"; 
		default:
			sprintf( buf, "EVENT#%i", event );
			return buf;
	}
}
#endif


/*
===============
HandleEvents
===============
*/
//static void IN_ProcessEvents( void )
void HandleEvents( void )
{
	SDL_Event e;
	keyNum_t key = 0;
	static keyNum_t lastKeyDown = 0;

	if ( !SDL_WasInit( SDL_INIT_VIDEO ) )
			return;

	in_eventTime = Sys_Milliseconds();

	while ( SDL_PollEvent( &e ) )
	{
		switch( e.type )
		{
			case SDL_KEYDOWN:
				if ( e.key.repeat && Key_GetCatcher() == 0 )
					break;
				key = IN_TranslateSDLToQ3Key( &e.key.keysym, qtrue );

				if ( key == K_ENTER && keys[K_ALT].down ) {
					Cvar_Set("r_fullscreen", va("%i", glw_state.isFullscreen ? 0 : 2));
					Cbuf_AddText( "vid_restart\n" );
					break;
				}

				if ( key ) {
					Com_QueueEvent( in_eventTime, SE_KEY, key, qtrue, 0, NULL );

					if ( key == K_BACKSPACE )
						Com_QueueEvent( in_eventTime, SE_CHAR, CTRL('h'), 0, 0, NULL );
					else if ( key == K_ESCAPE )
						Com_QueueEvent( in_eventTime, SE_CHAR, key, 0, 0, NULL );
					else if( keys[K_CTRL].down && key >= 'a' && key <= 'z' )
						Com_QueueEvent( in_eventTime, SE_CHAR, CTRL(key), 0, 0, NULL );
				}

				lastKeyDown = key;
				break;

			case SDL_KEYUP:
				if( ( key = IN_TranslateSDLToQ3Key( &e.key.keysym, qfalse ) ) ){
					Com_QueueEvent( in_eventTime, SE_KEY, key, qfalse, 0, NULL );
				}

				lastKeyDown = 0;
				break;

			case SDL_TEXTINPUT:
				if( lastKeyDown != K_CONSOLE )
				{
					char *c = e.text.text;

					key = IN_TranslateSDLToQ3Key( &e.key.keysym, qfalse );

					// Quick and dirty UTF-8 to UTF-32 conversion
					while ( *c )
					{
						int utf32 = 0;

						if( ( *c & 0x80 ) == 0 )
							utf32 = *c++;
						else if( ( *c & 0xE0 ) == 0xC0 ) // 110x xxxx
						{
							utf32 |= ( *c++ & 0x1F ) << 6;
							utf32 |= ( *c++ & 0x3F );
						}
						else if( ( *c & 0xF0 ) == 0xE0 ) // 1110 xxxx
						{
							utf32 |= ( *c++ & 0x0F ) << 12;
							utf32 |= ( *c++ & 0x3F ) << 6;
							utf32 |= ( *c++ & 0x3F );
						}
						else if( ( *c & 0xF8 ) == 0xF0 ) // 1111 0xxx
						{
							utf32 |= ( *c++ & 0x07 ) << 18;
							utf32 |= ( *c++ & 0x3F ) << 12;
							utf32 |= ( *c++ & 0x3F ) << 6;
							utf32 |= ( *c++ & 0x3F );
						}
						else
						{
							Com_DPrintf( "Unrecognised UTF-8 lead byte: 0x%x\n", (unsigned int)*c );
							c++;
						}

						if( utf32 != 0 )
						{
							if ( IN_IsConsoleKey( 0, utf32 ) ) {
								Com_QueueEvent( in_eventTime, SE_KEY, K_CONSOLE, qtrue, 0, NULL );
								Com_QueueEvent( in_eventTime, SE_KEY, K_CONSOLE, qfalse, 0, NULL );
							} else {
								Com_QueueEvent( in_eventTime, SE_CHAR, utf32, 0, 0, NULL );
							}
						}
					}
				}
				break;

			case SDL_MOUSEMOTION:
				if( mouseActive )
				{
					if( !e.motion.xrel && !e.motion.yrel )
						break;
					Com_QueueEvent( in_eventTime, SE_MOUSE, e.motion.xrel, e.motion.yrel, 0, NULL );
				}
				break;

			case SDL_MOUSEBUTTONDOWN:
			case SDL_MOUSEBUTTONUP:
				{
					int b;
					switch( e.button.button )
					{
						case SDL_BUTTON_LEFT:   b = K_MOUSE1;     break;
						case SDL_BUTTON_MIDDLE: b = K_MOUSE3;     break;
						case SDL_BUTTON_RIGHT:  b = K_MOUSE2;     break;
						case SDL_BUTTON_X1:     b = K_MOUSE4;     break;
						case SDL_BUTTON_X2:     b = K_MOUSE5;     break;
						default:                b = K_AUX1 + ( e.button.button - SDL_BUTTON_X2 + 1 ) % 16; break;
					}
					Com_QueueEvent( in_eventTime, SE_KEY, b,
						( e.type == SDL_MOUSEBUTTONDOWN ? qtrue : qfalse ), 0, NULL );
				}
				break;

			case SDL_MOUSEWHEEL:
				if( e.wheel.y > 0 )
				{
					Com_QueueEvent( in_eventTime, SE_KEY, K_MWHEELUP, qtrue, 0, NULL );
					Com_QueueEvent( in_eventTime, SE_KEY, K_MWHEELUP, qfalse, 0, NULL );
				}
				else if( e.wheel.y < 0 )
				{
					Com_QueueEvent( in_eventTime, SE_KEY, K_MWHEELDOWN, qtrue, 0, NULL );
					Com_QueueEvent( in_eventTime, SE_KEY, K_MWHEELDOWN, qfalse, 0, NULL );
				}
				break;

			case SDL_QUIT:
				Cbuf_ExecuteText( EXEC_NOW, "quit Closed window\n" );
				break;

			case SDL_WINDOWEVENT:
#ifdef DEBUG_EVENTS
				Com_Printf( "%4i %s\n", e.window.timestamp, eventName( e.window.event ) );
#endif
				switch ( e.window.event )
				{
					case SDL_WINDOWEVENT_MOVED:
						break;
					// window states:
					case SDL_WINDOWEVENT_HIDDEN:
					case SDL_WINDOWEVENT_MINIMIZED:		gw_active = qfalse; gw_minimized = qtrue; break;
					case SDL_WINDOWEVENT_SHOWN:
					case SDL_WINDOWEVENT_RESTORED:
					case SDL_WINDOWEVENT_MAXIMIZED:		gw_minimized = qfalse; break;
					// keyboard focus:
					case SDL_WINDOWEVENT_FOCUS_LOST:	lastKeyDown = 0; Key_ClearStates(); gw_active = qfalse; break;
					case SDL_WINDOWEVENT_FOCUS_GAINED:	lastKeyDown = 0; Key_ClearStates(); gw_active = qtrue; gw_minimized = qfalse;
						if ( re.SetColorMappings ) {
							re.SetColorMappings();
						}
						break;
					// mouse focus:
					case SDL_WINDOWEVENT_ENTER: mouse_focus = qtrue; break;
					case SDL_WINDOWEVENT_LEAVE: if ( glw_state.isFullscreen ) mouse_focus = qfalse; break;
				}
				break;
			default:
				break;
		}
	}
}


/*
===============
IN_Minimize

Minimize the game so that user is back at the desktop
===============
*/
static void IN_Minimize( void )
{
	SDL_MinimizeWindow( SDL_window );
}


/*
===============
IN_Frame
===============
*/
void IN_Frame( void ) {
	if ( Key_GetCatcher() & KEYCATCH_CONSOLE ) {
		// temporarily deactivate if not in the game and
		// running on the desktop with multimonitor configuration
		if ( !glw_state.isFullscreen || glw_state.monitorCount > 1 ) {
			IN_DeactivateMouse();
			return;
		}
	}

	if ( !gw_active || !mouse_focus || in_nograb->integer ) {
		IN_DeactivateMouse();
		return;
	}

	IN_ActivateMouse();
}


/*
===============
IN_Restart
===============
*/
static void IN_Restart( void )
{
	IN_Shutdown();
	IN_Init();
}


/*
===============
IN_Init
===============
*/
void IN_Init( void )
{
	if ( !SDL_WasInit( SDL_INIT_VIDEO ) )
	{
		Com_Error( ERR_FATAL, "IN_Init called before SDL_Init( SDL_INIT_VIDEO )" );
		return;
	}

	Com_DPrintf( "\n------- Input Initialization -------\n" );

	in_keyboardDebug = Cvar_Get( "in_keyboardDebug", "0", CVAR_ARCHIVE );
	in_forceCharset = Cvar_Get( "in_forceCharset", "1", CVAR_ARCHIVE );
	in_mouse = Cvar_Get( "in_mouse", "1", CVAR_ARCHIVE );
	cl_consoleKeys = Cvar_Get( "cl_consoleKeys", "~ ` 0x7e 0x60", CVAR_ARCHIVE );

	mouseAvailable = ( in_mouse->value != 0 ) ? qtrue : qfalse;

	SDL_StartTextInput();

	Cmd_AddCommand( "minimize", IN_Minimize );
	Cmd_AddCommand( "in_restart", IN_Restart );

	Com_DPrintf( "------------------------------------\n" );
}


/*
===============
IN_Shutdown
===============
*/
void IN_Shutdown( void )
{
	SDL_StopTextInput();

	IN_DeactivateMouse();

	mouseAvailable = qfalse;

	Cmd_RemoveCommand( "minimize" );
	Cmd_RemoveCommand( "in_restart" );
}
