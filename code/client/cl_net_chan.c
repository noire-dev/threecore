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

#include "../qcommon/q_shared.h"
#include "../qcommon/qcommon.h"
#include "client.h"

/*
=================
CL_Netchan_TransmitNextFragment
=================
*/
static qboolean CL_Netchan_TransmitNextFragment( netchan_t *chan )
{
	if ( chan->unsentFragments )
	{
		Netchan_TransmitNextFragment( chan );
		return qtrue;
	}
	
	return qfalse;
}

/*
===============
CL_Netchan_Transmit
================
*/
void CL_Netchan_Transmit( netchan_t *chan, msg_t* msg ) {

	Netchan_Transmit( chan, msg->cursize, msg->data );
	
	// Transmit all fragments without delay
	while ( CL_Netchan_TransmitNextFragment( chan ) ) {
		// might happen if server die silently but client continue adding/sending commands
		Com_DPrintf( S_COLOR_YELLOW "%s: unsent fragments\n", __func__ );
	}
}

/*
===============
CL_Netchan_Enqueue
================
*/
void CL_Netchan_Enqueue( netchan_t *chan, msg_t* msg, int times ) {
	int i;

	// make sure we send all pending fragments to get correct chan->outgoingSequence
	while ( CL_Netchan_TransmitNextFragment( chan ) ) {
		;
	}

	for ( i = 0; i < times; i++ ) {
		Netchan_Enqueue( chan, msg->cursize, msg->data );
	}

	chan->outgoingSequence++;
}

/*
=================
CL_Netchan_Process
=================
*/
qboolean CL_Netchan_Process( netchan_t *chan, msg_t *msg ) {
	qboolean ret;

	ret = Netchan_Process( chan, msg );
	if ( !ret )
		return qfalse;

	return qtrue;
}
