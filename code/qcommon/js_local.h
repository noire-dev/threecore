// Copyright (C) 1999-2005 ID Software, Inc.
// Copyright (C) 2023-2026 Noire.dev
// SourceTech — GPLv2; see LICENSE for details.

void JS_Init(void);
qboolean JSOpenFile(const char* filename);
qboolean JSEval(const char* code, qboolean doPrint, qboolean doCopy, char* buffer, int bufferLength);
