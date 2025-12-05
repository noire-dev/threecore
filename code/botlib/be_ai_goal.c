/*
===========================================================================
Copyright (C) 1999-2005 Id Software, Inc.

This file is part of Quake III Arena source code.

Quake III Arena source code is free software; you can redistribute it
and/or modify it under the terms of the GNU General Public License as
published by the Free Software Foundation; either version 2 of the License,
or (at your option) any later version.

Quake III Arena source code is distributed in the hope that it will be
useful, but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with Quake III Arena source code; if not, write to the Free Software
Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
===========================================================================
*/

/*****************************************************************************
 * name:		be_ai_goal.c
 *
 * desc:		goal AI
 *
 * $Archive: /MissionPack/code/botlib/be_ai_goal.c $
 *
 *****************************************************************************/

#include "../qcommon/q_shared.h"
#include "l_utils.h"
#include "l_libvar.h"
#include "l_memory.h"
#include "l_log.h"
#include "l_script.h"
#include "l_precomp.h"
#include "l_struct.h"
#include "aasfile.h"
#include "botlib.h"
#include "be_aas.h"
#include "be_aas_funcs.h"
#include "be_interface.h"
#include "be_ai_goal.h"
#include "be_ai_move.h"

int BotTouchingGoal(const vec3_t origin, const bot_goal_t *goal) {
	int i;
	vec3_t boxmins, boxmaxs;
	vec3_t absmins, absmaxs;
	const vec3_t safety_maxs = {0, 0, 0}; //{4, 4, 10};
	const vec3_t safety_mins = {0, 0, 0}; //{-4, -4, 0};

	AAS_PresenceTypeBoundingBox(PRESENCE_NORMAL, boxmins, boxmaxs);
	VectorSubtract(goal->mins, boxmaxs, absmins);
	VectorSubtract(goal->maxs, boxmins, absmaxs);
	VectorAdd(absmins, goal->origin, absmins);
	VectorAdd(absmaxs, goal->origin, absmaxs);
	//make the box a little smaller for safety
	VectorSubtract(absmaxs, safety_maxs, absmaxs);
	VectorSubtract(absmins, safety_mins, absmins);

	for (i = 0; i < 3; i++) {
		if (origin[i] < absmins[i] || origin[i] > absmaxs[i]) return qfalse;
	}
	return qtrue;
}
