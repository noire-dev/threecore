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
#include "cm_local.h"

/*
===============================================================================

BASIC MATH

===============================================================================
*/

/*
================
RotatePoint
================
*/
static void RotatePoint( vec3_t point, /*const*/ vec3_t matrix[3] ) {
	vec3_t tvec;

	VectorCopy(point, tvec);
	point[0] = DotProduct(matrix[0], tvec);
	point[1] = DotProduct(matrix[1], tvec);
	point[2] = DotProduct(matrix[2], tvec);
}

/*
================
TransposeMatrix
================
*/
static void TransposeMatrix( /*const*/ vec3_t matrix[3], vec3_t transpose[3] ) {
	int i, j;
	for (i = 0; i < 3; i++) {
		for (j = 0; j < 3; j++) {
			transpose[i][j] = matrix[j][i];
		}
	}
}

/*
================
CreateRotationMatrix
================
*/
static void CreateRotationMatrix( const vec3_t angles, vec3_t matrix[3] ) {
	AngleVectors(angles, matrix[0], matrix[1], matrix[2]);
	VectorInverse(matrix[1]);
}

/*
===============================================================================

POSITION TESTING

===============================================================================
*/

/*
================
CM_TestBoxInBrush
================
*/
static void CM_TestBoxInBrush( traceWork_t *tw, const cbrush_t *brush ) {
	int			i;
	cplane_t	*plane;
	double		dist;
	double		d1;
	cbrushside_t	*side;

	if (!brush->numsides) {
		return;
	}

	// special test for axial
	if ( tw->bounds[0][0] > brush->bounds[1][0]
		|| tw->bounds[0][1] > brush->bounds[1][1]
		|| tw->bounds[0][2] > brush->bounds[1][2]
		|| tw->bounds[1][0] < brush->bounds[0][0]
		|| tw->bounds[1][1] < brush->bounds[0][1]
		|| tw->bounds[1][2] < brush->bounds[0][2]
		) {
		return;
	}

	// the first six planes are the axial planes, so we only
	// need to test the remainder
	for ( i = 6 ; i < brush->numsides ; i++ ) {
		side = brush->sides + i;
		plane = side->plane;
		// adjust the plane distance appropriately for mins/maxs
		dist = plane->dist - DotProduct( tw->offsets[ plane->signbits ], plane->normal );
		d1 = DotProductDP( tw->start, plane->normal ) - dist;
		// if completely in front of face, no intersection
		if ( d1 > 0 ) {
			return;
		}
	}

	// inside this brush
	tw->trace.startsolid = tw->trace.allsolid = qtrue;
	tw->trace.fraction = 0;
	tw->trace.contents = brush->contents;
}

/*
================
CM_TestInLeaf
================
*/
static void CM_TestInLeaf( traceWork_t *tw, const cLeaf_t *leaf ) {
	int			k;
	int			brushnum;
	cbrush_t	*b;
	cPatch_t	*patch;

	// test box position against all brushes in the leaf
	for (k=0 ; k<leaf->numLeafBrushes ; k++) {
		brushnum = cm.leafbrushes[leaf->firstLeafBrush+k];
		b = &cm.brushes[brushnum];
		if (b->checkcount == cm.checkcount) {
			continue;	// already checked this brush in another leaf
		}
		b->checkcount = cm.checkcount;

		if ( !(b->contents & tw->contents)) {
			continue;
		}

		CM_TestBoxInBrush( tw, b );
		if ( tw->trace.allsolid ) {
			return;
		}
	}

	// test against all patches
#ifdef BSPC
	if (1) {
#else
	if ( !cm_noCurves->integer ) {
#endif //BSPC
		for ( k = 0 ; k < leaf->numLeafSurfaces ; k++ ) {
			patch = cm.surfaces[ cm.leafsurfaces[ leaf->firstLeafSurface + k ] ];
			if ( !patch ) {
				continue;
			}
			if ( patch->checkcount == cm.checkcount ) {
				continue;	// already checked this brush in another leaf
			}
			patch->checkcount = cm.checkcount;

			if ( !(patch->contents & tw->contents)) {
				continue;
			}

			if ( CM_PositionTestInPatchCollide( tw, patch->pc ) ) {
				tw->trace.startsolid = tw->trace.allsolid = qtrue;
				tw->trace.fraction = 0;
				tw->trace.contents = patch->contents;
				return;
			}
		}
	}
}

/*
==================
CM_PositionTest
==================
*/
#define	MAX_POSITION_LEAFS	1024
static void CM_PositionTest( traceWork_t *tw ) {
	int		leafs[MAX_POSITION_LEAFS];
	int		i;
	leafList_t	ll;

	// identify the leafs we are touching
	VectorAdd( tw->start, tw->size[0], ll.bounds[0] );
	VectorAdd( tw->start, tw->size[1], ll.bounds[1] );

	for (i=0 ; i<3 ; i++) {
		ll.bounds[0][i] -= 1;
		ll.bounds[1][i] += 1;
	}

	ll.count = 0;
	ll.maxcount = MAX_POSITION_LEAFS;
	ll.list = leafs;
	ll.storeLeafs = CM_StoreLeafs;
	ll.lastLeaf = 0;
	ll.overflowed = qfalse;

	cm.checkcount++;

	CM_BoxLeafnums_r( &ll, 0 );


	cm.checkcount++;

	// test the contents of the leafs
	for (i=0 ; i < ll.count ; i++) {
		CM_TestInLeaf( tw, &cm.leafs[leafs[i]] );
		if ( tw->trace.allsolid ) {
			break;
		}
	}
}

/*
===============================================================================

TRACING

===============================================================================
*/

/*
================
CM_TraceThroughPatch
================
*/
static void CM_TraceThroughPatch( traceWork_t *tw, const cPatch_t *patch ) {
	float		oldFrac;

	c_patch_traces++;

	oldFrac = tw->trace.fraction;

	CM_TraceThroughPatchCollide( tw, patch->pc );

	if ( tw->trace.fraction < oldFrac ) {
		tw->trace.surfaceFlags = patch->surfaceFlags;
		tw->trace.contents = patch->contents;
	}
}

/*
================
CM_TraceThroughBrush
================
*/
static void CM_TraceThroughBrush( traceWork_t *tw, const cbrush_t *brush ) {
	int			i;
	cplane_t	*plane, *clipplane;
	double		dist;
	float		enterFrac, leaveFrac;
	double		d1, d2;
	qboolean	getout, startout;
	float		f;
	cbrushside_t	*side, *leadside;

	enterFrac = -1.0;
	leaveFrac = 1.0;
	clipplane = NULL;

	if ( !brush->numsides ) {
		return;
	}

	c_brush_traces++;

	getout = qfalse;
	startout = qfalse;

	leadside = NULL;

	//
	// compare the trace against all planes of the brush
	// find the latest time the trace crosses a plane towards the interior
	// and the earliest time the trace crosses a plane towards the exterior
	//
	for (i = 0; i < brush->numsides; i++) {
		side = brush->sides + i;
		plane = side->plane;
		// adjust the plane distance appropriately for mins/maxs
		dist = plane->dist - DotProductDP( tw->offsets[ plane->signbits ], plane->normal );
		d1 = DotProductDP( tw->start, plane->normal ) - dist;
		d2 = DotProductDP( tw->end, plane->normal ) - dist;
		if (d2 > 0) {
			getout = qtrue;	// endpoint is not in solid
		}
		if (d1 > 0) {
			startout = qtrue;
		}
		// if completely in front of face, no intersection with the entire brush
		if (d1 > 0 && ( d2 >= SURFACE_CLIP_EPSILON || d2 >= d1 )  ) {
			return;
		}
		// if it doesn't cross the plane, the plane isn't relevant
		if (d1 <= 0 && d2 <= 0 ) {
			continue;
		}
		// crosses face
		if (d1 > d2) {	// enter
			f = (d1-SURFACE_CLIP_EPSILON) / (d1-d2);
			if ( f < 0 ) {
				f = 0;
			}
			if (f > enterFrac) {
				enterFrac = f;
				clipplane = plane;
				leadside = side;
			}
		} else {	// leave
			f = (d1+SURFACE_CLIP_EPSILON) / (d1-d2);
			if ( f > 1 ) {
				f = 1;
			}
			if (f < leaveFrac) {
				leaveFrac = f;
			}
		}
	}

	//
	// all planes have been checked, and the trace was not
	// completely outside the brush
	//
	if (!startout) {	// original point was inside brush
		tw->trace.startsolid = qtrue;
		if (!getout) {
			tw->trace.allsolid = qtrue;
			tw->trace.fraction = 0;
			tw->trace.contents = brush->contents;
		}
		return;
	}

	if (enterFrac < leaveFrac) {
		if (enterFrac > -1 && enterFrac < tw->trace.fraction) {
			if (enterFrac < 0) {
				enterFrac = 0;
			}
			tw->trace.fraction = enterFrac;
			if ( clipplane != NULL ) {
				tw->trace.plane = *clipplane;
			}
			if ( leadside != NULL ) {
				tw->trace.surfaceFlags = leadside->surfaceFlags;
			}
			tw->trace.contents = brush->contents;
		}
	}
}

/*
================
CM_TraceThroughLeaf
================
*/
static void CM_TraceThroughLeaf( traceWork_t *tw, const cLeaf_t *leaf ) {
	int			k;
	int			brushnum;
	cbrush_t	*b;
	cPatch_t	*patch;

	// trace line against all brushes in the leaf
	for ( k = 0 ; k < leaf->numLeafBrushes ; k++ ) {
		brushnum = cm.leafbrushes[leaf->firstLeafBrush+k];

		b = &cm.brushes[brushnum];
		if ( b->checkcount == cm.checkcount ) {
			continue;	// already checked this brush in another leaf
		}
		b->checkcount = cm.checkcount;

		if ( !(b->contents & tw->contents) ) {
			continue;
		}

		if ( !CM_BoundsIntersect( tw->bounds[0], tw->bounds[1],
					b->bounds[0], b->bounds[1] ) ) {
			continue;
		}

		CM_TraceThroughBrush( tw, b );
		if ( !tw->trace.fraction ) {
			return;
		}
	}

	// trace line against all patches in the leaf
#ifdef BSPC
	if (1) {
#else
	if ( !cm_noCurves->integer ) {
#endif
		for ( k = 0 ; k < leaf->numLeafSurfaces ; k++ ) {
			patch = cm.surfaces[ cm.leafsurfaces[ leaf->firstLeafSurface + k ] ];
			if ( !patch ) {
				continue;
			}
			if ( patch->checkcount == cm.checkcount ) {
				continue;	// already checked this patch in another leaf
			}
			patch->checkcount = cm.checkcount;

			if ( !(patch->contents & tw->contents) ) {
				continue;
			}

			CM_TraceThroughPatch( tw, patch );
			if ( !tw->trace.fraction ) {
				return;
			}
		}
	}
}

/*
==================
CM_TraceThroughTree

Traverse all the contacted leafs from the start to the end position.
If the trace is a point, they will be exactly in order, but for larger
trace volumes it is possible to hit something in a later leaf with
a smaller intercept fraction.
==================
*/
static void CM_TraceThroughTree( traceWork_t *tw, int num, float p1f, float p2f, const vec3_t p1, const vec3_t p2 ) {
	cNode_t		*node;
	cplane_t	*plane;
	double		t1, t2, offset;
	float		frac, frac2;
	float		idist;
	vec3_t		mid;
	int			side;
	float		midf;

	if (tw->trace.fraction <= p1f) {
		return;		// already hit something nearer
	}

	// if < 0, we are in a leaf node
	if (num < 0) {
		CM_TraceThroughLeaf( tw, &cm.leafs[-1-num] );
		return;
	}

	//
	// find the point distances to the separating plane
	// and the offset for the size of the box
	//
	node = cm.nodes + num;
	plane = node->plane;

	// adjust the plane distance appropriately for mins/maxs
	if ( plane->type < 3 ) {
		t1 = p1[plane->type] - plane->dist;
		t2 = p2[plane->type] - plane->dist;
		offset = tw->extents[plane->type];
	} else {
		t1 = DotProductDP( plane->normal, p1 ) - plane->dist;
		t2 = DotProductDP( plane->normal, p2 ) - plane->dist;
		if ( tw->isPoint ) {
			offset = 0;
		} else {
			// this is silly
			offset = 2048;
		}
	}

	// see which sides we need to consider
	if ( t1 >= offset + 1 && t2 >= offset + 1 ) {
		CM_TraceThroughTree( tw, node->children[0], p1f, p2f, p1, p2 );
		return;
	}
	if ( t1 < -offset - 1 && t2 < -offset - 1 ) {
		CM_TraceThroughTree( tw, node->children[1], p1f, p2f, p1, p2 );
		return;
	}

	// put the crosspoint SURFACE_CLIP_EPSILON pixels on the near side
	if ( t1 < t2 ) {
		idist = 1.0/(t1-t2);
		side = 1;
		frac2 = (t1 + offset + SURFACE_CLIP_EPSILON)*idist;
		frac = (t1 - offset + SURFACE_CLIP_EPSILON)*idist;
	} else if (t1 > t2) {
		idist = 1.0/(t1-t2);
		side = 0;
		frac2 = (t1 - offset - SURFACE_CLIP_EPSILON)*idist;
		frac = (t1 + offset + SURFACE_CLIP_EPSILON)*idist;
	} else {
		side = 0;
		frac = 1;
		frac2 = 0;
	}

	// move up to the node
	if ( frac < 0 ) {
		frac = 0;
	} else if ( frac > 1 ) {
		frac = 1;
	}

	midf = p1f + (p2f - p1f)*frac;

	mid[0] = p1[0] + frac*(p2[0] - p1[0]);
	mid[1] = p1[1] + frac*(p2[1] - p1[1]);
	mid[2] = p1[2] + frac*(p2[2] - p1[2]);

	CM_TraceThroughTree( tw, node->children[side], p1f, midf, p1, mid );

	// go past the node
	if ( frac2 < 0 ) {
		frac2 = 0;
	} else if ( frac2 > 1 ) {
		frac2 = 1;
	}

	midf = p1f + (p2f - p1f)*frac2;

	mid[0] = p1[0] + frac2*(p2[0] - p1[0]);
	mid[1] = p1[1] + frac2*(p2[1] - p1[1]);
	mid[2] = p1[2] + frac2*(p2[2] - p1[2]);

	CM_TraceThroughTree( tw, node->children[side^1], midf, p2f, mid, p2 );
}

/*
==================
CM_Trace
==================
*/
void CM_Trace( trace_t *results, const vec3_t start, const vec3_t end, const vec3_t mins, const vec3_t maxs, clipHandle_t model, const vec3_t origin, int brushmask ) {
	int			i;
	traceWork_t	tw;
	vec3_t		offset;
	cmodel_t	*cmod;

	cmod = CM_ClipHandleToModel( model );

	cm.checkcount++;		// for multi-check avoidance

	c_traces++;				// for statistics, may be zeroed

	// fill in a default trace
	Com_Memset( &tw, 0, sizeof(tw) );
	tw.trace.fraction = 1;	// assume it goes the entire distance until shown otherwise
	VectorCopy(origin, tw.modelOrigin);

	if (!cm.numNodes) {
		*results = tw.trace;

		return;	// map not loaded, shouldn't happen
	}

	// allow NULL to be passed in for 0,0,0
	if ( !mins ) {
		mins = vec3_origin;
	}
	if ( !maxs ) {
		maxs = vec3_origin;
	}

	// set basic parms
	tw.contents = brushmask;

	// adjust so that mins and maxs are always symmetric, which
	// avoids some complications with plane expanding of rotated
	// bmodels
	for ( i = 0 ; i < 3 ; i++ ) {
		offset[i] = ( mins[i] + maxs[i] ) * 0.5;
		tw.size[0][i] = mins[i] - offset[i];
		tw.size[1][i] = maxs[i] - offset[i];
		tw.start[i] = start[i] + offset[i];
		tw.end[i] = end[i] + offset[i];
	}

	tw.maxOffset = tw.size[1][0] + tw.size[1][1] + tw.size[1][2];

	// tw.offsets[signbits] = vector to appropriate corner from origin
	tw.offsets[0][0] = tw.size[0][0];
	tw.offsets[0][1] = tw.size[0][1];
	tw.offsets[0][2] = tw.size[0][2];

	tw.offsets[1][0] = tw.size[1][0];
	tw.offsets[1][1] = tw.size[0][1];
	tw.offsets[1][2] = tw.size[0][2];

	tw.offsets[2][0] = tw.size[0][0];
	tw.offsets[2][1] = tw.size[1][1];
	tw.offsets[2][2] = tw.size[0][2];

	tw.offsets[3][0] = tw.size[1][0];
	tw.offsets[3][1] = tw.size[1][1];
	tw.offsets[3][2] = tw.size[0][2];

	tw.offsets[4][0] = tw.size[0][0];
	tw.offsets[4][1] = tw.size[0][1];
	tw.offsets[4][2] = tw.size[1][2];

	tw.offsets[5][0] = tw.size[1][0];
	tw.offsets[5][1] = tw.size[0][1];
	tw.offsets[5][2] = tw.size[1][2];

	tw.offsets[6][0] = tw.size[0][0];
	tw.offsets[6][1] = tw.size[1][1];
	tw.offsets[6][2] = tw.size[1][2];

	tw.offsets[7][0] = tw.size[1][0];
	tw.offsets[7][1] = tw.size[1][1];
	tw.offsets[7][2] = tw.size[1][2];

	//
	// calculate bounds
	//
	for ( i = 0 ; i < 3 ; i++ ) {
		if ( tw.start[i] < tw.end[i] ) {
			tw.bounds[0][i] = tw.start[i] + tw.size[0][i];
			tw.bounds[1][i] = tw.end[i] + tw.size[1][i];
		} else {
			tw.bounds[0][i] = tw.end[i] + tw.size[0][i];
			tw.bounds[1][i] = tw.start[i] + tw.size[1][i];
		}
	}

	//
	// check for position test special case
	//
	if (start[0] == end[0] && start[1] == end[1] && start[2] == end[2]) {
		if (model) {
			CM_TestInLeaf(&tw, &cmod->leaf);
		} else {
			CM_PositionTest(&tw);
		}
	} else {
		//
		// check for point special case
		//
		if ( tw.size[0][0] == 0 && tw.size[0][1] == 0 && tw.size[0][2] == 0 ) {
			tw.isPoint = qtrue;
			VectorClear( tw.extents );
		} else {
			tw.isPoint = qfalse;
			tw.extents[0] = tw.size[1][0];
			tw.extents[1] = tw.size[1][1];
			tw.extents[2] = tw.size[1][2];
		}

		//
		// general sweeping through world
		//
		if (model) {
			CM_TraceThroughLeaf(&tw, &cmod->leaf);
		} else {
			CM_TraceThroughTree(&tw, 0, 0, 1, tw.start, tw.end);
		}
	}

	// generate endpos from the original, unmodified start/end
	if ( tw.trace.fraction == 1 ) {
		VectorCopy (end, tw.trace.endpos);
	} else {
		for ( i=0 ; i<3 ; i++ ) {
			tw.trace.endpos[i] = start[i] + tw.trace.fraction * (end[i] - start[i]);
		}
	}

        // If allsolid is set (was entirely inside something solid), the plane is not valid.
        // If fraction == 1.0, we never hit anything, and thus the plane is not valid.
        // Otherwise, the normal on the plane should have unit length
        assert(tw.trace.allsolid ||
               tw.trace.fraction == 1.0 ||
               VectorLengthSquared(tw.trace.plane.normal) > 0.9999);
	*results = tw.trace;
}

/*
==================
CM_BoxTrace
==================
*/
void CM_BoxTrace( trace_t *results, const vec3_t start, const vec3_t end,
						const vec3_t mins, const vec3_t maxs,
						clipHandle_t model, int brushmask ) {
	CM_Trace( results, start, end, mins, maxs, model, vec3_origin, brushmask );
}

/*
==================
CM_TransformedBoxTrace

Handles offsetting and rotation of the end points for moving and
rotating entities
==================
*/
void CM_TransformedBoxTrace( trace_t *results, const vec3_t start, const vec3_t end,
						const vec3_t mins, const vec3_t maxs,
						clipHandle_t model, int brushmask,
						const vec3_t origin, const vec3_t angles ) {
	trace_t		trace;
	vec3_t		start_l, end_l;
	qboolean	rotated;
	vec3_t		offset;
	vec3_t		symetricSize[2];
	vec3_t		matrix[3], transpose[3];
	int			i;

	if ( !mins ) {
		mins = vec3_origin;
	}
	if ( !maxs ) {
		maxs = vec3_origin;
	}

	// adjust so that mins and maxs are always symmetric, which
	// avoids some complications with plane expanding of rotated
	// bmodels
	for ( i = 0 ; i < 3 ; i++ ) {
		offset[i] = ( mins[i] + maxs[i] ) * 0.5;
		symetricSize[0][i] = mins[i] - offset[i];
		symetricSize[1][i] = maxs[i] - offset[i];
		start_l[i] = start[i] + offset[i];
		end_l[i] = end[i] + offset[i];
	}

	// subtract origin offset
	VectorSubtract( start_l, origin, start_l );
	VectorSubtract( end_l, origin, end_l );

	// rotate start and end into the models frame of reference
	if ( angles[0] || angles[1] || angles[2] ) {
		rotated = qtrue;
	} else {
		rotated = qfalse;
	}

	if (rotated) {
		CreateRotationMatrix(angles, matrix);
		RotatePoint(start_l, matrix);
		RotatePoint(end_l, matrix);
	}

	// sweep the box through the model
	CM_Trace( &trace, start_l, end_l, symetricSize[0], symetricSize[1], model, origin, brushmask );

	// if the bmodel was rotated and there was a collision
	if ( rotated && trace.fraction != 1.0 ) {
		// rotation of bmodel collision plane
		TransposeMatrix(matrix, transpose);
		RotatePoint(trace.plane.normal, transpose);
	}

	// re-calculate the end position of the trace because the trace.endpos
	// calculated by CM_Trace could be rotated and have an offset
	trace.endpos[0] = start[0] + trace.fraction * (end[0] - start[0]);
	trace.endpos[1] = start[1] + trace.fraction * (end[1] - start[1]);
	trace.endpos[2] = start[2] + trace.fraction * (end[2] - start[2]);

	*results = trace;
}
