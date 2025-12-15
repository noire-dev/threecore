// Copyright (C) 1999-2005 ID Software, Inc.
// Copyright (C) 2023-2026 Noire.dev
// SourceTech — GPLv2; see LICENSE for details.

#define MAX_JS_ARGS 16
#define MAX_JS_STRINGSIZE 1024

typedef enum {
    JS_TYPE_NONE,
    JS_TYPE_INT,
    JS_TYPE_FLOAT,
    JS_TYPE_BOOL,
    JS_TYPE_STRING
} js_type_t;

typedef union {
    int int_val;
    float float_val;
    qboolean bool_val;
    char string_val[MAX_JS_STRINGSIZE];
} js_value_t;

typedef struct {
    js_type_t type[MAX_JS_ARGS];
    js_value_t value[MAX_JS_ARGS];
} js_args_t;

typedef struct {
    js_type_t type;
    js_value_t value;
} js_result_t;

void JS_Init(void);
qboolean JSOpenFile(const char* filename);
qboolean JSEval(const char* code, qboolean doPrint, qboolean doResult, js_result_t* result);
qboolean JSCall(int func_id, js_result_t* result, js_args_t* args);