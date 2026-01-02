// Copyright (C) 1999-2005 ID Software, Inc.
// Copyright (C) 2023-2026 Noire.dev
// SourceTech — GPLv2; see LICENSE for details.

#define MAX_JS_ARGS 32
#define MAX_JS_STRINGSIZE 4096

typedef enum { JS_TYPE_NONE, JS_TYPE_INT, JS_TYPE_FLOAT, JS_TYPE_STRING } js_type_t;

typedef struct {
    int i;
    float f;
    char s[MAX_JS_STRINGSIZE];
} js_value_t;

typedef struct {
    js_type_t t[MAX_JS_ARGS];
    js_value_t v[MAX_JS_ARGS];
} js_args_t;

typedef struct {
    js_type_t t;
    js_value_t v;
} js_result_t;

extern js_args_t* vmargs;
extern js_result_t* vmresult;

void JS_Restart(void);
void JS_Init(void);
void VMContext(js_args_t* args, js_result_t* result);
qboolean JSOpenFile(const char* filename, int notify);
void JSLoadScripts(const char* path, const char* name);
qboolean JSEval(const char* code, qboolean doPrint, qboolean doResult, js_result_t* result);
qboolean JSCall(int func_id, js_args_t* args, js_result_t* result);
