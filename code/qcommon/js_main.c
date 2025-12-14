// Copyright (C) 1999-2005 ID Software, Inc.
// Copyright (C) 2023-2026 Noire.dev
// SourceTech — GPLv2; see LICENSE for details.

#include "q_shared.h"
#include "qcommon.h"
#include "duktape.h"

static duk_context *js_ctx = NULL;

static duk_ret_t jsexport_console_log(duk_context *ctx) {
    const char *str = duk_safe_to_string(ctx, 0);
    Com_Printf("%s\n", str);
    return 0;
}

static duk_ret_t jsexport_console_cmd(duk_context *ctx) {
    const char *str = duk_safe_to_string(ctx, 0);
    Cmd_ExecuteString(str);
    return 0;
}

static duk_ret_t jsexport_cvar_get(duk_context *ctx) {
    const char *cvar_name = duk_get_string(ctx, 0);
    
    if (!cvar_name) {
        Com_Printf("^1Error: Calling cvar.get without name\n");
        duk_push_null(ctx);
        return 1;
    }
    
    cvar_t *var = Cvar_FindVar(cvar_name);
    if (var) {
        duk_push_string(ctx, var->string);
    } else {
        duk_push_null(ctx);
    }
    
    return 1;  // return 1 value
}

static duk_ret_t jsexport_cvar_set(duk_context *ctx) {
    const char *cvar_name = duk_get_string(ctx, 0);
    const char *cvar_value = duk_get_string(ctx, 1);
    
    if (!cvar_name) {
        Com_Printf("^1Error: Calling cvar.set without name\n");
        return 0;
    }
    
    if (!cvar_value) {
        Com_Printf("^1Error: Calling cvar.set without value\n");
        return 0;
    }
    
    Cvar_Set(cvar_name, cvar_value);
    
    return 0;
}

static void Cmd_JSOpen_f(void) {
    union {
		char* c;
		void* v;
	} f;
	char filename[MAX_QEXTENDEDPATH];
	
	if (!js_ctx) {
        Com_Printf("^1Error: JavaScript VM not initialized");
        return;
    }
	
    if (Cmd_Argc() < 2) {
        Com_Printf("js.open <filename>\n");
        return;
    }
    
    Q_strncpyz(filename, Cmd_Argv(1), sizeof(filename));
	COM_DefaultExtension(filename, sizeof(filename), ".js");
	FS_ReadFile(filename, &f.v);
        
    if(f.v == NULL) {
        Com_Printf("^1Error: Could not load file '%s'\n", filename);
        return;
    }
    
    if (duk_peval_string(js_ctx, f.c) != 0) Com_Printf("^1JavaScript - %s\n", duk_safe_to_string(js_ctx, -1));
    
    duk_pop(js_ctx);
    
    FS_FreeFile(f.v);
}

static void Cmd_CompleteJSName(const char* args, int argNum) {
	if(argNum == 2) Field_CompleteFilename("", "js", qfalse, FS_MATCH_ANY | FS_MATCH_STICK | FS_MATCH_SUBDIRS);
}

void JS_Init(void) {
    if (!js_ctx) {
        js_ctx = duk_create_heap_default();
        if (!js_ctx) {
            Com_Error(ERR_FATAL, "^1Failed to create JavaScript VM");
            return;
        }
        
        duk_push_global_object(js_ctx);
        
        // console
        duk_push_object(js_ctx);
        duk_push_c_function(js_ctx, jsexport_console_log, DUK_VARARGS);
        duk_put_prop_string(js_ctx, -2, "log");
        duk_push_c_function(js_ctx, jsexport_console_cmd, 1);
        duk_put_prop_string(js_ctx, -2, "cmd");
        duk_put_prop_string(js_ctx, -2, "console");
        
        // cvar
        duk_push_object(js_ctx);
        duk_push_c_function(js_ctx, jsexport_cvar_get, 1);
        duk_put_prop_string(js_ctx, -2, "get");
        duk_push_c_function(js_ctx, jsexport_cvar_set, 2);
        duk_put_prop_string(js_ctx, -2, "set");
        duk_put_prop_string(js_ctx, -2, "cvar");
        
        duk_pop(js_ctx);
        
        Com_Printf("^2JavaScript VM initialized!\n");
        Cmd_AddCommand("js.open", Cmd_JSOpen_f);
        Cmd_SetCommandCompletionFunc("js.open", Cmd_CompleteJSName);
    }
}
