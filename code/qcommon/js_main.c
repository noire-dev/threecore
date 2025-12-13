// Copyright (C) 1999-2005 ID Software, Inc.
// Copyright (C) 2023-2026 Noire.dev
// SourceTech — GPLv2; see LICENSE for details.

#include "q_shared.h"
#include "qcommon.h"
#include "duktape.h"

static duk_context *js_ctx = NULL;

static duk_ret_t js_export_print(duk_context *ctx) {
    const char *str = duk_safe_to_string(ctx, 0);
    Com_Printf("^3[JS] ^7%s\n", str);  // Цветной вывод
    return 0;
}

static void Cmd_JSOpen_f(void) {
    union {
		char* c;
		void* v;
	} f;
	char filename[MAX_QEXTENDEDPATH];
	
	if (!js_ctx) {
        Com_Printf("^1Error: JavaScript VM not initialized!");
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
    
    Com_Printf("^5Executing JS file: %s\n", filename);
    
    if (duk_peval_string(js_ctx, f.c) != 0) {
        Com_Printf("^1JS Error: %s\n", duk_safe_to_string(js_ctx, -1));
    } else {
        const char *result = duk_safe_to_string(js_ctx, -1);
        if (result && result[0] != '\0') {
            Com_Printf("^2Result: %s\n", result);
        } else {
            Com_Printf("^2Script executed successfully!\n");
        }
    }
    
    duk_pop(js_ctx);
    
    FS_FreeFile(f.v);
}

void JS_Init(void) {
    if (!js_ctx) {
        js_ctx = duk_create_heap_default();
        if (!js_ctx) {
            Com_Error(ERR_FATAL, "Failed to create JavaScript VM");
            return;
        }
        
        duk_push_global_object(js_ctx);
        
        // print
        duk_push_c_function(js_ctx, js_export_print, DUK_VARARGS);
        duk_put_prop_string(js_ctx, -2, "print");
        
        duk_pop(js_ctx);  // pop global object
        
        Com_Printf("^2JavaScript VM initialized\n");
        Cmd_AddCommand("js.open", Cmd_JSOpen_f);
    }
}
