// Copyright (C) 1999-2005 ID Software, Inc.
// Copyright (C) 2023-2026 Noire.dev
// SourceTech — GPLv2; see LICENSE for details.

#include "client.h"

static void CL_KeyDownEvent(int key) {
	keys[key].down = qtrue;

	// escape is always handled special
	if(key == K_ESCAPE) {
		// escape always gets out of CGAME stuff
		if(Key_GetCatcher() & KEYCATCH_CGAME) {
			Key_SetCatcher(Key_GetCatcher() & ~KEYCATCH_CGAME);
			return;
		}

		if(!(Key_GetCatcher() & KEYCATCH_UI)) {
			if(cls.state == CA_ACTIVE && !clc.demoplaying) {
				VM_Call(uivm, 1, UI_SET_ACTIVE_MENU, UIMENU_INGAME);
			} else if(cls.state != CA_DISCONNECTED) {
				Cmd_Clear();
				if(!CL_Disconnect(qfalse)) CL_FlushMemory();
				VM_Call(uivm, 1, UI_SET_ACTIVE_MENU, UIMENU_MAIN);
			}
			return;
		}

		VM_Call(uivm, 2, UI_KEY_EVENT, key, qtrue);
		return;
	}

	// distribute the key down event to the appropriate handler
	if(Key_GetCatcher() & KEYCATCH_UI) {
		if(uivm) VM_Call(uivm, 2, UI_KEY_EVENT, key, qtrue);
	} else {
		Key_ParseBinding(key, qtrue);
	}
}

static void CL_KeyUpEvent(int key) {
	keys[key].down = qfalse;

	if(cls.state != CA_DISCONNECTED) Key_ParseBinding(key, qfalse);
	if(Key_GetCatcher() & KEYCATCH_UI && uivm) VM_Call(uivm, 2, UI_KEY_EVENT, key, qfalse);
}

void CL_KeyEvent(int key, qboolean down, unsigned time) {
	if(down) CL_KeyDownEvent(key, time);
	else CL_KeyUpEvent(key, time);
}

void CL_CharEvent(int key) {
	if(key == 127) return;
	if(Key_GetCatcher() & KEYCATCH_UI) VM_Call(uivm, 2, UI_KEY_EVENT, key | K_CHAR_FLAG, qtrue);
}

void Key_ClearStates(void) {
	int i;

	for(i = 0; i < MAX_KEYS; i++) {
		if(keys[i].down) CL_KeyEvent(i, qfalse, 0);

		keys[i].down = qfalse;
	}
}

static int keyCatchers = 0;

int Key_GetCatcher(void) { return keyCatchers; }

void Key_SetCatcher(int catcher) {
	if(catcher != keyCatchers) Key_ClearStates();
	keyCatchers = catcher;
}
