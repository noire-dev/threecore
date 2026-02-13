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

#define MAX_ZPATH			256

typedef struct {
	char		*path;		// c:\quake3
	char		*gamedir;	// baseq3
} directory_t;

typedef enum {
	DIR_STATIC = 0,	// always allowed, never changes
	DIR_ALLOW,
	DIR_DENY
} dirPolicy_t;

static	int			fs_readCount;			// total bytes read
static	int			fs_loadCount;			// total files read
static	int			fs_loadStack;			// total files in memory
static	int			fs_packFiles;			// total number of files in all loaded packs

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
	char		name[MAX_ZPATH];
} fileHandleData_t;

static fileHandleData_t	fsh[MAX_FILE_HANDLES];

void FS_Reload( void );

qboolean FS_Initialized( void ) {
	return qtrue;
}

int FS_LoadStack( void ) {
	return fs_loadStack;
}

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

char *FS_BuildPath( const char *qpath ) {
	static char ospath[sizeof(temp)+MAX_OSPATH];
	
	if(!qpath) Com_Error(ERR_FATAL, "FS_BuildPath NULL path!");
	FS_ReplaceSeparators( temp );
	Com_sprintf( ospath, sizeof(ospath), "%s%c%s", Sys_DefaultBasePath, PATH_SEP, qpath );
	return ospath;
}

static qboolean FS_CreatePath( const char *OSPath ) {
	char	path[MAX_OSPATH*2+1];
	char	*ofs;

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

void FS_FCloseFile( fileHandle_t f ) {
	fileHandleData_t *fd;

	fd = &fsh[ f ];

	if ( fd->handleFiles.file.o ) {
		fclose( fd->handleFiles.file.o );
		fd->handleFiles.file.o = NULL;
	}

	Com_Memset( fd, 0, sizeof( *fd ) );
}

fileHandle_t FS_FOpenFileWrite( const char *filename ) {
	char ospath[MAX_OSPATH];
	fileHandle_t f;
	fileHandleData_t *fd;

	if ( !filename || !*filename ) {
		return FS_INVALID_HANDLE;
	}

	// Строим путь: <exe_dir>/<filename>
	Q_strncpyz( ospath, Sys_DefaultBasePath(), sizeof( ospath ) );
	Q_strcat( ospath, sizeof( ospath ), "/" );
	Q_strcat( ospath, sizeof( ospath ), filename );

	f = FS_HandleForFile();
	fd = &fsh[ f ];

	fd->handleFiles.file.o = Sys_FOpen( ospath, "wb" );
	if ( fd->handleFiles.file.o == NULL ) {
		// Попробуем создать путь (включая подкаталоги)
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

	return f;
}

fileHandle_t FS_FOpenFileAppend( const char *filename ) {
	char ospath[MAX_OSPATH];
	fileHandle_t f;
	fileHandleData_t *fd;

	if ( !filename || !*filename ) {
		return FS_INVALID_HANDLE;
	}

	// Строим путь: <exe_dir>/<filename>
	Q_strncpyz( ospath, Sys_DefaultBasePath(), sizeof( ospath ) );
	Q_strcat( ospath, sizeof( ospath ), "/" );
	Q_strcat( ospath, sizeof( ospath ), filename );

	f = FS_HandleForFile();
	fd = &fsh[ f ];

	fd->handleFiles.file.o = Sys_FOpen( ospath, "ab" );
	if ( fd->handleFiles.file.o == NULL ) {
		// Попробуем создать путь (включая подкаталоги)
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

	return f;
}

int FS_FOpenFileRead( const char *filename, fileHandle_t *file, qboolean uniqueFILE ) {
	char netpath[MAX_OSPATH];
	FILE *temp;
	int length;

	if ( !filename ) {
		Com_Error( ERR_FATAL, "FS_FOpenFileRead: NULL 'filename' parameter passed\n" );
	}

	// Убираем начальный слэш, если есть
	if ( filename[0] == '/' || filename[0] == '\\' ) {
		filename++;
	}

	// Строим путь: <exe_dir>/<filename>
	Q_strncpyz( netpath, Sys_DefaultBasePath(), sizeof( netpath ) );
	Q_strcat( netpath, sizeof( netpath ), "/" );
	Q_strcat( netpath, sizeof( netpath ), filename );

	// Открываем файл
	temp = Sys_FOpen( netpath, "rb" );
	if ( !temp ) {
		if ( file ) {
			*file = FS_INVALID_HANDLE;
		}
		return -1;
	}

	// Если запрашивали только длину — закрываем и возвращаем её
	if ( !file ) {
		length = FS_FileLength( temp );
		fclose( temp );
		return length;
	}

	// Иначе выделяем хэндл и сохраняем FILE*
	*file = FS_HandleForFile();
	fileHandleData_t *f = &fsh[*file];
	f->handleFiles.file.o = temp;
	Q_strncpyz( f->name, filename, sizeof( f->name ) );

	return FS_FileLength( temp );
}

int FS_Read( void *buffer, int len, fileHandle_t f ) {
	int		block, remaining;
	int		read;
	byte	*buf;
	int		tries;

	if ( f <= 0 || f >= MAX_FILE_HANDLES ) {
		return 0;
	}

	buf = (byte *)buffer;
	fs_readCount += len;

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
}

int FS_Write( const void *buffer, int len, fileHandle_t h ) {
	int		block, remaining;
	int		written;
	byte	*buf;
	int		tries;
	FILE	*f;

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

int FS_Seek( fileHandle_t f, long offset, fsOrigin_t origin ) {
	int		_origin;

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

int FS_ReadFile( const char *qpath, void **buffer ) {
	fileHandle_t	h;
	byte*			buf;
	long			len;

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

char *FS_CopyString( const char *in ) {
	char *out;
	out = Z_Malloc( strlen( in ) + 1 );
	strcpy( out, in );
	return out;
}

static char **FS_ListFiles( const char *path, const char *extension, int *numfiles ) {
	char netpath[MAX_OSPATH];
	char **sysFiles;
	int numSysFiles;
	char **listCopy;
	int i;

	if ( !path ) {
		*numfiles = 0;
		return NULL;
	}

	if ( !extension ) {
		extension = "";
	}

	// Строим путь: <exe_dir>/<path>
	Q_strncpyz( netpath, Sys_DefaultBasePath(), sizeof( netpath ) );
	Q_strcat( netpath, sizeof( netpath ), "/" );
	Q_strcat( netpath, sizeof( netpath ), path );

	// Просим систему перечислить файлы
	sysFiles = Sys_ListFiles( netpath, extension, NULL, &numSysFiles, qfalse );
	*numfiles = numSysFiles;

	if ( !numSysFiles ) {
		if ( sysFiles ) {
			Sys_FreeFileList( sysFiles );
		}
		return NULL;
	}

	// Возвращаем копию списка (Sys_ListFiles уже выделяет строки через Z_Malloc)
	listCopy = Z_Malloc( (numSysFiles + 1) * sizeof(char*) );
	for ( i = 0; i < numSysFiles; i++ ) {
		listCopy[i] = sysFiles[i];
	}
	listCopy[i] = NULL;

	// Освобождаем оболочку списка, но не строки — они теперь в listCopy
	Z_Free( sysFiles );

	return listCopy;
}

void FS_FreeFileList( char **list ) {
	int		i;

	if ( !list ) {
		return;
	}

	for ( i = 0 ; list[i] ; i++ ) {
		Z_Free( list[i] );
	}

	Z_Free( list );
}

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

void FS_Shutdown( qboolean closemfp )
{
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
}

void FS_InitFilesystem( void ) {
#ifdef _WIN32
 	_setmaxstdio( 2048 );
#endif

	FS_Restart(0);
}

void FS_Restart( int checksumFeed ) {
	if(FS_ReadFile("default.cfg", NULL) <= 0) Com_Error(ERR_FATAL, "Couldn't load default.cfg");
	Cbuf_AddText("exec " CONFIG_CFG "\n");
}

void FS_Reload( void ) 
{
	FS_Restart( 0 );
}

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
	return ftell( fsh[f].handleFiles.file.o );
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
