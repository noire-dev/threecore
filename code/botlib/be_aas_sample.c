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
 * name:		be_aas_sample.c
 *
 * desc:		AAS environment sampling
 *
 * $Archive: /MissionPack/code/botlib/be_aas_sample.c $
 *
 *****************************************************************************/

#include "../qcommon/q_shared.h"
#include "aasfile.h"
#include "botlib.h"
#include "be_aas.h"
#include "be_interface.h"
#include "be_aas_funcs.h"
#include "be_aas_def.h"

#define BBOX_NORMAL_EPSILON		0.001
#define ON_EPSILON					0 //0.0005
#define TRACEPLANE_EPSILON			0.125

typedef struct aas_tracestack_s {
	vec3_t start;		//start point of the piece of line to trace
	vec3_t end;			//end point of the piece of line to trace
	int planenum;		//last plane used as splitter
	int nodenum;		//node found after splitting with planenum
} aas_tracestack_t;

void AAS_PresenceTypeBoundingBox(int presencetype, vec3_t mins, vec3_t maxs) {
	int index;
	//bounding box size for each presence type
	static const vec3_t boxmins[3] = {{0, 0, 0}, {-15, -15, -24}, {-15, -15, -24}};
	static const vec3_t boxmaxs[3] = {{0, 0, 0}, { 15,  15,  32}, { 15,  15,   8}};

	if (presencetype == PRESENCE_NORMAL) index = 1;
	else if (presencetype == PRESENCE_CROUCH) index = 2;
	else
	{
		botimport.Print(PRT_FATAL, "AAS_PresenceTypeBoundingBox: unknown presence type\n");
		index = 2;
	} //end if
	VectorCopy(boxmins[index], mins);
	VectorCopy(boxmaxs[index], maxs);
}

void AAS_InitAASLinkHeap(void) {
	int i, max_aaslinks;

	max_aaslinks = aasworld.linkheapsize;
	//if there's no link heap present
	if (!aasworld.linkheap)
	{
		max_aaslinks = 6144;
		if (max_aaslinks < 0) max_aaslinks = 0;
		aasworld.linkheapsize = max_aaslinks;
		aasworld.linkheap = (aas_link_t *) malloc(max_aaslinks * sizeof(aas_link_t));
	} //end if
	//link the links on the heap
	aasworld.linkheap[0].prev_ent = NULL;
	aasworld.linkheap[0].next_ent = &aasworld.linkheap[1];
	for (i = 1; i < max_aaslinks-1; i++)
	{
		aasworld.linkheap[i].prev_ent = &aasworld.linkheap[i - 1];
		aasworld.linkheap[i].next_ent = &aasworld.linkheap[i + 1];
	} //end for
	aasworld.linkheap[max_aaslinks-1].prev_ent = &aasworld.linkheap[max_aaslinks-2];
	aasworld.linkheap[max_aaslinks-1].next_ent = NULL;
	//pointer to the first free link
	aasworld.freelinks = &aasworld.linkheap[0];
}

void AAS_FreeAASLinkHeap(void) {
	if (aasworld.linkheap) free(aasworld.linkheap);
	aasworld.linkheap = NULL;
	aasworld.linkheapsize = 0;
}

void AAS_InitAASLinkedEntities(void) {
	if (!aasworld.loaded) return;
	if (aasworld.arealinkedentities) free(aasworld.arealinkedentities);
	aasworld.arealinkedentities = (aas_link_t **) malloc(
						aasworld.numareas * sizeof(aas_link_t *));
}

void AAS_FreeAASLinkedEntities(void) {
	if (aasworld.arealinkedentities) free(aasworld.arealinkedentities);
	aasworld.arealinkedentities = NULL;
}

int AAS_PointAreaNum(vec3_t point) {
	int nodenum;
	vec_t	dist;
	aas_node_t *node;
	aas_plane_t *plane;

	if (!aasworld.loaded) {
		botimport.Print(PRT_ERROR, "AAS_PointAreaNum: aas not loaded\n");
		return 0;
	} //end if

	//start with node 1 because node zero is a dummy used for solid leafs
	nodenum = 1;
	while (nodenum > 0) {
		node = &aasworld.nodes[nodenum];
		plane = &aasworld.planes[node->planenum];
		dist = DotProduct(point, plane->normal) - plane->dist;
		if (dist > 0) nodenum = node->children[0];
		else nodenum = node->children[1];
	} //end while
	if (!nodenum) {
		return 0;
	} //end if
	return -nodenum;
}

int AAS_PointReachabilityAreaIndex( vec3_t origin ) {
	int areanum, cluster, i, index;

	if (!aasworld.initialized)
		return 0;

	if ( !origin )
	{
		index = 0;
		for (i = 0; i < aasworld.numclusters; i++)
		{
			index += aasworld.clusters[i].numreachabilityareas;
		} //end for
		return index;
	} //end if

	areanum = AAS_PointAreaNum( origin );
	if ( !areanum || !AAS_AreaReachability(areanum) )
		return 0;
	cluster = aasworld.areasettings[areanum].cluster;
	areanum = aasworld.areasettings[areanum].clusterareanum;
	if (cluster < 0)
	{
		cluster = aasworld.portals[-cluster].frontcluster;
		areanum = aasworld.portals[-cluster].clusterareanum[0];
	} //end if

	index = 0;
	for (i = 0; i < cluster; i++)
	{
		index += aasworld.clusters[i].numreachabilityareas;
	} //end for
	index += areanum;
	return index;
}

int AAS_AreaPresenceType(int areanum) {
	if (!aasworld.loaded) return 0;
	if (areanum <= 0 || areanum >= aasworld.numareas)
	{
		botimport.Print(PRT_ERROR, "AAS_AreaPresenceType: invalid area number\n");
		return 0;
	} //end if
	return aasworld.areasettings[areanum].presencetype;
}

static qboolean AAS_AreaEntityCollision(int areanum, vec3_t start, vec3_t end, int presencetype, int passent, aas_trace_t *trace) {
	int collision;
	vec3_t boxmins, boxmaxs;
	aas_link_t *link;
	bsp_trace_t bsptrace;

	AAS_PresenceTypeBoundingBox(presencetype, boxmins, boxmaxs);

	Com_Memset(&bsptrace, 0, sizeof(bsp_trace_t)); //make compiler happy
	//assume no collision
	bsptrace.fraction = 1;
	collision = qfalse;
	for (link = aasworld.arealinkedentities[areanum]; link; link = link->next_ent)
	{
		//ignore the pass entity
		if (link->entnum == passent) continue;
		//
		if (AAS_EntityCollision(link->entnum, start, boxmins, boxmaxs, end,
												CONTENTS_SOLID|CONTENTS_PLAYERCLIP, &bsptrace))
		{
			collision = qtrue;
		} //end if
	} //end for
	if (collision)
	{
		trace->startsolid = bsptrace.startsolid;
		trace->ent = bsptrace.ent;
		VectorCopy(bsptrace.endpos, trace->endpos);
		trace->area = 0;
		trace->planenum = 0;
		return qtrue;
	} //end if
	return qfalse;
}

aas_trace_t AAS_TraceClientBBox(vec3_t start, vec3_t end, int presencetype, int passent) {
	int side, nodenum, tmpplanenum;
	float front, back, frac;
	vec3_t cur_start, cur_end, cur_mid, v1, v2;
	aas_tracestack_t tracestack[127];
	aas_tracestack_t *tstack_p;
	aas_node_t *aasnode;
	aas_plane_t *plane;
	aas_trace_t trace;

	//clear the trace structure
	Com_Memset(&trace, 0, sizeof(aas_trace_t));

	if (!aasworld.loaded) return trace;
	
	tstack_p = tracestack;
	//we start with the whole line on the stack
	VectorCopy(start, tstack_p->start);
	VectorCopy(end, tstack_p->end);
	tstack_p->planenum = 0;
	//start with node 1 because node zero is a dummy for a solid leaf
	tstack_p->nodenum = 1;		//starting at the root of the tree
	tstack_p++;
	
	while (1)
	{
		//pop up the stack
		tstack_p--;
		//if the trace stack is empty (ended up with a piece of the
		//line to be traced in an area)
		if (tstack_p < tracestack)
		{
			tstack_p++;
			//nothing was hit
			trace.startsolid = qfalse;
			trace.fraction = 1.0;
			//endpos is the end of the line
			VectorCopy(end, trace.endpos);
			//nothing hit
			trace.ent = 0;
			trace.area = 0;
			trace.planenum = 0;
			return trace;
		} //end if
		//number of the current node to test the line against
		nodenum = tstack_p->nodenum;
		//if it is an area
		if (nodenum < 0)
		{
			//botimport.Print(PRT_MESSAGE, "areanum = %d, must be %d\n", -nodenum, AAS_PointAreaNum(start));
			//if can't enter the area because it hasn't got the right presence type
			if (!(aasworld.areasettings[-nodenum].presencetype & presencetype))
			{
				//if the start point is still the initial start point
				//NOTE: no need for epsilons because the points will be
				//exactly the same when they're both the start point
				if (tstack_p->start[0] == start[0] &&
						tstack_p->start[1] == start[1] &&
						tstack_p->start[2] == start[2])
				{
					trace.startsolid = qtrue;
					trace.fraction = 0.0;
					VectorClear(v1);
				} //end if
				else
				{
					trace.startsolid = qfalse;
					VectorSubtract(end, start, v1);
					VectorSubtract(tstack_p->start, start, v2);
					trace.fraction = VectorLength(v2) / VectorNormalize(v1);
					VectorMA(tstack_p->start, -0.125, v1, tstack_p->start);
				} //end else
				VectorCopy(tstack_p->start, trace.endpos);
				trace.ent = 0;
				trace.area = -nodenum;
//				VectorSubtract(end, start, v1);
				trace.planenum = tstack_p->planenum;
				//always take the plane with normal facing towards the trace start
				plane = &aasworld.planes[trace.planenum];
				if (DotProduct(v1, plane->normal) > 0) trace.planenum ^= 1;
				return trace;
			} //end if
			else
			{
				if (passent >= 0)
				{
					if (AAS_AreaEntityCollision(-nodenum, tstack_p->start,
													tstack_p->end, presencetype, passent,
													&trace))
					{
						if (!trace.startsolid)
						{
							VectorSubtract(end, start, v1);
							VectorSubtract(trace.endpos, start, v2);
							trace.fraction = VectorLength(v2) / VectorLength(v1);
						} //end if
						return trace;
					} //end if
				} //end if
			} //end else
			trace.lastarea = -nodenum;
			continue;
		} //end if
		//if it is a solid leaf
		if (!nodenum)
		{
			//if the start point is still the initial start point
			//NOTE: no need for epsilons because the points will be
			//exactly the same when they're both the start point
			if (tstack_p->start[0] == start[0] &&
					tstack_p->start[1] == start[1] &&
					tstack_p->start[2] == start[2])
			{
				trace.startsolid = qtrue;
				trace.fraction = 0.0;
				VectorClear(v1);
			} //end if
			else
			{
				trace.startsolid = qfalse;
				VectorSubtract(end, start, v1);
				VectorSubtract(tstack_p->start, start, v2);
				trace.fraction = VectorLength(v2) / VectorNormalize(v1);
				VectorMA(tstack_p->start, -0.125, v1, tstack_p->start);
			} //end else
			VectorCopy(tstack_p->start, trace.endpos);
			trace.ent = 0;
			trace.area = 0;	//hit solid leaf
//			VectorSubtract(end, start, v1);
			trace.planenum = tstack_p->planenum;
			//always take the plane with normal facing towards the trace start
			plane = &aasworld.planes[trace.planenum];
			if (DotProduct(v1, plane->normal) > 0) trace.planenum ^= 1;
			return trace;
		} //end if
		//the node to test against
		aasnode = &aasworld.nodes[nodenum];
		//start point of current line to test against node
		VectorCopy(tstack_p->start, cur_start);
		//end point of the current line to test against node
		VectorCopy(tstack_p->end, cur_end);
		//the current node plane
		plane = &aasworld.planes[aasnode->planenum];

		switch(plane->type)
		{
			default: //gee it's not an axial plane
			{
				front = DotProduct(cur_start, plane->normal) - plane->dist;
				back = DotProduct(cur_end, plane->normal) - plane->dist;
				break;
			} //end default
		} //end switch
		if ((front >= -ON_EPSILON && back >= -ON_EPSILON))
		{
			//keep the current start and end point on the stack
			//and go down the tree with the front child
			tstack_p->nodenum = aasnode->children[0];
			tstack_p++;
			if (tstack_p >= &tracestack[127])
			{
				botimport.Print(PRT_ERROR, "AAS_TraceBoundingBox: stack overflow\n");
				return trace;
			} //end if
		} //end if
		//if the whole to be traced line is totally at the back of this node
		//only go down the tree with the back child
		else if ((front < ON_EPSILON && back < ON_EPSILON))
		{
			//keep the current start and end point on the stack
			//and go down the tree with the back child
			tstack_p->nodenum = aasnode->children[1];
			tstack_p++;
			if (tstack_p >= &tracestack[127])
			{
				botimport.Print(PRT_ERROR, "AAS_TraceBoundingBox: stack overflow\n");
				return trace;
			} //end if
		} //end if
		//go down the tree both at the front and back of the node
		else
		{
			tmpplanenum = tstack_p->planenum;
			// bk010221 - new location of divide by zero (see above)
			if ( front == back ) front -= 0.001f; // bk0101022 - hack/FPE 
                	//calculate the hitpoint with the node (split point of the line)
			//put the crosspoint TRACEPLANE_EPSILON pixels on the near side
			if (front < 0) frac = (front + TRACEPLANE_EPSILON)/(front-back);
			else frac = (front - TRACEPLANE_EPSILON)/(front-back); // bk010221
			//
			if (frac < 0)
				frac = 0.001f; //0
			else if (frac > 1)
				frac = 0.999f; //1
			//frac = front / (front-back);
			cur_mid[0] = cur_start[0] + (cur_end[0] - cur_start[0]) * frac;
			cur_mid[1] = cur_start[1] + (cur_end[1] - cur_start[1]) * frac;
			cur_mid[2] = cur_start[2] + (cur_end[2] - cur_start[2]) * frac;

			//side the front part of the line is on
			side = front < 0;
			//first put the end part of the line on the stack (back side)
			VectorCopy(cur_mid, tstack_p->start);
			//not necessary to store because still on stack
			//VectorCopy(cur_end, tstack_p->end);
			tstack_p->planenum = aasnode->planenum;
			tstack_p->nodenum = aasnode->children[!side];
			tstack_p++;
			if (tstack_p >= &tracestack[127])
			{
				botimport.Print(PRT_ERROR, "AAS_TraceBoundingBox: stack overflow\n");
				return trace;
			} //end if
			//now put the part near the start of the line on the stack so we will
			//continue with that part first. This way we'll find the first
			//hit of the bbox
			VectorCopy(cur_start, tstack_p->start);
			VectorCopy(cur_mid, tstack_p->end);
			tstack_p->planenum = tmpplanenum;
			tstack_p->nodenum = aasnode->children[side];
			tstack_p++;
			if (tstack_p >= &tracestack[127]) {
				botimport.Print(PRT_ERROR, "AAS_TraceBoundingBox: stack overflow\n");
				return trace;
			}
		}
	}
}

int AAS_TraceAreas(vec3_t start, vec3_t end, int *areas, vec3_t *points, int maxareas) {
	int side, nodenum, tmpplanenum;
	int numareas;
	float front, back, frac;
	vec3_t cur_start, cur_end, cur_mid;
	aas_tracestack_t tracestack[127];
	aas_tracestack_t *tstack_p;
	aas_node_t *aasnode;
	aas_plane_t *plane;

	numareas = 0;
	areas[0] = 0;
	if (!aasworld.loaded) return numareas;

	tstack_p = tracestack;
	//we start with the whole line on the stack
	VectorCopy(start, tstack_p->start);
	VectorCopy(end, tstack_p->end);
	tstack_p->planenum = 0;
	//start with node 1 because node zero is a dummy for a solid leaf
	tstack_p->nodenum = 1;		//starting at the root of the tree
	tstack_p++;

	while (1)
	{
		//pop up the stack
		tstack_p--;
		//if the trace stack is empty (ended up with a piece of the
		//line to be traced in an area)
		if (tstack_p < tracestack)
		{
			return numareas;
		} //end if
		//number of the current node to test the line against
		nodenum = tstack_p->nodenum;
		//if it is an area
		if (nodenum < 0)
		{
			//botimport.Print(PRT_MESSAGE, "areanum = %d, must be %d\n", -nodenum, AAS_PointAreaNum(start));
			areas[numareas] = -nodenum;
			if (points) VectorCopy(tstack_p->start, points[numareas]);
			numareas++;
			if (numareas >= maxareas) return numareas;
			continue;
		} //end if
		//if it is a solid leaf
		if (!nodenum)
		{
			continue;
		} //end if
		//the node to test against
		aasnode = &aasworld.nodes[nodenum];
		//start point of current line to test against node
		VectorCopy(tstack_p->start, cur_start);
		//end point of the current line to test against node
		VectorCopy(tstack_p->end, cur_end);
		//the current node plane
		plane = &aasworld.planes[aasnode->planenum];

		switch(plane->type)
		{
			default: //gee it's not an axial plane
			{
				front = DotProduct(cur_start, plane->normal) - plane->dist;
				back = DotProduct(cur_end, plane->normal) - plane->dist;
				break;
			} //end default
		} //end switch

		//if the whole to be traced line is totally at the front of this node
		//only go down the tree with the front child
		if (front > 0 && back > 0)
		{
			//keep the current start and end point on the stack
			//and go down the tree with the front child
			tstack_p->nodenum = aasnode->children[0];
			tstack_p++;
			if (tstack_p >= &tracestack[127])
			{
				botimport.Print(PRT_ERROR, "AAS_TraceAreas: stack overflow\n");
				return numareas;
			} //end if
		} //end if
		//if the whole to be traced line is totally at the back of this node
		//only go down the tree with the back child
		else if (front <= 0 && back <= 0)
		{
			//keep the current start and end point on the stack
			//and go down the tree with the back child
			tstack_p->nodenum = aasnode->children[1];
			tstack_p++;
			if (tstack_p >= &tracestack[127])
			{
				botimport.Print(PRT_ERROR, "AAS_TraceAreas: stack overflow\n");
				return numareas;
			} //end if
		} //end if
		//go down the tree both at the front and back of the node
		else
		{
			tmpplanenum = tstack_p->planenum;
			//calculate the hitpoint with the node (split point of the line)
			//put the crosspoint TRACEPLANE_EPSILON pixels on the near side
			if (front < 0) frac = (front)/(front-back);
			else frac = (front)/(front-back);
			if (frac < 0) frac = 0;
			else if (frac > 1) frac = 1;
			//frac = front / (front-back);
			//
			cur_mid[0] = cur_start[0] + (cur_end[0] - cur_start[0]) * frac;
			cur_mid[1] = cur_start[1] + (cur_end[1] - cur_start[1]) * frac;
			cur_mid[2] = cur_start[2] + (cur_end[2] - cur_start[2]) * frac;

//			AAS_DrawPlaneCross(cur_mid, plane->normal, plane->dist, plane->type, LINECOLOR_RED);
			//side the front part of the line is on
			side = front < 0;
			//first put the end part of the line on the stack (back side)
			VectorCopy(cur_mid, tstack_p->start);
			//not necessary to store because still on stack
			//VectorCopy(cur_end, tstack_p->end);
			tstack_p->planenum = aasnode->planenum;
			tstack_p->nodenum = aasnode->children[!side];
			tstack_p++;
			if (tstack_p >= &tracestack[127])
			{
				botimport.Print(PRT_ERROR, "AAS_TraceAreas: stack overflow\n");
				return numareas;
			} //end if
			//now put the part near the start of the line on the stack so we will
			//continue with that part first. This way we'll find the first
			//hit of the bbox
			VectorCopy(cur_start, tstack_p->start);
			VectorCopy(cur_mid, tstack_p->end);
			tstack_p->planenum = tmpplanenum;
			tstack_p->nodenum = aasnode->children[side];
			tstack_p++;
			if (tstack_p >= &tracestack[127]) {
				botimport.Print(PRT_ERROR, "AAS_TraceAreas: stack overflow\n");
				return numareas;
			}
		}
	}
}

qboolean AAS_PointInsideFace(int facenum, vec3_t point, float epsilon) {
	int i, firstvertex, edgenum;
	vec_t *v1, *v2;
	vec3_t edgevec, pointvec, sepnormal;
	aas_edge_t *edge;
	aas_plane_t *plane;
	aas_face_t *face;

	if (!aasworld.loaded) return qfalse;

	face = &aasworld.faces[facenum];
	plane = &aasworld.planes[face->planenum];
	//
	for (i = 0; i < face->numedges; i++)
	{
		edgenum = aasworld.edgeindex[face->firstedge + i];
		edge = &aasworld.edges[abs(edgenum)];
		//get the first vertex of the edge
		firstvertex = edgenum < 0;
		v1 = aasworld.vertexes[edge->v[firstvertex]];
		v2 = aasworld.vertexes[edge->v[!firstvertex]];
		//edge vector
		VectorSubtract(v2, v1, edgevec);
		//vector from first edge point to point possible in face
		VectorSubtract(point, v1, pointvec);
		//
		CrossProduct(edgevec, plane->normal, sepnormal);
		//
		if (DotProduct(pointvec, sepnormal) < -epsilon) return qfalse;
	} //end for
	return qtrue;
}

aas_plane_t *AAS_PlaneFromNum(int planenum) {
	if (!aasworld.loaded) return NULL;

	return &aasworld.planes[planenum];
}
