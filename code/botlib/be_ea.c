// Copyright (C) 1999-2005 ID Software, Inc.
// Copyright (C) 2023-2025 Noire.dev
// SourceTech — GPLv2; see LICENSE for details.

#include "../qcommon/q_shared.h"
#include "botlib.h"
#include "be_interface.h"
#include "be_ea.h"

#define MAX_USERMOVE 400

static bot_input_t* botinputs;

void EA_UseItem(int client, const char* it) { botimport.BotClientCommand(client, va("use %s", it)); }

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

void EA_Jump(int client) {
	bot_input_t* bi;

	bi = &botinputs[client];
	bi->actionflags |= ACTION_JUMP;
}

void EA_DelayedJump(int client) {
	bot_input_t* bi;

	bi = &botinputs[client];
	bi->actionflags |= ACTION_DELAYEDJUMP;
}

void EA_Crouch(int client) {
	bot_input_t* bi;

	bi = &botinputs[client];

	bi->actionflags |= ACTION_CROUCH;
}

void EA_Walk(int client) {
	bot_input_t* bi;

	bi = &botinputs[client];

	bi->actionflags |= ACTION_WALK;
}

void EA_MoveUp(int client) {
	bot_input_t* bi;

	bi = &botinputs[client];

	bi->actionflags |= ACTION_MOVEUP;
}

void EA_MoveDown(int client) {
	bot_input_t* bi;

	bi = &botinputs[client];

	bi->actionflags |= ACTION_MOVEDOWN;
}

void EA_MoveForward(int client) {
	bot_input_t* bi;

	bi = &botinputs[client];

	bi->actionflags |= ACTION_MOVEFORWARD;
}

void EA_MoveBack(int client) {
	bot_input_t* bi;

	bi = &botinputs[client];

	bi->actionflags |= ACTION_MOVEBACK;
}

void EA_MoveLeft(int client) {
	bot_input_t* bi;

	bi = &botinputs[client];

	bi->actionflags |= ACTION_MOVELEFT;
}

void EA_MoveRight(int client) {
	bot_input_t* bi;

	bi = &botinputs[client];

	bi->actionflags |= ACTION_MOVERIGHT;
}

void EA_Move(int client, vec3_t dir, float speed) {
	bot_input_t* bi;

	bi = &botinputs[client];

	VectorCopy(dir, bi->dir);
	// cap speed
	if(speed > MAX_USERMOVE)
		speed = MAX_USERMOVE;
	else if(speed < -MAX_USERMOVE)
		speed = -MAX_USERMOVE;
	bi->speed = speed;
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

	bi = &botinputs[client];

	bi->thinktime = 0;
	VectorClear(bi->dir);
	bi->speed = 0;
	bi->actionflags = 0;
}

int EA_Setup(void) {
	// initialize the bot inputs
	botinputs = (bot_input_t*)malloc(botlibglobals.maxclients * sizeof(bot_input_t));
	return BLERR_NOERROR;
}

void EA_Shutdown(void) {
	free(botinputs);
	botinputs = NULL;
}
