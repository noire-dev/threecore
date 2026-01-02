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

js_args_t* vmargs;
js_result_t* vmresult;
static qboolean qvmcall_using = qfalse;

static cvar_t* js_error;

static void *JSCall_ref = NULL;
static qboolean JSCall_compiled = qfalse;

void VMContext(js_args_t* args, js_result_t* result) {
    vmargs = args;
    vmresult = result;
}

static void JS_InitCompiler(void) {
    if(duk_get_global_string(js_ctx, "JSCall") && duk_is_function(js_ctx, -1)) {
        JSCall_ref = duk_get_heapptr(js_ctx, -1);
        JSCall_compiled = qtrue;
    }
    duk_pop(js_ctx);
}

void JSLoadScripts(const char* path, const char* name) {
    char filelist[32000];
    int numfiles = FS_GetFileList(path, ".js", filelist, sizeof(filelist));
    char *file;
    
    Com_Printf("^5Loading %d JS %s scripts...\n", numfiles, name);
    
    file = filelist;
    for(int i = 0; i < numfiles; i++) {
        char fullpath[MAX_QPATH];
        Com_sprintf(fullpath, sizeof(fullpath), "%s/%s", path, file);
        Com_Printf("^5[%d/%d] %s\n", i+1, numfiles, file);
        
        JSOpenFile(fullpath, qfalse);
        file += strlen(file) + 1;
    }
}

static void ParseDuktapeResult(duk_context* ctx, js_result_t* result) {
    if(duk_is_number(ctx, -1) || duk_is_boolean(ctx, -1)) {
        double val = duk_get_number(ctx, -1);
        if(val == (int)val) result->t = JS_TYPE_INT;
        else result->t = JS_TYPE_FLOAT;
        result->v.i = (int)val;
        result->v.f = (float)val;
    } else {
        result->t = JS_TYPE_STRING;
        Q_strncpyz(result->v.s, duk_safe_to_string(ctx, -1), MAX_JS_STRINGSIZE);
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

static duk_ret_t jsexport_openjs_file(duk_context *ctx) {
    const char *str = duk_safe_to_string(ctx, 0);
    JSOpenFile(str, qtrue);
    return 0;
}

static duk_ret_t jsexport_openjs_folder(duk_context *ctx) {
    const char *str = duk_safe_to_string(ctx, 0);
    const char *name = duk_safe_to_string(ctx, 1);
    JSLoadScripts(str, name);
    return 0;
}

static duk_ret_t jsexport_file_open(duk_context *ctx) {
    const char *filename = duk_safe_to_string(ctx, 0);
    union {
		char* c;
		void* v;
	} f;
    
    if(!filename) {
        Com_Printf("^1Calling file.open without filename\n");
        duk_push_null(ctx);
        return 1;
    }
    
    FS_ReadFile(filename, &f.v);
    
    if(f.v == NULL) {
        Com_Printf("^1Could not open file '%s'\n", filename);
        FS_FreeFile(f.v);
        duk_push_null(ctx);
        return 1;
    }
    
    duk_push_string(ctx, f.v);
    FS_FreeFile(f.v);
    
    return 1;  // return 1 value
}

static duk_ret_t jsexport_file_save(duk_context *ctx) {
    const char *filename = duk_safe_to_string(ctx, 0);
    const char *buffer = duk_safe_to_string(ctx, 1);
    
    if(!filename) {
        Com_Printf("^1Calling file.save without filename\n");
        return 0;
    }
    
    FS_WriteFile(filename, buffer, sizeof(buffer));
    
    return 0;
}

static duk_ret_t jsexport_cvar_int(duk_context *ctx) {
    const char *cvar_name = duk_safe_to_string(ctx, 0);
    
    if(!cvar_name) {
        Com_Printf("^1Calling cvar.int without name\n");
        duk_push_null(ctx);
        return 1;
    }
    
    cvar_t *var = Cvar_FindVar(cvar_name);
    if(var) duk_push_int(ctx, var->integer);
    else duk_push_null(ctx);
    
    return 1;  // return 1 value
}

static duk_ret_t jsexport_cvar_float(duk_context *ctx) {
    const char *cvar_name = duk_safe_to_string(ctx, 0);
    
    if(!cvar_name) {
        Com_Printf("^1Calling cvar.float without name\n");
        duk_push_null(ctx);
        return 1;
    }
    
    cvar_t *var = Cvar_FindVar(cvar_name);
    if(var) duk_push_number(ctx, (double)var->value);
    else duk_push_null(ctx);
    
    return 1;  // return 1 value
}

static duk_ret_t jsexport_cvar_string(duk_context *ctx) {
    const char *cvar_name = duk_safe_to_string(ctx, 0);
    
    if(!cvar_name) {
        Com_Printf("^1Calling cvar.string without name\n");
        duk_push_null(ctx);
        return 1;
    }
    
    cvar_t *var = Cvar_FindVar(cvar_name);
    if(var) duk_push_string(ctx, var->string);
    else duk_push_null(ctx);
    
    return 1;  // return 1 value
}

static duk_ret_t jsexport_cvar_register(duk_context *ctx) {
    const char *cvar_name = duk_safe_to_string(ctx, 0);
    const char *cvar_value = duk_safe_to_string(ctx, 1);
    int flags = duk_get_number(ctx, 2);
    
    if(!cvar_name) {
        Com_Printf("^1Calling cvar.register without name\n");
        return 0;
    }
    
    if(!cvar_value) {
        Com_Printf("^1Calling cvar.register without value\n");
        return 0;
    }
    
    Cvar_Get(cvar_name, cvar_value, flags);
    return 0;
}

static duk_ret_t jsexport_cvar_set(duk_context *ctx) {
    const char *cvar_name = duk_safe_to_string(ctx, 0);
    const char *cvar_value = duk_safe_to_string(ctx, 1);
    
    if(!cvar_name) {
        Com_Printf("^1Calling cvar.set without name\n");
        return 0;
    }
    
    if(!cvar_value) {
        Com_Printf("^1Calling cvar.set without value\n");
        return 0;
    }
    
    Cvar_Set(cvar_name, cvar_value);
    return 0;
}

static duk_ret_t jsexport_vmcall(duk_context* ctx) {
    duk_idx_t nargs = duk_get_top(ctx);
    
    if(nargs < 2) {
        duk_push_error_object(ctx, DUK_ERR_TYPE_ERROR, "^1qvm.call requires at least func_id and qvm_id");
        return duk_throw(ctx);
    }
    
    if(qvmcall_using) {
        duk_push_error_object(ctx, DUK_ERR_ERROR, "^1Recursive qvm.call detected");
        return duk_throw(ctx);
    }
    
    qvmcall_using = qtrue;
    
    int func_id = duk_require_int(ctx, 0);
    int qvm_id = duk_require_int(ctx, 1);
    
    if(qvm_id == VM_GAME && !gvm) {
        duk_push_error_object(ctx, DUK_ERR_ERROR, "^1game.qvm not initialized");
        qvmcall_using = qfalse;
        return duk_throw(ctx);
    }
#ifndef DEDICATED
    if(qvm_id == VM_CGAME && !cgvm) {
        duk_push_error_object(ctx, DUK_ERR_ERROR, "^1cgame.qvm not initialized");
        qvmcall_using = qfalse;
        return duk_throw(ctx);
    }
    if(qvm_id == VM_UI && !uivm) {
        duk_push_error_object(ctx, DUK_ERR_ERROR, "^1ui.qvm not initialized");
        qvmcall_using = qfalse;
        return duk_throw(ctx);
    }
#endif
    
    if(qvm_id == VM_GAME) VM_Call(gvm, 0, GETVMCONTEXT);
#ifndef DEDICATED
    if(qvm_id == VM_CGAME) VM_Call(cgvm, 0, GETVMCONTEXT);
    if(qvm_id == VM_UI) VM_Call(uivm, 0, GETVMCONTEXT);
#endif
    
    for (int i = 0; i < nargs - 2 && i < MAX_JS_ARGS; i++) {
        duk_int_t arg_idx = i + 2;
        
        if(duk_is_number(ctx, arg_idx)) {
            double val = duk_get_number(ctx, arg_idx);
            if(val == (int)val) vmargs->t[i] = JS_TYPE_INT;
            else vmargs->t[i] = JS_TYPE_FLOAT;
            vmargs->v[i].i = (int)val;
            vmargs->v[i].f = (float)val;
        } else if(duk_is_string(ctx, arg_idx)) {
            vmargs->t[i] = JS_TYPE_STRING;
            const char* str = duk_safe_to_string(ctx, arg_idx);
            Q_strncpyz(vmargs->v[i].s, str, MAX_JS_STRINGSIZE);
        } else if(duk_is_null_or_undefined(ctx, arg_idx)) {
            vmargs->t[i] = JS_TYPE_NONE;
        }
    }
    
    if(qvm_id == VM_GAME) VM_Call(gvm, 1, VMCALL, func_id);
#ifndef DEDICATED
    if(qvm_id == VM_CGAME) VM_Call(cgvm, 1, VMCALL, func_id);
    if(qvm_id == VM_UI) VM_Call(uivm, 1, VMCALL, func_id);
#endif
    
    switch(vmresult->t) {
        case JS_TYPE_NONE: duk_push_undefined(ctx); break;
        case JS_TYPE_INT: duk_push_int(ctx, vmresult->v.i); break;
        case JS_TYPE_FLOAT: duk_push_number(ctx, (double)vmresult->v.f); break;
        case JS_TYPE_STRING: duk_push_string(ctx, vmresult->v.s); break;
        default: duk_push_undefined(ctx); break;
    }
    
    qvmcall_using = qfalse;
    return 1;
}

qboolean JSOpenFile(const char* filename, int notify) {
    union {
		char* c;
		void* v;
	} f;
	char fullpath[MAX_QEXTENDEDPATH];
    
    Q_strncpyz(fullpath, filename, sizeof(fullpath));
	COM_DefaultExtension(fullpath, sizeof(fullpath), ".js");
	FS_ReadFile(fullpath, &f.v);
        
    if(f.v == NULL) {
        Com_Printf("^3Could not load script '%s'\n", fullpath);
        return qfalse;
    }
    
    if(notify) Com_Printf("^5Loading %s JS script...\n", filename);
    
    if(duk_peval_string(js_ctx, f.c) != 0) {
        const char* error = duk_safe_to_string(js_ctx, -1);
        Com_Printf("^1%s: %s\n", filename, error);
        Cvar_Set("js_error", va("%s: %s", filename, error));
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
    
    if(Cmd_Argc() < 2) {
        Com_Printf("js.open <filename>\n");
        return;
    }
    
    Q_strncpyz(filename, Cmd_Argv(1), sizeof(filename));
    JSOpenFile(filename, qtrue);
}

qboolean JSEval(const char* code, qboolean doPrint, qboolean doResult, js_result_t* result) {
    if(duk_peval_string(js_ctx, code) != 0) {
        const char* error = duk_safe_to_string(js_ctx, -1);
        Com_Printf("^1%s\n", error);
        Cvar_Set("js_error", va("%s", error));
        duk_pop(js_ctx);
        return qfalse;
    }

    if(doPrint) {
        const char* text = duk_safe_to_string(js_ctx, -1);
        Com_Printf("%s\n", text);
    }

    if(doResult && result) ParseDuktapeResult(js_ctx, result);

    duk_pop(js_ctx);
    return qtrue;
}

static void Cmd_JSEval_f(void) {
    if(Cmd_Argc() < 2) {
        Com_Printf("js.eval <javascript code>\n");
        return;
    }
    
    JSEval(Cmd_Argv(1), qtrue, qfalse, NULL);
}

qboolean JSCall(int func_id, js_args_t* args, js_result_t* result) {
    int arg_count;
    
    duk_idx_t top = duk_get_top(js_ctx);
    if(JSCall_compiled) {
        duk_push_heapptr(js_ctx, JSCall_ref);
    } else {
        Com_Printf("^1JavaScript JSCall not compiled\n");
        return qfalse;
    }
    
    duk_push_int(js_ctx, func_id);
    arg_count = 1;
    
    if(args) {
        for (int i = 0; i < MAX_JS_ARGS; i++) {
            if(args->t[i] == JS_TYPE_NONE) break;
            switch (args->t[i]) {
                case JS_TYPE_NONE: break;
                case JS_TYPE_INT:
                    duk_push_int(js_ctx, args->v[i].i);
                    break;
                case JS_TYPE_FLOAT:
                    duk_push_number(js_ctx, args->v[i].f);
                    break;
                case JS_TYPE_STRING:
                    duk_push_string(js_ctx, args->v[i].s);
                    break;
            }
            arg_count++;
        }
    }
    
    if(duk_pcall(js_ctx, arg_count) != DUK_EXEC_SUCCESS) {
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

void JS_Restart(void) {
    Com_Printf("^5Restarting JavaScript VM...\n");
    
    Cmd_RemoveCommand("js.open");
    Cmd_RemoveCommand("js.eval");
    Cmd_RemoveCommand("js.restart");
    
    if(js_ctx) {
        duk_destroy_heap(js_ctx);
        js_ctx = NULL;
        JSCall_ref = NULL;
        JSCall_compiled = qfalse;
        qvmcall_using = qfalse;
    }
    
    vmargs = NULL;
    vmresult = NULL;
    
    Cvar_Set("js_error", "");
    JS_Init();
}

static void Cmd_JSRestart_f(void) { JS_Restart(); }

void JS_Init(void) {
    if(!js_ctx) {
        Com_Printf("^5Creating JavaScript context...\n");
        js_ctx = duk_create_heap_default();
        if(!js_ctx) {
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
        
        // openjs
        duk_push_object(js_ctx);
        duk_push_c_function(js_ctx, jsexport_openjs_file, 1);
        duk_put_prop_string(js_ctx, -2, "file");
        duk_push_c_function(js_ctx, jsexport_openjs_folder, 2);
        duk_put_prop_string(js_ctx, -2, "folder");
        duk_put_prop_string(js_ctx, -2, "openjs");
        
        // file
        duk_push_object(js_ctx);
        duk_push_c_function(js_ctx, jsexport_file_open, 1);
        duk_put_prop_string(js_ctx, -2, "open");
        duk_push_c_function(js_ctx, jsexport_file_save, 2);
        duk_put_prop_string(js_ctx, -2, "save");
        duk_put_prop_string(js_ctx, -2, "file");
        
        // cvar
        duk_push_object(js_ctx);
        duk_push_c_function(js_ctx, jsexport_cvar_register, 3);
        duk_put_prop_string(js_ctx, -2, "register");
        duk_push_c_function(js_ctx, jsexport_cvar_set, 2);
        duk_put_prop_string(js_ctx, -2, "set");
        duk_push_c_function(js_ctx, jsexport_cvar_int, 1);
        duk_put_prop_string(js_ctx, -2, "int");
        duk_push_c_function(js_ctx, jsexport_cvar_float, 1);
        duk_put_prop_string(js_ctx, -2, "float");
        duk_push_c_function(js_ctx, jsexport_cvar_string, 1);
        duk_put_prop_string(js_ctx, -2, "string");
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
        Cmd_AddCommand("js.eval", Cmd_JSEval_f);
        Cmd_AddCommand("js.restart", Cmd_JSRestart_f);
        
        js_error = Cvar_Get("js_error", "", 0);
        
        JSOpenFile("js/init.js", qtrue);
        JSLoadScripts("js/framework", "framework");
        JSOpenFile("js/main.js", qtrue);
        JS_InitCompiler();
    }
}
