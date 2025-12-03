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
 * name:		be_aas_route.c
 *
 * desc:		AAS
 *
 * $Archive: /MissionPack/code/botlib/be_aas_route.c $
 *
 *****************************************************************************/

#include "../qcommon/q_shared.h"
#include "aasfile.h"
#include "botlib.h"
#include "be_aas.h"
#include "be_aas_funcs.h"
#include "be_interface.h"
#include "be_aas_def.h"

//travel time in hundredths of a second = distance * 100 / speed
#define DISTANCEFACTOR_CROUCH		1.3f		//crouch speed = 100
#define DISTANCEFACTOR_SWIM			1		//should be 0.66, swim speed = 150
#define DISTANCEFACTOR_WALK			0.33f	//walk speed = 300

static ID_INLINE int AAS_ClusterAreaNum(int cluster, int areanum) {
	int side, areacluster;

	areacluster = aasworld.areasettings[areanum].cluster;
	if (areacluster > 0) return aasworld.areasettings[areanum].clusterareanum;
	else
	{
		side = aasworld.portals[-areacluster].frontcluster != cluster;
		return aasworld.portals[-areacluster].clusterareanum[side];
	} //end else
}

static void AAS_InitTravelFlagFromType(void) {
	int i;

	for (i = 0; i < MAX_TRAVELTYPES; i++)
	{
		aasworld.travelflagfortype[i] = TFL_INVALID;
	} //end for
	aasworld.travelflagfortype[TRAVEL_INVALID] = TFL_INVALID;
	aasworld.travelflagfortype[TRAVEL_WALK] = TFL_WALK;
	aasworld.travelflagfortype[TRAVEL_CROUCH] = TFL_CROUCH;
	aasworld.travelflagfortype[TRAVEL_BARRIERJUMP] = TFL_BARRIERJUMP;
	aasworld.travelflagfortype[TRAVEL_JUMP] = TFL_JUMP;
	aasworld.travelflagfortype[TRAVEL_LADDER] = TFL_LADDER;
	aasworld.travelflagfortype[TRAVEL_WALKOFFLEDGE] = TFL_WALKOFFLEDGE;
	aasworld.travelflagfortype[TRAVEL_SWIM] = TFL_SWIM;
	aasworld.travelflagfortype[TRAVEL_WATERJUMP] = TFL_WATERJUMP;
	aasworld.travelflagfortype[TRAVEL_TELEPORT] = TFL_TELEPORT;
	aasworld.travelflagfortype[TRAVEL_ELEVATOR] = TFL_ELEVATOR;
	aasworld.travelflagfortype[TRAVEL_ROCKETJUMP] = TFL_ROCKETJUMP;
	aasworld.travelflagfortype[TRAVEL_BFGJUMP] = TFL_BFGJUMP;
	aasworld.travelflagfortype[TRAVEL_GRAPPLEHOOK] = TFL_GRAPPLEHOOK;
	aasworld.travelflagfortype[TRAVEL_DOUBLEJUMP] = TFL_DOUBLEJUMP;
	aasworld.travelflagfortype[TRAVEL_RAMPJUMP] = TFL_RAMPJUMP;
	aasworld.travelflagfortype[TRAVEL_STRAFEJUMP] = TFL_STRAFEJUMP;
	aasworld.travelflagfortype[TRAVEL_JUMPPAD] = TFL_JUMPPAD;
	aasworld.travelflagfortype[TRAVEL_FUNCBOB] = TFL_FUNCBOB;
}

static ID_INLINE int AAS_TravelFlagForType_inline(unsigned int traveltype) {
	int tfl;

	tfl = 0;
	if (traveltype & TRAVELFLAG_NOTTEAM1)
		tfl |= TFL_NOTTEAM1;
	if (traveltype & TRAVELFLAG_NOTTEAM2)
		tfl |= TFL_NOTTEAM2;
	traveltype &= TRAVELTYPE_MASK;
	if (traveltype >= MAX_TRAVELTYPES)
		return TFL_INVALID;
	tfl |= aasworld.travelflagfortype[traveltype];
	return tfl;
}

int AAS_TravelFlagForType(int traveltype) {
	return AAS_TravelFlagForType_inline(traveltype);
}

static ID_INLINE float AAS_RoutingTime(void) {
	return AAS_Time();
}

static int AAS_GetAreaContentsTravelFlags(int areanum) {
	int contents, tfl;

	contents = aasworld.areasettings[areanum].contents;
	tfl = 0;
	if (contents & AREACONTENTS_WATER)
		tfl |= TFL_WATER;
	else if (contents & AREACONTENTS_SLIME)
		tfl |= TFL_SLIME;
	else if (contents & AREACONTENTS_LAVA)
		tfl |= TFL_LAVA;
	else
		tfl |= TFL_AIR;
	if (contents & AREACONTENTS_DONOTENTER)
		tfl |= TFL_DONOTENTER;
	if (contents & AREACONTENTS_NOTTEAM1)
		tfl |= TFL_NOTTEAM1;
	if (contents & AREACONTENTS_NOTTEAM2)
		tfl |= TFL_NOTTEAM2;
	if (aasworld.areasettings[areanum].areaflags & AREA_BRIDGE)
		tfl |= TFL_BRIDGE;
	return tfl;
}

static ID_INLINE int AAS_AreaContentsTravelFlags_inline(int areanum) {
	return aasworld.areacontentstravelflags[areanum];
}

int AAS_AreaContentsTravelFlags(int areanum) {
	return aasworld.areacontentstravelflags[areanum];
}

static void AAS_InitAreaContentsTravelFlags(void) {
	int i;

	if (aasworld.areacontentstravelflags) free(aasworld.areacontentstravelflags);
	aasworld.areacontentstravelflags = (int *) malloc(aasworld.numareas * sizeof(int));
	//
	for (i = 0; i < aasworld.numareas; i++) {
		aasworld.areacontentstravelflags[i] = AAS_GetAreaContentsTravelFlags(i);
	}
}

static void AAS_CreateReversedReachability(void) {
	int i, n;
	aas_reversedlink_t *revlink;
	aas_reachability_t *reach;
	aas_areasettings_t *settings;
	char *ptr;
	
	//free reversed links that have already been created
	if (aasworld.reversedreachability) free(aasworld.reversedreachability);
	//allocate memory for the reversed reachability links
	ptr = (char *) malloc(aasworld.numareas * sizeof(aas_reversedreachability_t) +
							aasworld.reachabilitysize * sizeof(aas_reversedlink_t));
	//
	aasworld.reversedreachability = (aas_reversedreachability_t *) ptr;
	//pointer to the memory for the reversed links
	ptr += aasworld.numareas * sizeof(aas_reversedreachability_t);
	//check all reachabilities of all areas
	for (i = 1; i < aasworld.numareas; i++)
	{
		//settings of the area
		settings = &aasworld.areasettings[i];
		//
		if (settings->numreachableareas > 128)
			botimport.Print(PRT_WARNING, "area %d has more than 128 reachabilities\n", i);
		//create reversed links for the reachabilities
		for (n = 0; n < settings->numreachableareas && n < 128; n++)
		{
			//reachability link
			reach = &aasworld.reachability[settings->firstreachablearea + n];
			//
			revlink = (aas_reversedlink_t *) ptr;
			ptr += sizeof(aas_reversedlink_t);
			//
			revlink->areanum = i;
			revlink->linknum = settings->firstreachablearea + n;
			revlink->next = aasworld.reversedreachability[reach->areanum].first;
			aasworld.reversedreachability[reach->areanum].first = revlink;
			aasworld.reversedreachability[reach->areanum].numlinks++;
		} //end for
	} //end for
}

unsigned short int AAS_AreaTravelTime(int areanum, vec3_t start, vec3_t end) {
	int intdist;
	float dist;
	vec3_t dir;

	VectorSubtract(start, end, dir);
	dist = VectorLength(dir);
	//if crouch only area
	if (AAS_AreaCrouch(areanum)) dist *= DISTANCEFACTOR_CROUCH;
	//if swim area
	else if (AAS_AreaSwim(areanum)) dist *= DISTANCEFACTOR_SWIM;
	//normal walk area
	else dist *= DISTANCEFACTOR_WALK;
	//
	intdist = (int) dist;
	//make sure the distance isn't zero
	if (intdist <= 0) intdist = 1;
	return intdist;
} 

static void AAS_CalculateAreaTravelTimes(void) {
	int i, l, n, size;
	char *ptr;
	vec3_t end;
	const aas_reversedreachability_t *revreach;
	const aas_reversedlink_t *revlink;
	aas_reachability_t *reach;
	aas_areasettings_t *settings;
	
	//if there are still area travel times, free the memory
	if (aasworld.areatraveltimes) free(aasworld.areatraveltimes);
	//get the total size of all the area travel times
	size = aasworld.numareas * sizeof(unsigned short **);
	for (i = 0; i < aasworld.numareas; i++)
	{
		revreach = &aasworld.reversedreachability[i];
		//settings of the area
		settings = &aasworld.areasettings[i];
		//
		size += settings->numreachableareas * sizeof(unsigned short *);
		//
		size += settings->numreachableareas *
			PAD(revreach->numlinks, sizeof(long)) * sizeof(unsigned short);
	} //end for
	//allocate memory for the area travel times
	ptr = (char *) malloc(size);
	aasworld.areatraveltimes = (unsigned short ***) ptr;
	ptr += aasworld.numareas * sizeof(unsigned short **);
	//calcluate the travel times for all the areas
	for (i = 0; i < aasworld.numareas; i++)
	{
		//reversed reachabilities of this area
		revreach = &aasworld.reversedreachability[i];
		//settings of the area
		settings = &aasworld.areasettings[i];
		//
		aasworld.areatraveltimes[i] = (unsigned short **) ptr;
		ptr += settings->numreachableareas * sizeof(unsigned short *);
		//
		for (l = 0; l < settings->numreachableareas; l++)
		{
			aasworld.areatraveltimes[i][l] = (unsigned short *) ptr;
			ptr += PAD(revreach->numlinks, sizeof(long)) * sizeof(unsigned short);
			//reachability link
			reach = &aasworld.reachability[settings->firstreachablearea + l];
			//
			for (n = 0, revlink = revreach->first; revlink; revlink = revlink->next, n++)
			{
				VectorCopy(aasworld.reachability[revlink->linknum].end, end);
				//
				aasworld.areatraveltimes[i][l][n] = AAS_AreaTravelTime(i, end, reach->start);
			} //end for
		} //end for
	} //end for
}

static int AAS_PortalMaxTravelTime(int portalnum) {
	int l, n, t, maxt;
	aas_portal_t *portal;
	aas_reversedreachability_t *revreach;
	aas_reversedlink_t *revlink;
	aas_areasettings_t *settings;

	portal = &aasworld.portals[portalnum];
	//reversed reachabilities of this portal area
	revreach = &aasworld.reversedreachability[portal->areanum];
	//settings of the portal area
	settings = &aasworld.areasettings[portal->areanum];
	//
	maxt = 0;
	for (l = 0; l < settings->numreachableareas; l++)
	{
		for (n = 0, revlink = revreach->first; revlink; revlink = revlink->next, n++)
		{
			t = aasworld.areatraveltimes[portal->areanum][l][n];
			if (t > maxt)
			{
				maxt = t;
			} //end if
		} //end for
	} //end for
	return maxt;
}

static void AAS_InitPortalMaxTravelTimes(void) {
	int i;

	if (aasworld.portalmaxtraveltimes) free(aasworld.portalmaxtraveltimes);

	aasworld.portalmaxtraveltimes = (int *) malloc(aasworld.numportals * sizeof(int));

	for (i = 0; i < aasworld.numportals; i++) {
		aasworld.portalmaxtraveltimes[i] = AAS_PortalMaxTravelTime(i);
	} //end for
}

static void AAS_InitRoutingUpdate(void) {
	int i, maxreachabilityareas;

	//free routing update fields if already existing
	if (aasworld.areaupdate) free(aasworld.areaupdate);
	//
	maxreachabilityareas = 0;
	for (i = 0; i < aasworld.numclusters; i++)
	{
		if (aasworld.clusters[i].numreachabilityareas > maxreachabilityareas)
		{
			maxreachabilityareas = aasworld.clusters[i].numreachabilityareas;
		} //end if
	} //end for
	//allocate memory for the routing update fields
	aasworld.areaupdate = (aas_routingupdate_t *) malloc(
									maxreachabilityareas * sizeof(aas_routingupdate_t));
	//
	if (aasworld.portalupdate) free(aasworld.portalupdate);
	//allocate memory for the portal update fields
	aasworld.portalupdate = (aas_routingupdate_t *) malloc(
									(aasworld.numportals+1) * sizeof(aas_routingupdate_t));
}

#define MAX_REACHABILITYPASSAREAS		32
static void AAS_InitReachabilityAreas(void) {
	int i, j, numareas, areas[MAX_REACHABILITYPASSAREAS];
	int numreachareas;
	aas_reachability_t *reach;
	vec3_t start, end;

	if (aasworld.reachabilityareas)
		free(aasworld.reachabilityareas);
	if (aasworld.reachabilityareaindex)
		free(aasworld.reachabilityareaindex);

	aasworld.reachabilityareas = (aas_reachabilityareas_t *)
				malloc(aasworld.reachabilitysize * sizeof(aas_reachabilityareas_t));
	aasworld.reachabilityareaindex = (int *)
				malloc(aasworld.reachabilitysize * MAX_REACHABILITYPASSAREAS * sizeof(int));
	numreachareas = 0;
	for (i = 0; i < aasworld.reachabilitysize; i++)
	{
		reach = &aasworld.reachability[i];
		numareas = 0;
		switch(reach->traveltype & TRAVELTYPE_MASK)
		{
			//trace areas from start to end
			case TRAVEL_BARRIERJUMP:
			case TRAVEL_WATERJUMP:
				VectorCopy(reach->start, end);
				end[2] = reach->end[2];
				numareas = AAS_TraceAreas(reach->start, end, areas, NULL, MAX_REACHABILITYPASSAREAS);
				break;
			case TRAVEL_WALKOFFLEDGE:
				VectorCopy(reach->end, start);
				start[2] = reach->start[2];
				numareas = AAS_TraceAreas(start, reach->end, areas, NULL, MAX_REACHABILITYPASSAREAS);
				break;
			case TRAVEL_GRAPPLEHOOK:
				numareas = AAS_TraceAreas(reach->start, reach->end, areas, NULL, MAX_REACHABILITYPASSAREAS);
				break;

			//trace arch
			case TRAVEL_JUMP: break;
			case TRAVEL_ROCKETJUMP: break;
			case TRAVEL_BFGJUMP: break;
			case TRAVEL_JUMPPAD: break;

			//trace from reach->start to entity center, along entity movement
			//and from entity center to reach->end
			case TRAVEL_ELEVATOR: break;
			case TRAVEL_FUNCBOB: break;

			//no areas in between
			case TRAVEL_WALK: break;
			case TRAVEL_CROUCH: break;
			case TRAVEL_LADDER: break;
			case TRAVEL_SWIM: break;
			case TRAVEL_TELEPORT: break;
			default: break;
		} //end switch
		aasworld.reachabilityareas[i].firstarea = numreachareas;
		aasworld.reachabilityareas[i].numareas = numareas;
		for (j = 0; j < numareas; j++)
		{
			aasworld.reachabilityareaindex[numreachareas++] = areas[j];
		} //end for
	} //end for
}

void AAS_InitRouting(void) {
	AAS_InitTravelFlagFromType();
	//
	AAS_InitAreaContentsTravelFlags();
	//initialize the routing update fields
	AAS_InitRoutingUpdate();
	//create reversed reachability links used by the routing update algorithm
	AAS_CreateReversedReachability();
	//initialize the area travel times
	AAS_CalculateAreaTravelTimes();
	//calculate the maximum travel times through portals
	AAS_InitPortalMaxTravelTimes();
	//get the areas reachabilities go through
	AAS_InitReachabilityAreas();
}

static int AAS_AreaRouteToGoalArea(int areanum, vec3_t origin, int goalareanum, int travelflags, int *traveltime, int *reachnum) {
	int clusternum, goalclusternum, portalnum, i, clusterareanum, bestreachnum;
	unsigned short int t, besttime;
	aas_portal_t *portal;
	aas_cluster_t *cluster;
	aas_reachability_t *reach;

	if (!aasworld.initialized) return qfalse;

	if (areanum == goalareanum)
	{
		*traveltime = 1;
		*reachnum = 0;
		return qtrue;
	}
	//check !AAS_AreaReachability(areanum) with custom developer-only debug message
	if (areanum <= 0 || areanum >= aasworld.numareas)
	{
		if (botDeveloper)
		{
			botimport.Print(PRT_ERROR, "AAS_AreaTravelTimeToGoalArea: areanum %d out of range\n", areanum);
		} //end if
		return qfalse;
	} //end if
	if (goalareanum <= 0 || goalareanum >= aasworld.numareas)
	{
		if (botDeveloper)
		{
			botimport.Print(PRT_ERROR, "AAS_AreaTravelTimeToGoalArea: goalareanum %d out of range\n", goalareanum);
		} //end if
		return qfalse;
	} //end if
	if (!aasworld.areasettings[areanum].numreachableareas || !aasworld.areasettings[goalareanum].numreachableareas)
	{
		return qfalse;
	} //end if

	//
	if (AAS_AreaDoNotEnter(areanum) || AAS_AreaDoNotEnter(goalareanum))
	{
		travelflags |= TFL_DONOTENTER;
	} //end if
	clusternum = aasworld.areasettings[areanum].cluster;
	goalclusternum = aasworld.areasettings[goalareanum].cluster;
	//check if the area is a portal of the goal area cluster
	if (clusternum < 0 && goalclusternum > 0)
	{
		portal = &aasworld.portals[-clusternum];
		if (portal->frontcluster == goalclusternum ||
				portal->backcluster == goalclusternum)
		{
			clusternum = goalclusternum;
		} //end if
	} //end if
	//check if the goalarea is a portal of the area cluster
	else if (clusternum > 0 && goalclusternum < 0)
	{
		portal = &aasworld.portals[-goalclusternum];
		if (portal->frontcluster == clusternum ||
				portal->backcluster == clusternum)
		{
			goalclusternum = clusternum;
		} //end if
	} //end if
	//if both areas are in the same cluster
	//NOTE: there might be a shorter route via another cluster!!! but we don't care
	if (clusternum > 0 && goalclusternum > 0 && clusternum == goalclusternum) {
		//the number of the area in the cluster
		clusterareanum = AAS_ClusterAreaNum(clusternum, areanum);
		//the cluster the area is in
		cluster = &aasworld.clusters[clusternum];
		//if the area is NOT a reachability area
		if (clusterareanum >= cluster->numreachabilityareas) return 0;
	} //end if
	//
	clusternum = aasworld.areasettings[areanum].cluster;
	goalclusternum = aasworld.areasettings[goalareanum].cluster;
	//if the goal area is a portal
	if (goalclusternum < 0)
	{
		//just assume the goal area is part of the front cluster
		portal = &aasworld.portals[-goalclusternum];
		goalclusternum = portal->frontcluster;
	} //end if

	besttime = 0;
	bestreachnum = -1;
	//the cluster the area is in
	cluster = &aasworld.clusters[clusternum];
	//find the portal of the area cluster leading towards the goal area
	for (i = 0; i < cluster->numportals; i++)
	{
		portalnum = aasworld.portalindex[cluster->firstportal + i];
		portal = &aasworld.portals[portalnum];
		//current area inside the current cluster
		clusterareanum = AAS_ClusterAreaNum(clusternum, areanum);
		//if the area is NOT a reachability area
		if (clusterareanum >= cluster->numreachabilityareas) continue;

		t += aasworld.portalmaxtraveltimes[portalnum];
		//
		if (origin)
		{
			*reachnum = aasworld.areasettings[areanum].firstreachablearea;
			reach = aasworld.reachability + *reachnum;
			t += AAS_AreaTravelTime(areanum, origin, reach->start);
		} //end if
		//if the time is better than the one already found
		if (!besttime || t < besttime)
		{
			bestreachnum = *reachnum;
			besttime = t;
		} //end if
	} //end for
	if (bestreachnum < 0) {
		return qfalse;
	}
	*reachnum = bestreachnum;
	*traveltime = besttime;
	return qtrue;
}

int AAS_AreaTravelTimeToGoalArea(int areanum, vec3_t origin, int goalareanum, int travelflags) {
	int traveltime, reachnum = 0;

	if (AAS_AreaRouteToGoalArea(areanum, origin, goalareanum, travelflags, &traveltime, &reachnum))
	{
		return traveltime;
	}
	return 0;
}

void AAS_ReachabilityFromNum(int num, struct aas_reachability_s *reach) {
	if (!aasworld.initialized) {
		Com_Memset(reach, 0, sizeof(aas_reachability_t));
		return;
	} //end if
	if (num < 0 || num >= aasworld.reachabilitysize) {
		Com_Memset(reach, 0, sizeof(aas_reachability_t));
		return;
	} //end if
	Com_Memcpy(reach, &aasworld.reachability[num], sizeof(aas_reachability_t));;
}

int AAS_NextAreaReachability(int areanum, int reachnum) {
	aas_areasettings_t *settings;

	if (!aasworld.initialized) return 0;

	if (areanum <= 0 || areanum >= aasworld.numareas)
	{
		botimport.Print(PRT_ERROR, "AAS_NextAreaReachability: areanum %d out of range\n", areanum);
		return 0;
	} //end if

	settings = &aasworld.areasettings[areanum];
	if (!reachnum)
	{
		return settings->firstreachablearea;
	} //end if
	if (reachnum < settings->firstreachablearea)
	{
		botimport.Print(PRT_FATAL, "AAS_NextAreaReachability: reachnum < settings->firstreachableara");
		return 0;
	} //end if
	reachnum++;
	if (reachnum >= settings->firstreachablearea + settings->numreachableareas)
	{
		return 0;
	} //end if
	return reachnum;
}
