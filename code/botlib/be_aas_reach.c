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
 * name:		be_aas_reach.c
 *
 * desc:		reachability calculations
 *
 * $Archive: /MissionPack/code/botlib/be_aas_reach.c $
 *
 *****************************************************************************/

#include "../qcommon/q_shared.h"
#include "aasfile.h"
#include "botlib.h"
#include "be_aas.h"
#include "be_aas_funcs.h"
#include "be_aas_def.h"

extern botlib_import_t botimport;

int AAS_AreaReachability(int areanum) {
	if (areanum < 0 || areanum >= aasworld.numareas) {
		AAS_Error("AAS_AreaReachability: areanum %d out of range\n", areanum);
		return 0;
	}
	return aasworld.areasettings[areanum].numreachableareas;
}

int AAS_AreaCrouch(int areanum) {
	if (!(aasworld.areasettings[areanum].presencetype & PRESENCE_NORMAL)) return qtrue;
	else return qfalse;
}

int AAS_AreaSwim(int areanum) {
	if (aasworld.areasettings[areanum].areaflags & AREA_LIQUID) return qtrue;
	else return qfalse;
}

int AAS_AreaJumpPad(int areanum) {
	return (aasworld.areasettings[areanum].contents & AREACONTENTS_JUMPPAD);
}

int AAS_AreaDoNotEnter(int areanum) {
	return (aasworld.areasettings[areanum].contents & AREACONTENTS_DONOTENTER);
}
