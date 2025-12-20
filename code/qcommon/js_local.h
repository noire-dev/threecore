// Copyright (C) 1999-2005 ID Software, Inc.
// Copyright (C) 2023-2026 Noire.dev
// SourceTech — GPLv2; see LICENSE for details.

#define MAX_JS_ARGS 32
#define MAX_JS_STRINGSIZE 4096

typedef enum {
    JS_TYPE_NONE,
    JS_TYPE_INT,
    JS_TYPE_FLOAT,
    JS_TYPE_BOOL,
    JS_TYPE_STRING
} js_type_t;

typedef union {
    int i;
    float f;
    qboolean b;
    char s[MAX_JS_STRINGSIZE];
} js_value_t;

typedef struct {
    js_type_t type[MAX_JS_ARGS];
    js_value_t value[MAX_JS_ARGS];
} js_args_t;

typedef struct {
    js_type_t type;
    js_value_t value;
} js_result_t;

extern js_args_t* vmargs;
extern js_result_t* vmresult;

void JS_Init(void);
void JSContext(js_args_t* args, js_result_t* result);
qboolean JSOpenFile(const char* filename);
void JSLoadScripts(const char* path, const char* name);
qboolean JSEval(const char* code, qboolean doPrint, qboolean doResult, js_result_t* result);
qboolean JSCall(int func_id, js_args_t* args, js_result_t* result);