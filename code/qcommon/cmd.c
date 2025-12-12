// Copyright (C) 1999-2005 ID Software, Inc.
// Copyright (C) 2023-2026 Noire.dev
// SourceTech — GPLv2; see LICENSE for details.

#include "q_shared.h"
#include "qcommon.h"

#define MAX_CMD_BUFFER 65536

typedef struct {
	byte* data;
	int maxsize;
	int cursize;
} cmd_t;

static int cmd_wait;
static cmd_t cmd_text;
static byte cmd_text_buf[MAX_CMD_BUFFER];

static void Cmd_Wait_f(void) {
	if(Cmd_Argc() == 2) {
		cmd_wait = atoi(Cmd_Argv(1));
		if(cmd_wait < 0) cmd_wait = 1;  // ignore the argument
	} else {
		cmd_wait = 1;
	}
}

void Cbuf_Init(void) {
	cmd_text.data = cmd_text_buf;
	cmd_text.maxsize = MAX_CMD_BUFFER;
	cmd_text.cursize = 0;
}

void Cbuf_AddText(const char* text) {
	const int l = (int)strlen(text);

	if(cmd_text.cursize + l >= cmd_text.maxsize) {
		Com_Printf("Cbuf_AddText: overflow\n");
		return;
	}

	Com_Memcpy(&cmd_text.data[cmd_text.cursize], text, l);
	cmd_text.cursize += l;
}

static int nestedCmdOffset;

void Cbuf_NestedReset(void) { nestedCmdOffset = 0; }

void Cbuf_NestedAdd(const char* text) {
	int len = (int)strlen(text);
	int pos = nestedCmdOffset;
	qboolean separate = qfalse;
	int i;

	if(len <= 0) {
		nestedCmdOffset = cmd_text.cursize;
		return;
	}

	if(pos > cmd_text.cursize || pos < 0) {
		// insert at the text end
		pos = cmd_text.cursize;
	}

	if(text[len - 1] == '\n' || text[len - 1] == ';') {
		// command already has separator
	} else {
		separate = qtrue;
		len += 1;
	}

	if(len + cmd_text.cursize > cmd_text.maxsize) {
		Com_Printf(S_COLOR_YELLOW "%s(%i) overflowed\n", __func__, pos);
		nestedCmdOffset = cmd_text.cursize;
		return;
	}

	// move the existing command text
	for(i = cmd_text.cursize - 1; i >= pos; i--) {
		cmd_text.data[i + len] = cmd_text.data[i];
	}

	if(separate) {
		// copy the new text in + add a \n
		Com_Memcpy(cmd_text.data + pos, text, len - 1);
		cmd_text.data[pos + len - 1] = '\n';
	} else {
		// copy the new text in
		Com_Memcpy(cmd_text.data + pos, text, len);
	}

	cmd_text.cursize += len;

	nestedCmdOffset = cmd_text.cursize;
}

void Cbuf_InsertText(const char* text) {
	int len;
	int i;

	len = strlen(text) + 1;

	if(len + cmd_text.cursize > cmd_text.maxsize) {
		Com_Printf("Cbuf_InsertText overflowed\n");
		return;
	}

	// move the existing command text
	for(i = cmd_text.cursize - 1; i >= 0; i--) {
		cmd_text.data[i + len] = cmd_text.data[i];
	}

	// copy the new text in
	Com_Memcpy(cmd_text.data, text, len - 1);

	// add a \n
	cmd_text.data[len - 1] = '\n';
	cmd_text.cursize += len;
}

void Cbuf_ExecuteText(cbufExec_t exec_when, const char* text) {
	switch(exec_when) {
		case EXEC_NOW:
			cmd_wait = 0;  // discard any pending waiting
			if(text && text[0] != '\0') {
				Com_DPrintf(S_COLOR_YELLOW "EXEC_NOW %s\n", text);
				Cmd_ExecuteString(text);
			} else {
				Cbuf_Execute();
				Com_DPrintf(S_COLOR_YELLOW "EXEC_NOW %s\n", cmd_text.data);
			}
			break;
		case EXEC_INSERT: Cbuf_InsertText(text); break;
		case EXEC_APPEND: Cbuf_AddText(text); break;
		default: Com_Error(ERR_FATAL, "Cbuf_ExecuteText: bad exec_when");
	}
}

void Cbuf_Execute(void) {
	char line[MAX_CMD_LINE], *text;
	int i, n, quotes;
	qboolean in_star_comment;
	qboolean in_slash_comment;

	if(cmd_wait > 0) return;

	// This will keep // style comments all on one line by not breaking on
	// a semicolon.  It will keep /* ... */ style comments all on one line by not
	// breaking it for semicolon or newline.
	in_star_comment = qfalse;
	in_slash_comment = qfalse;

	while(cmd_text.cursize > 0) {
		// find a \n or ; line break or comment: // or /* */
		text = (char*)cmd_text.data;

		quotes = 0;
		for(i = 0; i < cmd_text.cursize; i++) {
			if(text[i] == '"') quotes++;

			if(!(quotes & 1)) {
				if(i < cmd_text.cursize - 1) {
					if(!in_star_comment && text[i] == '/' && text[i + 1] == '/')
						in_slash_comment = qtrue;
					else if(!in_slash_comment && text[i] == '/' && text[i + 1] == '*')
						in_star_comment = qtrue;
					else if(in_star_comment && text[i] == '*' && text[i + 1] == '/') {
						in_star_comment = qfalse;
						// If we are in a star comment, then the part after it is valid
						// Note: This will cause it to NUL out the terminating '/'
						// but ExecuteString doesn't require it anyway.
						i++;
						break;
					}
				}
				if(!in_slash_comment && !in_star_comment && text[i] == ';') break;
			}
			if(!in_star_comment && (text[i] == '\n' || text[i] == '\r')) {
				if(quotes & 1) continue;
				in_slash_comment = qfalse;
				break;
			}
		}

		// copy up to (MAX_CMD_LINE - 1) chars but keep buffer position intact to prevent parsing truncated leftover
		if(i > (MAX_CMD_LINE - 1))
			n = MAX_CMD_LINE - 1;
		else
			n = i;

		Com_Memcpy(line, text, n);
		line[n] = '\0';

		// delete the text from the command buffer and move remaining commands down
		// this is necessary because commands (exec) can insert data at the
		// beginning of the text buffer

		if(i != cmd_text.cursize) {
			++i;
			// skip all repeating newlines/semicolons/whitespaces
			while(i < cmd_text.cursize && (text[i] == '\n' || text[i] == '\r' || text[i] == ';' || (text[i] != '\0' && text[i] <= ' '))) ++i;
		}

		cmd_text.cursize -= i;

		if(cmd_text.cursize) {
			memmove(text, text + i, cmd_text.cursize);
		}

		if(nestedCmdOffset > 0) {
			nestedCmdOffset -= i;
			if(nestedCmdOffset < 0) {
				nestedCmdOffset = 0;
			}
		}

		// execute the command line
		Cmd_ExecuteString(line);

		// break on wait command
		if(cmd_wait > 0) break;
	}
}

void Cbuf_Wait(void) {
	if(cmd_wait > 0) --cmd_wait;
}

static void Cmd_Exec_f(void) {
	qboolean quiet;
	union {
		char* c;
		void* v;
	} f;
	char filename[MAX_QEXTENDEDPATH];

	if(Cmd_Argc() != 2) {
		Com_Printf("exec <filename> : execute a sandbox.script file\n");
		return;
	}

	Q_strncpyz(filename, Cmd_Argv(1), sizeof(filename));
	COM_DefaultExtension(filename, sizeof(filename), ".sbscript");
	FS_ReadFile(filename, &f.v);
	if(f.v == NULL) {
		Com_Printf("couldn't exec %s\n", filename);
		return;
	}

	Cbuf_InsertText(f.c);
	if(!Q_stricmp(filename, CONFIG_CFG)) Com_WriteConfiguration();  // to avoid loading outdated values
	FS_FreeFile(f.v);
}

static void Cmd_Echo_f(void) { Com_Printf("%s\n", Cmd_ArgsFrom(1)); }
static void Cmd_CvarExec_f(void) { Cmd_ExecuteString("%s\n", Cmd_ArgsFrom(1)); }

typedef struct cmd_function_s {
	struct cmd_function_s* next;
	char* name;
	xcommand_t function;
	completionFunc_t complete;
} cmd_function_t;

static int cmd_argc;
static char* cmd_argv[MAX_STRING_TOKENS];                        // points into cmd_tokenized
static char cmd_tokenized[BIG_INFO_STRING + MAX_STRING_TOKENS];  // will have 0 bytes inserted
static char cmd_cmd[BIG_INFO_STRING];                            // the original command we received (no token processing)

static cmd_function_t* cmd_functions;  // possible commands to execute

int Cmd_Argc(void) { return cmd_argc; }

void Cmd_Clear(void) {
	cmd_cmd[0] = '\0';
	cmd_argc = 0;
}

const char* Cmd_Argv(int arg) {
	if((unsigned)arg >= cmd_argc) return "";
	return cmd_argv[arg];
}

void Cmd_ArgvBuffer(int arg, char* buffer, int bufferLength) { Q_strncpyz(buffer, Cmd_Argv(arg), bufferLength); }

char* Cmd_ArgsFrom(int arg) {
	static char cmd_args[BIG_INFO_STRING], *s;
	int i;

	s = cmd_args;
	*s = '\0';
	if(arg < 0) arg = 0;
	for(i = arg; i < cmd_argc; i++) {
		s = Q_stradd(s, cmd_argv[i]);
		if(i != cmd_argc - 1) {
			s = Q_stradd(s, " ");
		}
	}

	return cmd_args;
}

void Cmd_ArgsBuffer(char* buffer, int bufferLength) { Q_strncpyz(buffer, Cmd_ArgsFrom(1), bufferLength); }

char* Cmd_Cmd(void) { return cmd_cmd; }

void Cmd_Args_Sanitize(const char* separators) {
	int i;

	for(i = 1; i < cmd_argc; i++) {
		char* c = cmd_argv[i];

		while((c = strpbrk(c, separators)) != NULL) {
			*c = ' ';
			++c;
		}
	}
}

static void PrepareNewLinesInString(char* str) {
    char* p;
    for (p = str; *p; p++) {
        if (*p == '\n') *p = ' ';
    }
}

static void Cmd_TokenizeString2(const char* text_in, qboolean ignoreQuotes) {
	const char* text;
	char* textOut;

	// clear previous args
	cmd_argc = 0;
	cmd_cmd[0] = '\0';

	if(!text_in) return;

	Q_strncpyz(cmd_cmd, text_in, sizeof(cmd_cmd));
	
	PrepareNewLinesInString(cmd_cmd);

	text = cmd_cmd;  // read from safe-length buffer
	textOut = cmd_tokenized;

	while(1) {
		if(cmd_argc >= ARRAY_LEN(cmd_argv)) {
			return;  // this is usually something malicious
		}

		while(1) {
			// skip whitespace
			while(*text && *text <= ' ') {
				text++;
			}
			if(!*text) {
				return;  // all tokens parsed
			}

			// skip // comments
			if(text[0] == '/' && text[1] == '/') {
				// accept protocol headers (e.g. http://) in command lines that matching "*?[a-z]://" pattern
				if(text < cmd_cmd + 3 || text[-1] != ':' || text[-2] < 'a' || text[-2] > 'z') {
					return;  // all tokens parsed
				}
			}

			// skip /* */ comments
			if(text[0] == '/' && text[1] == '*') {
				while(*text && (text[0] != '*' || text[1] != '/')) {
					text++;
				}
				if(!*text) {
					return;  // all tokens parsed
				}
				text += 2;
			} else {
				break;  // we are ready to parse a token
			}
		}

		// handle quoted strings
		if(!ignoreQuotes && *text == '"') {
			cmd_argv[cmd_argc] = textOut;
			cmd_argc++;
			text++;
			while(*text && *text != '"') {
				*textOut++ = *text++;
			}
			*textOut++ = '\0';
			if(!*text) {
				return;  // all tokens parsed
			}
			text++;
			continue;
		}

		// regular token
		cmd_argv[cmd_argc] = textOut;
		cmd_argc++;

		// skip until whitespace, quote, or command
		while(*text > ' ') {
		    
		    // variable via $
		    if(text[0] == '$') {
				const char* var_start = text + 1;
				const char* var_end = var_start;
				
				while(*var_end && *var_end != '$') var_end++;
				
				if(*var_end == '$' && var_end > var_start) {
					char var_name[MAX_TOKEN_CHARS];
					int var_len = var_end - var_start;
					strncpy(var_name, var_start, var_len);
					var_name[var_len] = '\0';
					
					const char* value = Cvar_VariableString(var_name);
					
					while(*value) *textOut++ = *value++;
					
					text = var_end + 1;
					continue;
				}
		    }
		    
			if(!ignoreQuotes && text[0] == '"') {
				break;
			}

			if(text[0] == '/' && text[1] == '/') {
				// accept protocol headers (e.g. http://) in command lines that matching "*?[a-z]://" pattern
				if(text < cmd_cmd + 3 || text[-1] != ':' || text[-2] < 'a' || text[-2] > 'z') {
					break;
				}
			}

			// skip /* */ comments
			if(text[0] == '/' && text[1] == '*') {
				break;
			}

			*textOut++ = *text++;
		}

		*textOut++ = '\0';

		if(!*text) {
			return;  // all tokens parsed
		}
	}
}

void Cmd_TokenizeString(const char* text_in) { Cmd_TokenizeString2(text_in, qfalse); }

void Cmd_TokenizeStringIgnoreQuotes(const char* text_in) { Cmd_TokenizeString2(text_in, qtrue); }

static cmd_function_t* Cmd_FindCommand(const char* cmd_name) {
	cmd_function_t* cmd;
	for(cmd = cmd_functions; cmd; cmd = cmd->next)
		if(!Q_stricmp(cmd_name, cmd->name)) return cmd;
	return NULL;
}

void Cmd_AddCommand(const char* cmd_name, xcommand_t function) {
	cmd_function_t* cmd;

	if(Cmd_FindCommand(cmd_name)) {
		if(function != NULL) Com_Printf("Cmd_AddCommand: %s already defined\n", cmd_name);
		return;
	}

	cmd = S_Malloc(sizeof(*cmd));
	cmd->name = CopyString(cmd_name);
	cmd->function = function;
	cmd->complete = NULL;
	cmd->next = cmd_functions;
	cmd_functions = cmd;
}

void Cmd_SetCommandCompletionFunc(const char* command, completionFunc_t complete) {
	cmd_function_t* cmd;

	for(cmd = cmd_functions; cmd; cmd = cmd->next) {
		if(!Q_stricmp(command, cmd->name)) {
			cmd->complete = complete;
			return;
		}
	}
}

void Cmd_RemoveCommand(const char* cmd_name) {
	cmd_function_t *cmd, **back;

	back = &cmd_functions;
	while(1) {
		cmd = *back;
		if(!cmd) return;
		if(!Q_stricmp(cmd_name, cmd->name)) {
			*back = cmd->next;
			if(cmd->name) Z_Free(cmd->name);
			Z_Free(cmd);
			return;
		}
		back = &cmd->next;
	}
}

void Cmd_CommandCompletion(void (*callback)(const char* s)) {
	const cmd_function_t* cmd;

	for(cmd = cmd_functions; cmd; cmd = cmd->next) callback(cmd->name);
}

qboolean Cmd_CompleteArgument(const char* command, const char* args, int argNum) {
	const cmd_function_t* cmd;

	for(cmd = cmd_functions; cmd; cmd = cmd->next) {
		if(!Q_stricmp(command, cmd->name)) {
			if(cmd->complete) cmd->complete(args, argNum);
			return qtrue;
		}
	}

	return qfalse;
}

static void PrepareVariablesInString(char* str) {
    char* p;
    for (p = str; *p; p++) {
        if (*p == '&') *p = '$';
    }
}

static void Cmd_PrepareVariables(void) {
	for(int i = 0; i < Cmd_Argc(); i++) PrepareVariablesInString(cmd_argv[i]);
}

void Cmd_ExecuteString(const char* text) {
	cmd_function_t *cmd, **prev;
    
	Cmd_TokenizeString(text);
	if(!Cmd_Argc()) return;
	
	Cmd_PrepareVariables();

	for(prev = &cmd_functions; *prev; prev = &cmd->next) {
		cmd = *prev;
		if(!Q_stricmp(cmd_argv[0], cmd->name)) {
			*prev = cmd->next;
			cmd->next = cmd_functions;
			cmd_functions = cmd;

			if(!cmd->function) break;
			else cmd->function();
			return;
		}
	}

	if(Cvar_Command()) return;
	if(com_sv_running && com_sv_running->integer && SV_GameCommand()) return;
#ifndef DEDICATED
    if(com_cl_running && com_cl_running->integer && CL_GameCommand()) return;
	if(com_cl_running && com_cl_running->integer && UI_GameCommand()) return;
	CL_ForwardCommandToServer(text);
#endif
}

static void Cmd_CompleteCfgName(const char* args, int argNum) {
	if(argNum == 2) Field_CompleteFilename("", "sbscript", qfalse, FS_MATCH_ANY | FS_MATCH_STICK | FS_MATCH_SUBDIRS);
}

void Cmd_CompleteWriteCfgName(const char* args, int argNum) {
	if(argNum == 2) Field_CompleteFilename("", "sbscript", qfalse, FS_MATCH_EXTERN | FS_MATCH_STICK);
}

void Cmd_Init(void) {
	Cmd_AddCommand("exec", Cmd_Exec_f);
	Cmd_SetCommandCompletionFunc("exec", Cmd_CompleteCfgName);
	Cmd_AddCommand("echo", Cmd_Echo_f);
	Cmd_AddCommand("%", Cmd_CvarExec_f);
	Cmd_AddCommand("wait", Cmd_Wait_f);
}
