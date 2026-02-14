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
// sv_bot.c

#include "server.h"
#include "../botlib/botlib.h"

typedef struct bot_debugpoly_s
{
	int inuse;
	int color;
	int numPoints;
	vec3_t points[128];
} bot_debugpoly_t;

static bot_debugpoly_t *debugpolygons;
static int bot_maxdebugpolys;

extern botlib_export_t	*botlib_export;
int	bot_enable;

int SV_BotAllocateClient( void ) {
	int			i;
	client_t	*cl;

	// find a client slot
	for ( i = 0, cl = svs.clients; i < sv.maxclients; i++, cl++ ) {
		if ( cl->state == CS_FREE ) {
			break;
		}
	}

	if ( i == sv.maxclients ) {
		return -1;
	}

	cl->gentity = SV_GentityNum( i );
	cl->gentity->s.number = i;
	cl->state = CS_ACTIVE;
	cl->lastPacketTime = svs.time;
	cl->snapshotMsec = 1000 / sv_fps->integer;
	cl->netchan.remoteAddress.type = NA_BOT;
	cl->rate = 0;
	
	return i;
}

void SV_BotFreeClient( int clientNum ) {
	client_t	*cl;

	if ( (unsigned) clientNum >= sv.maxclients ) {
		Com_Error( ERR_DROP, "SV_BotFreeClient: bad clientNum: %i", clientNum );
	}

	cl = &svs.clients[clientNum];
	cl->state = CS_FREE;
	cl->name[0] = '\0';
	if ( cl->gentity ) {
		cl->gentity->r.svFlags &= ~SVF_BOT;
	}
}

void SV_BotFrame( int time ) {
	if (!gvm) return;
	VM_Call( gvm, 1, BOTAI_START_FRAME, time );
}

int SV_BotGetConsoleMessage( int client, char *buf, int size )
{
	if ( (unsigned) client < sv.maxclients ) {
		client_t* cl;
		int index;

		cl = &svs.clients[client];
		cl->lastPacketTime = svs.time;

		if ( cl->reliableAcknowledge == cl->reliableSequence ) {
			return qfalse;
		}

		cl->reliableAcknowledge++;
		index = cl->reliableAcknowledge & ( MAX_RELIABLE_COMMANDS - 1 );

		if ( !cl->reliableCommands[index][0] ) {
			return qfalse;
		}

		Q_strncpyz( buf, cl->reliableCommands[index], size );
		return qtrue;
	} else {
		return qfalse;
	}
}
