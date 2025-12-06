// Copyright (C) 1999-2005 ID Software, Inc.
// Copyright (C) 2023-2026 Noire.dev
// SourceTech — GPLv2; see LICENSE for details.

#include "../qcommon/q_shared.h"
#include "l_memory.h"
#include "l_script.h"
#include "l_precomp.h"
#include "l_struct.h"
#include "botlib.h"
#include "be_interface.h"
#include "be_ea.h"

static bot_input_t* botinputs;

void EA_Gesture(int client) {
	bot_input_t* bi;

	bi = &botinputs[client];
	bi->actionflags |= ACTION_GESTURE;
}

void EA_Command(int client, const char* command) { botimport.BotClientCommand(client, command); }

void EA_SelectWeapon(int client, int weapon) {
	bot_input_t* bi;

	bi = &botinputs[client];
	bi->weapon = weapon;
}

void EA_Attack(int client) {
	bot_input_t* bi;

	bi = &botinputs[client];
	bi->actionflags |= ACTION_ATTACK;
}

void EA_Use(int client) {
	bot_input_t* bi;

	bi = &botinputs[client];
	bi->actionflags |= ACTION_USE;
}

void EA_View(int client, vec3_t viewangles) {
	bot_input_t* bi;

	bi = &botinputs[client];
	VectorCopy(viewangles, bi->viewangles);
}

void EA_GetInput(int client, float thinktime, bot_input_t* input) {
	bot_input_t* bi;

	bi = &botinputs[client];
	bi->thinktime = thinktime;
	Com_Memcpy(input, bi, sizeof(bot_input_t));
}

void EA_ResetInput(int client) {
	bot_input_t* bi;
	int jumped;

	bi = &botinputs[client];
	bi->thinktime = 0;
	VectorClear(bi->dir);
	bi->speed = 0;
	jumped = bi->actionflags & ACTION_JUMP;
	bi->actionflags = 0;
	if(jumped) bi->actionflags |= ACTION_JUMPEDLASTFRAME;
}

int EA_Setup(void) {
	// initialize the bot inputs
	botinputs = (bot_input_t*)GetClearedHunkMemory(botlibglobals.maxclients * sizeof(bot_input_t));
	return BLERR_NOERROR;
}

void EA_Shutdown(void) {
	FreeMemory(botinputs);
	botinputs = NULL;
}
