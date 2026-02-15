// Copyright (C) 1999-2005 ID Software, Inc.
// Copyright (C) 2023-2025 Noire.dev
// ThreeCore — GPLv2; see LICENSE for details.

#include "server.h"

int SV_NumForGentity(sharedEntity_t* ent) {
	int num;

	num = ((byte*)ent - (byte*)sv.gentities) / sv.gentitySize;
	return num;
}

sharedEntity_t* SV_GentityNum(int num) {
	sharedEntity_t* ent;

	ent = (sharedEntity_t*)((byte*)sv.gentities + sv.gentitySize * (num));
	return ent;
}

playerState_t* SV_GameClientNum(int num) {
	playerState_t* ps;

	ps = (playerState_t*)((byte*)sv.gameClients + sv.gameClientSize * (num));
	return ps;
}

svEntity_t* SV_SvEntityForGentity(sharedEntity_t* gEnt) {
	if(!gEnt || gEnt->s.number < 0 || gEnt->s.number >= MAX_GENTITIES) Com_Error(ERR_DROP, "SV_SvEntityForGentity: bad gEnt");

	return &sv.svEntities[gEnt->s.number];
}

sharedEntity_t* SV_GEntityForSvEntity(svEntity_t* svEnt) {
	int num;

	num = svEnt - sv.svEntities;
	return SV_GentityNum(num);
}

static void SV_GameSendServerCommand(int clientNum, const char* text) {
	if(clientNum == -1) {
		SV_SendServerCommand(NULL, "%s", text);
	} else {
		if(clientNum < 0 || clientNum >= sv.maxclients) return;
		SV_SendServerCommand(svs.clients + clientNum, "%s", text);
	}
}

static void SV_GameDropClient(int clientNum, const char* reason) {
	if(clientNum < 0 || clientNum >= sv.maxclients) return;
	SV_DropClient(svs.clients + clientNum, reason);
}

static void SV_SetBrushModel(sharedEntity_t* ent, const char* name) {
	clipHandle_t h;
	vec3_t mins, maxs;

	if(!name) Com_Error(ERR_DROP, "SV_SetBrushModel: NULL");
	if(name[0] != '*') Com_Error(ERR_DROP, "SV_SetBrushModel: %s isn't a brush model", name);

	ent->s.modelindex = atoi(name + 1);

	h = CM_InlineModel(ent->s.modelindex);
	CM_ModelBounds(h, mins, maxs);
	VectorCopy(mins, ent->r.mins);
	VectorCopy(maxs, ent->r.maxs);
	ent->r.bmodel = qtrue;

	ent->r.contents = -1;  // we don't know exactly what is in the brushes

	SV_LinkEntity(ent);  // FIXME: remove
}

qboolean SV_inPVS(const vec3_t p1, const vec3_t p2) {
	int leafnum;
	int cluster;
	int area1, area2;
	byte* mask;

	leafnum = CM_PointLeafnum(p1);
	cluster = CM_LeafCluster(leafnum);
	area1 = CM_LeafArea(leafnum);
	mask = CM_ClusterPVS(cluster);

	leafnum = CM_PointLeafnum(p2);
	cluster = CM_LeafCluster(leafnum);
	area2 = CM_LeafArea(leafnum);
	if(mask && (!(mask[cluster >> 3] & (1 << (cluster & 7))))) return qfalse;
	if(!CM_AreasConnected(area1, area2)) return qfalse;  // a door blocks sight
	return qtrue;
}

static void SV_AdjustAreaPortalState(sharedEntity_t* ent, qboolean open) {
	svEntity_t* svEnt;

	svEnt = SV_SvEntityForGentity(ent);
	if(svEnt->areanum2 == -1) return;
	CM_AdjustAreaPortalState(svEnt->areanum, svEnt->areanum2, open);
}

static qboolean SV_EntityContact(const vec3_t mins, const vec3_t maxs, const sharedEntity_t* gEnt) {
	const float *origin, *angles;
	clipHandle_t ch;
	trace_t trace;

	// check for exact collision
	origin = gEnt->r.currentOrigin;
	angles = gEnt->r.currentAngles;

	ch = SV_ClipHandleForEntity(gEnt);
	CM_TransformedBoxTrace(&trace, vec3_origin, vec3_origin, mins, maxs, ch, -1, origin, angles);

	return trace.startsolid;
}

static void SV_GetServerinfo(char* buffer, int bufferSize) {
	if(bufferSize < 1) Com_Error(ERR_DROP, "SV_GetServerinfo: bufferSize == %i", bufferSize);
	if(sv.state != SS_GAME || !sv.configstrings[CS_SERVERINFO]) {
		Q_strncpyz(buffer, Cvar_InfoString(CVAR_SERVERINFO, NULL), bufferSize);
	} else {
		Q_strncpyz(buffer, sv.configstrings[CS_SERVERINFO], bufferSize);
	}
}

static void SV_LocateGameData(sharedEntity_t* gEnts, int numGEntities, int sizeofGEntity_t, playerState_t* clients, int sizeofGameClient) {
	if(numGEntities > MAX_GENTITIES) {
		Com_Error(ERR_DROP, "%s: bad entity count %i", __func__, numGEntities);
	} else {
		if(sizeofGEntity_t > gvm->exactDataLength / numGEntities) {
			Com_Error(ERR_DROP, "%s: bad entity size %i", __func__, sizeofGEntity_t);
		} else if((byte*)gEnts + (numGEntities * sizeofGEntity_t) > (gvm->dataBase + gvm->exactDataLength)) {
			Com_Error(ERR_DROP, "%s: entities located out of data segment", __func__);
		}
	}

	if(sizeofGameClient > gvm->exactDataLength / MAX_CLIENTS) {
		Com_Error(ERR_DROP, "%s: bad game client size %i", __func__, sizeofGameClient);
	} else if((byte*)clients + (sizeofGameClient * MAX_CLIENTS) > gvm->dataBase + gvm->exactDataLength) {
		Com_Error(ERR_DROP, "%s: clients located out of data segment", __func__);
	}

	sv.gentities = gEnts;
	sv.gentitySize = sizeofGEntity_t;
	sv.num_entities = numGEntities;

	sv.gameClients = clients;
	sv.gameClientSize = sizeofGameClient;
}

static void SV_GetUsercmd(int clientNum, usercmd_t* cmd) {
	if((unsigned)clientNum < sv.maxclients) {
		*cmd = svs.clients[clientNum].lastUsercmd;
	} else {
		Com_Error(ERR_DROP, "%s(): bad clientNum: %i", __func__, clientNum);
	}
}

static int FloatAsInt(float f) {
	floatint_t fi;
	fi.f = f;
	return fi.i;
}

static void* VM_ArgPtr(intptr_t intValue) {
	if(!intValue || gvm == NULL) return NULL;

	return (void*)(gvm->dataBase + (intValue & gvm->dataMask));
}

void* GVM_ArgPtr(intptr_t intValue) { return VM_ArgPtr(intValue); }

static intptr_t SV_GameSystemCalls(intptr_t* args) {
    int qvmIndex = VM_GAME;
	switch(args[0]) {
		case G_LOCATE_GAME_DATA: SV_LocateGameData(VMA(1), args[2], args[3], VMA(4), args[5]); return 0;
		case G_DROP_CLIENT: SV_GameDropClient(args[1], VMA(2)); return 0;
		case G_SEND_SERVER_COMMAND: SV_GameSendServerCommand(args[1], VMA(2)); return 0;
		case G_LINKENTITY: SV_LinkEntity(VMA(1)); return 0;
		case G_UNLINKENTITY: SV_UnlinkEntity(VMA(1)); return 0;
		case G_ENTITIES_IN_BOX: return 0; //SV_AreaEntities(VMA(1), VMA(2), VMA(3), args[4]);
		case G_ENTITY_CONTACT: return 0; //SV_EntityContact(VMA(1), VMA(2), VMA(3));
		case G_TRACE: /*SV_Trace(VMA(1), VMA(2), VMA(3), VMA(4), VMA(5), args[6], args[7]);*/ return 0;
		case G_POINT_CONTENTS: return SV_PointContents(VMA(1), args[2]);
		case G_SET_BRUSH_MODEL: SV_SetBrushModel(VMA(1), VMA(2)); return 0;
		case G_IN_PVS: return SV_inPVS(VMA(1), VMA(2));
		case G_SET_CONFIGSTRING: SV_SetConfigstring(args[1], VMA(2)); return 0;
		case G_GET_CONFIGSTRING: SV_GetConfigstring(args[1], VMA(2), args[3]); return 0;
		case G_SET_USERINFO: SV_SetUserinfo(args[1], VMA(2)); return 0;
		case G_GET_USERINFO: SV_GetUserinfo(args[1], VMA(2), args[3]); return 0;
		case G_GET_SERVERINFO: SV_GetServerinfo(VMA(1), args[2]); return 0;
		case G_ADJUST_AREA_PORTAL_STATE: SV_AdjustAreaPortalState(VMA(1), args[2]); return 0;
		case G_BOT_ALLOCATE_CLIENT: return SV_BotAllocateClient();
		case G_GET_USERCMD: SV_GetUsercmd(args[1], VMA(2)); return 0;
		case G_GET_ENTITY_TOKEN: {
			char* s = (char*)COM_Parse(&sv.entityParsePoint);
			{
				char* dst = (char*)VMA(1);
				const int size = args[2] - 1;
				if(size >= 0) {
					Q_strncpy(dst, s, size);
					dst[size] = '\0';
				}
			}
			if(!sv.entityParsePoint && s[0] == '\0') {
				return qfalse;
			} else {
				return qtrue;
			}
		}
		case BOTLIB_GET_CONSOLE_MESSAGE: return SV_BotGetConsoleMessage(args[1], VMA(2), args[3]);
		case BOTLIB_USER_COMMAND: {
			unsigned clientNum = args[1];
			if(clientNum < sv.maxclients) SV_ClientThink(&svs.clients[clientNum], VMA(2));
			return 0;
		}
#include "../q_sharedsyscalls.inc"
		
		default: Com_Error(ERR_DROP, "Bad game.qvm system trap: %ld", (long int)args[0]);
	}

	return 0;
}

void SV_ShutdownGameProgs(void) {
	if(!gvm) return;
	VM_Call(gvm, 1, GAME_SHUTDOWN, qfalse);
	VM_Free(gvm);
	gvm = NULL;
}

static void SV_InitGameVM(qboolean restart) {
	int i;

	// start the entity parsing at the beginning
	sv.entityParsePoint = CM_EntityString();

	for(i = 0; i < sv.maxclients; i++) svs.clients[i].gentity = NULL;

	VM_Call(gvm, 3, GAME_INIT, sv.time, Com_Milliseconds(), restart);
}

void SV_RestartGameProgs(void) {
	if(!gvm) return;
	VM_Call(gvm, 1, GAME_SHUTDOWN, qtrue);

	// do a restart instead of a free
	gvm = VM_Restart(gvm);
	if(!gvm) Com_Error(ERR_DROP, "VM_Restart on game failed");

	SV_InitGameVM(qtrue);

	// load userinfo filters
	SV_LoadFilters(sv_filter->string);
}

void SV_InitGameProgs(void) {
	// load the bytecode
	gvm = VM_Create(VM_GAME, SV_GameSystemCalls);
	if(!gvm) Com_Error(ERR_DROP, "VM_Create on game failed");

	SV_InitGameVM(qfalse);

	// load userinfo filters
	SV_LoadFilters(sv_filter->string);
}

qboolean SV_GameCommand(void) {
	if(sv.state != SS_GAME) return qfalse;

	return VM_Call(gvm, 0, GAME_CONSOLE_COMMAND);
}
