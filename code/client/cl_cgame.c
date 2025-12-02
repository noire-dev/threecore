// Copyright (C) 1999-2005 ID Software, Inc.
// Copyright (C) 2023-2025 Noire.dev
// SourceTech — GPLv2; see LICENSE for details.

#include "client.h"
#include "../botlib/botlib.h"

extern botlib_export_t* botlib_export;

static void CL_GetGameState(gameState_t* gs) { *gs = cl.gameState; }

static void CL_GetGlconfig(glconfig_t* glconfig) { *glconfig = cls.glconfig; }

static qboolean CL_GetUserCmd(int cmdNumber, usercmd_t* ucmd) {
	if(cl.cmdNumber - cmdNumber < 0) Com_Error(ERR_DROP, "CL_GetUserCmd: cmdNumber (%i) > cl.cmdNumber (%i)", cmdNumber, cl.cmdNumber);

	// the usercmd has been overwritten in the wrapping
	// buffer because it is too far out of date
	if(cl.cmdNumber - cmdNumber >= CMD_BACKUP) return qfalse;

	*ucmd = cl.cmds[cmdNumber & CMD_MASK];

	return qtrue;
}

static int CL_GetCurrentCmdNumber(void) { return cl.cmdNumber; }

static void CL_GetCurrentSnapshotNumber(int* snapshotNumber, int* serverTime) {
	*snapshotNumber = cl.snap.messageNum;
	*serverTime = cl.snap.serverTime;
}

static qboolean CL_GetSnapshot(int snapshotNumber, snapshot_t* snapshot) {
	clSnapshot_t* clSnap;
	int i, count;

	if(cl.snap.messageNum - snapshotNumber < 0) {
		Com_Error(ERR_DROP, "CL_GetSnapshot: snapshotNumber (%i) > cl.snapshot.messageNum (%i)", snapshotNumber, cl.snap.messageNum);
	}

	// if the frame has fallen out of the circular buffer, we can't return it
	if(cl.snap.messageNum - snapshotNumber >= PACKET_BACKUP) return qfalse;

	// if the frame is not valid, we can't return it
	clSnap = &cl.snapshots[snapshotNumber & PACKET_MASK];
	if(!clSnap->valid) return qfalse;

	// if the entities in the frame have fallen out of their
	// circular buffer, we can't return it
	if(cl.parseEntitiesNum - clSnap->parseEntitiesNum >= MAX_PARSE_ENTITIES) return qfalse;

	// write the snapshot
	snapshot->snapFlags = clSnap->snapFlags;
	snapshot->serverCommandSequence = clSnap->serverCommandNum;
	snapshot->ping = clSnap->ping;
	snapshot->serverTime = clSnap->serverTime;
	Com_Memcpy(snapshot->areamask, clSnap->areamask, sizeof(snapshot->areamask));
	snapshot->ps = clSnap->ps;
	count = clSnap->numEntities;
	if(count > MAX_ENTITIES_IN_SNAPSHOT) {
		Com_DPrintf("CL_GetSnapshot: truncated %i entities to %i\n", count, MAX_ENTITIES_IN_SNAPSHOT);
		count = MAX_ENTITIES_IN_SNAPSHOT;
	}
	snapshot->numEntities = count;
	for(i = 0; i < count; i++) {
		snapshot->entities[i] = cl.parseEntities[(clSnap->parseEntitiesNum + i) & (MAX_PARSE_ENTITIES - 1)];
	}

	return qtrue;
}

static void CL_SetUserCmdValue(int userCmdValue, float sensitivityScale) {
	cl.cgameUserCmdValue = userCmdValue;
	cl.cgameSensitivity = sensitivityScale;
}

static void CL_AddCgameCommand(const char* cmdName) { Cmd_AddCommand(cmdName, NULL); }

static void CL_ConfigstringModified(void) {
	const char *old, *s;
	int i, index;
	const char* dup;
	gameState_t oldGs;
	int len;

	index = atoi(Cmd_Argv(1));
	if((unsigned)index >= MAX_CONFIGSTRINGS) {
		Com_Error(ERR_DROP, "%s: bad configstring index %i", __func__, index);
	}
	// get everything after "cs <num>"
	s = Cmd_ArgsFrom(2);

	old = cl.gameState.stringData + cl.gameState.stringOffsets[index];
	if(!strcmp(old, s)) return;  // unchanged

	// build the new gameState_t
	oldGs = cl.gameState;

	Com_Memset(&cl.gameState, 0, sizeof(cl.gameState));

	// leave the first 0 for uninitialized strings
	cl.gameState.dataCount = 1;

	for(i = 0; i < MAX_CONFIGSTRINGS; i++) {
		if(i == index) {
			dup = s;
		} else {
			dup = oldGs.stringData + oldGs.stringOffsets[i];
		}
		if(!dup[0]) continue;  // leave with the default empty string

		len = strlen(dup);

		if(len + 1 + cl.gameState.dataCount > MAX_GAMESTATE_CHARS) {
			Com_Error(ERR_DROP, "%s: MAX_GAMESTATE_CHARS exceeded", __func__);
		}

		// append it to the gameState string buffer
		cl.gameState.stringOffsets[i] = cl.gameState.dataCount;
		Com_Memcpy(cl.gameState.stringData + cl.gameState.dataCount, dup, len + 1);
		cl.gameState.dataCount += len + 1;
	}

	if(index == CS_SYSTEMINFO) {
		// parse serverId and other cvars
		CL_SystemInfoChanged(qfalse);
	}
}

static qboolean CL_GetServerCommand(int serverCommandNumber) {
	const char* s;
	const char* cmd;
	static char bigConfigString[BIG_INFO_STRING];
	int argc, index;

	// if we have irretrievably lost a reliable command, drop the connection
	if(clc.serverCommandSequence - serverCommandNumber >= MAX_RELIABLE_COMMANDS) {
		// when a demo record was started after the client got a whole bunch of
		// reliable commands then the client never got those first reliable commands
		if(clc.demoplaying) {
			Cmd_Clear();
			return qfalse;
		}
		Com_Error(ERR_DROP, "CL_GetServerCommand: a reliable command was cycled out");
		return qfalse;
	}

	if(clc.serverCommandSequence - serverCommandNumber < 0) {
		Com_Error(ERR_DROP, "CL_GetServerCommand: requested a command not received");
		return qfalse;
	}

	index = serverCommandNumber & (MAX_RELIABLE_COMMANDS - 1);
	s = clc.serverCommands[index];
	clc.lastExecutedServerCommand = serverCommandNumber;

	Com_DPrintf("serverCommand: %i : %s\n", serverCommandNumber, s);

	if(clc.serverCommandsIgnore[index]) {
		Cmd_Clear();
		return qfalse;
	}

rescan:
	Cmd_TokenizeString(s);
	cmd = Cmd_Argv(0);
	argc = Cmd_Argc();

	if(!strcmp(cmd, "disconnect")) {
		if(argc >= 2)
			Com_Error(ERR_SERVERDISCONNECT, "Server disconnected - %s", Cmd_Argv(1));
		else
			Com_Error(ERR_SERVERDISCONNECT, "Server disconnected");
	}

	if(!strcmp(cmd, "bcs0")) {
		Com_sprintf(bigConfigString, BIG_INFO_STRING, "cs %s \"%s", Cmd_Argv(1), Cmd_Argv(2));
		return qfalse;
	}

	if(!strcmp(cmd, "bcs1")) {
		s = Cmd_Argv(2);
		if(strlen(bigConfigString) + strlen(s) >= BIG_INFO_STRING) {
			Com_Error(ERR_DROP, "bcs exceeded BIG_INFO_STRING");
		}
		strcat(bigConfigString, s);
		return qfalse;
	}

	if(!strcmp(cmd, "bcs2")) {
		s = Cmd_Argv(2);
		if(strlen(bigConfigString) + strlen(s) + 1 >= BIG_INFO_STRING) {
			Com_Error(ERR_DROP, "bcs exceeded BIG_INFO_STRING");
		}
		strcat(bigConfigString, s);
		strcat(bigConfigString, "\"");
		s = bigConfigString;
		goto rescan;
	}

	if(!strcmp(cmd, "cs")) {
		CL_ConfigstringModified();
		// reparse the string, because CL_ConfigstringModified may have done another Cmd_TokenizeString()
		Cmd_TokenizeString(s);
		return qtrue;
	}

	if(!strcmp(cmd, "map_restart")) {
		// clear notify lines and outgoing commands before passing
		// the restart to the cgame
		Con_ClearNotify();
		// reparse the string, because Con_ClearNotify() may have done another Cmd_TokenizeString()
		Cmd_TokenizeString(s);
		Com_Memset(cl.cmds, 0, sizeof(cl.cmds));
		cls.lastVidRestart = Sys_Milliseconds();  // hack for OSP mod
		return qtrue;
	}

	return qtrue;
}

static void CL_CM_LoadMap(const char* mapname) {
	int checksum;

	CM_LoadMap(mapname, qtrue, &checksum);
}

void CL_ShutdownCGame(void) {
	Key_SetCatcher(Key_GetCatcher() & ~KEYCATCH_CGAME);
	cls.cgameStarted = qfalse;

	if(!cgvm) return;

	VM_Call(cgvm, 0, CG_SHUTDOWN);
	VM_Free(cgvm);
	cgvm = NULL;
}

static int FloatAsInt(float f) {
	floatint_t fi;
	fi.f = f;
	return fi.i;
}

static void* VM_ArgPtr(intptr_t intValue) {
	if(!intValue || cgvm == NULL) return NULL;

	return (void*)(cgvm->dataBase + (intValue & cgvm->dataMask));
}

static intptr_t CL_CgameSystemCalls(intptr_t* args) {
	switch(args[0]) {
		case CG_ADDCOMMAND: CL_AddCgameCommand(VMA(1)); return 0;
		case CG_SENDCLIENTCOMMAND: CL_AddReliableCommand(VMA(1), qfalse); return 0;
		case CG_CM_LOADMAP: CL_CM_LoadMap(VMA(1)); return 0;
		case CG_CM_NUMINLINEMODELS: return CM_NumInlineModels();
		case CG_CM_INLINEMODEL: return CM_InlineModel(args[1]);
		case CG_CM_TEMPBOXMODEL: return CM_TempBoxModel(VMA(1), VMA(2));
		case CG_CM_POINTCONTENTS: return CM_PointContents(VMA(1), args[2]);
		case CG_CM_TRANSFORMEDPOINTCONTENTS: return CM_TransformedPointContents(VMA(1), args[2], VMA(3), VMA(4));
		case CG_CM_BOXTRACE: CM_BoxTrace(VMA(1), VMA(2), VMA(3), VMA(4), VMA(5), args[6], args[7]); return 0;
		case CG_CM_TRANSFORMEDBOXTRACE: CM_TransformedBoxTrace(VMA(1), VMA(2), VMA(3), VMA(4), VMA(5), args[6], args[7], VMA(8), VMA(9)); return 0;
		case CG_CM_MARKFRAGMENTS: return re.MarkFragments(args[1], VMA(2), VMA(3), args[4], VMA(5), args[6], VMA(7));
		case CG_S_STARTSOUND: S_StartSound(VMA(1), args[2], args[3], args[4]); return 0;
		case CG_S_CLEARLOOPINGSOUNDS: S_ClearLoopingSounds(args[1]); return 0;
		case CG_S_ADDLOOPINGSOUND: S_AddLoopingSound(args[1], VMA(2), VMA(3), args[4]); return 0;
		case CG_S_UPDATEENTITYPOSITION: S_UpdateEntityPosition(args[1], VMA(2)); return 0;
		case CG_S_RESPATIALIZE: S_Respatialize(args[1], VMA(2), VMA(3)); return 0;
		case CG_S_STARTBACKGROUNDTRACK: S_StartBackgroundTrack(VMA(1), VMA(2)); return 0;
		case CG_R_LOADWORLDMAP: re.LoadWorld(VMA(1)); return 0;
		case CG_R_MODELBOUNDS: re.ModelBounds(args[1], VMA(2), VMA(3)); return 0;
		case CG_GETGAMESTATE: CL_GetGameState(VMA(1)); return 0;
		case CG_GETCURRENTSNAPSHOTNUMBER: CL_GetCurrentSnapshotNumber(VMA(1), VMA(2)); return 0;
		case CG_GETSNAPSHOT: return CL_GetSnapshot(args[1], VMA(2));
		case CG_GETSERVERCOMMAND: return CL_GetServerCommand(args[1]);
		case CG_GETCURRENTCMDNUMBER: return CL_GetCurrentCmdNumber();
		case CG_GETUSERCMD: return CL_GetUserCmd(args[1], VMA(2));
		case CG_SETUSERCMDVALUE: CL_SetUserCmdValue(args[1], VMF(2)); return 0;
		case CG_KEY_GETKEY: return Key_GetKey(VMA(1));
		case CG_S_ADDREALLOOPINGSOUND: S_AddRealLoopingSound(args[1], VMA(2), VMA(3), args[4]); return 0;
		case CG_S_STOPLOOPINGSOUND: S_StopLoopingSound(args[1]); return 0;
		case CG_IMPORTOBJ: CL_StartConvertOBJ(VMA(1)); return 0;

#include "../q_sharedsyscalls.inc"
#include "../q_sharedsyscalls_client.inc"

		default: Com_Error(ERR_DROP, "Bad CGame system trap: %ld", (long int)args[0]);
	}

	return 0;
}

void CL_InitCGame(void) {
	int t1, t2;

	Cbuf_NestedReset();
	t1 = Sys_Milliseconds();
	Con_Close();

	cgvm = VM_Create(VM_CGAME, CL_CgameSystemCalls);
	if(!cgvm) Com_Error(ERR_DROP, "VM_Create on cgame failed");
	cls.state = CA_LOADING;

	VM_Call(cgvm, 3, CG_INIT, clc.serverMessageSequence, clc.lastExecutedServerCommand, clc.clientNum);

	if(!clc.demoplaying && !cl_connectedToCheatServer) Cvar_SetCheatState();

	// we will send a usercmd this frame, which
	// will cause the server to send us the first snapshot
	cls.state = CA_PRIMED;

	t2 = Sys_Milliseconds();
	Com_Printf("CL_InitCGame: %5.2f seconds\n", (t2 - t1) / 1000.0);

	if(!Sys_LowPhysicalMemory()) Com_TouchMemory();

	Con_ClearNotify();

	cls.lastVidRestart = Sys_Milliseconds();
}

qboolean CL_GameCommand(void) {
	qboolean bRes;

	if(!cgvm) return qfalse;

	bRes = (qboolean)VM_Call(cgvm, 0, CG_CONSOLE_COMMAND);

	Cbuf_NestedReset();

	return bRes;
}

void CL_CGameRendering(void) { VM_Call(cgvm, 2, CG_DRAW_ACTIVE_FRAME, cl.serverTime, clc.demoplaying); }

#define RESET_TIME 500

static void CL_AdjustTimeDelta(void) {
	int newDelta;
	int deltaDelta;

	cl.newSnapshots = qfalse;

	// the delta never drifts when replaying a demo
	if(clc.demoplaying) return;

	newDelta = cl.snap.serverTime - cls.realtime;
	deltaDelta = abs(newDelta - cl.serverTimeDelta);

	if(deltaDelta > RESET_TIME) {
		cl.serverTimeDelta = newDelta;
		cl.oldServerTime = cl.snap.serverTime;  // FIXME: is this a problem for cgame?
		cl.serverTime = cl.snap.serverTime;
	} else if(deltaDelta > 100) {
		// fast adjust, cut the difference in half
		cl.serverTimeDelta = (cl.serverTimeDelta + newDelta) >> 1;
	} else {
		// slow drift adjust, only move 1 or 2 msec

		// if any of the frames between this and the previous snapshot
		// had to be extrapolated, nudge our sense of time back a little
		// the granularity of +1 / -2 is too high for timescale modified frametimes
		if(com_timescale->value == 0 || com_timescale->value == 1) {
			if(cl.extrapolatedSnapshot) {
				cl.extrapolatedSnapshot = qfalse;
				cl.serverTimeDelta -= 2;
			} else {
				// otherwise, move our sense of time forward to minimize total latency
				cl.serverTimeDelta++;
			}
		}
	}
}

static void CL_FirstSnapshot(void) {
	// ignore snapshots that don't have entities
	if(cl.snap.snapFlags & SNAPFLAG_NOT_ACTIVE) return;

	cls.state = CA_ACTIVE;

	// set the timedelta so we are exactly on this first frame
	cl.serverTimeDelta = cl.snap.serverTime - cls.realtime;
	cl.oldServerTime = cl.snap.serverTime;

	// if this is the first frame of active play,
	// execute the contents of activeAction now
	if(cl_activeAction->string[0]) {
		Cbuf_AddText(cl_activeAction->string);
		Cbuf_AddText("\n");
		Cvar_Set("activeAction", "");
	}
}

void CL_SetCGameTime(void) {
	qboolean demoFreezed;

	// getting a valid frame message ends the connection process
	if(cls.state != CA_ACTIVE) {
		if(cls.state != CA_PRIMED) return;
		if(clc.demoplaying) {
			// we shouldn't get the first snapshot on the same frame
			// as the gamestate, because it causes a bad time skip
			if(!clc.firstDemoFrameSkipped) {
				clc.firstDemoFrameSkipped = qtrue;
				return;
			}
			CL_ReadDemoMessage();
		}
		if(cl.newSnapshots) {
			cl.newSnapshots = qfalse;
			CL_FirstSnapshot();
		}
		if(cls.state != CA_ACTIVE) return;
	}

	// if we have gotten to this point, cl.snap is guaranteed to be valid
	if(!cl.snap.valid) {
		Com_Error(ERR_DROP, "CL_SetCGameTime: !cl.snap.valid");
	}

	// allow pause in single player
	if(sv_paused->integer && CL_CheckPaused() && com_sv_running->integer) {
		// paused
		return;
	}

	if(cl.snap.serverTime - cl.oldFrameServerTime < 0) {
		Com_Error(ERR_DROP, "cl.snap.serverTime < cl.oldFrameServerTime");
	}
	cl.oldFrameServerTime = cl.snap.serverTime;

	// get our current view of time
	demoFreezed = clc.demoplaying && com_timescale->value == 0.0f;
	if(demoFreezed) {
		// \timescale 0 is used to lock a demo in place for single frame advances
		cl.serverTimeDelta -= cls.frametime;
	} else {
		cl.serverTime = cls.realtime + cl.serverTimeDelta;

		// guarantee that time will never flow backwards
		if(cl.serverTime - cl.oldServerTime < 0) {
			cl.serverTime = cl.oldServerTime;
		}
		cl.oldServerTime = cl.serverTime;

		// note if we are almost past the latest frame (without timeNudge),
		// so we will try and adjust back a bit when the next snapshot arrives
		if(cls.realtime + cl.serverTimeDelta - cl.snap.serverTime >= -5) {
			cl.extrapolatedSnapshot = qtrue;
		}
	}

	// if we have gotten new snapshots, drift serverTimeDelta
	// don't do this every frame, or a period of packet loss would
	// make a huge adjustment
	if(cl.newSnapshots) CL_AdjustTimeDelta();

	if(!clc.demoplaying) return;

	while(cl.serverTime - cl.snap.serverTime >= 0) {
		// feed another message, which should change
		// the contents of cl.snap
		CL_ReadDemoMessage();
		if(cls.state != CA_ACTIVE) return;
	}
}
