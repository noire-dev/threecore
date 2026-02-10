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

#include "q_shared.h"
#include "qcommon.h"
#include "unzip.h"

#define MAX_CACHED_HANDLES  512
#define MAX_ZPATH			256
#define MAX_FILEHASH_SIZE	4096

typedef struct fileInPack_s {
	char					*name;		// name of the file
	unsigned long			pos;		// file info position in zip
	unsigned long			size;		// file size
	struct	fileInPack_s*	next;		// next file in the hash
} fileInPack_t;

typedef struct pack_s {
	char			*pakFilename;				// c:\quake3\baseq3\pak0.pk3
	char			*pakBasename;				// pak0
	const char		*pakGamename;				// baseq3
	unzFile			handle;						// handle to zip file
	int				checksum;					// regular checksum
	int				numfiles;					// number of files in pk3
	int				referenced;					// referenced file flags
	qboolean		exclude;					// found in \fs_excludeReference list
	int				hashSize;					// hash table size (power of 2)
	fileInPack_t*	*hashTable;					// hash table
	fileInPack_t*	buildBuffer;				// buffer with the filenames etc.
	int				index;

	int				handleUsed;

	struct pack_s	*next_h;						// double-linked list of unreferenced paks with open file handles
	struct pack_s	*prev_h;
} pack_t;

typedef struct {
	char		*path;		// c:\quake3
	char		*gamedir;	// baseq3
} directory_t;

typedef enum {
	DIR_STATIC = 0,	// always allowed, never changes
	DIR_ALLOW,
	DIR_DENY
} dirPolicy_t;

typedef struct searchpath_s {
	struct searchpath_s *next;
	pack_t		*pack;		// only one of pack / dir will be non NULL
	directory_t	*dir;
	dirPolicy_t	policy;
} searchpath_t;

static	char		fs_gamedir[MAX_OSPATH];	// this will be a single file name with no separators
static	cvar_t		*fs_debug;
static	cvar_t		*fs_homepath;

#ifdef __APPLE__
// Also search the .app bundle for .pk3 files
static  cvar_t          *fs_apppath;
#endif

static	cvar_t		*fs_basepath;
static	cvar_t		*fs_basegame;
static	cvar_t		*fs_copyfiles;
static	cvar_t		*fs_excludeReference;

static	searchpath_t	*fs_searchpaths;
static	int			fs_readCount;			// total bytes read
static	int			fs_loadCount;			// total files read
static	int			fs_loadStack;			// total files in memory
static	int			fs_packFiles;			// total number of files in all loaded packs

static	int			fs_pk3dirCount;			// total number of pk3 directories in searchpath
static	int			fs_packCount;			// total number of packs in searchpath
static	int			fs_dirCount;			// total number of directories in searchpath

static	int			fs_checksumFeed;

typedef union qfile_gus {
	FILE*		o;
	unzFile		z;
	void*		v;
} qfile_gut;

typedef struct qfile_us {
	qfile_gut	file;
	qboolean	unique;
} qfile_ut;

typedef struct {
	qfile_ut	handleFiles;
	qboolean	handleSync;
	qboolean	zipFile;
	int			zipFilePos;
	int			zipFileLen;
	char		name[MAX_ZPATH];
	int			pakIndex;
	pack_t		*pak;
} fileHandleData_t;

static fileHandleData_t	fsh[MAX_FILE_HANDLES];

// TTimo - https://zerowing.idsoftware.com/bugzilla/show_bug.cgi?id=540
// whether we did a reorder on the current search path when joining the server
qboolean fs_reordered;

#define MAX_REF_PAKS	MAX_STRING_TOKENS

// only used for autodownload, to make sure the client has at least
// all the pk3 files that are referenced at the server side
static int		fs_numServerReferencedPaks;
static int		fs_serverReferencedPaks[MAX_REF_PAKS];		// checksums
static char		*fs_serverReferencedPakNames[MAX_REF_PAKS];	// pk3 names

int	fs_lastPakIndex;

#ifdef FS_MISSING
static FILE*		missingFiles = NULL;
#endif

void FS_Reload( void );


/*
==============
FS_Initialized
==============
*/
qboolean FS_Initialized( void ) {
	return ( fs_searchpaths != NULL );
}

/*
=================
FS_LoadStack
return load stack
=================
*/
int FS_LoadStack( void ) {
	return fs_loadStack;
}


/*
================
return a hash value for the filename
================
*/
#define FS_HashFileName Com_GenerateHashValue


/*
=================
FS_HandleForFile
=================
*/
static fileHandle_t	FS_HandleForFile( void ) 
{
	int		i;

	for ( i = 1 ; i < MAX_FILE_HANDLES ; i++ ) 
	{
		if ( fsh[i].handleFiles.file.v == NULL )
			return i;
	}

	Com_Error( ERR_DROP, "FS_HandleForFile: none free" );
	return FS_INVALID_HANDLE;
}


static FILE	*FS_FileForHandle( fileHandle_t f ) {
	if ( f <= 0 || f >= MAX_FILE_HANDLES ) {
		Com_Error( ERR_DROP, "FS_FileForHandle: out of range" );
	}
	if ( fsh[f].zipFile ) {
		Com_Error( ERR_DROP, "FS_FileForHandle: can't get FILE on zip file" );
	}
	if ( ! fsh[f].handleFiles.file.o ) {
		Com_Error( ERR_DROP, "FS_FileForHandle: NULL" );
	}
	
	return fsh[f].handleFiles.file.o;
}


void FS_ForceFlush( fileHandle_t f ) {
	FILE *file;

	file = FS_FileForHandle(f);
	setvbuf( file, NULL, _IONBF, 0 );
}

/*
================
FS_FileLength
================
*/
static int FS_FileLength( FILE* h ) 
{
	int		pos;
	int		end;

	pos = ftell( h );
	fseek( h, 0, SEEK_END );
	end = ftell( h );
	fseek( h, pos, SEEK_SET );

	return end;
}


/*
====================
FS_PakIndexForHandle
====================
*/
int FS_PakIndexForHandle( fileHandle_t f ) {

	if ( f <= FS_INVALID_HANDLE || f >= MAX_FILE_HANDLES )
		return -1;

	return fsh[ f ].pakIndex;
}


/*
====================
FS_ReplaceSeparators

Fix things up differently for win/unix/mac
====================
*/
static void FS_ReplaceSeparators( char *path ) {
	char	*s;

	for ( s = path ; *s ; s++ ) {
		if ( *s == PATH_SEP_FOREIGN ) {
			*s = PATH_SEP;
		}
	}
}


/*
===================
FS_BuildOSPath

Qpath may have either forward or backwards slashes
===================
*/
char *FS_BuildOSPath( const char *base, const char *game, const char *qpath ) {
	char	temp[MAX_OSPATH*2+1];
	static char ospath[2][sizeof(temp)+MAX_OSPATH];
	static int toggle;
	
	toggle ^= 1;		// flip-flop to allow two returns without clash

	if( !game || !game[0] ) {
		game = fs_gamedir;
	}

	if ( qpath )
		Com_sprintf( temp, sizeof( temp ), "%c%s%c%s", PATH_SEP, game, PATH_SEP, qpath );
	else
		Com_sprintf( temp, sizeof( temp ), "%c%s", PATH_SEP, game );

	FS_ReplaceSeparators( temp );
	Com_sprintf( ospath[toggle], sizeof( ospath[0] ), "%s%s", base, temp );
	
	return ospath[toggle];
}


/*
================
FS_CheckDirTraversal

Check whether the string contains stuff like "../" to prevent directory traversal bugs
and return qtrue if it does.
================
*/
static qboolean FS_CheckDirTraversal( const char *checkdir )
{
	if ( strstr( checkdir, "../" ) || strstr( checkdir, "..\\" ) )
		return qtrue;

	if ( strstr( checkdir, "::" ) )
		return qtrue;
	
	return qfalse;
}


/*
============
FS_CreatePath

Creates any directories needed to store the given filename
============
*/
static qboolean FS_CreatePath( const char *OSPath ) {
	char	path[MAX_OSPATH*2+1];
	char	*ofs;
	
	// make absolutely sure that it can't back up the path
	// FIXME: is c: allowed???
	if ( FS_CheckDirTraversal( OSPath ) ) {
		Com_Printf( "WARNING: refusing to create relative path \"%s\"\n", OSPath );
		return qtrue;
	}

	Q_strncpyz( path, OSPath, sizeof( path ) );
	// Make sure we have OS correct slashes
	FS_ReplaceSeparators( path );
	for ( ofs = path + 1; *ofs; ofs++ ) {
		if ( *ofs == PATH_SEP ) {
			// create the directory
			*ofs = '\0';
			Sys_Mkdir( path );
			*ofs = PATH_SEP;
		}
	}
	return qfalse;
}


/*
=================
FS_CopyFile

Copy a fully specified file from one place to another
=================
*/
static void FS_CopyFile( const char *fromOSPath, const char *toOSPath ) {
	FILE	*f;
	size_t	len;
	byte	*buf;

	Com_Printf( "copy %s to %s\n", fromOSPath, toOSPath );

	f = Sys_FOpen( fromOSPath, "rb" );
	if ( !f ) {
		return;
	}

	len = FS_FileLength( f );

	// we are using direct malloc instead of Z_Malloc here, so it
	// probably won't work on a mac... It's only for developers anyway...
	buf = malloc( len );
	if ( !buf ) {
		fclose( f );
		Com_Error( ERR_FATAL, "Memory alloc error in FS_Copyfiles()\n" );
	}

	if (fread( buf, 1, len, f ) != len) {
		free( buf );
		fclose( f );
		Com_Error( ERR_FATAL, "Short read in FS_Copyfiles()\n" );
	}
	fclose( f );

	f = Sys_FOpen( toOSPath, "wb" );
	if ( !f ) {
		if ( FS_CreatePath( toOSPath ) ) {
			free( buf );
			return;
		}
		f = Sys_FOpen( toOSPath, "wb" );
		if ( !f ) {
			free( buf );
			return;
		}
	}

	if ( fwrite( buf, 1, len, f ) != len ) {
		free( buf );
		fclose( f );
		Com_Error( ERR_FATAL, "Short write in FS_Copyfiles()\n" );
	}
	fclose( f );
	free( buf );
}


static const char *FS_HasExt( const char *fileName, const char **extList, int extCount );

/*
===========
FS_Remove

===========
*/
void FS_Remove( const char *osPath ) {
	remove( osPath );
}


/*
===========
FS_HomeRemove
===========
*/
void FS_HomeRemove( const char *osPath ) {
	remove( FS_BuildOSPath( fs_homepath->string, fs_gamedir, osPath ) );
}


/*
================
FS_FileExists

Tests if the file exists in the current gamedir, this DOES NOT
search the paths.  This is to determine if opening a file to write
(which always goes into the current gamedir) will cause any overwrites.
NOTE TTimo: this goes with FS_FOpenFileWrite for opening the file afterwards
================
*/
qboolean FS_FileExists( const char *file )
{
	FILE *f;
	char *testpath;

	testpath = FS_BuildOSPath( fs_homepath->string, fs_gamedir, file );

	f = Sys_FOpen( testpath, "rb" );
	if (f) {
		fclose( f );
		return qtrue;
	}
	return qfalse;
}


/*
================
FS_SV_FileExists

Tests if the file exists 
================
*/
qboolean FS_SV_FileExists( const char *file )
{
	FILE *f;
	char *testpath;

	// search in homepath
	testpath = FS_BuildOSPath( fs_homepath->string, file, NULL );
	f = Sys_FOpen( testpath, "rb" );
	if ( f ) {
		fclose( f );
		return qtrue;
	}

	// search in basepath
	if ( Q_stricmp( fs_homepath->string, fs_basepath->string ) ) {
		testpath = FS_BuildOSPath( fs_basepath->string, file, NULL );
		f = Sys_FOpen( testpath, "rb" );
		if ( f ) {
			fclose( f );
			return qtrue;
		}
	}

	return qfalse;
}


/*
===========
FS_InitHandle
===========
*/
static void FS_InitHandle( fileHandleData_t *fd ) {
	fd->pak = NULL;
	fd->pakIndex = -1;
	fs_lastPakIndex = -1;
}


/*
===========
FS_SV_FOpenFileWrite
===========
*/
fileHandle_t FS_SV_FOpenFileWrite( const char *filename ) {
	char *ospath;
	fileHandle_t	f;
	fileHandleData_t *fd;

	if ( !fs_searchpaths ) {
		Com_Error( ERR_FATAL, "Filesystem call made without initialization" );
	}

	if ( !*filename ) {
		return FS_INVALID_HANDLE;
	}

	ospath = FS_BuildOSPath( fs_homepath->string, filename, NULL );

	f = FS_HandleForFile();
	fd = &fsh[ f ];
	FS_InitHandle( fd );

	if ( fs_debug->integer ) {
		Com_Printf( "FS_SV_FOpenFileWrite: %s\n", ospath );
	}

	Com_DPrintf( "writing to: %s\n", ospath );

	fd->handleFiles.file.o = Sys_FOpen( ospath, "wb" );
	if ( !fd->handleFiles.file.o ) {
		if ( FS_CreatePath( ospath ) ) {
			return FS_INVALID_HANDLE;
		}
		fd->handleFiles.file.o = Sys_FOpen( ospath, "wb" );
		if ( !fd->handleFiles.file.o ) {
			return FS_INVALID_HANDLE;
		}
	}

	Q_strncpyz( fd->name, filename, sizeof( fd->name ) );
	fd->handleSync = qfalse;
	fd->zipFile = qfalse;

	return f;
}


/*
===========
FS_SV_FOpenFileRead
search for a file somewhere below the home path, base path or cd path
we search in that order, matching FS_SV_FOpenFileRead order
===========
*/
int FS_SV_FOpenFileRead( const char *filename, fileHandle_t *fp ) {
	fileHandleData_t *fd;
	fileHandle_t f;
	char *ospath;

	if ( !fs_searchpaths ) {
		Com_Error( ERR_FATAL, "Filesystem call made without initialization" );
	}

	// should never happen but for safe
	if ( !fp ) { 
		return -1;
	}

	// allocate new file handle
	f = FS_HandleForFile(); 
	fd = &fsh[ f ];
	FS_InitHandle( fd );

#ifndef DEDICATED
	// don't let sound stutter
	// S_ClearSoundBuffer();
#endif

	// search homepath
	ospath = FS_BuildOSPath( fs_homepath->string, filename, NULL );

	if ( fs_debug->integer ) {
		Com_Printf( "FS_SV_FOpenFileRead (fs_homepath): %s\n", ospath );
	}

	fd->handleFiles.file.o = Sys_FOpen( ospath, "rb" );
	if ( !fd->handleFiles.file.o )
	{
		// NOTE TTimo on non *nix systems, fs_homepath == fs_basepath, might want to avoid
		if ( Q_stricmp( fs_homepath->string, fs_basepath->string ) != 0 ) {
			// search basepath
			ospath = FS_BuildOSPath( fs_basepath->string, filename, NULL );

			if ( fs_debug->integer )
			{
				Com_Printf( "FS_SV_FOpenFileRead (fs_basepath): %s\n", ospath );
			}

			fd->handleFiles.file.o = Sys_FOpen( ospath, "rb" );
		}
	}

	if( fd->handleFiles.file.o != NULL ) {
		Q_strncpyz( fd->name, filename, sizeof( fd->name ) );
		fd->handleSync = qfalse;
		fd->zipFile = qfalse;
		*fp = f;
		return FS_FileLength( fd->handleFiles.file.o );
	}

	*fp = FS_INVALID_HANDLE;
	return -1;
}


/*
===========
FS_SV_Rename
===========
*/
void FS_SV_Rename( const char *from, const char *to ) {
	const char			*from_ospath, *to_ospath;

	if ( !fs_searchpaths ) {
		Com_Error( ERR_FATAL, "Filesystem call made without initialization" );
	}

#ifndef DEDICATED
	// don't let sound stutter
	// S_ClearSoundBuffer();
#endif

	from_ospath = FS_BuildOSPath( fs_homepath->string, from, NULL );
	to_ospath = FS_BuildOSPath( fs_homepath->string, to, NULL );

	if ( fs_debug->integer ) {
		Com_Printf( "FS_SV_Rename: %s --> %s\n", from_ospath, to_ospath );
	}

	if ( rename( from_ospath, to_ospath ) ) {
		// Failed, try copying it and deleting the original
		FS_CopyFile( from_ospath, to_ospath );
		FS_Remove( from_ospath );
	}
}


/*
===========
FS_Rename
===========
*/
void FS_Rename( const char *from, const char *to ) {
	const char *from_ospath, *to_ospath;
	FILE *f;

	if ( !fs_searchpaths ) {
		Com_Error( ERR_FATAL, "Filesystem call made without initialization" );
	}

#ifndef DEDICATED
	// don't let sound stutter
	// S_ClearSoundBuffer();
#endif

	from_ospath = FS_BuildOSPath( fs_homepath->string, fs_gamedir, from );
	to_ospath = FS_BuildOSPath( fs_homepath->string, fs_gamedir, to );

	if ( fs_debug->integer ) {
		Com_Printf( "FS_Rename: %s --> %s\n", from_ospath, to_ospath );
	}

	f = Sys_FOpen( from_ospath, "rb" );
	if ( f ) {
		fclose( f );
		FS_Remove( to_ospath );
	}

	if ( rename( from_ospath, to_ospath ) ) {
		// Failed, try copying it and deleting the original
		FS_CopyFile( from_ospath, to_ospath );
		FS_Remove( from_ospath );
	}
}

static int		hpaksCount;
static pack_t	*hhead;

static void FS_RemoveFromHandleList( pack_t *pak )
{
	if ( pak->next_h != pak ) {
		// cut pak from list
		pak->next_h->prev_h = pak->prev_h;
		pak->prev_h->next_h = pak->next_h;
		if ( hhead == pak ) {
			hhead = pak->next_h;
		}
	} else {
#ifdef _DEBUG
		if ( hhead != pak )
			Com_Error( ERR_DROP, "%s(): invalid head pointer", __func__ );
#endif
		hhead = NULL;
	}

	pak->next_h = NULL;
	pak->prev_h = NULL;
	
	hpaksCount--;

#ifdef _DEBUG
	if ( hpaksCount < 0 ) {
		Com_Error( ERR_DROP, "%s(): negative paks count", __func__ );
	}

	if ( hpaksCount == 0 && hhead != NULL ) {
		Com_Error( ERR_DROP, "%s(): non-null head with zero paks count", __func__ );
	}
#endif
}


static void FS_AddToHandleList( pack_t *pak )
{
#ifdef _DEBUG
	if ( !pak->handle ) {
		Com_Error( ERR_DROP, "%s(): invalid pak handle", __func__ );
	}
	if ( pak->next_h || pak->prev_h ) {
		Com_Error( ERR_DROP, "%s(): invalid pak pointers", __func__ );
	}
#endif
	while ( hpaksCount >= MAX_CACHED_HANDLES ) {
		pack_t *pk = hhead->prev_h; // tail item
#ifdef _DEBUG
		if ( pk->handle == NULL || pk->handleUsed != 0 ) {
			Com_Error( ERR_DROP, "%s(): invalid pak handle", __func__ );
		}
#endif
		unzClose( pk->handle );
		pk->handle = NULL;
		FS_RemoveFromHandleList( pk );
	} 

	if ( hhead == NULL ) {
		pak->next_h = pak;
		pak->prev_h = pak;
	} else {
		hhead->prev_h->next_h = pak;
		pak->prev_h = hhead->prev_h;
		hhead->prev_h = pak;
		pak->next_h = hhead;
	}

	hhead = pak;
	hpaksCount++;
}


/*
==============
FS_FCloseFile

If the FILE pointer is an open pak file, leave it open.

For some reason, other dll's can't just cal fclose()
on files returned by FS_FOpenFile...
==============
*/
void FS_FCloseFile( fileHandle_t f ) {
	fileHandleData_t *fd;

	if ( !fs_searchpaths ) {
		Com_Error( ERR_FATAL, "Filesystem call made without initialization" );
	}

	fd = &fsh[ f ];

	if ( fd->zipFile && fd->pak ) {
		unzCloseCurrentFile( fd->handleFiles.file.z );
		if ( fd->handleFiles.unique ) {
			unzClose( fd->handleFiles.file.z );
		}
		fd->handleFiles.file.z = NULL;
		fd->zipFile = qfalse;
		fd->pak->handleUsed--;
		if ( fd->pak->handleUsed == 0 ) {
			FS_AddToHandleList( fd->pak );
		}
	} else {
		if ( fd->handleFiles.file.o ) {
			fclose( fd->handleFiles.file.o );
			fd->handleFiles.file.o = NULL;
		}
	}

	Com_Memset( fd, 0, sizeof( *fd ) );
}


/*
===========
FS_ResetReadOnlyAttribute
===========
*/
qboolean FS_ResetReadOnlyAttribute( const char *filename ) {
	char *ospath;
	
	ospath = FS_BuildOSPath( fs_homepath->string, fs_gamedir, filename );

	return Sys_ResetReadOnlyAttribute( ospath );
}


/*
===========
FS_FOpenFileWrite
===========
*/
fileHandle_t FS_FOpenFileWrite( const char *filename ) {
	char			*ospath;
	fileHandle_t	f;
	fileHandleData_t *fd;

	if ( !fs_searchpaths ) {
		Com_Error( ERR_FATAL, "Filesystem call made without initialization" );
	}

	if ( !filename || !*filename ) {
		return FS_INVALID_HANDLE;
	}

	ospath = FS_BuildOSPath( fs_homepath->string, fs_gamedir, filename );

	if ( fs_debug->integer ) {
		Com_Printf( "FS_FOpenFileWrite: %s\n", ospath );
	}

	f = FS_HandleForFile();
	fd = &fsh[ f ];
	FS_InitHandle( fd );

	// enabling the following line causes a recursive function call loop
	// when running with +set logfile 1 +set developer 1
	//Com_DPrintf( "writing to: %s\n", ospath );
	fd->handleFiles.file.o = Sys_FOpen( ospath, "wb" );
	if ( fd->handleFiles.file.o == NULL ) {
		if ( FS_CreatePath( ospath ) ) {
			return FS_INVALID_HANDLE;
		}
		fd->handleFiles.file.o = Sys_FOpen( ospath, "wb" );
		if ( fd->handleFiles.file.o == NULL ) {
			return FS_INVALID_HANDLE;
		}
	}

	Q_strncpyz( fd->name, filename, sizeof( fd->name ) );
	fd->handleSync = qfalse;
	fd->zipFile = qfalse;

	return f;
}


/*
===========
FS_FOpenFileAppend
===========
*/
fileHandle_t FS_FOpenFileAppend( const char *filename ) {
	char			*ospath;
	fileHandleData_t *fd;
	fileHandle_t	f;

	if ( !fs_searchpaths ) {
		Com_Error( ERR_FATAL, "Filesystem call made without initialization" );
	}

	if ( !*filename ) {
		return FS_INVALID_HANDLE;
	}

#ifndef DEDICATED
	// don't let sound stutter
	// S_ClearSoundBuffer();
#endif

	ospath = FS_BuildOSPath( fs_homepath->string, fs_gamedir, filename );

	if ( fs_debug->integer ) {
		Com_Printf( "FS_FOpenFileAppend: %s\n", ospath );
	}

	f = FS_HandleForFile();
	fd = &fsh[ f ];
	FS_InitHandle( fd );

	fd->handleFiles.file.o = Sys_FOpen( ospath, "ab" );
	if ( fd->handleFiles.file.o == NULL ) {
		if ( FS_CreatePath( ospath ) ) {
			return FS_INVALID_HANDLE;
		}
		fd->handleFiles.file.o = Sys_FOpen( ospath, "ab" );
		if ( fd->handleFiles.file.o == NULL ) {
			return FS_INVALID_HANDLE;
		}
	}

	Q_strncpyz( fd->name, filename, sizeof( fd->name ) );
	fd->handleSync = qfalse;
	fd->zipFile = qfalse;

	return f;
}


/*
===========
FS_FilenameCompare

Ignore case and separator char distinctions
===========
*/
qboolean FS_FilenameCompare( const char *s1, const char *s2 ) {
	int		c1, c2;
	
	do {
		c1 = *s1++;
		c2 = *s2++;

		if ( c1 <= 'Z' && c1 >= 'A' )
			c1 += ('a' - 'A');
		else if ( c1 == '\\' || c1 == ':' )
			c1 = '/';

		if ( c2 <= 'Z' && c2 >= 'A' )
			c2 += ('a' - 'A');
		else if ( c2 == '\\' || c2 == ':' )
			c2 = '/';

		if ( c1 != c2 ) {
			return qtrue;		// strings not equal
		}
	} while ( c1 );
	
	return qfalse;		// strings are equal
}


/*
===========
FS_IsExt

Return qtrue if ext matches file extension filename
===========
*/
static qboolean FS_IsExt( const char *filename, const char *ext, size_t namelen )
{
	size_t extlen;

	extlen = strlen( ext );

	if ( extlen > namelen )
		return qfalse;

	filename += namelen - extlen;

	return !Q_stricmp( filename, ext );
}


/*
===========
FS_StripExt
===========
*/
qboolean FS_StripExt( char *filename, const char *ext )
{
	int extlen, namelen;

	extlen = strlen( ext );
	namelen = strlen( filename );

	if ( extlen > namelen )
		return qfalse;

	filename += namelen - extlen;

	if ( !Q_stricmp( filename, ext ) ) 
	{
		filename[0] = '\0';
		return qtrue;
	}

	return qfalse;
}


static const char *FS_HasExt( const char *fileName, const char **extList, int extCount ) 
{
	const char *e;
	int i;

	e = strrchr( fileName, '.' );

	if ( !e ) 
		return NULL;

	for ( i = 0, e++; i < extCount; i++ ) 
	{
		if ( !Q_stricmp( e, extList[i] ) )
			return e;
	}

	return NULL;
}


static qboolean FS_GeneralRef( const char *filename ) {
	// allowed non-ref extensions
	static const char *extList[] = { "config", "shader", "arena", "menu", "bot", "cfg", "txt" };

	if ( FS_HasExt( filename, extList, ARRAY_LEN( extList ) ) )
		return qfalse;
	
	if ( !Q_stricmp( filename, "qvm/qagame.qvm" ) )
		return qfalse;

	if ( strstr( filename, "levelshots" ) )
		return qfalse;

	return qtrue;
}

static int FS_OpenFileInPak( fileHandle_t *file, pack_t *pak, fileInPack_t *pakFile, qboolean uniqueFILE ) {
	fileHandleData_t *f;
	unz_s *zfi;
	FILE *temp;

	// mark the pak as having been referenced and mark specifics on cgame and ui
	// these are loaded from all pk3s
	// from every pk3 file.

	if ( !( pak->referenced & FS_GENERAL_REF ) && FS_GeneralRef( pakFile->name ) ) {
		pak->referenced |= FS_GENERAL_REF;
	}
	if ( !( pak->referenced & FS_CGAME_REF ) && !strcmp( pakFile->name, "qvm/cgame.qvm" ) ) {
		pak->referenced |= FS_CGAME_REF;
	}
	if ( !( pak->referenced & FS_UI_REF ) && !strcmp( pakFile->name, "qvm/ui.qvm" ) ) {
		pak->referenced |= FS_UI_REF;
	}

	if ( !pak->handle ) {
		pak->handle = unzOpen( pak->pakFilename );
		if ( !pak->handle ) {
			Com_Printf( S_COLOR_RED "Error opening %s@%s\n", pak->pakBasename, pakFile->name );
			*file = FS_INVALID_HANDLE;
			return -1;
		}
	}

	if ( uniqueFILE ) {
		// open a new file on the pakfile
		temp = unzReOpen( pak->pakFilename, pak->handle );
		if ( temp == NULL ) {
			Com_Printf( S_COLOR_RED "Couldn't reopen %s", pak->pakFilename );
			*file = FS_INVALID_HANDLE;
			return -1;
		}
	} else {
		temp = pak->handle;
	}

	*file = FS_HandleForFile();
	f = &fsh[ *file ];
	FS_InitHandle( f );

	f->zipFile = qtrue;
	f->handleFiles.file.z = temp;
	f->handleFiles.unique = uniqueFILE;

	Q_strncpyz( f->name, pakFile->name, sizeof( f->name ) );
	zfi = (unz_s *)f->handleFiles.file.z;
	// in case the file was new
	temp = zfi->file;
	// set the file position in the zip file (also sets the current file info)
	unzSetCurrentFileInfoPosition( pak->handle, pakFile->pos );
	// copy the file info into the unzip structure
	Com_Memcpy( zfi, pak->handle, sizeof( *zfi ) );
	// we copy this back into the structure
	zfi->file = temp;
	// open the file in the zip
	unzOpenCurrentFile( f->handleFiles.file.z );
	f->zipFilePos = pakFile->pos;
	f->zipFileLen = pakFile->size;
	f->pakIndex = pak->index;
	fs_lastPakIndex = pak->index;
	f->pak = pak;

	if ( pak->next_h ) {
		FS_RemoveFromHandleList( pak );
	}

	pak->handleUsed++;

	if ( fs_debug->integer ) {
		Com_Printf( "FS_FOpenFileRead: %s (found in '%s')\n",
			pakFile->name, pak->pakFilename );
	}

	return zfi->cur_file_info.uncompressed_size;
}


/*
===========
FS_FOpenFileRead

Finds the file in the search path.
Returns filesize and an open FILE pointer.
Used for streaming data out of either a
separate file or a ZIP file.
===========
*/
extern qboolean		com_fullyInitialized;

int FS_FOpenFileRead( const char *filename, fileHandle_t *file, qboolean uniqueFILE ) {
	const searchpath_t	*search;
	char			*netpath;
	pack_t			*pak;
	fileInPack_t	*pakFile;
	directory_t		*dir;
	long			hash;
	long			fullHash;
	FILE			*temp;
	int				length;
	fileHandleData_t *f;

	if ( !fs_searchpaths ) {
		Com_Error( ERR_FATAL, "Filesystem call made without initialization" );
	}

	if ( !filename ) {
		Com_Error( ERR_FATAL, "FS_FOpenFileRead: NULL 'filename' parameter passed\n" );
	}

	// qpaths are not supposed to have a leading slash
	if ( filename[0] == '/' || filename[0] == '\\' ) {
		filename++;
	}

	// make absolutely sure that it can't back up the path.
	// The searchpaths do guarantee that something will always
	// be prepended, so we don't need to worry about "c:" or "//limbo"
	if ( FS_CheckDirTraversal( filename ) ) {
		*file = FS_INVALID_HANDLE;
		return -1;
	}

	// we will calculate full hash only once then just mask it by current pack->hashSize
	// we can do that as long as we know properties of our hash function
	fullHash = FS_HashFileName( filename, 0U );

	if ( file == NULL ) {
		// just wants to see if file is there
		for ( search = fs_searchpaths ; search ; search = search->next ) {
			// is the element a pak file?
			if ( search->pack && search->pack->hashTable[ (hash = fullHash & (search->pack->hashSize-1)) ] ) {
				// look through all the pak file elements
				pak = search->pack;
				pakFile = pak->hashTable[hash];
				do {
					// case and separator insensitive comparisons
					if ( !FS_FilenameCompare( pakFile->name, filename ) ) {
						// found it!
						return pakFile->size; 
					}
					pakFile = pakFile->next;
				} while ( pakFile != NULL );
			} else if ( search->dir && search->policy != DIR_DENY ) {
				dir = search->dir;
				netpath = FS_BuildOSPath( dir->path, dir->gamedir, filename );
				temp = Sys_FOpen( netpath, "rb" );
				if ( temp ) {
					length = FS_FileLength( temp );
					fclose( temp );
					return length;
				}
			}
		}
		return -1;
	}

	// make sure the q3key file is only readable by the quake3.exe at initialization
	// any other time the key should only be accessed in memory using the provided functions
	if ( com_fullyInitialized && strstr( filename, "q3key" ) ) {
		*file = FS_INVALID_HANDLE;
		return -1;
	}

	//
	// search through the path, one element at a time
	//
	for ( search = fs_searchpaths ; search ; search = search->next ) {
		// is the element a pak file?
		if ( search->pack && search->pack->hashTable[ (hash = fullHash & (search->pack->hashSize-1)) ] ) {
			// look through all the pak file elements
			pak = search->pack;
			pakFile = pak->hashTable[hash];
			do {
				// case and separator insensitive comparisons
				if ( !FS_FilenameCompare( pakFile->name, filename ) ) {
					// found it!
					return FS_OpenFileInPak( file, pak, pakFile, uniqueFILE );
				}
				pakFile = pakFile->next;
			} while ( pakFile != NULL );
		} else if ( search->dir && search->policy != DIR_DENY ) {
			// check a file in the directory tree
			dir = search->dir;

			netpath = FS_BuildOSPath( dir->path, dir->gamedir, filename );

			temp = Sys_FOpen( netpath, "rb" );
			if ( temp == NULL ) {
				continue;
			}

			*file = FS_HandleForFile();
			f = &fsh[ *file ];
			FS_InitHandle( f );

			f->handleFiles.file.o = temp;
			Q_strncpyz( f->name, filename, sizeof( f->name ) );
			f->zipFile = qfalse;

			if ( fs_debug->integer ) {
				Com_Printf( "FS_FOpenFileRead: %s (found in '%s/%s')\n", filename,
					dir->path, dir->gamedir );
			}

			return FS_FileLength( f->handleFiles.file.o );
		}
	}

#ifdef FS_MISSING
	if ( missingFiles ) {
		fprintf( missingFiles, "%s\n", filename );
	}
#endif

	*file = FS_INVALID_HANDLE;
	return -1;
}


/*
===========
FS_TouchFileInPak
===========
*/
void FS_TouchFileInPak( const char *filename ) {
	const searchpath_t *search;
	long			fullHash, hash;
	pack_t			*pak;
	fileInPack_t	*pakFile;

	fullHash = FS_HashFileName( filename, 0U );

	for ( search = fs_searchpaths ; search ; search = search->next ) {
		
		// is the element a pak file?
		if ( !search->pack )
			continue;
		
		if ( search->pack->exclude ) // skip paks in \fs_excludeReference list
			continue;

		if ( search->pack->hashTable[ (hash = fullHash & (search->pack->hashSize-1)) ] ) {
			pak = search->pack;
			pakFile = pak->hashTable[hash];
			do {
				// case and separator insensitive comparisons
				if ( !FS_FilenameCompare( pakFile->name, filename ) ) {
					// found it!
					if ( !( pak->referenced & FS_GENERAL_REF ) && FS_GeneralRef( filename ) ) {
						pak->referenced |= FS_GENERAL_REF;
					}
					if ( !( pak->referenced & FS_CGAME_REF ) && !strcmp( filename, "qvm/cgame.qvm" ) ) {
						pak->referenced |= FS_CGAME_REF;
					}
					if ( !( pak->referenced & FS_UI_REF ) && !strcmp( filename, "qvm/ui.qvm" ) ) {
						pak->referenced |= FS_UI_REF;
					}
					return;
				}
				pakFile = pakFile->next;
			} while ( pakFile != NULL );
		}
	}
}


/*
===========
FS_Home_FOpenFileRead
===========
*/
int FS_Home_FOpenFileRead( const char *filename, fileHandle_t *file ) 
{
	char path[ MAX_OSPATH*3 + 1 ];
	fileHandleData_t *fd;
	fileHandle_t f;	

	if ( !fs_searchpaths ) {
		Com_Error( ERR_FATAL, "Filesystem call made without initialization" );
	}

	// should never happen but for safe
	if ( !file ) { 
		return -1;
	}

	// allocate new file handle
	f = FS_HandleForFile(); 
	fd = &fsh[ f ];
	FS_InitHandle( fd );

	Com_sprintf( path, sizeof( path ), "%s%c%s%c%s", fs_homepath->string,
		PATH_SEP, fs_gamedir, PATH_SEP, filename );

	if ( fs_debug->integer ) {
		Com_Printf( "%s: %s\n", __func__, path );
	}

	fd->handleFiles.file.o = Sys_FOpen( path, "rb" );
	if ( fd->handleFiles.file.o != NULL ) {
		Q_strncpyz( fd->name, filename, sizeof( fd->name ) );
		fd->handleSync = qfalse;
		fd->zipFile = qfalse;
		*file = f;
		return FS_FileLength( fd->handleFiles.file.o );
	}

	*file = FS_INVALID_HANDLE;
	return -1;
}


/*
=================
FS_Read

Properly handles partial reads
=================
*/
int FS_Read( void *buffer, int len, fileHandle_t f ) {
	int		block, remaining;
	int		read;
	byte	*buf;
	int		tries;

	if ( !fs_searchpaths ) {
		Com_Error( ERR_FATAL, "Filesystem call made without initialization" );
	}

	if ( f <= 0 || f >= MAX_FILE_HANDLES ) {
		return 0;
	}

	buf = (byte *)buffer;
	fs_readCount += len;

	if ( !fsh[f].zipFile ) {
		remaining = len;
		tries = 0;
		while (remaining) {
			block = remaining;
			read = fread( buf, 1, block, fsh[f].handleFiles.file.o );
			if (read == 0) {
				// we might have been trying to read from a CD, which
				// sometimes returns a 0 read on windows
				if (!tries) {
					tries = 1;
				} else {
					return len-remaining;	//Com_Error (ERR_FATAL, "FS_Read: 0 bytes read");
				}
			}

			if (read == -1) {
				Com_Error (ERR_FATAL, "FS_Read: -1 bytes read");
			}

			remaining -= read;
			buf += read;
		}
		return len;
	} else {
		return unzReadCurrentFile( fsh[f].handleFiles.file.z, buffer, len );
	}
}


/*
=================
FS_Write

Properly handles partial writes
=================
*/
int FS_Write( const void *buffer, int len, fileHandle_t h ) {
	int		block, remaining;
	int		written;
	byte	*buf;
	int		tries;
	FILE	*f;

	if ( !fs_searchpaths ) {
		Com_Error( ERR_FATAL, "Filesystem call made without initialization" );
	}

	//if ( h <= 0 || h >= MAX_FILE_HANDLES ) {
	//	return 0;
	//}

	f = FS_FileForHandle(h);
	buf = (byte *)buffer;

	remaining = len;
	tries = 0;
	while (remaining) {
		block = remaining;
		written = fwrite (buf, 1, block, f);
		if (written == 0) {
			if (!tries) {
				tries = 1;
			} else {
				Com_Printf( "FS_Write: 0 bytes written\n" );
				return 0;
			}
		}

		if (written == -1) {
			Com_Printf( "FS_Write: -1 bytes written\n" );
			return 0;
		}

		remaining -= written;
		buf += written;
	}
	if ( fsh[h].handleSync ) {
		fflush( f );
	}
	return len;
}

void QDECL FS_Printf( fileHandle_t h, const char *fmt, ... ) {
	va_list		argptr;
	char		msg[MAXPRINTMSG];

	va_start (argptr,fmt);
	Q_vsnprintf (msg, sizeof(msg), fmt, argptr);
	va_end (argptr);

	FS_Write(msg, strlen(msg), h);
}

#define PK3_SEEK_BUFFER_SIZE 65536

/*
=================
FS_Seek

=================
*/
int FS_Seek( fileHandle_t f, long offset, fsOrigin_t origin ) {
	int		_origin;

	if ( !fs_searchpaths ) {
		Com_Error( ERR_FATAL, "Filesystem call made without initialization" );
		return -1;
	}

	if ( fsh[f].zipFile == qtrue ) {
		//FIXME: this is really, really crappy
		//(but better than what was here before)
		byte	buffer[PK3_SEEK_BUFFER_SIZE];
		int		remainder;
		int		currentPosition = FS_FTell( f );

		// change negative offsets into FS_SEEK_SET
		if ( offset < 0 ) {
			switch( origin ) {
				case FS_SEEK_END:
					remainder = fsh[f].zipFileLen + offset;
					break;

				case FS_SEEK_CUR:
					remainder = currentPosition + offset;
					break;

				case FS_SEEK_SET:
				default:
					remainder = 0;
					break;
			}

			if ( remainder < 0 ) {
				remainder = 0;
			}

			origin = FS_SEEK_SET;
		} else {
			if ( origin == FS_SEEK_END ) {
				remainder = fsh[f].zipFileLen - currentPosition + offset;
			} else {
				remainder = offset;
			}
		}

		switch( origin ) {
			case FS_SEEK_SET:
				if ( remainder == currentPosition ) {
					return offset;
				}
				unzSetCurrentFileInfoPosition( fsh[f].handleFiles.file.z, fsh[f].zipFilePos );
				unzOpenCurrentFile( fsh[f].handleFiles.file.z );
				//fallthrough

			case FS_SEEK_END:
			case FS_SEEK_CUR:
				while( remainder > PK3_SEEK_BUFFER_SIZE ) {
					FS_Read( buffer, PK3_SEEK_BUFFER_SIZE, f );
					remainder -= PK3_SEEK_BUFFER_SIZE;
				}
				FS_Read( buffer, remainder, f );
				return offset;

			default:
				Com_Error( ERR_FATAL, "Bad origin in FS_Seek" );
				return -1;
		}
	} else {
		FILE *file;
		file = FS_FileForHandle( f );
		switch( origin ) {
		case FS_SEEK_CUR:
			_origin = SEEK_CUR;
			break;
		case FS_SEEK_END:
			_origin = SEEK_END;
			break;
		case FS_SEEK_SET:
			_origin = SEEK_SET;
			break;
		default:
			Com_Error( ERR_FATAL, "Bad origin in FS_Seek" );
			return -1;
		}

		return fseek( file, offset, _origin );
	}
}


/*
======================================================================================

CONVENIENCE FUNCTIONS FOR ENTIRE FILES

======================================================================================
*/

qboolean FS_FileIsInPAK( const char *filename, char *pakName ) {
	const searchpath_t	*search;
	const pack_t		*pak;
	const fileInPack_t	*pakFile;
	long			hash;
	long			fullHash;

	if ( !fs_searchpaths ) {
		Com_Error( ERR_FATAL, "Filesystem call made without initialization" );
	}

	if ( !filename ) {
		Com_Error( ERR_FATAL, "FS_FOpenFileRead: NULL 'filename' parameter passed" );
	}

	// qpaths are not supposed to have a leading slash
	while ( filename[0] == '/' || filename[0] == '\\' )
		filename++;

	// make absolutely sure that it can't back up the path.
	// The searchpaths do guarantee that something will always
	// be prepended, so we don't need to worry about "c:" or "//limbo" 
	if ( FS_CheckDirTraversal( filename ) ) {
		return qfalse;
	}

	fullHash = FS_HashFileName( filename, 0U );

	//
	// search through the path, one element at a time
	//
	for ( search = fs_searchpaths ; search ; search = search->next ) {

		// is the element a pak file?
		if ( search->pack && search->pack->hashTable[ (hash = fullHash & (search->pack->hashSize-1)) ] ) {
			if ( search->pack->exclude ) {
				continue;
			}

			// look through all the pak file elements
			pak = search->pack;
			pakFile = pak->hashTable[hash];
			do {
				// case and separator insensitive comparisons
				if ( !FS_FilenameCompare( pakFile->name, filename ) ) {
					if ( pakName ) {
						Com_sprintf( pakName, MAX_OSPATH, "%s/%s", pak->pakGamename, pak->pakBasename );
					}
					return qtrue;
				}
				pakFile = pakFile->next;
			} while ( pakFile != NULL );
		}
	}
	return qfalse;
}

int FS_ReadFile( const char *qpath, void **buffer ) {
	fileHandle_t	h;
	byte*			buf;
	long			len;

	if ( !fs_searchpaths ) {
		Com_Error( ERR_FATAL, "Filesystem call made without initialization" );
	}

	if ( !qpath || !qpath[0] ) {
		Com_Error( ERR_FATAL, "FS_ReadFile with empty name" );
	}

	buf = NULL;	// quiet compiler warning

	// look for it in the filesystem or pack files
	len = FS_FOpenFileRead( qpath, &h, qfalse );
	if ( h == FS_INVALID_HANDLE ) {
		if ( buffer ) *buffer = NULL;
		return -1;
	}

	if ( !buffer ) {
		FS_FCloseFile( h );
		return len;
	}

	buf = Hunk_AllocateTempMemory( len + 1 );
	*buffer = buf;

	FS_Read( buf, len, h );

	fs_loadCount++;
	fs_loadStack++;

	// guarantee that it will have a trailing 0 for string operations
	buf[ len ] = '\0';
	FS_FCloseFile( h );
	
	return len;
}

void FS_FreeFile( void *buffer ) {
	if ( !fs_searchpaths ) {
		Com_Error( ERR_FATAL, "Filesystem call made without initialization" );
	}
	if ( !buffer ) {
		Com_Error( ERR_FATAL, "FS_FreeFile( NULL )" );
	}
	fs_loadStack--;

	Hunk_FreeTempMemory( buffer );

	// if all of our temp files are free, clear all of our space
	if ( fs_loadStack == 0 ) {
		Hunk_ClearTempMemory();
	}
}


/*
============
FS_WriteFile

Filename are relative to the quake search path
============
*/
void FS_WriteFile( const char *qpath, const void *buffer, int size ) {
	fileHandle_t f;

	if ( !qpath || !buffer ) {
		Com_Error( ERR_FATAL, "FS_WriteFile: NULL parameter" );
	}

	f = FS_FOpenFileWrite( qpath );
	if ( f == FS_INVALID_HANDLE ) {
		Com_Printf( "Failed to open %s\n", qpath );
		return;
	}

	FS_Write( buffer, size, f );

	FS_FCloseFile( f );
}



/*
==========================================================================

ZIP FILE LOADING

==========================================================================
*/
static int FS_PakHashSize( const int filecount )
{
	int hashSize;

	for ( hashSize = 2; hashSize < MAX_FILEHASH_SIZE; hashSize <<= 1 ) {
		if ( hashSize >= filecount ) {
			break;
		}
	}

	return hashSize;
}


/*
============
FS_BannedPakFile

Check if file should NOT be added to hash search table
============
*/
static qboolean FS_BannedPakFile( const char *filename )
{
	if ( !strcmp( filename, "autoexec.cfg" ) || !strcmp( filename, CONFIG_CFG ) )
		return qtrue;
	else
		return qfalse;
}


/*
=================
FS_ConvertFilename

lower case and replace '\\' ':' with '/'
=================
*/
static void FS_ConvertFilename( char *name )
{
	int c;
	while ( (c = *name) != '\0' ) {
		if ( c <= 'Z' && c >= 'A' ) {
			*name = c - 'A' + 'a';
		} else if ( c == '\\' || c == ':' ) {
			*name = '/';
		}
		name++;
	}
}

/*
=================
FS_LoadZipFile

Creates a new pak_t in the search chain for the contents
of a zip file.
=================
*/
static pack_t *FS_LoadZipFile( const char *zipfile )
{
	fileInPack_t	*curFile;
	pack_t			*pack;
	unzFile			uf;
	int				err;
	unz_global_info gi;
	char			filename_inzip[MAX_ZPATH];
	unz_file_info	file_info;
	unsigned int	i, namelen, hashSize, size;
	long			hash;
	int				fs_numHeaderLongs;
	int				*fs_headerLongs;
	int				filecount;
	char			*namePtr;
	const char		*basename;
	int				fileNameLen;
	int				baseNameLen;

	// extract basename from zip path
	basename = strrchr( zipfile, PATH_SEP );
	if ( basename == NULL ) {
		basename = zipfile;
	} else {
		basename++;
	}

	fileNameLen = (int) strlen( zipfile ) + 1;
	baseNameLen = (int) strlen( basename ) + 1;

	uf = unzOpen( zipfile );
	err = unzGetGlobalInfo( uf, &gi );

	if ( err != UNZ_OK ) {
		return NULL;
	}

	namelen = 0;
	filecount = 0;
	unzGoToFirstFile( uf );
	for (i = 0; i < gi.number_entry; i++)
	{
		err = unzGetCurrentFileInfo(uf, &file_info, filename_inzip, sizeof(filename_inzip), NULL, 0, NULL, 0);
		filename_inzip[sizeof(filename_inzip)-1] = '\0';
		if (err != UNZ_OK) {
			break;
		}
		if ( file_info.compression_method != 0 && file_info.compression_method != 8 /*Z_DEFLATED*/ ) {
			Com_Printf( S_COLOR_YELLOW "%s|%s: unsupported compression method %i\n", basename, filename_inzip, (int)file_info.compression_method );
			unzGoToNextFile( uf );
			continue;
		} 
		namelen += strlen( filename_inzip ) + 1;
		unzGoToNextFile( uf );
		filecount++;
	}

	if ( filecount == 0 ) {
		unzClose( uf );
		return NULL;
	}

	// get the hash table size from the number of files in the zip
	// because lots of custom pk3 files have less than 32 or 64 files
	hashSize = FS_PakHashSize( filecount );

	namelen = PAD( namelen, sizeof( int ) );
	size = sizeof( *pack ) + hashSize * sizeof( pack->hashTable[0] ) + filecount * sizeof( pack->buildBuffer[0] ) + namelen;
	size += PAD( fileNameLen, sizeof( int ) );
	size += PAD( baseNameLen, sizeof( int ) );

	pack = Z_TagMalloc( size, TAG_PACK );
	Com_Memset( pack, 0, size );

	pack->handle = uf;
	pack->numfiles = filecount;
	pack->hashSize = hashSize;
	pack->hashTable = (fileInPack_t **)( pack + 1 );

	pack->buildBuffer = (fileInPack_t*)( pack->hashTable + pack->hashSize );
	namePtr = (char*)( pack->buildBuffer + filecount );

	pack->pakFilename = (char*)( namePtr + namelen );
	pack->pakBasename = (char*)( pack->pakFilename + PAD( fileNameLen, sizeof( int ) ) );

	fs_headerLongs = Z_Malloc( ( filecount + 1 ) * sizeof( fs_headerLongs[0] ) );

	fs_numHeaderLongs = 0;
	fs_headerLongs[ fs_numHeaderLongs++ ] = LittleLong( fs_checksumFeed );

	Com_Memcpy( pack->pakFilename, zipfile, fileNameLen );
	Com_Memcpy( pack->pakBasename, basename, baseNameLen );

	// strip .pk3 if needed
	FS_StripExt( pack->pakBasename, ".pk3" );

	unzGoToFirstFile( uf );
	curFile = pack->buildBuffer;
	for ( i = 0; i < gi.number_entry; i++ )
	{
		err = unzGetCurrentFileInfo( uf, &file_info, filename_inzip, sizeof(filename_inzip), NULL, 0, NULL, 0 );
		filename_inzip[sizeof(filename_inzip)-1] = '\0';
		if (err != UNZ_OK) {
			break;
		}
		if ( file_info.compression_method != 0 && file_info.compression_method != 8 /*Z_DEFLATED*/ ) {
			unzGoToNextFile( uf );
			continue;
		} 
		if ( file_info.uncompressed_size > 0 ) {
			fs_headerLongs[fs_numHeaderLongs++] = LittleLong( file_info.crc );
		}

		FS_ConvertFilename( filename_inzip );
		if ( !FS_BannedPakFile( filename_inzip ) ) {
			// store the file position in the zip
			unzGetCurrentFileInfoPosition( uf, &curFile->pos );
			curFile->size = file_info.uncompressed_size;
			curFile->name = namePtr;
			strcpy( curFile->name, filename_inzip );
			namePtr += strlen( filename_inzip ) + 1;

			// update hash table
			hash = FS_HashFileName( filename_inzip, pack->hashSize );
			curFile->next = pack->hashTable[ hash ];
			pack->hashTable[ hash ] = curFile; 
			curFile++;
		} else {
			pack->numfiles--;
		}

		unzGoToNextFile( uf );
	}

	pack->checksum = Com_BlockChecksum( fs_headerLongs + 1, sizeof( fs_headerLongs[0] ) * ( fs_numHeaderLongs - 1 ) );
	pack->checksum = LittleLong( pack->checksum );

	Z_Free( fs_headerLongs );

	FS_AddToHandleList( pack );

	return pack;
}


/*
=================
FS_FreePak

Frees a pak structure and releases all associated resources
=================
*/
static void FS_FreePak( pack_t *pak )
{
	if ( pak->handle )
	{
		if ( pak->next_h ) FS_RemoveFromHandleList( pak );
		unzClose( pak->handle );
		pak->handle = NULL;
	}

	Z_Free( pak );
}


/*
=================
FS_CompareZipChecksum

Compares whether the given pak file matches a referenced checksum
=================
*/
qboolean FS_CompareZipChecksum(const char *zipfile)
{
	pack_t *thepak;
	int index, checksum;
	
	thepak = FS_LoadZipFile( zipfile );
	
	if ( !thepak )
		return qfalse;
	
	checksum = thepak->checksum;
	FS_FreePak(thepak);

	for(index = 0; index < fs_numServerReferencedPaks; index++)
	{
		if(checksum == fs_serverReferencedPaks[index])
			return qtrue;
	}
	
	return qfalse;
}


/*
=================
FS_GetZipChecksum
=================
*/
int FS_GetZipChecksum( const char *zipfile ) 
{
	pack_t *pak;
	int checksum;
	
	pak = FS_LoadZipFile( zipfile );
	
	if ( !pak )
		return 0xFFFFFFFF;
	
	checksum = pak->checksum;
	FS_FreePak( pak );

	return checksum;
} 


/*
=================================================================================

DIRECTORY SCANNING FUNCTIONS

=================================================================================
*/

static int FS_ReturnPath( const char *zname, char *zpath, int *depth ) {
	int len, at, newdep;

	newdep = 0;
	zpath[0] = '\0';
	len = 0;
	at = 0;

	while(zname[at] != 0)
	{
		if (zname[at]=='/' || zname[at]=='\\') {
			len = at;
			newdep++;
		}
		at++;
	}
	strcpy(zpath, zname);
	zpath[len] = '\0';
	*depth = newdep;

	return len;
}


char *FS_CopyString( const char *in ) {
	char *out;
	//out = S_Malloc( strlen( in ) + 1 );
	out = Z_Malloc( strlen( in ) + 1 );
	strcpy( out, in );
	return out;
}


/*
==================
FS_AddFileToList
==================
*/
static int FS_AddFileToList( const char *name, char **list, int nfiles ) {
	int		i;

	if ( nfiles == MAX_FOUND_FILES - 1 ) {
		return nfiles;
	}
	for ( i = 0 ; i < nfiles ; i++ ) {
		if ( !Q_stricmp( name, list[i] ) ) {
			return nfiles; // already in list
		}
	}
	list[ nfiles ] = FS_CopyString( name );
	nfiles++;

	return nfiles;
}

static fnamecallback_f fnamecallback = NULL;

void FS_SetFilenameCallback( fnamecallback_f func ) 
{
	fnamecallback = func;
}


/*
===============
FS_ListFilteredFiles

Returns a unique list of files that match the given criteria
from all search paths
===============
*/
static char **FS_ListFilteredFiles( const char *path, const char *extension, const char *filter, int *numfiles, int flags ) {
	int				nfiles;
	char			**listCopy;
	char			*list[MAX_FOUND_FILES];
	const searchpath_t	*search;
	int				i;
	int				pathLength;
	int				extLen;
	int				length, pathDepth, temp;
	pack_t			*pak;
	fileInPack_t	*buildBuffer;
	char			zpath[MAX_ZPATH];
	qboolean		hasPatterns;
	const char		*x;

	if ( !fs_searchpaths ) {
		Com_Error( ERR_FATAL, "Filesystem call made without initialization" );
	}

	if ( !path ) {
		*numfiles = 0;
		return NULL;
	}

	if ( !extension ) {
		extension = "";
	}

	extLen = (int)strlen( extension );
	hasPatterns = Com_HasPatterns( extension );
	if ( hasPatterns && extension[0] == '.' && extension[1] != '\0' ) {
		extension++;
	}

	pathLength = strlen( path );
	if ( pathLength > 0 && ( path[pathLength-1] == '\\' || path[pathLength-1] == '/' ) ) {
		pathLength--;
	}
	nfiles = 0;
	FS_ReturnPath(path, zpath, &pathDepth);

	//
	// search through the path, one element at a time, adding to list
	//
	for (search = fs_searchpaths ; search ; search = search->next) {
		// is the element a pak file?
		if ( search->pack && ( flags & FS_MATCH_PK3s ) ) {

			// look through all the pak file elements
			pak = search->pack;
			buildBuffer = pak->buildBuffer;
			for (i = 0; i < pak->numfiles; i++) {
				const char *name;
				int zpathLen, depth;

				// check for directory match
				name = buildBuffer[i].name;
				//
				if ( filter ) {
					// case insensitive
					if ( !Com_FilterPath( filter, name ) )
						continue;
					// unique the match
					nfiles = FS_AddFileToList( name, list, nfiles );
				}
				else {

					zpathLen = FS_ReturnPath(name, zpath, &depth);

					if ( (depth-pathDepth)>2 || pathLength > zpathLen || Q_stricmpn( name, path, pathLength ) ) {
						continue;
					}

					// check for extension match
					length = (int)strlen( name );

					if ( fnamecallback ) {
						// use custom filter
						if ( !fnamecallback( name, length ) )
							continue;
					} else {
						if ( length < extLen )
							continue;
						if ( *extension ) {
							if ( hasPatterns ) {
								x = strrchr( name, '.' );
								if ( !x || !Com_FilterExt( extension, x+1 ) ) {
									continue;
								}
							} else {
								if ( Q_stricmp( name + length - extLen, extension ) ) {
									continue;
								}
							}
						}
					}
					// unique the match

					temp = pathLength;
					if (pathLength) {
						temp++;		// include the '/'
					}
					nfiles = FS_AddFileToList( name + temp, list, nfiles );
				}
			}
		} else if ( search->dir && ( flags & FS_MATCH_EXTERN ) && search->policy != DIR_DENY ) { // scan for files in the filesystem
			const char *netpath;
			int		numSysFiles;
			char	**sysFiles;
			const char *name;

			netpath = FS_BuildOSPath( search->dir->path, search->dir->gamedir, path );
			sysFiles = Sys_ListFiles( netpath, extension, filter, &numSysFiles, qfalse );
			for ( i = 0 ; i < numSysFiles ; i++ ) {
				// unique the match
				name = sysFiles[ i ];
				length = strlen( name );
				if ( fnamecallback ) {
					// use custom filter
					if ( !fnamecallback( name, length ) )
						continue;
				} // else - should be already filtered by Sys_ListFiles

				nfiles = FS_AddFileToList( name, list, nfiles );
			}
			Sys_FreeFileList( sysFiles );
		}		
	}

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

	return listCopy;
}


/*
=================
FS_ListFiles
=================
*/
char **FS_ListFiles( const char *path, const char *extension, int *numfiles ) 
{
	return FS_ListFilteredFiles( path, extension, NULL, numfiles, FS_MATCH_ANY );
}


/*
=================
FS_FreeFileList
=================
*/
void FS_FreeFileList( char **list ) {
	int		i;

	if ( !fs_searchpaths ) {
		Com_Error( ERR_FATAL, "Filesystem call made without initialization" );
	}

	if ( !list ) {
		return;
	}

	for ( i = 0 ; list[i] ; i++ ) {
		Z_Free( list[i] );
	}

	Z_Free( list );
}


/*
================
FS_GetFileList
================
*/
int	FS_GetFileList( const char *path, const char *extension, char *listbuf, int bufsize ) {
	int		nFiles, i, nTotal, nLen;
	char **pFiles = NULL;

	*listbuf = '\0';
	nFiles = 0;
	nTotal = 0;

	pFiles = FS_ListFiles(path, extension, &nFiles);

	for (i =0; i < nFiles; i++) {
		nLen = strlen(pFiles[i]) + 1;
		if (nTotal + nLen + 1 < bufsize) {
			strcpy(listbuf, pFiles[i]);
			listbuf += nLen;
			nTotal += nLen;
		}
		else {
			nFiles = i;
			break;
		}
	}

	FS_FreeFileList(pFiles);

	return nFiles;
}


/*
=======================
Sys_ConcatenateFileLists

mkv: Naive implementation. Concatenates three lists into a
     new list, and frees the old lists from the heap.
bk001129 - from cvs1.17 (mkv)

FIXME TTimo those two should move to common.c next to Sys_ListFiles
=======================
 */
static unsigned int Sys_CountFileList( char **list )
{
	int i = 0;

	if ( list )
	{
		while ( *list )
		{
			list++;
			i++;
		}
	}

	return i;
}


static char** Sys_ConcatenateFileLists( char **list0, char **list1 )
{
	int totalLength;
	char **src, **dst, **cat;

	totalLength = Sys_CountFileList( list0 );
	totalLength += Sys_CountFileList( list1 );

	/* Create new list. */
	dst = cat = Z_Malloc( ( totalLength + 1 ) * sizeof( char* ) );

	/* Copy over lists. */
	if ( list0 )
	{
		for (src = list0; *src; src++, dst++)
			*dst = *src;
	}

	if ( list1 )
	{
		for ( src = list1; *src; src++, dst++ )
			*dst = *src;
	}

	// Terminate the list
	*dst = NULL;

	// Free our old lists.
	// NOTE: not freeing their content, it's been merged in dst and still being used
	if ( list0 ) Z_Free( list0 );
	if ( list1 ) Z_Free( list1 );

	return cat;
}

/*
===========
FS_ConvertPath
===========
*/
static void FS_ConvertPath( char *s ) {
	while (*s) {
		if ( *s == '\\' || *s == ':' ) {
			*s = '/';
		}
		s++;
	}
}


/*
===========
FS_PathCmp

Ignore case and separator char distinctions
===========
*/
static int FS_PathCmp( const char *s1, const char *s2 ) {
	int		c1, c2;
	
	do {
		c1 = *s1++;
		c2 = *s2++;

		if (c1 >= 'a' && c1 <= 'z') {
			c1 -= ('a' - 'A');
		}
		if (c2 >= 'a' && c2 <= 'z') {
			c2 -= ('a' - 'A');
		}

		if ( c1 == '\\' || c1 == ':' ) {
			c1 = '/';
		}
		if ( c2 == '\\' || c2 == ':' ) {
			c2 = '/';
		}
		
		if (c1 < c2) {
			return -1;		// strings not equal
		}
		if (c1 > c2) {
			return 1;
		}
	} while (c1);
	
	return 0;		// strings are equal
}


/*
================
FS_SortFileList
================
*/
static void FS_SortFileList( char **list, int n ) {
	const char *m;
	char *temp;
	int i, j;
	i = 0;
	j = n;
	m = list[ n >> 1 ];
	do {
		while ( FS_PathCmp( list[i], m ) < 0 ) i++;
		while ( FS_PathCmp( list[j], m ) > 0 ) j--;
		if ( i <= j ) {
			temp = list[i];
			list[i] = list[j];
			list[j] = temp;
			i++; 
			j--;
		}
	} while ( i <= j );
	if ( j > 0 ) FS_SortFileList( list, j );
	if ( n > i ) FS_SortFileList( list+i, n-i );
}

/*
================
FS_AddGameDirectory

Sets fs_gamedir, adds the directory to the head of the path,
then loads the zip headers
================
*/
static void FS_AddGameDirectory( const char *path, const char *dir ) {
	const searchpath_t *sp;
	int				len;
	searchpath_t	*search;
	const char		*gamedir;
	pack_t			*pak;
	char			curpath[MAX_OSPATH*2 + 1];
	char			*pakfile;
	int				numfiles;
	char			**pakfiles;
	int				pakfilesi;
	int				numdirs;
	char			**pakdirs;
	int				pakdirsi;
	int				pakwhich;
	int				path_len;
	int				dir_len;

	for ( sp = fs_searchpaths ; sp ; sp = sp->next ) {
		if ( sp->dir && !Q_stricmp( sp->dir->path, path ) && !Q_stricmp( sp->dir->gamedir, dir )) {
			return;	// we've already got this one
		}
	}
	
	Q_strncpyz( fs_gamedir, dir, sizeof( fs_gamedir ) );

	//
	// add the directory to the search path
	//
	path_len = (int) strlen( path ) + 1;
	path_len = PAD( path_len, sizeof( int ) );
	dir_len = (int) strlen( dir ) + 1;
	dir_len = PAD( dir_len, sizeof( int ) );
	len = sizeof( *search ) + sizeof( *search->dir ) + path_len + dir_len;

	search = Z_TagMalloc( len, TAG_SEARCH_PATH );
	Com_Memset( search, 0, len );
	search->dir = (directory_t*)( search + 1 );
	search->dir->path = (char*)( search->dir + 1 );
	search->dir->gamedir = (char*)( search->dir->path + path_len );

	strcpy( search->dir->path, path );
	strcpy( search->dir->gamedir, dir );
	gamedir = search->dir->gamedir;

	search->next = fs_searchpaths;
	fs_searchpaths = search;
	fs_dirCount++;

	// find all pak files in this directory
	Q_strncpyz( curpath, FS_BuildOSPath( path, dir, NULL ), sizeof( curpath ) );

	// Get .pk3 files
	pakfiles = Sys_ListFiles(curpath, ".pk3", NULL, &numfiles, qfalse);

	if ( numfiles >= 2 )
		FS_SortFileList( pakfiles, numfiles - 1 );

	pakfilesi = 0;
	pakdirsi = 0;

	// Get top level directories (we'll filter them later since the Sys_ListFiles filtering is terrible)
	pakdirs = Sys_ListFiles( curpath, "/", NULL, &numdirs, qfalse );
	if ( numdirs >= 2 ) {
		FS_SortFileList( pakdirs, numdirs - 1 );
	}

	while (( pakfilesi < numfiles) || (pakdirsi < numdirs) ) 
	{
		// Check if a pakfile or pakdir comes next
		if (pakfilesi >= numfiles) {
			// We've used all the pakfiles, it must be a pakdir.
			pakwhich = 0;
		}
		else if (pakdirsi >= numdirs) {
			// We've used all the pakdirs, it must be a pakfile.
			pakwhich = 1;
		}
		else {
			// Could be either, compare to see which name comes first
			pakwhich = (FS_PathCmp( pakfiles[pakfilesi], pakdirs[pakdirsi] ) < 0);
		}

		if ( pakwhich ) {

			len = strlen( pakfiles[pakfilesi] );
			if ( !FS_IsExt( pakfiles[pakfilesi], ".pk3", len ) ) {
				// not a pk3 file
				pakfilesi++;
				continue;
			}

			// The next .pk3 file is before the next .pk3dir
			pakfile = FS_BuildOSPath( path, dir, pakfiles[pakfilesi] );
			if ( (pak = FS_LoadZipFile( pakfile ) ) == NULL ) {
				// This isn't a .pk3! Next!
				pakfilesi++;
				continue;
			}

			// store the game name for downloading
			pak->pakGamename = gamedir;

			pak->index = fs_packCount;
			pak->referenced = 0;
			pak->exclude = qfalse;

			fs_packFiles += pak->numfiles;
			fs_packCount++;

			search = Z_TagMalloc( sizeof( *search ), TAG_SEARCH_PACK );
			Com_Memset( search, 0, sizeof( *search ) );
			search->pack = pak;

			search->next = fs_searchpaths;
			fs_searchpaths = search;

			pakfilesi++;
		} else {

			len = strlen(pakdirs[pakdirsi]);

			// The next .pk3dir is before the next .pk3 file
			// But wait, this could be any directory, we're filtering to only ending with ".pk3dir" here.
			if (!FS_IsExt(pakdirs[pakdirsi], va(".%s",cl_selectedmod->string), len)) {
				// This isn't a .pk3dir! Next!
				pakdirsi++;
				continue;
			}

			// add the directory to the search path
			path_len = (int) strlen( curpath ) + 1; 
			path_len = PAD( path_len, sizeof( int ) );
			dir_len = PAD( len + 1, sizeof( int ) );
			len = sizeof( *search ) + sizeof( *search->dir ) + path_len + dir_len;

			search = Z_TagMalloc( len, TAG_SEARCH_DIR );
			Com_Memset( search, 0, len );
			search->dir = (directory_t*)(search + 1);
			search->dir->path = (char*)( search->dir + 1 );
			search->dir->gamedir = (char*)( search->dir->path + path_len );
			search->policy = DIR_ALLOW;

			strcpy( search->dir->path, curpath );				// c:\quake3\baseq3
			strcpy( search->dir->gamedir, pakdirs[ pakdirsi ] );// mypak.pk3dir

			search->next = fs_searchpaths;
			fs_searchpaths = search;
			fs_pk3dirCount++;

			pakdirsi++;
		}
	}

	// done
	Sys_FreeFileList( pakdirs );
	Sys_FreeFileList( pakfiles );
}

/*
================
FS_InvalidGameDir
return true if path is a reference to current directory or directory traversal
or a sub-directory
================
*/
qboolean FS_InvalidGameDir( const char *gamedir ) 
{
	if ( !strcmp( gamedir, "." ) || !strcmp( gamedir, ".." )
		|| strchr( gamedir, '/' ) || strchr( gamedir, '\\' ) ) {
		return qtrue;
	}

	return qfalse;
}


/*
================
FS_ComparePaks

Returns a list of pak files that we should download from the server. They all get stored
in the current gamedir and an FS_Restart will be fired up after we download them all.
================
*/
qboolean FS_ComparePaks( char *neededpaks, int len, qboolean dlstring ) {
	const searchpath_t	*sp;
	qboolean havepak;
	char *origpos = neededpaks;
	int i;

	if (!fs_numServerReferencedPaks)
		return qfalse; // Server didn't send any pack information along

	*neededpaks = '\0';

	for ( i = 0 ; i < fs_numServerReferencedPaks ; i++ )
	{
		// Ok, see if we have this pak file
		havepak = qfalse;

		// Make sure the server cannot make us write to non-quake3 directories.
		if ( FS_CheckDirTraversal( fs_serverReferencedPakNames[i] ) ) {
			Com_Printf( "WARNING: Invalid download name %s\n", fs_serverReferencedPakNames[i] );
			continue;
		}

		for ( sp = fs_searchpaths ; sp ; sp = sp->next ) {
			if ( sp->pack && sp->pack->checksum == fs_serverReferencedPaks[i] ) {
				havepak = qtrue; // This is it!
				break;
			}
		}

		if ( !havepak && fs_serverReferencedPakNames[i] && *fs_serverReferencedPakNames[i] ) { 
			// Don't got it

      if (dlstring)
      {
		// We need this to make sure we won't hit the end of the buffer or the server could
		// overwrite non-pk3 files on clients by writing so much crap into neededpaks that
		// Q_strcat cuts off the .pk3 extension.
	
		origpos += strlen(origpos);
	
        // Remote name
        Q_strcat( neededpaks, len, "@");
        Q_strcat( neededpaks, len, fs_serverReferencedPakNames[i] );
        Q_strcat( neededpaks, len, ".pk3" );

        // Local name
        Q_strcat( neededpaks, len, "@");
        // Do we have one with the same name?
        if ( FS_SV_FileExists( va( "%s.pk3", fs_serverReferencedPakNames[i] ) ) )
        {
          char st[MAX_ZPATH];
          // We already have one called this, we need to download it to another name
          // Make something up with the checksum in it
          Com_sprintf( st, sizeof( st ), "%s.%08x.pk3", fs_serverReferencedPakNames[i], fs_serverReferencedPaks[i] );
          Q_strcat( neededpaks, len, st );
        } else
        {
          Q_strcat( neededpaks, len, fs_serverReferencedPakNames[i] );
          Q_strcat( neededpaks, len, ".pk3" );
        }
        
        // Find out whether it might have overflowed the buffer and don't add this file to the
        // list if that is the case.
        if(strlen(origpos) + (origpos - neededpaks) >= len - 1)
	{
		*origpos = '\0';
		break;
	}
      }
      else
      {
        Q_strcat( neededpaks, len, fs_serverReferencedPakNames[i] );
			  Q_strcat( neededpaks, len, ".pk3" );
        // Do we have one with the same name?
        if ( FS_SV_FileExists( va( "%s.pk3", fs_serverReferencedPakNames[i] ) ) )
        {
          Q_strcat( neededpaks, len, " (local file exists with wrong checksum)");
        }
        Q_strcat( neededpaks, len, "\n");
      }
		}
	}

	if ( *neededpaks ) {
		return qtrue;
	}

	return qfalse; // We have them all
}


/*
================
FS_Shutdown

Frees all resources.
================
*/
void FS_Shutdown( qboolean closemfp )
{
	searchpath_t	*p, *next;
	int i;

	// close opened files
	if ( closemfp ) 
	{
		for ( i = 1; i < MAX_FILE_HANDLES; i++ )
		{
			if ( !fsh[i].handleFiles.file.v  )
				continue;

			FS_FCloseFile( i );
		}
	}

	if ( fs_searchpaths ) Com_WriteConfiguration();

	// free everything
	for( p = fs_searchpaths; p; p = next )
	{
		next = p->next;

		if ( p->pack )
		{
			FS_FreePak( p->pack );
			p->pack = NULL;
		}

		Z_Free( p );
	}

	// any FS_ calls will now be an error until reinitialized
	fs_searchpaths = NULL;
	fs_packFiles = 0;

	fs_pk3dirCount = 0;
	fs_packCount = 0;
	fs_dirCount = 0;

	Cmd_RemoveCommand( "fs_restart" );
}

/*
================
FS_ReorderSearchPaths
================
*/
static void FS_ReorderSearchPaths( void ) {
	searchpath_t **list, **paks, **dirs;
	searchpath_t *path;
	int i, ndirs, npaks, cnt;

	cnt = fs_packCount + fs_dirCount + fs_pk3dirCount;
	if ( cnt == 0 )
		return;

	// relink path chains in following order:
	// 1. pk3dirs @ pak files
	// 2. directories
	list = (searchpath_t **)Z_Malloc( cnt * sizeof( list[0] ) );
	paks = list;
	dirs = list + fs_pk3dirCount + fs_packCount;

	npaks = ndirs = 0;
	path = fs_searchpaths;
	while ( path ) {
		if ( path->pack || path->policy != DIR_STATIC ) {
			paks[npaks++] = path;
		} else {
			dirs[ndirs++] = path;
		}
		path = path->next;
	}

	fs_searchpaths = list[0];
	for ( i = 0; i < cnt-1; i++ ) {
		list[i]->next = list[i+1];
	}
	list[cnt-1]->next = NULL;

	Z_Free( list );
}

/*
================
FS_Startup
================
*/
static void FS_Startup( void ) {
	const char *homePath;
	int start, end;

	Com_Printf( "----- FS_Startup -----\n" );

	fs_debug = Cvar_Get( "fs_debug", "0", 0 );
	fs_copyfiles = Cvar_Get( "fs_copyfiles", "0", 0 );
	fs_basepath = Cvar_Get( "fs_basepath", Sys_DefaultBasePath(), 0 );
	fs_basegame = Cvar_Get( "fs_basegame", BASEGAME, 0 );

	if ( fs_basegame->string[0] == '\0' ) Com_Error( ERR_FATAL, "* fs_basegame is not set *" );

	homePath = Sys_DefaultHomePath();
	if ( homePath == NULL || homePath[0] == '\0' ) {
		homePath = fs_basepath->string;
	}

	fs_homepath = Cvar_Get( "fs_homepath", homePath, 0 );
	fs_excludeReference = Cvar_Get( "fs_excludeReference", "", CVAR_ARCHIVE | CVAR_LATCH );

	start = Sys_Milliseconds();

	if (fs_basepath->string[0])
		FS_AddGameDirectory( fs_basepath->string, fs_basegame->string );

#ifdef __APPLE__
	fs_apppath = Cvar_Get( "fs_apppath", Sys_DefaultAppPath(), 0 );
	// Make MacOSX also include the base path included with the .app bundle
	if ( fs_apppath->string[0] )
		FS_AddGameDirectory( fs_apppath->string, fs_basegame->string );
#endif

	// fs_homepath is somewhat particular to *nix systems, only add if relevant
	// NOTE: same filtering below for mods and basegame
	if ( fs_homepath->string[0] && Q_stricmp( fs_homepath->string, fs_basepath->string ) )
		FS_AddGameDirectory( fs_homepath->string, fs_basegame->string );

	// reorder search paths to minimize further changes
	FS_ReorderSearchPaths();

	end = Sys_Milliseconds();

	// add our commands
	Cmd_AddCommand( "fs_restart", FS_Reload );

	Com_Printf( "...loaded in %i milliseconds\n", end - start );

	Com_Printf( "----------------------\n" );
	Com_Printf( "%d files in %d pk3 files\n", fs_packFiles, fs_packCount );

#ifdef FS_MISSING
	if (missingFiles == NULL) {
		missingFiles = Sys_FOpen( "\\missing.txt", "ab" );
	}
#endif
}

/*
=====================
FS_LoadedPakNames

Returns a space separated string containing the names of all loaded pk3 files.
=====================
*/
#ifndef DEDICATED
const char *FS_LoadedPakNames( void ) {
	static char	info[BIG_INFO_STRING];
	const searchpath_t *search;
	char *s, *max;
	int len;

	s = info;
	info[0] = '\0';
	max = &info[sizeof(info)-1];

	for ( search = fs_searchpaths ; search ; search = search->next ) {
		// is the element a pak file?
		if ( !search->pack )
			continue;

		if ( search->pack->exclude )
			continue;

		len = (int)strlen( search->pack->pakBasename );
		if ( info[0] )
			len++;

		if ( s + len > max )
			break;

		if ( info[0] )
			s = Q_stradd( s, " " );

		s = Q_stradd( s, search->pack->pakBasename );
	}

	return info;
}
#endif

/*
=====================
FS_ExcludeReference
=====================
*/
qboolean FS_ExcludeReference( void ) {
	const searchpath_t *search;
	const char *pakName;
	int i, nargs;
	qboolean x;

	if ( fs_excludeReference->string[0] == '\0' )
		return qfalse;

	Cmd_TokenizeStringIgnoreQuotes( fs_excludeReference->string );
	nargs = Cmd_Argc();
	x = qfalse;

	for ( search = fs_searchpaths ; search ; search = search->next ) {
		if ( search->pack ) {
			if ( !search->pack->referenced ) {
				continue;
			}
			pakName = va( "%s/%s", search->pack->pakGamename, search->pack->pakBasename );
			for ( i = 0; i < nargs; i++ ) {
				if ( Q_stricmp( Cmd_Argv( i ), pakName ) == 0 ) {
					search->pack->exclude = qtrue;
					x = qtrue;
					break;
				}
			}
		}
	}

	return x;
}

/*
=====================
FS_ReferencedPakNames

Returns a space separated string containing the names of all referenced pk3 files.
The server will send this to the clients so they can check which files should be auto-downloaded. 
=====================
*/
const char *FS_ReferencedPakNames( void ) {
	static char	info[BIG_INFO_STRING];
	const searchpath_t *search;
	const char *pakName;
	info[0] = '\0';

	// we want to return ALL pk3's from the base path
	// and referenced one's from baseq3
	for ( search = fs_searchpaths ; search ; search = search->next ) {
		// is the element a pak file?
		if ( search->pack ) {
			if ( search->pack->exclude ) {
				continue;
			}
			if ( search->pack->referenced ) {
				pakName = va( "%s/%s", search->pack->pakGamename, search->pack->pakBasename );
				if ( *info != '\0' ) {
					Q_strcat( info, sizeof( info ), " " );
				}
				Q_strcat( info, sizeof( info ), pakName );
			}
		}
	}

	return info;
}

/*
=====================
FS_ClearPakReferences
=====================
*/
void FS_ClearPakReferences( int flags ) {
	const searchpath_t *search;

	if ( !flags ) {
		flags = -1;
	}
	for ( search = fs_searchpaths; search; search = search->next ) {
		// is the element a pak file and has it been referenced?
		if ( search->pack ) {
			search->pack->referenced &= ~flags;
		}
	}
}

/*
================
FS_InitFilesystem

Called only at initial startup, not when the filesystem
is resetting due to a game change
================
*/
void FS_InitFilesystem( void ) {
#ifdef _WIN32
 	_setmaxstdio( 2048 );
#endif

	// try to start up normally
	FS_Restart( 0 );
}


/*
================
FS_Restart
================
*/
void FS_Restart( int checksumFeed ) {

	// last valid base folder used
	static char lastValidBase[MAX_OSPATH];

	static qboolean execConfig = qfalse;

	// free anything we currently have loaded
	FS_Shutdown( qfalse );

	// set the checksum feed
	fs_checksumFeed = checksumFeed;

	// try to start up normally
	FS_Startup();

	// if we can't find default.cfg, assume that the paths are
	// busted and error out now, rather than getting an unreadable
	// graphics screen when the font fails to load
	if ( FS_ReadFile( "default.cfg", NULL ) <= 0 ) {
		if (lastValidBase[0]) {
			Cvar_Set( "fs_basepath", lastValidBase );
			lastValidBase[0] = '\0';
			Cvar_Set( "fs_restrict", "0" );
			execConfig = qtrue;
			FS_Restart( checksumFeed );
			Com_Error( ERR_DROP, "Invalid game folder" );
			return;
		}
		Com_Error( ERR_FATAL, "Couldn't load default.cfg" );
	}

	// new check before safeMode
	if ( execConfig ) Cbuf_AddText( "exec " CONFIG_CFG "\n" );
	execConfig = qfalse;

	Q_strncpyz( lastValidBase, fs_basepath->string, sizeof( lastValidBase ) );
}


/*
=================
FS_Reload
=================
*/
void FS_Reload( void ) 
{
	FS_Restart( fs_checksumFeed );
}


/*
=================
FS_ConditionalRestart
restart if necessary
=================
*/
qboolean FS_ConditionalRestart( int checksumFeed, qboolean clientRestart )
{
	if ( checksumFeed != fs_checksumFeed ) {
		FS_Restart( checksumFeed );
		return qtrue;
	}
	
	return qfalse;
}


/*
========================================================================================

Handle based file calls for virtual machines

========================================================================================
*/

int	FS_FOpenFileByMode( const char *qpath, fileHandle_t *f, fsMode_t mode ) {
	int		r;
	qboolean	sync;
	fileHandleData_t *fhd;

	if ( !qpath || !*qpath ) {
		if ( f ) 
			*f = FS_INVALID_HANDLE;
		return -1;
	}

	r = 0;	// file size
	sync = qfalse;

	switch( mode ) {
	case FS_READ:
		r = FS_FOpenFileRead( qpath, f, qtrue );
		break;
	case FS_WRITE:
		if ( f == NULL )
			return -1;
		*f = FS_FOpenFileWrite( qpath );
		break;
	case FS_APPEND_SYNC:
		sync = qtrue;
	case FS_APPEND:
		if ( f == NULL )
			return -1;
		*f = FS_FOpenFileAppend( qpath );
		break;
	default:
		Com_Error( ERR_FATAL, "FSH_FOpenFile: bad mode %i", mode );
		return -1;
	}

	if ( !f )
		return r;

	if ( *f == FS_INVALID_HANDLE ) {
		return -1;
	}

	fhd = &fsh[ *f ];

	fhd->handleSync = sync;

	return r;
}


int FS_FTell( fileHandle_t f ) {
	int pos;
	if ( fsh[f].zipFile ) {
		pos = unztell( fsh[f].handleFiles.file.z );
	} else {
		pos = ftell( fsh[f].handleFiles.file.o );
	}
	return pos;
}


void FS_Flush( fileHandle_t f ) 
{
	fflush( fsh[f].handleFiles.file.o );
}

int FS_VM_OpenFile( const char *qpath, fileHandle_t *f, fsMode_t mode ) {
	int r;

	r = FS_FOpenFileByMode( qpath, f, mode );

	return r;
}


int FS_VM_ReadFile( void *buffer, int len, fileHandle_t f ) {

	if ( f <= 0 || f >= MAX_FILE_HANDLES )
		return 0;

	if ( !fsh[f].handleFiles.file.v )
		return 0; 

	return FS_Read( buffer, len, f );
}


void FS_VM_WriteFile( void *buffer, int len, fileHandle_t f ) {

	if ( f <= 0 || f >= MAX_FILE_HANDLES )
		return;

	if ( !fsh[f].handleFiles.file.v )
		return;

	FS_Write( buffer, len, f );
}

void FS_VM_CloseFile( fileHandle_t f ) {

	if ( f <= 0 || f >= MAX_FILE_HANDLES )
		return;

	if ( !fsh[f].handleFiles.file.v )
		return;

	FS_FCloseFile( f );
}

const char *FS_GetBaseGameDir( void ) { return fs_basegame->string; }


const char *FS_GetBasePath( void )
{
	if ( fs_basepath && fs_basepath->string[0] != '\0' )
		return fs_basepath->string;
	else
		return "";
}


const char *FS_GetHomePath( void )
{
	if ( fs_homepath && fs_homepath->string[0] != '\0' )
		return fs_homepath->string;
	else
		return FS_GetBasePath();
}


fileHandle_t FS_PipeOpenWrite( const char *cmd, const char *filename ) {
	fileHandleData_t *fd;
	fileHandle_t f;
	const char *ospath;

	if ( !fs_searchpaths ) {
		Com_Error( ERR_FATAL, "Filesystem call made without initialization" );
	}

	ospath = FS_BuildOSPath( fs_homepath->string, fs_gamedir, filename );

	if ( fs_debug->integer ) {
		Com_Printf( "FS_PipeOpenWrite: %s\n", ospath );
	}

	f = FS_HandleForFile();
	fd = &fsh[ f ];
	FS_InitHandle( fd );

	if ( FS_CreatePath( ospath ) ) {
		return FS_INVALID_HANDLE;
	}

#ifdef _WIN32
	fd->handleFiles.file.o = _popen( cmd, "wb" );
#else
	fd->handleFiles.file.o = popen( cmd, "w" );
#endif

	if ( fd->handleFiles.file.o == NULL ) {
		return FS_INVALID_HANDLE;
	}

	Q_strncpyz( fd->name, filename, sizeof( fd->name ) );
	fd->handleSync = qfalse;
	fd->zipFile = qfalse;

	return f;
}


void FS_PipeClose( fileHandle_t f )
{
	if ( !fs_searchpaths )
		Com_Error( ERR_FATAL, "Filesystem call made without initialization" );

	if ( fsh[f].zipFile )
		return;

	if ( fsh[f].handleFiles.file.o ) {
#ifdef _WIN32
		_pclose( fsh[f].handleFiles.file.o );
#else
		pclose( fsh[f].handleFiles.file.o );
#endif
	}

	Com_Memset( &fsh[f], 0, sizeof( fsh[f] ) );
}
