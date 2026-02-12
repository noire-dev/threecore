// Copyright (C) 1999-2005 ID Software, Inc.
// Copyright (C) 2023-2026 Noire.dev
// ThreeCore — GPLv2; see LICENSE for details.

#include "q_shared.h"
#include "qcommon.h"

static cvar_t* cvar_vars = NULL;
static cvar_t* cvar_cheats;
int cvar_modifiedFlags;

static cvar_t cvar_indexes[MAX_CVARS];
static int cvar_numIndexes;

static int cvar_group[CVG_MAX];

#define FILE_HASH_SIZE 16384
static cvar_t* hashTable[FILE_HASH_SIZE];

static long generateHashValue(const char* fname) {
    unsigned int hash = 2166136261u;
    
    while (*fname) {
        hash ^= (unsigned char)*fname++;
        hash *= 16777619u;
    }
    
    return hash & (FILE_HASH_SIZE - 1);
}

static qboolean Cvar_ValidateName(const char* name) {
	const char* s;
	int c;

	if(!name) {
		return qfalse;
	}

	s = name;
	while((c = *s++) != '\0') {
		if(c == '\\' || c == '\"' || c == ';' || c == '%' || c <= ' ' || c >= '~') return qfalse;
	}

	if((s - name) >= MAX_STRING_CHARS) {
		return qfalse;
	}

	return qtrue;
}

cvar_t* Cvar_FindVar(const char* var_name) {
	cvar_t* var;
	long hash;

	if(!var_name) return NULL;

	hash = generateHashValue(var_name);

	for(var = hashTable[hash]; var; var = var->hashNext) {
		if(!Q_stricmp(var_name, var->name)) {
			return var;
		}
	}

	return NULL;
}

const char* Cvar_VariableString(const char* var_name) {
	cvar_t* var;

	var = Cvar_FindVar(var_name);
	if(!var) return "";
	return var->string;
}

void Cvar_CommandCompletion(void (*callback)(const char* s)) {
	const cvar_t* cvar;

	for(cvar = cvar_vars; cvar; cvar = cvar->next) {
		if(cvar->name) callback(va("%s = %s", cvar->name, cvar->string));
	}
}

cvar_t* Cvar_Get(const char* var_name, const char* var_value, int flags) {
	cvar_t* var;
	long hash;
	int index;

	if(!var_name || !var_value) Com_Error(ERR_FATAL, "Cvar_Get: NULL parameter");

	if(!Cvar_ValidateName(var_name)) {
		Com_Printf("invalid cvar name string: %s\n", var_name);
		var_name = "BADNAME";
	}

	var = Cvar_FindVar(var_name);

	if(var) {
		var->flags = flags;
		cvar_modifiedFlags |= flags;
		var->resetString = CopyString(var_value);
		if (var->latchedString) {
		    var->string = CopyString(var->latchedString);
		    var->latchedString = NULL;
	        var->modified = CVARMOD_ALL;
	        var->value = atof(var->string);
	        var->integer = atoi(var->string);
	    }
		return var;
	}

	// find a free cvar
	for(index = 0; index < MAX_CVARS; index++) {
		if(!cvar_indexes[index].name) break;
	}

	if(index >= MAX_CVARS) {
		if(!com_errorEntered) Com_Error(ERR_FATAL, "Error: Too many cvars, cannot create a new one!");
		return NULL;
	}

	var = &cvar_indexes[index];

	if(index >= cvar_numIndexes) cvar_numIndexes = index + 1;

	var->name = CopyString(var_name);
	var->string = CopyString(var_value);
	var->modified = CVARMOD_ALL;
	var->value = atof(var->string);
	var->integer = atoi(var->string);
	var->resetString = CopyString(var_value);
	var->description = NULL;
	var->group = CVG_NONE;
	cvar_group[var->group] = 1;

	// link the variable in
	var->next = cvar_vars;
	if(cvar_vars) cvar_vars->prev = var;

	var->prev = NULL;
	cvar_vars = var;

	var->flags = flags;
	cvar_modifiedFlags |= var->flags;

	hash = generateHashValue(var_name);
	var->hashIndex = hash;

	var->hashNext = hashTable[hash];
	if(hashTable[hash]) hashTable[hash]->hashPrev = var;

	var->hashPrev = NULL;
	hashTable[hash] = var;

	return var;
}

static void Cvar_Print(const cvar_t* v) {
	Com_Printf("\"%s\" = \"%s" S_COLOR_WHITE "\"\n", v->name, v->string);

	if(v->resetString) Com_Printf("default:\"%s" S_COLOR_WHITE "\"\n", v->resetString);
	if(v->latchedString) Com_Printf("latched: \"%s\"\n", v->latchedString);
	if(v->description) Com_Printf("%s\n", v->description);
}

cvar_t* Cvar_Set(const char* var_name, const char* value) {
	cvar_t* var;

	if(!Cvar_ValidateName(var_name)) {
		Com_Printf("invalid cvar name string: %s\n", var_name);
		var_name = "BADNAME";
	}

	var = Cvar_FindVar(var_name);
	if(!var) {
		if(!value) return NULL;
		return Cvar_Get(var_name, value, 0);  // create it
	}

	if((var->flags & CVAR_CHEAT) && !cvar_cheats->integer) {
		Com_Printf("%s is cheat protected.\n", var_name);
		return var;
	}

	if(!value) value = var->resetString;
		
	cvar_modifiedFlags |= var->flags;

    if(strcmp(value, var->string) == 0) return var;  // not changed

	if(var->flags & CVAR_LATCH) {
		Com_Printf("%s will be changed upon restarting.\n", var_name);
		var->latchedString = CopyString(value);
		var->modified = CVARMOD_ALL;
		cvar_group[var->group] = 1;
		return var;
	}

	var->modified = CVARMOD_ALL;
	cvar_group[var->group] = 1;

	Z_Free(var->string);  // free the old value string

	var->string = CopyString(value);
	var->value = atof(var->string);
	var->integer = atoi(var->string);

	return var;
}

int Cvar_VariableIntegerValue(const char* var_name) {
	cvar_t* var;

	var = Cvar_FindVar(var_name);
	if(!var) return 0;
	return var->integer;
}

void Cvar_Reset(const char* var_name) { Cvar_Set(var_name, NULL); }

void Cvar_SetCheatState(void) {
	cvar_t* var;

	// set all default vars to the safe value
	for(var = cvar_vars; var; var = var->next) {
		if(var->flags & CVAR_CHEAT) {
			if(var->latchedString) {
				Z_Free(var->latchedString);
				var->latchedString = NULL;
			}
			if(strcmp(var->resetString, var->string)) Cvar_Set(var->name, var->resetString);
		}
	}
}

typedef enum {
	FT_BAD = 0,
	FT_SET,
	FT_RESET,
} funcType_t;

static funcType_t GetFuncType(void) {
	const char* cmd;
	cmd = Cmd_Argv(1);
	if(!Q_stricmp(cmd, "=")) return FT_SET;
	if(!Q_stricmp(cmd, "*")) return FT_RESET;

	return FT_BAD;
}

static void Cvar_SetDescription(cvar_t* var, const char* var_description) {
	if(var_description && var_description[0] != '\0') {
		if(var->description != NULL) Z_Free(var->description);
		var->description = CopyString(var_description);
	}
}

qboolean Cvar_Command(void) {
	cvar_t* v;
	funcType_t ftype;

	v = Cvar_FindVar(Cmd_Argv(0));

	if(Cmd_Argc() == 1 && v) {
		Cvar_Print(v);
		return qtrue;
	} else if(Cmd_Argc() >= 2) {
		ftype = GetFuncType();
		if(ftype == FT_SET) {
			Cvar_Set(Cmd_Argv(0), Cmd_ArgsFrom(2));
			return qtrue;
		} else if(ftype == FT_RESET && v) {
			Cvar_Set(v->name, NULL);
			return qtrue;
		}
	}
	
	return qfalse;
}

static void Cvar_Toggle_f(void) {
	int i, c;
	const char* curval;

	c = Cmd_Argc();
	if(c < 2) {
		Com_Printf("usage: toggle <variable> [value1, value2, ...]\n");
		return;
	}

	if(c == 2) {
		Cvar_Set(Cmd_Argv(1), va("%i", !Cvar_VariableIntegerValue(Cmd_Argv(1))));
		return;
	}

	if(c == 3) {
		Com_Printf("toggle: nothing to toggle to\n");
		return;
	}

	curval = Cvar_VariableString(Cmd_Argv(1));

	// don't bother checking the last arg for a match since the desired
	// behaviour is the same as no match (set to the first argument)
	for(i = 2; i + 1 < c; i++) {
		if(strcmp(curval, Cmd_Argv(i)) == 0) {
			Cvar_Set(Cmd_Argv(1), Cmd_Argv(i + 1));
			return;
		}
	}

	// fallback
	Cvar_Set(Cmd_Argv(1), Cmd_Argv(2));
}

void Cvar_WriteVariables(fileHandle_t f) {
	cvar_t* var;
	char buffer[MAX_CMD_LINE];
	const char* value;

	for(var = cvar_vars; var; var = var->next) {
		if(var->flags & CVAR_ARCHIVE) {
			int len;
			// write the latched value, even if it hasn't taken effect yet
			value = var->latchedString ? var->latchedString : var->string;
			if(strlen(var->name) + strlen(value) + 10 > sizeof(buffer)) {
				Com_Printf(S_COLOR_YELLOW "WARNING: %svalue of variable \"%s\" too long to write to file\n", value == var->latchedString ? "latched " : "", var->name);
				continue;
			}
			if(var->resetString && !strcmp(value, var->resetString)) continue;
			len = Com_sprintf(buffer, sizeof(buffer), "%s = \"%s\"" Q_NEWLINE, var->name, value);

			FS_Write(buffer, len, f);
		}
	}
}

void Cvar_Restart(void) {
	cvar_t* curvar = cvar_vars;

	while(curvar) {
		if(curvar->resetString[0]) Cvar_Set(curvar->name, curvar->resetString);
		curvar = curvar->next;
	}
}

static void Cvar_Restart_f(void) { Cvar_Restart(); }

const char* Cvar_InfoString(int bit, qboolean* truncated) {
	static char info[BIG_INFO_STRING];
	const cvar_t* var;
	qboolean allSet;

	info[0] = '\0';
	allSet = qtrue;

	for(var = cvar_vars; var; var = var->next) {
		if(var->name && (var->flags & bit)) allSet &= Info_SetValueForKey_s(info, sizeof(info), var->name, var->string);
	}

	if(truncated) *truncated = !allSet;

	return info;
}

void Cvar_InfoStringBuffer(int bit, char* buff, int buffsize) { Q_strncpyz(buff, Cvar_InfoString(bit, NULL), buffsize); }

void Cvar_SetGroup(cvar_t* var, cvarGroup_t group) {
	if(group < CVG_MAX) {
		var->group = group;
	} else {
		Com_Error(ERR_DROP, "Bad group index %i for %s", group, var->name);
	}
}

int Cvar_CheckGroup(cvarGroup_t group) {
	if(group < CVG_MAX) {
		return cvar_group[group];
	} else {
		return 0;
	}
}

void Cvar_ResetGroup(cvarGroup_t group) {
	if(group < CVG_MAX) cvar_group[group] = 0;
}

void Cvar_Update(vmCvar_t* vmCvar, int cvarID, int vmIndex) {
	cvar_t* cv = NULL;
	assert(vmCvar);

	cv = cvar_indexes + cvarID;

	if(!(cv->modified & vmIndex)) return;
	if(!cv->string) return;  // variable might have been cleared by a cvar_restart
	cv->modified &= ~vmIndex;

	Q_strncpyz(vmCvar->string, cv->string, sizeof(vmCvar->string));
	vmCvar->value = cv->value;
	vmCvar->integer = cv->integer;
}

void Cvar_Reload(void) {
    cvar_t* var;
    
    for(var = cvar_vars; var; var = var->next) var->modified = CVARMOD_ALL;  
}

int Cvar_ID(const char* name) {
    cvar_t* cv = Cvar_FindVar(name);
    return cv ? cv - cvar_indexes : -1;
}

void Cvar_CompleteCvarName(const char* args, int argNum) {
	if(argNum == 2) {
		const char* p = Com_SkipTokens(args, 1, " ");  // Skip "<cmd> "

		if(p > args) Field_CompleteCommand(p, qfalse, qtrue);
	}
}

void Cvar_Init(void) {
	Com_Memset(cvar_indexes, '\0', sizeof(cvar_indexes));
	Com_Memset(hashTable, '\0', sizeof(hashTable));

	cvar_cheats = Cvar_Get("sv_cheats", "0", CVAR_SYSTEMINFO);
	
	Cmd_AddCommand("toggle", Cvar_Toggle_f);
	Cmd_SetCommandCompletionFunc("toggle", Cvar_CompleteCvarName);
	Cmd_AddCommand("cvar_restart", Cvar_Restart_f);
}
