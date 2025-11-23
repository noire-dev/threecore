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
// win_main.c

#include "../qcommon/q_shared.h"
#include "../qcommon/qcommon.h"
#include "../client/client.h"
#include "win_local.h"
#include "resource.h"
#include <sys/types.h>
#include <sys/stat.h>
#include <errno.h>
#include <direct.h>
#include <io.h>
#include <fcntl.h>

#define MEM_THRESHOLD (96*1024*1024)

WinVars_t	g_wv;

/*
==================
Sys_LowPhysicalMemory
==================
*/
qboolean Sys_LowPhysicalMemory( void ) {
	MEMORYSTATUS stat;
	GlobalMemoryStatus( &stat );
	return (stat.dwTotalPhys <= MEM_THRESHOLD) ? qtrue : qfalse;
}

/*
=============
Sys_Error

Show the early console as an error dialog
=============
*/
void NORETURN FORMAT_PRINTF(1, 2) QDECL Sys_Error( const char *error, ... ) {
	va_list	argptr;
	char	text[4096];
	MSG		msg;

	va_start( argptr, error );
	Q_vsnprintf( text, sizeof( text ), error, argptr );
	va_end( argptr );

	CL_Shutdown( text, qtrue );

	timeEndPeriod( 1 );

	// wait for the user to quit
	while ( 1 ) {
		if ( GetMessage( &msg, NULL, 0, 0 ) <= 0 ) {
			Cmd_Clear();
			Com_Quit_f();
		}
		TranslateMessage( &msg );
		DispatchMessage( &msg );
	}

	exit( 1 );
}

/*
==============
Sys_Quit
==============
*/
void NORETURN Sys_Quit( void ) {
	timeEndPeriod( 1 );
	exit( 0 );
}

/*
==============
Sys_Print
==============
*/
void Sys_Print( const char *msg ) { return; }

/*
==============
Sys_ConsoleInput
==============
*/
char *Sys_ConsoleInput( void ) { return NULL; }

/*
==============
Sys_Mkdir
==============
*/
qboolean Sys_Mkdir( const char *path ) {
	if ( _mkdir( path ) == 0 ) {
		return qtrue;
	} else {
		if ( errno == EEXIST ) {
			return qtrue;
		} else {
			return qfalse;
		}
	}
}

/*
==============
Sys_FOpen
==============
*/
FILE *Sys_FOpen( const char *ospath, const char *mode ) {
	size_t length;

	// Windows API ignores all trailing spaces and periods which can get around Quake 3 file system restrictions.
	length = strlen( ospath );
	if ( length == 0 || ospath[length-1] == ' ' || ospath[length-1] == '.' ) {
		return NULL;
	}

	return fopen( ospath, mode );
}

/*
==============
Sys_ResetReadOnlyAttribute
==============
*/
qboolean Sys_ResetReadOnlyAttribute( const char *ospath ) {
	DWORD dwAttr;

	dwAttr = GetFileAttributesA( ospath );
	if ( dwAttr & FILE_ATTRIBUTE_READONLY ) {
		dwAttr &= ~FILE_ATTRIBUTE_READONLY;
		if ( SetFileAttributesA( ospath, dwAttr ) ) {
			return qtrue;
		} else {
			return qfalse;
		}
	} else {
		return qfalse;
	}
}

/*
==============
Sys_Pwd
==============
*/
const char *Sys_Pwd( void ) {
	static char pwd[ MAX_OSPATH ];
	TCHAR	buffer[ MAX_OSPATH ];
	char *s;

	if ( pwd[0] )
		return pwd;

	GetModuleFileName( NULL, buffer, ARRAY_LEN( buffer ) );
	buffer[ ARRAY_LEN( buffer ) - 1 ] = '\0';

	Q_strncpyz( pwd, WtoA( buffer ), sizeof( pwd ) );

	s = strrchr( pwd, PATH_SEP );
	if ( s ) {
		*s = '\0';
	} else {
		_getcwd( pwd, sizeof( pwd ) - 1 );
		pwd[ sizeof( pwd ) - 1 ] = '\0';
	}

	return pwd;
}

/*
==============
Sys_DefaultBasePath
==============
*/
const char *Sys_DefaultBasePath( void )
{
	return Sys_Pwd();
}

/*
==============================================================

DIRECTORY SCANNING

==============================================================
*/
void Sys_ListFilteredFiles( const char *basedir, const char *subdirs, const char *filter, char **list, int *numfiles ) {
	char		search[MAX_OSPATH*2+1];
	char		newsubdirs[MAX_OSPATH*2];
	char		filename[MAX_OSPATH*2];
	intptr_t	findhandle;
	struct _finddata_t findinfo;

	if ( *numfiles >= MAX_FOUND_FILES - 1 ) {
		return;
	}

	if ( *subdirs ) {
		Com_sprintf( search, sizeof(search), "%s\\%s\\*", basedir, subdirs );
	}
	else {
		Com_sprintf( search, sizeof(search), "%s\\*", basedir );
	}

	findhandle = _findfirst (search, &findinfo);
	if (findhandle == -1) {
		return;
	}

	do {
		if (findinfo.attrib & _A_SUBDIR) {
			if ( !Q_streq( findinfo.name, "." ) && !Q_streq( findinfo.name, ".." ) ) {
				if ( *subdirs ) {
					Com_sprintf( newsubdirs, sizeof(newsubdirs), "%s\\%s", subdirs, findinfo.name );
				} else {
					Com_sprintf( newsubdirs, sizeof(newsubdirs), "%s", findinfo.name );
				}
				Sys_ListFilteredFiles( basedir, newsubdirs, filter, list, numfiles );
			}
		}
		if ( *numfiles >= MAX_FOUND_FILES - 1 ) {
			break;
		}
		Com_sprintf( filename, sizeof(filename), "%s\\%s", subdirs, findinfo.name );
		if ( !Com_FilterPath( filter, filename ) )
			continue;
		list[ *numfiles ] = FS_CopyString( filename );
		(*numfiles)++;
	} while ( _findnext (findhandle, &findinfo) != -1 );

	_findclose (findhandle);
}

/*
=============
Sys_Sleep
=============
*/
void Sys_Sleep( int msec ) {
	
	if ( msec < 0 ) {
		// special case: wait for event or network packet
		DWORD dwResult;
		msec = 300;
		do {
			dwResult = MsgWaitForMultipleObjects( 0, NULL, FALSE, msec, QS_ALLEVENTS );
		} while ( dwResult == WAIT_TIMEOUT && NET_Sleep( 10 * 1000 ) );
		return;
	}

	Sleep ( msec );
}

/*
=============
Sys_ListFiles
=============
*/
char **Sys_ListFiles( const char *directory, const char *extension, const char *filter, int *numfiles, qboolean wantsubs ) {
	char		search[MAX_OSPATH*2+MAX_QEXTENDEDPATH+1];
	int			nfiles;
	char		**listCopy;
	char		*list[MAX_FOUND_FILES];
	struct _finddata_t findinfo;
	intptr_t	findhandle;
	int			flag;
	int			extLen;
	int			length;
	int			i;
	const char	*x;
	qboolean	hasPatterns;

	if ( filter ) {
		nfiles = 0;
		Sys_ListFilteredFiles( directory, "", filter, list, &nfiles );

		list[ nfiles ] = NULL;
		*numfiles = nfiles;

		if (!nfiles)
			return NULL;

		listCopy = Z_Malloc( ( nfiles + 1 ) * sizeof( listCopy[0] ) );
		for ( i = 0 ; i < nfiles ; i++ ) {
			listCopy[i] = list[i];
		}
		listCopy[i] = NULL;

		return listCopy;
	}

	if ( !extension ) {
		extension = "";
	}

	// passing a slash as extension will find directories
	if ( extension[0] == '/' && extension[1] == 0 ) {
		extension = "";
		flag = 0;
	} else {
		flag = _A_SUBDIR;
	}

	Com_sprintf( search, sizeof(search), "%s\\*%s", directory, extension );

	findhandle = _findfirst( search, &findinfo );
	if ( findhandle == -1 ) {
		*numfiles = 0;
		return NULL;
	}

	extLen = (int)strlen( extension );
	hasPatterns = Com_HasPatterns( extension );
	if ( hasPatterns && extension[0] == '.' && extension[1] != '\0' ) {
		extension++;
	}

	// search
	nfiles = 0;

	do {
		if ( (!wantsubs && flag ^ ( findinfo.attrib & _A_SUBDIR )) || (wantsubs && findinfo.attrib & _A_SUBDIR) ) {
			if ( nfiles == MAX_FOUND_FILES - 1 ) {
				break;
			}
			if ( *extension ) {
				if ( hasPatterns ) {
					x = strrchr( findinfo.name, '.' );
					if ( !x || !Com_FilterExt( extension, x+1 ) ) {
						continue;
					}
				} else {
					length = strlen( findinfo.name );
					if ( length < extLen || Q_stricmp( findinfo.name + length - extLen, extension ) ) {
						continue;
					}
				}
			}
			list[ nfiles ] = FS_CopyString( findinfo.name );
			nfiles++;
		}
	} while ( _findnext (findhandle, &findinfo) != -1 );

	list[ nfiles ] = NULL;

	_findclose (findhandle);

	// return a copy of the list
	*numfiles = nfiles;

	if ( !nfiles ) {
		return NULL;
	}

	listCopy = Z_Malloc( ( nfiles + 1 ) * sizeof( listCopy[0] ) );
	for ( i = 0 ; i < nfiles ; i++ ) {
		listCopy[i] = list[i];
	}
	listCopy[i] = NULL;

	Com_SortFileList( listCopy, nfiles, extension[0] != '\0' );

	return listCopy;
}

/*
=============
Sys_FreeFileList
=============
*/
void Sys_FreeFileList( char **list ) {
	int		i;

	if ( !list ) {
		return;
	}

	for ( i = 0 ; list[i] ; i++ ) {
		Z_Free( list[i] );
	}

	Z_Free( list );
}

/*
=============
Sys_GetFileStats
=============
*/
qboolean Sys_GetFileStats( const char *filename, fileOffset_t *size, fileTime_t *mtime, fileTime_t *ctime ) {
	struct _stat s;

	if ( _stat( filename, &s ) == 0 ) {
		*size = (fileOffset_t)s.st_size;
		*mtime = (fileTime_t)s.st_mtime;
		*ctime = (fileTime_t)s.st_ctime;
		return qtrue;
	} else {
		*size = 0;
		*mtime = *ctime = 0;
		return qfalse;
	}
}

/*
=================
Sys_SendKeyEvents

Platform-dependent event handling
=================
*/
void Sys_SendKeyEvents( void ) {
	if (!com_dedicated->integer)
		HandleEvents();
}

/*
==================
SetTimerResolution

Try to set lower timer period
==================
*/
static void SetTimerResolution( void ) {
	typedef HRESULT (WINAPI *pfnNtQueryTimerResolution)( PULONG MinRes, PULONG MaxRes, PULONG CurRes );
	typedef HRESULT (WINAPI *pfnNtSetTimerResolution)( ULONG NewRes, BOOLEAN SetRes, PULONG CurRes );
	pfnNtQueryTimerResolution pNtQueryTimerResolution;
	pfnNtSetTimerResolution pNtSetTimerResolution;
	ULONG curr, minr, maxr;
	HMODULE dll;

	dll = LoadLibrary( T( "ntdll" ) );
	if ( dll ) {
		pNtQueryTimerResolution = (pfnNtQueryTimerResolution) GetProcAddress( dll, "NtQueryTimerResolution" );
		pNtSetTimerResolution = (pfnNtSetTimerResolution) GetProcAddress( dll, "NtSetTimerResolution" );
		if ( pNtQueryTimerResolution && pNtSetTimerResolution ) {
			pNtQueryTimerResolution( &minr, &maxr, &curr );
			if ( maxr < 5000 ) // well, we don't need less than 0.5ms periods for select()
				maxr = 5000;
			pNtSetTimerResolution( maxr, TRUE, &curr );
		}
		FreeLibrary( dll );
	}
}

/*
================
Sys_Init

Called after the common systems (cvars, files, etc)
are initialized
================
*/
void Sys_Init( void ) {
	// make sure the timer is high precision, otherwise
	// NT gets 18ms resolution
	timeBeginPeriod( 1 );
	SetTimerResolution();
	Cvar_Set( "arch", "winnt" );
}

static const char *GetExceptionName( DWORD code ) {
	static char buf[ 32 ];

	switch ( code ) {
		case EXCEPTION_ACCESS_VIOLATION: return "ACCESS_VIOLATION";
		case EXCEPTION_DATATYPE_MISALIGNMENT: return "DATATYPE_MISALIGNMENT";
		case EXCEPTION_ARRAY_BOUNDS_EXCEEDED: return "ARRAY_BOUNDS_EXCEEDED";
		case EXCEPTION_PRIV_INSTRUCTION: return "PRIV_INSTRUCTION";
		case EXCEPTION_IN_PAGE_ERROR: return "IN_PAGE_ERROR";
		case EXCEPTION_ILLEGAL_INSTRUCTION: return "ILLEGAL_INSTRUCTION";
		case EXCEPTION_NONCONTINUABLE_EXCEPTION: return "NONCONTINUABLE_EXCEPTION";
		case EXCEPTION_STACK_OVERFLOW: return "STACK_OVERFLOW";
		case EXCEPTION_INVALID_DISPOSITION: return "INVALID_DISPOSITION";
		case EXCEPTION_GUARD_PAGE: return "GUARD_PAGE";
		case EXCEPTION_INVALID_HANDLE: return "INVALID_HANDLE";
		default: break;
	}

	sprintf( buf, "0x%08X", (unsigned int)code );
	return buf;
}

/*
==================
ExceptionFilter

Restore gamma and hide fullscreen window in case of crash
==================
*/
static LONG WINAPI ExceptionFilter( struct _EXCEPTION_POINTERS *ExceptionInfo ) {
	if (com_dedicated->integer == 0) {
		extern cvar_t *com_cl_running;
		if ( com_cl_running  && com_cl_running->integer ) {
			// assume we can restart client module
		} else {
			GLW_RestoreGamma();
			GLW_HideFullscreenWindow();
		}
	}

	if ( ExceptionInfo->ExceptionRecord->ExceptionCode != EXCEPTION_BREAKPOINT ) {
		char msg[128], name[MAX_OSPATH];
		const char *basename;
		HMODULE hModule, hKernel32;
		byte *addr;

		hModule = NULL;
		name[0] = '\0';
		basename = name;
		addr = (byte*)ExceptionInfo->ExceptionRecord->ExceptionAddress;

		hKernel32 = GetModuleHandleA( "kernel32" );
		if ( hKernel32 != NULL ) {
			typedef BOOL (WINAPI *PFN_GetModuleHandleExA)( DWORD dwFlags, LPCSTR lpModuleName, HMODULE *phModule );
			PFN_GetModuleHandleExA pGetModuleHandleExA;

			pGetModuleHandleExA = (PFN_GetModuleHandleExA) GetProcAddress( hKernel32, "GetModuleHandleExA" );
			if ( pGetModuleHandleExA != NULL ) {
				if ( pGetModuleHandleExA( GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCTSTR)addr, &hModule ) ) {
					if (GetModuleFileNameA( hModule, name, ARRAY_LEN(name) - 1) != 0 ) {
						name[ARRAY_LEN(name) - 1] = '\0';
						basename = strrchr( name, '\\' );
						if ( basename ) {
							basename = basename + 1;
						} else {
							basename = strrchr( name, '/' );
							if ( basename ) {
								basename = basename + 1;
							}
						}
					}
				}
			}
		}

		if ( basename && *basename ) {
			Com_sprintf( msg, sizeof( msg ), "Exception Code: %s\nException Address: %s@%x",
				GetExceptionName( ExceptionInfo->ExceptionRecord->ExceptionCode ),
				basename, (uint32_t)(addr - (byte*)hModule) );
		} else {
			Com_sprintf( msg, sizeof( msg ), "Exception Code: %s\nException Address: %p",
				GetExceptionName( ExceptionInfo->ExceptionRecord->ExceptionCode ),
				addr );
		}

		Com_Error( ERR_DROP, "Unhandled exception caught\n%s", msg );
	}

	return EXCEPTION_EXECUTE_HANDLER;
}

/*
==================
WinMain
==================
*/
int WINAPI WinMain( HINSTANCE hInstance, HINSTANCE hPrevInstance, LPSTR lpCmdLine, int nCmdShow ) {
	static char	sys_cmdline[ MAX_STRING_CHARS ];
	HANDLE hProcess;
	DWORD dwPriority;

	// should never get a previous instance in Win32
	if ( hPrevInstance ) {
		return 0;
	}

	// slightly boost process priority if it set to default
	hProcess = GetCurrentProcess();
	dwPriority = GetPriorityClass( hProcess );
	if ( dwPriority == NORMAL_PRIORITY_CLASS || dwPriority == ABOVE_NORMAL_PRIORITY_CLASS ) {
		SetPriorityClass( hProcess, HIGH_PRIORITY_CLASS );
	}

	g_wv.hInstance = hInstance;
	Q_strncpyz( sys_cmdline, lpCmdLine, sizeof( sys_cmdline ) );

	// no abort/retry/fail errors
	SetErrorMode( SEM_FAILCRITICALERRORS );

	SetUnhandledExceptionFilter( ExceptionFilter );

	Com_Init( sys_cmdline );

	// main game loop
	while ( 1 ) {
		// make sure mouse and joystick are only called once a frame
		IN_Frame();
		// run the game
		Com_Frame( CL_NoDelay() );
	}

	// never gets here
	return 0;
}
