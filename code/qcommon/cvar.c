// Copyright (C) 1999-2005 ID Software, Inc.
// Copyright (C) 2023-2025 Noire.dev
// SourceTech — GPLv2; see LICENSE for details.

#include "q_shared.h"
#include "qcommon.h"

static cvar_t* cvar_vars = NULL;
static cvar_t* cvar_cheats;
static cvar_t* cvar_developer;
int cvar_modifiedFlags;

static cvar_t cvar_indexes[MAX_CVARS];
static int cvar_numIndexes;

static int cvar_group[CVG_MAX];

#define FILE_HASH_SIZE 256
static cvar_t* hashTable[FILE_HASH_SIZE];

static long generateHashValue(const char* fname) {
	int i;
	long hash;
	char letter;

	hash = 0;
	i = 0;
	while(fname[i] != '\0') {
		letter = locase[(byte)fname[i]];
		hash += (long)(letter) * (i + 119);
		i++;
	}
	hash &= (FILE_HASH_SIZE - 1);
	return hash;
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
		if (var->latchedString) {
		    var->string = CopyString(var->latchedString);
		    var->latchedString = NULL;
	        var->modified = qtrue;
	        var->value = Q_atof(var->string);
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
	var->modified = qtrue;
	var->value = Q_atof(var->string);
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
	Com_Printf("\"%s\" \"%s" S_COLOR_WHITE "\"", v->name, v->string);

	if(!(v->flags & CVAR_ROM)) Com_Printf(" default:\"%s" S_COLOR_WHITE "\"\n", v->resetString);

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

	if((var->flags & CVAR_DEVELOPER) && !cvar_developer->integer) {
		Com_Printf("%s can be set only in developer mode.\n", var_name);
		return var;
	}

	if(!value) value = var->resetString;
		
	cvar_modifiedFlags |= var->flags;

    if(strcmp(value, var->string) == 0) return var;  // not changed

	if(var->flags & CVAR_LATCH) {
		Com_Printf("%s will be changed upon restarting.\n", var_name);
		var->latchedString = CopyString(value);
		var->modified = qtrue;
		cvar_group[var->group] = 1;
		return var;
	}

	var->modified = qtrue;
	cvar_group[var->group] = 1;

	Z_Free(var->string);  // free the old value string

	var->string = CopyString(value);
	var->value = Q_atof(var->string);
	var->integer = atoi(var->string);

	return var;
}

void Cvar_SetValue(const char* var_name, float value) {
	char val[32];

	if(value == (int)value) {
		Com_sprintf(val, sizeof(val), "%i", (int)value);
	} else {
		Com_sprintf(val, sizeof(val), "%f", value);
	}
	Cvar_Set(var_name, val);
}

void Cvar_SetIntegerValue(const char* var_name, int value) {
	char val[32];

	sprintf(val, "%i", value);
	Cvar_Set(var_name, val);
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
			// the CVAR_LATCHED|CVAR_CHEAT vars might escape the reset here
			// because of a different var->latchedString
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
	FT_CREATE,
	FT_SAVE,
	FT_UNSAVE,
	FT_SHARE,
	FT_UNSHARE,
	FT_RESET,
	FT_UNSET,
	FT_ADD,
	FT_SUB,
	FT_MUL,
	FT_DIV,
	FT_MOD,
	FT_SIN,
	FT_COS,
	FT_RAND,
} funcType_t;

static funcType_t GetFuncType(void) {
	const char* cmd;
	cmd = Cmd_Argv(1);
	if(!Q_stricmp(cmd, "=")) return FT_CREATE;
	if(!Q_stricmp(cmd, "-")) return FT_SAVE;
	if(!Q_stricmp(cmd, "--")) return FT_UNSAVE;
	if(!Q_stricmp(cmd, "+")) return FT_SHARE;
	if(!Q_stricmp(cmd, "++")) return FT_UNSHARE;
	if(!Q_stricmp(cmd, "*")) return FT_RESET;
	if(!Q_stricmp(cmd, "**")) return FT_UNSET;
	if(!Q_stricmp(cmd, "+=")) return FT_ADD;
	if(!Q_stricmp(cmd, "-=")) return FT_SUB;
	if(!Q_stricmp(cmd, "*=")) return FT_MUL;
	if(!Q_stricmp(cmd, "/=")) return FT_DIV;
	if(!Q_stricmp(cmd, "%=")) return FT_MOD;
	if(!Q_stricmp(cmd, "s=")) return FT_SIN;
	if(!Q_stricmp(cmd, "c=")) return FT_COS;
	if(!Q_stricmp(cmd, ":=")) return FT_RAND;

	return FT_BAD;
}

static const char* GetValue(int index, float* val) {
	static char buf[MAX_CVAR_VALUE_STRING];
	const char* cmd;

	cmd = Cmd_Argv(index);

	if((*cmd == '-' && *(cmd + 1) == '\0') || *cmd == '\0') {
		*val = 0.0f;
		buf[0] = '\0';
		return NULL;
	}

	*val = Q_atof(cmd);
	Q_strncpyz(buf, cmd, sizeof(buf));
	return buf;
}

static void Cvar_Op(funcType_t ftype, float* val) {
	float cap, mod;

	GetValue(2, &mod);

	switch(ftype) {
		case FT_ADD: *val += mod; break;
		case FT_SUB: *val -= mod; break;
		case FT_MUL: *val *= mod; break;
		case FT_DIV:
			if(mod) *val /= mod;
			break;
		case FT_MOD:
			if(mod) *val = fmodf(*val, mod);
			break;

		case FT_SIN: *val = sin(mod); break;

		case FT_COS: *val = cos(mod); break;
		default: break;
	}

	if(Cmd_Argc() > 3) {  // low bound
		if(GetValue(3, &cap)) {
			if(*val < cap) *val = cap;
		}
	}
	if(Cmd_Argc() > 4) {  // high bound
		if(GetValue(4, &cap)) {
			if(*val > cap) *val = cap;
			if(*val > cap) *val = cap;
		}
	}
}

static void Cvar_Rand(float* val) {
	float cap;

	*val = rand();

	if(Cmd_Argc() > 2) {  // base
		if(GetValue(2, &cap)) *val += cap;
	}
	if(Cmd_Argc() > 3) {  // modulus
		if(GetValue(3, &cap)) {
			if(cap) *val = fmodf(*val, cap);
		}
	}
}

static cvar_t* Cvar_Unset(cvar_t* cv) {
	cvar_t* next = cv->next;
	
	cvar_modifiedFlags |= cv->flags;

	if(cv->name) Z_Free(cv->name);
	if(cv->string) Z_Free(cv->string);
	if(cv->latchedString) Z_Free(cv->latchedString);
	if(cv->resetString) Z_Free(cv->resetString);
	if(cv->description) Z_Free(cv->description);

	if(cv->prev)
		cv->prev->next = cv->next;
	else
		cvar_vars = cv->next;
	if(cv->next) cv->next->prev = cv->prev;

	if(cv->hashPrev)
		cv->hashPrev->hashNext = cv->hashNext;
	else
		hashTable[cv->hashIndex] = cv->hashNext;
	if(cv->hashNext) cv->hashNext->hashPrev = cv->hashPrev;

	Com_Memset(cv, '\0', sizeof(*cv));

	return next;
}

qboolean Cvar_Command(void) {
	cvar_t* v;
	funcType_t ftype;
	char value[MAX_CVAR_VALUE_STRING];
	float val;

	v = Cvar_FindVar(Cmd_Argv(0));

	if(Cmd_Argc() == 1 && v) {
		Cvar_Print(v);
		return qtrue;
	} else if(Cmd_Argc() >= 2) {
		ftype = GetFuncType();
		if(ftype == FT_CREATE) {
			Cvar_Set(Cmd_Argv(0), Cmd_ArgsFrom(2));
			return qtrue;
		} else if(ftype == FT_SAVE) {
			v = Cvar_Set(Cmd_Argv(0), Cmd_ArgsFrom(2));
			if(v && !(v->flags & CVAR_ARCHIVE)) {
				v->flags |= CVAR_ARCHIVE;
				cvar_modifiedFlags |= CVAR_ARCHIVE;
			}
			return qtrue;
		} else if(ftype == FT_UNSAVE) {
			v = Cvar_Set(Cmd_Argv(0), Cmd_ArgsFrom(2));
			if(v && (v->flags & CVAR_ARCHIVE)) {
				v->flags &= ~CVAR_ARCHIVE;
				cvar_modifiedFlags &= ~CVAR_ARCHIVE;
			}
			return qtrue;
		} else if(ftype == FT_SHARE) {
			v = Cvar_Set(Cmd_Argv(0), Cmd_ArgsFrom(2));
			if(v && !(v->flags & CVAR_SYSTEMINFO)) {
				v->flags |= CVAR_SYSTEMINFO;
				cvar_modifiedFlags |= CVAR_SYSTEMINFO;
			}
			return qtrue;
		} else if(ftype == FT_UNSHARE) {
			v = Cvar_Set(Cmd_Argv(0), Cmd_ArgsFrom(2));
			if(v && (v->flags & CVAR_SYSTEMINFO)) {
				v->flags &= ~CVAR_SYSTEMINFO;
				cvar_modifiedFlags &= ~CVAR_SYSTEMINFO;
			}
			return qtrue;
		} else if(ftype == FT_RESET && v) {
			Cvar_Set(v->name, NULL);
			return qtrue;
		} else if(ftype == FT_UNSET && v) {
			Cvar_Unset(v);
			return qtrue;
		}
	}

	if(!v) return qfalse;

	ftype = GetFuncType();
	if(ftype == FT_BAD) {
		Cvar_Set(v->name, Cmd_ArgsFrom(1));
		return qtrue;
	} else {
		val = v->value;

		if(ftype == FT_RAND)
			Cvar_Rand(&val);
		else
			Cvar_Op(ftype, &val);

		sprintf(value, "%g", val);

		Cvar_Set(v->name, value);
		return qtrue;
	}
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
			if(!strcmp(value, var->resetString)) continue;
			len = Com_sprintf(buffer, sizeof(buffer), "%s - \"%s\"" Q_NEWLINE, var->name, value);

			FS_Write(buffer, len, f);
		}
	}
}

void Cvar_Restart(qboolean unsetVM) {
	cvar_t* curvar = cvar_vars;

	while(curvar) {
		if(curvar->resetString[0]) {
			Cvar_Set(curvar->name, curvar->resetString);
		} else {
			curvar = Cvar_Unset(curvar);
			continue;
		}

		curvar = curvar->next;
	}
}

static void Cvar_Restart_f(void) { Cvar_Restart(qfalse); }

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

void Cvar_SetDescription(cvar_t* var, const char* var_description) {
	if(var_description && var_description[0] != '\0') {
		if(var->description != NULL) {
			Z_Free(var->description);
		}
		var->description = CopyString(var_description);
	}
}

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

void Cvar_ResetGroup(cvarGroup_t group, qboolean resetModifiedFlags) {
	if(group < CVG_MAX) {
		cvar_group[group] = 0;
		if(resetModifiedFlags) {
			int i;
			for(i = 0; i < cvar_numIndexes; i++) {
				if(cvar_indexes[i].group == group && cvar_indexes[i].name) {
					cvar_indexes[i].modified = qfalse;
				}
			}
		}
	}
}

void Cvar_Update(vmCvar_t* vmCvar, int cvarID) {
	cvar_t* cv = NULL;
	assert(vmCvar);

	cv = cvar_indexes + cvarID;

	if(!cv->modified) return;
	if(!cv->string) return;  // variable might have been cleared by a cvar_restart
	cv->modified = qfalse;

	Q_strncpyz(vmCvar->string, cv->string, sizeof(vmCvar->string));
	vmCvar->value = cv->value;
	vmCvar->integer = cv->integer;
}

void Cvar_Reload(void) {
    cvar_t* var;
    
    for(var = cvar_vars; var; var = var->next) {
        var->modified = qtrue;  
    }
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
	Cvar_SetDescription(cvar_cheats, "Enable cheating commands (server side only).");
	cvar_developer = Cvar_Get("developer", "0", 0);
	Cvar_SetDescription(cvar_developer, "Toggles developer mode. Prints more info to console and provides more commands.");

	Cmd_AddCommand("toggle", Cvar_Toggle_f);
	Cmd_SetCommandCompletionFunc("toggle", Cvar_CompleteCvarName);

	Cmd_AddCommand("cvar_restart", Cvar_Restart_f);
}
