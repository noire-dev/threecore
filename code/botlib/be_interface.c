// Copyright (C) 1999-2005 ID Software, Inc.
// Copyright (C) 2023-2026 Noire.dev
// SourceTech — GPLv2; see LICENSE for details.

#include "../qcommon/q_shared.h"
#include "l_memory.h"
#include "l_log.h"
#include "l_libvar.h"
#include "l_script.h"
#include "l_precomp.h"
#include "l_struct.h"
#include "aasfile.h"
#include "botlib.h"
#include "be_aas.h"
#include "be_aas_funcs.h"
#include "be_aas_def.h"
#include "be_interface.h"
#include "be_ea.h"
#include "be_ai_goal.h"
#include "be_ai_move.h"

botlib_globals_t botlibglobals;
botlib_export_t be_botlib_export;
botlib_import_t botimport;

int botlibsetup = qfalse;

int Sys_MilliSeconds(void) { return clock() * 1000 / CLOCKS_PER_SEC; }

static qboolean ValidEntityNumber(int num, const char* str) {
	if((unsigned)num > botlibglobals.maxentities) {
		botimport.Print(PRT_ERROR, "%s: invalid entity number %d, [0, %d]\n", str, num, botlibglobals.maxentities);
		return qfalse;
	}
	return qtrue;
}

static qboolean BotLibSetup(const char* str) {
	if(!botlibglobals.botlibsetup) {
		botimport.Print(PRT_ERROR, "%s: bot library used before being setup\n", str);
		return qfalse;
	}
	return qtrue;
}

static int Export_BotLibSetup(void) {
	int errnum;

	memset(&botlibglobals, 0, sizeof(botlibglobals));

	botlibglobals.maxclients = (int)LibVarValue("maxclients", "128");
	botlibglobals.maxentities = (int)LibVarValue("maxentities", "4096");

	errnum = AAS_Setup();
	if(errnum != BLERR_NOERROR) return errnum;
	errnum = EA_Setup();
	if(errnum != BLERR_NOERROR) return errnum;
	errnum = BotSetupMoveAI();
	if(errnum != BLERR_NOERROR) return errnum;

	botlibsetup = qtrue;
	botlibglobals.botlibsetup = qtrue;

	return BLERR_NOERROR;
}

static int Export_BotLibShutdown(void) {
	if(!botlibglobals.botlibsetup) return BLERR_LIBRARYNOTSETUP;

	BotShutdownMoveAI();
	AAS_Shutdown();
	EA_Shutdown();
	LibVarDeAllocAll();
	PC_RemoveAllGlobalDefines();
	Log_Shutdown();
	botlibsetup = qfalse;
	botlibglobals.botlibsetup = qfalse;
	PC_CheckOpenSourceHandles();
	return BLERR_NOERROR;
}

static int Export_BotLibVarSet(const char* var_name, const char* value) {
	LibVarSet(var_name, value);
	return BLERR_NOERROR;
}

static int Export_BotLibStartFrame(float time) {
	if(!BotLibSetup("BotStartFrame")) return BLERR_LIBRARYNOTSETUP;
	return AAS_StartFrame(time);
}

static int Export_BotLibLoadMap(const char* mapname) {
	int errnum;

	if(!BotLibSetup("BotLoadMap")) return BLERR_LIBRARYNOTSETUP;
	errnum = AAS_LoadMap(mapname);
	if(errnum != BLERR_NOERROR) return errnum;
	BotSetBrushModelTypes();
	return BLERR_NOERROR;
}

static int Export_BotLibUpdateEntity(int ent, bot_entitystate_t* state) {
	if(!BotLibSetup("BotUpdateEntity")) return BLERR_LIBRARYNOTSETUP;
	if(!ValidEntityNumber(ent, "BotUpdateEntity")) return BLERR_INVALIDENTITYNUMBER;

	return AAS_UpdateEntity(ent, state);
}

void ElevatorBottomCenter(aas_reachability_t* reach, vec3_t bottomcenter);
int BotGetReachabilityToGoal(vec3_t origin, int areanum, int lastgoalareanum, int lastareanum, bot_goal_t* goal, int travelflags, int* flags);
int AAS_PointLight(vec3_t origin, int* red, int* green, int* blue);
int AAS_TraceAreas(vec3_t start, vec3_t end, int* areas, vec3_t* points, int maxareas);
int AAS_Reachability_WeaponJump(int area1num, int area2num);
int BotFuzzyPointReachabilityArea(vec3_t origin);
float BotGapDistance(vec3_t origin, vec3_t hordir, int entnum);
void AAS_FloodAreas(vec3_t origin);

static void Init_AAS_Export(aas_export_t* aas) {
	aas->AAS_Initialized = AAS_Initialized;
	aas->AAS_Time = AAS_Time;
	aas->AAS_PointAreaNum = AAS_PointAreaNum;
	aas->AAS_TraceAreas = AAS_TraceAreas;
}

static void Init_EA_Export(ea_export_t* ea) {
	ea->EA_Command = EA_Command;
	ea->EA_Gesture = EA_Gesture;
	ea->EA_Attack = EA_Attack;
	ea->EA_Use = EA_Use;
	ea->EA_SelectWeapon = EA_SelectWeapon;
	ea->EA_View = EA_View;
	ea->EA_GetInput = EA_GetInput;
	ea->EA_ResetInput = EA_ResetInput;
}

static void Init_AI_Export(ai_export_t* ai) {
	ai->BotTouchingGoal = BotTouchingGoal;
	ai->BotMoveToGoal = BotMoveToGoal;
	ai->BotResetMoveState = BotResetMoveState;
	ai->BotAllocMoveState = BotAllocMoveState;
	ai->BotFreeMoveState = BotFreeMoveState;
	ai->BotInitMoveState = BotInitMoveState;
}

botlib_export_t* GetBotLibAPI(botlib_import_t* import) {
	assert(import);
	botimport = *import;
	assert(botimport.Print);

	Com_Memset(&be_botlib_export, 0, sizeof(be_botlib_export));

	Init_AAS_Export(&be_botlib_export.aas);
	Init_EA_Export(&be_botlib_export.ea);
	Init_AI_Export(&be_botlib_export.ai);

	be_botlib_export.BotLibSetup = Export_BotLibSetup;
	be_botlib_export.BotLibShutdown = Export_BotLibShutdown;
	be_botlib_export.BotLibStartFrame = Export_BotLibStartFrame;
	be_botlib_export.BotLibLoadMap = Export_BotLibLoadMap;
	be_botlib_export.BotLibUpdateEntity = Export_BotLibUpdateEntity;

	return &be_botlib_export;
}
