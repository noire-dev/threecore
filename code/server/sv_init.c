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

#include "server.h"


/*
===============
SV_SendConfigstring

Creates and sends the server command necessary to update the CS index for the
given client
===============
*/
static void SV_SendConfigstring(client_t *client, int index)
{
	int maxChunkSize = MAX_STRING_CHARS - 24;
	int len;

	len = strlen(sv.configstrings[index]);

	if( len >= maxChunkSize ) {
		int		sent = 0;
		int		remaining = len;
		const char	*cmd;
		char	buf[MAX_STRING_CHARS];

		while (remaining > 0 ) {
			if ( sent == 0 ) {
				cmd = "bcs0";
			}
			else if( remaining < maxChunkSize ) {
				cmd = "bcs2";
			}
			else {
				cmd = "bcs1";
			}
			Q_strncpyz( buf, &sv.configstrings[index][sent],
				maxChunkSize );

			SV_SendServerCommand( client, "%s %i \"%s\"", cmd,
				index, buf );

			sent += (maxChunkSize - 1);
			remaining -= (maxChunkSize - 1);
		}
	} else {
		// standard cs, just send it
		SV_SendServerCommand( client, "cs %i \"%s\"", index,
			sv.configstrings[index] );
	}
}

/*
===============
SV_UpdateConfigstrings

Called when a client goes from CS_PRIMED to CS_ACTIVE.  Updates all
Configstring indexes that have changed while the client was in CS_PRIMED
===============
*/
void SV_UpdateConfigstrings(client_t *client)
{
	int index;

	for( index = 0; index < MAX_CONFIGSTRINGS; index++ ) {
		// if the CS hasn't changed since we went to CS_PRIMED, ignore
		if(!client->csUpdated[index])
			continue;

		// do not always send server info to all clients
		if ( index == CS_SERVERINFO && ( SV_GentityNum( client - svs.clients )->r.svFlags & SVF_NOSERVERINFO ) ) {
			continue;
		}

		SV_SendConfigstring(client, index);
		client->csUpdated[index] = qfalse;
	}
}

/*
===============
SV_SetConfigstring

===============
*/
void SV_SetConfigstring (int index, const char *val) {
	int		i;
	client_t	*client;

	if ( index < 0 || index >= MAX_CONFIGSTRINGS ) {
		Com_Error (ERR_DROP, "SV_SetConfigstring: bad index %i", index);
	}

	if ( !val ) {
		val = "";
	}

	// don't bother broadcasting an update if no change
	if ( !strcmp( val, sv.configstrings[ index ] ) ) {
		return;
	}

	// change the string in sv
	Z_Free( sv.configstrings[index] );
	sv.configstrings[index] = CopyString( val );

	// send it to all the clients if we aren't
	// spawning a new server
	if ( sv.state == SS_GAME || sv.restarting ) {

		// send the data to all relevant clients
		for (i = 0, client = svs.clients; i < sv.maxclients; i++, client++) {
			if ( client->state < CS_ACTIVE ) {
				if ( client->state == CS_PRIMED || client->state == CS_CONNECTED ) {
					// track CS_CONNECTED clients as well to optimize gamestate acknowledge after downloading/retransmission
					client->csUpdated[index] = qtrue;
				}
				continue;
			}
			// do not always send server info to all clients
			if ( index == CS_SERVERINFO && ( SV_GentityNum( i )->r.svFlags & SVF_NOSERVERINFO ) ) {
				continue;
			}

			SV_SendConfigstring(client, index);
		}
	}
}


/*
===============
SV_GetConfigstring
===============
*/
void SV_GetConfigstring( int index, char *buffer, int bufferSize ) {
	if ( bufferSize < 1 ) {
		Com_Error( ERR_DROP, "SV_GetConfigstring: bufferSize == %i", bufferSize );
	}
	if ( index < 0 || index >= MAX_CONFIGSTRINGS ) {
		Com_Error (ERR_DROP, "SV_GetConfigstring: bad index %i", index);
	}
	if ( !sv.configstrings[index] ) {
		buffer[0] = '\0';
		return;
	}

	Q_strncpyz( buffer, sv.configstrings[index], bufferSize );
}


/*
===============
SV_SetUserinfo

===============
*/
void SV_SetUserinfo( int index, const char *val ) {
	if ( index < 0 || index >= sv.maxclients ) {
		Com_Error( ERR_DROP, "%s: bad index %i", __func__, index );
	}

	if ( !val ) {
		val = "";
	}

	Q_strncpyz( svs.clients[index].userinfo, val, sizeof( svs.clients[ index ].userinfo ) );
	Q_strncpyz( svs.clients[index].name, Info_ValueForKey( val, "name" ), sizeof(svs.clients[index].name) );
}



/*
===============
SV_GetUserinfo

===============
*/
void SV_GetUserinfo( int index, char *buffer, int bufferSize ) {
	if ( bufferSize < 1 ) {
		Com_Error( ERR_DROP, "%s: bufferSize == %i", __func__, bufferSize );
	}
	if ( index < 0 || index >= sv.maxclients ) {
		Com_Error( ERR_DROP, "%s: bad index %i", __func__, index );
	}
	Q_strncpyz( buffer, svs.clients[ index ].userinfo, bufferSize );
}


/*
================
SV_CreateBaseline

Entity baselines are used to compress non-delta messages
to the clients -- only the fields that differ from the
baseline will be transmitted
================
*/
static void SV_CreateBaseline( void ) {
	sharedEntity_t *ent;
	int				entnum;

	for ( entnum = 0; entnum < sv.num_entities ; entnum++ ) {
		ent = SV_GentityNum( entnum );
		if ( !ent->r.linked ) {
			continue;
		}
		ent->s.number = entnum;

		//
		// take current state as baseline
		//
		sv.svEntities[ entnum ].baseline = ent->s;
		sv.baselineUsed[ entnum ] = 1;
	}
}

/*
===============
SV_SetSnapshotParams
===============
*/
static void SV_SetSnapshotParams( void )
{
	// PACKET_BACKUP frames is just about 6.67MB so use that even on listen servers
	svs.numSnapshotEntities = PACKET_BACKUP * MAX_GENTITIES;
}


/*
===============
SV_AllocClients
===============
*/
static void SV_AllocClients( int count )
{
	svs.clients = Z_TagMalloc( count * sizeof( client_t ), TAG_CLIENTS );
	Com_Memset( svs.clients, 0x0, count * sizeof( client_t ) );
	sv.maxclients = count;
	SV_SetSnapshotParams();
}


/*
===============
SV_Startup

Called when a host starts a map when it wasn't running
one before.  Successive map or map_restart commands will
NOT cause this to be called, unless the game is exited to
the menu system first.
===============
*/
static void SV_Startup( void ) {
	if ( svs.initialized ) {
		Com_Error( ERR_FATAL, "SV_Startup: svs.initialized" );
	}

	SV_AllocClients( MAX_CLIENTS );

	svs.initialized = qtrue;

	Cvar_Set( "sv_running", "1" );

	// Join the ipv6 multicast group now that a map is running so clients can scan for us on the local network.
#ifdef USE_IPV6
	NET_JoinMulticast6();
#endif
}

/*
================
SV_ClearServer
================
*/
static void SV_ClearServer( void ) {
	int i;

	for ( i = 0; i < MAX_CONFIGSTRINGS; i++ ) {
		if ( sv.configstrings[i] ) {
			Z_Free( sv.configstrings[i] );
		}
	}

	i = sv.time;
	Com_Memset( &sv, 0, sizeof( sv ) );
	sv.time = i;
}


/*
================
SV_SpawnServer

Change the server to a new map, taking all connected
clients along with it.
This is NOT called for map_restart
================
*/
void SV_SpawnServer( const char *mapname ) {
	int			i;
	int			checksum;
	qboolean	isBot;
	const char	*p;

	// shut down the existing game if it is running
	SV_ShutdownGameProgs();
	JS_Restart();

	Com_Printf( "------ Server Initialization ------\n" );
	Com_Printf( "Server: %s\n", mapname );

#ifndef DEDICATED
	// if not running a dedicated server CL_MapLoading will connect the client to the server
	// also print some status stuff
	CL_MapLoading();

	// make sure all the client stuff is unloaded
	CL_ShutdownAll();
#endif

	// clear the whole hunk because we're (re)loading the server
	Hunk_Clear();

	// clear collision map data
	CM_ClearMap();

	// init client structures and svs.numSnapshotEntities
	if ( !com_sv_running->integer ) SV_Startup();

	// clear pak references
	FS_ClearPakReferences( 0 );

	// allocate the snapshot entities on the hunk
	svs.snapshotEntities = Hunk_Alloc( sizeof(entityState_t)*svs.numSnapshotEntities );

	// initialize snapshot storage
	SV_InitSnapshotStorage();

	// toggle the server bit so clients can detect that a
	// server has changed
	svs.snapFlagServerBit ^= SNAPFLAG_SERVERCOUNT;

	// try to reset level time if server is empty
	if ( !sv.restartTime ) {
		for ( i = 0; i < sv.maxclients; i++ ) {
			if ( svs.clients[i].state >= CS_CONNECTED ) {
				break;
			}
		}
		if ( i == sv.maxclients ) {
			sv.time = 0;
		}
	}

	for ( i = 0; i < sv.maxclients; i++ ) {
		// save when the server started for each client already connected
		if ( svs.clients[i].state >= CS_CONNECTED) {
			svs.clients[i].oldServerTime = sv.time;
		} else {
			svs.clients[i].oldServerTime = 0;
		}
	}

	// preserve maxclients
	i = sv.maxclients;
	// wipe the entire per-level structure
	SV_ClearServer();
	sv.maxclients = i;
	for ( i = 0; i < MAX_CONFIGSTRINGS; i++ ) {
		sv.configstrings[i] = CopyString("");
	}

	// get a new checksum feed and restart the file system
	srand( Com_Milliseconds() );
	Com_RandomBytes( (byte*)&sv.checksumFeed, sizeof( sv.checksumFeed ) );
	FS_Restart( sv.checksumFeed );

	CM_LoadMap( va( "maps/%s.bsp", mapname ), qfalse, &checksum );

	// set serverinfo visible name
	Cvar_Set( "sv_mapname", mapname );

	// serverid should be different each time
	sv.serverId = com_frameTime;
	sv.restartedServerId = sv.serverId;
	Cvar_Set("sv_serverid", va("%i", sv.serverId));

	// clear physics interaction links
	SV_ClearWorld();

	// media configstring setting should be done during
	// the loading stage, so connected clients don't have
	// to load during actual gameplay
	sv.state = SS_LOADING;

	// load and spawn all other entities
	SV_InitGameProgs();

	// don't allow a map_restart if game is modified
	sv_gametype->modified = qfalse;

	// run a few frames to allow everything to settle
	for ( i = 0; i < 3; i++ ) {
		sv.time += 100;
		VM_Call( gvm, 1, GAME_RUN_FRAME, sv.time );
		SV_BotFrame( sv.time );
	}

	// create a baseline for more efficient communications
	SV_CreateBaseline();

	for ( i = 0; i < sv.maxclients; i++ ) {
		// send the new gamestate to all connected clients
		if ( svs.clients[i].state >= CS_CONNECTED ) {
			const char *denied;

			if ( svs.clients[i].netchan.remoteAddress.type == NA_BOT ) {
				isBot = qtrue;
			}
			else {
				isBot = qfalse;
			}

			// connect the client again
			denied = GVM_ArgPtr( VM_Call( gvm, 3, GAME_CLIENT_CONNECT, i, qfalse, isBot ) );	// firstTime = qfalse
			if ( denied ) {
				// this generally shouldn't happen, because the client
				// was connected before the level change
				SV_DropClient( &svs.clients[i], denied );
			} else {
				if ( !isBot ) {
					svs.clients[i].gamestateAck = GSA_INIT; // resend gamestate, accept first correct serverId
					// when we get the next packet from a connected client,
					// the new gamestate will be sent
					svs.clients[i].state = CS_CONNECTED;
					svs.clients[i].gentity = NULL;
				} else {
					SV_ClientEnterWorld( &svs.clients[i] );
				}
			}
		}
	}

	// run another frame to allow things to look at all the players
	sv.time += 100;
	VM_Call( gvm, 1, GAME_RUN_FRAME, sv.time );
	SV_BotFrame( sv.time );
	svs.time += 100;

	FS_TouchFileInPak( "qvm/cgame.qvm" );
	FS_TouchFileInPak( "qvm/ui.qvm" );

	// the server sends these to the clients so they can figure
	// out which pk3s should be auto-downloaded
	p = FS_ReferencedPakNames();
	if ( FS_ExcludeReference() ) {
		// \fs_excludeReference may mask our current ui/cgame qvms
		FS_TouchFileInPak( "qvm/cgame.qvm" );
		FS_TouchFileInPak( "qvm/ui.qvm" );
		// rebuild referenced paks list
		p = FS_ReferencedPakNames();
	}
	Cvar_Set( "sv_referencedPakNames", p );

	// save systeminfo and serverinfo strings
	SV_SetConfigstring( CS_SYSTEMINFO, Cvar_InfoString( CVAR_SYSTEMINFO, NULL ) );
	cvar_modifiedFlags &= ~CVAR_SYSTEMINFO;

	SV_SetConfigstring( CS_SERVERINFO, Cvar_InfoString( CVAR_SERVERINFO, NULL ) );
	cvar_modifiedFlags &= ~CVAR_SERVERINFO;

	// any media configstring setting now should issue a warning
	// and any configstring changes should be reliably transmitted
	// to all clients
	sv.state = SS_GAME;

	// send a heartbeat now so the master will get up to date info
	SV_Heartbeat_f();

	Hunk_SetMark();

	Com_Printf ("-----------------------------------\n");

	// suppress hitch warning
	Com_FrameInit();
}


/*
===============
SV_Init

Only called at main exe startup, not for each game
===============
*/
void SV_Init( void )
{
	int index;

	SV_AddOperatorCommands();

	// serverinfo vars
	sv_gametype = Cvar_Get ("g_gametype", "0", CVAR_SERVERINFO | CVAR_LATCH );
	sv_mapname = Cvar_Get ("sv_mapname", "", CVAR_SERVERINFO );
	sv_privateClients = Cvar_Get( "sv_privateClients", "0", 0 );
	sv_hostname = Cvar_Get ("sv_hostname", "OpenSandbox", CVAR_SERVERINFO | CVAR_ARCHIVE );
	sv_maxclientsPerIP = Cvar_Get( "sv_maxclientsPerIP", "3", CVAR_ARCHIVE );
	sv_minRate = Cvar_Get( "sv_minRate", "0", CVAR_ARCHIVE );
	sv_maxRate = Cvar_Get( "sv_maxRate", "0", CVAR_ARCHIVE );
	sv_dlRate = Cvar_Get( "sv_dlRate", "100", CVAR_ARCHIVE );
	sv_floodProtect = Cvar_Get( "sv_floodProtect", "1", CVAR_ARCHIVE | CVAR_SERVERINFO );
	Cvar_Get( "sv_cheats", "0", CVAR_SYSTEMINFO);
	sv_serverid = Cvar_Get( "sv_serverid", "0", CVAR_SYSTEMINFO );
	sv_referencedPakNames = Cvar_Get( "sv_referencedPakNames", "", CVAR_SYSTEMINFO );
	sv_rconPassword = Cvar_Get ("rconPassword", "", 0 );
	sv_privatePassword = Cvar_Get ("sv_privatePassword", "", 0 );
	sv_fps = Cvar_Get ("sv_fps", "60", 0 );
	sv_timeout = Cvar_Get( "sv_timeout", "999999", 0 );
	sv_zombietime = Cvar_Get( "sv_zombietime", "2", 0 );
	Cvar_Get ("nextmap", "", 0 );
	sv_allowDownload = Cvar_Get ("sv_allowDownload", "1", CVAR_SERVERINFO);
	Cvar_Get ("sv_dlURL", "", CVAR_SERVERINFO | CVAR_ARCHIVE);

	for ( index = 0; index < MAX_MASTER_SERVERS; index++ )
		sv_master[ index ] = Cvar_Get( va( "sv_master%d", index + 1 ), "", CVAR_ARCHIVE );

	sv_reconnectlimit = Cvar_Get( "sv_reconnectlimit", "3", 0 );
	sv_padPackets = Cvar_Get( "sv_padPackets", "0", 0 );
	sv_lanForceRate = Cvar_Get( "sv_lanForceRate", "1", CVAR_ARCHIVE );
	sv_anticheatengine = Cvar_Get( "sv_anticheatengine", "0", CVAR_ARCHIVE | CVAR_SERVERINFO );
	sv_ace_wallhack = Cvar_Get( "sv_ace_wallhack", "2", CVAR_ARCHIVE );
	sv_filter = Cvar_Get( "sv_filter", "filter.txt", CVAR_ARCHIVE );

	SV_BotInitCvars();
	SV_BotInitBotLib();

	Cvar_SetGroup( sv_lanForceRate, CVG_SERVER );
	Cvar_SetGroup( sv_minRate, CVG_SERVER );
	Cvar_SetGroup( sv_maxRate, CVG_SERVER );
	Cvar_SetGroup( sv_fps, CVG_SERVER );

	SV_TrackCvarChanges();

	SV_InitChallenger();
}


/*
==================
SV_FinalMessage

Used by SV_Shutdown to send a final message to all
connected clients before the server goes down.  The messages are sent immediately,
not just stuck on the outgoing message list, because the server is going
to totally exit after returning from this function.
==================
*/
static void SV_FinalMessage( const char *message ) {
	int			i, j;
	client_t	*cl;

	// send it twice, ignoring rate
	for ( j = 0 ; j < 2 ; j++ ) {
		for ( i = 0, cl = svs.clients; i < sv.maxclients; i++, cl++) {
			if (cl->state >= CS_CONNECTED ) {
				// don't send a disconnect to a local client
				if ( cl->netchan.remoteAddress.type != NA_LOOPBACK ) {
					SV_SendServerCommand( cl, "print \"%s\n\"\n", message );
					SV_SendServerCommand( cl, "disconnect \"%s\"", message );
				}
				// force a snapshot to be sent
				cl->lastSnapshotTime = svs.time - 9999; // generate a snapshot immediately
				cl->state = CS_ZOMBIE; // skip delta generation
				SV_SendClientSnapshot( cl );
			}
		}
	}

	NET_FlushPacketQueue( 99999 );
}


/*
================
SV_Shutdown

Called when each game quits,
before Sys_Quit or Sys_Error
================
*/
void SV_Shutdown( const char *finalmsg ) {
	if ( !com_sv_running || !com_sv_running->integer ) {
		return;
	}

	Com_Printf( "Server shutdown... (%s)\n", finalmsg );

#ifdef USE_IPV6
	NET_LeaveMulticast6();
#endif

	if ( svs.clients && !com_errorEntered ) {
		SV_FinalMessage( finalmsg );
	}

	SV_MasterShutdown();
	SV_ShutdownGameProgs();
	SV_InitChallenger();

	// free current level
	SV_ClearServer();

	// free server static data
	if ( svs.clients ) {
		int index;

		for ( index = 0; index < sv.maxclients; index++ )
			SV_FreeClient( &svs.clients[ index ] );

		Z_Free( svs.clients );
	}
	Com_Memset( &svs, 0, sizeof( svs ) );
	sv.time = 0;

	Cvar_Set( "sv_running", "0" );
	Cvar_Set( "sv_referencedPakNames", "" );
	Cvar_Set( "sv_serverid", "0" );
}
