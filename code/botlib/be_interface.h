// Copyright (C) 1999-2005 ID Software, Inc.
// Copyright (C) 2023-2026 Noire.dev
// SourceTech — GPLv2; see LICENSE for details.

typedef struct botlib_globals_s {
	int botlibsetup;						//true when the bot library has been setup
	int maxentities;						//maximum number of entities
	int maxclients;							//maximum number of clients
	float time;								//the global time
} botlib_globals_t;

extern botlib_globals_t botlibglobals;
extern botlib_import_t botimport;

int Sys_MilliSeconds(void);
