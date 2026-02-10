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
// cl.input.c  -- builds an intended movement command to send to the server

#include "client.h"

static unsigned frame_msec;
static int old_com_frameTime;

static qboolean in_buttons[32];

static cvar_t *cl_nodelta;

static cvar_t *cl_showSend;

static cvar_t *cl_sensitivity;

static cvar_t *cl_run;
static cvar_t *cl_freelook;

static cvar_t *cl_yawspeed;
static cvar_t *cl_pitchspeed;
static cvar_t *cl_anglespeedkey;

static cvar_t *cl_maxpackets;
static cvar_t *cl_packetdup;

static cvar_t *m_pitch;
static cvar_t *m_yaw;
static cvar_t *m_forward;
static cvar_t *m_side;
static cvar_t *m_filter;

void IN_ButtonDown(void) {
    int id = atoi(Cmd_Argv(1));
    if (id < 0 || id >= MAX_BUTTONS) return;
    in_buttons[id] = qtrue;
}

void IN_ButtonUp(void) {
    int id = atoi(Cmd_Argv(1));
    if (id < 0 || id >= MAX_BUTTONS) return;
    in_buttons[id] = qfalse;
}

void CL_MouseEvent( int dx, int dy ) {
	if ( Key_GetCatcher() & KEYCATCH_UI ) {
		VM_Call( uivm, 2, UI_MOUSE_EVENT, dx, dy );
	} else {
		cl.mouseDx += dx;
		cl.mouseDy += dy;
	}
}

static void CL_MouseMove( usercmd_t *cmd ) {
	float mx, my;

	mx = cl.mouseDx;
	my = cl.mouseDy;

	cl.mouseDx = 0;
	cl.mouseDy = 0;

	if (mx == 0.0f && my == 0.0f)
		return;

	mx *= cl_sensitivity->value;
	my *= cl_sensitivity->value;

	// ingame FOV
	mx *= cl.cgameSensitivity;
	my *= cl.cgameSensitivity;

	// add mouse X/Y to cmd
	cl.viewangles[YAW] -= m_yaw->value * mx;
	cl.viewangles[PITCH] += m_pitch->value * my;
}


/*
==============
CL_CmdButtons
==============
*/
static void CL_CmdButtons( usercmd_t *cmd ) {
	int		i;
	
	for ( i = 0 ; i < MAX_BUTTONS; i++ ) {
		if ( in_buttons[i] ) cmd->buttons |= 1 << i;
	}
}


/*
==============
CL_FinishMove
==============
*/
static void CL_FinishMove( usercmd_t *cmd ) {
	int		i;

	// send the current server time so the amount of movement
	// can be determined without allowing cheating
	cmd->serverTime = cl.serverTime;

	for (i=0 ; i<3 ; i++) {
		cmd->angles[i] = ANGLE2SHORT(cl.viewangles[i]);
	}
}


/*
=================
CL_CreateCmd
=================
*/
static usercmd_t CL_CreateCmd( void ) {
	usercmd_t	cmd;
	vec3_t		oldAngles;

	VectorCopy( cl.viewangles, oldAngles );

	Com_Memset( &cmd, 0, sizeof( cmd ) );

	CL_CmdButtons( &cmd );
	CL_MouseMove( &cmd );

	// check to make sure the angles haven't wrapped
	if ( cl.viewangles[PITCH] - oldAngles[PITCH] > 90 ) {
		cl.viewangles[PITCH] = oldAngles[PITCH] + 90;
	} else if ( oldAngles[PITCH] - cl.viewangles[PITCH] > 90 ) {
		cl.viewangles[PITCH] = oldAngles[PITCH] - 90;
	}

	// store out the final values
	CL_FinishMove( &cmd );

	return cmd;
}


/*
=================
CL_CreateNewCommands

Create a new usercmd_t structure for this frame
=================
*/
static void CL_CreateNewCommands( void ) {
	int			cmdNum;

	// no need to create usercmds until we have a gamestate
	if ( cls.state < CA_PRIMED ) {
		return;
	}

	frame_msec = com_frameTime - old_com_frameTime;

	// if running over 1000fps, act as if each frame is 1ms
	// prevents divisions by zero
	if ( frame_msec < 1 ) {
		frame_msec = 1;
	}

	// if running less than 5fps, truncate the extra time to prevent
	// unexpected moves after a hitch
	if ( frame_msec > 200 ) {
		frame_msec = 200;
	}
	old_com_frameTime = com_frameTime;


	// generate a command for this frame
	cl.cmdNumber++;
	cmdNum = cl.cmdNumber & CMD_MASK;
	cl.cmds[cmdNum] = CL_CreateCmd();
}


/*
=================
CL_ReadyToSendPacket

Returns qfalse if we are over the maxpackets limit
and should choke back the bandwidth a bit by not sending
a packet this frame.  All the commands will still get
delivered in the next packet, but saving a header and
getting more delta compression will reduce total bandwidth.
=================
*/
static qboolean CL_ReadyToSendPacket( void ) {
	int		oldPacketNum;
	int		delta;

	// if we don't have a valid gamestate yet, only send
	// one packet a second
	if ( cls.state != CA_ACTIVE &&
		cls.state != CA_PRIMED &&
		cls.realtime - clc.lastPacketSentTime < 1000 ) {
		return qfalse;
	}

	// send every frame for loopbacks
	if ( clc.netchan.remoteAddress.type == NA_LOOPBACK ) {
		return qtrue;
	}

	// send every frame for LAN
	if ( cl_lanForcePackets->integer && clc.netchan.isLANAddress ) {
		return qtrue;
	}

	oldPacketNum = (clc.netchan.outgoingSequence - 1) & PACKET_MASK;
	delta = cls.realtime - cl.outPackets[ oldPacketNum ].p_realtime;
	if ( delta < 1000 / cl_maxpackets->integer ) {
		// the accumulated commands will go out in the next packet
		return qfalse;
	}

	return qtrue;
}


/*
===================
CL_WritePacket

Create and send the command packet to the server
Including both the reliable commands and the usercmds

During normal gameplay, a client packet will contain something like:

4	sequence number
2	qport
4	serverid
4	acknowledged sequence number
4	clc.serverCommandSequence
<optional reliable commands>
1	clc_move or clc_moveNoDelta
1	command count
<count * usercmds>

===================
*/
void CL_WritePacket( int repeat ) {
	msg_t		buf;
	byte		data[ MAX_MSGLEN_BUF ];
	int			i, j, n;
	usercmd_t	*cmd, *oldcmd;
	usercmd_t	nullcmd;
	int			packetNum;
	int			oldPacketNum;
	int			count, key;

	Com_Memset( &nullcmd, 0, sizeof(nullcmd) );
	oldcmd = &nullcmd;

	MSG_Init( &buf, data, MAX_MSGLEN );

	MSG_Bitstream( &buf );
	// write the current serverId so the server
	// can tell if this is from the current gameState
	MSG_WriteLong( &buf, cl.serverId );

	// write the last message we received, which can
	// be used for delta compression, and is also used
	// to tell if we dropped a gamestate
	MSG_WriteLong( &buf, clc.serverMessageSequence );

	// write the last reliable message we received
	MSG_WriteLong( &buf, clc.serverCommandSequence );

	// write any unacknowledged clientCommands
	n = clc.reliableSequence - clc.reliableAcknowledge;
	for ( i = 0; i < n; i++ ) {
		const int index = clc.reliableAcknowledge + 1 + i;
		MSG_WriteByte( &buf, clc_clientCommand );
		MSG_WriteLong( &buf, index );
		MSG_WriteString( &buf, clc.reliableCommands[ index & ( MAX_RELIABLE_COMMANDS - 1 ) ] );
	}

	// we want to send all the usercmds that were generated in the last
	// few packet, so even if a couple packets are dropped in a row,
	// all the cmds will make it to the server

	oldPacketNum = (clc.netchan.outgoingSequence - 1 - cl_packetdup->integer) & PACKET_MASK;
	count = cl.cmdNumber - cl.outPackets[ oldPacketNum ].p_cmdNumber;
	if ( count > MAX_PACKET_USERCMDS ) {
		count = MAX_PACKET_USERCMDS;
		Com_Printf("MAX_PACKET_USERCMDS\n");
	}
	if ( count >= 1 ) {
		if ( cl_showSend->integer ) {
			Com_Printf( "(%i)", count );
		}

		// begin a client move command
		if ( !cl.snap.valid || clc.serverMessageSequence != cl.snap.messageNum ) {
			MSG_WriteByte( &buf, clc_moveNoDelta );
		} else {
			MSG_WriteByte( &buf, clc_move );
		}

		// write the command count
		MSG_WriteByte( &buf, count );

		// use the checksum feed in the key
		key = clc.checksumFeed;
		// also use the message acknowledge
		key ^= clc.serverMessageSequence;
		// also use the last acknowledged server command in the key
		key ^= MSG_HashKey(clc.serverCommands[ clc.serverCommandSequence & (MAX_RELIABLE_COMMANDS-1) ], 32);

		// write all the commands, including the predicted command
		for ( i = 0 ; i < count ; i++ ) {
			j = (cl.cmdNumber - count + i + 1) & CMD_MASK;
			cmd = &cl.cmds[j];
			MSG_WriteDeltaUsercmdKey (&buf, key, oldcmd, cmd);
			oldcmd = cmd;
		}
	}

	//
	// deliver the message
	//
	packetNum = clc.netchan.outgoingSequence & PACKET_MASK;
	cl.outPackets[ packetNum ].p_realtime = cls.realtime;
	cl.outPackets[ packetNum ].p_serverTime = oldcmd->serverTime;
	cl.outPackets[ packetNum ].p_cmdNumber = cl.cmdNumber;
	clc.lastPacketSentTime = cls.realtime;

	if ( cl_showSend->integer ) {
		Com_Printf( "%i ", buf.cursize );
	}

	MSG_WriteByte( &buf, clc_EOF );

	if ( buf.overflowed ) {
		if ( cls.state >= CA_CONNECTED ) {
			cls.state = CA_CONNECTING; // to avoid recursive error
		}
		Com_Error( ERR_DROP, "%s: message overflowed", __func__ );
	}

	if ( repeat == 0 || clc.netchan.remoteAddress.type == NA_LOOPBACK ) {
		CL_Netchan_Transmit( &clc.netchan, &buf );
	} else {
		CL_Netchan_Enqueue( &clc.netchan, &buf, repeat + 1 );
		NET_FlushPacketQueue( 0 );
	}
}


/*
=================
CL_SendCmd

Called every frame to builds and sends a command packet to the server.
=================
*/
void CL_SendCmd( void ) {
	// don't send any message if not connected
	if ( cls.state < CA_CONNECTED ) {
		return;
	}

	// don't send commands if paused
	if ( com_sv_running->integer && sv_paused->integer ) {
		return;
	}

	CL_CreateNewCommands();

	// don't send a packet if the last packet was sent too recently
	if ( !CL_ReadyToSendPacket() ) {
		if ( cl_showSend->integer ) {
			Com_Printf( ". " );
		}
		return;
	}

	CL_WritePacket( 0 );
}

void CL_InitInput( void ) {
	Cmd_AddCommand("+button", IN_ButtonDown);
    Cmd_AddCommand("-button", IN_ButtonUp);

	cl_nodelta = Cvar_Get( "cl_nodelta", "0", 0 );
	cl_showSend = Cvar_Get( "cl_showSend", "0", 0 );
	cl_yawspeed = Cvar_Get( "cl_yawspeed", "140", CVAR_ARCHIVE );
	cl_pitchspeed = Cvar_Get( "cl_pitchspeed", "140", CVAR_ARCHIVE );
	cl_anglespeedkey = Cvar_Get( "cl_anglespeedkey", "1.5", 0 );
	cl_maxpackets = Cvar_Get ("cl_maxpackets", "60", CVAR_ARCHIVE );
	cl_packetdup = Cvar_Get( "cl_packetdup", "1", CVAR_ARCHIVE );
	cl_run = Cvar_Get( "cl_run", "1", CVAR_ARCHIVE );
	cl_sensitivity = Cvar_Get( "sensitivity", "5", CVAR_ARCHIVE );
	cl_freelook = Cvar_Get( "cl_freelook", "1", CVAR_ARCHIVE );
	m_pitch = Cvar_Get( "m_pitch", "0.022", CVAR_ARCHIVE );
	m_yaw = Cvar_Get( "m_yaw", "0.022", CVAR_ARCHIVE );
	m_forward = Cvar_Get( "m_forward", "0.25", CVAR_ARCHIVE );
	m_side = Cvar_Get( "m_side", "0.25", CVAR_ARCHIVE );
	m_filter = Cvar_Get( "m_filter", "0", CVAR_ARCHIVE );
}

void CL_ClearInput( void ) {
	Cmd_RemoveCommand ("+button");
	Cmd_RemoveCommand ("-button");
}
