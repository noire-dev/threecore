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
	int t, besttime, bestreachnum, reachnum;
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

static bot_moveresult_t BotTravel_Walk(bot_movestate_t* ms, aas_reachability_t* reach) {
	float dist, speed;
	vec3_t hordir;
	bot_moveresult_t_cleared(result);

	// first walk straight to the reachability start
	hordir[0] = reach->start[0] - ms->origin[0];
	hordir[1] = reach->start[1] - ms->origin[1];
	hordir[2] = 0;
	dist = VectorNormalize(hordir);
	
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

static bot_moveresult_t BotTravel_BarrierJump(bot_movestate_t* ms, aas_reachability_t* reach) {
	float dist, speed;
	vec3_t hordir;
	bot_moveresult_t_cleared(result);

	// walk straight to reachability start
	hordir[0] = reach->start[0] - ms->origin[0];
	hordir[1] = reach->start[1] - ms->origin[1];
	hordir[2] = 0;
	dist = VectorNormalize(hordir);

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

	// elementary actions
	EA_Move(ms->client, dir, 400);
	//
	VectorCopy(dir, result.movedir);
	vectoangles(dir, result.ideal_viewangles);
	result.flags |= MOVERESULT_SWIMVIEW;
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

void BotMoveToGoal(int movestate, bot_goal_t* goal, int travelflags) {
	int reachnum, lastreachnum, resultflags;
	aas_reachability_t reach, lastreach;
	bot_movestate_t* ms;

	ms = BotMoveStateFromHandle(movestate);
	if(!ms) return;
	if(!goal) return;
	
	// remove some of the move flags
	ms->moveflags &= ~(MFL_SWIMMING | MFL_AGAINSTLADDER);
	
	if(AAS_Swimming(ms->origin)) ms->moveflags |= MFL_SWIMMING;
	if(AAS_AgainstLadder(ms->origin)) ms->moveflags |= MFL_AGAINSTLADDER;
	if(ms->moveflags & (MFL_ONGROUND | MFL_SWIMMING | MFL_AGAINSTLADDER)) {
		AAS_ReachabilityFromNum(ms->lastreachnum, &lastreach);
		// reachability area the bot is in
		ms->areanum = BotFuzzyPointReachabilityArea(ms->origin);
		if(!ms->areanum) return;
		// if the bot is in the goal area
		if(ms->areanum == goal->areanum) {
			BotMoveInGoalArea(ms, goal);
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
				if(ms->lastgoalareanum != goal->areanum || ms->reachability_time < AAS_Time() || ms->lastareanum != ms->areanum) reachnum = 0;
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
			}  
		}
		
		ms->lastreachnum = reachnum;
		ms->lastgoalareanum = goal->areanum;
		ms->lastareanum = ms->areanum;
		// if the bot has a reachability
		if(reachnum) {
			// get the reachability from the number
			AAS_ReachabilityFromNum(reachnum, &reach);
			switch(reach.traveltype & TRAVELTYPE_MASK) {
				case TRAVEL_WALK: BotTravel_Walk(ms, &reach); break;
				case TRAVEL_BARRIERJUMP: BotTravel_BarrierJump(ms, &reach); break;
				case TRAVEL_SWIM: BotTravel_Swim(ms, &reach); break;
				case TRAVEL_TELEPORT: BotTravel_Teleport(ms, &reach); break;
				case TRAVEL_JUMPPAD: BotTravel_JumpPad(ms, &reach); break;
				default: break;
			}  
		} else {
			Com_Memset(&reach, 0, sizeof(aas_reachability_t));
		}  
	} else {
		int i, numareas, areas[16];
		vec3_t end;

		// special handling of jump pads when the bot uses a jump pad without knowing it
		VectorMA(ms->origin, -2 * ms->thinktime, ms->velocity, end);
		numareas = AAS_TraceAreas(ms->origin, end, areas, NULL, 16);
		for(i = numareas - 1; i >= 0; i--) {
			if(AAS_AreaJumpPad(areas[i])) {
				lastreachnum = BotGetReachabilityToGoal(end, areas[i], ms->lastgoalareanum, ms->lastareanum, goal, TFL_JUMPPAD, NULL);
				if(lastreachnum) {
					ms->lastreachnum = lastreachnum;
					ms->lastareanum = areas[i];
					break;
				} else {
					for(lastreachnum = AAS_NextAreaReachability(areas[i], 0); lastreachnum; lastreachnum = AAS_NextAreaReachability(areas[i], lastreachnum)) {
						// get the reachability from the number
						AAS_ReachabilityFromNum(lastreachnum, &reach);
						if((reach.traveltype & TRAVELTYPE_MASK) == TRAVEL_JUMPPAD) {
							ms->lastreachnum = lastreachnum;
							ms->lastareanum = areas[i];
						}  
					}  
					if(lastreachnum) break;
				}  
			}  
		}  
		if(ms->lastreachnum) {
			AAS_ReachabilityFromNum(ms->lastreachnum, &reach);
			switch(reach.traveltype & TRAVELTYPE_MASK) {
				case TRAVEL_WALK: BotTravel_Walk(ms, &reach); break;
				case TRAVEL_BARRIERJUMP: BotFinishTravel_BarrierJump(ms, &reach); break;
				case TRAVEL_SWIM: BotTravel_Swim(ms, &reach); break;
				case TRAVEL_TELEPORT: break;
				case TRAVEL_JUMPPAD: BotFinishTravel_JumpPad(ms, &reach); break;
				default: break;
			}  
		}  
	}
	
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
