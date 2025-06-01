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

#ifndef _BG_PUBLIC_H
#define _BG_PUBLIC_H

// CS_SERVERINFO and CS_SYSTEMINFO are defined in q_shared.h
#define	CS_MUSIC				2
#define	CS_SCORES1				3
#define	CS_SCORES2				4
#define	CS_GAME_VERSION			5
#define	CS_LEVEL_START_TIME		6
#define	CS_INTERMISSION			7
#define CS_FLAGSTATUS			8
#define CS_SHADERSTATE			9
#define CS_BOTINFO				10 // 2 slots
#define	CS_ITEMS				12
#define	CS_MODELS				13
#define	CS_SOUNDS				(CS_MODELS+MAX_MODELS)
#define	CS_PLAYERS				(CS_SOUNDS+MAX_SOUNDS)
#define CS_LOCATIONS			(CS_PLAYERS+MAX_CLIENTS)
#define CS_MAX					(CS_PLAYERS+MAX_CLIENTS)

#if (CS_MAX) > MAX_CONFIGSTRINGS
#error overflow: (CS_MAX) > MAX_CONFIGSTRINGS
#endif

typedef enum {
	GT_SANDBOX,
	GT_MAPEDITOR,
	GT_SINGLE, //SHOULD BE 3 FOR ENGINE 
	GT_FFA,
	GT_TOURNAMENT,
	GT_TEAM,
	GT_CTF,
	GT_1FCTF,
	GT_OBELISK,
	GT_HARVESTER,	
	GT_ELIMINATION,
	GT_CTF_ELIMINATION,
	GT_LMS,
	GT_DOUBLE_D,
	GT_DOMINATION,
	GT_MAX_GAME_TYPE
} gametype_t;

// content masks
#define	MASK_ALL				(-1)
#define	MASK_SOLID				(CONTENTS_SOLID)
#define	MASK_PLAYERSOLID		(CONTENTS_SOLID|CONTENTS_PLAYERCLIP|CONTENTS_BODY)
#define	MASK_DEADSOLID			(CONTENTS_SOLID|CONTENTS_PLAYERCLIP)
#define	MASK_WATER				(CONTENTS_WATER|CONTENTS_LAVA|CONTENTS_SLIME)
#define	MASK_OPAQUE				(CONTENTS_SOLID|CONTENTS_SLIME|CONTENTS_LAVA)
#define	MASK_SHOT				(CONTENTS_SOLID|CONTENTS_BODY|CONTENTS_CORPSE)

//
// entityState_t->eType
//
typedef enum {
	ET_GENERAL,
	ET_PLAYER,
	ET_ITEM,
	ET_MISSILE,
	ET_MOVER,
	ET_BEAM,
	ET_PORTAL,
	ET_SPEAKER,
	ET_PUSH_TRIGGER,
	ET_TELEPORT_TRIGGER,
	ET_INVISIBLE,
	ET_GRAPPLE,
	ET_TEAM,
	ET_EVENTS
} entityType_t;

#endif // _BG_PUBLIC_H
