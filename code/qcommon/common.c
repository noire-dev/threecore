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

#if ARCH == x86
    #define DEF_COMHUNKMEGS 1023
#elif ARCH == x86_64
    #define DEF_COMHUNKMEGS 2047
#else
    #define DEF_COMHUNKMEGS 2047  // defaults
#endif
#define DEF_COMZONEMEGS			12

static jmp_buf abortframe;	// an ERR_DROP occurred, exit the entire frame

int		CPU_Flags = 0;

static fileHandle_t logfile = FS_INVALID_HANDLE;
static fileHandle_t com_journalFile = FS_INVALID_HANDLE ; // events are written here
fileHandle_t com_journalDataFile = FS_INVALID_HANDLE; // config files are written here

cvar_t	*com_developer;
cvar_t	*com_timescale;
static cvar_t *com_fixedtime;
cvar_t	*com_journal;
#ifndef DEDICATED
cvar_t	*com_maxfps;
cvar_t	*com_maxfpsUnfocused;
cvar_t	*com_yieldCPU;
#endif
#ifdef USE_AFFINITY_MASK
cvar_t	*com_affinityMask;
#endif
static cvar_t *com_logfile;		// 1 = buffer log, 2 = flush after each print
cvar_t	*com_version;

#ifndef DEDICATED
cvar_t	*cl_paused;
cvar_t	*cl_packetdelay;
cvar_t	*com_cl_running;
#endif

cvar_t	*sv_paused;
cvar_t  *sv_packetdelay;
cvar_t	*com_sv_running;
cvar_t	*cl_selectedmod;
cvar_t	*cl_changeqvm;
cvar_t	*os_32bit;
cvar_t	*os_linux;
cvar_t	*os_windows;
cvar_t	*os_macos;

cvar_t	*com_cameraMode;
#if defined(_WIN32) && defined(_DEBUG)
cvar_t	*com_noErrorInterrupt;
#endif

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
void CIN_CloseAllVideos( void );

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
		// TTimo nooo .. that would defeat the purpose
		//rd_flush(rd_buffer);
		//*rd_buffer = '\0';
		return;
	}

#ifndef DEDICATED
    CL_ConsolePrint( msg );
#endif

	// echo to dedicated console and early console
	Sys_Print( msg );

	// logfile
	if ( com_logfile && com_logfile->integer ) {
		// TTimo: only open the qconsole.log if the filesystem is in an initialized state
		//   also, avoid recursing in the qconsole.log opening (i.e. if fs_debug is on)
		if ( logfile == FS_INVALID_HANDLE && FS_Initialized() && !opening_qconsole ) {
			const char *logName = "qconsole.log";
			int mode;

			opening_qconsole = qtrue;

			mode = com_logfile->integer - 1;

			if ( mode & 2 )
				logfile = FS_FOpenFileAppend( logName );
			else
				logfile = FS_FOpenFileWrite( logName );

			if ( logfile != FS_INVALID_HANDLE ) {
				struct tm *newtime;
				time_t aclock;
				char timestr[32];

				time( &aclock );
				newtime = localtime( &aclock );
				strftime( timestr, sizeof( timestr ), "%a %b %d %X %Y", newtime );

				Com_Printf( "logfile opened on %s\n", timestr );

				if ( mode & 1 ) {
					// force it to not buffer so we get valid
					// data even if we are crashing
					FS_ForceFlush( logfile );
				}
			} else {
				Com_Printf( S_COLOR_YELLOW "Opening %s failed!\n", logName );
				Cvar_Set( "logfile", "0" );
			}

			opening_qconsole = qfalse;
		}
		if ( logfile != FS_INVALID_HANDLE && FS_Initialized() ) {
			FS_Write( msg, len, logfile );
		}
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

#if defined(_WIN32) && defined(_DEBUG)
	if ( code != ERR_DISCONNECT ) {
		if ( !com_noErrorInterrupt->integer ) {
			DebugBreak();
		}
	}
#endif

	if ( com_errorEntered ) {
		if ( !calledSysError ) {
			calledSysError = qtrue;
			Sys_Error( "recursive error after: %s", com_errorMessage );
		}
	}

	com_errorEntered = qtrue;

	Cvar_SetIntegerValue( "com_errorCode", code );

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

/*
===============
Com_StartupVariable

Searches for command line parameters that are set commands.
If match is not NULL, only that cvar will be looked for.
That is necessary because cddir and basedir need to be set
before the filesystem is started, but all other sets should
be after execing the config and default.
===============
*/
void Com_StartupVariable( const char *match ) {
	int i;
	const char *name;

	for ( i = 0; i < com_numConsoleLines; i++ ) {
		Cmd_TokenizeString( com_consoleLines[i] );
		if ( Q_stricmp( Cmd_Argv( 1 ), "=" ) ) {
			continue;
		}

		name = Cmd_Argv( 0 );
		if ( !match || Q_stricmp( name, match ) == 0 )
			Cvar_Set( name, Cmd_ArgsFrom( 2 ) );
	}
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

#define MAX_ALLOCATIONS 65535

typedef struct {
    void* ptr;
    int tag;
    size_t size;
} allocation_t;

static allocation_t allocations[MAX_ALLOCATIONS];
static int num_allocations = 0;
static size_t total_size = 0;

static int find_free_slot(void) {
    for (int i = 0; i < MAX_ALLOCATIONS; i++) {
        if (allocations[i].ptr == NULL) {
            return i;
        }
    }
    return -1;
}

static int find_pointer_slot(void* ptr) {
    for (int i = 0; i < MAX_ALLOCATIONS; i++) {
        if (allocations[i].ptr == ptr) {
            return i;
        }
    }
    return -1;
}

void* Z_TagMalloc(int size, int tag) {
    void* ptr = malloc(size);
    if (!ptr){
        Com_Printf("Allocate ptr ERROR!\n");
        return NULL;
    }
    
    int slot = find_free_slot();
    if (slot == -1) {
        Com_Printf("Allocate slot ERROR!\n");
        free(ptr);
        return NULL;
    }
    
    allocations[slot].ptr = ptr;
    allocations[slot].tag = tag;
    allocations[slot].size = size;
    
    total_size += size;
    num_allocations++;
    
    Com_Printf("Allocating %.2fmb, total = %.2fmb, slot = %i\n",
           (float)size / (1024.0f * 1024.0f),
           (float)total_size / (1024.0f * 1024.0f),
           slot);
    
    return ptr;
}

void* Z_Malloc(int size) {
    return Z_TagMalloc(size, TAG_GENERAL);
}

void Z_Free(void* ptr) {
    if (!ptr) return;
    
    int slot = find_pointer_slot(ptr);
    if (slot != -1) {
        total_size -= allocations[slot].size;
        allocations[slot].ptr = NULL;
        num_allocations--;
    }
    
    free(ptr);
}

void Z_TagFree(int tag) {
    for (int i = 0; i < MAX_ALLOCATIONS; i++) {
        if (allocations[i].ptr && allocations[i].tag == tag) {
            free(allocations[i].ptr);
            allocations[i].ptr = NULL;
        }
    }
}

void Z_ClearAll(void) {
    for (int i = 0; i < MAX_ALLOCATIONS; i++) {
        if (allocations[i].ptr) {
            free(allocations[i].ptr);
            allocations[i].ptr = NULL;
        }
    }
    total_size = 0;
    num_allocations = 0;
}

void *S_Malloc( int size ) {
	return Z_TagMalloc( size, TAG_SMALL );
}

/*
========================
CopyString

 NOTE:	never write over the memory CopyString returns because
		memory from a memstatic_t might be returned
========================
*/
char *CopyString( const char *in ) {
	char *out;
#ifdef USE_STATIC_TAGS
	if ( in[0] == '\0' ) {
		return ((char *)&emptystring) + sizeof(memblock_t);
	}
	else if ( in[0] >= '0' && in[0] <= '9' && in[1] == '\0' ) {
		return ((char *)&numberstring[in[0]-'0']) + sizeof(memblock_t);
	}
#endif
	out = S_Malloc( strlen( in ) + 1 );
	strcpy( out, in );
	return out;
}

void CL_ShutdownCGame( void );
void CL_ShutdownUI( void );
void SV_ShutdownGameProgs( void );

void Hunk_Clear( void ) {
#ifndef DEDICATED
	CL_ShutdownCGame();
	CL_ShutdownUI();
#endif
	SV_ShutdownGameProgs();
#ifndef DEDICATED
	CIN_CloseAllVideos();
#endif

	Com_Printf( "Hunk_Clear: reset ok\n" );
	VM_Clear();
}

/*
===================================================================

EVENTS AND JOURNALING

In addition to these events, .cfg files are also copied to the
journaled file
===================================================================
*/

#define	MAX_PUSHED_EVENTS 256
static int com_pushedEventsHead = 0;
static int com_pushedEventsTail = 0;
static sysEvent_t com_pushedEvents[MAX_PUSHED_EVENTS];


/*
=================
Com_InitJournaling
=================
*/
static void Com_InitJournaling( void ) {
	if ( !com_journal->integer ) {
		return;
	}

	if ( com_journal->integer == 1 ) {
		Com_Printf( "Journaling events\n" );
		com_journalFile = FS_FOpenFileWrite( "journal.dat" );
		com_journalDataFile = FS_FOpenFileWrite( "journaldata.dat" );
	} else if ( com_journal->integer == 2 ) {
		Com_Printf( "Replaying journaled events\n" );
		FS_FOpenFileRead( "journal.dat", &com_journalFile, qtrue );
		FS_FOpenFileRead( "journaldata.dat", &com_journalDataFile, qtrue );
	}

	if ( com_journalFile == FS_INVALID_HANDLE || com_journalDataFile == FS_INVALID_HANDLE ) {
		Cvar_Set( "com_journal", "0" );
		if ( com_journalFile != FS_INVALID_HANDLE ) {
			FS_FCloseFile( com_journalFile );
			com_journalFile = FS_INVALID_HANDLE;
		}
		if ( com_journalDataFile != FS_INVALID_HANDLE ) {
			FS_FCloseFile( com_journalDataFile );
			com_journalDataFile = FS_INVALID_HANDLE;
		}
		Com_Printf( "Couldn't open journal files\n" );
	}
}


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

static const char *Sys_EventName( sysEventType_t evType ) {

	static const char *evNames[ SE_MAX ] = {
		"SE_NONE",
		"SE_KEY",
		"SE_CHAR",
		"SE_MOUSE",
		"SE_JOYSTICK_AXIS",
		"SE_CONSOLE"
	};

	if ( (unsigned)evType >= ARRAY_LEN( evNames ) ) {
		return "SE_UNKNOWN";
	} else {
		return evNames[ evType ];
	}
}


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
		Com_Printf( "%s(type=%s,keys=(%i,%i),time=%i): overflow\n", __func__, Sys_EventName( evType ), value, value2, evTime );
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
Com_GetRealEvent
=================
*/
static sysEvent_t Com_GetRealEvent( void ) {

	// get or save an event from/to the journal file
	if ( com_journalFile != FS_INVALID_HANDLE ) {
		int			r;
		sysEvent_t	ev;

		if ( com_journal->integer == 2 ) {
			Sys_SendKeyEvents();
			r = FS_Read( &ev, sizeof(ev), com_journalFile );
			if ( r != sizeof(ev) ) {
				Com_Error( ERR_FATAL, "Error reading from journal file" );
			}
			if ( ev.evPtrLength ) {
				ev.evPtr = Z_Malloc( ev.evPtrLength );
				r = FS_Read( ev.evPtr, ev.evPtrLength, com_journalFile );
				if ( r != ev.evPtrLength ) {
					Com_Error( ERR_FATAL, "Error reading from journal file" );
				}
			}
		} else {
			ev = Com_GetSystemEvent();

			// write the journal value out if needed
			if ( com_journal->integer == 1 ) {
				r = FS_Write( &ev, sizeof(ev), com_journalFile );
				if ( r != sizeof(ev) ) {
					Com_Error( ERR_FATAL, "Error writing to journal file" );
				}
				if ( ev.evPtrLength ) {
					r = FS_Write( ev.evPtr, ev.evPtrLength, com_journalFile );
					if ( r != ev.evPtrLength ) {
						Com_Error( ERR_FATAL, "Error writing to journal file" );
					}
				}
			}
		}

		return ev;
	}

	return Com_GetSystemEvent();
}


/*
=================
Com_InitPushEvent
=================
*/
static void Com_InitPushEvent( void ) {
  // clear the static buffer array
  // this requires SE_NONE to be accepted as a valid but NOP event
  memset( com_pushedEvents, 0, sizeof(com_pushedEvents) );
  // reset counters while we are at it
  // beware: GetEvent might still return an SE_NONE from the buffer
  com_pushedEventsHead = 0;
  com_pushedEventsTail = 0;
}


/*
=================
Com_PushEvent
=================
*/
static void Com_PushEvent( const sysEvent_t *event ) {
	sysEvent_t		*ev;
	static int printedWarning = 0;

	ev = &com_pushedEvents[ com_pushedEventsHead & (MAX_PUSHED_EVENTS-1) ];

	if ( com_pushedEventsHead - com_pushedEventsTail >= MAX_PUSHED_EVENTS ) {

		// don't print the warning constantly, or it can give time for more...
		if ( !printedWarning ) {
			printedWarning = qtrue;
			Com_Printf( "WARNING: Com_PushEvent overflow\n" );
		}

		if ( ev->evPtr ) {
			Z_Free( ev->evPtr );
		}
		com_pushedEventsTail++;
	} else {
		printedWarning = qfalse;
	}

	*ev = *event;
	com_pushedEventsHead++;
}


/*
=================
Com_GetEvent
=================
*/
static sysEvent_t Com_GetEvent( void ) {
	if ( com_pushedEventsHead - com_pushedEventsTail > 0 ) {
		return com_pushedEvents[ (com_pushedEventsTail++) & (MAX_PUSHED_EVENTS-1) ];
	}
	return Com_GetRealEvent();
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
		ev = Com_GetEvent();

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


/*
================
Com_Milliseconds

Can be used for profiling, but will be journaled accurately
================
*/
int Com_Milliseconds( void ) {

	sysEvent_t	ev;

	// get events and push them until we get a null event with the current time
	do {
		ev = Com_GetRealEvent();
		if ( ev.evType != SE_NONE ) {
			Com_PushEvent( &ev );
		}
	} while ( ev.evType != SE_NONE );

	return ev.evTime;
}

//============================================================================

/*
=============
Com_Error_f

Just throw a fatal error to
test error shutdown procedures
=============
*/
static void __attribute__((__noreturn__)) Com_Error_f (void) {
	if ( Cmd_Argc() > 1 ) {
		Com_Error( ERR_DROP, "Testing drop error" );
	} else {
		Com_Error( ERR_FATAL, "Testing fatal error" );
	}
}


/*
=============
Com_Freeze_f

Just freeze in place for a given number of seconds to test
error recovery
=============
*/
static void Com_Freeze_f( void ) {
	int		s;
	int		start, now;

	if ( Cmd_Argc() != 2 ) {
		Com_Printf( "freeze <seconds>\n" );
		return;
	}
	s = atoi( Cmd_Argv(1) ) * 1000;

	start = Com_Milliseconds();

	while ( 1 ) {
		now = Com_Milliseconds();
		if ( now - start > s ) {
			break;
		}
	}
}


/*
=================
Com_Crash_f

A way to force a bus error for development reasons
=================
*/
static void Com_Crash_f( void ) {
	* ( volatile int * ) 0 = 0x12345678;
}


/*
==================
Com_ExecuteCfg

For controlling environment variables
==================
*/
static void Com_ExecuteCfg( void )
{
	Cbuf_ExecuteText(EXEC_NOW, "exec default.cfg\n");
	Cbuf_Execute(); // Always execute after exec to prevent text buffer overflowing

	Cbuf_ExecuteText(EXEC_NOW, "exec " CONFIG_CFG "\n");
	Cbuf_Execute();
	Cbuf_ExecuteText(EXEC_NOW, "exec autoexec.cfg\n");
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
		Cvar_Restart( qtrue );

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
** --------------------------------------------------------------------------------
**
** PROCESSOR STUFF
**
** --------------------------------------------------------------------------------
*/

#ifdef USE_AFFINITY_MASK
static uint64_t eCoreMask;
static uint64_t pCoreMask;
static uint64_t affinityMask; // saved at startup
#endif

#if (idx64 || id386)

#if defined _MSC_VER
#include <intrin.h>
static void CPUID( int func, unsigned int *regs )
{
	__cpuid( (int*)regs, func );
}

#ifdef USE_AFFINITY_MASK
#if idx64
extern void CPUID_EX( int func, int param, unsigned int *regs );
#else
void CPUID_EX( int func, int param, unsigned int *regs )
{
	__asm {
		push edi
		mov eax, func
		mov ecx, param
		cpuid
		mov edi, regs
		mov [edi +0], eax
		mov [edi +4], ebx
		mov [edi +8], ecx
		mov [edi+12], edx
		pop edi
	}
}
#endif // !idx64
#endif // USE_AFFINITY_MASK

#else // clang/gcc/mingw

static void CPUID( int func, unsigned int *regs )
{
	__asm__ __volatile__( "cpuid" :
		"=a"(regs[0]),
		"=b"(regs[1]),
		"=c"(regs[2]),
		"=d"(regs[3]) :
		"a"(func) );
}

#ifdef USE_AFFINITY_MASK
static void CPUID_EX( int func, int param, unsigned int *regs )
{
	__asm__ __volatile__( "cpuid" :
		"=a"(regs[0]),
		"=b"(regs[1]),
		"=c"(regs[2]),
		"=d"(regs[3]) :
		"a"(func),
		"c"(param) );
}
#endif // USE_AFFINITY_MASK

#endif  // clang/gcc/mingw

static void Sys_GetProcessorId( char *vendor )
{
	uint32_t regs[4]; // EAX, EBX, ECX, EDX
	uint32_t cpuid_level_ex;
	char vendor_str[12 + 1]; // short CPU vendor string

	// setup initial features
#if idx64
	CPU_Flags |= CPU_SSE | CPU_SSE2 | CPU_FCOM;
#else
	CPU_Flags = 0;
#endif
	vendor[0] = '\0';

	CPUID( 0x80000000, regs );
	cpuid_level_ex = regs[0];

	// get CPUID level & short CPU vendor string
	CPUID( 0x0, regs );
	memcpy(vendor_str + 0, (char*)&regs[1], 4);
	memcpy(vendor_str + 4, (char*)&regs[3], 4);
	memcpy(vendor_str + 8, (char*)&regs[2], 4);
	vendor_str[12] = '\0';

	// get CPU feature bits
	CPUID( 0x1, regs );

	// bit 15 of EDX denotes CMOV/FCMOV/FCOMI existence
	if ( regs[3] & ( 1 << 15 ) )
		CPU_Flags |= CPU_FCOM;

	// bit 23 of EDX denotes MMX existence
	if ( regs[3] & ( 1 << 23 ) )
		CPU_Flags |= CPU_MMX;

	// bit 25 of EDX denotes SSE existence
	if ( regs[3] & ( 1 << 25 ) )
		CPU_Flags |= CPU_SSE;

	// bit 26 of EDX denotes SSE2 existence
	if ( regs[3] & ( 1 << 26 ) )
		CPU_Flags |= CPU_SSE2;

	// bit 0 of ECX denotes SSE3 existence
	//if ( regs[2] & ( 1 << 0 ) )
	//	CPU_Flags |= CPU_SSE3;

	// bit 19 of ECX denotes SSE41 existence
	if ( regs[ 2 ] & ( 1 << 19 ) )
		CPU_Flags |= CPU_SSE41;

	if ( vendor ) {
		if ( cpuid_level_ex >= 0x80000004 ) {
			// read CPU Brand string
			uint32_t i;
			for ( i = 0x80000002; i <= 0x80000004; i++) {
				CPUID( i, regs );
				memcpy( vendor+0, (char*)&regs[0], 4 );
				memcpy( vendor+4, (char*)&regs[1], 4 );
				memcpy( vendor+8, (char*)&regs[2], 4 );
				memcpy( vendor+12, (char*)&regs[3], 4 );
				vendor[16] = '\0';
				vendor += strlen( vendor );
			}
		} else {
			const int print_flags = CPU_Flags;
			vendor = Q_stradd( vendor, vendor_str );
			if (print_flags) {
				// print features
				strcat(vendor, " w/");
				if (print_flags & CPU_FCOM)
					strcat(vendor, " CMOV");
				if (print_flags & CPU_MMX)
					strcat(vendor, " MMX");
				if (print_flags & CPU_SSE)
					strcat(vendor, " SSE");
				if (print_flags & CPU_SSE2)
					strcat(vendor, " SSE2");
				//if ( CPU_Flags & CPU_SSE3 )
				//	strcat( vendor, " SSE3" );
				if (print_flags & CPU_SSE41)
					strcat(vendor, " SSE4.1");
			}
		}
	}
}


#ifdef USE_AFFINITY_MASK
static void DetectCPUCoresConfig( void )
{
	uint32_t regs[4];
	uint32_t i;

	// get highest function parameter and vendor id
	CPUID( 0x0, regs );
	if ( regs[1] != 0x756E6547 || regs[2] != 0x6C65746E || regs[3] != 0x49656E69 || regs[0] < 0x1A ) {
		// non-intel signature or too low cpuid level - unsupported
		eCoreMask = pCoreMask = affinityMask;
		return;
	}

	eCoreMask = 0;
	pCoreMask = 0;

	for ( i = 0; i < sizeof( affinityMask ) * 8; i++ ) {
		const uint64_t mask = 1ULL << i;
		if ( (mask & affinityMask) && Sys_SetAffinityMask( mask ) ) {
			CPUID_EX( 0x1A, 0x0, regs );
			switch ( (regs[0] >> 24) & 0xFF ) {
				case 0x20: eCoreMask |= mask; break;
				case 0x40: pCoreMask |= mask; break;
				default: // non-existing leaf
					eCoreMask = pCoreMask = 0;
					break;
			}
		}
	}

	// restore original affinity
	Sys_SetAffinityMask( affinityMask );

	if ( pCoreMask == 0 || eCoreMask == 0 ) {
		// if either mask is empty - assume non-hybrid configuration
		eCoreMask = pCoreMask = affinityMask;
	}
}
#endif // USE_AFFINITY_MASK

#else // non-x86

#ifndef __linux__

static void Sys_GetProcessorId( char *vendor )
{
	Com_sprintf( vendor, 100, "%s", ARCH_STRING );
}

#else // __linux__

#include <sys/auxv.h>

#if arm32
#include <asm/hwcap.h>
#endif

static void Sys_GetProcessorId( char *vendor )
{
#if arm32
	const char *platform;
	long hwcaps;
	CPU_Flags = 0;

	platform = (const char*)getauxval( AT_PLATFORM );

	if ( !platform || *platform == '\0' ) {
		platform = "(unknown)";
	}

	if ( platform[0] == 'v' || platform[0] == 'V' ) {
		if ( atoi( platform + 1 ) >= 7 ) {
			CPU_Flags |= CPU_ARMv7;
		}
	}

	Com_sprintf( vendor, 100, "ARM %s", platform );
	hwcaps = getauxval( AT_HWCAP );
	if ( hwcaps & ( HWCAP_IDIVA | HWCAP_VFPv3 ) ) {
		strcat( vendor, " /w" );

		if ( hwcaps & HWCAP_IDIVA ) {
			CPU_Flags |= CPU_IDIVA;
			strcat( vendor, " IDIVA" );
		}

		if ( hwcaps & HWCAP_VFPv3 ) {
			CPU_Flags |= CPU_VFPv3;
			strcat( vendor, " VFPv3" );
		}

		if ( ( CPU_Flags & ( CPU_ARMv7 | CPU_VFPv3 ) ) == ( CPU_ARMv7 | CPU_VFPv3 ) ) {
			strcat( vendor, " QVM-bytecode" );
		}
	}
#else // !arm32
	CPU_Flags = 0;
#if arm64
	Com_sprintf( vendor, 100, "%s", ARCH_STRING );
#else
	Com_sprintf( vendor, 128, "%s %s", ARCH_STRING, (const char*)getauxval( AT_PLATFORM ) );
#endif
#endif // !arm32
}

#endif // __linux__

#endif // non-x86

#ifdef USE_AFFINITY_MASK

static int hex_code( const int code ) {
	if ( code >= '0' && code <= '9' ) {
		return code - '0';
	}
	if ( code >= 'A' && code <= 'F' ) {
		return code - 'A' + 10;
	}
	if ( code >= 'a' && code <= 'f' ) {
		return code - 'a' + 10;
	}
	return -1;
}


static const char *parseAffinityMask( const char *str, uint64_t *outv, int level ) {
	uint64_t v, mask = 0;

	while ( *str != '\0' ) {
		if ( *str == 'A' || *str == 'a' ) {
			mask = affinityMask;
			++str;
			continue;
		}
		else if ( *str == 'P' || *str == 'p' ) {
			mask = pCoreMask;
			++str;
			continue;
		}
		else if ( *str == 'E' || *str == 'e' ) {
			mask = eCoreMask;
			++str;
			continue;
		}
		else if ( *str == '0' && (str[1] == 'x' || str[1] == 'X') && (v = hex_code( str[2] )) >= 0 ) {
			int hex;
			str += 3; // 0xH
			while ( (hex = hex_code( *str )) >= 0 ) {
				v = v * 16 + hex;
				str++;
			}
			mask = v;
			continue;
		}
		else if ( *str >= '0' && *str <= '9' ) {
			mask = *str++ - '0';
			while ( *str >= '0' && *str <= '9' ) {
				mask = mask * 10 + *str - '0';
				++str;
			}
			continue;
		}

		if ( level == 0 ) {
			while ( *str == '+' || *str == '-' ) {
				str = parseAffinityMask( str + 1, &v, level + 1 );
				switch ( *str ) {
					case '+': mask |= v; break;
					case '-': mask &= ~v; break;
					default: str = ""; break;
				}
			}
			if ( *str != '\0' ) {
				++str; // skip unknown characters
			}
		} else {
			break;
		}
	}

	*outv = mask;
	return str;
}


// parse and set affinity mask
static void Com_SetAffinityMask( const char *str )
{
	uint64_t mask = 0;

	parseAffinityMask( str, &mask, 0 );

	if ( ( mask & affinityMask ) == 0 ) {
		mask = affinityMask; // reset to default
	}

	if ( mask != 0 ) {
		Sys_SetAffinityMask( mask );
	}
}
#endif // USE_AFFINITY_MASK


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

	Com_Printf( "%s %s %s\n", ENGINE_VERSION, PLATFORM_STRING, __DATE__ );

	if ( Q_setjmp( abortframe ) ) {
		Sys_Error ("Error during initialization");
	}

	// bk001129 - do this before anything else decides to push events
	Com_InitPushEvent();

	Cvar_Init();

#if defined(_WIN32) && defined(_DEBUG)
	com_noErrorInterrupt = Cvar_Get( "com_noErrorInterrupt", "0", 0 );
#endif

	// prepare enough of the subsystems to handle
	// cvar and command buffer management
	Com_ParseCommandLine( commandLine );

//	Swap_Init ();
	Cbuf_Init();

	// override anything from the config files with command line args
	Com_StartupVariable( NULL );

	Cmd_Init();

	// get the developer cvar set as early as possible
	Com_StartupVariable( "developer" );
	com_developer = Cvar_Get( "developer", "0", 0 );

	Com_StartupVariable( "journal" );
	com_journal = Cvar_Get( "journal", "0", CVAR_INIT );
	Cvar_SetDescription( com_journal, "When enabled, writes events and its data to 'journal.dat' and 'journaldata.dat'.");

	Com_StartupVariable( "sv_master1" );
	Com_StartupVariable( "sv_master2" );
	Com_StartupVariable( "sv_master3" );
	Cvar_Get( "sv_master1", "", CVAR_ARCHIVE );
	Cvar_Get( "sv_master2", "", CVAR_ARCHIVE );
	Cvar_Get( "sv_master3", "", CVAR_ARCHIVE );

	// done early so bind command exists
	Com_InitKeyCommands();
	
	cl_selectedmod = Cvar_Get("cl_selectedmod", "default", CVAR_ARCHIVE | CVAR_SERVERINFO);
	cl_changeqvm = Cvar_Get("cl_changeqvm", "0", 0);
	#if defined(__i386__)
	os_32bit = Cvar_Get("os_32bit", "1", CVAR_ARCHIVE);
	#else
	os_32bit = Cvar_Get("os_32bit", "0", CVAR_ARCHIVE);
	#endif

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

	com_logfile = Cvar_Get( "logfile", "0", 0 );
	Cvar_SetDescription( com_logfile, "System console logging:\n"
		" 0 - disabled\n"
		" 1 - overwrite mode, buffered\n"
		" 2 - overwrite mode, synced\n"
		" 3 - append mode, buffered\n"
		" 4 - append mode, synced\n" );

	Com_InitJournaling();

	Com_ExecuteCfg();

	// override anything from the config files with command line args
	Com_StartupVariable( NULL );

	// if any archived cvars are modified after this, we will trigger a writing
	// of the config file
	cvar_modifiedFlags &= ~CVAR_ARCHIVE;

	//
	// init commands and vars
	//
#ifndef DEDICATED
	com_maxfps = Cvar_Get( "com_maxfps", "60", 0 ); // try to force that in some light way
	Cvar_SetDescription( com_maxfps, "Sets maximum frames per second." );
	com_maxfpsUnfocused = Cvar_Get( "com_maxfpsUnfocused", "60", CVAR_ARCHIVE );
	Cvar_SetDescription( com_maxfpsUnfocused, "Sets maximum frames per second in unfocused game window." );
	com_yieldCPU = Cvar_Get( "com_yieldCPU", "1", CVAR_ARCHIVE );
	Cvar_SetDescription( com_yieldCPU, "Attempt to sleep specified amount of time between rendered frames when game is active, this will greatly reduce CPU load. Use 0 only if you're experiencing some lag." );
#endif

#ifdef USE_AFFINITY_MASK
	com_affinityMask = Cvar_Get( "com_affinityMask", "", CVAR_ARCHIVE );
	Cvar_SetDescription( com_affinityMask, "Bind game process to bitmask-specified CPU core(s), special characters:\n A or a - all default cores\n P or p - performance cores\n E or e - efficiency cores\n 0x<value> - use hexadecimal notation\n + or - can be used to add or exclude particular cores" );
	com_affinityMask->modified = qfalse;
#endif
	com_timescale = Cvar_Get( "timescale", "1", CVAR_CHEAT | CVAR_SYSTEMINFO );
	Cvar_SetDescription( com_timescale, "System timing factor:\n < 1: Slows the game down\n = 1: Regular speed\n > 1: Speeds the game up" );
	com_fixedtime = Cvar_Get( "fixedtime", "0", CVAR_CHEAT );
	Cvar_SetDescription( com_fixedtime, "Toggle the rendering of every frame the game will wait until each frame is completely rendered before sending the next frame." );
	com_cameraMode = Cvar_Get( "com_cameraMode", "0", CVAR_CHEAT );

#ifndef DEDICATED
	cl_paused = Cvar_Get( "cl_paused", "0", CVAR_ROM );
	Cvar_SetDescription( cl_paused, "Read-only CVAR to toggle functionality of paused games (the variable holds the status of the paused flag on the client side)." );
	cl_packetdelay = Cvar_Get( "cl_packetdelay", "0", CVAR_CHEAT );
	Cvar_SetDescription( cl_packetdelay, "Artificially set the client's latency. Simulates packet delay, which can lead to packet loss." );
	com_cl_running = Cvar_Get( "cl_running", "0", CVAR_ROM );
	Cvar_SetDescription( com_cl_running, "Can be used to check the status of the client game." );
#endif

	sv_paused = Cvar_Get( "sv_paused", "0", CVAR_ROM );
	sv_packetdelay = Cvar_Get( "sv_packetdelay", "0", CVAR_CHEAT );
	Cvar_SetDescription( sv_packetdelay, "Simulates packet delay, which can lead to packet loss. Server side." );
	com_sv_running = Cvar_Get( "sv_running", "0", CVAR_ROM );
	Cvar_SetDescription( com_sv_running, "Communicates to game modules if there is a server currently running." );

	Cvar_Get( "com_errorMessage", "", CVAR_ROM );

	gw_minimized = qfalse;

	if ( com_developer->integer ) {
		Cmd_AddCommand( "error", Com_Error_f );
		Cmd_AddCommand( "crash", Com_Crash_f );
		Cmd_AddCommand( "freeze", Com_Freeze_f );
	}

	Cmd_AddCommand( "quit", Com_Quit_f );
	Cmd_AddCommand( "changeVectors", MSG_ReportChangeVectors_f );
	Cmd_AddCommand( "writeconfig", Com_WriteConfig_f );
	Cmd_SetCommandCompletionFunc( "writeconfig", Cmd_CompleteWriteCfgName );
	Cmd_AddCommand( "game_restart", Com_GameRestart_f );

	s = va( "%s %s %s", ENGINE_VERSION, PLATFORM_STRING, __DATE__ );
	com_version = Cvar_Get( "version", s, CVAR_ROM | CVAR_SERVERINFO );
	Cvar_SetDescription( com_version, "Read-only CVAR to see the version of the game." );

	Sys_Init();

	// CPU detection
	Cvar_Get( "sys_cpustring", "detect", CVAR_ROM );
	if ( !Q_stricmp( Cvar_VariableString( "sys_cpustring" ), "detect" ) ) {
		char vendor[128];
		Com_Printf( "...detecting CPU, found " );
		Sys_GetProcessorId( vendor );
		Cvar_Set( "sys_cpustring", vendor );
	}
	Com_Printf( "%s\n", Cvar_VariableString( "sys_cpustring" ) );

#ifdef USE_AFFINITY_MASK
	// get initial process affinity - we will respect it when setting custom affinity masks
	eCoreMask = pCoreMask = affinityMask = Sys_GetAffinityMask();
#if (idx64 || id386)
	DetectCPUCoresConfig();
#endif
	if ( com_affinityMask->string[0] != '\0' ) {
		Com_SetAffinityMask( com_affinityMask->string );
		com_affinityMask->modified = qfalse;
	}
#endif

	// Pick a random port value
	Com_RandomBytes( (byte*)&qport, sizeof( qport ) );
	Netchan_Init( qport & 0xffff );

	VM_Init();
//#ifdef DEDICATED
	SV_Init();
//#endif

#ifndef DEDICATED
	CL_Init();
	CL_StartHunkUsers();
#endif

	// set com_frameTime so that if a map is started on the
	// command line it will still be able to count on com_frameTime
	// being random enough for a serverid
	// lastTime = com_frameTime = Com_Milliseconds();
	Com_FrameInit();

	com_fullyInitialized = qtrue;

	Com_Printf( "--- Common Initialization Complete ---\n" );

	NET_Init();

	Com_Printf( "Working directory: %s\n", Sys_Pwd() );
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
	// if we are quitting without fully initializing, make sure
	// we don't write out anything
	if ( !com_fullyInitialized ) {
		return;
	}

	if ( !(cvar_modifiedFlags & CVAR_ARCHIVE ) ) {
		return;
	}
	cvar_modifiedFlags &= ~CVAR_ARCHIVE;

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
	COM_DefaultExtension( filename, sizeof( filename ), ".cfg" );

	if ( !FS_AllowedExtension( filename, qfalse, &ext ) ) {
		Com_Printf( "%s: Invalid filename extension: '%s'.\n", __func__, ext );
		return;
	}

	Com_Printf( "Writing %s.\n", filename );
	Com_WriteConfigToFile( filename );
}


/*
================
Com_ModifyMsec
================
*/
static int Com_ModifyMsec( int msec ) {
	int		clampTime;

	// modify time for debugging values
	if ( com_fixedtime->integer ) {
		msec = com_fixedtime->integer;
	} else if ( com_timescale->value ) {
		msec *= com_timescale->value;
	} else if (com_cameraMode->integer) {
		msec *= com_timescale->value;
	}

	// don't let it scale below 1 msec
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

#ifdef USE_AFFINITY_MASK
	if ( com_affinityMask->modified ) {
		Com_SetAffinityMask( com_affinityMask->string );
		com_affinityMask->modified = qfalse;
	}
#endif

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
		if ( !gw_minimized && timeVal > com_yieldCPU->integer )
			sleepMsec = com_yieldCPU->integer;
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

//#ifdef DEDICATED
	SV_Frame( msec );
//#endif

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

	if ( com_journalFile != FS_INVALID_HANDLE ) {
		FS_FCloseFile( com_journalFile );
		com_journalFile = FS_INVALID_HANDLE;
	}

	if ( com_journalDataFile != FS_INVALID_HANDLE ) {
		FS_FCloseFile( com_journalDataFile );
		com_journalDataFile = FS_INVALID_HANDLE;
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
	do
	{
		while ( strcmp( list[i], m ) < 0 ) i++;
		while ( strcmp( list[j], m ) > 0 ) j--;
		if ( i <= j )
		{
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
	if ( nfiles > 1 && fastSort )
	{
		Com_SortList( list, nfiles-1 );
	}
	else // defrag mod demo UI can't handle _properly_ sorted directories
	{
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
