/*
===========================================================================
Copyright (C) 1999-2005 Id Software, Inc.

This file is part of Quake III Arena source code.

Quake III Arena source code is free software; you can redistribute it
and/or modify it under the terms of the GNU General Public License as
published by the Free Software Foundation; either version 2 of the License,
or (at your option) any later version.

Quake III Arena source code is distributed in the hope that it will be
useful, but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with Quake III Arena source code; if not, write to the Free Software
Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
===========================================================================
*/

#include "q_shared.h"
#include "qcommon.h"

static cvar_t* cvar_vars = NULL;
static cvar_t* cvar_cheats;
static cvar_t* cvar_developer;
int cvar_modifiedFlags;

#define MAX_CVARS 4096
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

float Cvar_VariableValue(const char* var_name) {
	cvar_t* var;

	var = Cvar_FindVar(var_name);
	if(!var) return 0;
	return var->value;
}

int Cvar_VariableIntegerValue(const char* var_name) {
	cvar_t* var;

	var = Cvar_FindVar(var_name);
	if(!var) return 0;
	return var->integer;
}

const char* Cvar_VariableString(const char* var_name) {
	cvar_t* var;

	var = Cvar_FindVar(var_name);
	if(!var) return "";
	return var->string;
}

void Cvar_VariableStringBuffer(const char* var_name, char* buffer, int bufsize) {
	cvar_t* var;

	var = Cvar_FindVar(var_name);
	if(!var) {
		*buffer = '\0';
	} else {
		Q_strncpyz(buffer, var->string, bufsize);
	}
}

void Cvar_VariableStringBufferSafe(const char* var_name, char* buffer, int bufsize, int flag) {
	cvar_t* var;

	var = Cvar_FindVar(var_name);
	if(!var || var->flags & flag) {
		*buffer = '\0';
	} else {
		Q_strncpyz(buffer, var->string, bufsize);
	}
}

unsigned Cvar_Flags(const char* var_name) {
	const cvar_t* var;

	if((var = Cvar_FindVar(var_name)) == NULL)
		return CVAR_NONEXISTENT;
	else {
		if(var->modified)
			return var->flags | CVAR_MODIFIED;
		else
			return var->flags;
	}
}

void Cvar_CommandCompletion(void (*callback)(const char* s)) {
	const cvar_t* cvar;

	for(cvar = cvar_vars; cvar; cvar = cvar->next) {
		if(cvar->name && (cvar->flags & CVAR_NOTABCOMPLETE) == 0) {
			callback(cvar->name);
		}
	}
}

cvar_t* Cvar_Get(const char* var_name, const char* var_value, int flags) {
	cvar_t* var;
	long hash;
	int index;

	if(!var_name || !var_value) {
		Com_Error(ERR_FATAL, "Cvar_Get: NULL parameter");
	}

	if(!Cvar_ValidateName(var_name)) {
		Com_Printf("invalid cvar name string: %s\n", var_name);
		var_name = "BADNAME";
	}

	var = Cvar_FindVar(var_name);

	if(var) {
		int vm_created = (flags & CVAR_VM_CREATED);

		// Make sure the game code cannot mark engine-added variables as gamecode vars
		if(var->flags & CVAR_VM_CREATED) {
			if(!vm_created) var->flags &= ~CVAR_VM_CREATED;
		} else if(!(var->flags & CVAR_USER_CREATED)) {
			if(vm_created) flags &= ~CVAR_VM_CREATED;
		}

		// if the C code is now specifying a variable that the user already
		// set a value for, take the new value as the reset value
		if(var->flags & CVAR_USER_CREATED) {
			var->flags &= ~CVAR_USER_CREATED;
			Z_Free(var->resetString);
			var->resetString = CopyString(var_value);

			if(flags & CVAR_ROM || ((flags & CVAR_DEVELOPER) && !cvar_developer->integer)) {
				// this variable was set by the user,
				// so force it to value given by the engine.

				if(var->latchedString) Z_Free(var->latchedString);

				var->latchedString = CopyString(var_value);
			}
		}

		// Make sure servers cannot mark engine-added variables as SERVER_CREATED
		if(var->flags & CVAR_SERVER_CREATED) {
			if(!(flags & CVAR_SERVER_CREATED)) {
				// reset server-created flag
				var->flags &= ~CVAR_SERVER_CREATED;
				if(vm_created) {
					// reset to state requested by local VM module
					var->flags &= ~CVAR_ROM;
					Z_Free(var->resetString);
					var->resetString = CopyString(var_value);
					if(var->latchedString) Z_Free(var->latchedString);
					var->latchedString = CopyString(var_value);
				}
			}
		} else {
			if(flags & CVAR_SERVER_CREATED) flags &= ~CVAR_SERVER_CREATED;
		}

		var->flags |= flags;

		// only allow one non-empty reset string without a warning
		if(!var->resetString[0]) {
			// we don't have a reset string yet
			Z_Free(var->resetString);
			var->resetString = CopyString(var_value);
		} else if(var_value[0] && strcmp(var->resetString, var_value)) {
			Com_DPrintf("Warning: cvar \"%s\" given initial values: \"%s\" and \"%s\"\n", var_name, var->resetString, var_value);
		}

		// if we have a latched string, take that value now
		if(var->latchedString) {
			char* s;

			s = var->latchedString;
			var->latchedString = NULL;  // otherwise cvar_set2 would free it
			Cvar_Set2(var_name, s, qtrue);
			Z_Free(s);
		}

		// ZOID--needs to be set so that cvars the game sets as
		// SERVERINFO get sent to clients
		cvar_modifiedFlags |= flags;

		return var;
	}

	//
	// allocate a new cvar
	//

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
	var->modificationCount = 1;
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
	// note what types of cvars have been modified (userinfo, archive, serverinfo, systeminfo)
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

	if(!(v->flags & CVAR_ROM)) {
		Com_Printf(" default:\"%s" S_COLOR_WHITE "\"\n", v->resetString);
	}

	if(v->latchedString) {
		Com_Printf("latched: \"%s\"\n", v->latchedString);
	}

	if(v->description) {
		Com_Printf("%s\n", v->description);
	}
}

cvar_t* Cvar_Set2(const char* var_name, const char* value, qboolean force) {
	cvar_t* var;

	if(!Cvar_ValidateName(var_name)) {
		Com_Printf("invalid cvar name string: %s\n", var_name);
		var_name = "BADNAME";
	}

	var = Cvar_FindVar(var_name);
	if(!var) {
		if(!value) return NULL;
		// create it
		if(!force)
			return Cvar_Get(var_name, value, CVAR_USER_CREATED);
		else
			return Cvar_Get(var_name, value, 0);
	}

	if(var->flags & (CVAR_ROM | CVAR_INIT | CVAR_CHEAT | CVAR_DEVELOPER) && !force) {
		if(var->flags & CVAR_ROM) {
			Com_Printf("%s is read only.\n", var_name);
			return var;
		}

		if(var->flags & CVAR_INIT) {
			Com_Printf("%s is write protected.\n", var_name);
			return var;
		}

		if((var->flags & CVAR_CHEAT) && !cvar_cheats->integer) {
			Com_Printf("%s is cheat protected.\n", var_name);
			return var;
		}

		if((var->flags & CVAR_DEVELOPER) && !cvar_developer->integer) {
			Com_Printf("%s can be set only in developer mode.\n", var_name);
			return var;
		}
	}

	if(!value) value = var->resetString;

	if((var->flags & CVAR_LATCH) && var->latchedString) {
		if(strcmp(value, var->string) == 0) {
			Z_Free(var->latchedString);
			var->latchedString = NULL;
			return var;
		}

		if(strcmp(value, var->latchedString) == 0) return var;
	} else if(strcmp(value, var->string) == 0)
		return var;

	// note what types of cvars have been modified (userinfo, archive, serverinfo, systeminfo)
	cvar_modifiedFlags |= var->flags;

	if(!force) {
		if(var->flags & CVAR_LATCH) {
			if(var->latchedString) {
				if(strcmp(value, var->latchedString) == 0) return var;
				Z_Free(var->latchedString);
			} else {
				if(strcmp(value, var->string) == 0) return var;
			}

			Com_Printf("%s will be changed upon restarting.\n", var_name);
			var->latchedString = CopyString(value);
			var->modified = qtrue;
			var->modificationCount++;
			cvar_group[var->group] = 1;
			return var;
		}
	} else {
		if(var->latchedString) {
			Z_Free(var->latchedString);
			var->latchedString = NULL;
		}
	}

	if(strcmp(value, var->string) == 0) return var;  // not changed

	var->modified = qtrue;
	var->modificationCount++;
	cvar_group[var->group] = 1;

	Z_Free(var->string);  // free the old value string

	var->string = CopyString(value);
	var->value = Q_atof(var->string);
	var->integer = atoi(var->string);

	return var;
}

void Cvar_Set(const char* var_name, const char* value) { Cvar_Set2(var_name, value, qtrue); }

void Cvar_SetSafe(const char* var_name, const char* value) {
	unsigned flags = Cvar_Flags(var_name);
	qboolean force = qtrue;

	if(flags != CVAR_NONEXISTENT) {
		if(flags & (CVAR_PROTECTED | CVAR_PRIVATE)) {
			if(value)
				Com_Printf(S_COLOR_YELLOW "Restricted source tried to set "
				                          "\"%s\" to \"%s\"\n",
				           var_name,
				           value);
			else
				Com_Printf(S_COLOR_YELLOW "Restricted source tried to "
				                          "modify \"%s\"\n",
				           var_name);
			return;
		}
	}

	Cvar_Set2(var_name, value, force);
}

void Cvar_SetLatched(const char* var_name, const char* value) { Cvar_Set2(var_name, value, qfalse); }

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

void Cvar_SetValueSafe(const char* var_name, float value) {
	char val[32];

	if(Q_isintegral(value))
		Com_sprintf(val, sizeof(val), "%i", (int)value);
	else
		Com_sprintf(val, sizeof(val), "%f", value);
	Cvar_SetSafe(var_name, val);
}

qboolean Cvar_SetModified(const char* var_name, qboolean modified) {
	cvar_t* var;

	var = Cvar_FindVar(var_name);
	if(var) {
		var->modified = modified;
		return qtrue;
	} else {
		return qfalse;
	}
}

void Cvar_Reset(const char* var_name) { Cvar_Set2(var_name, NULL, qfalse); }

void Cvar_ForceReset(const char* var_name) { Cvar_Set2(var_name, NULL, qtrue); }

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
		case FT_ADD:
			*val += mod;
			break;
		case FT_SUB:
			*val -= mod;
			break;
		case FT_MUL:
			*val *= mod;
			break;
		case FT_DIV:
			if(mod) *val /= mod;
			break;
		case FT_MOD:
			if(mod) *val = fmodf(*val, mod);
			break;

		case FT_SIN:
			*val = sin(mod);
			break;

		case FT_COS:
			*val = cos(mod);
			break;
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

	// note what types of cvars have been modified (userinfo, archive, serverinfo, systeminfo)
	cvar_modifiedFlags |= cv->flags;

	if(cv->name) Z_Free(cv->name);
	if(cv->string) Z_Free(cv->string);
	if(cv->latchedString) Z_Free(cv->latchedString);
	if(cv->resetString) Z_Free(cv->resetString);
	if(cv->description) Z_Free(cv->description);
	if(cv->mins) Z_Free(cv->mins);
	if(cv->maxs) Z_Free(cv->maxs);

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
		    Cvar_Set2(Cmd_Argv(0), Cmd_ArgsFrom(2), qfalse);
		    return qtrue;
	    } else if (ftype == FT_SAVE) {
	        v = Cvar_Set2(Cmd_Argv(0), Cmd_ArgsFrom(2), qfalse);
	        if(v && !(v->flags & CVAR_ARCHIVE)) {
				v->flags |= CVAR_ARCHIVE;
				cvar_modifiedFlags |= CVAR_ARCHIVE;
			}
			return qtrue;
	    } else if (ftype == FT_UNSAVE) {
	        v = Cvar_Set2(Cmd_Argv(0), Cmd_ArgsFrom(2), qfalse);
	        if(v && (v->flags & CVAR_ARCHIVE)) {
				v->flags &= ~CVAR_ARCHIVE;
				cvar_modifiedFlags |= CVAR_ARCHIVE;
			}
			return qtrue;
	    } else if(ftype == FT_RESET && v) {
	        Cvar_Set2(v->name, NULL, qfalse);
		    return qtrue;
	    } else if(ftype == FT_UNSET && v) {
		    Cvar_Unset(v);
		    return qtrue;
	    }
	}
	
	if(!v) return qfalse;

	ftype = GetFuncType();
	if(ftype == FT_BAD) {
		Cvar_Set2(v->name, Cmd_ArgsFrom(1), qfalse);
		return qtrue;
	} else {
		val = v->value;

		if(ftype == FT_RAND)
			Cvar_Rand(&val);
		else
			Cvar_Op(ftype, &val);

		sprintf(value, "%g", val); 

		Cvar_Set2(v->name, value, qfalse);
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
		Cvar_Set2(Cmd_Argv(1), va("%d", !Cvar_VariableValue(Cmd_Argv(1))), qfalse);
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
			Cvar_Set2(Cmd_Argv(1), Cmd_Argv(i + 1), qfalse);
			return;
		}
	}

	// fallback
	Cvar_Set2(Cmd_Argv(1), Cmd_Argv(2), qfalse);
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
			if((var->flags & CVAR_NODEFAULT) && !strcmp(value, var->resetString)) {
				continue;
			}
			len = Com_sprintf(buffer, sizeof(buffer), "%s - \"%s\"" Q_NEWLINE, var->name, value);

			FS_Write(buffer, len, f);
		}
	}
}

static void Cvar_List_f(void) {
	cvar_t* var;
	int i;

	i = 0;
	for(var = cvar_vars; var; var = var->next, i++) {
		if(!var->name) continue;

		if(var->flags & CVAR_SERVERINFO) Com_Printf("S");
		if(var->flags & CVAR_USERINFO) Com_Printf("U");
		if(var->flags & CVAR_ARCHIVE) Com_Printf("A");
		if(var->flags & CVAR_CHEAT) Com_Printf("C");
		if(Q_stricmp(var->string, var->resetString)) Com_Printf("*");

		Com_Printf(" %s \"%s\"\n", var->name, var->string);
	}

	Com_Printf("\n%i total cvars\n", i);
}

void Cvar_Restart(qboolean unsetVM) {
	cvar_t* curvar = cvar_vars;

	while(curvar) {
		if((curvar->flags & CVAR_USER_CREATED) || (unsetVM && (curvar->flags & CVAR_VM_CREATED))) {
			// throw out any variables the user/vm created
			curvar = Cvar_Unset(curvar);
			continue;
		}

		if(!(curvar->flags & (CVAR_ROM | CVAR_INIT | CVAR_NORESTART))) {
			// Just reset the rest to their default values.
			Cvar_Set2(curvar->name, curvar->resetString, qfalse);
		}

		curvar = curvar->next;
	}
}

static void Cvar_Restart_f(void) { Cvar_Restart(qfalse); }

const char* Cvar_InfoString(int bit, qboolean* truncated) {
	static char info[MAX_INFO_STRING];
	const cvar_t* user_vars[MAX_CVARS];
	const cvar_t* vm_vars[MAX_CVARS];
	const cvar_t* var;
	int user_count;
	int vm_count;
	int i;
	qboolean allSet;

	info[0] = '\0';
	user_count = 0;
	vm_count = 0;
	allSet = qtrue;  // this will be qfalse on overflow

	for(var = cvar_vars; var; var = var->next) {
		if(var->name && (var->flags & bit)) {
			// put vm/user-created cvars to the end
			if(var->flags & (CVAR_USER_CREATED | CVAR_VM_CREATED)) {
				if(var->flags & CVAR_USER_CREATED)
					user_vars[user_count++] = var;
				else
					vm_vars[vm_count++] = var;
			} else {
				allSet &= Info_SetValueForKey(info, var->name, var->string);
			}
		}
	}

	// add vm-created cvars
	for(i = 0; i < vm_count; i++) {
		var = vm_vars[i];
		allSet &= Info_SetValueForKey(info, var->name, var->string);
	}

	// add user-created cvars
	for(i = 0; i < user_count; i++) {
		var = user_vars[i];
		allSet &= Info_SetValueForKey(info, var->name, var->string);
	}

	if(truncated) {
		*truncated = !allSet;
	}

	return info;
}

const char* Cvar_InfoString_Big(int bit, qboolean* truncated) {
	static char info[BIG_INFO_STRING];
	const cvar_t* var;
	qboolean allSet;

	info[0] = '\0';
	allSet = qtrue;

	for(var = cvar_vars; var; var = var->next) {
		if(var->name && (var->flags & bit)) allSet &= Info_SetValueForKey_s(info, sizeof(info), var->name, var->string);
	}

	if(truncated) {
		*truncated = !allSet;
	}

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

void Cvar_Register(vmCvar_t* vmCvar, const char* varName, const char* defaultValue, int flags, int privateFlag) {
	cvar_t* cv;

	cv = Cvar_FindVar(varName);

	// Don't modify cvar if it's protected.
	cv = Cvar_Get(varName, defaultValue, flags | CVAR_VM_CREATED);

	if(!vmCvar) return;

	vmCvar->handle = cv - cvar_indexes;
	vmCvar->modificationCount = -1;

	Cvar_Update(vmCvar, 0);
}

void Cvar_Update(vmCvar_t* vmCvar, int privateFlag) {
	size_t len;
	cvar_t* cv = NULL;
	assert(vmCvar);

	if((unsigned)vmCvar->handle >= cvar_numIndexes) {
		// Com_Printf( S_COLOR_YELLOW "Cvar_Update: handle out of range\n");
		return;
	}

	cv = cvar_indexes + vmCvar->handle;

	if(cv->modificationCount == vmCvar->modificationCount) {
		return;
	}
	if(!cv->string) {
		return;  // variable might have been cleared by a cvar_restart
	}
	if(cv->flags & CVAR_PRIVATE) {
		if(privateFlag) {
			return;
		}
	}
	vmCvar->modificationCount = cv->modificationCount;

	len = strlen(cv->string);
	if(len + 1 > MAX_CVAR_VALUE_STRING) {
		Com_Printf(S_COLOR_YELLOW "Cvar_Update: src %s length %d exceeds MAX_CVAR_VALUE_STRING - truncate\n", cv->string, (int)len);
	}

	Q_strncpyz(vmCvar->string, cv->string, sizeof(vmCvar->string));

	vmCvar->value = cv->value;
	vmCvar->integer = cv->integer;
}

void Cvar_CompleteCvarName(const char* args, int argNum) {
	if(argNum == 2) {
		// Skip "<cmd> "
		const char* p = Com_SkipTokens(args, 1, " ");

		if(p > args) Field_CompleteCommand(p, qfalse, qtrue);
	}
}

void Cvar_Init(void) {
	Com_Memset(cvar_indexes, '\0', sizeof(cvar_indexes));
	Com_Memset(hashTable, '\0', sizeof(hashTable));

	cvar_cheats = Cvar_Get("sv_cheats", "0", CVAR_SYSTEMINFO);
	Cvar_SetDescription(cvar_cheats, "Enable cheating commands (server side only).");
	cvar_developer = Cvar_Get("developer", "0", CVAR_TEMP);
	Cvar_SetDescription(cvar_developer, "Toggles developer mode. Prints more info to console and provides more commands.");

	Cmd_AddCommand("toggle", Cvar_Toggle_f);
	Cmd_SetCommandCompletionFunc("toggle", Cvar_CompleteCvarName);

	Cmd_AddCommand("cvarlist", Cvar_List_f);
	Cmd_AddCommand("cvar_restart", Cvar_Restart_f);
}
