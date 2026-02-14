// Copyright (C) 1999-2005 ID Software, Inc.
// Copyright (C) 2023-2025 Noire.dev
// ThreeCore — GPLv2; see LICENSE for details.

#define	SVF_NOCLIENT			1	// don't send entity to clients, even if it has effects
#define SVF_BOT					2	// set if the entity is a bot
#define	SVF_BROADCAST			4	// send to all connected clients
#define	SVF_PORTAL				8	// merge a second pvs at origin2 into snapshots
#define	SVF_USE_CURRENT_ORIGIN	16	// entity->r.currentOrigin instead of entity->s.origin
#define SVF_NOSERVERINFO		32	// don't send CS_SERVERINFO updates to this client
#define SVF_SELF_PORTAL2		64  // merge a second pvs at entity->r.s.origin2 into snapshots

typedef struct {
	entityState_t	s;				// communicated by server to clients

	qboolean	linked;				// qfalse if not in any good cluster
	int			linkcount;

	int			svFlags;			// SVF_NOCLIENT, SVF_BROADCAST, etc

	// only send to this client when SVF_SINGLECLIENT is set
	int			singleClient;		

	qboolean	bmodel;				// if false, assume an explicit mins / maxs bounding box
									// only set by trap_SetBrushModel
	vec3_t		mins, maxs;
	int			contents;			// CONTENTS_TRIGGER, CONTENTS_SOLID, CONTENTS_BODY, etc
									// a non-solid entity should set to 0

	vec3_t		absmin, absmax;		// derived from mins/maxs and origin + rotation

	// currentOrigin will be used for all collision detection and world linking.
	// it will not necessarily be the same as the trajectory evaluation for the current
	// time, because each entity must be moved one at a time after time is advanced
	// to avoid simultanious collision issues
	vec3_t		currentOrigin;
	vec3_t		currentAngles;

	// when a trace call is made and passEntityNum != ENTITYNUM_NONE,
	// an ent will be excluded from testing if:
	// ent->s.number == passEntityNum	(don't interact with self)
	// ent->s.ownerNum = passEntityNum	(don't interact with your own missiles)
	// entity[ent->s.ownerNum].ownerNum = passEntityNum	(don't interact with other missiles from owner)
	int			ownerNum;
} entityShared_t;

typedef struct {
	entityState_t	s;				// communicated by server to clients
	entityShared_t	r;				// shared by both the server system and game
} sharedEntity_t;

typedef enum {
	G_LOCATE_GAME_DATA,
    G_DROP_CLIENT,
    G_SEND_SERVER_COMMAND,
    G_LINKENTITY,
    G_UNLINKENTITY,
    G_ENTITIES_IN_BOX,
    G_ENTITY_CONTACT,
    G_TRACE,
    G_POINT_CONTENTS,
    G_SET_BRUSH_MODEL,
    G_IN_PVS,
    G_SET_CONFIGSTRING,
    G_GET_CONFIGSTRING,
    G_SET_USERINFO,
    G_GET_USERINFO,
    G_GET_SERVERINFO,
    G_ADJUST_AREA_PORTAL_STATE,
    G_BOT_ALLOCATE_CLIENT,
    G_GET_USERCMD,
    G_GET_ENTITY_TOKEN,
	BOTLIB_GET_CONSOLE_MESSAGE,
	BOTLIB_USER_COMMAND
} gameImport_t;

typedef enum {
	GAME_INIT,
	GAME_SHUTDOWN,
	GAME_CLIENT_CONNECT,
	GAME_CLIENT_BEGIN,
	GAME_CLIENT_USERINFO_CHANGED,
	GAME_CLIENT_DISCONNECT,
	GAME_CLIENT_COMMAND,
	GAME_CLIENT_THINK,
	GAME_RUN_FRAME,	
	GAME_CONSOLE_COMMAND,
	BOTAI_START_FRAME
} gameExport_t;
