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
// common.c -- misc functions used in client and server

#include "q_shared.h"
#include "qcommon.h"
#include <setjmp.h>
#ifndef _WIN32
#include <netinet/in.h>
#include <sys/stat.h> // umask
#include <sys/time.h>
#else
#include <winsock.h>
#endif

#include "../client/keys.h"

#define DEF_COMHUNKMEGS 768
#define DEF_COMZONEMEGS 32

static jmp_buf abortframe;	// an ERR_DROP occurred, exit the entire frame

int		CPU_Flags = 0;

static fileHandle_t logfile = FS_INVALID_HANDLE;

cvar_t	*com_developer;
cvar_t	*com_timescale;
#ifndef DEDICATED
cvar_t	*com_maxfps;
cvar_t	*com_maxfpsUnfocused;
#endif
static cvar_t *com_log;

#ifndef DEDICATED
cvar_t	*cl_packetdelay;
cvar_t	*com_cl_running;
#endif

cvar_t	*sv_paused;
cvar_t  *sv_packetdelay;
cvar_t	*com_sv_running;
cvar_t	*cl_selectedmod;
cvar_t	*cl_changeqvm;
cvar_t	*os_linux;
cvar_t	*os_windows;
cvar_t	*os_macos;

static int	lastTime;
int			com_frameTime;
static int	com_frameNumber;

qboolean	com_errorEntered = qfalse;
qboolean	com_fullyInitialized = qfalse;

// renderer window states
qboolean	gw_minimized = qfalse; // this will be always true for dedicated servers
#ifndef DEDICATED
qboolean	gw_active = qtrue;
#endif

static char com_errorMessage[ MAXPRINTMSG ];

static void Com_Shutdown( void );
static void Com_WriteConfig_f( void );

//============================================================================

static char	*rd_buffer;
static int	rd_buffersize;
static qboolean rd_flushing = qfalse;
static void	(*rd_flush)( const char *buffer );

void Com_BeginRedirect( char *buffer, int buffersize, void (*flush)(const char *) )
{
	if (!buffer || !buffersize || !flush)
		return;
	rd_buffer = buffer;
	rd_buffersize = buffersize;
	rd_flush = flush;

	*rd_buffer = '\0';
}


void Com_EndRedirect( void )
{
	if ( rd_flush ) {
		rd_flushing = qtrue;
		rd_flush( rd_buffer );
		rd_flushing = qfalse;
	}

	rd_buffer = NULL;
	rd_buffersize = 0;
	rd_flush = NULL;
}


/*
=============
Com_Printf

Both client and server can use this, and it will output
to the appropriate place.

A raw string should NEVER be passed as fmt, because of "%f" type crashers.
=============
*/
void FORMAT_PRINTF(1, 2) QDECL Com_Printf( const char *fmt, ... ) {
	static qboolean opening_qconsole = qfalse;
	va_list		argptr;
	char		msg[MAXPRINTMSG];
	int			len;

	va_start( argptr, fmt );
	len = Q_vsnprintf( msg, sizeof( msg ), fmt, argptr );
	va_end( argptr );
    
	if ( rd_buffer && !rd_flushing ) {
		if ( len + (int)strlen( rd_buffer ) > ( rd_buffersize - 1 ) ) {
			rd_flushing = qtrue;
			rd_flush( rd_buffer );
			rd_flushing = qfalse;
			*rd_buffer = '\0';
		}
		Q_strcat( rd_buffer, rd_buffersize, msg );
		return;
	}
	
#ifndef DEDICATED
    CL_ConsolePrint( msg );
#endif

	Sys_Print( msg );

	if ( com_log && com_log->integer ) {
		if ( logfile == FS_INVALID_HANDLE && FS_Initialized() && !opening_qconsole ) {
			const char *logName = "console.log";
			opening_qconsole = qtrue;
		    logfile = FS_FOpenFileAppend( logName );

			if ( logfile != FS_INVALID_HANDLE ) {
				Com_Printf( "Logging enabled\n" );
				FS_ForceFlush( logfile );
			} else {
				Com_Printf( S_COLOR_YELLOW "Logging failed\n" );
				Cvar_Set( "log", "0" );
			}

			opening_qconsole = qfalse;
		}
		if (logfile != FS_INVALID_HANDLE && FS_Initialized()) FS_Write( msg, len, logfile );
	}
}


/*
================
Com_DPrintf

A Com_Printf that only shows up if the "developer" cvar is set
================
*/
void FORMAT_PRINTF(1, 2) QDECL Com_DPrintf( const char *fmt, ... ) {
	va_list		argptr;
	char		msg[MAXPRINTMSG];

	if ( !com_developer || !com_developer->integer ) {
		return;			// don't confuse non-developers with techie stuff...
	}

	va_start( argptr,fmt );
	Q_vsnprintf( msg, sizeof( msg ), fmt, argptr );
	va_end( argptr );

	Com_Printf( S_COLOR_CYAN "%s", msg );
}


/*
=============
Com_Error

Both client and server can use this, and it will
do the appropriate things.
=============
*/
void NORETURN FORMAT_PRINTF(2, 3) QDECL Com_Error( errorParm_t code, const char *fmt, ... ) {
	va_list		argptr;
	static int	lastErrorTime;
	static int	errorCount;
	static qboolean	calledSysError = qfalse;
	int			currentTime;

	if ( com_errorEntered ) {
		if ( !calledSysError ) {
			calledSysError = qtrue;
			Sys_Error( "recursive error after: %s", com_errorMessage );
		}
	}

	com_errorEntered = qtrue;

	Cvar_Set("com_errorCode", va("%i", code));

	// if we are getting a solid stream of ERR_DROP, do an ERR_FATAL
	currentTime = Sys_Milliseconds();
	if ( currentTime - lastErrorTime < 100 ) {
		if ( ++errorCount > 3 ) {
			code = ERR_FATAL;
		}
	} else {
		errorCount = 0;
	}
	lastErrorTime = currentTime;

	va_start( argptr, fmt );
	Q_vsnprintf( com_errorMessage, sizeof( com_errorMessage ), fmt, argptr );
	va_end( argptr );

	if ( code != ERR_DISCONNECT ) {
		if ( code != ERR_FATAL ) {
			Cvar_Set( "com_errorMessage", com_errorMessage );
		}
	}

	Cbuf_Init();

	if ( code == ERR_DISCONNECT || code == ERR_SERVERDISCONNECT ) {
		VM_Forced_Unload_Start();
		SV_Shutdown( "Server disconnected" );
		Com_EndRedirect();
#ifndef DEDICATED
		CL_Disconnect( qfalse );
		CL_FlushMemory();
#endif
		VM_Forced_Unload_Done();

		com_errorEntered = qfalse;

		Q_longjmp( abortframe, 1 );
	} else if ( code == ERR_DROP ) {
		Com_Printf( "********************\nERROR: %s\n********************\n",
			com_errorMessage );
		VM_Forced_Unload_Start();
		SV_Shutdown( va( "Server crashed: %s",  com_errorMessage ) );
		Com_EndRedirect();
#ifndef DEDICATED
		CL_Disconnect( qfalse );
		CL_FlushMemory();
#endif
		VM_Forced_Unload_Done();

		com_errorEntered = qfalse;

		Q_longjmp( abortframe, 1 );
	} else {
		VM_Forced_Unload_Start();
#ifndef DEDICATED
		CL_Shutdown( va( "Server fatal crashed: %s", com_errorMessage ), qtrue );
#endif
		SV_Shutdown( va( "Server fatal crashed: %s", com_errorMessage ) );
		Com_EndRedirect();
		VM_Forced_Unload_Done();
	}

	Com_Shutdown();

	calledSysError = qtrue;
	Sys_Error( "%s", com_errorMessage );
}


/*
=============
Com_Quit_f

Both client and server can use this, and it will
do the appropriate things.
=============
*/
void Com_Quit_f( void ) {
	const char *p = Cmd_ArgsFrom( 1 );
	// don't try to shutdown if we are in a recursive error
	if ( !com_errorEntered ) {
		// Some VMs might execute "quit" command directly,
		// which would trigger an unload of active VM error.
		// Sys_Quit will kill this process anyways, so
		// a corrupt call stack makes no difference
		VM_Forced_Unload_Start();
		SV_Shutdown( p[0] ? p : "Server quit" );
#ifndef DEDICATED
		CL_Shutdown( p[0] ? p : "Client quit", qtrue );
#endif
		VM_Forced_Unload_Done();
		Com_Shutdown();
		FS_Shutdown( qtrue );
	}
	Sys_Quit();
}


/*
============================================================================

COMMAND LINE FUNCTIONS

+ characters separate the commandLine string into multiple console
command lines.

All of these are valid:

quake3 +set test blah +map test
quake3 set test blah+map test
quake3 set test blah + map test

============================================================================
*/

#define	MAX_CONSOLE_LINES	32
static int	com_numConsoleLines;
static char	*com_consoleLines[MAX_CONSOLE_LINES];

/*
==================
Com_ParseCommandLine

Break it up into multiple console lines
==================
*/
static void Com_ParseCommandLine( char *commandLine ) {
	static int parsed = 0;
	int inq;

	if ( parsed )
		return;

	inq = 0;
	com_consoleLines[0] = commandLine;

	while ( *commandLine ) {
		if (*commandLine == '"') {
			inq = !inq;
		}
		// look for a + separating character
		// if commandLine came from a file, we might have real line separators
		if ( (*commandLine == '+' && !inq) || *commandLine == '\n'  || *commandLine == '\r' ) {
			if ( com_numConsoleLines == MAX_CONSOLE_LINES ) {
				break;
			}
			com_consoleLines[com_numConsoleLines] = commandLine + 1;
			com_numConsoleLines++;
			*commandLine = '\0';
		}
		commandLine++;
	}
	parsed = 1;
}

void Info_Print( const char *s ) {
	char	key[BIG_INFO_KEY];
	char	value[BIG_INFO_VALUE];

	do {
		s = Info_NextPair( s, key, value );
		if ( key[0] == '\0' )
			break;

		if ( value[0] == '\0' )
			strcpy( value, "MISSING VALUE" );

		Com_Printf( "%-20s %s\n", key, value );

	} while ( *s != '\0' );
}

/*
============
Com_StringContains
============
*/
static const char *Com_StringContains( const char *str1, const char *str2, int len2 ) {
	int len, i, j;

	len = strlen(str1) - len2;
	for (i = 0; i <= len; i++, str1++) {
		for (j = 0; str2[j]; j++) {
			if (locase[(byte)str1[j]] != locase[(byte)str2[j]]) {
				break;
			}
		}
		if (!str2[j]) {
			return str1;
		}
	}
	return NULL;
}

/*
============
Com_Filter
============
*/
int Com_Filter( const char *filter, const char *name )
{
	char buf[ MAX_TOKEN_CHARS ];
	const char *ptr;
	int i, found;

	while(*filter) {
		if (*filter == '*') {
			filter++;
			for (i = 0; *filter; i++) {
				if (*filter == '*' || *filter == '?')
					break;
				buf[i] = *filter;
				filter++;
			}
			buf[i] = '\0';
			if ( i ) {
				ptr = Com_StringContains( name, buf, i );
				if ( !ptr )
					return qfalse;
				name = ptr + i;
			}
		}
		else if (*filter == '?') {
			filter++;
			name++;
		}
		else if (*filter == '[' && *(filter+1) == '[') {
			filter++;
		}
		else if (*filter == '[') {
			filter++;
			found = qfalse;
			while(*filter && !found) {
				if (*filter == ']' && *(filter+1) != ']') break;
				if (*(filter+1) == '-' && *(filter+2) && (*(filter+2) != ']' || *(filter+3) == ']')) {
					if (locase[(byte)*name] >= locase[(byte)*filter] &&
						locase[(byte)*name] <= locase[(byte)*(filter+2)])
							found = qtrue;
					filter += 3;
				}
				else {
					if (locase[(byte)*filter] == locase[(byte)*name])
						found = qtrue;
					filter++;
				}
			}
			if (!found) return qfalse;
			while(*filter) {
				if (*filter == ']' && *(filter+1) != ']') break;
				filter++;
			}
			filter++;
			name++;
		}
		else {
			if (locase[(byte)*filter] != locase[(byte)*name])
				return qfalse;
			filter++;
			name++;
		}
	}
	return qtrue;
}

/*
============
Com_FilterExt
============
*/
qboolean Com_FilterExt( const char *filter, const char *name )
{
	char buf[ MAX_TOKEN_CHARS ];
	const char *ptr;
	int i;

	while ( *filter ) {
		if ( *filter == '*' ) {
			filter++;
			for ( i = 0; *filter != '\0' && i < sizeof(buf)-1; i++ ) {
				if ( *filter == '*' || *filter == '?' )
					break;
				buf[i] = *filter++;
			}
			buf[ i ] = '\0';
			if ( i ) {
				ptr = Com_StringContains( name, buf, i );
				if ( !ptr )
					return qfalse;
				name = ptr + i;
			} else if ( *filter == '\0' ) {
				return qtrue;
			}
		}
		else if ( *filter == '?' ) {
			if ( *name == '\0' )
				return qfalse;
			filter++;
			name++;
		}
		else {
			if ( locase[(byte)*filter] != locase[(byte)*name] )
				return qfalse;
			filter++;
			name++;
		}
	}
	if ( *name ) {
		return qfalse;
	}
	return qtrue;
}


/*
============
Com_HasPatterns
============
*/
qboolean Com_HasPatterns( const char *str )
{
	int c;

	while ( (c = *str++) != '\0' )
	{
		if ( c == '*' || c == '?' )
		{
			return qtrue;
		}
	}

	return qfalse;
}


/*
============
Com_FilterPath
============
*/
int Com_FilterPath( const char *filter, const char *name )
{
	int i;
	char new_filter[MAX_QEXTENDEDPATH];
	char new_name[MAX_QEXTENDEDPATH];

	for (i = 0; i < MAX_QEXTENDEDPATH-1 && filter[i]; i++) {
		if ( filter[i] == '\\' || filter[i] == ':' ) {
			new_filter[i] = '/';
		}
		else {
			new_filter[i] = filter[i];
		}
	}
	new_filter[i] = '\0';
	for (i = 0; i < MAX_QEXTENDEDPATH-1 && name[i]; i++) {
		if ( name[i] == '\\' || name[i] == ':' ) {
			new_name[i] = '/';
		}
		else {
			new_name[i] = name[i];
		}
	}
	new_name[i] = '\0';
	return Com_Filter( new_filter, new_name );
}


/*
================
Com_RealTime
================
*/
int Com_RealTime(qtime_t *qtime) {
	time_t t;
	struct tm *tms;

	t = time(NULL);
	if (!qtime)
		return t;
	tms = localtime(&t);
	if (tms) {
		qtime->tm_sec = tms->tm_sec;
		qtime->tm_min = tms->tm_min;
		qtime->tm_hour = tms->tm_hour;
		qtime->tm_mday = tms->tm_mday;
		qtime->tm_mon = tms->tm_mon;
		qtime->tm_year = tms->tm_year;
		qtime->tm_wday = tms->tm_wday;
		qtime->tm_yday = tms->tm_yday;
		qtime->tm_isdst = tms->tm_isdst;
	}
	return t;
}


/*
================
Sys_Microseconds
================
*/
int64_t Sys_Microseconds( void )
{
#ifdef _WIN32
	static qboolean inited = qfalse;
	static LARGE_INTEGER base;
	static LARGE_INTEGER freq;
	LARGE_INTEGER curr;

	if ( !inited )
	{
		QueryPerformanceFrequency( &freq );
		QueryPerformanceCounter( &base );
		if ( !freq.QuadPart )
		{
			return (int64_t)Sys_Milliseconds() * 1000LL; // fallback
		}
		inited = qtrue;
		return 0;
	}

	QueryPerformanceCounter( &curr );

	return ((curr.QuadPart - base.QuadPart) * 1000000LL) / freq.QuadPart;
#else
	struct timeval curr;
	gettimeofday( &curr, NULL );

	return (int64_t)curr.tv_sec * 1000000LL + (int64_t)curr.tv_usec;
#endif
}


/*
==============================================================================

						ZONE MEMORY ALLOCATION

There is never any space between memblocks, and there will never be two
contiguous free memblocks.

The rover can be left pointing at a non-empty block

The zone calls are pretty much only used for small strings and structures,
all big things are allocated on the hunk.
==============================================================================
*/

#define	ZONEID	0x1d4a11
#define MINFRAGMENT	64

#define DIRECTION next
// we may have up to 4 lists to group free blocks by size
#define SMALL_SIZE	64
#define MEDIUM_SIZE	128

#define USE_STATIC_TAGS
#define USE_TRASH_TEST

#ifdef ZONE_DEBUG
typedef struct zonedebug_s {
	const char *label;
	const char *file;
	int line;
	int allocSize;
} zonedebug_t;
#endif

typedef struct memblock_s {
	struct memblock_s	*next, *prev;
	int			size;	// including the header and possibly tiny fragments
	memtag_t	tag;	// a tag of 0 is a free block
	int			id;		// should be ZONEID
#ifdef ZONE_DEBUG
	zonedebug_t d;
#endif
} memblock_t;

typedef struct freeblock_s {
	struct freeblock_s *prev;
	struct freeblock_s *next;
} freeblock_t;

typedef struct memzone_s {
	int		size;			// total bytes malloced, including header
	int		used;			// total bytes used
	memblock_t	blocklist;	// start / end cap for linked list
	memblock_t	dummy0;		// just to allocate some space before freelist
	freeblock_t	freelist_tiny;
	memblock_t	dummy1;
	freeblock_t	freelist_small;
	memblock_t	dummy2;
	freeblock_t	freelist_medium;
	memblock_t	dummy3;
	freeblock_t	freelist;
} memzone_t;

static int minfragment = MINFRAGMENT; // may be adjusted at runtime

// main zone for all "dynamic" memory allocation
static memzone_t *mainzone;

// we also have a small zone for small allocations that would only
// fragment the main zone (think of cvar and cmd strings)
static memzone_t *smallzone;

static void InitFree( freeblock_t *fb )
{
	memblock_t *block = (memblock_t*)( (byte*)fb - sizeof( memblock_t ) );
	Com_Memset( block, 0, sizeof( *block ) );
}


static void RemoveFree( memblock_t *block )
{
	freeblock_t *fb = (freeblock_t*)( block + 1 );
	freeblock_t *prev;
	freeblock_t *next;

#ifdef ZONE_DEBUG
	if ( fb->next == NULL || fb->prev == NULL || fb->next == fb || fb->prev == fb ) {
		Com_Error( ERR_FATAL, "RemoveFree: bad pointers fb->next: %p, fb->prev: %p\n", fb->next, fb->prev );
	}
#endif

	prev = fb->prev;
	next = fb->next;

	prev->next = next;
	next->prev = prev;
}

static void InsertFree( memzone_t *zone, memblock_t *block )
{
	freeblock_t *fb = (freeblock_t*)( block + 1 );
	freeblock_t *prev, *next;
#ifdef TINY_SIZE
	if ( block->size <= TINY_SIZE )
		prev = &zone->freelist_tiny;
	else
#endif
#ifdef SMALL_SIZE
	if ( block->size <= SMALL_SIZE )
		prev = &zone->freelist_small;
	else
#endif
#ifdef MEDIUM_SIZE
	if ( block->size <= MEDIUM_SIZE )
		prev = &zone->freelist_medium;
	else
#endif
		prev = &zone->freelist;

	next = prev->next;

#ifdef ZONE_DEBUG
	if ( block->size < sizeof( *fb ) + sizeof( *block ) ) {
		Com_Error( ERR_FATAL, "InsertFree: bad block size: %i\n", block->size );
	}
#endif

	prev->next = fb;
	next->prev = fb;

	fb->prev = prev;
	fb->next = next;
}


/*
================
NewBlock

Allocates new free block within specified memory zone

Separator is needed to avoid additional runtime checks in Z_Free()
to prevent merging it with previous free block
================
*/
static freeblock_t *NewBlock( memzone_t *zone, int size )
{
	memblock_t *prev, *next;
	memblock_t *block, *sep;
	int alloc_size;

	// zone->prev is pointing on last block in the list
	prev = zone->blocklist.prev;
	next = prev->next;

	size = PAD( size, 1<<21 ); // round up to 2M blocks
	// allocate separator block before new free block
	alloc_size = size + sizeof( *sep );

	sep = (memblock_t *) calloc( alloc_size, 1 );
	if ( sep == NULL ) {
		Com_Error( ERR_FATAL, "Z_Malloc: failed on allocation of %i bytes from the %s zone",
			size, zone == smallzone ? "small" : "main" );
		return NULL;
	}
	block = sep+1;

	// link separator with prev
	prev->next = sep;
	sep->prev = prev;

	// link separator with block
	sep->next = block;
	block->prev = sep;

	// link block with next
	block->next = next;
	next->prev = block;

	sep->tag = TAG_GENERAL; // in-use block
	sep->id = -ZONEID;
	sep->size = 0;

	block->tag = TAG_FREE;
	block->id = ZONEID;
	block->size = size;

	// update zone statistics
	zone->size += alloc_size;
	zone->used += sizeof( *sep );

	InsertFree( zone, block );

	return (freeblock_t*)( block + 1 );
}


static memblock_t *SearchFree( memzone_t *zone, int size )
{
	const freeblock_t *fb;
	memblock_t *base;

#ifdef TINY_SIZE
	if ( size <= TINY_SIZE )
		fb = zone->freelist_tiny.DIRECTION;
	else
#endif
#ifdef SMALL_SIZE
	if ( size <= SMALL_SIZE )
		fb = zone->freelist_small.DIRECTION;
	else
#endif
#ifdef MEDIUM_SIZE
	if ( size <= MEDIUM_SIZE )
		fb = zone->freelist_medium.DIRECTION;
	else
#endif
		fb = zone->freelist.DIRECTION;

	for ( ;; ) {
		// not found, allocate new segment?
		if ( fb == &zone->freelist ) {
			fb = NewBlock( zone, size );
		} else {
#ifdef TINY_SIZE
			if ( fb == &zone->freelist_tiny ) {
				fb = zone->freelist_small.DIRECTION;
				continue;
			}
#endif
#ifdef SMALL_SIZE
			if ( fb == &zone->freelist_small ) {
				fb = zone->freelist_medium.DIRECTION;
				continue;
			}
#endif
#ifdef MEDIUM_SIZE
			if ( fb == &zone->freelist_medium ) {
				fb = zone->freelist.DIRECTION;
				continue;
			}
#endif
		}
		base = (memblock_t*)( (byte*) fb - sizeof( *base ) );
		fb = fb->DIRECTION;
		if ( base->size >= size ) {
			return base;
		}
	}
	return NULL;
}


/*
========================
Z_ClearZone
========================
*/
static void Z_ClearZone( memzone_t *zone, memzone_t *head, int size, int segnum ) {
	memblock_t	*block;
	int min_fragment;

	min_fragment = sizeof( memblock_t ) + sizeof( freeblock_t );

	if ( minfragment < min_fragment ) {
		// in debug mode size of memblock_t may exceed MINFRAGMENT
		minfragment = PAD( min_fragment, sizeof( intptr_t ) );
		Com_DPrintf( "zone.minfragment adjusted to %i bytes\n", minfragment );
	}

	// set the entire zone to one free block
	zone->blocklist.next = zone->blocklist.prev = block = (memblock_t *)( zone + 1 );
	zone->blocklist.tag = TAG_GENERAL; // in use block
	zone->blocklist.id = -ZONEID;
	zone->blocklist.size = 0;
	zone->size = size;
	zone->used = 0;

	block->prev = block->next = &zone->blocklist;
	block->tag = TAG_FREE;	// free block
	block->id = ZONEID;

	block->size = size - sizeof(memzone_t);

	InitFree( &zone->freelist );
	zone->freelist.next = zone->freelist.prev = &zone->freelist;

	InitFree( &zone->freelist_medium );
	zone->freelist_medium.next = zone->freelist_medium.prev = &zone->freelist_medium;

	InitFree( &zone->freelist_small );
	zone->freelist_small.next = zone->freelist_small.prev = &zone->freelist_small;

	InitFree( &zone->freelist_tiny );
	zone->freelist_tiny.next = zone->freelist_tiny.prev = &zone->freelist_tiny;

	InsertFree( zone, block );
}


/*
========================
Z_AvailableZoneMemory
========================
*/
static int Z_AvailableZoneMemory( const memzone_t *zone ) {
	return (1024*1024*1024); // unlimited
}


/*
========================
Z_AvailableMemory
========================
*/
int Z_AvailableMemory( void ) {
	return Z_AvailableZoneMemory( mainzone );
}


static void MergeBlock( memblock_t *curr_free, const memblock_t *next )
{
	curr_free->size += next->size;
	curr_free->next = next->next;
	curr_free->next->prev = curr_free;
}


/*
========================
Z_Free
========================
*/
void Z_Free( void *ptr ) {
	memblock_t	*block, *other;
	memzone_t *zone;

	if (!ptr) {
		Com_Error( ERR_DROP, "Z_Free: NULL pointer" );
	}

	block = (memblock_t *) ( (byte *)ptr - sizeof(memblock_t));
	if (block->id != ZONEID) {
		Com_Error( ERR_FATAL, "Z_Free: freed a pointer without ZONEID" );
	}

	if (block->tag == TAG_FREE) {
		Com_Error( ERR_FATAL, "Z_Free: freed a freed pointer" );
	}

	// if static memory
#ifdef USE_STATIC_TAGS
	if (block->tag == TAG_STATIC) {
		return;
	}
#endif

	// check the memory trash tester
#ifdef USE_TRASH_TEST
	if ( *(int *)((byte *)block + block->size - 4 ) != ZONEID ) {
		Com_Error( ERR_FATAL, "Z_Free: memory block wrote past end" );
	}
#endif

	if ( block->tag == TAG_SMALL ) {
		zone = smallzone;
	} else {
		zone = mainzone;
	}

	zone->used -= block->size;

	// set the block to something that should cause problems
	// if it is referenced...
	Com_Memset( ptr, 0xaa, block->size - sizeof( *block ) );

	block->tag = TAG_FREE; // mark as free
	block->id = ZONEID;

	other = block->prev;
	if ( other->tag == TAG_FREE ) {
		RemoveFree( other );
		// merge with previous free block
		MergeBlock( other, block );
		block = other;
	}

	other = block->next;
	if ( other->tag == TAG_FREE ) {
		RemoveFree( other );
		// merge the next free block onto the end
		MergeBlock( block, other );
	}

	InsertFree( zone, block );
}


/*
================
Z_FreeTags
================
*/
int Z_FreeTags( memtag_t tag ) {
	int			count;
	memzone_t	*zone;
	memblock_t	*block, *freed;

	if ( tag == TAG_STATIC ) {
		Com_Error( ERR_FATAL, "Z_FreeTags( TAG_STATIC )" );
		return 0;
	} else if ( tag == TAG_SMALL ) {
		zone = smallzone;
	} else {
		zone = mainzone;
	}

	count = 0;
	for ( block = zone->blocklist.next ; ; ) {
		if ( block->tag == tag && block->id == ZONEID ) {
			if ( block->prev->tag == TAG_FREE )
				freed = block->prev;  // current block will be merged with previous
			else
				freed = block; // will leave in place
			Z_Free( (void*)( block + 1 ) );
			block = freed;
			count++;
		}
		if ( block->next == &zone->blocklist ) {
			break;	// all blocks have been hit
		}
		block = block->next;
	}

	return count;
}


/*
================
Z_TagMalloc
================
*/
#ifdef ZONE_DEBUG
void *Z_TagMallocDebug( int size, memtag_t tag, char *label, char *file, int line ) {
	int		allocSize;
#else
void *Z_TagMalloc( int size, memtag_t tag ) {
#endif
	int		extra;
	memblock_t *base;
	memzone_t *zone;

	if ( tag == TAG_FREE ) {
		Com_Error( ERR_FATAL, "Z_TagMalloc: tried to use with TAG_FREE" );
	}

	if ( tag == TAG_SMALL ) {
		zone = smallzone;
	} else {
		zone = mainzone;
	}

#ifdef ZONE_DEBUG
	allocSize = size;
#endif

	if ( size < (sizeof( freeblock_t ) ) ) {
		size = (sizeof( freeblock_t ) );
	}

	//
	// scan through the block list looking for the first free block
	// of sufficient size
	//
	size += sizeof( *base );	// account for size of block header
#ifdef USE_TRASH_TEST
	size += 4;					// space for memory trash tester
#endif

	size = PAD(size, sizeof(intptr_t));		// align to 32/64 bit boundary
	base = SearchFree( zone, size );

	RemoveFree( base );

	//
	// found a block big enough
	//
	extra = base->size - size;
	if ( extra >= minfragment ) {
		memblock_t *fragment;
		// there will be a free fragment after the allocated block
		fragment = (memblock_t *)( (byte *)base + size );
		fragment->size = extra;
		fragment->tag = TAG_FREE; // free block
		fragment->id = ZONEID;
		fragment->prev = base;
		fragment->next = base->next;
		fragment->next->prev = fragment;
		base->next = fragment;
		base->size = size;
		InsertFree( zone, fragment );
	}

	zone->used += base->size;

	base->tag = tag;			// no longer a free block
	base->id = ZONEID;

#ifdef ZONE_DEBUG
	base->d.label = label;
	base->d.file = file;
	base->d.line = line;
	base->d.allocSize = allocSize;
#endif

#ifdef USE_TRASH_TEST
	// marker for memory trash testing
	*(int *)((byte *)base + base->size - 4) = ZONEID;
#endif

	return (void *) ( base + 1 );
}


/*
========================
Z_Malloc
========================
*/
#ifdef ZONE_DEBUG
void *Z_MallocDebug( int size, char *label, char *file, int line ) {
#else
void *Z_Malloc( int size ) {
#endif
	void	*buf;

  //Z_CheckHeap ();	// DEBUG

#ifdef ZONE_DEBUG
	buf = Z_TagMallocDebug( size, TAG_GENERAL, label, file, line );
#else
	buf = Z_TagMalloc( size, TAG_GENERAL );
#endif
	Com_Memset( buf, 0, size );

	return buf;
}


/*
========================
S_Malloc
========================
*/
#ifdef ZONE_DEBUG
void *S_MallocDebug( int size, char *label, char *file, int line ) {
	return Z_TagMallocDebug( size, TAG_SMALL, label, file, line );
}
#else
void *S_Malloc( int size ) {
	return Z_TagMalloc( size, TAG_SMALL );
}
#endif

#ifdef USE_STATIC_TAGS

// static mem blocks to reduce a lot of small zone overhead
typedef struct memstatic_s {
	memblock_t b;
	byte mem[2];
} memstatic_t;

#define MEM_STATIC(chr) { { NULL, NULL, PAD(sizeof(memstatic_t),4), TAG_STATIC, ZONEID }, {chr,'\0'} }

static const memstatic_t emptystring =
	MEM_STATIC( '\0' );

static const memstatic_t numberstring[] = {
	MEM_STATIC( '0' ),
	MEM_STATIC( '1' ),
	MEM_STATIC( '2' ),
	MEM_STATIC( '3' ),
	MEM_STATIC( '4' ),
	MEM_STATIC( '5' ),
	MEM_STATIC( '6' ),
	MEM_STATIC( '7' ),
	MEM_STATIC( '8' ),
	MEM_STATIC( '9' )
};
#endif // USE_STATIC_TAGS

char *CopyString( const char *in ) {
	char *out;
#ifdef USE_STATIC_TAGS
	if ( in[0] == '\0' ) {
		return ((char *)&emptystring) + sizeof(memblock_t);
	} else if ( in[0] >= '0' && in[0] <= '9' && in[1] == '\0' ) {
		return ((char *)&numberstring[in[0]-'0']) + sizeof(memblock_t);
	}
#endif
	out = S_Malloc( strlen( in ) + 1 );
	strcpy( out, in );
	return out;
}

static	byte	*s_hunkData = NULL;
static	int		s_hunkTotal;
static  int     s_hunkUsed = 0;
static  int     s_hunkMark = 0;

static void Com_InitSmallZoneMemory( void ) {
	static byte s_buf[ 512 * 1024 ];
	int smallZoneSize;

	smallZoneSize = sizeof( s_buf );
	Com_Memset( s_buf, 0, smallZoneSize );
	smallzone = (memzone_t *)s_buf;
	Z_ClearZone( smallzone, smallzone, smallZoneSize, 1 );
}

static void Com_InitZoneMemory( void ) {
	int		mainZoneSize;

	mainZoneSize = DEF_COMZONEMEGS * 1024 * 1024;

	mainzone = calloc( mainZoneSize, 1 );
	if ( !mainzone ) {
		Com_Error( ERR_FATAL, "Zone data failed to allocate %i megs", mainZoneSize / (1024*1024) );
	}
	Z_ClearZone( mainzone, mainzone, mainZoneSize, 1 );
}

static void Com_Meminfo_f(void) { Com_Printf("Hunk_Alloc (used=%dmb, total=%dmb) \n", s_hunkUsed / 1024 / 1024, s_hunkTotal / 1024 / 1024); }

static void Com_InitHunkMemory(void) {
	if(FS_LoadStack() != 0) Com_Error(ERR_FATAL, "Hunk initialization failed. File system load stack not zero");

	s_hunkTotal = DEF_COMHUNKMEGS * 1024 * 1024;
	s_hunkData = calloc(s_hunkTotal + 63, 1);

	if(!s_hunkData) Com_Error(ERR_FATAL, "Hunk data failed to allocate %i megs", s_hunkTotal / (1024 * 1024));

	s_hunkData = PADP(s_hunkData, 64);
	s_hunkUsed = 0;

	Cmd_AddCommand("meminfo", Com_Meminfo_f);
}

int Hunk_MemoryRemaining(void) { return s_hunkTotal - s_hunkUsed; }

void Hunk_SetMark(void) { s_hunkMark = s_hunkUsed; }

void Hunk_ClearToMark(void) { s_hunkUsed = s_hunkMark; }

qboolean Hunk_CheckMark(void) {
	if(s_hunkMark) return qtrue;
	return qfalse;
}

void CL_ShutdownCGame(void);
void CL_ShutdownUI(void);
void SV_ShutdownGameProgs(void);

void Hunk_Clear(void) {
#ifndef DEDICATED
	CL_ShutdownCGame();
	CL_ShutdownUI();
#endif
	SV_ShutdownGameProgs();

	s_hunkUsed = 0;
	Com_Printf("Hunk_Clear: reset ok\n");
	VM_Clear();
}

void* Hunk_Alloc(int size) {
	void* buf;

	if(s_hunkData == NULL) Com_Error(ERR_FATAL, "Hunk_Alloc: Hunk memory system not initialized");

	size = PAD(size, 64);

	if(s_hunkUsed + size > s_hunkTotal) Com_Error(ERR_DROP, "Hunk_Alloc failed on %i (used=%dmb, total=%dmb) \n", size, s_hunkUsed / 1024 / 1024, s_hunkTotal / 1024 / 1024);

	buf = (void*)(s_hunkData + s_hunkUsed);
	s_hunkUsed += size;
	Com_Memset(buf, 0, size);
	return buf;
}

void* Hunk_AllocateTempMemory(int size) { return Z_Malloc(size); }

void Hunk_FreeTempMemory(void* buf) { Z_Free(buf); }

void Hunk_ClearTempMemory(void) {}

/*
========================================================================

EVENT LOOP

========================================================================
*/

#define MAX_QUED_EVENTS		128
#define MASK_QUED_EVENTS	( MAX_QUED_EVENTS - 1 )

static sysEvent_t			eventQue[ MAX_QUED_EVENTS ];
static sysEvent_t			*lastEvent = eventQue + MAX_QUED_EVENTS - 1;
static unsigned int			eventHead = 0;
static unsigned int			eventTail = 0;

/*
================
Sys_QueEvent

A time of 0 will get the current time
Ptr should either be null, or point to a block of data that can
be freed by the game later.
================
*/
void Sys_QueEvent( int evTime, sysEventType_t evType, int value, int value2, int ptrLength, void *ptr ) {
	sysEvent_t	*ev;

	if ( evTime == 0 ) {
		evTime = Sys_Milliseconds();
	}

	// try to combine all sequential mouse moves in one event
	if ( evType == SE_MOUSE && lastEvent->evType == SE_MOUSE && eventHead != eventTail ) {
		lastEvent->evValue += value;
		lastEvent->evValue2 += value2;
		lastEvent->evTime = evTime;
		return;
	}

	ev = &eventQue[ eventHead & MASK_QUED_EVENTS ];

	if ( eventHead - eventTail >= MAX_QUED_EVENTS ) {
		// we are discarding an event, but don't leak memory
		if ( ev->evPtr ) {
			Z_Free( ev->evPtr );
		}
		eventTail++;
	}

	eventHead++;

	ev->evTime = evTime;
	ev->evType = evType;
	ev->evValue = value;
	ev->evValue2 = value2;
	ev->evPtrLength = ptrLength;
	ev->evPtr = ptr;

	lastEvent = ev;
}


/*
================
Com_GetSystemEvent
================
*/
static sysEvent_t Com_GetSystemEvent( void ) {
	sysEvent_t  ev;
	const char	*s;
	int			evTime;

	// return if we have data
	if ( eventHead - eventTail > 0 )
		return eventQue[ ( eventTail++ ) & MASK_QUED_EVENTS ];

	Sys_SendKeyEvents();

	evTime = Sys_Milliseconds();

	// check for console commands
	s = Sys_ConsoleInput();
	if ( s ) {
		char  *b;
		int   len;

		len = strlen( s ) + 1;
		b = Z_Malloc( len );
		strcpy( b, s );
		Sys_QueEvent( evTime, SE_CONSOLE, 0, 0, len, b );
	}

	// return if we have data
	if ( eventHead - eventTail > 0 )
		return eventQue[ ( eventTail++ ) & MASK_QUED_EVENTS ];

	// create an empty event to return
	memset( &ev, 0, sizeof( ev ) );
	ev.evTime = evTime;

	return ev;
}

/*
=================
Com_RunAndTimeServerPacket
=================
*/
void Com_RunAndTimeServerPacket( const netadr_t *evFrom, msg_t *buf ) { SV_PacketEvent( evFrom, buf ); }


/*
=================
Com_EventLoop

Returns last event time
=================
*/
int Com_EventLoop( void ) {
	sysEvent_t	ev;

#ifndef DEDICATED
	byte		bufData[ MAX_MSGLEN_BUF ];
	msg_t		buf;

	MSG_Init( &buf, bufData, MAX_MSGLEN );
#endif // !DEDICATED

	while ( 1 ) {
		ev = Com_GetSystemEvent();

		// if no more events are available
		if ( ev.evType == SE_NONE ) {
			// manually send packet events for the loopback channel
#ifndef DEDICATED
			netadr_t evFrom;
			while ( NET_GetLoopPacket( NS_CLIENT, &evFrom, &buf ) ) {
				CL_PacketEvent( &evFrom, &buf );
			}
			while ( NET_GetLoopPacket( NS_SERVER, &evFrom, &buf ) ) {
				// if the server just shut down, flush the events
				if ( com_sv_running->integer ) {
					Com_RunAndTimeServerPacket( &evFrom, &buf );
				}
			}
#endif // !DEDICATED
			return ev.evTime;
		}

		switch ( ev.evType ) {
#ifndef DEDICATED
		case SE_KEY:
			CL_KeyEvent( ev.evValue, ev.evValue2, ev.evTime );
			break;
		case SE_CHAR:
			CL_CharEvent( ev.evValue );
			break;
		case SE_MOUSE:
			CL_MouseEvent( ev.evValue, ev.evValue2 /*, ev.evTime*/ );
			break;
		case SE_JOYSTICK_AXIS:
			CL_JoystickEvent( ev.evValue, ev.evValue2, ev.evTime );
			break;
#endif // !DEDICATED
		case SE_CONSOLE:
			Cbuf_AddText( (char *)ev.evPtr );
			Cbuf_AddText( "\n" );
			break;
		default:
				Com_Error( ERR_FATAL, "Com_EventLoop: bad event type %i", ev.evType );
			break;
		}

		// free any block data
		if ( ev.evPtr ) {
			Z_Free( ev.evPtr );
			ev.evPtr = NULL;
		}
	}

	return 0;	// never reached
}

int Com_Milliseconds( void ) {
	return Sys_Milliseconds();
}

/*
==================
Com_ExecuteCfg

For controlling environment variables
==================
*/
static void Com_ExecuteCfg( void ) {
	Cbuf_ExecuteText(EXEC_NOW, "exec default.sbscript\n");
	Cbuf_Execute();
	Cbuf_ExecuteText(EXEC_NOW, "exec " CONFIG_CFG "\n");
	Cbuf_Execute();
	Cbuf_ExecuteText(EXEC_NOW, "exec autoexec.sbscript\n");
	Cbuf_Execute();
}


/*
==================
Com_GameRestart

Change to a new mod properly with cleaning up cvars before switching.
==================
*/
void Com_GameRestart( int checksumFeed, qboolean clientRestart )
{
	static qboolean com_gameRestarting = qfalse;

	// make sure no recursion can be triggered
	if ( !com_gameRestarting && com_fullyInitialized )
	{
		com_gameRestarting = qtrue;
#ifndef DEDICATED
		if ( clientRestart )
		{
			CL_Disconnect( qfalse );
			CL_ShutdownAll();
			CL_ClearMemory(); // Hunk_Clear(); // -EC-
		}
#endif

		// Kill server if we have one
		if ( com_sv_running->integer )
			SV_Shutdown( "Game directory changed" );

		// Reset console command history
		Con_ResetHistory();

		// Shutdown FS early so Cvar_Restart will not reset old game cvars
		FS_Shutdown( qtrue );

		// Clean out any user and VM created cvars
		Cvar_Restart();

#ifndef DEDICATED
		if ( CL_GameSwitch() )
			CL_SystemInfoChanged( qfalse );
#endif

		FS_Restart( checksumFeed );

		// Load new configuration
		Com_ExecuteCfg();

#ifndef DEDICATED
		if ( clientRestart )
			CL_StartHunkUsers();
#endif

		com_gameRestarting = qfalse;
	}
}


/*
==================
Com_GameRestart_f

Expose possibility to change current running mod to the user
==================
*/
static void Com_GameRestart_f( void ) {	Com_GameRestart( 0, qtrue ); }

/*
=================
Com_Init
=================
*/
void Com_Init( char *commandLine ) {
	const char *s;
	int	qport;

	// get the initial time base
	Sys_Milliseconds();

	Com_Printf( "^5%s %s\n", ENGINE_VERSION, __DATE__ );

	if (Q_setjmp( abortframe )) Sys_Error ("Error during initialization");

	Com_InitSmallZoneMemory();
	Cvar_Init();

	// prepare enough of the subsystems to handle
	// cvar and command buffer management
	Com_ParseCommandLine( commandLine );

	Cbuf_Init();

	Com_InitZoneMemory();
	Cmd_Init();

	// get the developer cvar set as early as possible
	com_developer = Cvar_Get( "developer", "0", 0 );
	Cvar_Get( "sv_master1", "", CVAR_ARCHIVE );
	Cvar_Get( "sv_master2", "", CVAR_ARCHIVE );
	Cvar_Get( "sv_master3", "", CVAR_ARCHIVE );

	Com_InitKeyCommands();
	
	cl_selectedmod = Cvar_Get("cl_selectedmod", "default", CVAR_ARCHIVE | CVAR_SERVERINFO);
	cl_changeqvm = Cvar_Get("cl_changeqvm", "0", 0);

	#if defined(__linux__)
	os_linux = Cvar_Get("os_linux", "1", CVAR_ARCHIVE);
	#else
	os_linux = Cvar_Get("os_linux", "0", CVAR_ARCHIVE);
	#endif

	#if defined(_WIN32)
	os_windows = Cvar_Get("os_windows", "1", CVAR_ARCHIVE);
	#else
	os_windows = Cvar_Get("os_windows", "0", CVAR_ARCHIVE);
	#endif

	#if defined(__APPLE__)
	os_macos = Cvar_Get("os_macos", "1", CVAR_ARCHIVE);
	#else
	os_macos = Cvar_Get("os_macos", "0", CVAR_ARCHIVE);
	#endif

	FS_InitFilesystem();

	com_log = Cvar_Get( "log", "0", 0 );

	Com_ExecuteCfg();
	Com_InitHunkMemory();
	JS_Init();

	cvar_modifiedFlags &= ~CVAR_ARCHIVE;

#ifndef DEDICATED
	com_maxfps = Cvar_Get( "com_maxfps", "60", CVAR_ARCHIVE ); // try to force that in some light way
	com_maxfpsUnfocused = Cvar_Get( "com_maxfpsUnfocused", "60", CVAR_ARCHIVE );
#endif

	com_timescale = Cvar_Get( "timescale", "1", CVAR_CHEAT | CVAR_SYSTEMINFO );

#ifndef DEDICATED
	cl_packetdelay = Cvar_Get( "cl_packetdelay", "0", CVAR_CHEAT );
	com_cl_running = Cvar_Get( "cl_running", "0", 0 );
#endif

	sv_paused = Cvar_Get( "sv_paused", "0", 0 );
	sv_packetdelay = Cvar_Get( "sv_packetdelay", "0", CVAR_CHEAT );
	com_sv_running = Cvar_Get( "sv_running", "0", 0 );
	Cvar_Get( "com_errorMessage", "", 0 );

	gw_minimized = qfalse;

	Cmd_AddCommand( "quit", Com_Quit_f );
	Cmd_AddCommand( "writeconfig", Com_WriteConfig_f );
	Cmd_SetCommandCompletionFunc( "writeconfig", Cmd_CompleteWriteCfgName );
	Cmd_AddCommand( "game_restart", Com_GameRestart_f );

	Sys_Init();

	// Pick a random port value
	Com_RandomBytes( (byte*)&qport, sizeof( qport ) );
	Netchan_Init( qport & 0xffff );

	VM_Init();
	SV_Init();

#ifndef DEDICATED
	CL_Init();
	CL_StartHunkUsers();
#endif

	Com_FrameInit();

	com_fullyInitialized = qtrue;

	Com_Printf( "^2Common initialization complete\n" );

	NET_Init();
}

static void Com_WriteConfigToFile( const char *filename ) {
	fileHandle_t	f;

	f = FS_FOpenFileWrite( filename );
	if ( f == FS_INVALID_HANDLE ) {
		if ( !FS_ResetReadOnlyAttribute( filename ) || ( f = FS_FOpenFileWrite( filename ) ) == FS_INVALID_HANDLE ) {
			Com_Printf( "Couldn't write %s.\n", filename );
			return;
		}
	}

	FS_Printf( f, "//generated by sourcetech" Q_NEWLINE );
#ifndef DEDICATED
	Key_WriteBindings( f );
#endif
	Cvar_WriteVariables( f );
	FS_FCloseFile( f );
}


/*
===============
Com_WriteConfiguration

Writes key bindings and archived cvars to config file if modified
===============
*/
void Com_WriteConfiguration( void ) {
	if ( !com_fullyInitialized ) return;
	Com_WriteConfigToFile( CONFIG_CFG );
}


/*
===============
Com_WriteConfig_f

Write the config file to a specific name
===============
*/
static void Com_WriteConfig_f( void ) {
	char	filename[MAX_QEXTENDEDPATH];
	const char *ext;

	if ( Cmd_Argc() != 2 ) {
		Com_Printf( "Usage: writeconfig <filename>\n" );
		return;
	}

	Q_strncpyz( filename, Cmd_Argv(1), sizeof( filename ) );
	COM_DefaultExtension( filename, sizeof( filename ), ".sbscript" );

	if ( !FS_AllowedExtension( filename, qfalse, &ext ) ) {
		Com_Printf( "%s: Invalid filename extension: '%s'.\n", __func__, ext );
		return;
	}

	Com_Printf( "Writing %s.\n", filename );
	Com_WriteConfigToFile( filename );
}

static int Com_ModifyMsec( int msec ) {
	int		clampTime;

	if ( com_timescale->value ) msec *= com_timescale->value;
	if ( msec < 1 && com_timescale->value) msec = 1;

	clampTime = 5000;
	if ( msec > clampTime ) msec = clampTime;

	return msec;
}


/*
=================
Com_TimeVal
=================
*/
static int Com_TimeVal( int minMsec )
{
	int timeVal;

	timeVal = Com_Milliseconds() - com_frameTime;

	if ( timeVal >= minMsec )
		timeVal = 0;
	else
		timeVal = minMsec - timeVal;

	return timeVal;
}

/*
=================
Com_FrameInit
=================
*/
void Com_FrameInit( void )
{
	lastTime = com_frameTime = Com_Milliseconds();
}

/*
=================
Com_Frame
=================
*/
void Com_Frame( void ) {
#ifndef DEDICATED
	static int bias = 0;
#endif
	int	msec, realMsec, minMsec;
	int	sleepMsec;
	int	timeVal;
	int	timeValSV;

	if ( Q_setjmp( abortframe ) ) return;			// an ERR_DROP was thrown

	minMsec = 0; // silent compiler warning

	// we may want to spin here if things are going too fast
#ifdef DEDICATED
	minMsec = SV_FrameMsec();
#else
	if ( !gw_active && com_maxfpsUnfocused->integer > 0 )
		minMsec = 1000 / com_maxfpsUnfocused->integer;
	else if ( com_maxfps->integer > 0 )
		minMsec = 1000 / com_maxfps->integer;
	else 
	    minMsec = 1;

	timeVal = com_frameTime - lastTime;
	bias += timeVal - minMsec;

	if ( bias > minMsec )
		bias = minMsec;
		
	minMsec -= bias;
#endif

	// waiting for incoming packets
	do {
		if ( com_sv_running->integer ) {
			timeValSV = SV_SendQueuedPackets();
			timeVal = Com_TimeVal( minMsec );
			if ( timeValSV < timeVal )
				timeVal = timeValSV;
		} else {
			timeVal = Com_TimeVal( minMsec );
		}
		sleepMsec = timeVal;
#ifndef DEDICATED
		if ( timeVal > sleepMsec )
			Com_EventLoop();
#endif
		NET_Sleep( sleepMsec * 1000 - 500 );
	} while( Com_TimeVal( minMsec ) );

	lastTime = com_frameTime;
	com_frameTime = Com_EventLoop();
	realMsec = com_frameTime - lastTime;

	Cbuf_Execute();

	// mess with msec if needed
	msec = Com_ModifyMsec( realMsec );

	SV_Frame( msec );

#ifndef DEDICATED
	// client system
	Com_EventLoop();
	Cbuf_Execute();
	CL_Frame( msec, realMsec );
#endif

	NET_FlushPacketQueue( 0 );
	Cbuf_Wait();
	com_frameNumber++;
}


/*
=================
Com_Shutdown
=================
*/
static void Com_Shutdown( void ) {
	if ( logfile != FS_INVALID_HANDLE ) {
		FS_FCloseFile( logfile );
		logfile = FS_INVALID_HANDLE;
	}
}

//------------------------------------------------------------------------


/*
===========================================
command line completion
===========================================
*/

/*
==================
Field_Clear
==================
*/
void Field_Clear( field_t *edit ) {
	memset( edit->buffer, 0, sizeof( edit->buffer ) );
	edit->cursor = 0;
	edit->scroll = 0;
}

static const char *completionString;
static char shortestMatch[MAX_TOKEN_CHARS];
static int	matchCount;
// field we are working on, passed to Field_AutoComplete(&g_consoleCommand for instance)
static field_t *completionField;

/*
===============
FindMatches
===============
*/
static void FindMatches( const char *s ) {
	int		i, n;

	if ( Q_stricmpn( s, completionString, strlen( completionString ) ) ) {
		return;
	}
	matchCount++;
	if ( matchCount == 1 ) {
		Q_strncpyz( shortestMatch, s, sizeof( shortestMatch ) );
		return;
	}

	n = (int)strlen(s);
	// cut shortestMatch to the amount common with s
	for ( i = 0 ; shortestMatch[i] ; i++ ) {
		if ( i >= n ) {
			shortestMatch[i] = '\0';
			break;
		}

		if ( tolower(shortestMatch[i]) != tolower(s[i]) ) {
			shortestMatch[i] = '\0';
		}
	}
}


/*
===============
PrintMatches
===============
*/
static void PrintMatches( const char *s ) {
	if ( !Q_stricmpn( s, shortestMatch, strlen( shortestMatch ) ) ) {
		Com_Printf( "    %s\n", s );
	}
}


/*
===============
PrintCvarMatches
===============
*/
static void PrintCvarMatches( const char *s ) {
	char value[ TRUNCATE_LENGTH ];

	if ( !Q_stricmpn( s, shortestMatch, strlen( shortestMatch ) ) ) {
		Com_TruncateLongString( value, Cvar_VariableString( s ) );
		Com_Printf( "    %s = \"%s\"\n", s, value );
	}
}


/*
===============
Field_FindFirstSeparator
===============
*/
static const char *Field_FindFirstSeparator( const char *s )
{
	char c;
	while ( (c = *s) != '\0' ) {
		if ( c == ';' )
			return s;
		s++;
	}
	return NULL;
}


/*
===============
Field_AddSpace
===============
*/
static void Field_AddSpace( void )
{
	size_t len = strlen( completionField->buffer );
	if ( len && len < sizeof( completionField->buffer ) - 1 && completionField->buffer[ len - 1 ] != ' ' )
	{
		memcpy( completionField->buffer + len, " ", 2 );
		completionField->cursor = (int)(len + 1);
	}
}


/*
===============
Field_Complete
===============
*/
static qboolean Field_Complete( void )
{
	int completionOffset;

	if( matchCount == 0 )
		return qtrue;

	completionOffset = strlen( completionField->buffer ) - strlen( completionString );

	Q_strncpyz( &completionField->buffer[ completionOffset ], shortestMatch,
		sizeof( completionField->buffer ) - completionOffset );

	completionField->cursor = strlen( completionField->buffer );

	if( matchCount == 1 )
	{
		Field_AddSpace();
		return qtrue;
	}

	Com_Printf( "]%s\n", completionField->buffer );

	return qfalse;
}


/*
===============
Field_CompleteKeyname
===============
*/
void Field_CompleteKeyname( void )
{
	matchCount = 0;
	shortestMatch[ 0 ] = '\0';

	Key_KeynameCompletion( FindMatches );

	if ( !Field_Complete() )
		Key_KeynameCompletion( PrintMatches );
}


/*
===============
Field_CompleteKeyBind
===============
*/
void Field_CompleteKeyBind( int key )
{
	const char *value;
	int vlen;
	int blen;

	value = Key_GetBinding( key );
	if ( value == NULL || *value == '\0' )
		return;

	blen = (int)strlen( completionField->buffer );
	vlen = (int)strlen( value );

	if ( Field_FindFirstSeparator( (char*)value ) )
	{
		value = va( "\"%s\"", value );
		vlen += 2;
	}

	if ( vlen + blen > sizeof( completionField->buffer ) - 1 )
	{
		//vlen = sizeof( completionField->buffer ) - 1 - blen;
		return;
	}

	memcpy( completionField->buffer + blen, value, vlen + 1 );
	completionField->cursor = blen + vlen;

	Field_AddSpace();
}


static void Field_CompleteCvarValue( const char *value, const char *current )
{
	int vlen;
	int blen;

	if ( *value == '\0' )
		return;

	blen = (int)strlen( completionField->buffer );
	vlen = (int)strlen( value );

	if ( *current != '\0' ) {
		return;
	}

	if ( Field_FindFirstSeparator( (char*)value ) )
	{
		value = va( "\"%s\"", value );
		vlen += 2;
	}

	if ( vlen + blen > sizeof( completionField->buffer ) - 1 )
	{
		//vlen = sizeof( completionField->buffer ) - 1 - blen;
		return;
	}

	if ( blen > 1 )
	{
		if ( completionField->buffer[ blen-1 ] == '"' && completionField->buffer[ blen-2 ] == ' ' )
		{
			completionField->buffer[ blen-- ] = '\0'; // strip starting quote
		}
	}

	memcpy( completionField->buffer + blen, value, vlen + 1 );
	completionField->cursor = vlen + blen;

	Field_AddSpace();
}


/*
===============
Field_CompleteFilename
===============
*/
void Field_CompleteFilename( const char *dir, const char *ext, qboolean stripExt, int flags )
{
	matchCount = 0;
	shortestMatch[ 0 ] = '\0';

	FS_FilenameCompletion( dir, ext, stripExt, FindMatches, flags );

	if ( !Field_Complete() )
		FS_FilenameCompletion( dir, ext, stripExt, PrintMatches, flags );
}


/*
===============
Field_CompleteCommand
===============
*/
void Field_CompleteCommand( const char *cmd, qboolean doCommands, qboolean doCvars )
{
	int	completionArgument;

	// Skip leading whitespace and quotes
	cmd = Com_SkipCharset( cmd, " \"" );

	Cmd_TokenizeStringIgnoreQuotes( cmd );
	completionArgument = Cmd_Argc();

	// If there is trailing whitespace on the cmd
	if( *( cmd + strlen( cmd ) - 1 ) == ' ' )
	{
		completionString = "";
		completionArgument++;
	}
	else
		completionString = Cmd_Argv( completionArgument - 1 );

#ifndef DEDICATED
	// Unconditionally add a '\' to the start of the buffer
	if ( completionField->buffer[ 0 ] && completionField->buffer[ 0 ] != '\\' )
	{
		if( completionField->buffer[ 0 ] != '/' )
		{
			// Buffer is full, refuse to complete
			if ( strlen( completionField->buffer ) + 1 >= sizeof( completionField->buffer ) )
				return;

			memmove( &completionField->buffer[ 1 ],
				&completionField->buffer[ 0 ],
				strlen( completionField->buffer ) + 1 );
			completionField->cursor++;
		}

		completionField->buffer[ 0 ] = '\\';
	}
#endif

	if ( completionArgument > 1 )
	{
		const char *baseCmd = Cmd_Argv( 0 );
		const char *p;

#ifndef DEDICATED
			// This should always be true
			if ( baseCmd[ 0 ] == '\\' || baseCmd[ 0 ] == '/' )
				baseCmd++;
#endif

		if( ( p = Field_FindFirstSeparator( cmd ) ) != NULL )
		{
 			Field_CompleteCommand( p + 1, qtrue, qtrue ); // Compound command
		}
		else
		{
			qboolean argumentCompleted = Cmd_CompleteArgument( baseCmd, cmd, completionArgument );
			if ( ( matchCount == 1 || argumentCompleted ) && doCvars )
			{
				if ( cmd[0] == '/' || cmd[0] == '\\' )
					cmd++;
				Cmd_TokenizeString( cmd );
				Field_CompleteCvarValue( Cvar_VariableString( Cmd_Argv( 0 ) ), Cmd_Argv( 1 ) );
			}
		}
	}
	else
	{
		if ( completionString[0] == '\\' || completionString[0] == '/' )
			completionString++;

		matchCount = 0;
		shortestMatch[ 0 ] = '\0';

		if ( completionString[0] == '\0' ) {
			return;
		}

		if ( doCommands )
			Cmd_CommandCompletion( FindMatches );

		if ( doCvars )
			Cvar_CommandCompletion( FindMatches );

		if ( !Field_Complete() )
		{
			// run through again, printing matches
			if ( doCommands )
				Cmd_CommandCompletion( PrintMatches );

			if ( doCvars )
				Cvar_CommandCompletion( PrintCvarMatches );
		}
	}
}


/*
===============
Field_AutoComplete

Perform Tab expansion
===============
*/
void Field_AutoComplete( field_t *field )
{
	completionField = field;

	Field_CompleteCommand( completionField->buffer, qtrue, qtrue );
}


/*
==================
Com_RandomBytes

fills string array with len random bytes, preferably from the OS randomizer
==================
*/
void Com_RandomBytes( byte *string, int len )
{
	int i;

	if ( Sys_RandomBytes( string, len ) )
		return;

	Com_Printf( S_COLOR_YELLOW "Com_RandomBytes: using weak randomization\n" );
	srand( time( NULL ) );
	for( i = 0; i < len; i++ )
		string[i] = (unsigned char)( rand() % 256 );
}


static qboolean strgtr(const char *s0, const char *s1) {
	int l0, l1, i;

	l0 = strlen( s0 );
	l1 = strlen( s1 );

	if ( l1 < l0 ) {
		l0 = l1;
	}

	for( i = 0; i < l0; i++ ) {
		if ( s1[i] > s0[i] ) {
			return qtrue;
		}
		if ( s1[i] < s0[i] ) {
			return qfalse;
		}
	}
	return qfalse;
}


/*
==================
Com_SortList
==================
*/
static void Com_SortList( char **list, int n )
{
	const char *m;
	char *temp;
	int i, j;
	i = 0;
	j = n;
	m = list[ n >> 1 ];
	do {
		while ( strcmp( list[i], m ) < 0 ) i++;
		while ( strcmp( list[j], m ) > 0 ) j--;
		if ( i <= j ) {
			temp = list[i];
			list[i] = list[j];
			list[j] = temp;
			i++;
			j--;
		}
	}
	while ( i <= j );
	if ( j > 0 ) Com_SortList( list, j );
	if ( n > i ) Com_SortList( list+i, n-i );
}


/*
==================
Com_SortFileList
==================
*/
void Com_SortFileList( char **list, int nfiles, int fastSort )
{
	if ( nfiles > 1 && fastSort ) {
		Com_SortList( list, nfiles-1 );
	} else {
		int i, flag;
		do {
			flag = 0;
			for( i = 1; i < nfiles; i++ ) {
				if ( strgtr( list[i-1], list[i] ) ) {
					char *temp = list[i];
					list[i] = list[i-1];
					list[i-1] = temp;
					flag = 1;
				}
			}
		} while( flag );
	}
}
