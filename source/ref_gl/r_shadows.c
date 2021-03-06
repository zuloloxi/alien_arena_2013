/*
Copyright (C) 2009 COR Entertainment, LLC

This program is free software; you can redistribute it and/or
modify it under the terms of the GNU General Public License
as published by the Free Software Foundation; either version 2
of the License, or (at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.

See the GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program; if not, write to the Free Software
Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA  02111-1307, USA.

Notes on shadow rendering:

CRX uses a combination of shadowmapping and stencil volumes for it's shadowing 
effects.  The shadow maps are used for dynamic shadows only, while the stencil 
volumes are used for static items and vegetation.  There are a couple of compelling 
reaons why we do this.  First, the goal is speed.  It is slightly faster to create 
the volumes than it is to render the mesh to create a shadow caster.  Another 
consideration is that the stencil volumes cast on any object without the need for 
a shader to be present.  We are able to create a soft shadow effect by blitting the
stencil buffer to an offscreen FBO, blurring it in a GLSL shader, then rendering the
image on a single quad overlay with the correct blending.  

The drawbacks are that we do not have fading penumbras(this could change depending on 
what the idtech 4 shadow code looks like).  

Dynamic shadows are handled via shadowmaps since dynamic lighting is handled using GLSL.
This allows us to easily create fading penumbras with dynamic lights.  While it is 
slightly slower, it allows for an excellent, and accurate effect.

*/

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "r_local.h"
#include "r_ragdoll.h"

/*
===============
SHADOW VOLUMES
===============
*/

glStencilFuncSeparatePROC			qglStencilFuncSeparate		= NULL;
glStencilOpSeparatePROC				qglStencilOpSeparate		= NULL;
glStencilMaskSeparatePROC			qglStencilMaskSeparate		= NULL;

extern glStencilFuncSeparatePROC	qglStencilFuncSeparate;
extern glStencilOpSeparatePROC		qglStencilOpSeparate;
extern glStencilMaskSeparatePROC	qglStencilMaskSeparate;

extern void R_ShadowBlend(float alpha);

vec3_t ShadowArray[MAX_SHADOW_VERTS];
static qboolean	triangleFacingLight	[MAX_INDICES / 3];

static vec3_t shadow_lerped[MAX_VERTS];

void R_InitShadowSubsystem(void)
{	
	// openGL 2.0 Unified Separate Stencil
	gl_state.stencil_wrap = false;
	if (strstr(gl_config.extensions_string, "GL_EXT_stencil_wrap"))
	{
		Com_Printf("...using GL_EXT_stencil_wrap\n");
		gl_state.stencil_wrap = true;
	} else
	{
		Com_Printf("...GL_EXT_stencil_wrap not found\n");
		gl_state.stencil_wrap = false;
	}

	qglStencilFuncSeparate		= (void *)qwglGetProcAddress("glStencilFuncSeparate");
	qglStencilOpSeparate		= (void *)qwglGetProcAddress("glStencilOpSeparate");
	qglStencilMaskSeparate		= (void *)qwglGetProcAddress("glStencilMaskSeparate");

	gl_state.separateStencil = false;
	if(qglStencilFuncSeparate && qglStencilOpSeparate && qglStencilMaskSeparate)
	{
			Com_Printf("...using GL_EXT_stencil_two_side\n");
			gl_state.separateStencil = true;

	}
	else
		Com_Printf("...GL_EXT_stencil_two_side not found\n");
}

void SHD_LerpVerts(int nverts, dtrivertx_t *v, dtrivertx_t *ov, float *lerp, float move[3], float frontv[3], float backv[3])
{
    int i;

    for (i = 0; i < nverts; i++, v++, ov++, lerp += 3) {
        lerp[0] = move[0] + ov->v[0]*backv[0] + v->v[0]*frontv[0];
        lerp[1] = move[1] + ov->v[1]*backv[1] + v->v[1]*frontv[1];
        lerp[2] = move[2] + ov->v[2]*backv[2] + v->v[2]*frontv[2];
    }
}

void SHD_MarkShadowTriangles(dmdl_t *paliashdr, dtriangle_t *tris, vec3_t lightOrg, qboolean lerp)
{
	vec3_t	r_triangleNormals[MAX_INDICES / 3];
	vec3_t	temp, dir0, dir1;
	int		i;
	float	f;
	float	*v0, *v1, *v2;

	for (i = 0; i < paliashdr->num_tris; i++, tris++)
	{
		v0 = (float*)shadow_lerped[tris->index_xyz[0]];
		v1 = (float*)shadow_lerped[tris->index_xyz[1]];
		v2 = (float*)shadow_lerped[tris->index_xyz[2]];

		//Calculate shadow volume triangle normals
		VectorSubtract( v0, v1, dir0 );
		VectorSubtract( v2, v1, dir1 );

		CrossProduct( dir0, dir1, r_triangleNormals[i] );

		// Find front facing triangles
		VectorSubtract(lightOrg, v0, temp);
		f = DotProduct(temp, r_triangleNormals[i]);

		triangleFacingLight[i] = f > 0;
	}
}

void SHD_BuildShadowVolume(dmdl_t * hdr, vec3_t light, float projectdistance, qboolean lerp)
{
	dtriangle_t *ot, *tris;
	neighbors_t *neighbors;
	int i, j, shadow_vert = 0, index = 0;
	unsigned	ShadowIndex[MAX_INDICES];
	vec3_t v0, v1, v2, v3;
	vec3_t currentShadowLight;

	daliasframe_t *frame;
	dtrivertx_t *verts;

	frame = (daliasframe_t *) ((byte *) hdr + hdr->ofs_frames
							   + currententity->frame * hdr->framesize);
	verts = frame->verts;

	ot = tris = (dtriangle_t *) ((unsigned char *) hdr + hdr->ofs_tris);

	SHD_MarkShadowTriangles(hdr, tris, light, lerp);

	VectorCopy(light, currentShadowLight);

	for (i = 0, tris = ot, neighbors = currentmodel->neighbors;
		 i < hdr->num_tris; i++, tris++, neighbors++)
	{
		if (!triangleFacingLight[i])
			continue;

		if (neighbors->n[0] < 0 || !triangleFacingLight[neighbors->n[0]])
		{
			for (j = 0; j < 3; j++)
			{
				v0[j] = shadow_lerped[tris->index_xyz[1]][j];
				v1[j] = shadow_lerped[tris->index_xyz[0]][j];

				v2[j] = v1[j] + ((v1[j] - light[j]) * projectdistance);
				v3[j] = v0[j] + ((v0[j] - light[j]) * projectdistance);

			}

			VA_SetElem3(ShadowArray[shadow_vert+0], v0[0], v0[1], v0[2]);
			VA_SetElem3(ShadowArray[shadow_vert+1], v1[0], v1[1], v1[2]);
			VA_SetElem3(ShadowArray[shadow_vert+2], v2[0], v2[1], v2[2]);
			VA_SetElem3(ShadowArray[shadow_vert+3], v3[0], v3[1], v3[2]);


			ShadowIndex[index++] = shadow_vert+0;
			ShadowIndex[index++] = shadow_vert+1;
			ShadowIndex[index++] = shadow_vert+3;
			ShadowIndex[index++] = shadow_vert+3;
			ShadowIndex[index++] = shadow_vert+1;
			ShadowIndex[index++] = shadow_vert+2;
			shadow_vert +=4;
		}

		if (neighbors->n[1] < 0 || !triangleFacingLight[neighbors->n[1]])
		{
			for (j = 0; j < 3; j++)
			{
				v0[j] = shadow_lerped[tris->index_xyz[2]][j];
				v1[j] = shadow_lerped[tris->index_xyz[1]][j];

				v2[j] = v1[j] + ((v1[j] - light[j]) * projectdistance);
				v3[j] = v0[j] + ((v0[j] - light[j]) * projectdistance);
			}

			VA_SetElem3(ShadowArray[shadow_vert+0], v0[0], v0[1], v0[2]);
			VA_SetElem3(ShadowArray[shadow_vert+1], v1[0], v1[1], v1[2]);
			VA_SetElem3(ShadowArray[shadow_vert+2], v2[0], v2[1], v2[2]);
			VA_SetElem3(ShadowArray[shadow_vert+3], v3[0], v3[1], v3[2]);


			ShadowIndex[index++] = shadow_vert+0;
			ShadowIndex[index++] = shadow_vert+1;
			ShadowIndex[index++] = shadow_vert+3;
			ShadowIndex[index++] = shadow_vert+3;
			ShadowIndex[index++] = shadow_vert+1;
			ShadowIndex[index++] = shadow_vert+2;
			shadow_vert +=4;
		}

		if (neighbors->n[2] < 0 || !triangleFacingLight[neighbors->n[2]])
		{
			for (j = 0; j < 3; j++)
			{
				v0[j] = shadow_lerped[tris->index_xyz[0]][j];
				v1[j] = shadow_lerped[tris->index_xyz[2]][j];

				v2[j] = v1[j] + ((v1[j] - light[j]) * projectdistance);
				v3[j] = v0[j] + ((v0[j] - light[j]) * projectdistance);
			}


			VA_SetElem3(ShadowArray[shadow_vert+0], v0[0], v0[1], v0[2]);
			VA_SetElem3(ShadowArray[shadow_vert+1], v1[0], v1[1], v1[2]);
			VA_SetElem3(ShadowArray[shadow_vert+2], v2[0], v2[1], v2[2]);
			VA_SetElem3(ShadowArray[shadow_vert+3], v3[0], v3[1], v3[2]);


			ShadowIndex[index++] = shadow_vert+0;
			ShadowIndex[index++] = shadow_vert+1;
			ShadowIndex[index++] = shadow_vert+3;
			ShadowIndex[index++] = shadow_vert+3;
			ShadowIndex[index++] = shadow_vert+1;
			ShadowIndex[index++] = shadow_vert+2;
			shadow_vert +=4;
		}
	}

	 // triangle is frontface and therefore casts shadow,
     // output front and back caps for shadow volume front cap

	for (i = 0, tris = ot; i < hdr->num_tris; i++, tris++)
	{
		if (!triangleFacingLight[i])
			continue;

		for (j = 0; j < 3; j++)
		{
			v0[j] = shadow_lerped[tris->index_xyz[0]][j];
			v1[j] = shadow_lerped[tris->index_xyz[1]][j];
			v2[j] = shadow_lerped[tris->index_xyz[2]][j];
		}
		VA_SetElem3(ShadowArray[shadow_vert+0], v0[0], v0[1], v0[2]);
		VA_SetElem3(ShadowArray[shadow_vert+1], v1[0], v1[1], v1[2]);
		VA_SetElem3(ShadowArray[shadow_vert+2], v2[0], v2[1], v2[2]);

		ShadowIndex[index++] = shadow_vert+0;
		ShadowIndex[index++] = shadow_vert+1;
		ShadowIndex[index++] = shadow_vert+2;
		shadow_vert +=3;

		// rear cap (with flipped winding order)
		for (j = 0; j < 3; j++)
		{
			v0[j] = shadow_lerped[tris->index_xyz[0]][j];
			v1[j] = shadow_lerped[tris->index_xyz[1]][j];
			v2[j] = shadow_lerped[tris->index_xyz[2]][j];

			v0[j] = v0[j] + ((v0[j] - light[j]) * projectdistance);
			v1[j] = v1[j] + ((v1[j] - light[j]) * projectdistance);
			v2[j] = v2[j] + ((v2[j] - light[j]) * projectdistance);
		}

		VA_SetElem3(ShadowArray[shadow_vert+0], v0[0], v0[1], v0[2]);
		VA_SetElem3(ShadowArray[shadow_vert+1], v1[0], v1[1], v1[2]);
		VA_SetElem3(ShadowArray[shadow_vert+2], v2[0], v2[1], v2[2]);

		ShadowIndex[index++] = shadow_vert+2;
		ShadowIndex[index++] = shadow_vert+1;
		ShadowIndex[index++] = shadow_vert+0;
		shadow_vert +=3;
	}

	if(shadow_vert > 0)
	{
		if ( qglLockArraysEXT != 0 )
		   qglLockArraysEXT( 0, shadow_vert );

		qglDrawElements(GL_TRIANGLES, index, GL_UNSIGNED_INT, ShadowIndex);

		if ( qglUnlockArraysEXT != 0 )
			 qglUnlockArraysEXT();
	}
}

void SHD_RenderVolumes(dmdl_t * paliashdr, vec3_t lightdir, int projdist, qboolean lerp)
{
	int incr = gl_state.stencil_wrap ? GL_INCR_WRAP_EXT : GL_INCR;
	int decr = gl_state.stencil_wrap ? GL_DECR_WRAP_EXT : GL_DECR;

	if(VectorCompare(lightdir, vec3_origin))
		return;
	
	if(gl_state.separateStencil)
	{
		qglDisable(GL_CULL_FACE);

		qglStencilOpSeparate(GL_BACK, GL_KEEP,  incr, GL_KEEP);
		qglStencilOpSeparate(GL_FRONT, GL_KEEP, decr, GL_KEEP);

		SHD_BuildShadowVolume(paliashdr, lightdir, projdist, lerp);

		qglEnable(GL_CULL_FACE);
	}
	else
	{
		qglEnable(GL_CULL_FACE);

		qglCullFace(GL_BACK);
		qglStencilOp(GL_KEEP, incr, GL_KEEP);
		SHD_BuildShadowVolume(paliashdr, lightdir, projdist, lerp);

		qglCullFace(GL_FRONT);
		qglStencilOp(GL_KEEP, decr, GL_KEEP);
		SHD_BuildShadowVolume(paliashdr, lightdir, projdist, lerp);
	}
}

void SHD_DrawAliasShadowVolume(dmdl_t * paliashdr, qboolean lerp)
{
	vec3_t light, temp, tempOrg;
	int i, j, o;
	float cost, sint;
	float is, it, dist;
	int worldlight = 0;
	float numlights, weight;
	float bob, project;
	trace_t	r_trace;
	vec3_t mins, maxs, lightAdd;

	VectorSet(mins, 0, 0, 0);
	VectorSet(maxs, 0, 0, 0);

	if(currententity->flags & RF_BOBBING)
		bob = currententity->bob;
	else
		bob = 0;

	VectorCopy(currententity->origin, tempOrg);
	tempOrg[2] -= bob;

	VectorClear(light);

	cost =  cos(-currententity->angles[1] / 180 * M_PI),
    sint =  sin(-currententity->angles[1] / 180 * M_PI);

	numlights = 0;
	VectorClear(lightAdd);
	for (i=0; i<r_lightgroups; i++)
	{
		if(LightGroups[i].group_origin[2] < currententity->origin[2] - bob)
			continue; //don't bother with world lights below the ent, creates undesirable shadows

		//need a trace(not for self model, too jerky when lights are blocked and reappear)
		if(!(currententity->flags & RF_VIEWERMODEL))
		{
			r_trace = CM_BoxTrace(tempOrg, LightGroups[i].group_origin, mins, maxs, r_worldmodel->firstnode, MASK_OPAQUE);
			if(r_trace.fraction != 1.0)
				continue;
		}

		VectorSubtract(LightGroups[i].group_origin, currententity->origin, temp);

		dist = VectorLength(temp);

		//accum and weight
		weight = (int)250000/(dist/(LightGroups[i].avg_intensity+1.0f));
		for(j = 0; j < 3; j++)
			lightAdd[j] += LightGroups[i].group_origin[j]*weight;
		numlights+=weight;

		if(numlights > 0.0)
		{
			for(o = 0; o < 3; o++)
				light[o] = -currententity->origin[o] + lightAdd[o]/numlights;

			is = light[0], it = light[1];
			light[0] = (cost * (is - 0) + sint * (0 - it) + 0);
			light[1] = (cost * (it - 0) + sint * (is - 0) + 0);
			light[2] += currententity->model->maxs[2] + 56;
		}

		worldlight++;
	}

	if(!worldlight)
	{ //no lights found, create light straight down

		VectorSet(light, 0, 0, 200);

		is = light[0], it = light[1];
		light[0] = (cost * (is - 0) + sint * (0 - it) + 0);
		light[1] = (cost * (it - 0) + sint * (is - 0) + 0);
		light[2] += 8;
	}

	if(currentmodel->maxs[2] - currentmodel->mins[2] > 200)
		project = 2.0;
	else
		project = 1.5;

	SHD_RenderVolumes(paliashdr, light, project, lerp);
}

void SHD_DrawShadowVolume()
{
	dmdl_t *paliashdr=NULL;
    vec3_t move, delta, vectors[3];
    vec3_t frontv, backv;
    int i;
	qboolean lerped=false;

	if(currentmodel->type == mod_md2)
	{
		daliasframe_t *frame, *oldframe;
		dtrivertx_t *v, *ov, *verts;
		float   *lerp;
		float frontlerp;

		paliashdr = (dmdl_t *) currentmodel->extradata;

		if ( (currententity->frame >= paliashdr->num_frames)
			|| (currententity->frame < 0) )	{

			currententity->frame = 0;
			currententity->oldframe = 0;
		}

		if(currententity->frame == 0 && currentmodel->num_frames == 1)
			lerped = false;
		else
			lerped = true;

		if(lerped)
			frame = (daliasframe_t *)((byte *)paliashdr + paliashdr->ofs_frames
				+ currententity->frame * paliashdr->framesize);
		else
			frame = (daliasframe_t *)((byte *)paliashdr + paliashdr->ofs_frames);
		verts = v = frame->verts;

		if(lerped)
		{
			if ( (currententity->oldframe >= paliashdr->num_frames)
				|| (currententity->oldframe < 0)) {

				currententity->frame = 0;
				currententity->oldframe = 0;
			}

			oldframe = (daliasframe_t *)((byte *)paliashdr + paliashdr->ofs_frames
				+ currententity->oldframe * paliashdr->framesize);
			ov = oldframe->verts;

			if ( !r_lerpmodels->value )
				currententity->backlerp = 0;

			frontlerp = 1.0 - currententity->backlerp;

			// move should be the delta back to the previous frame * backlerp
			VectorSubtract(currententity->oldorigin, currententity->origin, delta);
			AngleVectors(currententity->angles, vectors[0], vectors[1],
						 vectors[2]);

			move[0] = DotProduct(delta, vectors[0]);	// forward
			move[1] = -DotProduct(delta, vectors[1]);	// left
			move[2] = DotProduct(delta, vectors[2]);	// up

			if(oldframe && oldframe->translate)
				VectorAdd(move, oldframe->translate, move);

			for (i = 0; i < 3; i++) {
				move[i] =
					currententity->backlerp * move[i] +
					frontlerp * frame->translate[i];
				frontv[i] = frontlerp * frame->scale[i];
				backv[i] = currententity->backlerp * oldframe->scale[i];
			}

			lerp = shadow_lerped[0];

			SHD_LerpVerts(paliashdr->num_xyz, v, ov, lerp, move, frontv, backv);
		}
		else
		{
			for (i = 0; i < paliashdr->num_xyz; i++)
			{
				int j;
				for (j = 0; j < 3; j++)
					shadow_lerped[i][j] = frame->translate[j] + frame->scale[j] * v[i].v[j];
			}
		}
	}

	qglPushMatrix();
	qglDisable(GL_TEXTURE_2D);
	qglTranslatef(currententity->origin[0], currententity->origin[1], currententity->origin[2]);
	qglRotatef(currententity->angles[1], 0, 0, 1);

	if(currentmodel->type == mod_md2)
		SHD_DrawAliasShadowVolume(paliashdr, lerped);
	
	qglEnable(GL_TEXTURE_2D);
	qglPopMatrix();
}

#include "r_lodcalc.h"

//to do - all of this will be replaced by shadowmapping
extern cvar_t *cl_simpleitems;
void R_CastShadow(void)
{
	int i;
	vec3_t dist, tmp;
	float rad, thresh;
    trace_t r_trace;

	//note - we use a combination of stencil volumes(for world light shadows) and shadowmaps(for dynamic shadows)
	if (!gl_shadowmaps->value)
		return;

	if (r_newrefdef.rdflags & RDF_NOWORLDMODEL)
		return;

	qglEnableClientState(GL_VERTEX_ARRAY);
	qglVertexPointer(3, GL_FLOAT, sizeof(vec3_t), ShadowArray);

	qglClear(GL_STENCIL_BUFFER_BIT);

	qglColorMask(0,0,0,0);
	qglEnable(GL_STENCIL_TEST);

	qglDepthFunc (GL_LESS);
	qglDepthMask(0);

	qglEnable(GL_POLYGON_OFFSET_FILL);
	qglPolygonOffset(0.0f, 1000.0f);

	if(gl_state.separateStencil)
		qglStencilFuncSeparate(GL_FRONT_AND_BACK, GL_ALWAYS, 0x0, 0xFF);
	else
		qglStencilFunc( GL_ALWAYS, 0x0, 0xFF);

	for (i = 0; i < r_newrefdef.num_entities; i++)
	{
		currententity = &r_newrefdef.entities[i];

		if (currententity->flags & (RF_SHELL_ANY | RF_WEAPONMODEL | RF_NOSHADOWS | RF_TRANSLUCENT))
			continue;

		currentmodel = currententity->model;

		if (!currentmodel)
			continue;

		if (currentmodel->type != mod_md2)
			continue;
		
		if (cl_simpleitems->integer && currentmodel->simple_texnum != 0)
			continue;

		if (r_newrefdef.vieworg[2] < (currententity->origin[2] - 128))
			continue;

		VectorSubtract(currententity->model->maxs, currententity->model->mins, tmp);
		VectorScale (tmp, 1.666, tmp);
		rad = VectorLength (tmp);

		if( R_CullSphere( currententity->origin, rad, 15 ) )
			continue;

		if (r_worldmodel ) {
			//occulusion culling - why draw shadows of entities we cannot see?
			r_trace = CM_BoxTrace(r_origin, currententity->origin, currentmodel->maxs, currentmodel->mins, r_worldmodel->firstnode, MASK_OPAQUE);
			if(r_trace.fraction != 1.0)
				continue;
		}

		//get distance, set lod if available
		VectorSubtract(r_origin, currententity->origin, dist);

		//keep some of the truly massive items from having bad disappearing shadow artifacts.
		thresh = rad*5;
		if(thresh < 1024)
			thresh = 1024;

		//cull by distance if soft shadows
		if	(	VectorLength(dist) > LOD_DIST*(((float)thresh)/500.0) && 
				gl_state.hasFBOblit && 
				atoi(&gl_config.version_string[0]) >= 3.0
			)
			continue;

		if(VectorLength(dist) > LOD_DIST*2.0) {
			if(currententity->lod2)
				currentmodel = currententity->lod2;
		}
		else if(VectorLength(dist) > LOD_DIST) {
			if(currententity->lod1)
				currentmodel = currententity->lod1;
		}

		SHD_DrawShadowVolume();
	}
	
	qglDisableClientState(GL_VERTEX_ARRAY);
	qglColorMask(1,1,1,1);

	qglDepthMask(1);
	qglDepthFunc(GL_LEQUAL);
	qglDisable(GL_POLYGON_OFFSET_FILL);

	R_ShadowBlend(0.4);
}
