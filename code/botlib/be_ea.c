// Copyright (C) 1999-2005 ID Software, Inc.
// ThreeCore by noire.dev — GPLv2

#include "../qcommon/q_shared.h"
#include "l_memory.h"
#include "l_script.h"
#include "l_precomp.h"
#include "l_struct.h"
#include "botlib.h"
#include "be_interface.h"
#include "be_ea.h"

static bot_input_t *botinputs;

void EA_Button(int client, int button) {
	bot_input_t *bi;
	bi = &botinputs[client];
	bi->actionflags |= button;
}

void EA_Command(int client, const char *command) {
	botimport.BotClientCommand( client, command );
}

void EA_Move(int client, vec3_t dir) {
	bot_input_t *bi;
	vec3_t forwardvec, rightvec;
	float forward, right;
	vec3_t angles;

	bi = &botinputs[client];
	bi->actionflags &= ~(BMOVE_W | BMOVE_S | BMOVE_A | BMOVE_D);

	VectorCopy(bi->viewangles, angles);
	AngleVectors(angles, forwardvec, rightvec, NULL);

	forward = DotProduct(dir, forwardvec);
	right = DotProduct(dir, rightvec);

	if (forward > 0.0f) bi->actionflags |= BMOVE_W;
	else if (forward < 0.0f) bi->actionflags |= BMOVE_S;

	if (right > 0.0f) bi->actionflags |= BMOVE_D;
	else if (right < 0.0f) bi->actionflags |= BMOVE_A;
}

void EA_View(int client, vec3_t viewangles) {
	bot_input_t *bi;
	bi = &botinputs[client];
	VectorCopy(viewangles, bi->viewangles);
}

void EA_GetInput(int client, float thinktime, bot_input_t *input) {
	bot_input_t *bi;

	bi = &botinputs[client];
	bi->thinktime = thinktime;
	Com_Memcpy(input, bi, sizeof(bot_input_t));
}

void EA_ResetInput(int client) {
	bot_input_t *bi;
	bi = &botinputs[client];
	bi->thinktime = 0;
	bi->actionflags = 0;
}

int EA_Setup(void) {
	botinputs = (bot_input_t *) GetClearedHunkMemory(botlibglobals.maxclients * sizeof(bot_input_t));
	return BLERR_NOERROR;
}

void EA_Shutdown(void) {
	FreeMemory(botinputs);
	botinputs = NULL;
}
