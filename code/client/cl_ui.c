// Copyright (C) 1999-2005 ID Software, Inc.
// Copyright (C) 2023-2025 Noire.dev
// SourceTech — GPLv2; see LICENSE for details.

#include "client.h"
#include "../botlib/botlib.h"

extern botlib_export_t* botlib_export;

vm_t* uivm = NULL;

static void GetClientState(uiClientState_t* state) {
	state->connectPacketCount = clc.connectPacketCount;
	state->connState = cls.state;
	Q_strncpyz(state->servername, cls.servername, sizeof(state->servername));
	Q_strncpyz(state->updateInfoString, cls.updateInfoString, sizeof(state->updateInfoString));
	Q_strncpyz(state->messageString, clc.serverMessage, sizeof(state->messageString));
	state->clientNum = cl.snap.ps.clientNum;
}

static int LAN_GetServerCount(int source) {
	switch(source) {
		case AS_LOCAL: return cls.numlocalservers; break;
		case AS_GLOBAL: return cls.numglobalservers; break;
	}
	return 0;
}

static void LAN_GetServerAddressString(int source, int n, char* buf, int buflen) {
	switch(source) {
		case AS_LOCAL:
			if(n >= 0 && n < MAX_OTHER_SERVERS) {
				Q_strncpyz(buf, NET_AdrToStringwPort(&cls.localServers[n].adr), buflen);
				return;
			}
			break;
		case AS_GLOBAL:
			if(n >= 0 && n < MAX_GLOBAL_SERVERS) {
				Q_strncpyz(buf, NET_AdrToStringwPort(&cls.globalServers[n].adr), buflen);
				return;
			}
			break;
	}
	buf[0] = '\0';
}

static int LAN_GetPingQueueCount(void) { return (CL_GetPingQueueCount()); }

static void LAN_ClearPing(int n) { CL_ClearPing(n); }

static void LAN_GetPing(int n, char* buf, int buflen, int* pingtime) { CL_GetPing(n, buf, buflen, pingtime); }

static void LAN_GetPingInfo(int n, char* buf, int buflen) { CL_GetPingInfo(n, buf, buflen); }

static void CL_GetGlconfig(glconfig_t* config) { *config = *re.GetConfig(); }

static void CL_GetClipboardData(char* buf, int buflen) {
	char* cbd;

	cbd = Sys_GetClipboardData();

	if(!cbd) {
		*buf = '\0';
		return;
	}

	Q_strncpyz(buf, cbd, buflen);

	Z_Free(cbd);
}

static void Key_KeynumToStringBuf(int keynum, char* buf, int buflen) { Q_strncpyz(buf, Key_KeynumToString(keynum), buflen); }

static void Key_GetBindingBuf(int keynum, char* buf, int buflen) {
	const char* value;

	value = Key_GetBinding(keynum);
	if(value) {
		Q_strncpyz(buf, value, buflen);
	} else {
		*buf = '\0';
	}
}

static int GetConfigString(int index, char* buf, int size) {
	int offset;

	if(index < 0 || index >= MAX_CONFIGSTRINGS) return qfalse;

	offset = cl.gameState.stringOffsets[index];
	if(!offset) {
		if(size) {
			buf[0] = 0;
		}
		return qfalse;
	}

	Q_strncpyz(buf, cl.gameState.stringData + offset, size);

	return qtrue;
}

static int FloatAsInt(float f) {
	floatint_t fi;
	fi.f = f;
	return fi.i;
}

static void* VM_ArgPtr(intptr_t intValue) {
	if(!intValue || uivm == NULL) return NULL;

	return (void*)(uivm->dataBase + (intValue & uivm->dataMask));
}

static intptr_t CL_UISystemCalls(intptr_t* args) {
	switch(args[0]) {
		case UI_KEY_KEYNUMTOSTRINGBUF: Key_KeynumToStringBuf(args[1], VMA(2), args[3]); return 0;
		case UI_KEY_GETBINDINGBUF: Key_GetBindingBuf(args[1], VMA(2), args[3]); return 0;
		case UI_KEY_SETBINDING: Key_SetBinding(args[1], VMA(2)); return 0;
		case UI_KEY_ISDOWN: return Key_IsDown(args[1]);
		case UI_KEY_GETOVERSTRIKEMODE: return Key_GetOverstrikeMode();
		case UI_KEY_SETOVERSTRIKEMODE: Key_SetOverstrikeMode(args[1]); return 0;
		case UI_KEY_CLEARSTATES: Key_ClearStates(); return 0;
		case UI_KEY_SETCATCHER: Key_SetCatcher(args[1] | (Key_GetCatcher() & KEYCATCH_CONSOLE)); return 0;
		case UI_GETCLIPBOARDDATA: CL_GetClipboardData(VMA(1), args[2]); return 0;
		case UI_GETCLIENTSTATE: GetClientState(VMA(1)); return 0;
		case UI_GETCONFIGSTRING: return GetConfigString(args[1], VMA(2), args[3]);
		case UI_LAN_GETPINGQUEUECOUNT: return LAN_GetPingQueueCount();
		case UI_LAN_CLEARPING: LAN_ClearPing(args[1]); return 0;
		case UI_LAN_GETPING: LAN_GetPing(args[1], VMA(2), args[3], VMA(4)); return 0;
		case UI_LAN_GETPINGINFO: LAN_GetPingInfo(args[1], VMA(2), args[3]); return 0;
		case UI_MEMORY_REMAINING: return Hunk_MemoryRemaining();
		case UI_LAN_GETSERVERCOUNT: return LAN_GetServerCount(args[1]);
		case UI_LAN_GETSERVERADDRESSSTRING: LAN_GetServerAddressString(args[1], args[2], VMA(3), args[4]); return 0;

#include "../q_sharedsyscalls.inc"
#include "../q_sharedsyscalls_client.inc"

		default: Com_Error(ERR_DROP, "Bad UI system trap: %ld", (long int)args[0]);
	}

	return 0;
}

void CL_ShutdownUI(void) {
	Key_SetCatcher(Key_GetCatcher() & ~KEYCATCH_UI);
	cls.uiStarted = qfalse;
	if(!uivm) return;
	VM_Call(uivm, 0, UI_SHUTDOWN);
	VM_Free(uivm);
	uivm = NULL;
}

void CL_InitUI(void) {
	uivm = VM_Create(VM_UI, CL_UISystemCalls);
	if(!uivm) Com_Error(ERR_DROP, "VM_Create on UI failed");

	// init for this gamestate
	VM_Call(uivm, 1, UI_INIT, (cls.state >= CA_AUTHORIZING && cls.state < CA_ACTIVE));
}

qboolean UI_GameCommand(void) {
	if(!uivm) return qfalse;

	return VM_Call(uivm, 1, UI_CONSOLE_COMMAND, cls.realtime);
}
