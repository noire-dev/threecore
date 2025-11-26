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
// cl_main.c  -- client main loop

#include "client.h"
#include <limits.h>

cvar_t	*cl_noprint;
cvar_t	*cl_debugMove;

cvar_t	*rcon_client_password;
cvar_t	*rconAddress;

cvar_t	*cl_timeout;

cvar_t	*cl_shownet;

cvar_t	*cl_aviFrameRate;
cvar_t	*cl_aviMotionJpeg;
cvar_t	*cl_aviPipeFormat;

cvar_t	*cl_activeAction;

cvar_t	*cl_allowDownload;

cvar_t	*cl_serverStatusResendTime;

cvar_t	*cl_lanForcePackets;

cvar_t *r_swapInterval;
cvar_t *r_fullscreen;
cvar_t *r_resolution;

cvar_t *r_availableModes;

clientActive_t		cl;
clientConnection_t	clc;
clientStatic_t		cls;
vm_t				*cgvm = NULL;

netadr_t			rcon_address;

// Structure containing functions exported from refresh DLL
refexport_t	re;

static ping_t cl_pinglist[MAX_PINGREQUESTS];

typedef struct serverStatus_s
{
	char string[BIG_INFO_STRING];
	netadr_t address;
	int time, startTime;
	qboolean pending;
	qboolean print;
	qboolean retrieved;
} serverStatus_t;

static serverStatus_t cl_serverStatusList[MAX_SERVERSTATUSREQUESTS];

static void CL_CheckForResend( void );
static void CL_ShowIP_f( void );
static void CL_ServerStatus_f( void );
static void CL_ServerStatusResponse( const netadr_t *from, msg_t *msg );
static void CL_ServerInfoPacket( const netadr_t *from, msg_t *msg );

static void CL_LocalServers_f( void );
static void CL_GlobalServers_f( void );
static void CL_Ping_f( void );

static void CL_InitRef( void );
static void CL_ShutdownRef( refShutdownCode_t code );
static void CL_InitGLimp_Cvars( void );

/*
=======================================================================

CLIENT RELIABLE COMMAND COMMUNICATION

=======================================================================
*/

/*
======================
CL_AddReliableCommand

The given command will be transmitted to the server, and is guaranteed to
not have future usercmd_t executed before it is executed
======================
*/
void CL_AddReliableCommand( const char *cmd, qboolean isDisconnectCmd ) {
	int		index;
	int		unacknowledged = clc.reliableSequence - clc.reliableAcknowledge;

	if ( clc.serverAddress.type == NA_BAD )
		return;

	// if we would be losing an old command that hasn't been acknowledged,
	// we must drop the connection
	// also leave one slot open for the disconnect command in this case.

	if ((isDisconnectCmd && unacknowledged > MAX_RELIABLE_COMMANDS) ||
		(!isDisconnectCmd && unacknowledged >= MAX_RELIABLE_COMMANDS))
	{
		if( com_errorEntered )
			return;
		else
			Com_Error(ERR_DROP, "Client command overflow");
	}

	clc.reliableSequence++;
	index = clc.reliableSequence & ( MAX_RELIABLE_COMMANDS - 1 );
	Q_strncpyz( clc.reliableCommands[ index ], cmd, sizeof( clc.reliableCommands[ index ] ) );
}


/*
=======================================================================

CLIENT SIDE DEMO RECORDING

=======================================================================
*/

/*
====================
CL_WriteDemoMessage

Dumps the current net message, prefixed by the length
====================
*/
static void CL_WriteDemoMessage( msg_t *msg, int headerBytes ) {
	int		len, swlen;

	// write the packet sequence
	len = clc.serverMessageSequence;
	swlen = LittleLong( len );
	FS_Write( &swlen, 4, clc.recordfile );

	// skip the packet sequencing information
	len = msg->cursize - headerBytes;
	swlen = LittleLong(len);
	FS_Write( &swlen, 4, clc.recordfile );
	FS_Write( msg->data + headerBytes, len, clc.recordfile );
}


/*
====================
CL_StopRecording_f

stop recording a demo
====================
*/
void CL_StopRecord_f( void ) {

	if ( clc.recordfile != FS_INVALID_HANDLE ) {
		char tempName[MAX_OSPATH];
		char finalName[MAX_OSPATH];
		int	len, sequence;

		// finish up
		len = -1;
		FS_Write( &len, 4, clc.recordfile );
		FS_Write( &len, 4, clc.recordfile );
		FS_FCloseFile( clc.recordfile );
		clc.recordfile = FS_INVALID_HANDLE;

		Com_sprintf( tempName, sizeof( tempName ), "%s.tmp", clc.recordName );

		Com_sprintf( finalName, sizeof( finalName ), "%s.%s", clc.recordName, "demo" );

		if ( clc.explicitRecordName ) {
			FS_Remove( finalName );
		} else {
			// add sequence suffix to avoid overwrite
			sequence = 0;
			while ( FS_FileExists( finalName ) && ++sequence < 1000 ) {
				Com_sprintf( finalName, sizeof( finalName ), "%s-%02d.%s",
					clc.recordName, sequence, "demo" );
			}
		}

		FS_Rename( tempName, finalName );
	}

	if ( !clc.demorecording ) {
		Com_Printf( "Not recording a demo.\n" );
	} else {
		Com_Printf( "Stopped demo recording.\n" );
	}

	clc.demorecording = qfalse;
}


/*
====================
CL_WriteServerCommands
====================
*/
static void CL_WriteServerCommands( msg_t *msg ) {
	int i;

	if ( clc.serverCommandSequence - clc.demoCommandSequence > 0 ) {

		// do not write more than MAX_RELIABLE_COMMANDS
		if ( clc.serverCommandSequence - clc.demoCommandSequence > MAX_RELIABLE_COMMANDS ) {
			clc.demoCommandSequence = clc.serverCommandSequence - MAX_RELIABLE_COMMANDS;
		}

		for ( i = clc.demoCommandSequence + 1 ; i <= clc.serverCommandSequence; i++ ) {
			MSG_WriteByte( msg, svc_serverCommand );
			MSG_WriteLong( msg, i );
			MSG_WriteString( msg, clc.serverCommands[ i & (MAX_RELIABLE_COMMANDS-1) ] );
		}
	}

	clc.demoCommandSequence = clc.serverCommandSequence;
}


/*
====================
CL_WriteGamestate
====================
*/
static void CL_WriteGamestate( qboolean initial )
{
	byte		bufData[ MAX_MSGLEN_BUF ];
	char		*s;
	msg_t		msg;
	int			i;
	int			len;
	entityState_t	*ent;
	entityState_t	nullstate;

	// write out the gamestate message
	MSG_Init( &msg, bufData, MAX_MSGLEN );
	MSG_Bitstream( &msg );

	// NOTE, MRE: all server->client messages now acknowledge
	MSG_WriteLong( &msg, clc.reliableSequence );

	if ( initial ) {
		clc.demoMessageSequence = 1;
		clc.demoCommandSequence = clc.serverCommandSequence;
	} else {
		CL_WriteServerCommands( &msg );
	}

	clc.demoDeltaNum = 0; // reset delta for next snapshot

	MSG_WriteByte( &msg, svc_gamestate );
	MSG_WriteLong( &msg, clc.serverCommandSequence );

	// configstrings
	for ( i = 0 ; i < MAX_CONFIGSTRINGS ; i++ ) {
		if ( !cl.gameState.stringOffsets[i] ) {
			continue;
		}
		s = cl.gameState.stringData + cl.gameState.stringOffsets[i];
		MSG_WriteByte( &msg, svc_configstring );
		MSG_WriteShort( &msg, i );
		MSG_WriteBigString( &msg, s );
	}

	// baselines
	Com_Memset( &nullstate, 0, sizeof( nullstate ) );
	for ( i = 0; i < MAX_GENTITIES ; i++ ) {
		if ( !cl.baselineUsed[ i ] )
			continue;
		ent = &cl.entityBaselines[ i ];
		MSG_WriteByte( &msg, svc_baseline );
		MSG_WriteDeltaEntity( &msg, &nullstate, ent, qtrue );
	}

	// finalize message
	MSG_WriteByte( &msg, svc_EOF );

	// finished writing the gamestate stuff

	// write the client num
	MSG_WriteLong( &msg, clc.clientNum );

	// write the checksum feed
	MSG_WriteLong( &msg, clc.checksumFeed );

	// finished writing the client packet
	MSG_WriteByte( &msg, svc_EOF );

	// write it to the demo file
	if ( clc.demoplaying )
		len = LittleLong( clc.demoMessageSequence - 1 );
	else
		len = LittleLong( clc.serverMessageSequence - 1 );

	FS_Write( &len, 4, clc.recordfile );

	len = LittleLong( msg.cursize );
	FS_Write( &len, 4, clc.recordfile );
	FS_Write( msg.data, msg.cursize, clc.recordfile );
}


/*
=============
CL_EmitPacketEntities
=============
*/
static void CL_EmitPacketEntities( clSnapshot_t *from, clSnapshot_t *to, msg_t *msg, entityState_t *oldents ) {
	entityState_t	*oldent, *newent;
	int		oldindex, newindex;
	int		oldnum, newnum;
	int		from_num_entities;

	// generate the delta update
	if ( !from ) {
		from_num_entities = 0;
	} else {
		from_num_entities = from->numEntities;
	}

	newent = NULL;
	oldent = NULL;
	newindex = 0;
	oldindex = 0;
	while ( newindex < to->numEntities || oldindex < from_num_entities ) {
		if ( newindex >= to->numEntities ) {
			newnum = MAX_GENTITIES+1;
		} else {
			newent = &cl.parseEntities[(to->parseEntitiesNum + newindex) % MAX_PARSE_ENTITIES];
			newnum = newent->number;
		}

		if ( oldindex >= from_num_entities ) {
			oldnum = MAX_GENTITIES+1;
		} else {
			//oldent = &cl.parseEntities[(from->parseEntitiesNum + oldindex) % MAX_PARSE_ENTITIES];
			oldent = &oldents[ oldindex ];
			oldnum = oldent->number;
		}

		if ( newnum == oldnum ) {
			// delta update from old position
			// because the force parm is qfalse, this will not result
			// in any bytes being emitted if the entity has not changed at all
			MSG_WriteDeltaEntity (msg, oldent, newent, qfalse );
			oldindex++;
			newindex++;
			continue;
		}

		if ( newnum < oldnum ) {
			// this is a new entity, send it from the baseline
			MSG_WriteDeltaEntity (msg, &cl.entityBaselines[newnum], newent, qtrue );
			newindex++;
			continue;
		}

		if ( newnum > oldnum ) {
			// the old entity isn't present in the new message
			MSG_WriteDeltaEntity (msg, oldent, NULL, qtrue );
			oldindex++;
			continue;
		}
	}

	MSG_WriteBits( msg, (MAX_GENTITIES-1), GENTITYNUM_BITS );	// end of packetentities
}


/*
====================
CL_WriteSnapshot
====================
*/
static void CL_WriteSnapshot( void ) {

	static	clSnapshot_t saved_snap;
	static entityState_t saved_ents[ MAX_SNAPSHOT_ENTITIES ];

	clSnapshot_t *snap, *oldSnap;
	byte	bufData[ MAX_MSGLEN_BUF ];
	msg_t	msg;
	int		i, len;

	snap = &cl.snapshots[ cl.snap.messageNum & PACKET_MASK ]; // current snapshot
	//if ( !snap->valid ) // should never happen?
	//	return;

	if ( clc.demoDeltaNum == 0 ) {
		oldSnap = NULL;
	} else {
		oldSnap = &saved_snap;
	}

	MSG_Init( &msg, bufData, MAX_MSGLEN );
	MSG_Bitstream( &msg );

	// NOTE, MRE: all server->client messages now acknowledge
	MSG_WriteLong( &msg, clc.reliableSequence );

	// Write all pending server commands
	CL_WriteServerCommands( &msg );

	MSG_WriteByte( &msg, svc_snapshot );
	MSG_WriteLong( &msg, snap->serverTime ); // sv.time
	MSG_WriteByte( &msg, clc.demoDeltaNum ); // 0 or 1
	MSG_WriteByte( &msg, snap->snapFlags );  // snapFlags
	MSG_WriteByte( &msg, snap->areabytes );  // areabytes
	MSG_WriteData( &msg, snap->areamask, snap->areabytes );
	if ( oldSnap )
		MSG_WriteDeltaPlayerstate( &msg, &oldSnap->ps, &snap->ps );
	else
		MSG_WriteDeltaPlayerstate( &msg, NULL, &snap->ps );

	CL_EmitPacketEntities( oldSnap, snap, &msg, saved_ents );

	// finished writing the client packet
	MSG_WriteByte( &msg, svc_EOF );

	// write it to the demo file
	if ( clc.demoplaying )
		len = LittleLong( clc.demoMessageSequence );
	else
		len = LittleLong( clc.serverMessageSequence );
	FS_Write( &len, 4, clc.recordfile );

	len = LittleLong( msg.cursize );
	FS_Write( &len, 4, clc.recordfile );
	FS_Write( msg.data, msg.cursize, clc.recordfile );

	// save last sent state so if there any need - we can skip any further incoming messages
	for ( i = 0; i < snap->numEntities; i++ )
		saved_ents[ i ] = cl.parseEntities[ (snap->parseEntitiesNum + i) % MAX_PARSE_ENTITIES ];

	saved_snap = *snap;
	saved_snap.parseEntitiesNum = 0;

	clc.demoMessageSequence++;
	clc.demoDeltaNum = 1;
}


/*
====================
CL_Record_f

record <demoname>

Begins recording a demo from the current position
====================
*/
static void CL_Record_f( void ) {
	char		demoName[MAX_OSPATH];
	char		name[MAX_OSPATH];
	qtime_t		t;

	if ( Cmd_Argc() > 2 ) {
		Com_Printf( "record <demoname>\n" );
		return;
	}

	if ( clc.demorecording ) {
		return;
	}

	if ( cls.state != CA_ACTIVE ) {
		Com_Printf( "You must be in a level to record.\n" );
		return;
	}

	if ( Cmd_Argc() == 2 ) {
		// explicit demo name specified
		Q_strncpyz( demoName, Cmd_Argv( 1 ), sizeof( demoName ) );
		Com_sprintf( name, sizeof( name ), "demos/%s", demoName );

		clc.explicitRecordName = qtrue;
	} else {
		Com_RealTime( &t );
		Com_sprintf( name, sizeof( name ), "demos/demo-%04d%02d%02d-%02d%02d%02d",
			1900 + t.tm_year, 1 + t.tm_mon,	t.tm_mday, t.tm_hour, t.tm_min, t.tm_sec );

		clc.explicitRecordName = qfalse;
	}

	// save desired filename without extension
	Q_strncpyz( clc.recordName, name, sizeof( clc.recordName ) );

	Com_Printf( "recording to %s.\n", name );

	// start new record with temporary extension
	Q_strcat( name, sizeof( name ), ".tmp" );

	// open the demo file
	clc.recordfile = FS_FOpenFileWrite( name );
	if ( clc.recordfile == FS_INVALID_HANDLE ) {
		Com_Printf( "ERROR: couldn't open.\n" );
		clc.recordName[0] = '\0';
		return;
	}

	clc.demorecording = qtrue;

	Com_TruncateLongString( clc.recordNameShort, clc.recordName );

	// don't start saving messages until a non-delta compressed message is received
	clc.demowaiting = qtrue;

	// write out the gamestate message
	CL_WriteGamestate( qtrue );

	// the rest of the demo file will be copied from net messages
}


/*
====================
CL_CompleteRecordName
====================
*/
static void CL_CompleteRecordName(const char *args, int argNum ) {
	if ( argNum == 2 ){
		Field_CompleteFilename( "demos", "demo", qtrue, FS_MATCH_EXTERN | FS_MATCH_STICK );
	}
}


/*
=======================================================================

CLIENT SIDE DEMO PLAYBACK

=======================================================================
*/

/*
=================
CL_DemoCompleted
=================
*/
static void CL_DemoCompleted( void ) {
	CL_Disconnect( qtrue );
}

/*
=================
CL_ReadDemoMessage
=================
*/
void CL_ReadDemoMessage( void ) {
	int			r;
	msg_t		buf;
	byte		bufData[ MAX_MSGLEN_BUF ];
	int			s;

	if ( clc.demofile == FS_INVALID_HANDLE ) {
		CL_DemoCompleted();
		return;
	}

	// get the sequence number
	r = FS_Read( &s, 4, clc.demofile );
	if ( r != 4 ) {
		CL_DemoCompleted();
		return;
	}
	clc.serverMessageSequence = LittleLong( s );

	// init the message
	MSG_Init( &buf, bufData, MAX_MSGLEN );

	// get the length
	r = FS_Read( &buf.cursize, 4, clc.demofile );
	if ( r != 4 ) {
		CL_DemoCompleted();
		return;
	}
	buf.cursize = LittleLong( buf.cursize );
	if ( buf.cursize == -1 ) {
		CL_DemoCompleted();
		return;
	}
	if ( buf.cursize > buf.maxsize ) {
		Com_Error (ERR_DROP, "CL_ReadDemoMessage: demoMsglen > MAX_MSGLEN");
	}
	r = FS_Read( buf.data, buf.cursize, clc.demofile );
	if ( r != buf.cursize ) {
		Com_Printf( "Demo file was truncated.\n");
		CL_DemoCompleted();
		return;
	}

	clc.lastPacketTime = cls.realtime;
	buf.readcount = 0;

	clc.demoCommandSequence = clc.serverCommandSequence;

	CL_ParseServerMessage( &buf );

	if ( clc.demorecording ) {
		// track changes and write new message
		if ( clc.eventMask & EM_GAMESTATE ) {
			CL_WriteGamestate( qfalse );
			// nothing should came after gamestate in current message
		} else if ( clc.eventMask & (EM_SNAPSHOT|EM_COMMAND) ) {
			CL_WriteSnapshot();
		}
	}
}

/*
====================
CL_CompleteDemoName
====================
*/
static void CL_CompleteDemoName(const char *args, int argNum ) {
	if ( argNum == 2 ) {
		Field_CompleteFilename( "demos", "demo", qfalse, FS_MATCH_ANY | FS_MATCH_STICK );
		FS_SetFilenameCallback( NULL );
	}
}

/*
====================
CL_PlayDemo_f

demo <demoname>

====================
*/
static void CL_PlayDemo_f( void ) {
	char		name[MAX_OSPATH];
	const char	*arg;
	const char	*shortname, *slash;
	fileHandle_t hFile;

	if ( Cmd_Argc() != 2 ) {
		Com_Printf( "demo <demoname>\n" );
		return;
	}

	// open the demo file
	arg = Cmd_Argv( 1 );

	Com_sprintf(name, sizeof(name), "demos/%s", arg);
	FS_FOpenFileRead( name, &hFile, qtrue );

	if ( hFile == FS_INVALID_HANDLE ) {
		Com_Printf( S_COLOR_YELLOW "couldn't open %s\n", name );
		return;
	}

	FS_FCloseFile( hFile );
	hFile = FS_INVALID_HANDLE;

	// make sure a local server is killed
	// 2 means don't force disconnect of local client
	Cvar_Set( "sv_killserver", "2" );

	CL_Disconnect( qtrue );

	// clc.demofile will be closed during CL_Disconnect so reopen it
	if ( FS_FOpenFileRead( name, &clc.demofile, qtrue ) == -1 ) {
		// drop this time
		Com_Error( ERR_DROP, "couldn't open %s\n", name );
		return;
	}

	if ( (slash = strrchr( name, '/' )) != NULL )
		shortname = slash + 1;
	else
		shortname = name;

	Q_strncpyz( clc.demoName, shortname, sizeof( clc.demoName ) );

	Con_Close();

	cls.state = CA_CONNECTED;
	clc.demoplaying = qtrue;
	Q_strncpyz( cls.servername, shortname, sizeof( cls.servername ) );

	// read demo messages until connected
	while ( cls.state >= CA_CONNECTED && cls.state < CA_PRIMED ) {
		CL_ReadDemoMessage();
	}

	// don't get the first snapshot this frame, to prevent the long
	// time from the gamestate load from messing causing a time skip
	clc.firstDemoFrameSkipped = qfalse;
}

/*
=====================
CL_ShutdownVMs
=====================
*/
static void CL_ShutdownVMs( void )
{
	CL_ShutdownCGame();
	CL_ShutdownUI();
}

/*
=====================
Called by Com_GameRestart, CL_FlushMemory and SV_SpawnServer

CL_ShutdownAll
=====================
*/
void CL_ShutdownAll( void ) {

	// clear and mute all sounds until next registration
	S_DisableSounds();

	// shutdown VMs
	CL_ShutdownVMs();

	// shutdown the renderer
	if ( re.Shutdown ) {
		if ( CL_GameSwitch() ) {
			CL_ShutdownRef( REF_DESTROY_WINDOW ); // shutdown renderer & GLimp
		} else {
			re.Shutdown( REF_KEEP_CONTEXT ); // don't destroy window or context
		}
	}

	cls.rendererStarted = qfalse;
	cls.soundRegistered = qfalse;

	SCR_Done();
}

/*
=================
CL_ClearMemory
=================
*/
void CL_ClearMemory( void ) {
	// if not running a server clear the whole hunk
	if ( !com_sv_running->integer ) {
		// clear the whole hunk
		Hunk_Clear();
		// clear collision map data
		CM_ClearMap();
	} else {
		// clear all the client data on the hunk
		Hunk_ClearToMark();
	}
}

/*
=================
CL_FlushMemory

Called by CL_Disconnect_f, CL_DownloadsComplete
Also called by Com_Error
=================
*/
void CL_FlushMemory( void ) {

	// shutdown all the client stuff
	CL_ShutdownAll();

	CL_ClearMemory();

	CL_StartHunkUsers();
}

/*
=====================
CL_MapLoading

A local server is starting to load a map, so update the
screen to let the user know about it, then dump all client
memory on the hunk from cgame, ui, and renderer
=====================
*/
void CL_MapLoading( void ) {
	if ( com_dedicated->integer ) {
		cls.state = CA_DISCONNECTED;
		Key_SetCatcher( KEYCATCH_CONSOLE );
		return;
	}

	if ( !com_cl_running->integer ) {
		return;
	}

	Con_Close();
	Key_SetCatcher( 0 );

	// if we are already connected to the local host, stay connected
	if ( cls.state >= CA_CONNECTED && !Q_stricmp( cls.servername, "localhost" ) ) {
		cls.state = CA_CONNECTED;		// so the connect screen is drawn
		Com_Memset( cls.updateInfoString, 0, sizeof( cls.updateInfoString ) );
		Com_Memset( clc.serverMessage, 0, sizeof( clc.serverMessage ) );
		Com_Memset( &cl.gameState, 0, sizeof( cl.gameState ) );
		clc.lastPacketSentTime = cls.realtime - 9999;  // send packet immediately
		cls.framecount++;
		SCR_UpdateScreen();
	} else {
		// clear nextmap so the cinematic shutdown doesn't execute it
		Cvar_Set( "nextmap", "" );
		CL_Disconnect( qtrue );
		Q_strncpyz( cls.servername, "localhost", sizeof(cls.servername) );
		cls.state = CA_CHALLENGING;		// so the connect screen is drawn
		Key_SetCatcher( 0 );
		cls.framecount++;
		SCR_UpdateScreen();
		clc.connectTime = -RETRANSMIT_TIMEOUT;
		NET_StringToAdr( cls.servername, &clc.serverAddress, NA_UNSPEC );
		// we don't need a challenge on the localhost
		CL_CheckForResend();
	}
}

/*
=====================
CL_ClearState

Called before parsing a gamestate
=====================
*/
void CL_ClearState( void ) {

//	S_StopAllSounds();

	Com_Memset( &cl, 0, sizeof( cl ) );
}

/*
====================
CL_UpdateGUID

update cl_guid using QKEY_FILE and optional prefix
====================
*/
static void CL_UpdateGUID( const char *prefix, int prefix_len )
{
	fileHandle_t f;
	int len;

	len = FS_SV_FOpenFileRead( QKEY_FILE, &f );
	FS_FCloseFile( f );

	if( len != QKEY_SIZE )
		Cvar_Set( "cl_guid", "" );
	else
		Cvar_Set( "cl_guid", Com_MD5File( QKEY_FILE, QKEY_SIZE,
			prefix, prefix_len ) );
}

/*
=====================
CL_Disconnect

Called when a connection, demo, or cinematic is being terminated.
Goes from a connected state to either a menu state or a console state
Sends a disconnect message to the server
This is also called on Com_Error and Com_Quit, so it shouldn't cause any errors
=====================
*/
qboolean CL_Disconnect( qboolean showMainMenu ) {
	static qboolean cl_disconnecting = qfalse;

	if ( !com_cl_running || !com_cl_running->integer ) {
		return qfalse;
	}

	if ( cl_disconnecting ) {
		return qfalse;
	}

	cl_disconnecting = qtrue;

	// Stop demo recording
	if ( clc.demorecording ) {
		CL_StopRecord_f();
	}

	// Stop demo playback
	if ( clc.demofile != FS_INVALID_HANDLE ) {
		FS_FCloseFile( clc.demofile );
		clc.demofile = FS_INVALID_HANDLE;
	}

	// Finish downloads
	if ( clc.download != FS_INVALID_HANDLE ) {
		FS_FCloseFile( clc.download );
		clc.download = FS_INVALID_HANDLE;
	}
	*clc.downloadTempName = *clc.downloadName = '\0';
	Cvar_Set( "cl_downloadName", "" );

	// Stop recording any video
	if ( CL_VideoRecording() ) {
		// Finish rendering current frame
		cls.framecount++;
		SCR_UpdateScreen();
		CL_CloseAVI( qfalse );
	}

	if ( cgvm ) {
		// do that right after we rendered last video frame
		CL_ShutdownCGame();
	}

	SCR_StopCinematic();
	//S_StopAllSounds();
	Key_ClearStates();

	if ( uivm && showMainMenu ) {
		VM_Call( uivm, 1, UI_SET_ACTIVE_MENU, UIMENU_NONE );
	}

	FS_ClearPakReferences( FS_GENERAL_REF | FS_UI_REF | FS_CGAME_REF );

	if ( CL_GameSwitch() ) {
		// keep current gamestate and connection
		cl_disconnecting = qfalse;
		return qfalse;
	}

	// send a disconnect message to the server
	// send it a few times in case one is dropped
	if ( cls.state >= CA_CONNECTED && cls.state != CA_CINEMATIC && !clc.demoplaying ) {
		CL_AddReliableCommand( "disconnect", qtrue );
		CL_WritePacket( 2 );
	}

	CL_ClearState();

	// wipe the client connection
	Com_Memset( &clc, 0, sizeof( clc ) );

	cls.state = CA_DISCONNECTED;

	CL_UpdateGUID( NULL, 0 );

	Cmd_RemoveCgameCommands();

	cl_disconnecting = qfalse;

	return qfalse;
}

/*
===================
CL_ForwardCommandToServer

adds the current command line as a clientCommand
things like godmode, noclip, etc, are commands directed to the server,
so when they are typed in at the console, they will need to be forwarded.
===================
*/
void CL_ForwardCommandToServer( const char *string ) {
	const char *cmd;

	cmd = Cmd_Argv( 0 );

	// ignore key up commands
	if ( cmd[0] == '-' ) {
		return;
	}

	// no userinfo updates from command line
	if ( !strcmp( cmd, "userinfo" ) ) {
		return;
	}

	if ( clc.demoplaying || cls.state < CA_CONNECTED || cmd[0] == '+' ) {
		Com_Printf( "Unknown command \"%s" S_COLOR_WHITE "\"\n", cmd );
		return;
	}

	if ( Cmd_Argc() > 1 ) {
		CL_AddReliableCommand( string, qfalse );
	} else {
		CL_AddReliableCommand( cmd, qfalse );
	}
}

/*
======================================================================

CONSOLE COMMANDS

======================================================================
*/

/*
==================
CL_ForwardToServer_f
==================
*/
static void CL_ForwardToServer_f( void ) {
	if ( cls.state != CA_ACTIVE || clc.demoplaying ) {
		Com_Printf ("Not connected to a server.\n");
		return;
	}

	if ( Cmd_Argc() <= 1 || strcmp( Cmd_Argv( 1 ), "userinfo" ) == 0 )
		return;

	// don't forward the first argument
	CL_AddReliableCommand( Cmd_ArgsFrom( 1 ), qfalse );
}


/*
==================
CL_Disconnect_f
==================
*/
void CL_Disconnect_f( void ) {
	SCR_StopCinematic();
	Cvar_Set( "cl_changeqvm", "0" );
	if ( cls.state != CA_DISCONNECTED && cls.state != CA_CINEMATIC ) {
		if ( (uivm && uivm->callLevel) || (cgvm && cgvm->callLevel) ) {
			Com_Error( ERR_DISCONNECT, "Disconnected from server" );
		} else {
			// clear any previous "server full" type messages
			clc.serverMessage[0] = '\0';
			if ( com_sv_running && com_sv_running->integer ) {
				// if running a local server, kill it
				SV_Shutdown( "Disconnected from server" );
			} else {
				Com_Printf( "Disconnected from %s\n", cls.servername );
			}
			Cvar_Set( "com_errorMessage", "" );
			    
			CL_Disconnect( qfalse );
		    CL_FlushMemory();
		    
			if ( uivm ) {
				VM_Call( uivm, 1, UI_SET_ACTIVE_MENU, UIMENU_MAIN );
			}
		}
	}
}

/*
================
CL_Connect_f
================
*/
static void CL_Connect_f( void ) {
	netadrtype_t family;
	netadr_t	addr;
	char	buffer[ sizeof( cls.servername ) ];  // same length as cls.servername
	char	args[ sizeof( cls.servername ) + MAX_CVAR_VALUE_STRING ];
	const char	*server;
	const char	*serverString;
	int		len;
	int		argc;

	argc = Cmd_Argc();
	family = NA_UNSPEC;

	if ( argc != 2 && argc != 3 ) {
		Com_Printf( "usage: connect [-4|-6] <server>\n");
		return;
	}

	if ( argc == 2 ) {
		server = Cmd_Argv(1);
	} else {
		if( !strcmp( Cmd_Argv(1), "-4" ) )
			family = NA_IP;
#ifdef USE_IPV6
		else if( !strcmp( Cmd_Argv(1), "-6" ) )
			family = NA_IP6;
		else
			Com_Printf( S_COLOR_YELLOW "warning: only -4 or -6 as address type understood.\n" );
#else
			Com_Printf( S_COLOR_YELLOW "warning: only -4 as address type understood.\n" );
#endif
		server = Cmd_Argv(2);
	}

	Q_strncpyz( buffer, server, sizeof( buffer ) );
	server = buffer;

	// skip leading "q3a:/" in connection string
	if ( !Q_stricmpn( server, "q3a:/", 5 ) ) {
		server += 5;
	}

	// skip all slash prefixes
	while ( *server == '/' ) {
		server++;
	}

	len = strlen( server );
	if ( len <= 0 ) {
		return;
	}

	// some programs may add ending slash
	if ( buffer[len-1] == '/' ) {
		buffer[len-1] = '\0';
	}

	if ( !*server ) {
		return;
	}

	// try resolve remote server first
	if ( !NET_StringToAdr( server, &addr, family ) ) {
		Com_Printf( S_COLOR_YELLOW "Bad server address - %s\n", server );
		return;
	}

	// save arguments for reconnect
	Q_strncpyz( args, Cmd_ArgsFrom( 1 ), sizeof( args ) );

	// clear any previous "server full" type messages
	clc.serverMessage[0] = '\0';

	// if running a local server, kill it
	if ( com_sv_running->integer && !strcmp( server, "localhost" ) ) {
		SV_Shutdown( "Server quit" );
	}

	// make sure a local server is killed
	Cvar_Set( "sv_killserver", "1" );
	SV_Frame( 0 );

	CL_Disconnect( qtrue );
	Con_Close();

	Q_strncpyz( cls.servername, server, sizeof( cls.servername ) );

	// copy resolved address
	clc.serverAddress = addr;

	if (clc.serverAddress.port == 0) {
		clc.serverAddress.port = BigShort( PORT_SERVER );
	}

	serverString = NET_AdrToStringwPort( &clc.serverAddress );

	Com_Printf( "%s resolved to %s\n", cls.servername, serverString );

	CL_UpdateGUID( NULL, 0 );

	// if we aren't playing on a lan, we need to authenticate
	// with the cd key
	if ( NET_IsLocalAddress( &clc.serverAddress ) ) {
		cls.state = CA_CHALLENGING;
	} else {
		cls.state = CA_CONNECTING;

		// Set a client challenge number that ideally is mirrored back by the server.
		//clc.challenge = ((rand() << 16) ^ rand()) ^ Com_Milliseconds();
		Com_RandomBytes( (byte*)&clc.challenge, sizeof( clc.challenge ) );
	}

	Key_SetCatcher( 0 );
	clc.connectTime = -99999;	// CL_CheckForResend() will fire immediately
	clc.connectPacketCount = 0;

	Cvar_Set( "cl_reconnectArgs", args );

	// server connection string
	Cvar_Set( "cl_currentServerAddress", server );
}

#define MAX_RCON_MESSAGE (MAX_STRING_CHARS+4)

/*
==================
CL_CompleteRcon
==================
*/
static void CL_CompleteRcon(const char *args, int argNum )
{
	if ( argNum >= 2 )
	{
		// Skip "rcon "
		const char *p = Com_SkipTokens( args, 1, " " );

		if ( p > args )
			Field_CompleteCommand( p, qtrue, qtrue );
	}
}


/*
=====================
CL_Rcon_f

Send the rest of the command line over as
an unconnected command.
=====================
*/
static void CL_Rcon_f( void ) {
	char message[MAX_RCON_MESSAGE];
	const char *sp;
	int len;

	if ( !rcon_client_password->string[0] ) {
		Com_Printf( "You must set 'rconpassword' before\n"
			"issuing an rcon command.\n" );
		return;
	}

	if ( cls.state >= CA_CONNECTED ) {
		rcon_address = clc.netchan.remoteAddress;
	} else {
		if ( !rconAddress->string[0] ) {
			Com_Printf( "You must either be connected,\n"
				"or set the 'rconAddress' cvar\n"
				"to issue rcon commands\n" );
			return;
		}
		if ( !NET_StringToAdr( rconAddress->string, &rcon_address, NA_UNSPEC ) ) {
			return;
		}
		if ( rcon_address.port == 0 ) {
			rcon_address.port = BigShort( PORT_SERVER );
		}
	}

	message[0] = -1;
	message[1] = -1;
	message[2] = -1;
	message[3] = -1;
	message[4] = '\0';

	// we may need to quote password if it contains spaces
	sp = strchr( rcon_client_password->string, ' ' );

	len = Com_sprintf( message+4, sizeof( message )-4,
		sp ? "rcon \"%s\" %s" : "rcon %s %s",
		rcon_client_password->string,
		Cmd_Cmd() + 5 ) + 4 + 1; // including OOB marker and '\0'

	NET_SendPacket( NS_CLIENT, len, message, &rcon_address );
}

/*
=================
CL_Vid_Restart

Restart the video subsystem

we also have to reload the UI and CGame because the renderer
doesn't know what graphics to reload
=================
*/
static void CL_Vid_Restart( refShutdownCode_t shutdownCode ) {

	// Settings may have changed so stop recording now
	if ( CL_VideoRecording() )
		CL_CloseAVI( qfalse );

	if ( clc.demorecording )
		CL_StopRecord_f();

	// clear and mute all sounds until next registration
	S_DisableSounds();

	// shutdown VMs
	CL_ShutdownVMs();

	// shutdown the renderer and clear the renderer interface
	CL_ShutdownRef( shutdownCode ); // REF_KEEP_CONTEXT, REF_KEEP_WINDOW, REF_DESTROY_WINDOW

	// clear pak references
	FS_ClearPakReferences( FS_UI_REF | FS_CGAME_REF );

	// reinitialize the filesystem if the game directory or checksum has changed
	if ( !clc.demoplaying ) // -EC-
		FS_ConditionalRestart( clc.checksumFeed, qfalse );

	cls.soundRegistered = qfalse;

	// unpause so the cgame definitely gets a snapshot and renders a frame
	Cvar_Set( "cl_paused", "0" );

	CL_ClearMemory();

	// startup all the client stuff
	CL_StartHunkUsers();

	// start the cgame if connected
	if ( ( cls.state > CA_CONNECTED && cls.state != CA_CINEMATIC ) || cls.startCgame ) {
		cls.cgameStarted = qtrue;
		CL_InitCGame();
	}

	cls.startCgame = qfalse;
}


/*
=================
CL_Vid_Restart_f

Wrapper for CL_Vid_Restart
=================
*/
static void CL_Vid_Restart_f( void ) {

	if ( Q_stricmp( Cmd_Argv( 1 ), "keep_window" ) == 0 || Q_stricmp( Cmd_Argv( 1 ), "fast" ) == 0 ) {
		// fast path: keep window
		CL_Vid_Restart( REF_KEEP_WINDOW );
	} else {
		if ( cls.lastVidRestart ) {
			if ( abs( cls.lastVidRestart - Sys_Milliseconds() ) < 500 ) {
				// hack for OSP mod: do not allow vid restart right after cgame init
				return;
			}
		}
		CL_Vid_Restart( REF_DESTROY_WINDOW );
	}
}


/*
=================
CL_Snd_Restart_f

Restart the sound subsystem
The cgame and game must also be forced to restart because
handles will be invalid
=================
*/
static void CL_Snd_Restart_f( void )
{
	S_Shutdown();

	// sound will be reinitialized by vid_restart
	CL_Vid_Restart( REF_KEEP_CONTEXT /*REF_KEEP_WINDOW*/ );
}

/*
==================
CL_PK3List_f
==================
*/
void CL_OpenedPK3List_f( void ) {
	Com_Printf("Opened PK3 Names: %s\n", FS_LoadedPakNames());
}

/*
==================
CL_Configstrings_f
==================
*/
static void CL_Configstrings_f( void ) {
	int		i;
	int		ofs;

	if ( cls.state != CA_ACTIVE ) {
		Com_Printf( "Not connected to a server.\n");
		return;
	}

	for ( i = 0 ; i < MAX_CONFIGSTRINGS ; i++ ) {
		ofs = cl.gameState.stringOffsets[ i ];
		if ( !ofs ) {
			continue;
		}
		Com_Printf( "%4i: %s\n", i, cl.gameState.stringData + ofs );
	}
}

/*
==============
CL_Clientinfo_f
==============
*/
static void CL_Clientinfo_f( void ) {
	Com_Printf( "--------- Client Information ---------\n" );
	Com_Printf( "state: %i\n", cls.state );
	Com_Printf( "Server: %s\n", cls.servername );
	Com_Printf ("User info settings:\n");
	Info_Print( Cvar_InfoString( CVAR_USERINFO, NULL ) );
	Com_Printf( "--------------------------------------\n" );
}


/*
==============
CL_Serverinfo_f
==============
*/
static void CL_Serverinfo_f( void ) {
	int		ofs;

	ofs = cl.gameState.stringOffsets[ CS_SERVERINFO ];
	if ( !ofs )
		return;

	Com_Printf( "Server info settings:\n" );
	Info_Print( cl.gameState.stringData + ofs );
}


/*
===========
CL_Systeminfo_f
===========
*/
static void CL_Systeminfo_f( void ) {
	int ofs;

	ofs = cl.gameState.stringOffsets[ CS_SYSTEMINFO ];
	if ( !ofs )
		return;

	Com_Printf( "System info settings:\n" );
	Info_Print( cl.gameState.stringData + ofs );
}

//====================================================================

/*
=================
CL_DownloadsComplete

Called when all downloading has been completed
=================
*/
static void CL_DownloadsComplete( void ) {

	// if we downloaded files we need to restart the file system
	if ( clc.downloadRestart  ) {
		clc.downloadRestart = qfalse;

		FS_Restart(clc.checksumFeed); // We possibly downloaded a pak, restart the file system to load it

		// inform the server so we get new gamestate info
		CL_AddReliableCommand( "donedl", qfalse );

		// by sending the donedl command we request a new gamestate
		// so we don't want to load stuff yet
		return;
	}

	// let the client game init and load data
	cls.state = CA_LOADING;

	// Pump the loop, this may change gamestate!
	Com_EventLoop();

	// if the gamestate was changed by calling Com_EventLoop
	// then we loaded everything already and we don't want to do it again.
	if ( cls.state != CA_LOADING ) {
		return;
	}

	// flush client memory and start loading stuff
	// this will also (re)load the UI
	// if this is a local client then only the client part of the hunk
	// will be cleared, note that this is done after the hunk mark has been set
	//if ( !com_sv_running->integer )
	CL_FlushMemory();

	// initialize the CGame
	cls.cgameStarted = qtrue;
	CL_InitCGame();

	CL_WritePacket( 2 );
}


/*
=================
CL_BeginDownload

Requests a file to download from the server.  Stores it in the current
game directory.
=================
*/
static void CL_BeginDownload( const char *localName, const char *remoteName ) {

	Com_DPrintf("***** CL_BeginDownload *****\n"
				"Localname: %s\n"
				"Remotename: %s\n"
				"****************************\n", localName, remoteName);

	Q_strncpyz ( clc.downloadName, localName, sizeof(clc.downloadName) );
	Com_sprintf( clc.downloadTempName, sizeof(clc.downloadTempName), "%s.tmp", localName );

	// Set so UI gets access to it
	Cvar_Set( "cl_downloadName", remoteName );
	Cvar_Set( "cl_downloadSize", "0" );
	Cvar_Set( "cl_downloadCount", "0" );
	Cvar_SetIntegerValue( "cl_downloadTime", cls.realtime );

	clc.downloadBlock = 0; // Starting new file
	clc.downloadCount = 0;

	CL_AddReliableCommand( va("download %s", remoteName), qfalse );
}


/*
=================
CL_NextDownload

A download completed or failed
=================
*/
void CL_NextDownload( void )
{
	char *s;
	char *remoteName, *localName;

	// A download has finished, check whether this matches a referenced checksum
	if(*clc.downloadName)
	{
		const char *zippath = FS_BuildOSPath(Cvar_VariableString("fs_homepath"), clc.downloadName, NULL );

		if(!FS_CompareZipChecksum(zippath))
			Com_Error(ERR_DROP, "Incorrect checksum for file: %s", clc.downloadName);
	}

	*clc.downloadTempName = *clc.downloadName = '\0';
	Cvar_Set("cl_downloadName", "");

	// We are looking to start a download here
	if (*clc.downloadList) {
		s = clc.downloadList;

		// format is:
		//  @remotename@localname@remotename@localname, etc.

		if (*s == '@')
			s++;
		remoteName = s;

		if ( (s = strchr(s, '@')) == NULL ) {
			CL_DownloadsComplete();
			return;
		}

		*s++ = '\0';
		localName = s;
		if ( (s = strchr(s, '@')) != NULL )
			*s++ = '\0';
		else
			s = localName + strlen(localName); // point at the null byte

		if( (cl_allowDownload->integer & DLF_NO_UDP) ) {
			Com_Error(ERR_DROP, "UDP Downloads are disabled on your client. (cl_allowDownload is %d)", cl_allowDownload->integer);
			return;
		} else {
			CL_BeginDownload( localName, remoteName );
		}
		clc.downloadRestart = qtrue;

		// move over the rest
		memmove( clc.downloadList, s, strlen(s) + 1 );

		return;
	}

	CL_DownloadsComplete();
}


/*
=================
CL_InitDownloads

After receiving a valid game state, we valid the cgame and local zip files here
and determine if we need to download them
=================
*/
void CL_InitDownloads( void ) {

	if ( !(cl_allowDownload->integer & DLF_ENABLE) )
	{
		char missingfiles[ MAXPRINTMSG ];

		// autodownload is disabled on the client
		// but it's possible that some referenced files on the server are missing
		if ( FS_ComparePaks( missingfiles, sizeof( missingfiles ), qfalse ) )
		{
			// NOTE TTimo I would rather have that printed as a modal message box
			// but at this point while joining the game we don't know whether we will successfully join or not
			Com_Printf( "\nWARNING: You are missing some files referenced by the server:\n%s"
				"You might not be able to join the game\n"
				"Go to the setting menu to turn on autodownload, or get the file elsewhere\n\n", missingfiles );
		}
	}
	else if ( FS_ComparePaks( clc.downloadList, sizeof( clc.downloadList ) , qtrue ) ) {

		Com_Printf( "Need paks: %s\n", clc.downloadList );

		if ( *clc.downloadList ) {
			// if autodownloading is not enabled on the server
			cls.state = CA_CONNECTED;

			*clc.downloadTempName = *clc.downloadName = '\0';
			Cvar_Set( "cl_downloadName", "" );

			CL_NextDownload();
			return;
		}

	}

	CL_DownloadsComplete();
}


/*
=================
CL_CheckForResend

Resend a connect message if the last one has timed out
=================
*/
static void CL_CheckForResend( void ) {
	int		port, len;
	char	info[MAX_INFO_STRING*2]; // larger buffer to detect overflows
	char	data[MAX_INFO_STRING];
	qboolean	notOverflowed;
	qboolean	infoTruncated;

	// don't send anything if playing back a demo
	if ( clc.demoplaying ) {
		return;
	}

	// resend if we haven't gotten a reply yet
	if ( cls.state != CA_CONNECTING && cls.state != CA_CHALLENGING ) {
		return;
	}

	if ( cls.realtime - clc.connectTime < RETRANSMIT_TIMEOUT ) {
		return;
	}

	clc.connectTime = cls.realtime;	// for retransmit requests
	clc.connectPacketCount++;

	switch ( cls.state ) {
	case CA_CONNECTING:
		// requesting a challenge .. IPv6 users always get in as authorize server supports no ipv6.
		// The challenge request shall be followed by a client challenge so no malicious server can hijack this connection.
		NET_OutOfBandPrint( NS_CLIENT, &clc.serverAddress, "getchallenge %d %s", clc.challenge, GAMENAME_FOR_MASTER );
		break;

	case CA_CHALLENGING:
		// sending back the challenge
		port = Cvar_VariableIntegerValue( "net_qport" );

		infoTruncated = qfalse;
		Q_strncpyz( info, Cvar_InfoString( CVAR_USERINFO, &infoTruncated ), sizeof( info ) );

		len = strlen( info );
		if ( len > MAX_USERINFO_LENGTH ) {
			notOverflowed = qfalse;
		} else {
			notOverflowed = qtrue;
		}

		notOverflowed &= Info_SetValueForKey_s( info, MAX_USERINFO_LENGTH, "qport",
			va( "%i", port ) );

		notOverflowed &= Info_SetValueForKey_s( info, MAX_USERINFO_LENGTH, "challenge",
			va( "%i", clc.challenge ) );

		// for now - this will be used to inform server about q3msgboom fix
		// this is optional key so will not trigger oversize warning
		Info_SetValueForKey_s( info, MAX_USERINFO_LENGTH, "client", ENGINE_VERSION );

		if ( !notOverflowed ) {
			Com_Printf( S_COLOR_YELLOW "WARNING: oversize userinfo, you might be not able to join remote server!\n" );
		}

		len = Com_sprintf( data, sizeof( data ), "connect \"%s\"", info );
		// NOTE TTimo don't forget to set the right data length!
		NET_OutOfBandCompress( NS_CLIENT, &clc.serverAddress, (byte *) &data[0], len );
		// the most current userinfo has been sent, so watch for any
		// newer changes to userinfo variables
		cvar_modifiedFlags &= ~CVAR_USERINFO;

		// ... but force re-send if userinfo was truncated in any way
		if ( infoTruncated || !notOverflowed ) {
			cvar_modifiedFlags |= CVAR_USERINFO;
		}
		break;

	default:
		Com_Error( ERR_FATAL, "CL_CheckForResend: bad cls.state" );
	}
}

/*
===================
CL_InitServerInfo
===================
*/
static void CL_InitServerInfo( serverInfo_t *server, const netadr_t *address ) {
	server->adr = *address;
	server->clients = 0;
	server->hostName[0] = '\0';
	server->mapName[0] = '\0';
	server->maxClients = 0;
	server->maxPing = 0;
	server->minPing = 0;
	server->ping = -1;
	server->game[0] = '\0';
	server->gameType = 0;
	server->netType = 0;
	server->g_humanplayers = 0;
	server->g_needpass = 0;
}

#define MAX_SERVERSPERPACKET	256

typedef struct hash_chain_s {
	netadr_t             addr;
	struct hash_chain_s *next;
} hash_chain_t;

static hash_chain_t *hash_table[1024];
static hash_chain_t hash_list[MAX_GLOBAL_SERVERS];
static unsigned int hash_count = 0;

static unsigned int hash_func( const netadr_t *addr ) {

	const byte		*ip = NULL;
	unsigned int	size;
	unsigned int	i;
	unsigned int	hash = 0;

	switch ( addr->type ) {
		case NA_IP:  ip = addr->ipv._4; size = 4;  break;
#ifdef USE_IPV6
		case NA_IP6: ip = addr->ipv._6; size = 16; break;
#endif
		default: size = 0; break;
	}

	for ( i = 0; i < size; i++ )
		hash = hash * 101 + (int)( *ip++ );

	hash = hash ^ ( hash >> 16 );

	return (hash & 1023);
}

static void hash_insert( const netadr_t *addr )
{
	hash_chain_t **tab, *cur;
	unsigned int hash;
	if ( hash_count >= MAX_GLOBAL_SERVERS )
		return;
	hash = hash_func( addr );
	tab = &hash_table[ hash ];
	cur = &hash_list[ hash_count++ ];
	cur->addr = *addr;
	if ( cur != *tab )
		cur->next = *tab;
	else
		cur->next = NULL;
	*tab = cur;
}

static void hash_reset( void )
{
	hash_count = 0;
	memset( hash_list, 0, sizeof( hash_list ) );
	memset( hash_table, 0, sizeof( hash_table ) );
}

static hash_chain_t *hash_find( const netadr_t *addr )
{
	hash_chain_t *cur;
	cur = hash_table[ hash_func( addr ) ];
	while ( cur != NULL ) {
		if ( NET_CompareAdr( addr, &cur->addr ) )
			return cur;
		cur = cur->next;
	}
	return NULL;
}


/*
===================
CL_ServersResponsePacket
===================
*/
static void CL_ServersResponsePacket( const netadr_t* from, msg_t *msg, qboolean extended ) {
	int				i, count, total;
	netadr_t addresses[MAX_SERVERSPERPACKET];
	int				numservers;
	byte*			buffptr;
	byte*			buffend;
	serverInfo_t	*server;

	//Com_Printf("CL_ServersResponsePacket\n"); // moved down

	if (cls.numglobalservers == -1) {
		// state to detect lack of servers or lack of response
		cls.numglobalservers = 0;
		cls.numGlobalServerAddresses = 0;
		hash_reset();
	}

	// parse through server response string
	numservers = 0;
	buffptr    = msg->data;
	buffend    = buffptr + msg->cursize;

	// advance to initial token
	do
	{
		if(*buffptr == '\\' || (extended && *buffptr == '/'))
			break;

		buffptr++;
	} while (buffptr < buffend);

	while (buffptr + 1 < buffend)
	{
		// IPv4 address
		if (*buffptr == '\\')
		{
			buffptr++;

			if (buffend - buffptr < sizeof(addresses[numservers].ipv._4) + sizeof(addresses[numservers].port) + 1)
				break;

			for(i = 0; i < sizeof(addresses[numservers].ipv._4); i++)
				addresses[numservers].ipv._4[i] = *buffptr++;

			addresses[numservers].type = NA_IP;
		}
#ifdef USE_IPV6
		// IPv6 address, if it's an extended response
		else if (extended && *buffptr == '/')
		{
			buffptr++;

			if (buffend - buffptr < sizeof(addresses[numservers].ipv._6) + sizeof(addresses[numservers].port) + 1)
				break;

			for(i = 0; i < sizeof(addresses[numservers].ipv._6); i++)
				addresses[numservers].ipv._6[i] = *buffptr++;

			addresses[numservers].type = NA_IP6;
			addresses[numservers].scope_id = from->scope_id;
		}
#endif
		else
			// syntax error!
			break;

		// parse out port
		addresses[numservers].port = (*buffptr++) << 8;
		addresses[numservers].port += *buffptr++;
		addresses[numservers].port = BigShort( addresses[numservers].port );

		// syntax check
		if (*buffptr != '\\' && *buffptr != '/')
			break;

		numservers++;
		if (numservers >= MAX_SERVERSPERPACKET)
			break;
	}

	count = cls.numglobalservers;

	for (i = 0; i < numservers && count < MAX_GLOBAL_SERVERS; i++) {

		// Tequila: It's possible to have sent many master server requests. Then
		// we may receive many times the same addresses from the master server.
		// We just avoid to add a server if it is still in the global servers list.
		if ( hash_find( &addresses[i] ) )
			continue;

		hash_insert( &addresses[i] );

		// build net address
		server = &cls.globalServers[count];

		CL_InitServerInfo( server, &addresses[i] );
		// advance to next slot
		count++;
	}

	// if getting the global list
	if ( count >= MAX_GLOBAL_SERVERS && cls.numGlobalServerAddresses < MAX_GLOBAL_SERVERS )
	{
		// if we couldn't store the servers in the main list anymore
		for (; i < numservers && cls.numGlobalServerAddresses < MAX_GLOBAL_SERVERS; i++)
		{
			// just store the addresses in an additional list
			cls.globalServerAddresses[cls.numGlobalServerAddresses++] = addresses[i];
		}
	}

	cls.numglobalservers = count;
	total = count + cls.numGlobalServerAddresses;

	Com_Printf( "getserversResponse:%3d servers parsed (total %d)\n", numservers, total);
}


/*
=================
CL_ConnectionlessPacket

Responses to broadcasts, etc

return true only for commands indicating that our server is alive
or connection sequence is going into the right way
=================
*/
static qboolean CL_ConnectionlessPacket( const netadr_t *from, msg_t *msg ) {
	qboolean fromserver;
	const char *s;
	const char *c;
	int challenge = 0;

	MSG_BeginReadingOOB( msg );
	MSG_ReadLong( msg );	// skip the -1

	s = MSG_ReadStringLine( msg );

	Cmd_TokenizeString( s );

	c = Cmd_Argv(0);

	if ( com_developer->integer ) {
		Com_Printf( "CL packet %s: %s\n", NET_AdrToStringwPort( from ), s );
	}

	// challenge from the server we are connecting to
	if ( !Q_stricmp(c, "challengeResponse" ) ) {

		if ( cls.state != CA_CONNECTING ) {
			Com_DPrintf( "Unwanted challenge response received. Ignored.\n" );
			return qfalse;
		}

		c = Cmd_Argv( 2 );
		if ( *c != '\0' )
			challenge = atoi( c );

		if ( *c == '\0' || challenge != clc.challenge )
		{
			Com_Printf( "Bad challenge for challengeResponse. Ignored.\n" );
			return qfalse;
		}

		// start sending connect instead of challenge request packets
		clc.challenge = atoi(Cmd_Argv(1));
		cls.state = CA_CHALLENGING;
		clc.connectPacketCount = 0;
		clc.connectTime = -99999;

		// take this address as the new server address.  This allows
		// a server proxy to hand off connections to multiple servers
		clc.serverAddress = *from;
		Com_DPrintf( "challengeResponse: %d\n", clc.challenge );
		return qtrue;
	}

	// server connection
	if ( !Q_stricmp(c, "connectResponse") ) {
		if ( cls.state >= CA_CONNECTED ) {
			Com_Printf( "Dup connect received. Ignored.\n" );
			return qfalse;
		}
		if ( cls.state != CA_CHALLENGING ) {
			Com_Printf( "connectResponse packet while not connecting. Ignored.\n" );
			return qfalse;
		}
		if ( !NET_CompareAdr( from, &clc.serverAddress ) ) {
			Com_Printf( "connectResponse from wrong address. Ignored.\n" );
			return qfalse;
		}

		// first argument: challenge response
		c = Cmd_Argv( 1 );
		if ( *c != '\0' ) {
			challenge = atoi( c );
		} else {
			Com_Printf( "Bad connectResponse received. Ignored.\n" );
			return qfalse;
		}

		if ( challenge != clc.challenge ) {
			Com_Printf( "ConnectResponse with bad challenge received. Ignored.\n" );
			return qfalse;
		}

		Netchan_Setup( NS_CLIENT, &clc.netchan, from, Cvar_VariableIntegerValue( "net_qport" ), clc.challenge );

		cls.state = CA_CONNECTED;
		clc.lastPacketSentTime = cls.realtime - 9999; // send first packet immediately
		return qtrue;
	}

	// server responding to an info broadcast
	if ( !Q_stricmp(c, "infoResponse") ) {
		CL_ServerInfoPacket( from, msg );
		return qfalse;
	}

	// server responding to a get playerlist
	if ( !Q_stricmp(c, "statusResponse") ) {
		CL_ServerStatusResponse( from, msg );
		return qfalse;
	}

	// echo request from server
	if ( !Q_stricmp(c, "echo") ) {
		// NOTE: we may have to add exceptions for auth and update servers
		if ( (fromserver = NET_CompareAdr( from, &clc.serverAddress )) != qfalse || NET_CompareAdr( from, &rcon_address ) ) {
			NET_OutOfBandPrint( NS_CLIENT, from, "%s", Cmd_Argv(1) );
		}
		return fromserver;
	}

	// print string from server
	if ( !Q_stricmp(c, "print") ) {
		// NOTE: we may have to add exceptions for auth and update servers
		if ( (fromserver = NET_CompareAdr( from, &clc.serverAddress )) != qfalse || NET_CompareAdr( from, &rcon_address ) ) {
			s = MSG_ReadString( msg );
			Q_strncpyz( clc.serverMessage, s, sizeof( clc.serverMessage ) );
			Com_Printf( "%s", s );
		}
		return fromserver;
	}

	// list of servers sent back by a master server (classic)
	if ( !Q_strncmp(c, "getserversResponse", 18) ) {
		CL_ServersResponsePacket( from, msg, qfalse );
		return qfalse;
	}

	// list of servers sent back by a master server (extended)
	if ( !Q_strncmp(c, "getserversExtResponse", 21) ) {
		CL_ServersResponsePacket( from, msg, qtrue );
		return qfalse;
	}

	Com_DPrintf( "Unknown connectionless packet command.\n" );
	return qfalse;
}


/*
=================
CL_PacketEvent

A packet has arrived from the main event loop
=================
*/
void CL_PacketEvent( const netadr_t *from, msg_t *msg ) {
	int		headerBytes;

	if ( msg->cursize < 5 ) {
		Com_DPrintf( "%s: Runt packet\n", NET_AdrToStringwPort( from ) );
		return;
	}

	if ( *(int *)msg->data == -1 ) {
		if ( CL_ConnectionlessPacket( from, msg ) )
			clc.lastPacketTime = cls.realtime;
		return;
	}

	if ( cls.state < CA_CONNECTED ) {
		return;		// can't be a valid sequenced packet
	}

	//
	// packet from server
	//
	if ( !NET_CompareAdr( from, &clc.netchan.remoteAddress ) ) {
		if ( com_developer->integer ) {
			Com_Printf( "%s:sequenced packet without connection\n",
				NET_AdrToStringwPort( from ) );
		}
		// FIXME: send a client disconnect?
		return;
	}

	if ( !CL_Netchan_Process( &clc.netchan, msg ) ) {
		return;		// out of order, duplicated, etc
	}

	// the header is different lengths for reliable and unreliable messages
	headerBytes = msg->readcount;

	// track the last message received so it can be returned in
	// client messages, allowing the server to detect a dropped
	// gamestate
	clc.serverMessageSequence = LittleLong( *(int32_t *)msg->data );

	clc.lastPacketTime = cls.realtime;
	CL_ParseServerMessage( msg );

	//
	// we don't know if it is ok to save a demo message until
	// after we have parsed the frame
	//
	if ( clc.demorecording && !clc.demowaiting && !clc.demoplaying ) {
		CL_WriteDemoMessage( msg, headerBytes );
	}
}


/*
==================
CL_CheckTimeout
==================
*/
static void CL_CheckTimeout( void ) {
	//
	// check timeout
	//
	if ( ( !CL_CheckPaused() || !sv_paused->integer )
		&& cls.state >= CA_CONNECTED && cls.state != CA_CINEMATIC
		&& cls.realtime - clc.lastPacketTime > cl_timeout->integer * 1000 ) {
		if ( ++cl.timeoutcount > 5 ) { // timeoutcount saves debugger
			Com_Printf( "\nServer connection timed out.\n" );
			Cvar_Set( "com_errorMessage", "Server connection timed out." );
			if ( !CL_Disconnect( qfalse ) ) { // restart client if not done already
				CL_FlushMemory();
			}
			if ( uivm ) {
				VM_Call( uivm, 1, UI_SET_ACTIVE_MENU, UIMENU_MAIN );
			}
			return;
		}
	} else {
		cl.timeoutcount = 0;
	}
}


/*
==================
CL_CheckPaused
Check whether client has been paused.
==================
*/
qboolean CL_CheckPaused( void )
{
	// if cl_paused->modified is set, the cvar has only been changed in
	// this frame. Keep paused in this frame to ensure the server doesn't
	// lag behind.
	if(cl_paused->integer || cl_paused->modified)
		return qtrue;

	return qfalse;
}


/*
==================
CL_NoDelay
==================
*/
qboolean CL_NoDelay( void )
{
	if (CL_VideoRecording())
		return qtrue;

	return qfalse;
}


/*
==================
CL_CheckUserinfo
==================
*/
static void CL_CheckUserinfo( void ) {

	// don't add reliable commands when not yet connected
	if ( cls.state < CA_CONNECTED )
		return;

	// don't overflow the reliable command buffer when paused
	if ( CL_CheckPaused() )
		return;

	// send a reliable userinfo update if needed
	if ( cvar_modifiedFlags & CVAR_USERINFO )
	{
		qboolean infoTruncated = qfalse;
		const char *info;

		cvar_modifiedFlags &= ~CVAR_USERINFO;

		info = Cvar_InfoString( CVAR_USERINFO, &infoTruncated );
		if ( strlen( info ) > MAX_USERINFO_LENGTH || infoTruncated ) {
			Com_Printf( S_COLOR_YELLOW "WARNING: oversize userinfo, you might be not able to play on remote server!\n" );
		}

		CL_AddReliableCommand( va( "userinfo \"%s\"", info ), qfalse );
	}
}


/*
==================
CL_Frame
==================
*/
void CL_Frame( int msec, int realMsec ) {

	if ( !com_cl_running->integer ) {
		return;
	}

	// save the msec before checking pause
	cls.realFrametime = realMsec;

	if ( cls.state == CA_DISCONNECTED && !( Key_GetCatcher( ) & KEYCATCH_UI )
		&& !com_sv_running->integer && uivm ) {
		// if disconnected, bring up the menu
		//S_StopAllSounds();
		VM_Call( uivm, 1, UI_SET_ACTIVE_MENU, UIMENU_MAIN );
	}

	// if recording an avi, lock to a fixed fps
	if ( CL_VideoRecording() && msec ) {
		// save the current screen
		if ( cls.state == CA_ACTIVE ) {
			float fps, frameDuration;

			if ( com_timescale->value > 0.0001f )
				fps = MIN( cl_aviFrameRate->value / com_timescale->value, 1000.0f );
			else
				fps = 1000.0f;

			frameDuration = MAX( 1000.0f / fps, 1.0f ) + clc.aviVideoFrameRemainder;

			CL_TakeVideoFrame();

			msec = (int)frameDuration;
			clc.aviVideoFrameRemainder = frameDuration - msec;

			realMsec = msec; // sync sound duration
		}
	}

	// decide the simulation time
	cls.frametime = msec;
	cls.realtime += msec;

	if ( cl_timegraph->integer ) {
		SCR_DebugGraph( msec * 0.25f );
	}

	// see if we need to update any userinfo
	CL_CheckUserinfo();

	// if we haven't gotten a packet in a long time, drop the connection
	if ( !clc.demoplaying ) {
		CL_CheckTimeout();
	}

	// send intentions now
	CL_SendCmd();

	// resend a connection request if necessary
	CL_CheckForResend();

	// decide on the serverTime to render
	CL_SetCGameTime();

	// update the screen
	cls.framecount++;
	SCR_UpdateScreen();

	// update audio
	S_Update( realMsec );

	// advance local effects for next frame
	SCR_RunCinematic();

	Con_RunConsole();
}


//============================================================================

/*
================
CL_RefPrintf
================
*/
static void FORMAT_PRINTF(2, 3) QDECL CL_RefPrintf( printParm_t level, const char *fmt, ... ) {
	va_list		argptr;
	char		msg[MAXPRINTMSG];

	va_start( argptr, fmt );
	Q_vsnprintf( msg, sizeof( msg ), fmt, argptr );
	va_end( argptr );

	switch ( level ) {
		default: Com_Printf( "%s", msg ); break;
		case PRINT_DEVELOPER: Com_DPrintf( "%s", msg ); break;
		case PRINT_WARNING: Com_Printf( S_COLOR_YELLOW "%s", msg ); break;
		case PRINT_ERROR: Com_Printf( S_COLOR_RED "%s", msg ); break;
	}
}


/*
============
CL_ShutdownRef
============
*/
static void CL_ShutdownRef( refShutdownCode_t code ) {

	// clear and mute all sounds until next registration
	// S_DisableSounds();
	if ( code >= REF_DESTROY_WINDOW ) { // +REF_UNLOAD_DLL
		// shutdown sound system before renderer
		// because it may depend from window handle
		S_Shutdown();
	}

	SCR_Done();

	if ( re.Shutdown ) {
		re.Shutdown( code );
	}

	Com_Memset( &re, 0, sizeof( re ) );

	cls.rendererStarted = qfalse;
}


/*
============
CL_InitRenderer
============
*/
static void CL_InitRenderer( void ) {

	// fixup renderer -EC-
	if ( !re.BeginRegistration ) {
		CL_InitRef();
	}

	// this sets up the renderer and calls R_Init
	re.BeginRegistration( &cls.glconfig );

	// load character sets
	cls.defaultFont[0] = re.RegisterShader( "default_font0" ); //256
	cls.defaultFont[1] = re.RegisterShader( "default_font1" ); //512
	cls.defaultFont[2] = re.RegisterShader( "default_font2" ); //1024
	cls.defaultFont[3] = re.RegisterShader( "default_font3" ); //2048
	cls.defaultFont[4] = re.RegisterShader( "default_font4" ); //4096
	cls.whiteShader = re.RegisterShader( "white" );
	cls.consoleShader = re.RegisterShader( "console" );

	Con_CheckResize();

	g_console_field_width = ((cls.glconfig.vidWidth / smallchar_width)) - 2;
	g_consoleField.widthInChars = g_console_field_width;

	// for 640x480 virtualized screen
	cls.biasY = 0;
	cls.biasX = 0;
	if ( cls.glconfig.vidWidth * 480 > cls.glconfig.vidHeight * 640 ) {
		// wide screen, scale by height
		cls.scale = cls.glconfig.vidHeight * (1.0/480.0);
		cls.biasX = 0.5 * ( cls.glconfig.vidWidth - ( cls.glconfig.vidHeight * (640.0/480.0) ) );
	} else {
		// no wide screen, scale by width
		cls.scale = cls.glconfig.vidWidth * (1.0/640.0);
		cls.biasY = 0.5 * ( cls.glconfig.vidHeight - ( cls.glconfig.vidWidth * (480.0/640) ) );
	}

	SCR_Init();
}


/*
============================
CL_StartHunkUsers

After the server has cleared the hunk, these will need to be restarted
This is the only place that any of these functions are called from
============================
*/
void CL_StartHunkUsers( void ) {

	if ( !com_cl_running || !com_cl_running->integer ) {
		return;
	}
	
	FS_Reload();

	if ( !cls.rendererStarted ) {
		cls.rendererStarted = qtrue;
		CL_InitRenderer();
	}

	if ( !cls.soundStarted ) {
		cls.soundStarted = qtrue;
		S_Init();
	}

	if ( !cls.soundRegistered ) {
		cls.soundRegistered = qtrue;
		S_BeginRegistration();
	}

	if ( !cls.uiStarted ) {
		cls.uiStarted = qtrue;
		CL_InitUI();
	}
}


/*
============
CL_RefMalloc
============
*/
static void *CL_RefMalloc( int size ) {
	return Z_TagMalloc( size, TAG_RENDERER );
}


/*
============
CL_RefFreeAll
============
*/
static void CL_RefFreeAll( void ) {
	Z_FreeTags( TAG_RENDERER );
}


/*
============
CL_ScaledMilliseconds
============
*/
int CL_ScaledMilliseconds( void ) {
	return Sys_Milliseconds()*com_timescale->value;
}


/*
============
CL_IsMinimized
============
*/
static qboolean CL_IsMininized( void ) {
	return gw_minimized;
}


/*
============
CL_SetScaling

Sets console chars height
============
*/
static void CL_SetScaling( float factor, int captureWidth, int captureHeight ) {

	if ( cls.con_factor != factor ) {
		// rescale console
		con_scale->modified = qtrue;
	}

	cls.con_factor = factor;

	// set custom capture resolution
	cls.captureWidth = captureWidth;
	cls.captureHeight = captureHeight;
}


/*
============
CL_InitRef
============
*/
static void CL_InitRef( void ) {
	refimport_t	rimp;
	refexport_t	*ret;

	CL_InitGLimp_Cvars();

	Com_Printf( "----- Initializing Renderer ----\n" );

	Com_Memset( &rimp, 0, sizeof( rimp ) );

	rimp.Cmd_AddCommand = Cmd_AddCommand;
	rimp.Cmd_RemoveCommand = Cmd_RemoveCommand;
	rimp.Cmd_Argc = Cmd_Argc;
	rimp.Cmd_Argv = Cmd_Argv;
	rimp.Cmd_ExecuteText = Cbuf_ExecuteText;
	rimp.Printf = CL_RefPrintf;
	rimp.Error = Com_Error;
	rimp.Milliseconds = CL_ScaledMilliseconds;
	rimp.Microseconds = Sys_Microseconds;
	rimp.Malloc = CL_RefMalloc;
	rimp.FreeAll = CL_RefFreeAll;
	rimp.Free = Z_Free;
#ifdef HUNK_DEBUG
	rimp.Hunk_AllocDebug = Hunk_AllocDebug;
#else
	rimp.Hunk_Alloc = Hunk_Alloc;
#endif
	rimp.Hunk_AllocateTempMemory = Hunk_AllocateTempMemory;
	rimp.Hunk_FreeTempMemory = Hunk_FreeTempMemory;

	rimp.CM_ClusterPVS = CM_ClusterPVS;

	rimp.FS_ReadFile = FS_ReadFile;
	rimp.FS_FreeFile = FS_FreeFile;
	rimp.FS_WriteFile = FS_WriteFile;
	rimp.FS_FreeFileList = FS_FreeFileList;
	rimp.FS_ListFiles = FS_ListFiles;
	rimp.FS_FileExists = FS_FileExists;

	rimp.Cvar_Get = Cvar_Get;
	rimp.Cvar_Set = Cvar_Set;
	rimp.Cvar_SetValue = Cvar_SetValue;
	rimp.Cvar_SetDescription = Cvar_SetDescription;
	rimp.Cvar_VariableString = Cvar_VariableString;
	rimp.Cvar_VariableIntegerValue = Cvar_VariableIntegerValue;

	rimp.Cvar_SetGroup = Cvar_SetGroup;
	rimp.Cvar_CheckGroup = Cvar_CheckGroup;
	rimp.Cvar_ResetGroup = Cvar_ResetGroup;

	// cinematic stuff

	rimp.CIN_UploadCinematic = CIN_UploadCinematic;
	rimp.CIN_PlayCinematic = CIN_PlayCinematic;
	rimp.CIN_RunCinematic = CIN_RunCinematic;

	rimp.CL_WriteAVIVideoFrame = CL_WriteAVIVideoFrame;
	rimp.CL_SaveJPGToBuffer = CL_SaveJPGToBuffer;
	rimp.CL_SaveJPG = CL_SaveJPG;
	rimp.CL_LoadJPG = CL_LoadJPG;

	rimp.CL_IsMinimized = CL_IsMininized;
	rimp.CL_SetScaling = CL_SetScaling;

	rimp.Sys_SetClipboardBitmap = Sys_SetClipboardBitmap;
	rimp.Sys_LowPhysicalMemory = Sys_LowPhysicalMemory;
	rimp.Com_RealTime = Com_RealTime;

	rimp.GLimp_InitGamma = GLimp_InitGamma;
	rimp.GLimp_SetGamma = GLimp_SetGamma;

	// OpenGL API
#ifdef USE_OPENGL_API
	rimp.GLimp_Init = GLimp_Init;
	rimp.GLimp_Shutdown = GLimp_Shutdown;
	rimp.GL_GetProcAddress = GL_GetProcAddress;
	rimp.GLimp_EndFrame = GLimp_EndFrame;
#endif

	// Vulkan API
#ifdef USE_VULKAN_API
	rimp.VKimp_Init = VKimp_Init;
	rimp.VKimp_Shutdown = VKimp_Shutdown;
	rimp.VK_GetInstanceProcAddr = VK_GetInstanceProcAddr;
	rimp.VK_CreateSurface = VK_CreateSurface;
#endif

	ret = GetRefAPI( REF_API_VERSION, &rimp );

	Com_Printf( "-------------------------------\n");

	if ( !ret ) {
		Com_Error (ERR_FATAL, "Couldn't initialize refresh" );
	}

	re = *ret;

	// unpause so the cgame definitely gets a snapshot and renders a frame
	Cvar_Set( "cl_paused", "0" );
}

/*
===============
CL_Video_f

video
video [filename]
===============
*/
static void CL_Video_f( void )
{
	char filename[ MAX_OSPATH ];
	const char *ext;
	qboolean pipe;
	int i;

	if( !clc.demoplaying )
	{
		Com_Printf( "The %s command can only be used when playing back demos\n", Cmd_Argv( 0 ) );
		return;
	}

	pipe = ( Q_stricmp( Cmd_Argv( 0 ), "video-pipe" ) == 0 );

	if ( pipe )
		ext = "mp4";
	else
		ext = "avi";

	if ( Cmd_Argc() == 2 )
	{
		// explicit filename
		Com_sprintf( filename, sizeof( filename ), "videos/%s", Cmd_Argv( 1 ) );

		// override video file extension
		if ( pipe )
		{
			char *sep = strrchr( filename, '/' ); // last path separator
			char *e = strrchr( filename, '.' );

			if ( e && e > sep && *(e+1) != '\0' ) {
				ext = e + 1;
				*e = '\0';
			}
		}
	}
	else
	{
		 // scan for a free filename
		for ( i = 0; i <= 9999; i++ )
		{
			Com_sprintf( filename, sizeof( filename ), "videos/video%04d.%s", i, ext );
			if ( !FS_FileExists( filename ) )
				break; // file doesn't exist
		}

		if ( i > 9999 )
		{
			Com_Printf( S_COLOR_RED "ERROR: no free file names to create video\n" );
			return;
		}

		// without extension
		Com_sprintf( filename, sizeof( filename ), "videos/video%04d", i );
	}


	clc.aviSoundFrameRemainder = 0.0f;
	clc.aviVideoFrameRemainder = 0.0f;

	Q_strncpyz( clc.videoName, filename, sizeof( clc.videoName ) );
	clc.videoIndex = 0;

	CL_OpenAVIForWriting( va( "%s.%s", clc.videoName, ext ), pipe, qfalse );
}


/*
===============
CL_StopVideo_f
===============
*/
static void CL_StopVideo_f( void )
{
	CL_CloseAVI( qfalse );
}


/*
====================
CL_CompleteRecordName
====================
*/
static void CL_CompleteVideoName(const char *args, int argNum )
{
	if ( argNum == 2 )
	{
		Field_CompleteFilename( "videos", ".avi", qtrue, FS_MATCH_EXTERN | FS_MATCH_STICK );
	}
}

/*
===============
CL_GenerateQKey

test to see if a valid QKEY_FILE exists.  If one does not, try to generate
it by filling it with 2048 bytes of random data.
===============
*/
static void CL_GenerateQKey(void)
{
	int len = 0;
	unsigned char buff[ QKEY_SIZE ];
	fileHandle_t f;

	len = FS_SV_FOpenFileRead( QKEY_FILE, &f );
	FS_FCloseFile( f );
	if( len == QKEY_SIZE ) {
		Com_Printf( "QKEY found.\n" );
		return;
	}
	else {
		if( len > 0 ) {
			Com_Printf( "QKEY file size != %d, regenerating\n",
				QKEY_SIZE );
		}

		Com_Printf( "QKEY building random string\n" );
		Com_RandomBytes( buff, sizeof(buff) );

		f = FS_SV_FOpenFileWrite( QKEY_FILE );
		if( !f ) {
			Com_Printf( "QKEY could not open %s for write\n",
				QKEY_FILE );
			return;
		}
		FS_Write( buff, sizeof(buff), f );
		FS_FCloseFile( f );
		Com_Printf( "QKEY generated\n" );
	}
}

/*
==================
CL_ConvertOBJ
==================
*/
#define OBJ_TO_MD3_SCALE 3205 //for BlockBench sizes

// Rotation for BlockBench orient
#define MODEL_ROTATE_X 90
#define MODEL_ROTATE_Y 0
#define MODEL_ROTATE_Z 90

typedef struct {
    char newmtl[MAX_QPATH];
    char map_Kd[MAX_QPATH*2];
} md3Material_t;

typedef struct {
    char materialName[MAX_QPATH];
    vec3_t *vertices;
    vec2_t *texCoords;
    vec3_t *normals;
    md3Triangle_t *triangles;
    int vertCount;
    int texCoordCount;
    int normalCount;
    int triCount;
} md3SurfaceData_t;

void RotateVertex(vec3_t vertex) {
    float x = vertex[0], y = vertex[1], z = vertex[2];
    
    if (MODEL_ROTATE_X != 0) {
        float rad = DEG2RAD(MODEL_ROTATE_X);
        float cosX = cos(rad);
        float sinX = sin(rad);
        float newY = y * cosX - z * sinX;
        float newZ = y * sinX + z * cosX;
        y = newY;
        z = newZ;
    }
    
    if (MODEL_ROTATE_Y != 0) {
        float rad = DEG2RAD(MODEL_ROTATE_Y);
        float cosY = cos(rad);
        float sinY = sin(rad);
        float newX = x * cosY + z * sinY;
        float newZ = -x * sinY + z * cosY;
        x = newX;
        z = newZ;
    }
    
    if (MODEL_ROTATE_Z != 0) {
        float rad = DEG2RAD(MODEL_ROTATE_Z);
        float cosZ = cos(rad);
        float sinZ = sin(rad);
        float newX = x * cosZ - y * sinZ;
        float newY = x * sinZ + y * cosZ;
        x = newX;
        y = newY;
    }
    
    vertex[0] = x;
    vertex[1] = y;
    vertex[2] = z;
}

void RotateNormal(vec3_t normal) {
    RotateVertex(normal);
    VectorNormalize(normal);
}

short MD3_EncodeNormal(float x, float y, float z) {
    int16_t lat, lng;

    if (x == 0.0f && y == 0.0f) {
        if (z > 0.0f) {
            lat = 0;
            lng = 0;
        } else {
            lat = 128;
            lng = 0;
        }
    } else {
        lng = (int16_t)( acos(z) * 255 / (2 * M_PI) );
        lat = (int16_t)( atan2(y, x) * 255 / (2 * M_PI) );
    }

    return (short)((lat & 255) * 256 | (lng & 255));
}

void LoadMTR(const char *name, md3Shader_t *shaders, int *numShaders, md3Material_t *materials, int *numMaterials) {
    char mtlFilename[MAX_QPATH];
	char folderFilename[MAX_QPATH];
    Q_strncpyz(mtlFilename, name, sizeof(mtlFilename));
	Q_strncpyz(folderFilename, name, sizeof(folderFilename));
    Q_strcat(mtlFilename, sizeof(mtlFilename), ".mtl");
    
    if (FS_ReadFile(mtlFilename, NULL) <= 0) return;
    
    char *mtlData = NULL;
    int mtlSize = FS_ReadFile(mtlFilename, (void **)&mtlData);
    if (!mtlData || mtlSize <= 0) return;

    char *p = mtlData;
    int currentMaterial = -1;
    
    while (*p && *numMaterials < MD3_MAX_SHADERS) {
        if (strncmp(p, "newmtl ", 7) == 0) {
            currentMaterial = (*numMaterials)++;
            sscanf(p+7, "%s", materials[currentMaterial].newmtl);
            materials[currentMaterial].map_Kd[0] = '\0';
        }
        else if (strncmp(p, "map_Kd ", 7) == 0 && currentMaterial >= 0) {
			char texturePath[MAX_QPATH];
			sscanf(p + 7, "%s", texturePath);
			
			snprintf( materials[currentMaterial].map_Kd, sizeof(materials[currentMaterial].map_Kd), "%s/%s", folderFilename, texturePath );
		}
        while (*p && *p != '\n') p++;
        if (*p == '\n') p++;
    }
    FS_FreeFile(mtlData);
}

void ParseOBJ(const char *objData, vec3_t *vertices, vec2_t *texCoords, vec3_t *normals,
              md3SurfaceData_t *surfaces, int *numSurfaces, int *vCount, int *vtCount, int *vnCount,
              const md3Material_t *materials, int numMaterials, md3Shader_t *shaders, int *numShaders) {
    const char *p = objData;
    int currentSurface = -1;
    int currentMaterialIndex = -1;
    int currentShaderIndex = -1;

    while (*p) {
        if (strncmp(p, "v ", 2) == 0 && *vCount < MD3_MAX_VERTS) {
            sscanf(p+2, "%f %f %f", &vertices[*vCount][0], &vertices[*vCount][1], &vertices[*vCount][2]);
			RotateVertex(vertices[*vCount]); // Вращение вершины
            (*vCount)++;
        }
        else if (strncmp(p, "vt ", 3) == 0 && *vtCount < MD3_MAX_VERTS) {
            sscanf(p+3, "%f %f", &texCoords[*vtCount][0], &texCoords[*vtCount][1]);
            texCoords[*vtCount][1] = 1.0f - texCoords[*vtCount][1];
            (*vtCount)++;
        }
        else if (strncmp(p, "vn ", 3) == 0 && *vnCount < MD3_MAX_VERTS) {
            sscanf(p+3, "%f %f %f", &normals[*vnCount][0], &normals[*vnCount][1], &normals[*vnCount][2]);
			RotateNormal(normals[*vnCount]);
            (*vnCount)++;
        }
        else if (strncmp(p, "usemtl ", 7) == 0) {
            char materialName[MAX_QPATH];
            sscanf(p+7, "%63s", materialName);
            
            currentSurface = -1;
            for (int i = 0; i < *numSurfaces; i++) {
                if (!Q_stricmp(surfaces[i].materialName, materialName)) {
                    currentSurface = i;
                    break;
                }
            }
            
            if (currentSurface == -1 && *numSurfaces < MD3_MAX_SURFACES) {
                currentSurface = *numSurfaces;
                Q_strncpyz(surfaces[currentSurface].materialName, materialName, sizeof(surfaces[currentSurface].materialName));
                surfaces[currentSurface].vertices = malloc(MD3_MAX_VERTS * sizeof(vec3_t));
                surfaces[currentSurface].texCoords = malloc(MD3_MAX_VERTS * sizeof(vec2_t));
                surfaces[currentSurface].normals = malloc(MD3_MAX_VERTS * sizeof(vec3_t));
                surfaces[currentSurface].triangles = malloc(MD3_MAX_TRIANGLES * sizeof(md3Triangle_t));
                surfaces[currentSurface].vertCount = 0;
                surfaces[currentSurface].texCoordCount = 0;
                surfaces[currentSurface].normalCount = 0;
                surfaces[currentSurface].triCount = 0;
                (*numSurfaces)++;
            }
            
            currentMaterialIndex = -1;
            for (int i = 0; i < numMaterials; i++) {
                if (!Q_stricmp(materials[i].newmtl, materialName)) {
                    currentMaterialIndex = i;
                    break;
                }
            }
            
            currentShaderIndex = -1;
            if (currentMaterialIndex != -1) {
                for (int i = 0; i < *numShaders; i++) {
                    if (!Q_stricmp(shaders[i].name, materialName)) {
                        currentShaderIndex = i;
                        break;
                    }
                }
                
                if (currentShaderIndex == -1 && *numShaders < MD3_MAX_SHADERS) {
                    Q_strncpyz(shaders[*numShaders].name, materialName, sizeof(shaders[*numShaders].name));
                    shaders[*numShaders].shaderIndex = *numShaders;
                    currentShaderIndex = *numShaders;
                    (*numShaders)++;
                }
            }
        }
        else if (strncmp(p, "f ", 2) == 0 && currentSurface >= 0 && 
         surfaces[currentSurface].triCount < MD3_MAX_TRIANGLES - 1) {
		    int v[4] = {-1, -1, -1, -1}, vt[4] = {-1, -1, -1, -1}, vn[4] = {-1, -1, -1, -1};
		    int count = 0;
		    const char *faceStart = p + 2;

		    while (*faceStart && count < 4) {
		        while (*faceStart == ' ') faceStart++;
		        if (!*faceStart) break;
			
		        char *end = NULL;
		        long val;
			
		        val = strtol(faceStart, &end, 10);
		        if (end == faceStart || val < 1) break;
		        v[count] = (int)val - 1;
		        faceStart = end;
			
		        if (*faceStart == '/') {
		            faceStart++;
		            if (*faceStart != '/') {
		                val = strtol(faceStart, &end, 10);
		                if (end != faceStart && val >= 1) {
		                    vt[count] = (int)val - 1;
		                }
		                faceStart = end;
		            }
				
		            if (*faceStart == '/') {
		                faceStart++;
		                val = strtol(faceStart, &end, 10);
		                if (end != faceStart && val >= 1) {
		                    vn[count] = (int)val - 1;
		                }
		                faceStart = end;
		            }
		        }
			
		        count++;
		    }
		
		    if (count >= 3) {
		        int valid = 1;
		        for (int i = 0; i < count; i++) {
		            if (v[i] < 0 || v[i] >= *vCount) valid = 0;
		            if (vt[i] >= *vtCount) valid = 0;
		            if (vn[i] >= *vnCount) valid = 0;
		        }
			
		        if (valid) {
		            int indices[4];
		            for (int i = 0; i < count; i++) {
		                if (surfaces[currentSurface].vertCount >= MD3_MAX_VERTS) {
		                    valid = 0;
		                    break;
		                }
					
		                VectorCopy(vertices[v[i]], surfaces[currentSurface].vertices[surfaces[currentSurface].vertCount]);
					
		                if (vt[i] >= 0) {
		                    Vector2Copy(texCoords[vt[i]], surfaces[currentSurface].texCoords[surfaces[currentSurface].vertCount]);
		                } else {
		                    surfaces[currentSurface].texCoords[surfaces[currentSurface].vertCount][0] = 0;
							surfaces[currentSurface].texCoords[surfaces[currentSurface].vertCount][1] = 0;
		                }
					
		                if (vn[i] >= 0) {
		                    VectorCopy(normals[vn[i]], surfaces[currentSurface].normals[surfaces[currentSurface].vertCount]);
		                } else {
		                    VectorSet(surfaces[currentSurface].normals[surfaces[currentSurface].vertCount], 0, 0, 0);
		                }
					
		                indices[i] = surfaces[currentSurface].vertCount++;
		            }
				
					if (valid && surfaces[currentSurface].triCount < MD3_MAX_TRIANGLES) {
						surfaces[currentSurface].triangles[surfaces[currentSurface].triCount].indexes[0] = indices[0];
						surfaces[currentSurface].triangles[surfaces[currentSurface].triCount].indexes[1] = indices[2];
						surfaces[currentSurface].triangles[surfaces[currentSurface].triCount].indexes[2] = indices[1];
						surfaces[currentSurface].triCount++;

						if (count == 4 && surfaces[currentSurface].triCount < MD3_MAX_TRIANGLES) {
							surfaces[currentSurface].triangles[surfaces[currentSurface].triCount].indexes[0] = indices[0];
							surfaces[currentSurface].triangles[surfaces[currentSurface].triCount].indexes[1] = indices[3];
							surfaces[currentSurface].triangles[surfaces[currentSurface].triCount].indexes[2] = indices[2];
							surfaces[currentSurface].triCount++;
						}
					}
		        }
		    }
		}
        while (*p && *p != '\n') p++;
        if (*p == '\n') p++;
    }
}

void WriteMD3(const char *name, const md3SurfaceData_t *surfaces, int numSurfaces, 
              const md3Shader_t *shaders, int numShaders, const md3Material_t *materials, int numMaterials) {
    fileHandle_t f = FS_FOpenFileWrite(va("%s.md3", name));
    if (!f) {
        Com_Printf("Error: Failed to create %s.md3\n", name);
        return;
    }

    md3Header_t header = {
        .ident = MD3_IDENT,
        .version = MD3_VERSION,
        .numFrames = 1,
        .numTags = 0,
        .numSurfaces = numSurfaces,
        .numSkins = numShaders
    };
    Q_strncpyz(header.name, name, sizeof(header.name));
    
    md3Frame_t frame = {0};
    VectorSet(frame.bounds[0], 25, 25, 25);
    VectorSet(frame.bounds[1], -25, -25, -25);
	VectorSet(frame.localOrigin, 0, 0, 0);
    frame.radius = 50.0f;
    Q_strncpyz(frame.name, "default", sizeof(frame.name));
    
    FS_Write(&header, sizeof(header), f);
    FS_Write(&frame, sizeof(frame), f);
    
    int totalVertCount = 0;
    int totalTriCount = 0;
    
    for (int s = 0; s < numSurfaces; s++) {
        const md3SurfaceData_t *surf = &surfaces[s];
        
        md3Surface_t surface = {0};
        surface.ident = MD3_IDENT;
        Q_strncpyz(surface.name, surf->materialName, sizeof(surface.name));
        surface.numFrames = 1;
        surface.numShaders = 1;
        surface.numVerts = surf->vertCount;
        surface.numTriangles = surf->triCount;
        surface.ofsTriangles = sizeof(md3Surface_t);
        surface.ofsShaders = surface.ofsTriangles + surf->triCount * sizeof(md3Triangle_t);
        surface.ofsSt = surface.ofsShaders + sizeof(md3Shader_t);
        surface.ofsXyzNormals = surface.ofsSt + surf->vertCount * sizeof(md3St_t);
        surface.ofsEnd = surface.ofsXyzNormals + surf->vertCount * sizeof(md3XyzNormal_t);
        
        FS_Write(&surface, sizeof(surface), f);
        
        FS_Write(surf->triangles, surf->triCount * sizeof(md3Triangle_t), f);
        
        md3Shader_t shader;
        Q_strncpyz(shader.name, surf->materialName, sizeof(shader.name));
        shader.shaderIndex = 0;
        
        for (int i = 0; i < numMaterials; i++) {
            if (!Q_stricmp(materials[i].newmtl, surf->materialName)) {
                Q_strncpyz(shader.name, materials[i].map_Kd, sizeof(shader.name));
                break;
            }
        }
        
        FS_Write(&shader, sizeof(shader), f);
        
        for (int i = 0; i < surf->vertCount; i++) {
            md3St_t st = { .st = { surf->texCoords[i][0], surf->texCoords[i][1] } };
            FS_Write(&st, sizeof(st), f);
        }
        
        for (int i = 0; i < surf->vertCount; i++) {
            md3XyzNormal_t xyz;
            xyz.xyz[0] = (short)(surf->vertices[i][0] * OBJ_TO_MD3_SCALE);
            xyz.xyz[1] = (short)(surf->vertices[i][1] * OBJ_TO_MD3_SCALE);
            xyz.xyz[2] = (short)(surf->vertices[i][2] * OBJ_TO_MD3_SCALE);
            xyz.normal = MD3_EncodeNormal(surf->normals[i][0], surf->normals[i][1], surf->normals[i][2]);
            FS_Write(&xyz, sizeof(xyz), f);
        }
        
        totalVertCount += surf->vertCount;
        totalTriCount += surf->triCount;
    }
    
    header.ofsSurfaces = sizeof(md3Header_t) + sizeof(md3Frame_t);
    header.ofsEnd = FS_FTell(f);
    FS_Seek(f, 0, FS_SEEK_SET);
    FS_Write(&header, sizeof(header), f);
    FS_FCloseFile(f);
    
    Com_Printf("Success: Converted %s.obj to %s.md3 (%d verts, %d tris, %d surfaces, %d shaders)\n",
              name, name, totalVertCount, totalTriCount, numSurfaces, numShaders);
}

void FreeSurfaceData(md3SurfaceData_t *surfaces, int numSurfaces) {
    for (int i = 0; i < numSurfaces; i++) {
        free(surfaces[i].vertices);
        free(surfaces[i].texCoords);
        free(surfaces[i].normals);
        free(surfaces[i].triangles);
    }
}

void CL_StartConvertOBJ(const char *name) {
    char *objData = NULL;
    int objSize = FS_ReadFile(va("%s.obj", name), (void **)&objData);
    if (!objData || objSize <= 0) {
        Com_Printf("Error: Failed to open %s.obj\n", name);
        return;
    }

    md3Material_t materials[MD3_MAX_SHADERS];
    int numMaterials = 0;
    md3Shader_t shaders[MD3_MAX_SHADERS];
    int numShaders = 0;
    LoadMTR(name, shaders, &numShaders, materials, &numMaterials);

    vec3_t *vertices = (vec3_t *)malloc(MD3_MAX_VERTS * sizeof(vec3_t));
    vec2_t *texCoords = (vec2_t *)malloc(MD3_MAX_VERTS * sizeof(vec2_t));
    vec3_t *normals = (vec3_t *)malloc(MD3_MAX_VERTS * sizeof(vec3_t));
    int vCount = 0, vtCount = 0, vnCount = 0;
    
    md3SurfaceData_t surfaces[MD3_MAX_SURFACES];
    int numSurfaces = 0;

    ParseOBJ(objData, vertices, texCoords, normals, surfaces, &numSurfaces, 
             &vCount, &vtCount, &vnCount, materials, numMaterials, shaders, &numShaders);

    WriteMD3(name, surfaces, numSurfaces, shaders, numShaders, materials, numMaterials);

	free(vertices);
    free(texCoords);
    free(normals);
    FreeSurfaceData(surfaces, numSurfaces);
    FS_FreeFile(objData);
}

void CL_ConvertOBJ(void) {
    const char *name = Cmd_Argv(1);
    if (!name[0]) {
        Com_Printf("Usage: importOBJ <modelname> (without .obj)\n");
        return;
    }
    CL_StartConvertOBJ(name);
}

qboolean CL_ParseResolution( const char *str, int *width, int *height ) {
    if ( !str || !width || !height ) return qfalse;

    int w = 0, h = 0;
    if ( sscanf( str, "%dx%d", &w, &h ) == 2 && w > 0 && h > 0 ) {
        *width = w;
        *height = h;
		return qtrue;
    }
	return qfalse;
}

qboolean CL_GetModeInfo( int *width, int *height, float *windowAspect, const char *resolution, int dw, int dh, int fullscreen ) {
	float	pixelAspect;

	//fix unknown native desktop resolution
	if ( fullscreen == 3 && (dw == 0 || dh == 0) )
		resolution = "640x480";

	if ( fullscreen == 3 ) { //native desktop resolution
		*width = dw;
		*height = dh;
		pixelAspect = 1;
	} else { //custom resolution
		if (!CL_ParseResolution(resolution, width, height)) {
    		*width = 640;
    		*height = 480;
		}
		pixelAspect = 1;
	}

	*windowAspect = (float)*width / ( *height * pixelAspect );

	return qtrue;
}

static void CL_InitGLimp_Cvars( void )  {
	// shared with GLimp
	r_swapInterval = Cvar_Get( "r_swapInterval", "1", CVAR_ARCHIVE );
	Cvar_SetDescription( r_swapInterval, "V-blanks to wait before swapping buffers.\n 0: No V-Sync\n 1: Synced to the monitor's refresh rate." );
	r_resolution = Cvar_Get( "r_resolution", "640x480", CVAR_ARCHIVE | CVAR_LATCH );
	Cvar_SetDescription( r_resolution, "Set resolution in [width]x[height] format" );
	r_fullscreen = Cvar_Get( "r_fullscreen", "3", CVAR_ARCHIVE | CVAR_LATCH );
	Cvar_SetDescription( r_fullscreen, "Fullscreen mode: \n 0 - windowed. \n 1 - borderless windowed. \n 2 - fullscreen. \n 3 - fullscreen native." );
	r_availableModes = Cvar_Get( "r_availableModes", "0", CVAR_ROM );
}


/*
====================
CL_Init
====================
*/
void CL_Init( void ) {
	cvar_t *cv;

	Com_Printf( "----- Client Initialization -----\n" );

	Con_Init();

	CL_ClearState();
	cls.state = CA_DISCONNECTED;	// no longer CA_UNINITIALIZED

	cls.realtime = 0;

	CL_InitInput();

	//
	// register client variables
	//
	cl_noprint = Cvar_Get( "cl_noprint", "0", 0 );
	Cvar_SetDescription( cl_noprint, "Disable printing of information in the console." );

	cl_timeout = Cvar_Get( "cl_timeout", "200", 0 );
	Cvar_SetDescription( cl_timeout, "Duration of receiving nothing from server for client to decide it must be disconnected (in seconds)." );

	cl_shownet = Cvar_Get ("cl_shownet", "0", 0 );
	Cvar_SetDescription( cl_shownet, "Toggle the display of current network status." );
	rcon_client_password = Cvar_Get ("rconPassword", "", 0 );
	Cvar_SetDescription( rcon_client_password, "Sets a remote console password so clients may change server settings without direct access to the server console." );
	cl_activeAction = Cvar_Get( "activeAction", "", 0 );
	Cvar_SetDescription( cl_activeAction, "Contents of this variable will be executed upon first frame of play.\nNote: It is cleared every time it is executed." );

	cl_aviFrameRate = Cvar_Get ("cl_aviFrameRate", "25", CVAR_ARCHIVE);
	Cvar_SetDescription( cl_aviFrameRate, "The framerate used for capturing video." );
	cl_aviMotionJpeg = Cvar_Get ("cl_aviMotionJpeg", "1", CVAR_ARCHIVE);
	Cvar_SetDescription( cl_aviMotionJpeg, "Enable/disable the MJPEG codec for avi output." );

	cl_aviPipeFormat = Cvar_Get( "cl_aviPipeFormat",
		"-preset medium -crf 23 -c:v libx264 -flags +cgop -pix_fmt yuvj420p "
		"-bf 2 -c:a aac -strict -2 -b:a 160k -movflags faststart",
		CVAR_ARCHIVE );
	Cvar_SetDescription( cl_aviPipeFormat, "Encoder parameters used for \\video-pipe." );

	rconAddress = Cvar_Get ("rconAddress", "", 0);
	Cvar_SetDescription( rconAddress, "The IP address of the remote console you wish to connect to." );

	cl_allowDownload = Cvar_Get( "cl_allowDownload", "1", CVAR_ARCHIVE );
	Cvar_SetDescription( cl_allowDownload, "Enables downloading of content needed in server. Valid bitmask flags:\n 1: Downloading enabled\n 2: Do not use HTTP/FTP downloads\n 4: Do not use UDP downloads" );

	cl_serverStatusResendTime = Cvar_Get ("cl_serverStatusResendTime", "750", 0);
	Cvar_SetDescription( cl_serverStatusResendTime, "Time between re-sending server status requests if no response is received (in milliseconds)." );

	cv = Cvar_Get( "cl_maxPing", "999", CVAR_ARCHIVE );
	Cvar_SetDescription( cv, "Specify the maximum allowed ping to a server." );

	cl_lanForcePackets = Cvar_Get( "cl_lanForcePackets", "1", CVAR_ARCHIVE );
	Cvar_SetDescription( cl_lanForcePackets, "Bypass \\cl_maxpackets for LAN games, send packets every frame." );

	// userinfo
	Cvar_Get ("name", "Sandbox Player", CVAR_USERINFO | CVAR_ARCHIVE );
	Cvar_Get ("rate", "125000", CVAR_USERINFO | CVAR_ARCHIVE );
	Cvar_Get ("snaps", "60", CVAR_USERINFO | CVAR_ARCHIVE );
	Cvar_Get ("model", "beret/default", CVAR_USERINFO | CVAR_ARCHIVE );
	Cvar_Get ("headmodel", "beret/default", CVAR_USERINFO | CVAR_ARCHIVE );
	Cvar_Get ("legsmodel", "beret/default", CVAR_USERINFO | CVAR_ARCHIVE );
 	Cvar_Get ("team_model", "beret/default", CVAR_USERINFO | CVAR_ARCHIVE );
	Cvar_Get ("team_headmodel", "beret/default", CVAR_USERINFO | CVAR_ARCHIVE );
	Cvar_Get ("team_legsmodel", "beret/default", CVAR_USERINFO | CVAR_ARCHIVE );
	Cvar_Get ("sex", "male", CVAR_USERINFO | CVAR_ARCHIVE );
	Cvar_Get ("cl_anonymous", "0", CVAR_USERINFO | CVAR_ARCHIVE );

	Cvar_Get ("password", "", CVAR_USERINFO);

	Cvar_Get ("viewdistance", "180", CVAR_USERINFO );

	//
	// register client commands
	//
	Cmd_AddCommand ("cmd", CL_ForwardToServer_f);
	Cmd_AddCommand ("configstrings", CL_Configstrings_f);
	Cmd_AddCommand ("clientinfo", CL_Clientinfo_f);
	Cmd_AddCommand ("snd_restart", CL_Snd_Restart_f);
	Cmd_AddCommand ("vid_restart", CL_Vid_Restart_f);
	Cmd_AddCommand ("disconnect", CL_Disconnect_f);
	Cmd_AddCommand ("record", CL_Record_f);
	Cmd_SetCommandCompletionFunc( "record", CL_CompleteRecordName );
	Cmd_AddCommand ("demo", CL_PlayDemo_f);
	Cmd_SetCommandCompletionFunc( "demo", CL_CompleteDemoName );
	Cmd_AddCommand ("cinematic", CL_PlayCinematic_f);
	Cmd_AddCommand ("stoprecord", CL_StopRecord_f);
	Cmd_AddCommand ("connect", CL_Connect_f);
	Cmd_AddCommand ("localservers", CL_LocalServers_f);
	Cmd_AddCommand ("globalservers", CL_GlobalServers_f);
	Cmd_AddCommand ("rcon", CL_Rcon_f);
	Cmd_SetCommandCompletionFunc( "rcon", CL_CompleteRcon );
	Cmd_AddCommand ("ping", CL_Ping_f );
	Cmd_AddCommand ("serverstatus", CL_ServerStatus_f );
	Cmd_AddCommand ("showip", CL_ShowIP_f );
	Cmd_AddCommand ("fs_openedList", CL_OpenedPK3List_f );
	Cmd_AddCommand ("video", CL_Video_f );
	Cmd_AddCommand ("video-pipe", CL_Video_f );
	Cmd_SetCommandCompletionFunc( "video", CL_CompleteVideoName );
	Cmd_AddCommand ("stopvideo", CL_StopVideo_f );
	Cmd_AddCommand ("serverinfo", CL_Serverinfo_f );
	Cmd_AddCommand ("systeminfo", CL_Systeminfo_f );

	Cmd_AddCommand( "importOBJ", CL_ConvertOBJ );

	Cvar_Set( "cl_running", "1" );

	CL_GenerateQKey();

	Cvar_Get( "cl_guid", "", CVAR_USERINFO | CVAR_ROM );
	CL_UpdateGUID( NULL, 0 );

	Com_Printf( "----- Client Initialization Complete -----\n" );
}


/*
===============
CL_Shutdown

Called on fatal error, quit and dedicated mode switch
===============
*/
void CL_Shutdown( const char *finalmsg, qboolean quit ) {
	static qboolean recursive = qfalse;

	// check whether the client is running at all.
	if ( !( com_cl_running && com_cl_running->integer ) )
		return;

	Com_Printf( "----- Client Shutdown (%s) -----\n", finalmsg );

	if ( recursive ) {
		Com_Printf( "WARNING: Recursive CL_Shutdown()\n" );
		return;
	}
	recursive = qtrue;

	CL_Disconnect( qfalse );

	// clear and mute all sounds until next registration
	S_DisableSounds();

	CL_ShutdownVMs();

	CL_ShutdownRef( quit ? REF_UNLOAD_DLL : REF_DESTROY_WINDOW );

	Con_Shutdown();

	Cmd_RemoveCommand ("cmd");
	Cmd_RemoveCommand ("configstrings");
	Cmd_RemoveCommand ("userinfo");
	Cmd_RemoveCommand ("clientinfo");
	Cmd_RemoveCommand ("snd_restart");
	Cmd_RemoveCommand ("vid_restart");
	Cmd_RemoveCommand ("disconnect");
	Cmd_RemoveCommand ("record");
	Cmd_RemoveCommand ("demo");
	Cmd_RemoveCommand ("cinematic");
	Cmd_RemoveCommand ("stoprecord");
	Cmd_RemoveCommand ("connect");
	Cmd_RemoveCommand ("localservers");
	Cmd_RemoveCommand ("globalservers");
	Cmd_RemoveCommand ("rcon");
	Cmd_RemoveCommand ("ping");
	Cmd_RemoveCommand ("serverstatus");
	Cmd_RemoveCommand ("showip");
	Cmd_RemoveCommand ("fs_openedList");
	Cmd_RemoveCommand ("model");
	Cmd_RemoveCommand ("video");
	Cmd_RemoveCommand ("stopvideo");
	Cmd_RemoveCommand ("serverinfo");
	Cmd_RemoveCommand ("systeminfo");
	Cmd_RemoveCommand ("modelist");

	CL_ClearInput();

	Cvar_Set( "cl_running", "0" );

	recursive = qfalse;

	Com_Memset( &cls, 0, sizeof( cls ) );
	Key_SetCatcher( 0 );
	Com_Printf( "-----------------------\n" );
}


static void CL_SetServerInfo(serverInfo_t *server, const char *info, int ping) {
	if (server) {
		if (info) {
			server->clients = atoi(Info_ValueForKey(info, "clients"));
			Q_strncpyz(server->hostName,Info_ValueForKey(info, "hostname"), MAX_NAME_LENGTH);
			Q_strncpyz(server->addonName,Info_ValueForKey(info, "addonname"), MAX_NAME_LENGTH);
			Q_strncpyz(server->mapName, Info_ValueForKey(info, "mapname"), MAX_NAME_LENGTH);
			server->maxClients = atoi(Info_ValueForKey(info, "g_maxClients"));
			Q_strncpyz(server->game,Info_ValueForKey(info, "game"), MAX_NAME_LENGTH);
			server->gameType = atoi(Info_ValueForKey(info, "gametype"));
			server->netType = atoi(Info_ValueForKey(info, "nettype"));
			server->minPing = atoi(Info_ValueForKey(info, "minping"));
			server->maxPing = atoi(Info_ValueForKey(info, "maxping"));
			server->g_humanplayers = atoi(Info_ValueForKey(info, "g_humanplayers"));
			server->g_needpass = atoi(Info_ValueForKey(info, "g_needpass"));
		}
		server->ping = ping;
	}
}


static void CL_SetServerInfoByAddress(const netadr_t *from, const char *info, int ping) {
	int i;

	for (i = 0; i < MAX_OTHER_SERVERS; i++) {
		if (NET_CompareAdr(from, &cls.localServers[i].adr) ) {
			CL_SetServerInfo(&cls.localServers[i], info, ping);
		}
	}

	for (i = 0; i < MAX_GLOBAL_SERVERS; i++) {
		if (NET_CompareAdr(from, &cls.globalServers[i].adr)) {
			CL_SetServerInfo(&cls.globalServers[i], info, ping);
		}
	}

	for (i = 0; i < MAX_OTHER_SERVERS; i++) {
		if (NET_CompareAdr(from, &cls.favoriteServers[i].adr)) {
			CL_SetServerInfo(&cls.favoriteServers[i], info, ping);
		}
	}
}


/*
===================
CL_ServerInfoPacket
===================
*/
static void CL_ServerInfoPacket( const netadr_t *from, msg_t *msg ) {
	int		i, type, len;
	char	info[MAX_INFO_STRING];
	const char *infoString;

	infoString = MSG_ReadString( msg );

	// iterate servers waiting for ping response
	for (i=0; i<MAX_PINGREQUESTS; i++)
	{
		if ( cl_pinglist[i].adr.port && !cl_pinglist[i].time && NET_CompareAdr( from, &cl_pinglist[i].adr ) )
		{
			// calc ping time
			cl_pinglist[i].time = Sys_Milliseconds() - cl_pinglist[i].start;
			if ( cl_pinglist[i].time < 1 )
			{
				cl_pinglist[i].time = 1;
			}
			if ( com_developer->integer )
			{
				Com_Printf( "ping time %dms from %s\n", cl_pinglist[i].time, NET_AdrToString( from ) );
			}

			// save of info
			Q_strncpyz( cl_pinglist[i].info, infoString, sizeof( cl_pinglist[i].info ) );

			// tack on the net type
			// NOTE: make sure these types are in sync with the netnames strings in the UI
			switch (from->type)
			{
				case NA_BROADCAST:
				case NA_IP:
					type = 1;
					break;
#ifdef USE_IPV6
				case NA_IP6:
					type = 2;
					break;
#endif
				default:
					type = 0;
					break;
			}

			Info_SetValueForKey( cl_pinglist[i].info, "nettype", va( "%d", type ) );
			CL_SetServerInfoByAddress( from, infoString, cl_pinglist[i].time );

			return;
		}
	}

	// if not just sent a local broadcast or pinging local servers
	if (cls.pingUpdateSource != AS_LOCAL) {
		return;
	}

	for ( i = 0 ; i < MAX_OTHER_SERVERS ; i++ ) {
		// empty slot
		if ( cls.localServers[i].adr.port == 0 ) {
			break;
		}

		// avoid duplicate
		if ( NET_CompareAdr( from, &cls.localServers[i].adr ) ) {
			return;
		}
	}

	if ( i == MAX_OTHER_SERVERS ) {
		Com_DPrintf( "MAX_OTHER_SERVERS hit, dropping infoResponse\n" );
		return;
	}

	// add this to the list
	cls.numlocalservers = i+1;
	CL_InitServerInfo( &cls.localServers[i], from );

	Q_strncpyz( info, MSG_ReadString( msg ), sizeof( info ) );
	len = (int) strlen( info );
	if ( len > 0 ) {
		if ( info[ len-1 ] == '\n' ) {
			info[ len-1 ] = '\0';
		}
		Com_Printf( "%s: %s\n", NET_AdrToStringwPort( from ), info );
	}
}


/*
===================
CL_GetServerStatus
===================
*/
static serverStatus_t *CL_GetServerStatus( const netadr_t *from ) {
	int i, oldest, oldestTime;

	for (i = 0; i < MAX_SERVERSTATUSREQUESTS; i++) {
		if ( NET_CompareAdr( from, &cl_serverStatusList[i].address ) ) {
			return &cl_serverStatusList[i];
		}
	}
	for (i = 0; i < MAX_SERVERSTATUSREQUESTS; i++) {
		if ( cl_serverStatusList[i].retrieved ) {
			return &cl_serverStatusList[i];
		}
	}
	oldest = -1;
	oldestTime = 0;
	for (i = 0; i < MAX_SERVERSTATUSREQUESTS; i++) {
		if (oldest == -1 || cl_serverStatusList[i].startTime < oldestTime) {
			oldest = i;
			oldestTime = cl_serverStatusList[i].startTime;
		}
	}
	return &cl_serverStatusList[oldest];
}


/*
===================
CL_ServerStatus
===================
*/
int CL_ServerStatus( const char *serverAddress, char *serverStatusString, int maxLen ) {
	int i;
	netadr_t	to;
	serverStatus_t *serverStatus;

	// if no server address then reset all server status requests
	if ( !serverAddress ) {
		for (i = 0; i < MAX_SERVERSTATUSREQUESTS; i++) {
			cl_serverStatusList[i].address.port = 0;
			cl_serverStatusList[i].retrieved = qtrue;
		}
		return qfalse;
	}
	// get the address
	if ( !NET_StringToAdr( serverAddress, &to, NA_UNSPEC ) ) {
		return qfalse;
	}
	serverStatus = CL_GetServerStatus( &to );
	// if no server status string then reset the server status request for this address
	if ( !serverStatusString ) {
		serverStatus->retrieved = qtrue;
		return qfalse;
	}

	// if this server status request has the same address
	if ( NET_CompareAdr( &to, &serverStatus->address) ) {
		// if we received a response for this server status request
		if (!serverStatus->pending) {
			Q_strncpyz(serverStatusString, serverStatus->string, maxLen);
			serverStatus->retrieved = qtrue;
			serverStatus->startTime = 0;
			return qtrue;
		}
		// resend the request regularly
		else if ( Sys_Milliseconds() - serverStatus->startTime > cl_serverStatusResendTime->integer ) {
			serverStatus->print = qfalse;
			serverStatus->pending = qtrue;
			serverStatus->retrieved = qfalse;
			serverStatus->time = 0;
			serverStatus->startTime = Sys_Milliseconds();
			NET_OutOfBandPrint( NS_CLIENT, &to, "getstatus" );
			return qfalse;
		}
	}
	// if retrieved
	else if ( serverStatus->retrieved ) {
		serverStatus->address = to;
		serverStatus->print = qfalse;
		serverStatus->pending = qtrue;
		serverStatus->retrieved = qfalse;
		serverStatus->startTime = Sys_Milliseconds();
		serverStatus->time = 0;
		NET_OutOfBandPrint( NS_CLIENT, &to, "getstatus" );
		return qfalse;
	}
	return qfalse;
}


/*
===================
CL_ServerStatusResponse
===================
*/
static void CL_ServerStatusResponse( const netadr_t *from, msg_t *msg ) {
	const char	*s;
	char	info[MAX_INFO_STRING];
	char	buf[64], *v[2];
	int		i, l, score, ping;
	int		len;
	serverStatus_t *serverStatus;

	serverStatus = NULL;
	for (i = 0; i < MAX_SERVERSTATUSREQUESTS; i++) {
		if ( NET_CompareAdr( from, &cl_serverStatusList[i].address ) ) {
			serverStatus = &cl_serverStatusList[i];
			break;
		}
	}
	// if we didn't request this server status
	if (!serverStatus) {
		return;
	}

	s = MSG_ReadStringLine( msg );

	len = 0;
	Com_sprintf(&serverStatus->string[len], sizeof(serverStatus->string)-len, "%s", s);

	if (serverStatus->print) {
		Com_Printf("Server settings:\n");
		// print cvars
		while (*s) {
			for (i = 0; i < 2 && *s; i++) {
				if (*s == '\\')
					s++;
				l = 0;
				while (*s) {
					info[l++] = *s;
					if (l >= MAX_INFO_STRING-1)
						break;
					s++;
					if (*s == '\\') {
						break;
					}
				}
				info[l] = '\0';
				if (i) {
					Com_Printf("%s\n", info);
				}
				else {
					Com_Printf("%-24s", info);
				}
			}
		}
	}

	len = strlen(serverStatus->string);
	Com_sprintf(&serverStatus->string[len], sizeof(serverStatus->string)-len, "\\");

	if (serverStatus->print) {
		Com_Printf("\nPlayers:\n");
		Com_Printf("num: score: ping: name:\n");
	}
	for (i = 0, s = MSG_ReadStringLine( msg ); *s; s = MSG_ReadStringLine( msg ), i++) {

		len = strlen(serverStatus->string);
		Com_sprintf(&serverStatus->string[len], sizeof(serverStatus->string)-len, "\\%s", s);

		if (serverStatus->print) {
			//score = ping = 0;
			//sscanf(s, "%d %d", &score, &ping);
			Q_strncpyz( buf, s, sizeof (buf) );
			Com_Split( buf, v, 2, ' ' );
			score = atoi( v[0] );
			ping = atoi( v[1] );
			s = strchr(s, ' ');
			if (s)
				s = strchr(s+1, ' ');
			if (s)
				s++;
			else
				s = "unknown";
			Com_Printf("%-2d   %-3d    %-3d   %s\n", i, score, ping, s );
		}
	}
	len = strlen(serverStatus->string);
	Com_sprintf(&serverStatus->string[len], sizeof(serverStatus->string)-len, "\\");

	serverStatus->time = Sys_Milliseconds();
	serverStatus->address = *from;
	serverStatus->pending = qfalse;
	if (serverStatus->print) {
		serverStatus->retrieved = qtrue;
	}
}


/*
==================
CL_LocalServers_f
==================
*/
static void CL_LocalServers_f( void ) {
	char		*message;
	int			i, j, n;
	netadr_t	to;

	Com_Printf( "Scanning for servers on the local network...\n");

	// reset the list, waiting for response
	cls.numlocalservers = 0;
	cls.pingUpdateSource = AS_LOCAL;

	for (i = 0; i < MAX_OTHER_SERVERS; i++) {
		qboolean b = cls.localServers[i].visible;
		Com_Memset(&cls.localServers[i], 0, sizeof(cls.localServers[i]));
		cls.localServers[i].visible = b;
	}
	Com_Memset( &to, 0, sizeof( to ) );

	// The 'xxx' in the message is a challenge that will be echoed back
	// by the server.  We don't care about that here, but master servers
	// can use that to prevent spoofed server responses from invalid ip
	message = "\377\377\377\377getinfo xxx";
	n = (int)strlen( message );

	// send each message twice in case one is dropped
	for ( i = 0 ; i < 2 ; i++ ) {
		// send a broadcast packet on each server port
		// we support multiple server ports so a single machine
		// can nicely run multiple servers
		for ( j = 0 ; j < NUM_SERVER_PORTS ; j++ ) {
			to.port = BigShort( (short)(PORT_SERVER + j) );

			to.type = NA_BROADCAST;
			NET_SendPacket( NS_CLIENT, n, message, &to );
#ifdef USE_IPV6
			to.type = NA_MULTICAST6;
			NET_SendPacket( NS_CLIENT, n, message, &to );
#endif
		}
	}
}


/*
==================
CL_GlobalServers_f
==================
*/
static void CL_GlobalServers_f( void ) {
	netadr_t	to;
	int			count, i, masterNum;
	char		command[1024];
	const char	*masteraddress;

	if ( (count = Cmd_Argc()) < 3 || (masterNum = atoi(Cmd_Argv(1))) < 0 || masterNum > MAX_MASTER_SERVERS )
	{
		Com_Printf( "usage: globalservers <master# 0-%d> [keywords]\n", MAX_MASTER_SERVERS );
		return;
	}

	// request from all master servers
	if ( masterNum == 0 ) {
		int numAddress = 0;

		for ( i = 1; i <= MAX_MASTER_SERVERS; i++ ) {
			sprintf( command, "sv_master%d", i );
			masteraddress = Cvar_VariableString( command );

			if ( !*masteraddress )
				continue;

			numAddress++;

			Com_sprintf( command, sizeof( command ), "globalservers %d %s %s\n", i, Cmd_Argv( 2 ), Cmd_ArgsFrom( 3 ) );
			Cbuf_AddText( command );
		}

		if ( !numAddress ) {
			Com_Printf( "CL_GlobalServers_f: Error: No master server addresses.\n");
		}
		return;
	}

	sprintf( command, "sv_master%d", masterNum );
	masteraddress = Cvar_VariableString( command );

	if ( !*masteraddress )
	{
		Com_Printf( "CL_GlobalServers_f: Error: No master server address given.\n");
		return;
	}

	// reset the list, waiting for response
	// -1 is used to distinguish a "no response"

	i = NET_StringToAdr( masteraddress, &to, NA_UNSPEC );

	if ( i == 0 )
	{
		Com_Printf( "CL_GlobalServers_f: Error: could not resolve address of master %s\n", masteraddress );
		return;
	}
	else if ( i == 2 )
		to.port = BigShort( PORT_MASTER );

	Com_Printf( "Requesting servers from %s (%s)...\n", masteraddress, NET_AdrToStringwPort( &to ) );

	cls.numglobalservers = -1;
	cls.pingUpdateSource = AS_GLOBAL;

	// Use the extended query for IPv6 masters
#ifdef USE_IPV6
	if ( to.type == NA_IP6 || to.type == NA_MULTICAST6 )
	{
		int v4enabled = Cvar_VariableIntegerValue( "net_enabled" ) & NET_ENABLEV4;

		if ( v4enabled )
		{
			Com_sprintf( command, sizeof( command ), "getserversExt %s %s",
				GAMENAME_FOR_MASTER, Cmd_Argv(2) );
		}
		else
		{
			Com_sprintf( command, sizeof( command ), "getserversExt %s %s ipv6",
				GAMENAME_FOR_MASTER, Cmd_Argv(2) );
		}
	}
	else
#endif
		Com_sprintf( command, sizeof( command ), "getservers %s", Cmd_Argv(2) );

	for ( i = 3; i < count; i++ )
	{
		Q_strcat( command, sizeof( command ), " " );
		Q_strcat( command, sizeof( command ), Cmd_Argv( i ) );
	}

	NET_OutOfBandPrint( NS_SERVER, &to, "%s", command );
}


/*
==================
CL_GetPing
==================
*/
void CL_GetPing( int n, char *buf, int buflen, int *pingtime )
{
	const char	*str;
	int		time;
	int		maxPing;

	if (n < 0 || n >= MAX_PINGREQUESTS || !cl_pinglist[n].adr.port)
	{
		// empty or invalid slot
		buf[0]    = '\0';
		*pingtime = 0;
		return;
	}

	str = NET_AdrToStringwPort( &cl_pinglist[n].adr );
	Q_strncpyz( buf, str, buflen );

	time = cl_pinglist[n].time;
	if ( time == 0 )
	{
		// check for timeout
		time = Sys_Milliseconds() - cl_pinglist[n].start;
		maxPing = Cvar_VariableIntegerValue( "cl_maxPing" );
		if ( time < maxPing )
		{
			// not timed out yet
			time = 0;
		}
	}

	CL_SetServerInfoByAddress(&cl_pinglist[n].adr, cl_pinglist[n].info, cl_pinglist[n].time);

	*pingtime = time;
}


/*
==================
CL_GetPingInfo
==================
*/
void CL_GetPingInfo( int n, char *buf, int buflen )
{
	if (n < 0 || n >= MAX_PINGREQUESTS || !cl_pinglist[n].adr.port)
	{
		// empty or invalid slot
		if (buflen)
			buf[0] = '\0';
		return;
	}

	Q_strncpyz( buf, cl_pinglist[n].info, buflen );
}


/*
==================
CL_ClearPing
==================
*/
void CL_ClearPing( int n )
{
	if (n < 0 || n >= MAX_PINGREQUESTS)
		return;

	cl_pinglist[n].adr.port = 0;
}


/*
==================
CL_GetPingQueueCount
==================
*/
int CL_GetPingQueueCount( void )
{
	int		i;
	int		count;
	ping_t*	pingptr;

	count   = 0;
	pingptr = cl_pinglist;

	for (i=0; i<MAX_PINGREQUESTS; i++, pingptr++ ) {
		if (pingptr->adr.port) {
			count++;
		}
	}

	return (count);
}


/*
==================
CL_GetFreePing
==================
*/
static ping_t* CL_GetFreePing( void )
{
	ping_t* pingptr;
	ping_t* best;
	int		oldest;
	int		i;
	int		time, msec;

	msec = Sys_Milliseconds();
	pingptr = cl_pinglist;
	for ( i = 0; i < ARRAY_LEN( cl_pinglist ); i++, pingptr++ )
	{
		// find free ping slot
		if ( pingptr->adr.port )
		{
			if ( pingptr->time == 0 )
			{
				if ( msec - pingptr->start < 500 )
				{
					// still waiting for response
					continue;
				}
			}
			else if ( pingptr->time < 500 )
			{
				// results have not been queried
				continue;
			}
		}

		// clear it
		pingptr->adr.port = 0;
		return pingptr;
	}

	// use oldest entry
	pingptr = cl_pinglist;
	best    = cl_pinglist;
	oldest  = INT_MIN;
	for ( i = 0; i < ARRAY_LEN( cl_pinglist ); i++, pingptr++ )
	{
		// scan for oldest
		time = msec - pingptr->start;
		if ( time > oldest )
		{
			oldest = time;
			best   = pingptr;
		}
	}

	return best;
}


/*
==================
CL_Ping_f
==================
*/
static void CL_Ping_f( void ) {
	netadr_t	to;
	ping_t*		pingptr;
	const char*		server;
	int			argc;
	netadrtype_t	family = NA_UNSPEC;

	argc = Cmd_Argc();

	if ( argc != 2 && argc != 3 ) {
		Com_Printf( "usage: ping [-4|-6] <server>\n");
		return;
	}

	if ( argc == 2 )
		server = Cmd_Argv(1);
	else
	{
		if( !strcmp( Cmd_Argv(1), "-4" ) )
			family = NA_IP;
#ifdef USE_IPV6
		else if( !strcmp( Cmd_Argv(1), "-6" ) )
			family = NA_IP6;
		else
			Com_Printf( "warning: only -4 or -6 as address type understood.\n" );
#else
		else
			Com_Printf( "warning: only -4 as address type understood.\n" );
#endif

		server = Cmd_Argv(2);
	}

	Com_Memset( &to, 0, sizeof( to ) );

	if ( !NET_StringToAdr( server, &to, family ) ) {
		return;
	}

	pingptr = CL_GetFreePing();

	memcpy( &pingptr->adr, &to, sizeof (netadr_t) );
	pingptr->start = Sys_Milliseconds();
	pingptr->time  = 0;

	CL_SetServerInfoByAddress( &pingptr->adr, NULL, 0 );

	NET_OutOfBandPrint( NS_CLIENT, &to, "getinfo xxx" );
}


/*
==================
CL_UpdateVisiblePings_f
==================
*/
qboolean CL_UpdateVisiblePings_f(int source) {
	int			slots, i;
	char		buff[MAX_STRING_CHARS];
	int			pingTime;
	int			max;
	qboolean status = qfalse;

	if (source < 0 || source > AS_GLOBAL) {
		return qfalse;
	}

	cls.pingUpdateSource = source;

	slots = CL_GetPingQueueCount();
	if (slots < MAX_PINGREQUESTS) {
		serverInfo_t *server = NULL;

		switch (source) {
			case AS_LOCAL :
				server = &cls.localServers[0];
				max = cls.numlocalservers;
			break;
			case AS_GLOBAL :
				server = &cls.globalServers[0];
				max = cls.numglobalservers;
			break;
			default:
				return qfalse;
		}
		for (i = 0; i < max; i++) {
			if (server[i].visible) {
				if (server[i].ping == -1) {
					int j;

					if (slots >= MAX_PINGREQUESTS) {
						break;
					}
					for (j = 0; j < MAX_PINGREQUESTS; j++) {
						if (!cl_pinglist[j].adr.port) {
							continue;
						}
						if (NET_CompareAdr( &cl_pinglist[j].adr, &server[i].adr)) {
							// already on the list
							break;
						}
					}
					if (j >= MAX_PINGREQUESTS) {
						status = qtrue;
						for (j = 0; j < MAX_PINGREQUESTS; j++) {
							if (!cl_pinglist[j].adr.port) {
								memcpy(&cl_pinglist[j].adr, &server[i].adr, sizeof(netadr_t));
								cl_pinglist[j].start = Sys_Milliseconds();
								cl_pinglist[j].time = 0;
								NET_OutOfBandPrint(NS_CLIENT, &cl_pinglist[j].adr, "getinfo xxx");
								slots++;
								break;
							}
						}
					}
				}
				// if the server has a ping higher than cl_maxPing or
				// the ping packet got lost
				else if (server[i].ping == 0) {
					// if we are updating global servers
					if (source == AS_GLOBAL) {
						//
						if ( cls.numGlobalServerAddresses > 0 ) {
							// overwrite this server with one from the additional global servers
							cls.numGlobalServerAddresses--;
							CL_InitServerInfo(&server[i], &cls.globalServerAddresses[cls.numGlobalServerAddresses]);
							// NOTE: the server[i].visible flag stays untouched
						}
					}
				}
			}
		}
	}

	if (slots) {
		status = qtrue;
	}
	for (i = 0; i < MAX_PINGREQUESTS; i++) {
		if (!cl_pinglist[i].adr.port) {
			continue;
		}
		CL_GetPing( i, buff, MAX_STRING_CHARS, &pingTime );
		if (pingTime != 0) {
			CL_ClearPing(i);
			status = qtrue;
		}
	}

	return status;
}


/*
==================
CL_ServerStatus_f
==================
*/
static void CL_ServerStatus_f( void ) {
	netadr_t	to, *toptr = NULL;
	const char		*server;
	serverStatus_t *serverStatus;
	int			argc;
	netadrtype_t	family = NA_UNSPEC;

	argc = Cmd_Argc();

	if ( argc != 2 && argc != 3 )
	{
		if (cls.state != CA_ACTIVE || clc.demoplaying)
		{
			Com_Printf( "Not connected to a server.\n" );
#ifdef USE_IPV6
			Com_Printf( "usage: serverstatus [-4|-6] <server>\n" );
#else
			Com_Printf("usage: serverstatus <server>\n");
#endif
			return;
		}

		toptr = &clc.serverAddress;
	}

	if ( !toptr )
	{
		Com_Memset( &to, 0, sizeof( to ) );

		if ( argc == 2 )
			server = Cmd_Argv(1);
		else
		{
			if ( !strcmp( Cmd_Argv(1), "-4" ) )
				family = NA_IP;
#ifdef USE_IPV6
			else if ( !strcmp( Cmd_Argv(1), "-6" ) )
				family = NA_IP6;
			else
				Com_Printf( "warning: only -4 or -6 as address type understood.\n" );
#else
			else
				Com_Printf( "warning: only -4 as address type understood.\n" );
#endif

			server = Cmd_Argv(2);
		}

		toptr = &to;
		if ( !NET_StringToAdr( server, toptr, family ) )
			return;
	}

	NET_OutOfBandPrint( NS_CLIENT, toptr, "getstatus" );

	serverStatus = CL_GetServerStatus( toptr );
	serverStatus->address = *toptr;
	serverStatus->print = qtrue;
	serverStatus->pending = qtrue;
}


/*
==================
CL_ShowIP_f
==================
*/
static void CL_ShowIP_f( void ) {
	Sys_ShowIP();
}
