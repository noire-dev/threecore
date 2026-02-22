// Copyright (C) 1999-2005 ID Software, Inc.
// ThreeCore by noire.dev — GPLv2

void EA_Button(int client, int button);
void EA_Command(int client, const char *command );
void EA_Move(int client, vec3_t dir);
void EA_View(int client, vec3_t viewangles);
void EA_GetInput(int client, float thinktime, bot_input_t *input);
void EA_ResetInput(int client);
int EA_Setup(void);
void EA_Shutdown(void);
