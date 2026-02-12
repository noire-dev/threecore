// Copyright (C) 1999-2005 ID Software, Inc.
// Copyright (C) 2023-2025 Noire.dev
// ThreeCore — GPLv2; see LICENSE for details.

typedef struct {
	connstate_t		connState;
	int				connectPacketCount;
	int				clientNum;
	char			servername[MAX_STRING_CHARS];
	char			updateInfoString[MAX_STRING_CHARS];
	char			messageString[MAX_STRING_CHARS];
} uiClientState_t;

typedef enum {
    UI_KEY_SETBINDING,
    UI_KEY_CLEARSTATES,
    UI_KEY_SETCATCHER,
    UI_GETCLIPBOARDDATA,
    UI_GETCLIENTSTATE,
    UI_GETCONFIGSTRING,
    UI_LAN_GETPINGQUEUECOUNT,
    UI_LAN_CLEARPING,
    UI_LAN_GETPING,
    UI_LAN_GETPINGINFO,
    UI_MEMORY_REMAINING,
    UI_LAN_GETSERVERCOUNT,
    UI_LAN_GETSERVERADDRESSSTRING,
    UI_CONSOLESYNC
} uiImport_t;

typedef enum {
	UIMENU_NONE,
	UIMENU_MAIN,
	UIMENU_INGAME
} uiMenuCommand_t;

typedef enum {
	UI_INIT = 0,
	UI_SHUTDOWN,
	UI_KEY_EVENT,
	UI_MOUSE_EVENT,
	UI_REFRESH,
	UI_IS_FULLSCREEN,
	UI_SET_ACTIVE_MENU,
	UI_CONSOLE_COMMAND,
	UI_DRAW_CONNECT_SCREEN
} uiExport_t;
