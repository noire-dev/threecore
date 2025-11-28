// Copyright (C) 1999-2005 ID Software, Inc.
// Copyright (C) 2023-2025 Noire.dev
// SourceTech — GPLv2; see LICENSE for details.

typedef struct bot_goal_s {
	vec3_t origin;      // origin of the goal
	int areanum;        // area number of the goal
	vec3_t mins, maxs;  // mins and maxs of the goal
} bot_goal_t;

int BotTouchingGoal(const vec3_t origin, const bot_goal_t* goal);
