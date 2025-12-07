// Copyright (C) 1999-2005 ID Software, Inc.
// Copyright (C) 2023-2026 Noire.dev
// SourceTech — GPLv2; see LICENSE for details.

struct aas_clientmove_s;
struct aas_entityinfo_s;
struct aas_areainfo_s;
struct aas_altroutegoal_s;
struct aas_predictroute_s;
struct bot_consolemessage_s;
struct bot_match_s;
struct bot_goal_s;
struct bot_moveresult_s;
struct bot_initmove_s;
struct weaponinfo_s;

// Print types
#define PRT_MESSAGE 1
#define PRT_WARNING 2
#define PRT_ERROR 3
#define PRT_FATAL 4
#define PRT_EXIT 5

// botlib error codes
#define BLERR_NOERROR 0              // no error
#define BLERR_LIBRARYNOTSETUP 1      // library not setup
#define BLERR_INVALIDENTITYNUMBER 2  // invalid entity number
#define BLERR_NOAASFILE 3            // no AAS file available
#define BLERR_CANNOTOPENAASFILE 4    // cannot open AAS file
#define BLERR_WRONGAASFILEID 5       // incorrect AAS file id
#define BLERR_WRONGAASFILEVERSION 6  // incorrect AAS file version
#define BLERR_CANNOTREADAASLUMP 7    // cannot read AAS file lump

// action flags
#define ACTION_ATTACK 0x00000001
#define ACTION_USE 0x00000002
#define ACTION_RESPAWN 0x00000008
#define ACTION_JUMP 0x00000010
#define ACTION_MOVEUP 0x00000020
#define ACTION_CROUCH 0x00000080
#define ACTION_MOVEDOWN 0x00000100
#define ACTION_MOVEFORWARD 0x00000200
#define ACTION_MOVEBACK 0x00000800
#define ACTION_MOVELEFT 0x00001000
#define ACTION_MOVERIGHT 0x00002000
#define ACTION_DELAYEDJUMP 0x00008000
#define ACTION_TALK 0x00010000
#define ACTION_GESTURE 0x00020000
#define ACTION_WALK 0x00080000
#define ACTION_JUMPEDLASTFRAME 0x00100000

typedef struct bot_input_s {
	float thinktime;    // time since last output (in seconds)
	vec3_t dir;         // movement direction
	float speed;        // speed in the range [0, 400]
	vec3_t viewangles;  // the view angles
	int actionflags;    // one of the ACTION_? flags
	int weapon;         // weapon to use
} bot_input_t;

#ifndef BSPTRACE
#define BSPTRACE
typedef struct bsp_surface_s {
	char name[16];
	int flags;
	int value;
} bsp_surface_t;

typedef struct bsp_trace_s {
	qboolean allsolid;      // if true, plane is not valid
	qboolean startsolid;    // if true, the initial point was in a solid area
	float fraction;         // time completed, 1.0 = didn't hit anything
	vec3_t endpos;          // final position
	cplane_t plane;         // surface normal at impact
	float exp_dist;         // expanded plane distance
	int sidenum;            // number of the brush side hit
	bsp_surface_t surface;  // the hit point surface
	int contents;           // contents on other side of surface hit
	int ent;                // number of entity hit
} bsp_trace_t;
#endif

typedef struct bot_entitystate_s {
	int type;           // entity type
	int flags;          // entity flags
	vec3_t origin;      // origin of the entity
	vec3_t angles;      // angles of the model
	vec3_t old_origin;  // for lerping
	vec3_t mins;        // bounding box minimums
	vec3_t maxs;        // bounding box maximums
	int groundent;      // ground entity
	int solid;          // solid type
	int modelindex;     // model used
	int modelindex2;    // weapons, CTF flags, etc
	int frame;          // model frame number
	int event;          // impulse events -- muzzle flashes, footsteps, etc
	int eventParm;      // even parameter
	int powerups;       // bit flags
	int weapon;         // determines weapon and flash model, etc
	int legsAnim;       // mask off ANIM_TOGGLEBIT
	int torsoAnim;      // mask off ANIM_TOGGLEBIT
} bot_entitystate_t;

typedef struct botlib_import_s {
	void(QDECL* Print)(int type, const char* fmt, ...) __attribute__((format(printf, 2, 3)));
	void (*Trace)(bsp_trace_t* trace, vec3_t start, vec3_t mins, vec3_t maxs, vec3_t end, int passent, int contentmask);
	void (*EntityTrace)(bsp_trace_t* trace, vec3_t start, vec3_t mins, vec3_t maxs, vec3_t end, int entnum, int contentmask);
	int (*PointContents)(vec3_t point);
	int (*inPVS)(vec3_t p1, vec3_t p2);
	char* (*BSPEntityData)(void);
	void (*BSPModelMinsMaxsOrigin)(int modelnum, vec3_t angles, vec3_t mins, vec3_t maxs, vec3_t origin);
	void (*BotClientCommand)(int client, const char* command);
	int (*FS_FOpenFile)(const char* qpath, fileHandle_t* file, fsMode_t mode);
	int (*FS_Read)(void* buffer, int len, fileHandle_t f);
	int (*FS_Write)(const void* buffer, int len, fileHandle_t f);
	void (*FS_FCloseFile)(fileHandle_t f);
	int (*FS_Seek)(fileHandle_t f, long offset, fsOrigin_t origin);
	int (*Sys_Milliseconds)(void);
} botlib_import_t;

typedef struct aas_export_s {
	int (*AAS_Initialized)(void);
	float (*AAS_Time)(void);
	int (*AAS_PointAreaNum)(vec3_t point);
	int (*AAS_TraceAreas)(vec3_t start, vec3_t end, int* areas, vec3_t* points, int maxareas);
} aas_export_t;

typedef struct ea_export_s {
	void (*EA_Command)(int client, const char* command);
	void (*EA_Gesture)(int client);
	void (*EA_Attack)(int client);
	void (*EA_Use)(int client);
	void (*EA_SelectWeapon)(int client, int weapon);
	void (*EA_View)(int client, vec3_t viewangles);
	void (*EA_GetInput)(int client, float thinktime, bot_input_t* input);
	void (*EA_ResetInput)(int client);
} ea_export_t;

typedef struct ai_export_s {
	int (*BotTouchingGoal)(const vec3_t origin, const struct bot_goal_s* goal);
	void (*BotMoveToGoal)(int movestate, struct bot_goal_s* goal, int travelflags);
	void (*BotResetMoveState)(int movestate);
	int (*BotAllocMoveState)(void);
	void (*BotFreeMoveState)(int handle);
	void (*BotInitMoveState)(int handle, struct bot_initmove_s* initmove);
} ai_export_t;

typedef struct botlib_export_s {
	aas_export_t aas;
	ea_export_t ea;
	ai_export_t ai;

	int (*BotLibSetup)(void);
	int (*BotLibShutdown)(void);
	int (*BotLibStartFrame)(float time);
	int (*BotLibLoadMap)(const char* mapname);
	int (*BotLibUpdateEntity)(int ent, bot_entitystate_t* state);
} botlib_export_t;

botlib_export_t *GetBotLibAPI(botlib_import_t *import);
