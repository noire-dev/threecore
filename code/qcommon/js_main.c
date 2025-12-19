// Copyright (C) 1999-2005 ID Software, Inc.
// Copyright (C) 2023-2026 Noire.dev
// SourceTech — GPLv2; see LICENSE for details.

#include "q_shared.h"
#include "qcommon.h"
#include "duktape.h"
#include "../server/server.h"
#ifndef DEDICATED
#include "../client/client.h"
#endif

static duk_context *js_ctx = NULL;

static cvar_t* js_error;

static void *JSCall_ref = NULL;
static qboolean JSCall_compiled = qfalse;

static void JS_InitCompiler(void) {
    if (!js_ctx) return;
    
    if (duk_get_global_string(js_ctx, "JSCall") && duk_is_function(js_ctx, -1)) {
        JSCall_ref = duk_get_heapptr(js_ctx, -1);
        JSCall_compiled = qtrue;
    } else {
        duk_pop(js_ctx);
    }
    
    duk_pop(js_ctx);
}

static void JS_LoadCoreScripts(void) {
    char filelist[8192];
    char filename[MAX_QPATH];
    char *fileptr;
    
    int numfiles = FS_GetFileList("scripts/core", ".js", filelist, sizeof(filelist));
    if (numfiles == 0) return;
    Com_Printf("^2Loading %d JS core scripts...\n", numfiles);
    fileptr = filelist;
    
    for (int i = 0; i < numfiles; i++) {
        char *next = strchr(fileptr, '\n');
        if (next) *next = '\0';
        
        Q_strncpyz(filename, fileptr, sizeof(filename));
        char fullpath[MAX_QPATH];
        Com_sprintf(fullpath, sizeof(fullpath), "scripts/core/%s", filename);
        Com_Printf("  ^5[%d/%d] %s: \n", i+1, numfiles, filename);
        
        if (!JSOpenFile(fullpath)) {
            Com_Printf("^1Failed\n");
        } else {
            Com_Printf("^2Loaded\n");
        }
        
        if (next) fileptr = next + 1;
        else break;
    }
}

static void ParseDuktapeResult(duk_context* ctx, js_result_t* result) {
    if (duk_is_number(ctx, -1)) {
        double val = duk_get_number(ctx, -1);
        if (val == (int)val) {
            result->type = JS_TYPE_INT;
            result->value.int_val = (int)val;
        } else {
            result->type = JS_TYPE_FLOAT;
            result->value.float_val = (float)val;
        }
    } else if (duk_is_boolean(ctx, -1)) {
        result->type = JS_TYPE_BOOL;
        result->value.bool_val = duk_get_boolean(ctx, -1);
    } else {
        result->type = JS_TYPE_STRING;
        Q_strncpyz(result->value.string_val, duk_safe_to_string(ctx, -1), MAX_JS_STRINGSIZE);
    }
}

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
    const char *cvar_name = duk_safe_to_string(ctx, 0);
    
    if (!cvar_name) {
        Com_Printf("^1Error: Calling cvar.get without name\n");
        duk_push_null(ctx);
        return 1;
    }
    
    cvar_t *var = Cvar_FindVar(cvar_name);
    if (var) duk_push_string(ctx, var->string);
    else duk_push_null(ctx);
    
    return 1;  // return 1 value
}

static duk_ret_t jsexport_cvar_set(duk_context *ctx) {
    const char *cvar_name = duk_safe_to_string(ctx, 0);
    const char *cvar_value = duk_safe_to_string(ctx, 1);
    
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

static duk_ret_t jsexport_vmcall(duk_context* ctx) {
    duk_idx_t nargs = duk_get_top(ctx);
    
    if (nargs < 2) {
        duk_push_error_object(ctx, DUK_ERR_TYPE_ERROR, "^1VMCall requires at least func_id and qvm_id");
        return duk_throw(ctx);
    }
    
    int func_id = duk_require_int(ctx, 0);
    int qvm_id = duk_require_int(ctx, 1);
    
    js_args_t vm_args;
    memset(&vm_args, 0, sizeof(vm_args));
    
    for (int i = 0; i < nargs - 2 && i < MAX_JS_ARGS; i++) {
        duk_int_t arg_idx = i + 2;
        
        if (duk_is_number(ctx, arg_idx)) {
            double val = duk_get_number(ctx, arg_idx);
            if (val == (int)val) {
                vm_args.type[i] = JS_TYPE_INT;
                vm_args.value[i].int_val = (int)val;
            } else {
                vm_args.type[i] = JS_TYPE_FLOAT;
                vm_args.value[i].float_val = (float)val;
            }
        } else if (duk_is_boolean(ctx, arg_idx)) {
            vm_args.type[i] = JS_TYPE_BOOL;
            vm_args.value[i].bool_val = duk_get_boolean(ctx, arg_idx) ? qtrue : qfalse;
        } else if (duk_is_string(ctx, arg_idx)) {
            vm_args.type[i] = JS_TYPE_STRING;
            const char* str = duk_safe_to_string(ctx, arg_idx);
            Q_strncpyz(vm_args.value[i].string_val, str, MAX_JS_STRINGSIZE);
        } else if (duk_is_null_or_undefined(ctx, arg_idx)) {
            vm_args.type[i] = JS_TYPE_NONE;
        }
    }
    
    js_result_t vm_result;
    memset(&vm_result, 0, sizeof(vm_result));
    
    if(qvm_id == VM_GAME && gvm) {
        VM_Call(gvm, 3, GAME_VMCALL, func_id, &vm_args, &vm_result);
        success = qtrue;
    }
#ifndef DEDICATED
    if(qvm_id == VM_CGAME && cgvm) {
        VM_Call(cgvm, 3, CG_VMCALL, func_id, &vm_args, &vm_result);
        success = qtrue;
    }
    if(qvm_id == VM_UI && uivm) { 
        VM_Call(uivm, 3, UI_VMCALL, func_id, &vm_args, &vm_result);
        success = qtrue;
    }
#endif

    if (!success) {
        duk_push_error_object(ctx, DUK_ERR_TYPE_ERROR, "^1VMCall failed");
        return duk_throw(ctx);
    }
    
    switch (vm_result.type) {
        case JS_TYPE_INT: duk_push_int(ctx, vm_result.value.int_val); break;
        case JS_TYPE_FLOAT: duk_push_number(ctx, (double)vm_result.value.float_val); break;
        case JS_TYPE_BOOL: duk_push_boolean(ctx, vm_result.value.bool_val ? 1 : 0); break;
        case JS_TYPE_STRING: duk_push_string(ctx, vm_result.value.string_val); break;
        default: duk_push_undefined(ctx); break;
    }
    
    return 1;
}

qboolean JSOpenFile(const char* filename) {
    union {
		char* c;
		void* v;
	} f;
	char fullpath[MAX_QEXTENDEDPATH];
	
	if (!js_ctx) {
        Com_Printf("^1JavaScript VM not initialized");
        return qfalse;
    }
    
    Q_strncpyz(fullpath, filename, sizeof(fullpath));
	COM_DefaultExtension(fullpath, sizeof(fullpath), ".js");
	FS_ReadFile(fullpath, &f.v);
        
    if(f.v == NULL) {
        Com_Printf("^1Could not load file '%s'\n", fullpath);
        return qfalse;
    }
    
    if (duk_peval_string(js_ctx, f.c) != 0) {
        Com_Printf("^1%s - %s\n", filename, duk_safe_to_string(js_ctx, -1));
        Cvar_Set("js_error", va("%s - %s", filename, duk_safe_to_string(js_ctx, -1)));
        duk_pop(js_ctx);
        FS_FreeFile(f.v);
        return qfalse;
    }
    
    duk_pop(js_ctx);
    FS_FreeFile(f.v);
    return qtrue;
}

static void Cmd_JSOpenFile_f(void) {
    char filename[MAX_QEXTENDEDPATH];
    
    if (Cmd_Argc() < 2) {
        Com_Printf("js.open <filename>\n");
        return;
    }
    
    Q_strncpyz(filename, Cmd_Argv(1), sizeof(filename));
    JSOpenFile(filename);
}

qboolean JSEval(const char* code, qboolean doPrint, qboolean doResult, js_result_t* result) {
    if (!js_ctx) {
        Com_Printf("^1Error: JavaScript VM not initialized\n");
        return qfalse;
    }
    
    if (duk_peval_string(js_ctx, code) != 0) {
        Com_Printf("^1%s\n", duk_safe_to_string(js_ctx, -1));
        Cvar_Set("js_error", va("%s", duk_safe_to_string(js_ctx, -1)));
        duk_pop(js_ctx);
        return qfalse;
    }
    
    const char* text = duk_safe_to_string(js_ctx, -1);
    if(doPrint) Com_Printf("%s\n", text);
    if(doResult) ParseDuktapeResult(js_ctx, result);
    
    duk_pop(js_ctx);
    return qtrue;
}

static void Cmd_JSEval_f(void) {
    if (Cmd_Argc() < 2) {
        Com_Printf("js.eval <javascript code>\n");
        return;
    }
    
    JSEval(Cmd_Argv(1), qtrue, qfalse, NULL);
}

qboolean JSCall(int func_id, js_result_t* result, js_args_t* args) {
    int arg_count;
    
    if (!js_ctx) {
        Com_Printf("^1JavaScript VM not initialized\n");
        return qfalse;
    }
    
    duk_idx_t top = duk_get_top(js_ctx);
    if (JSCall_compiled) {
        duk_push_heapptr(js_ctx, JSCall_ref);
    } else {
        Com_Printf("^1JavaScript JSCall not compiled\n");
        return qfalse;
    }
    
    duk_push_int(js_ctx, func_id);
    arg_count = 1;
    
    if (args) {
        for (int i = 0; i < MAX_JS_ARGS; i++) {
            if(args->type[i] == JS_TYPE_NONE) break;
            switch (args->type[i]) {
                case JS_TYPE_INT:
                    duk_push_int(js_ctx, args->value[i].int_val);
                    break;
                case JS_TYPE_FLOAT:
                    duk_push_number(js_ctx, args->value[i].float_val);
                    break;
                case JS_TYPE_BOOL:
                    duk_push_boolean(js_ctx, args->value[i].bool_val);
                    break;
                case JS_TYPE_STRING:
                    duk_push_string(js_ctx, args->value[i].string_val);
                    break;
            }
            arg_count++;
        }
    }
    
    if (duk_pcall(js_ctx, arg_count) != DUK_EXEC_SUCCESS) {
        const char* error = duk_safe_to_string(js_ctx, -1);
        Com_Printf("^1%s\n", error);
        Cvar_Set("js_error", va("%s", error));
        duk_set_top(js_ctx, top);
        return qfalse;
    }
    
    ParseDuktapeResult(js_ctx, result);
    
    duk_set_top(js_ctx, top);
    return qtrue;
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
        
        // qvm
        duk_push_object(js_ctx);
        duk_push_c_function(js_ctx, jsexport_vmcall, DUK_VARARGS);
        duk_put_prop_string(js_ctx, -2, "call");
        duk_push_int(js_ctx, VM_GAME);
        duk_put_prop_string(js_ctx, -2, "game");
#ifndef DEDICATED
        duk_push_int(js_ctx, VM_CGAME); 
        duk_put_prop_string(js_ctx, -2, "cgame");
        duk_push_int(js_ctx, VM_UI);
        duk_put_prop_string(js_ctx, -2, "ui");
#endif
        duk_put_prop_string(js_ctx, -2, "qvm");
        
        duk_pop(js_ctx);
        
        Com_Printf("^2JavaScript VM initialized!\n");
        Cmd_AddCommand("js.open", Cmd_JSOpenFile_f);
        Cmd_SetCommandCompletionFunc("js.open", Cmd_CompleteJSName);
        Cmd_AddCommand("js.eval", Cmd_JSEval_f);
        
        js_error = Cvar_Get("js_error", "", 0);
        
        JS_LoadCoreScripts();
        JS_InitCompiler();
    }
}
