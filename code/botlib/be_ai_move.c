// Copyright (C) 1999-2005 ID Software, Inc.
// Copyright (C) 2023-2025 Noire.dev
// SourceTech — GPLv2; see LICENSE for details.

#include "../qcommon/q_shared.h"
#include "aasfile.h"
#include "botlib.h"
#include "be_aas.h"
#include "be_aas_funcs.h"
#include "be_aas_def.h"
#include "be_interface.h"

#include "be_ea.h"
#include "be_ai_goal.h"
#include "be_ai_move.h"


typedef struct bot_movestate_s {
	// input vars (all set outside the movement code)
	vec3_t origin;      // origin of the bot
	vec3_t velocity;    // velocity of the bot
	vec3_t viewoffset;  // view offset
	int entitynum;      // entity number of the bot
	int client;         // client number of the bot
	float thinktime;    // time the bot thinks
	int presencetype;   // presencetype of the bot
	vec3_t viewangles;  // view angles of the bot
	// state vars
	int areanum;                            // area the bot is in
	int lastareanum;                        // last area the bot was in
	int lastgoalareanum;                    // last goal area number
	int lastreachnum;                       // last reachability number
	vec3_t lastorigin;                      // origin previous cycle
	int reachareanum;                       // area number of the reachabilty
	int moveflags;                          // movement flags
	int jumpreach;                          // set when jumped
	float reachability_time;                // time to use current reachability
} bot_movestate_t;

// prediction times
#define PREDICTIONTIME_JUMP 3  // in seconds
#define PREDICTIONTIME_MOVE 2  // in seconds

static int sv_maxstep;
static int sv_maxbarrier;
static int sv_gravity;

static bot_movestate_t* botmovestates[MAX_CLIENTS + 1];

int BotAllocMoveState(void) {
	int i;

	for(i = 1; i <= MAX_CLIENTS; i++) {
		if(!botmovestates[i]) {
			botmovestates[i] = malloc(sizeof(bot_movestate_t));
			return i;
		}  
	}  
	return 0;
}

void BotFreeMoveState(int handle) {
	if(handle <= 0 || handle > MAX_CLIENTS) {
		botimport.Print(PRT_FATAL, "move state handle %d out of range\n", handle);
		return;
	}  
	if(!botmovestates[handle]) {
		botimport.Print(PRT_FATAL, "invalid move state %d\n", handle);
		return;
	}  
	free(botmovestates[handle]);
	botmovestates[handle] = NULL;
}

static bot_movestate_t* BotMoveStateFromHandle(int handle) {
	if(handle <= 0 || handle > MAX_CLIENTS) {
		botimport.Print(PRT_FATAL, "move state handle %d out of range\n", handle);
		return NULL;
	}  
	if(!botmovestates[handle]) {
		botimport.Print(PRT_FATAL, "invalid move state %d\n", handle);
		return NULL;
	}  
	return botmovestates[handle];
}

void BotInitMoveState(int handle, bot_initmove_t* initmove) {
	bot_movestate_t* ms;

	ms = BotMoveStateFromHandle(handle);
	if(!ms) return;
	VectorCopy(initmove->origin, ms->origin);
	VectorCopy(initmove->velocity, ms->velocity);
	VectorCopy(initmove->viewoffset, ms->viewoffset);
	ms->entitynum = initmove->entitynum;
	ms->client = initmove->client;
	ms->thinktime = initmove->thinktime;
	ms->presencetype = initmove->presencetype;
	VectorCopy(initmove->viewangles, ms->viewangles);
	//
	ms->moveflags &= ~MFL_ONGROUND;
	if(initmove->or_moveflags & MFL_ONGROUND) ms->moveflags |= MFL_ONGROUND;
	ms->moveflags &= ~MFL_TELEPORTED;
	if(initmove->or_moveflags & MFL_TELEPORTED) ms->moveflags |= MFL_TELEPORTED;
	ms->moveflags &= ~MFL_WATERJUMP;
	if(initmove->or_moveflags & MFL_WATERJUMP) ms->moveflags |= MFL_WATERJUMP;
	ms->moveflags &= ~MFL_WALK;
	if(initmove->or_moveflags & MFL_WALK) ms->moveflags |= MFL_WALK;
}

static float AngleDiff(float ang1, float ang2) {
	float diff;

	diff = ang1 - ang2;
	if(ang1 > ang2) {
		if(diff > 180.0) diff -= 360.0;
	}  
	else {
		if(diff < -180.0) diff += 360.0;
	}  
	return diff;
}

int BotFuzzyPointReachabilityArea(vec3_t origin) {
	int firstareanum, j, x, y, z;
	int areas[10], numareas, areanum, bestareanum;
	float dist, bestdist;
	vec3_t points[10], v, end;

	firstareanum = 0;
	areanum = AAS_PointAreaNum(origin);
	if(areanum) {
		firstareanum = areanum;
		if(AAS_AreaReachability(areanum)) return areanum;
	}  
	VectorCopy(origin, end);
	end[2] += 4;
	numareas = AAS_TraceAreas(origin, end, areas, points, 10);
	for(j = 0; j < numareas; j++) {
		if(AAS_AreaReachability(areas[j])) return areas[j];
	}  
	bestdist = 999999;
	bestareanum = 0;
	for(z = 1; z >= -1; z -= 1) {
		for(x = 1; x >= -1; x -= 1) {
			for(y = 1; y >= -1; y -= 1) {
				VectorCopy(origin, end);
				end[0] += x * 8;
				end[1] += y * 8;
				end[2] += z * 12;
				numareas = AAS_TraceAreas(origin, end, areas, points, 10);
				for(j = 0; j < numareas; j++) {
					if(AAS_AreaReachability(areas[j])) {
						VectorSubtract(points[j], origin, v);
						dist = VectorLength(v);
						if(dist < bestdist) {
							bestareanum = areas[j];
							bestdist = dist;
						}  
					}  
					if(!firstareanum) firstareanum = areas[j];
				}  
			}  
		}  
		if(bestareanum) return bestareanum;
	}  
	return firstareanum;
}

static int BotValidTravel(vec3_t origin, aas_reachability_t* reach, int travelflags) {
	// if the reachability uses an unwanted travel type
	if(AAS_TravelFlagForType(reach->traveltype) & ~travelflags) return qfalse;
	// don't go into areas with bad travel types
	if(AAS_AreaContentsTravelFlags(reach->areanum) & ~travelflags) return qfalse;
	return qtrue;
}

int BotGetReachabilityToGoal(vec3_t origin, int areanum, int lastgoalareanum, int lastareanum, bot_goal_t* goal, int travelflags, int* flags) {
	int i, t, besttime, bestreachnum, reachnum;
	aas_reachability_t reach;

	// if not in a valid area
	if(!areanum) return 0;
	//
	if(AAS_AreaDoNotEnter(areanum) || AAS_AreaDoNotEnter(goal->areanum)) {
		travelflags |= TFL_DONOTENTER;
	}  
	// use the routing to find the next area to go to
	besttime = 0;
	bestreachnum = 0;
	//
	for(reachnum = AAS_NextAreaReachability(areanum, 0); reachnum; reachnum = AAS_NextAreaReachability(areanum, reachnum)) {
        // get the reachability from the number
		AAS_ReachabilityFromNum(reachnum, &reach);
		// NOTE: do not go back to the previous area if the goal didn't change
		// NOTE: is this actually avoidance of local routing minima between two areas???
		if(lastgoalareanum == goal->areanum && reach.areanum == lastareanum) continue;
		// if (AAS_AreaContentsTravelFlags(reach.areanum) & ~travelflags) continue;
		// if the travel isn't valid
		if(!BotValidTravel(origin, &reach, travelflags)) continue;
		// get the travel time
		t = AAS_AreaTravelTimeToGoalArea(reach.areanum, reach.end, goal->areanum, travelflags);
		// if the goal area isn't reachable from the reachable area
		if(!t) continue;
		// add the travel time towards the area
		t += reach.traveltime;  // + AAS_AreaTravelTime(areanum, origin, reach.start);
		// if the travel time is better than the ones already found
		if(!besttime || t < besttime) {
			besttime = t;
			bestreachnum = reachnum;
		}  
	}  
	//
	return bestreachnum;
}

static float BotGapDistance(vec3_t origin, vec3_t hordir, int entnum) {
	int dist;
	float startz;
	vec3_t start, end;
	aas_trace_t trace;

	{
		VectorCopy(origin, start);
		VectorCopy(origin, end);
		end[2] -= 60;
		trace = AAS_TraceClientBBox(start, end, PRESENCE_CROUCH, entnum);
		if(trace.fraction >= 1) return 1;
		startz = trace.endpos[2] + 1;
	}
	//
	for(dist = 8; dist <= 100; dist += 8) {
		VectorMA(origin, dist, hordir, start);
		start[2] = startz + 24;
		VectorCopy(start, end);
		end[2] -= 48 + sv_maxbarrier;
		trace = AAS_TraceClientBBox(start, end, PRESENCE_CROUCH, entnum);
		// if solid is found the bot can't walk any further and fall into a gap
		if(!trace.startsolid) {
			// if it is a gap
			if(trace.endpos[2] < startz - sv_maxstep - 8) {
				VectorCopy(trace.endpos, end);
				end[2] -= 20;
				if(AAS_PointContents(end) & CONTENTS_WATER) break;
				// if a gap is found slow down
				// botimport.Print(PRT_MESSAGE, "gap at %i\n", dist);
				return dist;
			}  
			startz = trace.endpos[2];
		}  
	}  
	return 0;
}

static int BotCheckBarrierJump(bot_movestate_t* ms, vec3_t dir, float speed) {
	vec3_t start, hordir, end;
	aas_trace_t trace;

	VectorCopy(ms->origin, end);
	end[2] += sv_maxbarrier;
	// trace right up
	trace = AAS_TraceClientBBox(ms->origin, end, PRESENCE_NORMAL, ms->entitynum);
	// this shouldn't happen... but we check anyway
	if(trace.startsolid) return qfalse;
	// if very low ceiling it isn't possible to jump up to a barrier
	if(trace.endpos[2] - ms->origin[2] < sv_maxstep) return qfalse;
	//
	hordir[0] = dir[0];
	hordir[1] = dir[1];
	hordir[2] = 0;
	VectorNormalize(hordir);
	VectorMA(ms->origin, ms->thinktime * speed * 0.5, hordir, end);
	VectorCopy(trace.endpos, start);
	end[2] = trace.endpos[2];
	// trace from previous trace end pos horizontally in the move direction
	trace = AAS_TraceClientBBox(start, end, PRESENCE_NORMAL, ms->entitynum);
	// again this shouldn't happen
	if(trace.startsolid) return qfalse;
	//
	VectorCopy(trace.endpos, start);
	VectorCopy(trace.endpos, end);
	end[2] = ms->origin[2];
	// trace down from the previous trace end pos
	trace = AAS_TraceClientBBox(start, end, PRESENCE_NORMAL, ms->entitynum);
	// if solid
	if(trace.startsolid) return qfalse;
	// if no obstacle at all
	if(trace.fraction >= 1.0) return qfalse;
	// if less than the maximum step height
	if(trace.endpos[2] - ms->origin[2] < sv_maxstep) return qfalse;
	//
	EA_Jump(ms->client);
	EA_Move(ms->client, hordir, speed);
	ms->moveflags |= MFL_BARRIERJUMP;
	// there is a barrier
	return qtrue;
}

static int BotSwimInDirection(bot_movestate_t* ms, vec3_t dir, float speed, int type) {
	vec3_t normdir;

	VectorCopy(dir, normdir);
	VectorNormalize(normdir);
	EA_Move(ms->client, normdir, speed);
	return qtrue;
}

static int BotWalkInDirection(bot_movestate_t* ms, vec3_t dir, float speed, int type) {
	vec3_t hordir, cmdmove, velocity, tmpdir, origin;
	int presencetype, maxframes, cmdframes, stopevent;
	aas_clientmove_t move;
	float dist;

	if(AAS_OnGround(ms->origin, ms->presencetype, ms->entitynum)) ms->moveflags |= MFL_ONGROUND;
	// if the bot is on the ground
	if(ms->moveflags & MFL_ONGROUND) {
		// if there is a barrier the bot can jump on
		if(BotCheckBarrierJump(ms, dir, speed)) return qtrue;
		// remove barrier jump flag
		ms->moveflags &= ~MFL_BARRIERJUMP;
		// get the presence type for the movement
		if((type & MOVE_CROUCH) && !(type & MOVE_JUMP))
			presencetype = PRESENCE_CROUCH;
		else
			presencetype = PRESENCE_NORMAL;
		// horizontal direction
		hordir[0] = dir[0];
		hordir[1] = dir[1];
		hordir[2] = 0;
		VectorNormalize(hordir);
		// if the bot is not supposed to jump
		if(!(type & MOVE_JUMP)) {
			// if there is a gap, try to jump over it
			if(BotGapDistance(ms->origin, hordir, ms->entitynum) > 0) type |= MOVE_JUMP;
		}  
		// get command movement
		VectorScale(hordir, speed, cmdmove);
		VectorCopy(ms->velocity, velocity);
		//
		if(type & MOVE_JUMP) {
			// botimport.Print(PRT_MESSAGE, "trying jump\n");
			cmdmove[2] = 400;
			maxframes = PREDICTIONTIME_JUMP / 0.1;
			cmdframes = 1;
			stopevent = SE_HITGROUND | SE_HITGROUNDDAMAGE | SE_ENTERWATER | SE_ENTERSLIME | SE_ENTERLAVA;
		}  
		else {
			maxframes = 2;
			cmdframes = 2;
			stopevent = SE_HITGROUNDDAMAGE | SE_ENTERWATER | SE_ENTERSLIME | SE_ENTERLAVA;
		}  
		// AAS_ClearShownDebugLines();
		//
		VectorCopy(ms->origin, origin);
		origin[2] += 0.5;
		AAS_PredictClientMovement(&move, ms->entitynum, origin, presencetype, qtrue, velocity, cmdmove, cmdframes, maxframes, 0.1f, stopevent, 0, qfalse);  // qtrue);
		// if prediction time wasn't enough to fully predict the movement
		if(move.frames >= maxframes && (type & MOVE_JUMP)) {
			// botimport.Print(PRT_MESSAGE, "client %d: max prediction frames\n", ms->client);
			return qfalse;
		}  
		// don't enter slime or lava and don't fall from too high
		if(move.stopevent & (SE_ENTERSLIME | SE_ENTERLAVA | SE_HITGROUNDDAMAGE)) {
			// botimport.Print(PRT_MESSAGE, "client %d: would be hurt ", ms->client);
			// if (move.stopevent & SE_ENTERSLIME) botimport.Print(PRT_MESSAGE, "slime\n");
			// if (move.stopevent & SE_ENTERLAVA) botimport.Print(PRT_MESSAGE, "lava\n");
			// if (move.stopevent & SE_HITGROUNDDAMAGE) botimport.Print(PRT_MESSAGE, "hitground\n");
			return qfalse;
		}  
		// if ground was hit
		if(move.stopevent & SE_HITGROUND) {
			// check for nearby gap
			VectorNormalize2(move.velocity, tmpdir);
			dist = BotGapDistance(move.endpos, tmpdir, ms->entitynum);
			if(dist > 0) return qfalse;
			//
			dist = BotGapDistance(move.endpos, hordir, ms->entitynum);
			if(dist > 0) return qfalse;
		}  
		// get horizontal movement
		tmpdir[0] = move.endpos[0] - ms->origin[0];
		tmpdir[1] = move.endpos[1] - ms->origin[1];
		tmpdir[2] = 0;
		//
		// AAS_DrawCross(move.endpos, 4, LINECOLOR_BLUE);
		// the bot is blocked by something
		if(VectorLength(tmpdir) < speed * ms->thinktime * 0.5) return qfalse;
		// perform the movement
		if(type & MOVE_JUMP) EA_Jump(ms->client);
		if(type & MOVE_CROUCH) EA_Crouch(ms->client);
		EA_Move(ms->client, hordir, speed);
		// movement was successful
		return qtrue;
	}  
	else {
		if(ms->moveflags & MFL_BARRIERJUMP) {
			// if near the top or going down
			if(ms->velocity[2] < 50) {
				EA_Move(ms->client, dir, speed);
			}  
		}  
		// FIXME: do air control to avoid hazards
		return qtrue;
	}  
}

int BotMoveInDirection(int movestate, vec3_t dir, float speed, int type) {
	bot_movestate_t* ms;

	ms = BotMoveStateFromHandle(movestate);
	if(!ms) return qfalse;
	// if swimming
	if(AAS_Swimming(ms->origin)) {
		return BotSwimInDirection(ms, dir, speed, type);
	}  
	else {
		return BotWalkInDirection(ms, dir, speed, type);
	}  
}

static void BotCheckBlocked(bot_movestate_t* ms, vec3_t dir, int checkbottom, bot_moveresult_t* result) {
	vec3_t mins, maxs, end, up = {0, 0, 1};
	bsp_trace_t trace;

	// test for entities obstructing the bot's path
	AAS_PresenceTypeBoundingBox(ms->presencetype, mins, maxs);
	//
	if(fabs(DotProduct(dir, up)) < 0.7) {
		mins[2] += sv_maxstep;  // if the bot can step on
		maxs[2] -= 10;                 // a little lower to avoid low ceiling
	}  
	VectorMA(ms->origin, 3, dir, end);
	trace = AAS_Trace(ms->origin, mins, maxs, end, ms->entitynum, CONTENTS_SOLID | CONTENTS_PLAYERCLIP | CONTENTS_BODY);
	// if not started in solid and not hitting the world entity
	if(!trace.startsolid && (trace.ent != ENTITYNUM_WORLD && trace.ent != ENTITYNUM_NONE)) {
		result->blocked = qtrue;
		result->blockentity = trace.ent;
	} else if(checkbottom && !AAS_AreaReachability(ms->areanum)) {
		// check if the bot is standing on something
		AAS_PresenceTypeBoundingBox(ms->presencetype, mins, maxs);
		VectorMA(ms->origin, -3, up, end);
		trace = AAS_Trace(ms->origin, mins, maxs, end, ms->entitynum, CONTENTS_SOLID | CONTENTS_PLAYERCLIP);
		if(!trace.startsolid && (trace.ent != ENTITYNUM_WORLD && trace.ent != ENTITYNUM_NONE)) {
			result->blocked = qtrue;
			result->blockentity = trace.ent;
			result->flags |= MOVERESULT_ONTOPOFOBSTACLE;
		}  
	}  
}

static bot_moveresult_t BotTravel_Walk(bot_movestate_t* ms, aas_reachability_t* reach) {
	float dist, speed;
	vec3_t hordir;
	bot_moveresult_t_cleared(result);

	// first walk straight to the reachability start
	hordir[0] = reach->start[0] - ms->origin[0];
	hordir[1] = reach->start[1] - ms->origin[1];
	hordir[2] = 0;
	dist = VectorNormalize(hordir);
	//
	BotCheckBlocked(ms, hordir, qtrue, &result);
	//
	if(dist < 10) {
		// walk straight to the reachability end
		hordir[0] = reach->end[0] - ms->origin[0];
		hordir[1] = reach->end[1] - ms->origin[1];
		hordir[2] = 0;
		dist = VectorNormalize(hordir);
	}  
	// if going towards a crouch area
	if(!(AAS_AreaPresenceType(reach->areanum) & PRESENCE_NORMAL)) {
		// if pretty close to the reachable area
		if(dist < 20) EA_Crouch(ms->client);
	}  
	//
	dist = BotGapDistance(ms->origin, hordir, ms->entitynum);
	//
	if(ms->moveflags & MFL_WALK) {
		if(dist > 0)
			speed = 200 - (180 - 1 * dist);
		else
			speed = 200;
		EA_Walk(ms->client);
	}  
	else {
		if(dist > 0)
			speed = 400 - (360 - 2 * dist);
		else
			speed = 400;
	}  
	// elementary action move in direction
	EA_Move(ms->client, hordir, speed);
	VectorCopy(hordir, result.movedir);
	//
	return result;
}

static bot_moveresult_t BotTravel_Crouch(bot_movestate_t* ms, aas_reachability_t* reach) {
	float speed;
	vec3_t hordir;
	bot_moveresult_t_cleared(result);

	//
	speed = 400;
	// walk straight to reachability end
	hordir[0] = reach->end[0] - ms->origin[0];
	hordir[1] = reach->end[1] - ms->origin[1];
	hordir[2] = 0;
	VectorNormalize(hordir);
	//
	BotCheckBlocked(ms, hordir, qtrue, &result);
	// elementary actions
	EA_Crouch(ms->client);
	EA_Move(ms->client, hordir, speed);
	//
	VectorCopy(hordir, result.movedir);
	//
	return result;
}

static bot_moveresult_t BotTravel_BarrierJump(bot_movestate_t* ms, aas_reachability_t* reach) {
	float dist, speed;
	vec3_t hordir;
	bot_moveresult_t_cleared(result);

	// walk straight to reachability start
	hordir[0] = reach->start[0] - ms->origin[0];
	hordir[1] = reach->start[1] - ms->origin[1];
	hordir[2] = 0;
	dist = VectorNormalize(hordir);
	//
	BotCheckBlocked(ms, hordir, qtrue, &result);
	// if pretty close to the barrier
	if(dist < 9) {
		EA_Jump(ms->client);
	}  
	else {
		if(dist > 60) dist = 60;
		speed = 360 - (360 - 6 * dist);
		EA_Move(ms->client, hordir, speed);
	}  
	VectorCopy(hordir, result.movedir);
	//
	return result;
}

static bot_moveresult_t BotFinishTravel_BarrierJump(bot_movestate_t* ms, aas_reachability_t* reach) {
	vec3_t hordir;
	bot_moveresult_t_cleared(result);

	// if near the top or going down
	if(ms->velocity[2] < 250) {
		hordir[0] = reach->end[0] - ms->origin[0];
		hordir[1] = reach->end[1] - ms->origin[1];
		hordir[2] = 0;
		//
		BotCheckBlocked(ms, hordir, qtrue, &result);
		//
		EA_Move(ms->client, hordir, 400);
		VectorCopy(hordir, result.movedir);
	}  
	//
	return result;
}

static bot_moveresult_t BotTravel_Swim(bot_movestate_t* ms, aas_reachability_t* reach) {
	vec3_t dir;
	bot_moveresult_t_cleared(result);

	// swim straight to reachability end
	VectorSubtract(reach->start, ms->origin, dir);
	VectorNormalize(dir);
	//
	BotCheckBlocked(ms, dir, qtrue, &result);
	// elementary actions
	EA_Move(ms->client, dir, 400);
	//
	VectorCopy(dir, result.movedir);
	vectoangles(dir, result.ideal_viewangles);
	result.flags |= MOVERESULT_SWIMVIEW;
	//
	return result;
}

static bot_moveresult_t BotTravel_WaterJump(bot_movestate_t* ms, aas_reachability_t* reach) {
	vec3_t dir, hordir;
	float dist;
	bot_moveresult_t_cleared(result);

	// swim straight to reachability end
	VectorSubtract(reach->end, ms->origin, dir);
	VectorCopy(dir, hordir);
	hordir[2] = 0;
	dir[2] += 15 + crandom() * 40;
	// botimport.Print(PRT_MESSAGE, "BotTravel_WaterJump: dir[2] = %f\n", dir[2]);
	VectorNormalize(dir);
	dist = VectorNormalize(hordir);
	// elementary actions
	// EA_Move(ms->client, dir, 400);
	EA_MoveForward(ms->client);
	// move up if close to the actual out of water jump spot
	if(dist < 40) EA_MoveUp(ms->client);
	// set the ideal view angles
	vectoangles(dir, result.ideal_viewangles);
	result.flags |= MOVERESULT_MOVEMENTVIEW;
	//
	VectorCopy(dir, result.movedir);
	//
	return result;
}

static bot_moveresult_t BotFinishTravel_WaterJump(bot_movestate_t* ms, aas_reachability_t* reach) {
	vec3_t dir, pnt;
	bot_moveresult_t_cleared(result);

	// botimport.Print(PRT_MESSAGE, "BotFinishTravel_WaterJump\n");
	// if waterjumping there's nothing to do
	if(ms->moveflags & MFL_WATERJUMP) return result;
	// if not touching any water anymore don't do anything
	// otherwise the bot sometimes keeps jumping?
	VectorCopy(ms->origin, pnt);
	pnt[2] -= 32;  // extra for q2dm4 near red armor/mega health
	if(!(AAS_PointContents(pnt) & (CONTENTS_LAVA | CONTENTS_SLIME | CONTENTS_WATER))) return result;
	// swim straight to reachability end
	VectorSubtract(reach->end, ms->origin, dir);
	dir[0] += crandom() * 10;
	dir[1] += crandom() * 10;
	dir[2] += 70 + crandom() * 10;
	// elementary actions
	EA_Move(ms->client, dir, 400);
	// set the ideal view angles
	vectoangles(dir, result.ideal_viewangles);
	result.flags |= MOVERESULT_MOVEMENTVIEW;
	//
	VectorCopy(dir, result.movedir);
	//
	return result;
}

static bot_moveresult_t BotTravel_WalkOffLedge(bot_movestate_t* ms, aas_reachability_t* reach) {
	vec3_t hordir, dir;
	float dist, speed, reachhordist;
	bot_moveresult_t_cleared(result);

	// check if the bot is blocked by anything
	VectorSubtract(reach->start, ms->origin, dir);
	VectorNormalize(dir);
	BotCheckBlocked(ms, dir, qtrue, &result);
	// if the reachability start and end are practically above each other
	VectorSubtract(reach->end, reach->start, dir);
	dir[2] = 0;
	reachhordist = VectorLength(dir);
	// walk straight to the reachability start
	hordir[0] = reach->start[0] - ms->origin[0];
	hordir[1] = reach->start[1] - ms->origin[1];
	hordir[2] = 0;
	dist = VectorNormalize(hordir);
	// if pretty close to the start focus on the reachability end
	if(dist < 48) {
		hordir[0] = reach->end[0] - ms->origin[0];
		hordir[1] = reach->end[1] - ms->origin[1];
		hordir[2] = 0;
		VectorNormalize(hordir);
		//
		if(reachhordist < 20) {
			speed = 100;
		}  
		else if(!AAS_HorizontalVelocityForJump(0, reach->start, reach->end, &speed)) {
			speed = 400;
		}  
	}  
	else {
		if(reachhordist < 20) {
			if(dist > 64) dist = 64;
			speed = 400 - (256 - 4 * dist);
		}  
		else {
			speed = 400;
		}  
	}  
	//
	BotCheckBlocked(ms, hordir, qtrue, &result);
	// elementary action
	EA_Move(ms->client, hordir, speed);
	VectorCopy(hordir, result.movedir);
	//
	return result;
}

static int BotAirControl(vec3_t origin, vec3_t velocity, vec3_t goal, vec3_t dir, float* speed) {
	vec3_t org, vel;
	float dist;
	int i;

	VectorCopy(origin, org);
	VectorScale(velocity, 0.1, vel);
	for(i = 0; i < 50; i++) {
		vel[2] -= sv_gravity * 0.01;
		// if going down and next position would be below the goal
		if(vel[2] < 0 && org[2] + vel[2] < goal[2]) {
			VectorScale(vel, (goal[2] - org[2]) / vel[2], vel);
			VectorAdd(org, vel, org);
			VectorSubtract(goal, org, dir);
			dist = VectorNormalize(dir);
			if(dist > 32) dist = 32;
			*speed = 400 - (400 - 13 * dist);
			return qtrue;
		}  
		else {
			VectorAdd(org, vel, org);
		}  
	}  
	VectorSet(dir, 0, 0, 0);
	*speed = 400;
	return qfalse;
}

static bot_moveresult_t BotFinishTravel_WalkOffLedge(bot_movestate_t* ms, aas_reachability_t* reach) {
	vec3_t dir, hordir, end, v;
	float dist, speed;
	bot_moveresult_t_cleared(result);

	//
	VectorSubtract(reach->end, ms->origin, dir);
	BotCheckBlocked(ms, dir, qtrue, &result);
	//
	VectorSubtract(reach->end, ms->origin, v);
	v[2] = 0;
	dist = VectorNormalize(v);
	if(dist > 16)
		VectorMA(reach->end, 16, v, end);
	else
		VectorCopy(reach->end, end);
	//
	if(!BotAirControl(ms->origin, ms->velocity, end, hordir, &speed)) {
		// go straight to the reachability end
		VectorCopy(dir, hordir);
		hordir[2] = 0;
		//
		speed = 400;
	}  
	//
	EA_Move(ms->client, hordir, speed);
	VectorCopy(hordir, result.movedir);
	//
	return result;
}

static bot_moveresult_t BotTravel_Jump(bot_movestate_t* ms, aas_reachability_t* reach) {
	vec3_t hordir, dir1, dir2, start, end, runstart;
	//	vec3_t runstart, dir1, dir2, hordir;
	int gapdist;
	float dist1, dist2, speed;
	bot_moveresult_t_cleared(result);

	//
	AAS_JumpReachRunStart(reach, runstart);
	//*
	hordir[0] = runstart[0] - reach->start[0];
	hordir[1] = runstart[1] - reach->start[1];
	hordir[2] = 0;
	VectorNormalize(hordir);
	//
	VectorCopy(reach->start, start);
	start[2] += 1;
	VectorMA(reach->start, 80, hordir, runstart);
	// check for a gap
	for(gapdist = 0; gapdist < 80; gapdist += 10) {
		VectorMA(start, gapdist + 10, hordir, end);
		end[2] += 1;
		if(AAS_PointAreaNum(end) != ms->reachareanum) break;
	}  
	if(gapdist < 80) VectorMA(reach->start, gapdist, hordir, runstart);
	//
	VectorSubtract(ms->origin, reach->start, dir1);
	dir1[2] = 0;
	dist1 = VectorNormalize(dir1);
	VectorSubtract(ms->origin, runstart, dir2);
	dir2[2] = 0;
	dist2 = VectorNormalize(dir2);
	// if just before the reachability start
	if(DotProduct(dir1, dir2) < -0.8 || dist2 < 5) {
		//		botimport.Print(PRT_MESSAGE, "between jump start and run start point\n");
		hordir[0] = reach->end[0] - ms->origin[0];
		hordir[1] = reach->end[1] - ms->origin[1];
		hordir[2] = 0;
		VectorNormalize(hordir);
		// elementary action jump
		if(dist1 < 24)
			EA_Jump(ms->client);
		else if(dist1 < 32)
			EA_DelayedJump(ms->client);
		EA_Move(ms->client, hordir, 600);
		//
		ms->jumpreach = ms->lastreachnum;
	}  
	else {
		//		botimport.Print(PRT_MESSAGE, "going towards run start point\n");
		hordir[0] = runstart[0] - ms->origin[0];
		hordir[1] = runstart[1] - ms->origin[1];
		hordir[2] = 0;
		VectorNormalize(hordir);
		//
		if(dist2 > 80) dist2 = 80;
		speed = 400 - (400 - 5 * dist2);
		EA_Move(ms->client, hordir, speed);
	}  
	VectorCopy(hordir, result.movedir);
	//
	return result;
}

static bot_moveresult_t BotFinishTravel_Jump(bot_movestate_t* ms, aas_reachability_t* reach) {
	vec3_t hordir, hordir2;
	float speed, dist;
	bot_moveresult_t_cleared(result);

	// if not jumped yet
	if(!ms->jumpreach) return result;
	// go straight to the reachability end
	hordir[0] = reach->end[0] - ms->origin[0];
	hordir[1] = reach->end[1] - ms->origin[1];
	hordir[2] = 0;
	dist = VectorNormalize(hordir);
	//
	hordir2[0] = reach->end[0] - reach->start[0];
	hordir2[1] = reach->end[1] - reach->start[1];
	hordir2[2] = 0;
	VectorNormalize(hordir2);
	//
	if(DotProduct(hordir, hordir2) < -0.5 && dist < 24) return result;
	// always use max speed when traveling through the air
	speed = 800;
	//
	EA_Move(ms->client, hordir, speed);
	VectorCopy(hordir, result.movedir);
	//
	return result;
}

static bot_moveresult_t BotTravel_Ladder(bot_movestate_t* ms, aas_reachability_t* reach) {
	// float dist, speed;
	vec3_t dir, viewdir;  //, hordir;
	vec3_t origin = {0, 0, 0};
	//	vec3_t up = {0, 0, 1};
	bot_moveresult_t_cleared(result);
	
	{
		// botimport.Print(PRT_MESSAGE, "against ladder or not on ground\n");
		VectorSubtract(reach->end, ms->origin, dir);
		VectorNormalize(dir);
		// set the ideal view angles, facing the ladder up or down
		viewdir[0] = dir[0];
		viewdir[1] = dir[1];
		viewdir[2] = 3 * dir[2];
		vectoangles(viewdir, result.ideal_viewangles);
		// elementary action
		EA_Move(ms->client, origin, 0);
		EA_MoveForward(ms->client);
		// set movement view flag so the AI can see the view is focussed
		result.flags |= MOVERESULT_MOVEMENTVIEW;
	}  

	// save the movement direction
	VectorCopy(dir, result.movedir);
	//
	return result;
}

static bot_moveresult_t BotTravel_Teleport(bot_movestate_t* ms, aas_reachability_t* reach) {
	vec3_t hordir;
	float dist;
	bot_moveresult_t_cleared(result);

	// if the bot is being teleported
	if(ms->moveflags & MFL_TELEPORTED) return result;

	// walk straight to center of the teleporter
	VectorSubtract(reach->start, ms->origin, hordir);
	if(!(ms->moveflags & MFL_SWIMMING)) hordir[2] = 0;
	dist = VectorNormalize(hordir);
	//
	BotCheckBlocked(ms, hordir, qtrue, &result);

	if(dist < 30)
		EA_Move(ms->client, hordir, 200);
	else
		EA_Move(ms->client, hordir, 400);

	if(ms->moveflags & MFL_SWIMMING) result.flags |= MOVERESULT_SWIMVIEW;

	VectorCopy(hordir, result.movedir);
	return result;
}

static bot_moveresult_t BotTravel_JumpPad(bot_movestate_t* ms, aas_reachability_t* reach) {
	vec3_t hordir;
	bot_moveresult_t_cleared(result);

	// first walk straight to the reachability start
	hordir[0] = reach->start[0] - ms->origin[0];
	hordir[1] = reach->start[1] - ms->origin[1];
	hordir[2] = 0;
	//
	BotCheckBlocked(ms, hordir, qtrue, &result);
	// elementary action move in direction
	EA_Move(ms->client, hordir, 400);
	VectorCopy(hordir, result.movedir);
	//
	return result;
}

static bot_moveresult_t BotFinishTravel_JumpPad(bot_movestate_t* ms, aas_reachability_t* reach) {
	float speed;
	vec3_t hordir;
	bot_moveresult_t_cleared(result);

	if(!BotAirControl(ms->origin, ms->velocity, reach->end, hordir, &speed)) {
		hordir[0] = reach->end[0] - ms->origin[0];
		hordir[1] = reach->end[1] - ms->origin[1];
		hordir[2] = 0;
		VectorNormalize(hordir);
		speed = 400;
	}  
	BotCheckBlocked(ms, hordir, qtrue, &result);
	// elementary action move in direction
	EA_Move(ms->client, hordir, speed);
	VectorCopy(hordir, result.movedir);
	//
	return result;
}

static int BotReachabilityTime(aas_reachability_t* reach) {
	switch(reach->traveltype & TRAVELTYPE_MASK) {
		case TRAVEL_WALK: return 5;
		case TRAVEL_CROUCH: return 5;
		case TRAVEL_BARRIERJUMP: return 5;
		case TRAVEL_LADDER: return 6;
		case TRAVEL_WALKOFFLEDGE: return 5;
		case TRAVEL_JUMP: return 5;
		case TRAVEL_SWIM: return 5;
		case TRAVEL_WATERJUMP: return 5;
		case TRAVEL_TELEPORT: return 5;
		case TRAVEL_JUMPPAD: return 10;
		default: {
			botimport.Print(PRT_ERROR, "travel type %d not implemented yet\n", reach->traveltype);
			return 8;
		}  
	}  
}

static bot_moveresult_t BotMoveInGoalArea(bot_movestate_t* ms, bot_goal_t* goal) {
	bot_moveresult_t_cleared(result);
	vec3_t dir;
	float dist, speed;

	// walk straight to the goal origin
	dir[0] = goal->origin[0] - ms->origin[0];
	dir[1] = goal->origin[1] - ms->origin[1];
	if(ms->moveflags & MFL_SWIMMING) {
		dir[2] = goal->origin[2] - ms->origin[2];
		result.traveltype = TRAVEL_SWIM;
	} else {
		dir[2] = 0;
		result.traveltype = TRAVEL_WALK;
	}

	dist = VectorNormalize(dir);
	if(dist > 100) dist = 100;
	speed = 400 - (400 - 4 * dist);
	if(speed < 10) speed = 0;

	BotCheckBlocked(ms, dir, qtrue, &result);

	EA_Move(ms->client, dir, speed);
	VectorCopy(dir, result.movedir);

	if(ms->moveflags & MFL_SWIMMING) {
		vectoangles(dir, result.ideal_viewangles);
		result.flags |= MOVERESULT_SWIMVIEW;
	}

	ms->lastreachnum = 0;
	ms->lastareanum = 0;
	ms->lastgoalareanum = goal->areanum;
	VectorCopy(ms->origin, ms->lastorigin);
	return result;
}

void BotMoveToGoal(bot_moveresult_t* result, int movestate, bot_goal_t* goal, int travelflags) {
	int reachnum, lastreachnum, foundjumppad, ent, resultflags;
	aas_reachability_t reach, lastreach;
	bot_movestate_t* ms;

	result->failure = qfalse;
	result->type = 0;
	result->blocked = qfalse;
	result->blockentity = 0;
	result->traveltype = 0;
	result->flags = 0;

	//
	ms = BotMoveStateFromHandle(movestate);
	if(!ms) return;

	if(!goal) {
#ifdef DEBUG
		botimport.Print(PRT_MESSAGE, "client %d: movetogoal -> no goal\n", ms->client);
#endif  // DEBUG
		result->failure = qtrue;
		return;
	}  
	// remove some of the move flags
	ms->moveflags &= ~(MFL_SWIMMING | MFL_AGAINSTLADDER);
	// set some of the move flags
	// NOTE: the MFL_ONGROUND flag is also set in the higher AI
	if(AAS_OnGround(ms->origin, ms->presencetype, ms->entitynum)) ms->moveflags |= MFL_ONGROUND;
	// if swimming
	if(AAS_Swimming(ms->origin)) ms->moveflags |= MFL_SWIMMING;
	// if against a ladder
	if(AAS_AgainstLadder(ms->origin)) ms->moveflags |= MFL_AGAINSTLADDER;
	// if the bot is on the ground, swimming or against a ladder
	if(ms->moveflags & (MFL_ONGROUND | MFL_SWIMMING | MFL_AGAINSTLADDER)) {
		// botimport.Print(PRT_MESSAGE, "%s: onground, swimming or against ladder\n", ClientName(ms->entitynum-1));
		//
		AAS_ReachabilityFromNum(ms->lastreachnum, &lastreach);
		// reachability area the bot is in
		ms->areanum = BotFuzzyPointReachabilityArea(ms->origin);
		//
		if(!ms->areanum) {
			result->failure = qtrue;
			result->blocked = qtrue;
			result->blockentity = 0;
			result->type = RESULTTYPE_INSOLIDAREA;
			return;
		}  
		// if the bot is in the goal area
		if(ms->areanum == goal->areanum) {
			*result = BotMoveInGoalArea(ms, goal);
			return;
		}  
		// assume we can use the reachability from the last frame
		reachnum = ms->lastreachnum;
		// if there is a last reachability
		if(reachnum) {
			AAS_ReachabilityFromNum(reachnum, &reach);
			// check if the reachability is still valid
			if(!(AAS_TravelFlagForType(reach.traveltype) & travelflags)) {
				reachnum = 0;
			} else {
				// if the goal area changed or the reachability timed out
				// or the area changed
				if(ms->lastgoalareanum != goal->areanum || ms->reachability_time < AAS_Time() || ms->lastareanum != ms->areanum) {
					reachnum = 0;
					// botimport.Print(PRT_MESSAGE, "area change or timeout\n");
				}
			}  
		}  
		resultflags = 0;
		// if the bot needs a new reachability
		if(!reachnum) {
			// get a new reachability leading towards the goal
			reachnum = BotGetReachabilityToGoal(ms->origin, ms->areanum, ms->lastgoalareanum, ms->lastareanum, goal, travelflags, &resultflags);
			// the area number the reachability starts in
			ms->reachareanum = ms->areanum;
			// reset some state variables
			ms->jumpreach = 0;                   // for TRAVEL_JUMP
			// if there is a reachability to the goal
			if(reachnum) {
				AAS_ReachabilityFromNum(reachnum, &reach);
				// set a timeout for this reachability
				ms->reachability_time = AAS_Time() + BotReachabilityTime(&reach);
				//
			}  
		}  
		//
		ms->lastreachnum = reachnum;
		ms->lastgoalareanum = goal->areanum;
		ms->lastareanum = ms->areanum;
		// if the bot has a reachability
		if(reachnum) {
			// get the reachability from the number
			AAS_ReachabilityFromNum(reachnum, &reach);
			result->traveltype = reach.traveltype;
			//
			switch(reach.traveltype & TRAVELTYPE_MASK) {
				case TRAVEL_WALK: *result = BotTravel_Walk(ms, &reach); break;
				case TRAVEL_CROUCH: *result = BotTravel_Crouch(ms, &reach); break;
				case TRAVEL_BARRIERJUMP: *result = BotTravel_BarrierJump(ms, &reach); break;
				case TRAVEL_LADDER: *result = BotTravel_Ladder(ms, &reach); break;
				case TRAVEL_WALKOFFLEDGE: *result = BotTravel_WalkOffLedge(ms, &reach); break;
				case TRAVEL_JUMP: *result = BotTravel_Jump(ms, &reach); break;
				case TRAVEL_SWIM: *result = BotTravel_Swim(ms, &reach); break;
				case TRAVEL_WATERJUMP: *result = BotTravel_WaterJump(ms, &reach); break;
				case TRAVEL_TELEPORT: *result = BotTravel_Teleport(ms, &reach); break;
				case TRAVEL_JUMPPAD: *result = BotTravel_JumpPad(ms, &reach); break;
				default: {
					botimport.Print(PRT_FATAL, "travel type %d not implemented yet\n", (reach.traveltype & TRAVELTYPE_MASK));
					break;
				}  
			}  
			result->traveltype = reach.traveltype;
			result->flags |= resultflags;
		}  
		else {
			result->failure = qtrue;
			result->flags |= resultflags;
			Com_Memset(&reach, 0, sizeof(aas_reachability_t));
		}  
	}  
	else {
		int i, numareas, areas[16];
		vec3_t end;

		// special handling of jump pads when the bot uses a jump pad without knowing it
		foundjumppad = qfalse;
		VectorMA(ms->origin, -2 * ms->thinktime, ms->velocity, end);
		numareas = AAS_TraceAreas(ms->origin, end, areas, NULL, 16);
		for(i = numareas - 1; i >= 0; i--) {
			if(AAS_AreaJumpPad(areas[i])) {
				// botimport.Print(PRT_MESSAGE, "client %d used a jumppad without knowing, area %d\n", ms->client, areas[i]);
				foundjumppad = qtrue;
				lastreachnum = BotGetReachabilityToGoal(end, areas[i], ms->lastgoalareanum, ms->lastareanum, goal, TFL_JUMPPAD, NULL);
				if(lastreachnum) {
					ms->lastreachnum = lastreachnum;
					ms->lastareanum = areas[i];
					// botimport.Print(PRT_MESSAGE, "found jumppad reachability\n");
					break;
				}  
				else {
					for(lastreachnum = AAS_NextAreaReachability(areas[i], 0); lastreachnum; lastreachnum = AAS_NextAreaReachability(areas[i], lastreachnum)) {
						// get the reachability from the number
						AAS_ReachabilityFromNum(lastreachnum, &reach);
						if((reach.traveltype & TRAVELTYPE_MASK) == TRAVEL_JUMPPAD) {
							ms->lastreachnum = lastreachnum;
							ms->lastareanum = areas[i];
							// botimport.Print(PRT_MESSAGE, "found jumppad reachability hard!!\n");
						}  
					}  
					if(lastreachnum) break;
				}  
			}  
		}  
		//
		if(ms->lastreachnum) {
			// botimport.Print(PRT_MESSAGE, "%s: NOT onground, swimming or against ladder\n", ClientName(ms->entitynum-1));
			AAS_ReachabilityFromNum(ms->lastreachnum, &reach);
			result->traveltype = reach.traveltype;
			//
			switch(reach.traveltype & TRAVELTYPE_MASK) {
				case TRAVEL_WALK: *result = BotTravel_Walk(ms, &reach); break;  // BotFinishTravel_Walk(ms, &reach); break;
				case TRAVEL_CROUCH: /*do nothing*/ break;
				case TRAVEL_BARRIERJUMP: *result = BotFinishTravel_BarrierJump(ms, &reach); break;
				case TRAVEL_LADDER: *result = BotTravel_Ladder(ms, &reach); break;
				case TRAVEL_WALKOFFLEDGE: *result = BotFinishTravel_WalkOffLedge(ms, &reach); break;
				case TRAVEL_JUMP: *result = BotFinishTravel_Jump(ms, &reach); break;
				case TRAVEL_SWIM: *result = BotTravel_Swim(ms, &reach); break;
				case TRAVEL_WATERJUMP: *result = BotFinishTravel_WaterJump(ms, &reach); break;
				case TRAVEL_TELEPORT: break;
				case TRAVEL_JUMPPAD: *result = BotFinishTravel_JumpPad(ms, &reach); break;
				default: {
					botimport.Print(PRT_FATAL, "(last) travel type %d not implemented yet\n", (reach.traveltype & TRAVELTYPE_MASK));
					break;
				}  
			}  
			result->traveltype = reach.traveltype;
		}  
	}  
	// FIXME: is it right to do this here?
	if(result->blocked) ms->reachability_time -= 10 * ms->thinktime;
	// copy the last origin
	VectorCopy(ms->origin, ms->lastorigin);
}

void BotResetMoveState(int movestate) {
	bot_movestate_t* ms;

	ms = BotMoveStateFromHandle(movestate);
	if(!ms) return;
	Com_Memset(ms, 0, sizeof(bot_movestate_t));
}

int BotSetupMoveAI(void) {
	sv_maxstep = 18;
	sv_maxbarrier = 32;
	sv_gravity = 800;
	return BLERR_NOERROR;
}

void BotShutdownMoveAI(void) {
	int i;

	for(i = 1; i <= MAX_CLIENTS; i++) {
		if(botmovestates[i]) {
			free(botmovestates[i]);
			botmovestates[i] = NULL;
		}  
	}  
}
