// Copyright (C) 1999-2005 ID Software, Inc.
// Copyright (C) 2023-2026 Noire.dev
// SourceTech — GPLv2; see LICENSE for details.

// ClientCommand elementary actions
void EA_Command(int client, const char* command);
void EA_Attack(int client);
void EA_Gesture(int client);
void EA_Use(int client);
void EA_SelectWeapon(int client, int weapon);
void EA_View(int client, vec3_t viewangles);
void EA_MoveUp(int client);
void EA_MoveForward(int client);
void EA_Move(int client, vec3_t dir, float speed);

// send regular input to the server
void EA_GetInput(int client, float thinktime, bot_input_t* input);
void EA_ResetInput(int client);

// setup and shutdown routines
int EA_Setup(void);
void EA_Shutdown(void);
