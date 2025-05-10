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
===============================================================================

OPERATOR CONSOLE ONLY COMMANDS

These commands can only be entered from stdin or by a remote operator datagram
===============================================================================
*/


/*
==================
SV_GetPlayerByHandle

Returns the player with player id or name from Cmd_Argv(1)
==================
*/
client_t *SV_GetPlayerByHandle( void ) {
	client_t	*cl;
	int			i;
	const char		*s;
	char		cleanName[ MAX_NAME_LENGTH ];

	// make sure server is running
	if ( !com_sv_running->integer ) {
		return NULL;
	}

	if ( Cmd_Argc() < 2 ) {
		Com_Printf( "No player specified.\n" );
		return NULL;
	}

	s = Cmd_Argv(1);

	// Check whether this is a numeric player handle
	for(i = 0; s[i] >= '0' && s[i] <= '9'; i++);
	
	if(!s[i])
	{
		int plid = atoi(s);

		// Check for numeric playerid match
		if(plid >= 0 && plid < sv.maxclients)
		{
			cl = &svs.clients[plid];
			
			if (cl->state >= CS_CONNECTED)
				return cl;
		}
	}

	// check for a name match
	for ( i = 0, cl = svs.clients; i < sv.maxclients; i++, cl++ ) {
		if ( cl->state < CS_CONNECTED ) {
			continue;
		}
		if ( !Q_stricmp( cl->name, s ) ) {
			return cl;
		}

		Q_strncpyz( cleanName, cl->name, sizeof(cleanName) );
		Q_CleanStr( cleanName );
		if ( !Q_stricmp( cleanName, s ) ) {
			return cl;
		}
	}

	Com_Printf( "Player %s is not on the server\n", s );

	return NULL;
}


/*
==================
SV_GetPlayerByNum

Returns the player with idnum from Cmd_Argv(1)
==================
*/
static client_t *SV_GetPlayerByNum( void ) {
	client_t	*cl;
	int			i;
	int			idnum;
	const char		*s;

	// make sure server is running
	if ( !com_sv_running->integer ) {
		return NULL;
	}

	if ( Cmd_Argc() < 2 ) {
		Com_Printf( "No player specified.\n" );
		return NULL;
	}

	s = Cmd_Argv(1);

	for (i = 0; s[i]; i++) {
		if (s[i] < '0' || s[i] > '9') {
			Com_Printf( "Bad slot number: %s\n", s);
			return NULL;
		}
	}
	idnum = atoi( s );
	if ( idnum < 0 || idnum >= sv.maxclients ) {
		Com_Printf( "Bad client slot: %i\n", idnum );
		return NULL;
	}

	cl = &svs.clients[idnum];
	if ( cl->state < CS_CONNECTED ) {
		Com_Printf( "Client %i is not active\n", idnum );
		return NULL;
	}
	return cl;
}

//=========================================================


/*
==================
SV_Map_f

Restart the server on a different map
==================
*/
static void SV_Map_f( void ) {
	const char		*cmd;
	const char		*map;
	qboolean	killBots, cheat;
	char		expanded[MAX_QPATH];
	char		mapname[MAX_QPATH];
	int			len;

	map = Cmd_Argv(1);
	if ( !map || !*map ) {
		return;
	}

	// make sure the level exists before trying to change, so that
	// a typo at the server console won't end the game
	Com_sprintf( expanded, sizeof( expanded ), "maps/%s.bsp", map );

	len = FS_FOpenFileRead( expanded, NULL, qfalse );
	if ( len == -1 ) {
		Com_Printf( "Can't find map %s\n", expanded );
		return;
	}

	Cbuf_AddText( "exec maps/default.cfg \n" );			//load default map script on server
	Cbuf_AddText( va("exec maps/%s.cfg \n", map) );		//load map script on server
	Cvar_Set("cl_changeqvm", map);						//load map fs on server

	// force latched values to get set
	Cvar_Get ("g_gametype", "0", CVAR_SERVERINFO | CVAR_USERINFO | CVAR_LATCH );

	cmd = Cmd_Argv(0);
	if( Q_stricmpn( cmd, "sp", 2 ) == 0 ) {
		Cvar_SetIntegerValue( "g_gametype", GT_SINGLE_PLAYER );
		Cvar_Set( "g_doWarmup", "0" );
		cmd += 2;
		if (!Q_stricmp( cmd, "devmap" ) ) {
			cheat = qtrue;
		} else {
			cheat = qfalse;
		}
		killBots = qtrue;
	}
	else {
		if ( !Q_stricmp( cmd, "devmap" ) ) {
			cheat = qtrue;
		} else {
			cheat = qfalse;
		}
		if( sv_gametype->integer == GT_SINGLE_PLAYER ) {
			Cvar_SetIntegerValue( "g_gametype", GT_FFA );
			killBots = qtrue;
		} else {
			killBots = qfalse;
		}
	}

	// save the map name here cause on a map restart we reload the q3config.cfg
	// and thus nuke the arguments of the map command
	Q_strncpyz(mapname, map, sizeof(mapname));

	// start up the map
	SV_SpawnServer( mapname, killBots );

	// set the cheat value
	// if the level was started with "map <levelname>", then
	// cheats will not be allowed.  If started with "devmap <levelname>"
	// then cheats will be allowed
	if ( cheat ) {
		Cvar_Set( "sv_cheats", "1" );
	} else {
		Cvar_Set( "sv_cheats", "0" );
	}
}


/*
================
SV_MapRestart_f

Completely restarts a level, but doesn't send a new gamestate to the clients.
This allows fair starts with variable load times.
================
*/
static void SV_MapRestart_f( void ) {
	int			i;
	client_t	*client;
	const char		*denied;
	qboolean	isBot;
	int			delay;

	// make sure we aren't restarting twice in the same frame
	if ( com_frameTime == sv.restartedServerId ) {
		return;
	}

	// make sure server is running
	if ( !com_sv_running->integer ) {
		Com_Printf( "Server is not running.\n" );
		return;
	}

	if ( sv.restartTime != 0 ) {
		return;
	}

	if ( Cmd_Argc() > 1 ) {
		delay = atoi( Cmd_Argv(1) );
	} else {
		delay = 5;
	}

	if ( delay != 0 && Cvar_VariableIntegerValue( "g_doWarmup" ) == 0 ) {
		sv.restartTime = sv.time + delay * 1000;
		if ( sv.restartTime == 0 ) {
			sv.restartTime = 1;
		}
		SV_SetConfigstring( CS_WARMUP, va( "%i", sv.restartTime ) );
		return;
	}

	// check for changes in variables that can't just be restarted
	// check for maxclients change
	if ( sv_gametype->modified ) {
		char	mapname[MAX_QPATH];

		// restart the map the slow way
		Q_strncpyz( mapname, Cvar_VariableString( "mapname" ), sizeof( mapname ) );

		SV_SpawnServer( mapname, qfalse );
		return;
	}

	// toggle the server bit so clients can detect that a
	// map_restart has happened
	svs.snapFlagServerBit ^= SNAPFLAG_SERVERCOUNT;

	// generate a new restartedServerid
	sv.restartedServerId = com_frameTime;

	// if a map_restart occurs while a client is changing maps, we need
	// to give them the correct time so that when they finish loading
	// they don't violate the backwards time check in cl_cgame.c
	for ( i = 0; i < sv.maxclients; i++ ) {
		if ( svs.clients[i].state == CS_PRIMED ) {
			svs.clients[i].oldServerTime = sv.restartTime;
		}
	}

	// reset all the vm data in place without changing memory allocation
	// note that we do NOT set sv.state = SS_LOADING, so configstrings that
	// had been changed from their default values will generate broadcast updates
	sv.state = SS_LOADING;
	sv.restarting = qtrue;

	// make sure that level time is not zero
	//sv.time = sv.time ? sv.time : 8;

	SV_RestartGameProgs();

	// run a few frames to allow everything to settle
	for ( i = 0; i < 3; i++ )
	{
		sv.time += 100;
		VM_Call( gvm, 1, GAME_RUN_FRAME, sv.time );
	}

	sv.state = SS_GAME;
	sv.restarting = qfalse;

	// connect and begin all the clients
	for ( i = 0; i < sv.maxclients; i++ ) {
		client = &svs.clients[i];

		// send the new gamestate to all connected clients
		if ( client->state < CS_CONNECTED ) {
			continue;
		}

		if ( client->netchan.remoteAddress.type == NA_BOT ) {
			isBot = qtrue;
		} else {
			isBot = qfalse;
		}

		// add the map_restart command
		SV_AddServerCommand( client, "map_restart\n" );

		// connect the client again, without the firstTime flag
		denied = GVM_ArgPtr( VM_Call( gvm, 3, GAME_CLIENT_CONNECT, i, qfalse, isBot ) );
		if ( denied ) {
			// this generally shouldn't happen, because the client
			// was connected before the level change
			SV_DropClient( client, denied );
			Com_Printf( "SV_MapRestart_f(%d): dropped client %i - denied!\n", delay, i );
			continue;
		}

		if ( client->state == CS_ACTIVE ) {
			SV_ClientEnterWorld( client );
		}
	}

	// run another frame to allow things to look at all the players
	sv.time += 100;
	VM_Call( gvm, 1, GAME_RUN_FRAME, sv.time );
	svs.time += 100;

	for ( i = 0; i < sv.maxclients; i++ ) {
		client = &svs.clients[i];
		if ( client->state >= CS_PRIMED ) {
			// accept usercmds starting from current server time only
			// to emulate original behavior which dropped pre-restart commands via serverid check
			Com_Memset( &client->lastUsercmd, 0x0, sizeof( client->lastUsercmd ) );
			client->lastUsercmd.serverTime = sv.time - 1;
		}
	}
}


/*
==================
SV_Kick_f

Kick a user off of the server  FIXME: move to game
==================
*/
static void SV_Kick_f( void ) {
	client_t	*cl;
	int			i;

	// make sure server is running
	if ( !com_sv_running->integer ) {
		Com_Printf( "Server is not running.\n" );
		return;
	}

	if ( Cmd_Argc() != 2 ) {
		Com_Printf ("Usage: kick <player name>\nkick all = kick everyone\nkick allbots = kick all bots\n");
		return;
	}

	cl = SV_GetPlayerByHandle();
	if ( !cl ) {
		if ( !Q_stricmp( Cmd_Argv( 1 ), "all" ) ) {
			for ( i = 0, cl = svs.clients; i < sv.maxclients; i++, cl++ ) {
				if ( cl->state < CS_CONNECTED ) {
					continue;
				}
				if ( cl->netchan.remoteAddress.type == NA_LOOPBACK ) {
					continue;
				}
				SV_DropClient( cl, "was kicked" );
				cl->lastPacketTime = svs.time;	// in case there is a funny zombie
			}
		}
		else if ( !Q_stricmp( Cmd_Argv( 1 ), "allbots" ) ) {
			for ( i = 0, cl = svs.clients; i < sv.maxclients; i++, cl++ ) {
				if ( cl->state < CS_CONNECTED ) {
					continue;
				}
				if ( cl->netchan.remoteAddress.type != NA_BOT ) {
					continue;
				}
				SV_DropClient( cl, "was kicked" );
				cl->lastPacketTime = svs.time;	// in case there is a funny zombie
			}
		}
		return;
	}
	if ( cl->netchan.remoteAddress.type == NA_LOOPBACK ) {
		Com_Printf( "Cannot kick host player\n" );
		return;
	}

	SV_DropClient( cl, "was kicked" );
	cl->lastPacketTime = svs.time;	// in case there is a funny zombie
}

/*
==================
SV_KickBots_f

Kick all bots off of the server
==================
*/
static void SV_KickBots_f( void ) {
	client_t	*cl;
	int			i;

	// make sure server is running
	if ( !com_sv_running->integer ) {
		Com_Printf("Server is not running.\n");
		return;
	}

	for( i = 0, cl = svs.clients; i < sv.maxclients; i++, cl++ ) {
		if ( cl->state < CS_CONNECTED ) {
			continue;
		}

		if ( cl->netchan.remoteAddress.type != NA_BOT ) {
			continue;
		}

		SV_DropClient( cl, "was kicked" );
		cl->lastPacketTime = svs.time; // in case there is a funny zombie
	}
}
/*
==================
SV_KickAll_f

Kick all users off of the server
==================
*/
static void SV_KickAll_f( void ) {
	client_t *cl;
	int i;

	// make sure server is running
	if ( !com_sv_running->integer ) {
		Com_Printf( "Server is not running.\n" );
		return;
	}

	for( i = 0, cl = svs.clients; i < sv.maxclients; i++, cl++ ) {
		if ( cl->state < CS_CONNECTED ) {
			continue;
		}

		if ( cl->netchan.remoteAddress.type == NA_LOOPBACK ) {
			continue;
		}

		SV_DropClient( cl, "was kicked" );
		cl->lastPacketTime = svs.time; // in case there is a funny zombie
	}
}

/*
==================
SV_KickNum_f

Kick a user off of the server
==================
*/
static void SV_KickNum_f( void ) {
	client_t	*cl;

	// make sure server is running
	if ( !com_sv_running->integer ) {
		Com_Printf( "Server is not running.\n" );
		return;
	}

	if ( Cmd_Argc() != 2 ) {
		Com_Printf ("Usage: %s <client number>\n", Cmd_Argv(0));
		return;
	}

	cl = SV_GetPlayerByNum();
	if ( !cl ) {
		return;
	}
	if ( cl->netchan.remoteAddress.type == NA_LOOPBACK ) {
		Com_Printf("Cannot kick host player\n");
		return;
	}

	SV_DropClient( cl, "was kicked" );
	cl->lastPacketTime = svs.time;	// in case there is a funny zombie
}

/*
** SV_Strlen -- skips color escape codes
*/
int SV_Strlen( const char *str ) {
	const char *s = str;
	int count = 0;

	while ( *s ) {
		if ( Q_IsColorString( s ) ) {
			s += 2;
		} else {
			count++;
			s++;
		}
	}

	return count;
}


/*
================
SV_Status_f
================
*/
static void SV_Status_f( void ) {
	int i, j, l;
	const client_t *cl;
	const playerState_t *ps;
	const char *s;
	int max_namelength;
	int max_addrlength;
	char names[ MAX_CLIENTS * MAX_NAME_LENGTH ], *np[ MAX_CLIENTS ], nl[ MAX_CLIENTS ], *nc;
	char addrs[ MAX_CLIENTS * 48 ], *ap[ MAX_CLIENTS ], al[ MAX_CLIENTS ], *ac;

	// make sure server is running
	if ( !com_sv_running->integer ) {
		Com_Printf( "Server is not running.\n" );
		return;
	}

	max_namelength = 4; // strlen( "name" )
	max_addrlength = 7; // strlen( "address" )

	nc = names; *nc = '\0';
	ac = addrs; *ac = '\0';

	Com_Memset( np, 0, sizeof( np ) );
	Com_Memset( nl, 0, sizeof( nl ) );

	Com_Memset( ap, 0, sizeof( ap ) );
	Com_Memset( al, 0, sizeof( al ) );

	// first pass: save and determine max.lengths of name/address fields
	for ( i = 0, cl = svs.clients; i < sv.maxclients; i++, cl++ )
	{
		if ( cl->state == CS_FREE )
			continue;

		l = strlen( cl->name ) + 1;
		strcpy( nc, cl->name );
		np[ i ] = nc; nc += l;			// name pointer in name buffer
		nl[ i ] = SV_Strlen( cl->name );// name length without color sequences
		if ( nl[ i ] > max_namelength )
			max_namelength = nl[ i ];

		s = NET_AdrToString( &cl->netchan.remoteAddress );
		l = strlen( s ) + 1;
		strcpy( ac, s );
		ap[ i ] = ac; ac += l;			// address pointer in address buffer
		al[ i ] = l - 1;				// address length
		if ( al[ i ] > max_addrlength )
			max_addrlength = al[ i ];
	}

	Com_Printf( "map: %s\n", sv_mapname->string );

	Com_Printf( "cl score ping name" );
	for ( i = 0; i < max_namelength - 4; i++ )
		Com_Printf( " " );
	Com_Printf( " address" );
	for ( i = 0; i < max_addrlength - 7; i++ )
		Com_Printf( " " );
	Com_Printf( " rate\n" );

	Com_Printf( "-- ----- ---- " );
	for ( i = 0; i < max_namelength; i++ )
		Com_Printf( "-" );
	Com_Printf( " " );
	for ( i = 0; i < max_addrlength; i++ )
		Com_Printf( "-" );
	Com_Printf( " -----\n" );

	for ( i = 0, cl = svs.clients; i < sv.maxclients; i++, cl++ )
	{
		if ( cl->state == CS_FREE )
			continue;

		Com_Printf( "%2i ", i ); // id
		ps = SV_GameClientNum( i );
		Com_Printf( "%5i ", ps->persistant[PERS_SCORE] );

		// ping/status
		if ( cl->state == CS_PRIMED )
			Com_Printf( " PRM " );
		else if ( cl->state == CS_CONNECTED )
			Com_Printf( " CON " );
		else if ( cl->state == CS_ZOMBIE )
			Com_Printf( " ZMB " );
		else
			Com_Printf( "%4i ", cl->ping < 999 ? cl->ping : 999 );
	
		// variable-length name field
		s = np[ i ];
		Com_Printf( "%s", s );
		l = max_namelength - nl[ i ];
		for ( j = 0; j < l; j++ )
			Com_Printf( " " );

		// variable-length address field
		s = ap[ i ];
		Com_Printf( S_COLOR_WHITE " %s", s );
		l = max_addrlength - al[ i ];
		for ( j = 0; j < l; j++ )
			Com_Printf( " " );

		// rate
		Com_Printf( " %5i\n", cl->rate );
	}

	Com_Printf( "\n" );
}


/*
==================
SV_ConSay_f
==================
*/
static void SV_ConSay_f( void ) {
	char	*p;
	char	text[MAX_STRING_CHARS];
	int		len;

	// make sure server is running
	if ( !com_sv_running->integer ) {
		Com_Printf( "Server is not running.\n" );
		return;
	}

	if ( Cmd_Argc () < 2 ) {
		return;
	}

	p = Cmd_ArgsFrom( 1 );
	len = (int)strlen( p );

	if ( len > 1000 ) {
		return;
	}

	if ( *p == '"' ) {
		p[len-1] = '\0';
		p++;
	}

	strcpy( text, "console: " );
	strcat( text, p );

	SV_SendServerCommand( NULL, "chat \"%s\"", text );
}


/*
==================
SV_ConTell_f
==================
*/
static void SV_ConTell_f( void ) {
	char	*p;
	char	text[MAX_STRING_CHARS];
	client_t	*cl;
	int		len;

	// make sure server is running
	if ( !com_sv_running->integer ) {
		Com_Printf( "Server is not running.\n" );
		return;
	}

	if ( Cmd_Argc() < 3 ) {
		Com_Printf( "Usage: tell <client number> <text>\n" );
		return;
	}

	cl = SV_GetPlayerByNum();
	if ( !cl ) {
		return;
	}

	p = Cmd_ArgsFrom( 2 );
	len = (int)strlen( p );

	if ( len > 1000 ) {
		return;
	}

	if ( *p == '"' ) {
		p[len-1] = '\0';
		p++;
	}

	strcpy( text, S_COLOR_MAGENTA "console: " );
	strcat( text, p );

	Com_Printf( "%s\n", text );
	SV_SendServerCommand( cl, "chat \"%s\"", text );
}


/*
==================
SV_Heartbeat_f

Also called by SV_DropClient, SV_DirectConnect, and SV_SpawnServer
==================
*/
void SV_Heartbeat_f( void ) {
	svs.nextHeartbeatTime = svs.time;
}


/*
===========
SV_Serverinfo_f

Examine the serverinfo string
===========
*/
static void SV_Serverinfo_f( void ) {
	const char *info;

	// make sure server is running
	if ( !com_sv_running->integer ) {
		Com_Printf( "Server is not running.\n" );
		return;
	}

	Com_Printf ("Server info settings:\n");
	info = sv.configstrings[ CS_SERVERINFO ];
	if ( info ) {
		Info_Print( info );
	}
}


/*
===========
SV_Systeminfo_f

Examine the systeminfo string
===========
*/
static void SV_Systeminfo_f( void ) {
	const char *info;
	// make sure server is running
	if ( !com_sv_running->integer ) {
		Com_Printf( "Server is not running.\n" );
		return;
	}
	Com_Printf( "System info settings:\n" );
	info = sv.configstrings[ CS_SYSTEMINFO ];
	if ( info ) {
		Info_Print( info );
	}
}


/*
===========
SV_DumpUser_f

Examine all a users info strings
===========
*/
static void SV_DumpUser_f( void ) {
	client_t	*cl;

	// make sure server is running
	if ( !com_sv_running->integer ) {
		Com_Printf( "Server is not running.\n" );
		return;
	}

	if ( Cmd_Argc() != 2 ) {
		Com_Printf ("Usage: dumpuser <userid>\n");
		return;
	}

	cl = SV_GetPlayerByHandle();
	if ( !cl ) {
		return;
	}

	Com_Printf( "userinfo\n" );
	Com_Printf( "--------\n" );
	Info_Print( cl->userinfo );
}


/*
=================
SV_KillServer
=================
*/
static void SV_KillServer_f( void ) {
	SV_Shutdown( "killserver" );
}


/*
=================
SV_Locations
=================
*/
static void SV_Locations_f( void ) {

	// make sure server is running
	if ( !com_sv_running->integer ) {
		Com_Printf( "Server is not running.\n" );
		return;
	}

	if ( !sv_clientTLD->integer ) {
		Com_Printf( "Disabled on this server.\n" );
		return;
	}

	SV_PrintLocations_f( NULL );
}

/*
==================
SV_CompleteMapName
==================
*/
static void SV_CompleteMapName( const char *args, int argNum ) {
	if ( argNum == 2 ) 	{
		Field_CompleteFilename( "maps", "bsp", qtrue, FS_MATCH_ANY | FS_MATCH_STICK );
	}
}

/*
==================
SV_ConvertOBJ
==================
*/
#define OBJ_TO_MD3_SCALE 3200 //for BlockBench sizes

// Rotation for BlockBench orient
#define MODEL_ROTATE_X 90
#define MODEL_ROTATE_Y 0
#define MODEL_ROTATE_Z 90

typedef struct {
    char newmtl[MAX_QPATH];
    char map_Kd[MAX_QPATH];
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
    Q_strncpyz(mtlFilename, name, sizeof(mtlFilename));
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
            sscanf(p+7, "%63s", materials[currentMaterial].newmtl);
            materials[currentMaterial].map_Kd[0] = '\0';
        }
        else if (strncmp(p, "map_Kd ", 7) == 0 && currentMaterial >= 0) {
            sscanf(p+7, "%63s", materials[currentMaterial].map_Kd);
            
            char *baseName = strrchr(materials[currentMaterial].map_Kd, '/');
            if (!baseName) baseName = strrchr(materials[currentMaterial].map_Kd, '\\');
            if (baseName) {
                Q_strncpyz(materials[currentMaterial].map_Kd, baseName+1, 
                          sizeof(materials[currentMaterial].map_Kd));
            }
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
    VectorSet(frame.bounds[0], -25, -25, -25);
    VectorSet(frame.bounds[1], 25, 25, 25);
    frame.radius = 50.0f * OBJ_TO_MD3_SCALE;
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

void SV_ConvertOBJ(void) {
    const char *name = Cmd_Argv(1);
    if (!name[0]) {
        Com_Printf("Usage: convertOBJ <modelname> (without .obj)\n");
        return;
    }

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

    vec3_t vertices[MD3_MAX_VERTS];
    vec2_t texCoords[MD3_MAX_VERTS];
    vec3_t normals[MD3_MAX_VERTS];
    int vCount = 0, vtCount = 0, vnCount = 0;
    
    md3SurfaceData_t surfaces[MD3_MAX_SURFACES];
    int numSurfaces = 0;

    ParseOBJ(objData, vertices, texCoords, normals, surfaces, &numSurfaces, 
            &vCount, &vtCount, &vnCount, materials, numMaterials, shaders, &numShaders);

    WriteMD3(name, surfaces, numSurfaces, shaders, numShaders, materials, numMaterials);
    FreeSurfaceData(surfaces, numSurfaces);
    FS_FreeFile(objData);
}

/*
==================
SV_AddOperatorCommands
==================
*/
void SV_AddOperatorCommands( void ) {
	static qboolean	initialized;

	if ( initialized ) {
		return;
	}
	initialized = qtrue;

	Cmd_AddCommand ("heartbeat", SV_Heartbeat_f);
	Cmd_AddCommand ("kick", SV_Kick_f);
	Cmd_AddCommand ("kickbots", SV_KickBots_f);
	Cmd_AddCommand ("kickall", SV_KickAll_f);
	Cmd_AddCommand ("kicknum", SV_KickNum_f);
	Cmd_AddCommand ("clientkick", SV_KickNum_f); // Legacy command
	Cmd_AddCommand ("status", SV_Status_f);
	Cmd_AddCommand ("dumpuser", SV_DumpUser_f);
	Cmd_AddCommand ("map_restart", SV_MapRestart_f);
	Cmd_AddCommand ("sectorlist", SV_SectorList_f);
	Cmd_AddCommand ("map", SV_Map_f);
	Cmd_SetCommandCompletionFunc( "map", SV_CompleteMapName );
	Cmd_AddCommand ("devmap", SV_Map_f);
	Cmd_SetCommandCompletionFunc( "devmap", SV_CompleteMapName );
	Cmd_AddCommand ("spmap", SV_Map_f);
	Cmd_SetCommandCompletionFunc( "spmap", SV_CompleteMapName );
	Cmd_AddCommand ("spdevmap", SV_Map_f);
	Cmd_SetCommandCompletionFunc( "spdevmap", SV_CompleteMapName );
	Cmd_AddCommand ("killserver", SV_KillServer_f);
	Cmd_AddCommand( "filter", SV_AddFilter_f );
	Cmd_AddCommand( "filtercmd", SV_AddFilterCmd_f );
	Cmd_AddCommand( "convertOBJ", SV_ConvertOBJ );
}

void SV_AddDedicatedCommands( void )
{
	Cmd_AddCommand( "serverinfo", SV_Serverinfo_f );
	Cmd_AddCommand( "systeminfo", SV_Systeminfo_f );
	Cmd_AddCommand( "tell", SV_ConTell_f );
	Cmd_AddCommand( "say", SV_ConSay_f );
	Cmd_AddCommand( "locations", SV_Locations_f );
}


void SV_RemoveDedicatedCommands( void )
{
	Cmd_RemoveCommand( "serverinfo" );
	Cmd_RemoveCommand( "systeminfo" );
	Cmd_RemoveCommand( "tell" );
	Cmd_RemoveCommand( "say" );
	Cmd_RemoveCommand( "locations" );
}
