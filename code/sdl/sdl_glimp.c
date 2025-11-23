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
#ifdef USE_VULKAN_API
#include <SDL_vulkan.h>
#endif

#include "../client/client.h"
#include "../renderercommon/tr_public.h"
#include "sdl_glw.h"
#include "sdl_icon.h"

#ifdef _WIN32
#include <windows.h>
#endif

typedef enum {
	RSERR_OK,
	RSERR_INVALID_FULLSCREEN,
	RSERR_INVALID_MODE,
	RSERR_FATAL_ERROR,
	RSERR_UNKNOWN
} rserr_t;

glwstate_t glw_state;

SDL_Window *SDL_window = NULL;
static SDL_GLContext SDL_glContext = NULL;
#ifdef USE_VULKAN_API
static PFN_vkGetInstanceProcAddr qvkGetInstanceProcAddr;
#endif

cvar_t *in_nograb;

/*
===============
GLimp_Shutdown
===============
*/
void GLimp_Shutdown( qboolean unloadDLL ) {
	IN_Shutdown();

	SDL_DestroyWindow( SDL_window );
	SDL_window = NULL;

	if ( glw_state.isFullscreen )
		SDL_WarpMouseGlobal( glw_state.desktop_width / 2, glw_state.desktop_height / 2 );

	if ( unloadDLL )
		SDL_QuitSubSystem( SDL_INIT_VIDEO );
}

static int FindNearestDisplay( int *x, int *y, int w, int h ) {
	const int cx = *x + w / 2;
	const int cy = *y + h / 2;
	int i, index, numDisplays;
	SDL_Rect *list, *m;

	index = -1; // selected display index

	numDisplays = SDL_GetNumVideoDisplays();
	if ( numDisplays <= 0 )
		return -1;

	glw_state.monitorCount = numDisplays;

	list = Z_Malloc( numDisplays * sizeof( list[0] ) );

	for ( i = 0; i < numDisplays; i++ ) {
		SDL_GetDisplayBounds( i, list + i );
	}

	// select display by window center intersection
	for ( i = 0; i < numDisplays; i++ ) {
		m = list + i;
		if ( cx >= m->x && cx < (m->x + m->w) && cy >= m->y && cy < (m->y + m->h) )
		{
			index = i;
			break;
		}
	}

	// select display by nearest distance between window center and display center
	if ( index == -1 ) {
		unsigned long nearest, dist;
		int dx, dy;
		nearest = ~0UL;
		for ( i = 0; i < numDisplays; i++ ) {
			m = list + i;
			dx = (m->x + m->w/2) - cx;
			dy = (m->y + m->h/2) - cy;
			dist = ( dx * dx ) + ( dy * dy );
			if ( dist < nearest )
			{
				nearest = dist;
				index = i;
			}
		}
	}

	// adjust x and y coordinates if needed
	if ( index >= 0 ) {
		m = list + index;
		if ( *x < m->x )
			*x = m->x;

		if ( *y < m->y )
			*y = m->y;
	}

	Z_Free( list );

	return index;
}

static SDL_HitTestResult SDL_HitTestFunc( SDL_Window *win, const SDL_Point *area, void *data ) {
	if ( Key_GetCatcher() & KEYCATCH_CONSOLE && keys[ K_ALT ].down )
		return SDL_HITTEST_DRAGGABLE;

	return SDL_HITTEST_NORMAL;
}

#define MAX_RESOLUTIONS 25

void GLW_DetectDisplayModes( int display ) {
    int numModes = SDL_GetNumDisplayModes(display);
    if (numModes < 1) {
        Com_Printf("SDL_GetNumDisplayModes failed: %s\n", SDL_GetError());
        return;
    }

    char modesStr[1024] = {0};
    char resList[MAX_RESOLUTIONS][16];
    int resCount = 0;

    for (int i = 0; i < numModes && resCount < MAX_RESOLUTIONS; ++i) {
        SDL_DisplayMode mode;
        if (SDL_GetDisplayMode(display, i, &mode) != 0)
            continue;

        char resStr[16];
        Com_sprintf(resStr, sizeof(resStr), "%dx%d", mode.w, mode.h);

        qboolean duplicate = qfalse;
        for (int j = 0; j < resCount; ++j) {
            if (Q_stricmp(resList[j], resStr) == 0) {
                duplicate = qtrue;
                break;
            }
        }

        if (!duplicate) {
            Q_strncpyz(resList[resCount++], resStr, sizeof(resList[0]));
        }
    }

    for (int i = 0; i < resCount; ++i) {
        Q_strcat(modesStr, sizeof(modesStr), resList[i]);
        if (i < resCount - 1)
            Q_strcat(modesStr, sizeof(modesStr), " ");
    }

    Cvar_Set("r_availableModes", modesStr);
}

/*
===============
GLimp_SetMode
===============
*/
static int GLW_SetMode( const char *resolution, int fullscreen ) {
	glconfig_t *config = glw_state.config;
	int perChannelColorBits;
	int colorBits[3], depthBits, stencilBits;
	SDL_DisplayMode desktopMode;
	int display;
	int x;
	int y;
	Uint32 flags = SDL_WINDOW_SHOWN;

#ifdef USE_VULKAN_API
	flags |= SDL_WINDOW_VULKAN;
	Com_Printf( "Initializing Vulkan display\n");
#endif
#ifdef USE_OPENGL_API
	flags |= SDL_WINDOW_OPENGL;
	Com_Printf( "Initializing OpenGL display\n");
#endif

	// If a window exists, note its display index
	if ( SDL_window != NULL ) {
		display = SDL_GetWindowDisplayIndex( SDL_window );
		if ( display < 0 ) {
			Com_DPrintf( "SDL_GetWindowDisplayIndex() failed: %s\n", SDL_GetError() );
		}
	} else {
		x = 30;
		y = 30;
		display = FindNearestDisplay( &x, &y, 640, 480 );
	}

	if ( display >= 0 && SDL_GetDesktopDisplayMode( display, &desktopMode ) == 0 ) {
		glw_state.desktop_width = desktopMode.w;
		glw_state.desktop_height = desktopMode.h;
	} else {
		glw_state.desktop_width = 640;
		glw_state.desktop_height = 480;
	}

	GLW_DetectDisplayModes(display);

	if(fullscreen >= 2){
		config->isFullscreen = qtrue;
		glw_state.isFullscreen = qtrue;
	} else {
		config->isFullscreen = qfalse;
		glw_state.isFullscreen = qfalse;
	}

	Com_Printf( "...setting resolution %s:", resolution );

	if ( !CL_GetModeInfo( &config->vidWidth, &config->vidHeight, &config->windowAspect, resolution, glw_state.desktop_width, glw_state.desktop_height, fullscreen ) ) {
		Com_Printf( " invalid resolution\n" );
		return RSERR_INVALID_MODE;
	}

	Com_Printf( " %d %d\n", config->vidWidth, config->vidHeight );

	// Destroy existing state if it exists
	if ( SDL_glContext != NULL ) {
		SDL_GL_DeleteContext( SDL_glContext );
		SDL_glContext = NULL;
	}

	if ( SDL_window != NULL ) {
		SDL_GetWindowPosition( SDL_window, &x, &y );
		Com_DPrintf( "Existing window at %dx%d before being destroyed\n", x, y );
		SDL_DestroyWindow( SDL_window );
		SDL_window = NULL;
	}

	gw_active = qfalse;
	gw_minimized = qtrue;

	if ( fullscreen >= 2 ) {
#ifdef MACOS_X
		flags |= SDL_WINDOW_FULLSCREEN_DESKTOP;
#else
		flags |= SDL_WINDOW_FULLSCREEN;
#endif
	} else if ( r_fullscreen->integer == 1 ) {
		flags |= SDL_WINDOW_BORDERLESS;
	}

	depthBits = 24;
	stencilBits = 8;
	perChannelColorBits = 8;

#ifndef USE_VULKAN_API
	SDL_GL_SetAttribute( SDL_GL_RED_SIZE, perChannelColorBits );
	SDL_GL_SetAttribute( SDL_GL_GREEN_SIZE, perChannelColorBits );
	SDL_GL_SetAttribute( SDL_GL_BLUE_SIZE, perChannelColorBits );
	SDL_GL_SetAttribute( SDL_GL_DEPTH_SIZE, depthBits );
	SDL_GL_SetAttribute( SDL_GL_STENCIL_SIZE, stencilBits );
	SDL_GL_SetAttribute( SDL_GL_MULTISAMPLEBUFFERS, 0 );
	SDL_GL_SetAttribute( SDL_GL_MULTISAMPLESAMPLES, 0 );
	SDL_GL_SetAttribute( SDL_GL_STEREO, 0 );
	SDL_GL_SetAttribute( SDL_GL_DOUBLEBUFFER, 1 );
#endif

	if ( ( SDL_window = SDL_CreateWindow( CLIENT_WINDOW_TITLE, x, y, config->vidWidth, config->vidHeight, flags ) ) == NULL ) {
		Com_DPrintf( "SDL_CreateWindow failed: %s\n", SDL_GetError() );
		return RSERR_FATAL_ERROR;
	}

	if ( fullscreen >= 2 ) {
		SDL_DisplayMode mode;

		mode.w = config->vidWidth;
		mode.h = config->vidHeight;
		mode.driverdata = NULL;

		if ( SDL_SetWindowDisplayMode( SDL_window, &mode ) < 0 ) {
			Com_DPrintf( "SDL_SetWindowDisplayMode failed: %s\n", SDL_GetError( ) );
			return RSERR_FATAL_ERROR;
		}

		if ( SDL_GetWindowDisplayMode( SDL_window, &mode ) >= 0 ) {
			config->displayFrequency = mode.refresh_rate;
			config->vidWidth = mode.w;
			config->vidHeight = mode.h;
		}
	}

#ifdef USE_OPENGL_API
	if ( !SDL_glContext ) {
		if ( ( SDL_glContext = SDL_GL_CreateContext( SDL_window ) ) == NULL ) {
			Com_DPrintf( "SDL_GL_CreateContext failed: %s\n", SDL_GetError( ) );
			SDL_DestroyWindow( SDL_window );
			SDL_window = NULL;
			return RSERR_FATAL_ERROR;
		}
	}

	if ( SDL_GL_SetSwapInterval( r_swapInterval->integer ) == -1 ) {
		Com_DPrintf( "SDL_GL_SetSwapInterval failed: %s\n", SDL_GetError( ) );
	}

	SDL_GL_GetAttribute( SDL_GL_RED_SIZE, &colorBits[0] );
	SDL_GL_GetAttribute( SDL_GL_GREEN_SIZE, &colorBits[1] );
	SDL_GL_GetAttribute( SDL_GL_BLUE_SIZE, &colorBits[2] );
	SDL_GL_GetAttribute( SDL_GL_DEPTH_SIZE, &config->depthBits );
	SDL_GL_GetAttribute( SDL_GL_STENCIL_SIZE, &config->stencilBits );

	config->colorBits = colorBits[0] + colorBits[1] + colorBits[2];
#endif

	Com_Printf( "Using %d color bits, %d depth, %d stencil display.\n",	config->colorBits, config->depthBits, config->stencilBits );

	if ( !SDL_window ) {
		Com_Printf( "Couldn't get a visual\n" );
		return RSERR_INVALID_MODE;
	}

	if ( r_fullscreen->integer == 1 )
		SDL_SetWindowHitTest( SDL_window, SDL_HitTestFunc, NULL );

#ifdef USE_VULKAN_API
		SDL_Vulkan_GetDrawableSize( SDL_window, &config->vidWidth, &config->vidHeight );
#endif
#ifdef USE_OPENGL_API
		SDL_GL_GetDrawableSize( SDL_window, &config->vidWidth, &config->vidHeight );
#endif

	// save render dimensions as renderer may change it in advance
	glw_state.window_width = config->vidWidth;
	glw_state.window_height = config->vidHeight;

	SDL_WarpMouseInWindow( SDL_window, glw_state.window_width / 2, glw_state.window_height / 2 );

	return RSERR_OK;
}

/*
===============
GLimp_StartDriverAndSetMode
===============
*/
static rserr_t GLimp_StartDriverAndSetMode( const char *resolution, int fullscreen, qboolean vulkan ) {
	rserr_t err;

	if ( fullscreen >= 2 && in_nograb->integer ) {
		Com_Printf( "Fullscreen not allowed with \\in_nograb 1\n");
		Cvar_Set( "r_fullscreen", "0" );
		r_fullscreen->modified = qfalse;
		fullscreen = 0;
	}

	if ( !SDL_WasInit( SDL_INIT_VIDEO ) ) {
		const char *driverName;

		if ( SDL_Init( SDL_INIT_VIDEO ) != 0 ) {
			Com_Printf( "SDL_Init( SDL_INIT_VIDEO ) FAILED (%s)\n", SDL_GetError() );
			return RSERR_FATAL_ERROR;
		}

		driverName = SDL_GetCurrentVideoDriver();

		Com_Printf( "SDL using driver \"%s\"\n", driverName );
	}

	err = GLW_SetMode( resolution, fullscreen );

	switch ( err ) {
		case RSERR_INVALID_FULLSCREEN:
			Com_Printf( "...WARNING: fullscreen unavailable in this resolution\n" );
			return err;
		case RSERR_INVALID_MODE:
			Com_Printf( "...WARNING: could not set the given resolution (%s)\n", resolution );
			return err;
		default:
			break;
	}

	return RSERR_OK;
}

/*
===============
GLimp_Init

This routine is responsible for initializing the OS specific portions
of OpenGL
===============
*/
void GLimp_Init( glconfig_t *config ) {
	rserr_t err;

#ifndef _WIN32
	InitSig();
#endif

	Com_DPrintf( "GLimp_Init()\n" );

	glw_state.config = config; // feedback renderer configuration

	in_nograb = Cvar_Get( "in_nograb", "0", 0 );
	Cvar_SetDescription( in_nograb, "Do not capture mouse in game, may be useful during online streaming." );

	r_swapInterval = Cvar_Get( "r_swapInterval", "0", CVAR_ARCHIVE | CVAR_LATCH );

	// Create the window and set up the context
	err = GLimp_StartDriverAndSetMode( r_resolution->string, r_fullscreen->integer, qfalse );
	if ( err != RSERR_OK ) {
		if ( err == RSERR_FATAL_ERROR ) {
			Com_Error( ERR_FATAL, "GLimp_Init() - could not load OpenGL subsystem" );
			return;
		}

		Com_Printf( "Setting resolution %s failed, falling back on 640x480\n", r_resolution->string );
		if ( GLimp_StartDriverAndSetMode( "640x480", r_fullscreen->integer, qfalse ) != RSERR_OK ) {
			// Nothing worked, give up
			Com_Error( ERR_FATAL, "GLimp_Init() - could not load OpenGL subsystem" );
			return;
		}
	}

	// This depends on SDL_INIT_VIDEO, hence having it here
	IN_Init();

	HandleEvents();

	Key_ClearStates();
}

/*
===============
GLimp_EndFrame

Responsible for doing a swapbuffers
===============
*/
void GLimp_EndFrame( void ) {
	SDL_GL_SwapWindow( SDL_window );
}

/*
===============
GL_GetProcAddress

Used by opengl renderers to resolve all qgl* function pointers
===============
*/
void *GL_GetProcAddress( const char *symbol ) {
	return SDL_GL_GetProcAddress( symbol );
}

#ifdef USE_VULKAN_API
/*
===============
VKimp_Init

This routine is responsible for initializing the OS specific portions
of Vulkan
===============
*/
void VKimp_Init( glconfig_t *config ) {
	rserr_t err;

#ifndef _WIN32
	InitSig();
#endif

	Com_DPrintf( "VKimp_Init()\n" );

	in_nograb = Cvar_Get( "in_nograb", "0", CVAR_ARCHIVE );
	Cvar_SetDescription( in_nograb, "Do not capture mouse in game, may be useful during online streaming." );

	r_swapInterval = Cvar_Get( "r_swapInterval", "0", CVAR_ARCHIVE | CVAR_LATCH );

	// feedback to renderer configuration
	glw_state.config = config;

	// Create the window and set up the context
	err = GLimp_StartDriverAndSetMode( r_resolution->string, r_fullscreen->integer, qtrue /* Vulkan */ );
	if ( err != RSERR_OK ){
		if ( err == RSERR_FATAL_ERROR ){
			Com_Error( ERR_FATAL, "VKimp_Init() - could not load Vulkan subsystem" );
			return;
		}

		Com_Printf( "Setting resolution %s failed, falling back on 640x480\n", r_resolution->string );

		err = GLimp_StartDriverAndSetMode( "640x480", r_fullscreen->integer, qtrue /* Vulkan */ );
		if( err != RSERR_OK ){
			// Nothing worked, give up
			Com_Error( ERR_FATAL, "VKimp_Init() - could not load Vulkan subsystem" );
			return;
		}
	}

	qvkGetInstanceProcAddr = SDL_Vulkan_GetVkGetInstanceProcAddr();

	if ( qvkGetInstanceProcAddr == NULL ){
		SDL_QuitSubSystem( SDL_INIT_VIDEO );
		Com_Error( ERR_FATAL, "VKimp_Init: qvkGetInstanceProcAddr is NULL" );
	}

	// This depends on SDL_INIT_VIDEO, hence having it here
	IN_Init();

	HandleEvents();

	Key_ClearStates();
}

/*
===============
VK_GetInstanceProcAddr
===============
*/
void *VK_GetInstanceProcAddr( VkInstance instance, const char *name ) {
	return qvkGetInstanceProcAddr( instance, name );
}

/*
===============
VK_CreateSurface
===============
*/
qboolean VK_CreateSurface( VkInstance instance, VkSurfaceKHR *surface ) {
	if ( SDL_Vulkan_CreateSurface( SDL_window, instance, surface ) == SDL_TRUE )
		return qtrue;
	else
		return qfalse;
}

/*
===============
VKimp_Shutdown
===============
*/
void VKimp_Shutdown( qboolean unloadDLL ) {
	IN_Shutdown();

	SDL_DestroyWindow( SDL_window );
	SDL_window = NULL;

	if ( glw_state.isFullscreen )
		SDL_WarpMouseGlobal( glw_state.desktop_width / 2, glw_state.desktop_height / 2 );

	if ( unloadDLL )
		SDL_QuitSubSystem( SDL_INIT_VIDEO );
}
#endif // USE_VULKAN_API

/*
================
GLW_HideFullscreenWindow
================
*/
void GLW_HideFullscreenWindow( void ) {
	if ( SDL_window && glw_state.isFullscreen ) {
		SDL_HideWindow( SDL_window );
	}
}

/*
===============
Sys_GetClipboardData
===============
*/
char *Sys_GetClipboardData( void ) {
#ifdef DEDICATED
	return NULL;
#else
	char *data = NULL;
	char *cliptext;

	if ( ( cliptext = SDL_GetClipboardText() ) != NULL ) {
		if ( cliptext[0] != '\0' ) {
			size_t bufsize = strlen( cliptext ) + 1;

			data = Z_Malloc( bufsize );
			Q_strncpyz( data, cliptext, bufsize );

			// find first listed char and set to '\0'
			strtok( data, "\n\r\b" );
		}
		SDL_free( cliptext );
	}
	return data;
#endif
}

/*
===============
Sys_SetClipboardBitmap
===============
*/
void Sys_SetClipboardBitmap( const byte *bitmap, int length ) {
#ifdef _WIN32
	HGLOBAL hMem;
	byte *ptr;

	if ( !OpenClipboard( NULL ) )
		return;

	EmptyClipboard();
	hMem = GlobalAlloc( GMEM_MOVEABLE | GMEM_DDESHARE, length );
	if ( hMem != NULL ) {
		ptr = ( byte* )GlobalLock( hMem );
		if ( ptr != NULL ) {
			memcpy( ptr, bitmap, length ); 
		}
		GlobalUnlock( hMem );
		SetClipboardData( CF_DIB, hMem );
	}
	CloseClipboard();
#endif
}
