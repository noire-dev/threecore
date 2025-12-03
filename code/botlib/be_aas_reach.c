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

extern int Sys_MilliSeconds(void);


extern botlib_import_t botimport;

//#define REACH_DEBUG

//NOTE: all travel times are in hundredths of a second
//maximum number of reachability links
#define AAS_MAX_REACHABILITYSIZE			65536*8
//number of areas reachability is calculated for each frame
#define REACHABILITYAREASPERCYCLE			15
//number of units reachability points are placed inside the areas
#define INSIDEUNITS							2
#define INSIDEUNITS_WALKEND					5
#define INSIDEUNITS_WALKSTART				0.1
#define INSIDEUNITS_WATERJUMP				15
//area flag used for weapon jumping
#define AREA_WEAPONJUMP						8192	//valid area to weapon jump to
//number of reachabilities of each type
static int reach_swim;			//swim
static int reach_equalfloor;	//walk on floors with equal height
static int reach_step;			//step up
static int reach_walk;			//walk of step
static int reach_barrier;		//jump up to a barrier
static int reach_waterjump;		//jump out of water
static int reach_walkoffledge;	//walk of a ledge
static int reach_jump;			//jump
static int reach_ladder;		//climb or descent a ladder
static int reach_teleport;		//teleport
static int reach_elevator;		//use an elevator
static int reach_funcbob;		//use a func bob
static int reach_rocketjump;	//rocket jump
//linked reachability
typedef struct aas_lreachability_s
{
	int areanum;					//number of the reachable area
	int facenum;					//number of the face towards the other area
	int edgenum;					//number of the edge towards the other area
	vec3_t start;					//start point of inter area movement
	vec3_t end;						//end point of inter area movement
	int traveltype;					//type of travel required to get to the area
	unsigned short int traveltime;	//travel time of the inter area movement
	//
	struct aas_lreachability_s *next;
} aas_lreachability_t;
//temporary reachabilities
static aas_lreachability_t *reachabilityheap;	//heap with reachabilities
static aas_lreachability_t *nextreachability;	//next free reachability from the heap
static aas_lreachability_t **areareachability;	//reachability links for every area
static int numlreachabilities;

//===========================================================================
// returns the surface area of the given face
//
// Parameter:				-
// Returns:					-
// Changes Globals:		-
//===========================================================================
static float AAS_FaceArea(aas_face_t *face)
{
	int i, edgenum, side;
	float total;
	vec_t *v;
	vec3_t d1, d2, cross;
	aas_edge_t *edge;

	edgenum = aasworld.edgeindex[face->firstedge];
	side = edgenum < 0;
	edge = &aasworld.edges[abs(edgenum)];
	v = aasworld.vertexes[edge->v[side]];

	total = 0;
	for (i = 1; i < face->numedges - 1; i++)
	{
		edgenum = aasworld.edgeindex[face->firstedge + i];
		side = edgenum < 0;
		edge = &aasworld.edges[abs(edgenum)];
		VectorSubtract(aasworld.vertexes[edge->v[side]], v, d1);
		VectorSubtract(aasworld.vertexes[edge->v[!side]], v, d2);
		CrossProduct(d1, d2, cross);
		total += 0.5 * VectorLength(cross);
	} //end for
	return total;
} //end of the function AAS_FaceArea
//===========================================================================
// returns the volume of an area
//
// Parameter:				-
// Returns:					-
// Changes Globals:		-
//===========================================================================
static float AAS_AreaVolume(int areanum)
{
	int i, edgenum, facenum, side;
	vec_t d, a, volume;
	vec3_t corner;
	aas_plane_t *plane;
	aas_edge_t *edge;
	aas_face_t *face;
	aas_area_t *area;

	area = &aasworld.areas[areanum];
	facenum = aasworld.faceindex[area->firstface];
	face = &aasworld.faces[abs(facenum)];
	edgenum = aasworld.edgeindex[face->firstedge];
	edge = &aasworld.edges[abs(edgenum)];
	//
	VectorCopy(aasworld.vertexes[edge->v[0]], corner);

	//make tetrahedrons to all other faces
	volume = 0;
	for (i = 0; i < area->numfaces; i++)
	{
		facenum = abs(aasworld.faceindex[area->firstface + i]);
		face = &aasworld.faces[facenum];
		side = face->backarea != areanum;
		plane = &aasworld.planes[face->planenum ^ side];
		d = -(DotProduct (corner, plane->normal) - plane->dist);
		a = AAS_FaceArea(face);
		volume += d * a;
	} //end for

	volume /= 3;
	return volume;
} //end of the function AAS_AreaVolume
//===========================================================================
//
// Parameter:				-
// Returns:					-
// Changes Globals:		-
//===========================================================================
static int AAS_BestReachableLinkArea(aas_link_t *areas)
{
	aas_link_t *link;

	for (link = areas; link; link = link->next_area)
	{
		if (AAS_AreaGrounded(link->areanum) || AAS_AreaSwim(link->areanum))
		{
			return link->areanum;
		} //end if
	} //end for
	//
	for (link = areas; link; link = link->next_area)
	{
		if (link->areanum) return link->areanum;
		//FIXME: this is a bad idea when the reachability is not yet
		// calculated when the level items are loaded
		if (AAS_AreaReachability(link->areanum))
			return link->areanum;
	} //end for
	return 0;
} //end of the function AAS_BestReachableLinkArea
//===========================================================================
//
// Parameter:				-
// Returns:					-
// Changes Globals:		-
//===========================================================================
int AAS_BestReachableArea(vec3_t origin, vec3_t mins, vec3_t maxs, vec3_t goalorigin)
{
	int areanum, i, j, k, l;
	aas_link_t *areas;
	vec3_t absmins, absmaxs;
	//vec3_t bbmins, bbmaxs;
	vec3_t start, end;
	aas_trace_t trace;

	if (!aasworld.loaded)
	{
		botimport.Print(PRT_ERROR, "AAS_BestReachableArea: aas not loaded\n");
		return 0;
	} //end if
	//find a point in an area
	VectorCopy(origin, start);
	areanum = AAS_PointAreaNum(start);
	//while no area found fudge around a little
	for (i = 0; i < 5 && !areanum; i++) {
		for (j = 0; j < 5 && !areanum; j++) {
			for (k = -1; k <= 1 && !areanum; k++) {
				for (l = -1; l <= 1 && !areanum; l++) {
					VectorCopy(origin, start);
					start[0] += (float) j * 4 * k;
					start[1] += (float) j * 4 * l;
					start[2] += (float) i * 4;
					areanum = AAS_PointAreaNum(start);
				}
			}
		}
	}
	//if an area was found
	if (areanum) {
		//drop client bbox down and try again
		VectorCopy(start, end);
		start[2] += 0.25;
		end[2] -= 50;
		trace = AAS_TraceClientBBox(start, end, PRESENCE_CROUCH, -1);
		if (!trace.startsolid)
		{
			areanum = AAS_PointAreaNum(trace.endpos);
			VectorCopy(trace.endpos, goalorigin);
			//FIXME: cannot enable next line right now because the reachability
			// does not have to be calculated when the level items are loaded
			//if the origin is in an area with reachability
			//if (AAS_AreaReachability(areanum)) return areanum;
			if (areanum) return areanum;
		} else {
			VectorCopy(start, goalorigin);
			return areanum;
		} //end else
	} //end if
	//
	// NOTE: the goal origin does not have to be in the goal area
	// because the bot will have to move towards the item origin anyway
	VectorCopy(origin, goalorigin);
	//
	VectorAdd(origin, mins, absmins);
	VectorAdd(origin, maxs, absmaxs);
	//link an invalid (-1) entity
	areas = AAS_LinkEntityClientBBox(absmins, absmaxs, -1, PRESENCE_CROUCH);
	//get the reachable link area
	areanum = AAS_BestReachableLinkArea(areas);
	//unlink the invalid entity
	AAS_UnlinkFromAreas(areas);
	//
	return areanum;
} //end of the function AAS_BestReachableArea
//===========================================================================
//
// Parameter:				-
// Returns:					-
// Changes Globals:		-
//===========================================================================
static void AAS_SetupReachabilityHeap(void)
{
	int i;

	reachabilityheap = (aas_lreachability_t *) malloc(
						AAS_MAX_REACHABILITYSIZE * sizeof(aas_lreachability_t));
	for (i = 0; i < AAS_MAX_REACHABILITYSIZE-1; i++)
	{
		reachabilityheap[i].next = &reachabilityheap[i+1];
	} //end for
	reachabilityheap[AAS_MAX_REACHABILITYSIZE-1].next = NULL;
	nextreachability = reachabilityheap;
	numlreachabilities = 0;
} //end of the function AAS_InitReachabilityHeap
//===========================================================================
// returns a reachability link
//
// Parameter:				-
// Returns:					-
// Changes Globals:		-
//===========================================================================
static aas_lreachability_t *AAS_AllocReachability(void)
{
	aas_lreachability_t *r;

	if (!nextreachability) return NULL;
	//make sure the error message only shows up once
	if (!nextreachability->next) AAS_Error("AAS_MAX_REACHABILITYSIZE\n");
	//
	r = nextreachability;
	nextreachability = nextreachability->next;
	numlreachabilities++;
	return r;
} //end of the function AAS_AllocReachability
//===========================================================================
// frees a reachability link
//
// Parameter:				-
// Returns:					-
// Changes Globals:		-
//===========================================================================
static void AAS_FreeReachability(aas_lreachability_t *lreach)
{
	Com_Memset(lreach, 0, sizeof(aas_lreachability_t));

	lreach->next = nextreachability;
	nextreachability = lreach;
	numlreachabilities--;
} //end of the function AAS_FreeReachability
//===========================================================================
// returns qtrue if the area has reachability links
//
// Parameter:				-
// Returns:					-
// Changes Globals:		-
//===========================================================================
int AAS_AreaReachability(int areanum)
{
	if (areanum < 0 || areanum >= aasworld.numareas)
	{
		AAS_Error("AAS_AreaReachability: areanum %d out of range\n", areanum);
		return 0;
	} //end if
	return aasworld.areasettings[areanum].numreachableareas;
} //end of the function AAS_AreaReachability
//===========================================================================
// returns the surface area of all ground faces together of the area
//
// Parameter:				-
// Returns:					-
// Changes Globals:		-
//===========================================================================
float AAS_AreaGroundFaceArea(int areanum)
{
	int i;
	float total;
	aas_area_t *area;
	aas_face_t *face;

	total = 0;
	area = &aasworld.areas[areanum];
	for (i = 0; i < area->numfaces; i++)
	{
		face = &aasworld.faces[abs(aasworld.faceindex[area->firstface + i])];
		if (!(face->faceflags & FACE_GROUND)) continue;
		//
		total += AAS_FaceArea(face);
	} //end for
	return total;
} //end of the function AAS_AreaGroundFaceArea
//===========================================================================
// returns the center of a face
//
// Parameter:				-
// Returns:					-
// Changes Globals:		-
//===========================================================================
static void AAS_FaceCenter(int facenum, vec3_t center)
{
	int i;
	float scale;
	aas_face_t *face;
	aas_edge_t *edge;

	face = &aasworld.faces[facenum];

	VectorClear(center);
	for (i = 0; i < face->numedges; i++)
	{
		edge = &aasworld.edges[abs(aasworld.edgeindex[face->firstedge + i])];
		VectorAdd(center, aasworld.vertexes[edge->v[0]], center);
		VectorAdd(center, aasworld.vertexes[edge->v[1]], center);
	} //end for
	scale = 0.5 / face->numedges;
	VectorScale(center, scale, center);
} //end of the function AAS_FaceCenter
//===========================================================================
// distance = 0.5 * gravity * t * t
// vel = t * gravity
// damage = vel * vel * 0.0001
//
// Parameter:			-
// Returns:				-
// Changes Globals:		-
//===========================================================================
static float AAS_FallDelta(float distance)
{
	float t, delta, gravity;

	gravity = aassettings.phys_gravity;
	t = sqrt(fabs(distance) * 2 / gravity);
	delta = t * gravity;
	return delta * delta * 0.0001;
} //end of the function AAS_FallDelta
//===========================================================================
//
// Parameter:			-
// Returns:				-
// Changes Globals:		-
//===========================================================================
static float AAS_MaxJumpHeight(float phys_jumpvel)
{
	float phys_gravity;

	phys_gravity = aassettings.phys_gravity;
	//maximum height a player can jump with the given initial z velocity
	return 0.5 * phys_gravity * (phys_jumpvel / phys_gravity) * (phys_jumpvel / phys_gravity);
} //end of the function MaxJumpHeight
//===========================================================================
// returns true if a player can only crouch in the area
//
// Parameter:				-
// Returns:					-
// Changes Globals:		-
//===========================================================================
static float AAS_MaxJumpDistance(float phys_jumpvel)
{
	float phys_gravity, phys_maxvelocity, t;

	phys_gravity = aassettings.phys_gravity;
	phys_maxvelocity = aassettings.phys_maxvelocity;
	//time a player takes to fall the height
	t = sqrt(aassettings.rs_maxjumpfallheight / (0.5 * phys_gravity));
   //maximum distance
	return phys_maxvelocity * (t + phys_jumpvel / phys_gravity);
} //end of the function AAS_MaxJumpDistance
//===========================================================================
// returns true if a player can only crouch in the area
//
// Parameter:				-
// Returns:					-
// Changes Globals:		-
//===========================================================================
int AAS_AreaCrouch(int areanum)
{
	if (!(aasworld.areasettings[areanum].presencetype & PRESENCE_NORMAL)) return qtrue;
	else return qfalse;
} //end of the function AAS_AreaCrouch
//===========================================================================
// returns qtrue if it is possible to swim in the area
//
// Parameter:				-
// Returns:					-
// Changes Globals:		-
//===========================================================================
int AAS_AreaSwim(int areanum)
{
	if (aasworld.areasettings[areanum].areaflags & AREA_LIQUID) return qtrue;
	else return qfalse;
} //end of the function AAS_AreaSwim
//===========================================================================
// returns qtrue if the area contains a liquid
//
// Parameter:				-
// Returns:					-
// Changes Globals:		-
//===========================================================================
int AAS_AreaLiquid(int areanum)
{
	if (aasworld.areasettings[areanum].areaflags & AREA_LIQUID) return qtrue;
	else return qfalse;
} //end of the function AAS_AreaLiquid
//===========================================================================
//
// Parameter:			-
// Returns:				-
// Changes Globals:		-
//===========================================================================
int AAS_AreaLava(int areanum)
{
	return (aasworld.areasettings[areanum].contents & AREACONTENTS_LAVA);
} //end of the function AAS_AreaLava
//===========================================================================
//
// Parameter:			-
// Returns:				-
// Changes Globals:		-
//===========================================================================
int AAS_AreaSlime(int areanum)
{
	return (aasworld.areasettings[areanum].contents & AREACONTENTS_SLIME);
} //end of the function AAS_AreaSlime
//===========================================================================
// returns qtrue if the area contains ground faces
//
// Parameter:				-
// Returns:					-
// Changes Globals:		-
//===========================================================================
int AAS_AreaGrounded(int areanum)
{
	return (aasworld.areasettings[areanum].areaflags & AREA_GROUNDED);
} //end of the function AAS_AreaGround
//===========================================================================
// returns true if the area contains ladder faces
//
// Parameter:				-
// Returns:					-
// Changes Globals:		-
//===========================================================================
static int AAS_AreaLadder(int areanum)
{
	return (aasworld.areasettings[areanum].areaflags & AREA_LADDER);
} //end of the function AAS_AreaLadder
//===========================================================================
//
// Parameter:				-
// Returns:					-
// Changes Globals:		-
//===========================================================================
int AAS_AreaJumpPad(int areanum)
{
	return (aasworld.areasettings[areanum].contents & AREACONTENTS_JUMPPAD);
} //end of the function AAS_AreaJumpPad
//===========================================================================
//
// Parameter:				-
// Returns:					-
// Changes Globals:		-
//===========================================================================
static int AAS_AreaTeleporter(int areanum)
{
	return (aasworld.areasettings[areanum].contents & AREACONTENTS_TELEPORTER);
} //end of the function AAS_AreaTeleporter
//===========================================================================
//
// Parameter:				-
// Returns:					-
// Changes Globals:		-
//===========================================================================
static int AAS_AreaClusterPortal(int areanum)
{
	return (aasworld.areasettings[areanum].contents & AREACONTENTS_CLUSTERPORTAL);
} //end of the function AAS_AreaClusterPortal
//===========================================================================
//
// Parameter:				-
// Returns:					-
// Changes Globals:		-
//===========================================================================
int AAS_AreaDoNotEnter(int areanum)
{
	return (aasworld.areasettings[areanum].contents & AREACONTENTS_DONOTENTER);
} //end of the function AAS_AreaDoNotEnter
//===========================================================================
// returns true if there already exists a reachability from area1 to area2
//
// Parameter:				-
// Returns:					-
// Changes Globals:		-
//===========================================================================
static qboolean AAS_ReachabilityExists(int area1num, int area2num)
{
	aas_lreachability_t *r;

	for (r = areareachability[area1num]; r; r = r->next)
	{
		if (r->areanum == area2num) return qtrue;
	} //end for
	return qfalse;
} //end of the function AAS_ReachabilityExists
//===========================================================================
// returns the distance between the two vectors
//
// Parameter:				-
// Returns:					-
// Changes Globals:		-
//===========================================================================
static float VectorDistance( const vec3_t v1, const vec3_t v2)
{
	vec3_t dir;

	VectorSubtract(v2, v1, dir);
	return VectorLength(dir);
} //end of the function VectorDistance
//===========================================================================
// returns true if the first vector is between the last two vectors
//
// Parameter:				-
// Returns:					-
// Changes Globals:		-
//===========================================================================
static int VectorBetweenVectors( const vec3_t v, const vec3_t v1, const vec3_t v2)
{
	vec3_t dir1, dir2;

	VectorSubtract(v, v1, dir1);
	VectorSubtract(v, v2, dir2);
	return (DotProduct(dir1, dir2) <= 0);
} //end of the function VectorBetweenVectors
//===========================================================================
// returns the mid point between the two vectors
//
// Parameter:				-
// Returns:					-
// Changes Globals:		-
//===========================================================================
static void VectorMiddle( const vec3_t v1, const vec3_t v2, vec3_t middle)
{
	VectorAdd(v1, v2, middle);
	VectorScale(middle, 0.5, middle);
} //end of the function VectorMiddle
//===========================================================================
// calculate a range of points closest to each other on both edges
//
// Parameter:			beststart1		start of the range of points on edge v1-v2
//						beststart2		end of the range of points  on edge v1-v2
//						bestend1		start of the range of points on edge v3-v4
//						bestend2		end of the range of points  on edge v3-v4
//						bestdist		best distance so far
// Returns:				-
// Changes Globals:		-
//===========================================================================
/*
float AAS_ClosestEdgePoints(vec3_t v1, vec3_t v2, vec3_t v3, vec3_t v4,
							aas_plane_t *plane1, aas_plane_t *plane2,
							vec3_t beststart, vec3_t bestend, float bestdist)
{
	vec3_t dir1, dir2, p1, p2, p3, p4;
	float a1, a2, b1, b2, dist;
	int founddist;

	//edge vectors
	VectorSubtract(v2, v1, dir1);
	VectorSubtract(v4, v3, dir2);
	//get the horizontal directions
	dir1[2] = 0;
	dir2[2] = 0;
	//
	// p1 = point on an edge vector of area2 closest to v1
	// p2 = point on an edge vector of area2 closest to v2
	// p3 = point on an edge vector of area1 closest to v3
	// p4 = point on an edge vector of area1 closest to v4
	//
	if (dir2[0])
	{
		a2 = dir2[1] / dir2[0];
		b2 = v3[1] - a2 * v3[0];
		//point on the edge vector of area2 closest to v1
		p1[0] = (DotProduct(v1, dir2) - (a2 * dir2[0] + b2 * dir2[1])) / dir2[0];
		p1[1] = a2 * p1[0] + b2;
		//point on the edge vector of area2 closest to v2
		p2[0] = (DotProduct(v2, dir2) - (a2 * dir2[0] + b2 * dir2[1])) / dir2[0];
		p2[1] = a2 * p2[0] + b2;
	} //end if
	else
	{
		//point on the edge vector of area2 closest to v1
		p1[0] = v3[0];
		p1[1] = v1[1];
		//point on the edge vector of area2 closest to v2
		p2[0] = v3[0];
		p2[1] = v2[1];
	} //end else
	//
	if (dir1[0])
	{
		//
		a1 = dir1[1] / dir1[0];
		b1 = v1[1] - a1 * v1[0];
		//point on the edge vector of area1 closest to v3
		p3[0] = (DotProduct(v3, dir1) - (a1 * dir1[0] + b1 * dir1[1])) / dir1[0];
		p3[1] = a1 * p3[0] + b1;
		//point on the edge vector of area1 closest to v4
		p4[0] = (DotProduct(v4, dir1) - (a1 * dir1[0] + b1 * dir1[1])) / dir1[0];
		p4[1] = a1 * p4[0] + b1;
	} //end if
	else
	{
		//point on the edge vector of area1 closest to v3
		p3[0] = v1[0];
		p3[1] = v3[1];
		//point on the edge vector of area1 closest to v4
		p4[0] = v1[0];
		p4[1] = v4[1];
	} //end else
	//start with zero z-coordinates
	p1[2] = 0;
	p2[2] = 0;
	p3[2] = 0;
	p4[2] = 0;
	//calculate the z-coordinates from the ground planes
	p1[2] = (plane2->dist - DotProduct(plane2->normal, p1)) / plane2->normal[2];
	p2[2] = (plane2->dist - DotProduct(plane2->normal, p2)) / plane2->normal[2];
	p3[2] = (plane1->dist - DotProduct(plane1->normal, p3)) / plane1->normal[2];
	p4[2] = (plane1->dist - DotProduct(plane1->normal, p4)) / plane1->normal[2];
	//
	founddist = qfalse;
	//
	if (VectorBetweenVectors(p1, v3, v4))
	{
		dist = VectorDistance(v1, p1);
		if (dist > bestdist - 0.5 && dist < bestdist + 0.5)
		{
			VectorMiddle(beststart, v1, beststart);
			VectorMiddle(bestend, p1, bestend);
		} //end if
		else if (dist < bestdist)
		{
			bestdist = dist;
			VectorCopy(v1, beststart);
			VectorCopy(p1, bestend);
		} //end if
		founddist = qtrue;
	} //end if
	if (VectorBetweenVectors(p2, v3, v4))
	{
		dist = VectorDistance(v2, p2);
		if (dist > bestdist - 0.5 && dist < bestdist + 0.5)
		{
			VectorMiddle(beststart, v2, beststart);
			VectorMiddle(bestend, p2, bestend);
		} //end if
		else if (dist < bestdist)
		{
			bestdist = dist;
			VectorCopy(v2, beststart);
			VectorCopy(p2, bestend);
		} //end if
		founddist = qtrue;
	} //end else if
	if (VectorBetweenVectors(p3, v1, v2))
	{
		dist = VectorDistance(v3, p3);
		if (dist > bestdist - 0.5 && dist < bestdist + 0.5)
		{
			VectorMiddle(beststart, p3, beststart);
			VectorMiddle(bestend, v3, bestend);
		} //end if
		else if (dist < bestdist)
		{
			bestdist = dist;
			VectorCopy(p3, beststart);
			VectorCopy(v3, bestend);
		} //end if
		founddist = qtrue;
	} //end else if
	if (VectorBetweenVectors(p4, v1, v2))
	{
		dist = VectorDistance(v4, p4);
		if (dist > bestdist - 0.5 && dist < bestdist + 0.5)
		{
			VectorMiddle(beststart, p4, beststart);
			VectorMiddle(bestend, v4, bestend);
		} //end if
		else if (dist < bestdist)
		{
			bestdist = dist;
			VectorCopy(p4, beststart);
			VectorCopy(v4, bestend);
		} //end if
		founddist = qtrue;
	} //end else if
	//if no shortest distance was found the shortest distance
	//is between one of the vertexes of edge1 and one of edge2
	if (!founddist)
	{
		dist = VectorDistance(v1, v3);
		if (dist < bestdist)
		{
			bestdist = dist;
			VectorCopy(v1, beststart);
			VectorCopy(v3, bestend);
		} //end if
		dist = VectorDistance(v1, v4);
		if (dist < bestdist)
		{
			bestdist = dist;
			VectorCopy(v1, beststart);
			VectorCopy(v4, bestend);
		} //end if
		dist = VectorDistance(v2, v3);
		if (dist < bestdist)
		{
			bestdist = dist;
			VectorCopy(v2, beststart);
			VectorCopy(v3, bestend);
		} //end if
		dist = VectorDistance(v2, v4);
		if (dist < bestdist)
		{
			bestdist = dist;
			VectorCopy(v2, beststart);
			VectorCopy(v4, bestend);
		} //end if
	} //end if
	return bestdist;
} //end of the function AAS_ClosestEdgePoints*/

static float AAS_ClosestEdgePoints(vec3_t v1, vec3_t v2, vec3_t v3, vec3_t v4,
							aas_plane_t *plane1, aas_plane_t *plane2,
							vec3_t beststart1, vec3_t bestend1,
							vec3_t beststart2, vec3_t bestend2, float bestdist)
{
	vec3_t dir1, dir2, p1, p2, p3, p4;
	float a1, a2, b1, b2, dist, dist1, dist2;
	int founddist;

	//edge vectors
	VectorSubtract(v2, v1, dir1);
	VectorSubtract(v4, v3, dir2);
	//get the horizontal directions
	dir1[2] = 0;
	dir2[2] = 0;
	//
	// p1 = point on an edge vector of area2 closest to v1
	// p2 = point on an edge vector of area2 closest to v2
	// p3 = point on an edge vector of area1 closest to v3
	// p4 = point on an edge vector of area1 closest to v4
	//
	if (dir2[0])
	{
		a2 = dir2[1] / dir2[0];
		b2 = v3[1] - a2 * v3[0];
		//point on the edge vector of area2 closest to v1
		p1[0] = (DotProduct(v1, dir2) - (a2 * dir2[0] + b2 * dir2[1])) / dir2[0];
		p1[1] = a2 * p1[0] + b2;
		//point on the edge vector of area2 closest to v2
		p2[0] = (DotProduct(v2, dir2) - (a2 * dir2[0] + b2 * dir2[1])) / dir2[0];
		p2[1] = a2 * p2[0] + b2;
	} //end if
	else
	{
		//point on the edge vector of area2 closest to v1
		p1[0] = v3[0];
		p1[1] = v1[1];
		//point on the edge vector of area2 closest to v2
		p2[0] = v3[0];
		p2[1] = v2[1];
	} //end else
	//
	if (dir1[0])
	{
		//
		a1 = dir1[1] / dir1[0];
		b1 = v1[1] - a1 * v1[0];
		//point on the edge vector of area1 closest to v3
		p3[0] = (DotProduct(v3, dir1) - (a1 * dir1[0] + b1 * dir1[1])) / dir1[0];
		p3[1] = a1 * p3[0] + b1;
		//point on the edge vector of area1 closest to v4
		p4[0] = (DotProduct(v4, dir1) - (a1 * dir1[0] + b1 * dir1[1])) / dir1[0];
		p4[1] = a1 * p4[0] + b1;
	} //end if
	else
	{
		//point on the edge vector of area1 closest to v3
		p3[0] = v1[0];
		p3[1] = v3[1];
		//point on the edge vector of area1 closest to v4
		p4[0] = v1[0];
		p4[1] = v4[1];
	} //end else
	//start with zero z-coordinates
	p1[2] = 0;
	p2[2] = 0;
	p3[2] = 0;
	p4[2] = 0;
	//calculate the z-coordinates from the ground planes
	p1[2] = (plane2->dist - DotProduct(plane2->normal, p1)) / plane2->normal[2];
	p2[2] = (plane2->dist - DotProduct(plane2->normal, p2)) / plane2->normal[2];
	p3[2] = (plane1->dist - DotProduct(plane1->normal, p3)) / plane1->normal[2];
	p4[2] = (plane1->dist - DotProduct(plane1->normal, p4)) / plane1->normal[2];
	//
	founddist = qfalse;
	//
	if (VectorBetweenVectors(p1, v3, v4))
	{
		dist = VectorDistance(v1, p1);
		if (dist > bestdist - 0.5 && dist < bestdist + 0.5)
		{
			dist1 = VectorDistance(beststart1, v1);
			dist2 = VectorDistance(beststart2, v1);
			if (dist1 > dist2)
			{
				if (dist1 > VectorDistance(beststart1, beststart2)) VectorCopy(v1, beststart2);
			} //end if
			else
			{
				if (dist2 > VectorDistance(beststart1, beststart2)) VectorCopy(v1, beststart1);
			} //end else
			dist1 = VectorDistance(bestend1, p1);
			dist2 = VectorDistance(bestend2, p1);
			if (dist1 > dist2)
			{
				if (dist1 > VectorDistance(bestend1, bestend2)) VectorCopy(p1, bestend2);
			} //end if
			else
			{
				if (dist2 > VectorDistance(bestend1, bestend2)) VectorCopy(p1, bestend1);
			} //end else
		} //end if
		else if (dist < bestdist)
		{
			bestdist = dist;
			VectorCopy(v1, beststart1);
			VectorCopy(v1, beststart2);
			VectorCopy(p1, bestend1);
			VectorCopy(p1, bestend2);
		} //end if
		founddist = qtrue;
	} //end if
	if (VectorBetweenVectors(p2, v3, v4))
	{
		dist = VectorDistance(v2, p2);
		if (dist > bestdist - 0.5 && dist < bestdist + 0.5)
		{
			dist1 = VectorDistance(beststart1, v2);
			dist2 = VectorDistance(beststart2, v2);
			if (dist1 > dist2)
			{
				if (dist1 > VectorDistance(beststart1, beststart2)) VectorCopy(v2, beststart2);
			} //end if
			else
			{
				if (dist2 > VectorDistance(beststart1, beststart2)) VectorCopy(v2, beststart1);
			} //end else
			dist1 = VectorDistance(bestend1, p2);
			dist2 = VectorDistance(bestend2, p2);
			if (dist1 > dist2)
			{
				if (dist1 > VectorDistance(bestend1, bestend2)) VectorCopy(p2, bestend2);
			} //end if
			else
			{
				if (dist2 > VectorDistance(bestend1, bestend2)) VectorCopy(p2, bestend1);
			} //end else
		} //end if
		else if (dist < bestdist)
		{
			bestdist = dist;
			VectorCopy(v2, beststart1);
			VectorCopy(v2, beststart2);
			VectorCopy(p2, bestend1);
			VectorCopy(p2, bestend2);
		} //end if
		founddist = qtrue;
	} //end else if
	if (VectorBetweenVectors(p3, v1, v2))
	{
		dist = VectorDistance(v3, p3);
		if (dist > bestdist - 0.5 && dist < bestdist + 0.5)
		{
			dist1 = VectorDistance(beststart1, p3);
			dist2 = VectorDistance(beststart2, p3);
			if (dist1 > dist2)
			{
				if (dist1 > VectorDistance(beststart1, beststart2)) VectorCopy(p3, beststart2);
			} //end if
			else
			{
				if (dist2 > VectorDistance(beststart1, beststart2)) VectorCopy(p3, beststart1);
			} //end else
			dist1 = VectorDistance(bestend1, v3);
			dist2 = VectorDistance(bestend2, v3);
			if (dist1 > dist2)
			{
				if (dist1 > VectorDistance(bestend1, bestend2)) VectorCopy(v3, bestend2);
			} //end if
			else
			{
				if (dist2 > VectorDistance(bestend1, bestend2)) VectorCopy(v3, bestend1);
			} //end else
		} //end if
		else if (dist < bestdist)
		{
			bestdist = dist;
			VectorCopy(p3, beststart1);
			VectorCopy(p3, beststart2);
			VectorCopy(v3, bestend1);
			VectorCopy(v3, bestend2);
		} //end if
		founddist = qtrue;
	} //end else if
	if (VectorBetweenVectors(p4, v1, v2))
	{
		dist = VectorDistance(v4, p4);
		if (dist > bestdist - 0.5 && dist < bestdist + 0.5)
		{
			dist1 = VectorDistance(beststart1, p4);
			dist2 = VectorDistance(beststart2, p4);
			if (dist1 > dist2)
			{
				if (dist1 > VectorDistance(beststart1, beststart2)) VectorCopy(p4, beststart2);
			} //end if
			else
			{
				if (dist2 > VectorDistance(beststart1, beststart2)) VectorCopy(p4, beststart1);
			} //end else
			dist1 = VectorDistance(bestend1, v4);
			dist2 = VectorDistance(bestend2, v4);
			if (dist1 > dist2)
			{
				if (dist1 > VectorDistance(bestend1, bestend2)) VectorCopy(v4, bestend2);
			} //end if
			else
			{
				if (dist2 > VectorDistance(bestend1, bestend2)) VectorCopy(v4, bestend1);
			} //end else
		} //end if
		else if (dist < bestdist)
		{
			bestdist = dist;
			VectorCopy(p4, beststart1);
			VectorCopy(p4, beststart2);
			VectorCopy(v4, bestend1);
			VectorCopy(v4, bestend2);
		} //end if
		founddist = qtrue;
	} //end else if
	//if no shortest distance was found the shortest distance
	//is between one of the vertexes of edge1 and one of edge2
	if (!founddist)
	{
		dist = VectorDistance(v1, v3);
		if (dist < bestdist)
		{
			bestdist = dist;
			VectorCopy(v1, beststart1);
			VectorCopy(v1, beststart2);
			VectorCopy(v3, bestend1);
			VectorCopy(v3, bestend2);
		} //end if
		dist = VectorDistance(v1, v4);
		if (dist < bestdist)
		{
			bestdist = dist;
			VectorCopy(v1, beststart1);
			VectorCopy(v1, beststart2);
			VectorCopy(v4, bestend1);
			VectorCopy(v4, bestend2);
		} //end if
		dist = VectorDistance(v2, v3);
		if (dist < bestdist)
		{
			bestdist = dist;
			VectorCopy(v2, beststart1);
			VectorCopy(v2, beststart2);
			VectorCopy(v3, bestend1);
			VectorCopy(v3, bestend2);
		} //end if
		dist = VectorDistance(v2, v4);
		if (dist < bestdist)
		{
			bestdist = dist;
			VectorCopy(v2, beststart1);
			VectorCopy(v2, beststart2);
			VectorCopy(v4, bestend1);
			VectorCopy(v4, bestend2);
		} //end if
	} //end if
	return bestdist;
} //end of the function AAS_ClosestEdgePoints
//===========================================================================
//
// Parameter:			-
// Returns:				-
// Changes Globals:		-
//===========================================================================
static int AAS_TravelFlagsForTeam(int ent)
{
	int notteam;

	if (!AAS_IntForBSPEpairKey(ent, "bot_notteam", &notteam))
		return 0;
	if (notteam == 1)
		return TRAVELFLAG_NOTTEAM1;
	if (notteam == 2)
		return TRAVELFLAG_NOTTEAM2;
	return 0;
} //end of the function AAS_TravelFlagsForTeam
//===========================================================================
//
// Parameter:			-
// Returns:				-
// Changes Globals:		-
//===========================================================================
static aas_lreachability_t *AAS_FindFaceReachabilities(vec3_t *facepoints, int numpoints, aas_plane_t *plane, int towardsface)
{
	int i, j, k, l;
	int facenum, edgenum, bestfacenum;
	float *v1, *v2, *v3, *v4;
	float bestdist, speed, hordist, dist;
	vec3_t beststart, beststart2 = { 0 }, bestend, bestend2 = { 0 }, tmp, hordir, testpoint;
	aas_lreachability_t *lreach, *lreachabilities;
	aas_area_t *area;
	aas_face_t *face;
	aas_edge_t *edge;
	aas_plane_t *faceplane, *bestfaceplane;

	//
	lreachabilities = NULL;
	bestfacenum = 0;
	bestfaceplane = NULL;
	//
	for (i = 1; i < aasworld.numareas; i++)
	{
		area = &aasworld.areas[i];
		// get the shortest distance between one of the func_bobbing start edges and
		// one of the face edges of area1
		bestdist = 999999;
		for (j = 0; j < area->numfaces; j++)
		{
			facenum = aasworld.faceindex[area->firstface + j];
			face = &aasworld.faces[abs(facenum)];
			//if not a ground face
			if (!(face->faceflags & FACE_GROUND)) continue;
			//get the ground planes
			faceplane = &aasworld.planes[face->planenum];
			//
			for (k = 0; k < face->numedges; k++)
			{
				edgenum = abs(aasworld.edgeindex[face->firstedge + k]);
				edge = &aasworld.edges[edgenum];
				//calculate the minimum distance between the two edges
				v1 = aasworld.vertexes[edge->v[0]];
				v2 = aasworld.vertexes[edge->v[1]];
				//
				for (l = 0; l < numpoints; l++)
				{
					v3 = facepoints[l];
					v4 = facepoints[(l+1) % numpoints];
					dist = AAS_ClosestEdgePoints(v1, v2, v3, v4, faceplane, plane,
													beststart, bestend,
													beststart2, bestend2, bestdist);
					if (dist < bestdist)
					{
						bestfacenum = facenum;
						bestfaceplane = faceplane;
						bestdist = dist;
					} //end if
				} //end for
			} //end for
		} //end for
		//
		if (bestdist > 192) continue;
		//
		VectorMiddle(beststart, beststart2, beststart);
		VectorMiddle(bestend, bestend2, bestend);
		//
		if (!towardsface)
		{
			VectorCopy(beststart, tmp);
			VectorCopy(bestend, beststart);
			VectorCopy(tmp, bestend);
		} //end if
		//
		VectorSubtract(bestend, beststart, hordir);
		hordir[2] = 0;
		hordist = VectorLength(hordir);
		//
		if (hordist > 2 * AAS_MaxJumpDistance(aassettings.phys_jumpvel)) continue;
		//the end point should not be significantly higher than the start point
		if (bestend[2] - 32 > beststart[2]) continue;
		//don't fall down too far
		if (bestend[2] < beststart[2] - 128) continue;
		//the distance should not be too far
		if (hordist > 32)
		{
			//check for walk off ledge
			if (!AAS_HorizontalVelocityForJump(0, beststart, bestend, &speed)) continue;
		} //end if
		//
		beststart[2] += 1;
		bestend[2] += 1;
		//
		if (towardsface) VectorCopy(bestend, testpoint);
		else VectorCopy(beststart, testpoint);
		if (bestfaceplane != NULL)
			testpoint[2] = (bestfaceplane->dist - DotProduct(bestfaceplane->normal, testpoint)) / bestfaceplane->normal[2];
		else
			testpoint[2] = 0;
		//
		if (!AAS_PointInsideFace(bestfacenum, testpoint, 0.1f))
		{
			//if the faces are not overlapping then only go down
			if (bestend[2] - 16 > beststart[2]) continue;
		} //end if
		lreach = AAS_AllocReachability();
		if (!lreach) return lreachabilities;
		lreach->areanum = i;
		lreach->facenum = 0;
		lreach->edgenum = 0;
		VectorCopy(beststart, lreach->start);
		VectorCopy(bestend, lreach->end);
		lreach->traveltype = 0;
		lreach->traveltime = 0;
		lreach->next = lreachabilities;
		lreachabilities = lreach;
	} //end for
	return lreachabilities;
} //end of the function AAS_FindFaceReachabilities
//===========================================================================
//
// Parameter:				-
// Returns:					-
// Changes Globals:		-
//===========================================================================
static void AAS_SetWeaponJumpAreaFlags(void)
{
	int ent, i;
	vec3_t mins = {-15, -15, -15}, maxs = {15, 15, 15};
	vec3_t origin;
	int areanum, weaponjumpareas, spawnflags;
	char classname[MAX_EPAIRKEY];

	weaponjumpareas = 0;
	for (ent = AAS_NextBSPEntity(0); ent; ent = AAS_NextBSPEntity(ent))
	{
		if (!AAS_ValueForBSPEpairKey(ent, "classname", classname, MAX_EPAIRKEY)) continue;
		if (
			!strcmp(classname, "item_armor_body") ||
			!strcmp(classname, "item_armor_combat") ||
			!strcmp(classname, "item_health_mega") ||
			!strcmp(classname, "weapon_grenadelauncher") ||
			!strcmp(classname, "weapon_rocketlauncher") ||
			!strcmp(classname, "weapon_lightning") ||
			!strcmp(classname, "weapon_plasmagun") ||
			!strcmp(classname, "weapon_railgun") ||
			!strcmp(classname, "weapon_bfg") ||
			!strcmp(classname, "item_quad") ||
			!strcmp(classname, "item_regen") ||
			!strcmp(classname, "item_invulnerability"))
		{
			if (AAS_VectorForBSPEpairKey(ent, "origin", origin))
			{
				spawnflags = 0;
				AAS_IntForBSPEpairKey(ent, "spawnflags", &spawnflags);
				//if not a stationary item
				if (!(spawnflags & 1))
				{
					if (!AAS_DropToFloor(origin, mins, maxs))
					{
						botimport.Print(PRT_MESSAGE, "%s in solid at (%1.1f %1.1f %1.1f)\n",
														classname, origin[0], origin[1], origin[2]);
					} //end if
				} //end if
				//areanum = AAS_PointAreaNum(origin);
				areanum = AAS_BestReachableArea(origin, mins, maxs, origin);
				//the bot may rocket jump towards this area
				aasworld.areasettings[areanum].areaflags |= AREA_WEAPONJUMP;
				//
				//if (!AAS_AreaGrounded(areanum))
				//	botimport.Print(PRT_MESSAGE, "area not grounded\n");
				//
				weaponjumpareas++;
			} //end if
		} //end if
	} //end for
	for (i = 1; i < aasworld.numareas; i++)
	{
		if (aasworld.areasettings[i].contents & AREACONTENTS_JUMPPAD)
		{
			aasworld.areasettings[i].areaflags |= AREA_WEAPONJUMP;
			weaponjumpareas++;
		} //end if
	} //end for
	botimport.Print(PRT_MESSAGE, "%d weapon jump areas\n", weaponjumpareas);
} //end of the function AAS_SetWeaponJumpAreaFlags
//===========================================================================
// create a possible weapon jump reachability from area1 to area2
//
// check if there's a cool item in the second area
// check if area1 is lower than area2
// check if the bot can rocketjump from area1 to area2
//
// Parameter:				-
// Returns:					-
// Changes Globals:		-
//===========================================================================
int AAS_Reachability_WeaponJump(int area1num, int area2num)
{
	int face2num, i, n, ret, visualize;
	float speed, zvel;
	//float hordist;
	aas_face_t *face2;
	aas_area_t *area1, *area2;
	aas_lreachability_t *lreach;
	vec3_t areastart, facecenter, start, end, dir, cmdmove;// teststart;
	vec3_t velocity;
	aas_clientmove_t move;
	aas_trace_t trace;

	visualize = qfalse;
//	if (area1num == 4436 && area2num == 4318)
//	{
//		visualize = qtrue;
//	}
	if (!AAS_AreaGrounded(area1num) || AAS_AreaSwim(area1num)) return qfalse;
	if (!AAS_AreaGrounded(area2num)) return qfalse;
	//NOTE: only weapon jump towards areas with an interesting item in it??
	if (!(aasworld.areasettings[area2num].areaflags & AREA_WEAPONJUMP)) return qfalse;
	//
	area1 = &aasworld.areas[area1num];
	area2 = &aasworld.areas[area2num];
	//don't weapon jump towards way lower areas
	if (area2->maxs[2] < area1->mins[2]) return qfalse;
	//
	VectorCopy(aasworld.areas[area1num].center, start);
	VectorCopy(start, end);
	end[2] -= 1000;
	trace = AAS_TraceClientBBox(start, end, PRESENCE_CROUCH, -1);
	if (trace.startsolid) return qfalse;
	VectorCopy(trace.endpos, areastart);
	//
	//areastart is now the start point
	//
	for (i = 0; i < area2->numfaces; i++)
	{
		face2num = aasworld.faceindex[area2->firstface + i];
		face2 = &aasworld.faces[abs(face2num)];
		//if it is not a solid face
		if (!(face2->faceflags & FACE_GROUND)) continue;
		//get the center of the face
		AAS_FaceCenter(face2num, facecenter);
		//only go higher up with weapon jumps
		if (facecenter[2] < areastart[2] + 64) continue;
		//NOTE: set to 2 to allow bfg jump reachabilities
		for (n = 0; n < 1/*2*/; n++)
		{
			//get the weapon jump z velocity
			if (n) zvel = AAS_BFGJumpZVelocity(areastart);
			else zvel = AAS_RocketJumpZVelocity(areastart);
			//get the horizontal speed for the jump, if it isn't possible to calculate this
			//speed (the jump is not possible) then there's no jump reachability created
			ret = AAS_HorizontalVelocityForJump(zvel, areastart, facecenter, &speed);
			if (ret && speed < 300)
			{
				//direction towards the face center
				VectorSubtract(facecenter, areastart, dir);
				dir[2] = 0;
				//hordist = VectorNormalize(dir);
				//if (hordist < 1.6 * (facecenter[2] - areastart[2]))
				{
					//get command movement
					VectorScale(dir, speed, cmdmove);
					VectorSet(velocity, 0, 0, zvel);
					/*
					//get command movement
					VectorScale(dir, speed, velocity);
					velocity[2] = zvel;
					VectorSet(cmdmove, 0, 0, 0);
					*/
					//
					AAS_PredictClientMovement(&move, -1, areastart, PRESENCE_NORMAL, qtrue,
												velocity, cmdmove, 30, 30, 0.1f,
												SE_ENTERWATER|SE_ENTERSLIME|
												SE_ENTERLAVA|SE_HITGROUNDDAMAGE|
												SE_TOUCHJUMPPAD|SE_HITGROUND|SE_HITGROUNDAREA, area2num, visualize);
					//if prediction time wasn't enough to fully predict the movement
					//don't enter slime or lava and don't fall from too high
					if (move.frames < 30 && 
							!(move.stopevent & (SE_ENTERSLIME|SE_ENTERLAVA|SE_HITGROUNDDAMAGE))
								&& (move.stopevent & (SE_HITGROUNDAREA|SE_TOUCHJUMPPAD)))
					{
						//create a rocket or bfg jump reachability from area1 to area2
						lreach = AAS_AllocReachability();
						if (!lreach) return qfalse;
						lreach->areanum = area2num;
						lreach->facenum = 0;
						lreach->edgenum = 0;
						VectorCopy(areastart, lreach->start);
						VectorCopy(facecenter, lreach->end);
						if (n)
						{
							lreach->traveltype = TRAVEL_BFGJUMP;
							lreach->traveltime = aassettings.rs_bfgjump;
						} //end if
						else
						{
							lreach->traveltype = TRAVEL_ROCKETJUMP;
							lreach->traveltime = aassettings.rs_rocketjump;
						} //end else
						lreach->next = areareachability[area1num];
						areareachability[area1num] = lreach;
						//
						reach_rocketjump++;
						return qtrue;
					} //end if
				} //end if
			} //end if
		} //end for
	} //end for
	//
	return qfalse;
} //end of the function AAS_Reachability_WeaponJump
//===========================================================================
//
// Parameter:				-
// Returns:					-
// Changes Globals:		-
//===========================================================================
void AAS_InitReachability(void)
{
	if (!aasworld.loaded) return;

	if (aasworld.reachabilitysize)
	{
		aasworld.numreachabilityareas = aasworld.numareas + 2;
		return;
	} //end if
	aasworld.savefile = qtrue;
	//start with area 1 because area zero is a dummy
	aasworld.numreachabilityareas = 1;
	////aasworld.numreachabilityareas = aasworld.numareas + 1;		//only calculate entity reachabilities
	//setup the heap with reachability links
	AAS_SetupReachabilityHeap();
	//allocate area reachability link array
	areareachability = (aas_lreachability_t **) malloc(
									aasworld.numareas * sizeof(aas_lreachability_t *));
	//
	AAS_SetWeaponJumpAreaFlags();
} //end of the function AAS_InitReachable
