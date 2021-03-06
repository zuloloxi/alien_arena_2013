/*
Copyright (C) 1997-2001 Id Software, Inc.
Copyright (C) 2011 COR Entertainment, LLC.

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

*/
// R_SURF.C: surface-related refresh code

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <assert.h>

#include "r_local.h"

static vec3_t	modelorg;		// relative to viewpoint

vec3_t	r_worldLightVec;
dlight_t *dynLight;

#define LIGHTMAP_BYTES 4

//Pretty safe bet most cards support this
#define	LIGHTMAP_SIZE	2048 
#define	MAX_LIGHTMAPS	12

int		c_visible_lightmaps;
int		c_visible_textures;

// This is supposed to be faster on some older hardware.
#define GL_LIGHTMAP_FORMAT GL_BGRA 

typedef struct
{
	int	current_lightmap_texture;

	// For each column, what is the last row where a pixel is used
	int			allocated[LIGHTMAP_SIZE];

	// Lightmap texture data (RGBA, alpha not used)
	byte		lightmap_buffer[4*LIGHTMAP_SIZE*LIGHTMAP_SIZE];
} gllightmapstate_t;

// TODO: dynamically allocate this so we can free it for RAM savings? It's
// using over 16 megs. 
static gllightmapstate_t gl_lms; 

static void		LM_InitBlock( void );
static void		LM_UploadBlock( );
static qboolean	LM_AllocBlock (int w, int h, int *x, int *y);

extern void R_SetCacheState( msurface_t *surf );
extern void R_BuildLightMap (msurface_t *surf, byte *dest, int smax, int tmax, int stride);

/*
===============
BSP_TextureAnimation

Returns the proper texture for a given time and base texture
XXX: AFAIK this is only used for the old .wal textures, and is a bit redundant
with the rscript system, although it is implemented more efficiently. Maybe 
merge the two systems somehow?
===============
*/
image_t *BSP_TextureAnimation (mtexinfo_t *tex)
{
	int		c;

	if (!tex->next)
		return tex->image;

	c = currententity->frame % tex->numframes;
	while (c)
	{
		tex = tex->next;
		c--;
	}

	return tex->image;
}



/*
=========================================

Textureless Surface Rendering
Used by the shadow system

=========================================
*/

/*
================
BSP_DrawTexturelessPoly
================
*/
void BSP_DrawTexturelessPoly (msurface_t *fa)
{
	R_InitVArrays(VERT_NO_TEXTURE);
	R_AddSurfToVArray (fa);
	R_KillVArrays();
}

void BSP_DrawShadowPoly (msurface_t *fa, vec3_t origin)
{
	R_AddShadowSurfToVArray (fa, origin);
}

void BSP_DrawTexturelessInlineBModel (entity_t *e)
{
	int			i;
	msurface_t	*psurf;

	//
	// draw texture
	//
	psurf = &currentmodel->surfaces[currentmodel->firstmodelsurface];
	for (i=0 ; i<currentmodel->nummodelsurfaces ; i++, psurf++)
	{

		// draw the polygon
		BSP_DrawTexturelessPoly( psurf );
		psurf->visframe = r_framecount;
	}

	qglDisable (GL_BLEND);
	qglColor4f (1,1,1,1);
	GL_TexEnv( GL_REPLACE );
}

void BSP_DrawTexturelessBrushModel (entity_t *e)
{
	vec3_t		mins, maxs;
	int			i;
	qboolean	rotated;

	if (currentmodel->nummodelsurfaces == 0)
		return;

	currententity = e;

	if (e->angles[0] || e->angles[1] || e->angles[2])
	{
		rotated = true;
		for (i=0 ; i<3 ; i++)
		{
			mins[i] = e->origin[i] - currentmodel->radius;
			maxs[i] = e->origin[i] + currentmodel->radius;
		}
	}
	else
	{
		rotated = false;
		VectorAdd (e->origin, currentmodel->mins, mins);
		VectorAdd (e->origin, currentmodel->maxs, maxs);
	}

	if (R_CullBox (mins, maxs)) {
		return;
	}

	qglColor3f (1,1,1);

	VectorSubtract (r_newrefdef.vieworg, e->origin, modelorg);

	if (rotated)
	{
		vec3_t	temp;
		vec3_t	forward, right, up;

		VectorCopy (modelorg, temp);
		AngleVectors (e->angles, forward, right, up);
		modelorg[0] = DotProduct (temp, forward);
		modelorg[1] = -DotProduct (temp, right);
		modelorg[2] = DotProduct (temp, up);
	}

	qglPushMatrix ();
	R_RotateForEntity (e);

	BSP_DrawTexturelessInlineBModel (e);

	qglPopMatrix ();
}



/*
=========================================

BSP Surface Rendering
Common between brush and world models

=========================================
*/


/*
=========================================
Special surfaces - Somewhat less common, require more work to render
 - Translucent ("alpha") surfaces
   These are special because they have to be rendered all in one pass, despite
   consisting of several different types of surfaces, so the code can't make 
   too many assumptions about the surface.
 - Rscript surfaces (those with material shaders)
   Rscript surfaces are first rendered through the "ordinary" path, then 
   this one.
 - Wavy, rippling ("warp") surfaces
   The code to actually render these is in r_warp.c.
=========================================
*/


// The "special" surfaces use these for linked lists.
// The reason to have linked lists for surfaces from brush model entities
// separate from the linked lists for world surfaces is that the world
// surface linked lists can be preserved between frames if r_optimize is on,
// whereas the entity linked lists must be cleared each time an entity is
// drawn.
surfchain_t	r_alpha_surfaces;
surfchain_t	r_warp_surfaces;
msurface_t	*r_rscript_surfaces; // no brush models can have rscript surfs

// This is a chain of surfaces that may need to have their lightmaps updated.
// They are not rendered in the order of this chain and will be linked into
// other chains for rendering.
msurface_t	*r_flicker_surfaces;


/*
================
BSP_DrawWarpSurfaces
================
*/
void BSP_DrawWarpSurfaces (qboolean forEnt)
{
	msurface_t	*surf;
	image_t		*image;
	
	if (forEnt)
		surf = r_warp_surfaces.entchain;
	else
		surf = r_warp_surfaces.worldchain;
	
	if (surf == NULL)
		return;
	
	// no lightmaps rendered on these surfaces
	GL_EnableMultitexture( false );
	GL_TexEnv( GL_MODULATE );
	qglColor4f( gl_state.inverse_intensity,
				gl_state.inverse_intensity,
				gl_state.inverse_intensity,
				1.0F );
	while (surf)
	{
		c_brush_polys++;
		image = BSP_TextureAnimation (surf->texinfo);
		GL_MBind (0, image->texnum);
		R_RenderWaterPolys(surf, 0, 1, 1);
		surf = surf->texturechain;
	}
	
	if (forEnt)
		r_warp_surfaces.entchain = NULL;
	
	GL_EnableMultitexture( true );
	GL_TexEnv( GL_REPLACE );
	R_KillVArrays ();
}

/*
================
BSP_DrawAlphaPoly
================
*/
void BSP_DrawAlphaPoly (msurface_t *fa, int flags)

{
	float	scroll;

	scroll = 0;
	if (flags & SURF_FLOWING)
	{
		scroll = -64 * ( (r_newrefdef.time / 40.0) - (int)(r_newrefdef.time / 40.0) );
		if (scroll == 0.0)
			scroll = -64.0;
	}

	R_InitVArrays(VERT_SINGLE_TEXTURED);

	R_AddTexturedSurfToVArray (fa, scroll);
	R_KillVArrays();
}


/*
================
R_DrawAlphaSurfaces

Draw water surfaces and windows.

Annoyingly, because alpha surfaces have to be drawn from back to front, 
everything transparent-- water, rscripted surfs, and non-rscripted surfs-- has
to be drawn in a single pass. This is an inherently inefficient process.

The BSP tree is walked front to back, so unwinding the chain of alpha surfaces
will draw back to front, giving proper ordering FOR BSP SURFACES! 


It's a bit wrong for entity surfaces (i.e. glass doors.) Because they are in
separate linked lists, the entity surfaces must be either always behind or
always in front of the world surfaces. I chose always in front because that
seems to fix all rendering issues, regardless of whether the entity actually 
is in front. Search me why. NOTE: this bug existed even when it was all one
linked list, although at that time entity surfaces were always behind map
surfaces (added to the beginning of the linked list after all the BSP 
rendering code.)
================
*/
void R_DrawAlphaSurfaces_chain (msurface_t *chain)
{
	msurface_t	*s;
	float		intens;
	rscript_t	*rs_shader;
	rs_stage_t	*stage = NULL;
	int			texnum = 0;
	float		scaleX = 1.0f, scaleY = 1.0f;

	// the textures are prescaled up for a better lighting range,
	// so scale it back down
	intens = gl_state.inverse_intensity;
	
	GL_SelectTexture (0);
	qglDepthMask ( GL_FALSE );
	qglEnable (GL_BLEND);
	GL_TexEnv( GL_MODULATE );
	
	for (s=chain ; s ; s=s->texturechain)
	{
		GL_MBind (0, s->texinfo->image->texnum);
		c_brush_polys++;

		if (s->texinfo->flags & SURF_TRANS33)
			qglColor4f (intens, intens, intens, 0.33);
		else if (s->texinfo->flags & SURF_TRANS66)
			qglColor4f (intens, intens, intens, 0.66);
		else
			qglColor4f (intens, intens, intens, 1);

		//moving trans brushes
		if (s->entity)
		{
			qglLoadMatrixf (r_world_matrix); //moving trans brushes
			R_RotateForEntity (s->entity);
		}
		
		rs_shader = NULL;
		if (r_shaders->integer)
			rs_shader = (rscript_t *)s->texinfo->image->script;

		if (s->iflags & ISURF_DRAWTURB) 
		{
			//water shaders
			scaleX = scaleY = 1.0f;
			if(rs_shader) 
			{
				stage = rs_shader->stage;
				if(stage) 
				{	//for now, just map a reflection texture
					texnum = stage->texture->texnum; //pass this to renderwaterpolys
				}
				if(stage->scale.scaleX != 0 && stage->scale.scaleY !=0) 
				{
					scaleX = stage->scale.scaleX;
					scaleY = stage->scale.scaleY;
				}
			}
			R_RenderWaterPolys (s, texnum, scaleX, scaleY);
			GL_SelectTexture (0);
			qglEnable (GL_BLEND);
			GL_TexEnv( GL_MODULATE );
		}
		else if(rs_shader && !(s->texinfo->flags & SURF_FLOWING)) 
		{
			RS_Surface(s);
			GL_SelectTexture (0);
			qglEnable (GL_BLEND);
			GL_TexEnv( GL_MODULATE );
		}
		else
			BSP_DrawAlphaPoly (s, s->texinfo->flags);
	}

	GL_SelectTexture (0);
	GL_TexEnv( GL_REPLACE );
	qglColor4f (1,1,1,1);
	qglDisable (GL_BLEND);
	qglDepthMask ( GL_TRUE );
}

void R_DrawAlphaSurfaces (void)
{
	R_DrawAlphaSurfaces_chain (r_alpha_surfaces.worldchain);
	R_DrawAlphaSurfaces_chain (r_alpha_surfaces.entchain);
	qglLoadMatrixf (r_world_matrix); //moving trans brushes
	r_alpha_surfaces.entchain = NULL;
}

/*
================
R_DrawRSSurfaces

Draw shader surfaces
================
*/
void R_DrawRSSurfaces (void)
{
	msurface_t	*s = r_rscript_surfaces;

	if(!s)
		return;

	if (!r_shaders->integer)
	{
		r_rscript_surfaces = NULL;
		return;
	}

	qglDepthMask(false);
	qglShadeModel (GL_SMOOTH);

	qglEnable(GL_POLYGON_OFFSET_FILL);
	qglPolygonOffset(-3, -2);

	for (; s; s = s->rscriptchain)
		RS_Surface(s);

	qglDisable(GL_POLYGON_OFFSET_FILL);

	GLSTATE_DISABLE_BLEND
	GLSTATE_DISABLE_ALPHATEST

	qglDepthMask(true);
}



/*
=========================================
Ordinary surfaces (fixed-function, normalmapped, and dynamically lit)
These are the most commonly used types of surfaces, so the rendering code path
for each is more optimized-- surfaces grouped by texinfo, VBOs, VBO batching,
etc.
=========================================
*/

// The "ordinary" surfaces do not use global variables for linked lists. The
// linked lists are in the texinfo struct, so the textures can be grouped by
// texinfo. Like the "special" surfaces, there are separate linked lists for
// entity surfaces and world surfaces.

// State variables for detecting changes from one surface to the next. If any
// of these change, the current batch of polygons to render is flushed. This
// helps minimize GL state change calls and draw calls.
int 		r_currLMTex = -9999; // only bind a lightmap texture if it is not
								 // the same as previous surface
mtexinfo_t	*r_currTexInfo = NULL; //texinfo struct
float		*r_currTangentSpaceTransform; //etc.

// VBO batching
// This system allows contiguous sequences of polygons to be merged into 
// batches, even if they are added out of order.

// There is a linked list of these, but they are not dynamically allocated.
// They are allocated from a static array. This prevents us wasting CPU with
// lots of malloc/free calls. Keep this struct small for performance!
typedef struct vbobatch_s {
	int					first_vert, last_vert;
	struct vbobatch_s 	*next;
} vbobatch_t;

#define MAX_VBO_BATCHES 100	// 100 ought to be enough. If we run out, we can 
							// always just draw some prematurely. 

// use a static array and counter to avoid lots of malloc nonsense
int			num_vbo_batches; 
vbobatch_t	vbobatch_buffer[MAX_VBO_BATCHES];
// never used directly; linked list base only
vbobatch_t	first_vbobatch[] = {{-1, -1, NULL}}; 
// if false, flushVBOAccum will set up the VBO GL state
qboolean	r_vboOn = false; 
// for the rspeed_vbobatches HUD gauge
int			c_vbo_batches;

// clear all accumulated surfaces without rendering
static inline void BSP_ClearVBOAccum (void)
{
	memset (vbobatch_buffer, 0, sizeof(vbobatch_buffer));
	num_vbo_batches = 0;
	first_vbobatch->next = NULL;
}

// render all accumulated surfaces, then clear them
// XXX: assumes that global OpenGL state is correct for whatever surfaces are
// in the accumulator, so don't change state until you've called this!
static inline void BSP_FlushVBOAccum (void)
{
	vbobatch_t *batch = first_vbobatch->next;
	
	if (!batch)
		return;
	
	if (!r_vboOn)
	{
		GL_SetupWorldVBO ();
		r_vboOn = true;
	}
	
	// XXX: for future reference, the glDrawRangeElements code was last seen
	// here at revision 3246.
	for (; batch; batch = batch->next)
	{
		qglDrawArrays (GL_TRIANGLES, batch->first_vert, batch->last_vert-batch->first_vert);
		c_vbo_batches++;
	}
	
	BSP_ClearVBOAccum ();
}

// Add a new surface to the VBO batch accumulator. If its vertex data is next
// to any other surfaces within the VBO, opportunistically merge the surfaces
// together into a larger "batch," so they can be rendered with one draw call. 
// Whenever two batches touch each other, they are merged. Hundreds of
// surfaces can be rendered with a single call, which is easier on the OpenGL
// pipeline.
static inline void BSP_AddToVBOAccum (int first_vert, int last_vert)
{
	vbobatch_t *batch = first_vbobatch->next, *prev = first_vbobatch;
	vbobatch_t *new;
	
	if (!batch)
	{
		batch = first_vbobatch->next = vbobatch_buffer;
		batch->first_vert = first_vert;
		batch->last_vert = last_vert;
		num_vbo_batches++;
		return;
	}
	
	// This is optimal. Because of the way the surface linked lists are built
	// by BSP_AddToTextureChain, they are usually in reverse order, so it's
	// best to start toward the beginning of the list of VBO batches where
	// you're more likely to merge something.
	
	while (batch->next && batch->next->first_vert < first_vert)
	{
		prev = batch;
		batch = batch->next;
	}
	
	if (batch->first_vert > last_vert)
	{
		new = &vbobatch_buffer[num_vbo_batches++];
		new->next = batch;
		prev->next = new;
		new->first_vert = first_vert;
		new->last_vert = last_vert;
	}
	else if (batch->last_vert == first_vert)
	{
		batch->last_vert = last_vert;
		if (batch->next && batch->next->first_vert == last_vert)
		{
			// This is the special case where the new surface bridges the gap
			// between two existing batches, allowing us to merge them into 
			// the first one. This is the only case where we actually remove a
			// batch instead of growing one or adding one.
			batch->last_vert = batch->next->last_vert;
			if (batch->next == &vbobatch_buffer[num_vbo_batches-1])
				num_vbo_batches--;
			batch->next = batch->next->next;
		}
		return; //no need to check for maximum batch count being hit
	}
	else if (batch->next && batch->next->first_vert == last_vert)
	{
		batch->next->first_vert = first_vert;
		return; //no need to check for maximum batch count being hit
	}
	else if (batch->first_vert == last_vert)
	{
		batch->first_vert = first_vert;
		return; //no need to check for maximum batch count being hit
	}
	else //if (batch->last_vert < first_vert)
	{
		new = &vbobatch_buffer[num_vbo_batches++];
		new->next = batch->next;
		batch->next = new;
		new->first_vert = first_vert;
		new->last_vert = last_vert;
	}
	
	//running out of space
	if (num_vbo_batches == MAX_VBO_BATCHES)
	{
/*		Com_Printf ("MUSTFLUSH\n");*/
		BSP_FlushVBOAccum ();
	}
}

/*
================
R_SetLightingMode

Setup the fixed-function pipeline with texture combiners to enable rendering
of lightmapped surfaces. For GLSL renders, this is unnecessary, as the shader
handles this job.
================
*/
void R_SetLightingMode (void)
{
	GL_SelectTexture (0);
	GL_TexEnv ( GL_COMBINE_EXT );
	qglTexEnvi ( GL_TEXTURE_ENV, GL_COMBINE_RGB_EXT, GL_REPLACE );
	qglTexEnvi ( GL_TEXTURE_ENV, GL_SOURCE0_RGB_EXT, GL_TEXTURE );
	qglTexEnvi ( GL_TEXTURE_ENV, GL_COMBINE_ALPHA_EXT, GL_REPLACE );
	qglTexEnvi ( GL_TEXTURE_ENV, GL_SOURCE0_ALPHA_EXT, GL_TEXTURE );

	GL_SelectTexture (1);
	GL_TexEnv ( GL_COMBINE_EXT );
	if ( gl_lightmap->integer ) 
	{
		qglTexEnvi ( GL_TEXTURE_ENV, GL_COMBINE_RGB_EXT, GL_REPLACE );
		qglTexEnvi ( GL_TEXTURE_ENV, GL_SOURCE0_RGB_EXT, GL_TEXTURE );
		qglTexEnvi ( GL_TEXTURE_ENV, GL_COMBINE_ALPHA_EXT, GL_REPLACE );
		qglTexEnvi ( GL_TEXTURE_ENV, GL_SOURCE0_ALPHA_EXT, GL_TEXTURE );
	} 
	else 
	{
		qglTexEnvi ( GL_TEXTURE_ENV, GL_COMBINE_RGB_EXT, GL_MODULATE );
		qglTexEnvi ( GL_TEXTURE_ENV, GL_SOURCE0_RGB_EXT, GL_TEXTURE );
		qglTexEnvi ( GL_TEXTURE_ENV, GL_SOURCE1_RGB_EXT, GL_PREVIOUS_EXT );
		qglTexEnvi ( GL_TEXTURE_ENV, GL_COMBINE_ALPHA_EXT, GL_MODULATE );
		qglTexEnvi ( GL_TEXTURE_ENV, GL_SOURCE0_ALPHA_EXT, GL_TEXTURE );
		qglTexEnvi ( GL_TEXTURE_ENV, GL_SOURCE1_ALPHA_EXT, GL_PREVIOUS_EXT );
	}

	if ( r_overbrightbits->value )
	{
		qglTexEnvi ( GL_TEXTURE_ENV, GL_RGB_SCALE_EXT, r_overbrightbits->value );
	}
}

/*
================
BSP_TexinfoChanged

Update GL state as needed so we can draw a new batch of surfaces for the 
provided texinfo 
================
*/
static void BSP_TexinfoChanged (mtexinfo_t *texinfo, qboolean glsl, qboolean dynamic)
{
	int		texnum;
	float	scroll;
	
	BSP_FlushVBOAccum ();
	
	if (TexinfoIsAlphaBlended (texinfo))
	{
		if (!r_currTexInfo || !TexinfoIsAlphaBlended(r_currTexInfo))
			qglEnable( GL_ALPHA_TEST );
	}
	else
	{
		if (!r_currTexInfo || TexinfoIsAlphaBlended(r_currTexInfo))
			qglDisable( GL_ALPHA_TEST );
	}
	
	if (glsl)
		// no texture animation for normalmapped surfaces, for some reason
		texnum = texinfo->image->texnum;
	else
		// do this here so only have to do it once instead of for each surface
		texnum = BSP_TextureAnimation( texinfo )->texnum;
	
	GL_SelectTexture (0);
	GL_Bind (texnum);
	
	// scrolling is done using the texture matrix
	if (	!r_currTexInfo || (texinfo->flags & SURF_FLOWING) ||
			(r_currTexInfo->flags & SURF_FLOWING))
	{
		qglMatrixMode (GL_TEXTURE);
		qglLoadIdentity ();
		if (texinfo->flags & SURF_FLOWING)
		{
			scroll = -64 * ( (r_newrefdef.time / 40.0) - (int)(r_newrefdef.time / 40.0) );
			if (scroll == 0.0)
				scroll = -64.0;
			qglTranslatef (scroll, 0, 0);
		}
		qglMatrixMode (GL_MODELVIEW);
	}
	
	if (!glsl)
	{
		r_currTexInfo = texinfo;
		return;
	}
	
	GL_MBind (2, texinfo->heightMap->texnum);
	GL_MBind (3, texinfo->normalMap->texnum);
	KillFlags |= KILL_TMU2_POINTER | KILL_TMU3_POINTER;
	
	if (dynamic)
	{
		if(gl_bspnormalmaps->integer && texinfo->has_heightmap) 
		{
			if (!r_currTexInfo || !r_currTexInfo->has_heightmap)
			{
				glUniform1iARB( g_location_parallax, 1);
			}
		}
		else
		{
			if (!r_currTexInfo || r_currTexInfo->has_heightmap)
			{
				glUniform1iARB( g_location_parallax, 0);
			}
		}
	}
	
	if (!gl_bspnormalmaps->integer)
	{
		if (!r_currTexInfo)
		{
			glUniform1iARB( g_location_liquid, 0 );
			glUniform1iARB( g_location_shiny, 0 );
		}
	}
	else if	(r_currTexInfo &&
			(texinfo->flags & (SURF_BLOOD|SURF_WATER|SURF_SHINY)) == 
			(r_currTexInfo->flags & (SURF_BLOOD|SURF_WATER|SURF_SHINY)))
	{
		//no change to GL state is needed
	}
	else if (texinfo->flags & SURF_BLOOD) 
	{
		//need to bind the blood drop normal map, and set flag, and time
		glUniform1iARB( g_location_liquid, 8 ); //blood type 8, water 1
		glUniform1iARB( g_location_shiny, 0 );
		glUniform1fARB( g_location_rsTime, rs_realtime);
		glUniform1iARB( g_location_liquidTexture, 4); //for blood we are going to need to send a diffuse texture with it
		GL_MBind (4, r_blooddroplets->texnum);
		KillFlags |= KILL_TMU4_POINTER;
		glUniform1iARB( g_location_liquidNormTex, 5); 
		GL_MBind (5, r_blooddroplets_nm->texnum);
		KillFlags |= KILL_TMU5_POINTER;
	}
	else if (texinfo->flags & SURF_WATER) 
	{
		//need to bind the water drop normal map, and set flag, and time
		glUniform1iARB( g_location_liquid, 1 ); 
		glUniform1iARB( g_location_shiny, 0 );
		glUniform1fARB( g_location_rsTime, rs_realtime);
		glUniform1iARB( g_location_liquidNormTex, 4); //for blood we are going to need to send a diffuse texture with it(maybe even height!)
		GL_MBind (4, r_droplets->texnum);
		KillFlags |= KILL_TMU4_POINTER;
	}
	else if (texinfo->flags & SURF_SHINY)
	{
		glUniform1iARB( g_location_liquid, 0 );
		glUniform1iARB( g_location_shiny, 1 );

		glUniform1iARB( g_location_chromeTex, 4); 
		GL_MBind (4, r_mirrorspec->texnum);
		KillFlags |= KILL_TMU4_POINTER;
	}
	else if (!r_currTexInfo || r_currTexInfo->flags & (SURF_BLOOD|SURF_WATER|SURF_SHINY))
	{
		glUniform1iARB( g_location_liquid, 0 );
		glUniform1iARB( g_location_shiny, 0 );
	}
	
	r_currTexInfo = texinfo;
}

/*
================
BSP_RenderLightmappedPoly

Main polygon rendering routine (all standard surfaces)
================
*/
static void BSP_RenderLightmappedPoly( msurface_t *surf, qboolean glsl)
{
	unsigned lmtex = surf->lightmaptexturenum;
		
	c_brush_polys++;
	
	if (lmtex != r_currLMTex)
	{
		BSP_FlushVBOAccum ();
		GL_MBind (1, gl_state.lightmap_textures + lmtex);
	}
	
	if (glsl && r_currTangentSpaceTransform != surf->tangentSpaceTransform)
	{
		BSP_FlushVBOAccum ();
		glUniformMatrix3fvARB( g_tangentSpaceTransform,	1, GL_FALSE, (const GLfloat *) surf->tangentSpaceTransform );
		r_currTangentSpaceTransform = (float *)surf->tangentSpaceTransform; 
	}
	
	// If we've gotten this far, it's because the surface is not translucent,
	// warped, sky, or nodraw, thus it *will* be in the VBO.
	BSP_AddToVBOAccum (surf->vbo_first_vert, surf->vbo_first_vert+surf->vbo_num_verts);
}

void BSP_DrawNonGLSLSurfaces (qboolean forEnt)
{
	int		 i;

	// reset VBO batching state
	r_currLMTex = -99999;
	r_currTexInfo = NULL;
	r_currTangentSpaceTransform = NULL;
	
	BSP_FlushVBOAccum ();

	r_vboOn = false;
	
	qglEnableClientState( GL_VERTEX_ARRAY );
	qglClientActiveTextureARB (GL_TEXTURE0);
	qglEnableClientState(GL_TEXTURE_COORD_ARRAY);
	qglClientActiveTextureARB (GL_TEXTURE1);
	qglEnableClientState(GL_TEXTURE_COORD_ARRAY);
	KillFlags |= (KILL_TMU0_POINTER | KILL_TMU1_POINTER);
	
	for (i = 0; i < currentmodel->num_unique_texinfos; i++)
	{
		msurface_t	*s;
		if (forEnt)
		{
			s = currentmodel->unique_texinfo[i]->lightmap_surfaces.entchain;
			currentmodel->unique_texinfo[i]->lightmap_surfaces.entchain = NULL;
		}
		else
		{
			s = currentmodel->unique_texinfo[i]->lightmap_surfaces.worldchain;
		}
		if (!s)
			continue;
		BSP_TexinfoChanged (s->texinfo->equiv, false, false);
		for (; s; s = s->texturechain) {
			BSP_RenderLightmappedPoly(s, false);
			r_currLMTex = s->lightmaptexturenum;
		}
	}
	
	BSP_FlushVBOAccum ();
	
	qglDisable (GL_ALPHA_TEST);
}

void BSP_DrawGLSLSurfaces (qboolean forEnt)
{
	int		 i;

	// reset VBO batching state
	r_currLMTex = -99999;
	r_currTexInfo = NULL;
	r_currTangentSpaceTransform = NULL;
	
	if (!gl_bspnormalmaps->integer)
	{
		return;
	}
	
	BSP_ClearVBOAccum ();

	if(r_shadowmapcount == 2)
	{
		//static vegetation shadow
		glUniform1iARB( g_location_bspShadowmapTexture2, 6);
		GL_MBind (6, r_depthtexture2->texnum);

		glUniform1iARB( g_location_shadowmap, 1);
		glUniform1iARB( g_Location_statshadow, 1 );

		glUniform1fARB( g_location_xOffs, 1.0/(viddef.width*r_shadowmapscale->value));
		glUniform1fARB( g_location_yOffs, 1.0/(viddef.height*r_shadowmapscale->value));
	}
	else
	{
		glUniform1iARB( g_location_shadowmap, 0);
		glUniform1iARB( g_Location_statshadow, 0);
	}

	glUniform1iARB( g_location_dynamic, 0);
	glUniform1iARB( g_location_parallax, 1);  
	
	r_vboOn = false;
	
	qglEnableClientState( GL_VERTEX_ARRAY );
	qglClientActiveTextureARB (GL_TEXTURE0);
	qglEnableClientState(GL_TEXTURE_COORD_ARRAY);
	qglClientActiveTextureARB (GL_TEXTURE1);
	qglEnableClientState(GL_TEXTURE_COORD_ARRAY);
	KillFlags |= (KILL_TMU0_POINTER | KILL_TMU1_POINTER);
	
	for (i = 0; i < currentmodel->num_unique_texinfos; i++)
	{
		msurface_t	*s;
		if (forEnt)
		{
			s = currentmodel->unique_texinfo[i]->glsl_surfaces.entchain;
			currentmodel->unique_texinfo[i]->glsl_surfaces.entchain = NULL;
		}
		else
		{
			s = currentmodel->unique_texinfo[i]->glsl_surfaces.worldchain;
		}
		if (!s)
			continue;
		BSP_TexinfoChanged (s->texinfo->equiv, true, false);
		for (; s; s = s->texturechain) {
			BSP_RenderLightmappedPoly(s, true);
			r_currLMTex = s->lightmaptexturenum;
		}
	}
	
	BSP_FlushVBOAccum ();

	qglDisable (GL_ALPHA_TEST);
}

void BSP_DrawGLSLDynamicSurfaces (qboolean forEnt)
{
	int		 i;
	dlight_t	*dl = NULL;
	int			lnum, sv_lnum = 0;
	float		add, brightest = 0;
	vec3_t		lightVec;
	float		lightCutoffSquared = 0.0f;
	qboolean	foundLight = false;
	
	if (gl_dynamic->integer == 0)
	{
		return;
	}

	dl = r_newrefdef.dlights;
	for (lnum=0; lnum<r_newrefdef.num_dlights; lnum++, dl++)
	{
		VectorSubtract (r_origin, dl->origin, lightVec);
		add = dl->intensity - VectorLength(lightVec)/10;
		if (add > brightest) //only bother with lights close by
		{
			brightest = add;
			sv_lnum = lnum; //remember the position of most influencial light
		}
	}

	if(brightest > 0) 
	{ 
		//we have a light
		foundLight= true;
		dynLight = r_newrefdef.dlights;
		dynLight += sv_lnum; //our most influential light

		lightCutoffSquared = ( dynLight->intensity - DLIGHT_CUTOFF );

		if( lightCutoffSquared <= 0.0f )
			lightCutoffSquared = 0.0f;

		lightCutoffSquared *= 2.0f;
		lightCutoffSquared *= lightCutoffSquared;		

		// reset VBO batching state
		r_currLMTex = -99999;		
		r_currTexInfo = NULL;
		r_currTangentSpaceTransform = NULL;
		
		BSP_ClearVBOAccum ();

		glUniform3fARB( g_location_lightPosition, dynLight->origin[0], dynLight->origin[1], dynLight->origin[2]);
		glUniform3fARB( g_location_lightColour, dynLight->color[0], dynLight->color[1], dynLight->color[2]);

		glUniform1fARB( g_location_lightCutoffSquared, lightCutoffSquared);
		
		if(gl_shadowmaps->integer) 
		{
			//dynamic shadow
			glUniform1iARB( g_location_bspShadowmapTexture, 7);
			GL_MBind (7, r_depthtexture->texnum);

			glUniform1iARB( g_location_shadowmap, 1);

			glUniform1fARB( g_location_xOffs, 1.0/(viddef.width*r_shadowmapscale->value));
			glUniform1fARB( g_location_yOffs, 1.0/(viddef.height*r_shadowmapscale->value));
		}
		else
			glUniform1iARB( g_location_shadowmap, 0);		
	}

	glUniform1iARB( g_location_dynamic, foundLight);
	
	r_vboOn = false;
	
	qglEnableClientState( GL_VERTEX_ARRAY );
	qglClientActiveTextureARB (GL_TEXTURE0);
	qglEnableClientState(GL_TEXTURE_COORD_ARRAY);
	qglClientActiveTextureARB (GL_TEXTURE1);
	qglEnableClientState(GL_TEXTURE_COORD_ARRAY);
	KillFlags |= (KILL_TMU0_POINTER | KILL_TMU1_POINTER);
	
	for (i = 0; i < currentmodel->num_unique_texinfos; i++)
	{
		msurface_t	*s;
		if (forEnt)
		{
			s = currentmodel->unique_texinfo[i]->dynamic_surfaces.entchain;
			currentmodel->unique_texinfo[i]->dynamic_surfaces.entchain = NULL;
		}
		else
		{
			s = currentmodel->unique_texinfo[i]->dynamic_surfaces.worldchain;
		}
		if (!s)
			continue;
		BSP_TexinfoChanged (s->texinfo->equiv, true, true);
		for (; s; s = s->texturechain) {
			BSP_RenderLightmappedPoly(s, true);
			r_currLMTex = s->lightmaptexturenum;
		}
	}
	
	BSP_FlushVBOAccum ();
	
	qglDisable (GL_ALPHA_TEST);
}




/*
=========================================
This is the "API" for the BSP surface renderer backend, hiding most of the
complexity of the previous functions. 
=========================================
*/


/*
================
BSP_ClearWorldTextureChains

Reset linked lists for the world (non-entity/non-brushmodel) surfaces. This 
need not be called every frame if r_optimize is on. No equivalent function is
needed for entity surfaces because they are reset automatically when they are
drawn.
================
*/
void BSP_ClearWorldTextureChains (void)
{
	int i;
	
	for (i = 0; i < currentmodel->num_unique_texinfos; i++)
	{
		currentmodel->unique_texinfo[i]->lightmap_surfaces.worldchain = NULL;
		currentmodel->unique_texinfo[i]->glsl_surfaces.worldchain = NULL;
		currentmodel->unique_texinfo[i]->dynamic_surfaces.worldchain = NULL;
	}
	
	r_warp_surfaces.worldchain = NULL;
	r_alpha_surfaces.worldchain = NULL;
	r_rscript_surfaces = NULL;
	r_flicker_surfaces = NULL;
}

/*
================
BSP_AddToTextureChain

Call this on a surface (and indicate whether it's an entity surface) and it 
will figure out which texture chain to add it to; this function is responsible
for deciding if the surface is "ordinary" or somehow "special," whether it
should be normalmapped and/or dynamically lit, etc.

This function will be repeatedly called on all the surfaces in the current 
brushmodel entity or in the world's static geometry, followed by a single call
to BSP_DrawTextureChains to render them all in one fell swoop.
================
*/
void BSP_UpdateSurfaceLightmap (msurface_t *surf);
void BSP_AddToTextureChain(msurface_t *surf, qboolean forEnt)
{
	int			map;
	msurface_t	**chain;
	qboolean	is_dynamic = false;
	
	// Special surfaces that need to be handled separately
	
	if (surf->texinfo->flags & SURF_SKY)
	{	// just adds to visible sky bounds
		R_AddSkySurface (surf);
		return;
	}
	
#define AddToChainPair(chainpair)  \
	chain = (forEnt?\
				&((chainpair).entchain):\
				&((chainpair).worldchain));\
	surf->texturechain = *chain; \
	*chain = surf; 
	
	if (SurfaceIsTranslucent(surf) && !SurfaceIsAlphaBlended(surf))
	{	// add to the translucent chain
		AddToChainPair (r_alpha_surfaces);
		return;
	}
	
	if (surf->iflags & ISURF_DRAWTURB)
	{	// add to the warped surfaces chain
		AddToChainPair (r_warp_surfaces);
		return;
	}
	
	
	// The rest of the function handles most ordinary surfaces: normalmapped,
	// non-normalmapped, and dynamically lit surfaces. As these three cases 
	// are the most common, they are the most optimized-- grouped by texinfo,
	// etc. Note that with alpha, warp, and sky surfaces out of the way, all
	// the remaining surfaces have lightmaps.

	// XXX: we could require gl_bspnormalmaps here, but that would result in
	// weird inconsistency with only meshes lighting up. Better to fall back
	// on GLSL for dynamically lit surfaces, even with gl_bspnormalmaps 0.
	if(r_newrefdef.num_dlights && gl_dynamic->integer)
	{
		// Dynamic surfaces must have normalmaps, as the old fixed-function
		// texture-based dynamic lighting system is depreciated.
		is_dynamic = (surf->dlightframe == r_framecount && surf->texinfo->has_normalmap);
	}
	
	// reviving the ancient lightstyle system
	for ( map = 0; map < MAXLIGHTMAPS && surf->styles[map] != 255; map++ )
	{
		// Chain of surfaces that may need to have their lightmaps updated
		// in future frames (for dealing with r_optimize)
		if (surf->styles[map] != 0)
		{
			if (!forEnt)
			{
				surf->flickerchain = r_flicker_surfaces;
				r_flicker_surfaces = surf;
				break;
			}
			if ( r_newrefdef.lightstyles[surf->styles[map]].white != surf->cached_light[map])
			{
				BSP_UpdateSurfaceLightmap (surf);
				break;
			}
		}
	}

	if(is_dynamic) 
	{
		//always glsl for dynamic if it has a normalmap
		AddToChainPair (surf->texinfo->equiv->dynamic_surfaces);
	}
	else if(gl_bspnormalmaps->integer
			&& surf->texinfo->has_heightmap
			&& surf->texinfo->has_normalmap) 
	{
		AddToChainPair (surf->texinfo->equiv->glsl_surfaces);
	}
	else 
	{
		AddToChainPair (surf->texinfo->equiv->lightmap_surfaces);
	}
	
	// Add to the rscript chain if there is actually a shader
	// TODO: investigate the possibility of doing this for brush models as 
	// well-- might cause problems with caustics and brush models only half
	// submerged?
	if(!forEnt && r_shaders->integer)
	{ 
		if (surf->texinfo->image->script != NULL || (surf->iflags & ISURF_UNDERWATER))
		{
			surf->rscriptchain = r_rscript_surfaces;
			r_rscript_surfaces = surf;
		}
	}
#undef AddToChainPair
}

/*
================
BSP_DrawTextureChains

Draw ALL surfaces for either the current entity or the world model. If drawing
entity surfaces, reset the linked lists afterward.
================
*/
void BSP_DrawTextureChains (qboolean forEnt)
{
	msurface_t	*flickersurf;
	int			map;
	
	if (!forEnt)
	{
		R_KillVArrays (); // TODO: check if necessary
		
		for (flickersurf = r_flicker_surfaces; flickersurf != NULL; flickersurf = flickersurf->flickerchain)
		{
			for ( map = 0; map < MAXLIGHTMAPS && flickersurf->styles[map] != 255; map++ )
			{
				if ( r_newrefdef.lightstyles[flickersurf->styles[map]].white != flickersurf->cached_light[map])
				{
					BSP_UpdateSurfaceLightmap (flickersurf);
					break;
				}
			}
		}
	}

	// Setup GL state for lightmap render 
	// (TODO: only necessary for fixed-function pipeline?)

	GL_EnableMultitexture( true );
	
	R_SetLightingMode ();

	// render all fixed-function surfaces
	BSP_DrawNonGLSLSurfaces(forEnt);

	// render all GLSL surfaces, including normalmapped and dynamically lit
	if(gl_dynamic->integer || gl_bspnormalmaps->integer)
	{
		glUseProgramObjectARB( g_programObj );
		glUniform3fARB( g_location_eyePos, r_origin[0], r_origin[1], r_origin[2] );
		glUniform1iARB( g_location_fog, map_fog);
		glUniform3fARB( g_location_staticLightPosition, r_worldLightVec[0], r_worldLightVec[1], r_worldLightVec[2]);
		glUniform1iARB( g_location_surfTexture, 0);
		glUniform1iARB( g_location_lmTexture, 1);
		glUniform1iARB( g_location_heightTexture, 2);
		glUniform1iARB( g_location_normalTexture, 3);
		BSP_DrawGLSLSurfaces(forEnt); 
		BSP_DrawGLSLDynamicSurfaces(forEnt);
		glUseProgramObjectARB( 0 );
	}
	
	GL_SelectTexture (0);
	qglMatrixMode (GL_TEXTURE);
	qglLoadIdentity ();
	qglMatrixMode (GL_MODELVIEW);
	
	// this has to come last because it messes with GL state
	BSP_DrawWarpSurfaces (forEnt);
	
	GL_EnableMultitexture( false );
}



/*
=============================================================

	BRUSH MODELS

=============================================================
*/

/*
=================
BSP_DrawInlineBModel

Picks which of the current entity's surfaces face toward the camera, and calls
BSP_AddToTextureChain on those.
=================
*/
void BSP_DrawInlineBModel ( void )
{
	int			i;
	cplane_t	*pplane;
	float		dot;
	msurface_t	*psurf;

	R_PushDlightsForBModel ( currententity );

	if ( currententity->flags & RF_TRANSLUCENT )
	{
		qglEnable (GL_BLEND);
		qglColor4f (1,1,1,0.25);
		GL_TexEnv( GL_MODULATE );
	}

	psurf = &currentmodel->surfaces[currentmodel->firstmodelsurface];
	for (i=0 ; i<currentmodel->nummodelsurfaces ; i++, psurf++)
	{
		if (psurf->texinfo->flags & SURF_NODRAW)
			continue; //can skip dot product stuff
			// TODO: remove these at load-time for inline models?
		
		// find which side of the plane we are on
		pplane = psurf->plane;
		dot = DotProduct (modelorg, pplane->normal) - pplane->dist;

		// draw the polygon
		if (((psurf->iflags & ISURF_PLANEBACK) && (dot < -BACKFACE_EPSILON)) ||
			(!(psurf->iflags & ISURF_PLANEBACK) && (dot > BACKFACE_EPSILON)))
		{
			// TODO: do this once at load time
			psurf->entity = currententity;
			
			BSP_AddToTextureChain( psurf, true );

			psurf->visframe = r_framecount;
		}
	}
	
	BSP_DrawTextureChains (true);
	
	qglDisable (GL_BLEND);
	qglColor4f (1,1,1,1);
	GL_TexEnv( GL_REPLACE );
	
	R_KillVArrays ();
}

/*
=================
R_DrawBrushModel
=================
*/
void R_DrawBrushModel ( void )
{
	vec3_t		mins, maxs;
	int			i;
	qboolean	rotated;

	if (currentmodel->nummodelsurfaces == 0)
		return;

	gl_state.currenttextures[0] = gl_state.currenttextures[1] = -1;

	if (currententity->angles[0] || currententity->angles[1] || currententity->angles[2])
	{
		rotated = true;
		for (i=0 ; i<3 ; i++)
		{
			mins[i] = currententity->origin[i] - currentmodel->radius;
			maxs[i] = currententity->origin[i] + currentmodel->radius;
		}
	}
	else
	{
		rotated = false;
		VectorAdd (currententity->origin, currentmodel->mins, mins);
		VectorAdd (currententity->origin, currentmodel->maxs, maxs);
	}

	if (R_CullBox (mins, maxs)) {
		return;
	}

	qglColor3f (1,1,1);

	VectorSubtract (r_newrefdef.vieworg, currententity->origin, modelorg);

	if (rotated)
	{
		vec3_t	temp;
		vec3_t	forward, right, up;

		VectorCopy (modelorg, temp);
		AngleVectors (currententity->angles, forward, right, up);
		modelorg[0] = DotProduct (temp, forward);
		modelorg[1] = -DotProduct (temp, right);
		modelorg[2] = DotProduct (temp, up);
	}

	qglPushMatrix ();
	R_RotateForEntity (currententity);

	BSP_DrawInlineBModel ();

	qglPopMatrix ();
}



/*
=============================================================

	WORLD MODEL

=============================================================
*/

/*
=================
R_CullBox

Returns true if the box is completely outside the frustum

Variant: uses clipflags
=================
*/
qboolean R_CullBox_ClipFlags (vec3_t mins, vec3_t maxs, int clipflags)
{
	int		i;
	cplane_t *p;

	for (i=0,p=frustum ; i<4; i++,p++)
	{
		if (!(clipflags  & (1<<i)))
			continue;
		switch (p->signbits)
		{
		case 0:
			if (p->normal[0]*maxs[0] + p->normal[1]*maxs[1] + p->normal[2]*maxs[2] < p->dist)
				return true;
			break;
		case 1:
			if (p->normal[0]*mins[0] + p->normal[1]*maxs[1] + p->normal[2]*maxs[2] < p->dist)
				return true;
			break;
		case 2:
			if (p->normal[0]*maxs[0] + p->normal[1]*mins[1] + p->normal[2]*maxs[2] < p->dist)
				return true;
			break;
		case 3:
			if (p->normal[0]*mins[0] + p->normal[1]*mins[1] + p->normal[2]*maxs[2] < p->dist)
				return true;
			break;
		case 4:
			if (p->normal[0]*maxs[0] + p->normal[1]*maxs[1] + p->normal[2]*mins[2] < p->dist)
				return true;
			break;
		case 5:
			if (p->normal[0]*mins[0] + p->normal[1]*maxs[1] + p->normal[2]*mins[2] < p->dist)
				return true;
			break;
		case 6:
			if (p->normal[0]*maxs[0] + p->normal[1]*mins[1] + p->normal[2]*mins[2] < p->dist)
				return true;
			break;
		case 7:
			if (p->normal[0]*mins[0] + p->normal[1]*mins[1] + p->normal[2]*mins[2] < p->dist)
				return true;
			break;
		default:
			return false;
		}
	}

	return false;
}

/*
================
BSP_RecursiveWorldNode

Goes through the entire world (the BSP tree,) picks out which surfaces should
be drawn, and calls BSP_AddToTextureChain on each.

node: the BSP node to work on
clipflags: indicate which planes of the frustum may intersect the node

Since each node is inside its parent in 3D space, if a frustum plane can be
shown not to intersect a node at all, then it won't intersect either of its
children. 
================
*/
void BSP_RecursiveWorldNode (mnode_t *node, int clipflags)
{
	int			c, side;
	cplane_t	*plane;
	msurface_t	*surf, **mark;
	mleaf_t		*pleaf;
	float		dot;
	
	if (node->contents == CONTENTS_SOLID)
		return;		// solid

	if (node->visframe != r_visframecount)
		return;
	
	// if a leaf node, draw stuff (pt 1)
	c = 0;
	if (node->contents != -1)
	{
		pleaf = (mleaf_t *)node;
		
		if (! (c = pleaf->nummarksurfaces) )
			return;

		// check for door connected areas
		if (! (r_newrefdef.areabits[pleaf->area>>3] & (1<<(pleaf->area&7)) ) )
			return;		// not visible

		mark = pleaf->firstmarksurface;
	}

	if (!r_nocull->integer && clipflags)
	{
		int i, clipped;
		cplane_t *clipplane;

		for (i=0,clipplane=frustum ; i<4 ; i++,clipplane++)
		{
			if (!(clipflags  & (1<<i)))
				continue;
			clipped = BoxOnPlaneSide (node->minmaxs, node->minmaxs+3, clipplane);

			if (clipped == 1)
				clipflags &= ~(1<<i);	// node is entirely on screen
			else if (clipped == 2)
				return;					// fully clipped
		}
	}

	//if a leaf node, draw stuff (pt 2)
	if (c != 0)
	{
		do
		{
			(*mark++)->visframe = r_framecount;
		} while (--c);

		return;
	}

	// node is just a decision point, so go down the apropriate sides

	// find which side of the node we are on
	plane = node->plane;

	switch (plane->type)
	{
	case PLANE_X:
		dot = modelorg[0];
		break;
	case PLANE_Y:
		dot = modelorg[1];
		break;
	case PLANE_Z:
		dot = modelorg[2];
		break;
	default:
		dot = DotProduct (modelorg, plane->normal);
		break;
	}

	side = dot < plane->dist;

	// recurse down the children, front side first
	BSP_RecursiveWorldNode (node->children[side], clipflags);

	// draw stuff
	for ( c = node->numsurfaces, surf = r_worldmodel->surfaces + node->firstsurface; c ; c--, surf++)
	{
		if (surf->visframe != r_framecount)
			continue;

		/* XXX: this doesn't seem to cull any surfaces AT ALL when positioned
		 * here! The surf->visframe check seems to catch any back-facing 
		 * surfaces, but the back-facing surface check seems to allow some
		 * surfaces which are later caught by the surf->visframe check. So the
		 * visframe check renders the planeback check redundant and useless.
		 * I'm pretty sure it's because the map compiler structures the BSP 
		 * tree in such a way as to avoid back-facing surfaces being drawn.
		 * -M
		if ( (surf->flags & SURF_PLANEBACK) != side )
			continue;		// wrong side
		*/

		if (clipflags != 0 && !( surf->iflags & ISURF_DRAWTURB ))
		{
			if (R_CullBox_ClipFlags (surf->mins, surf->maxs, clipflags)) 
				continue;
		}

		// the polygon is visible, so add it to the appropriate linked list
		BSP_AddToTextureChain( surf, false );
	}

	// recurse down the back side
	BSP_RecursiveWorldNode (node->children[!side], clipflags);
}

/*
=============
R_CalcWorldLights - this is the fallback for non deluxmapped bsp's
=============
*/
void R_CalcWorldLights( void )
{	
	int		i, j;
	vec3_t	lightAdd, temp;
	float	dist, weight;
	int		numlights = 0;

	if(gl_dynamic->integer || gl_bspnormalmaps->integer)
	{
		//get light position relative to player's position
		VectorClear(lightAdd);
		for (i = 0; i < r_lightgroups; i++) 
		{
			VectorSubtract(r_origin, LightGroups[i].group_origin, temp);
			dist = VectorLength(temp);
			if(dist == 0)
				dist = 1;
			dist = dist*dist;
			weight = (int)250000/(dist/(LightGroups[i].avg_intensity+1.0f));
			for(j = 0; j < 3; j++)
				lightAdd[j] += LightGroups[i].group_origin[j]*weight;
			numlights+=weight;
		}

		if(numlights > 0.0) 
		{
			for(i = 0; i < 3; i++)
				r_worldLightVec[i] = (lightAdd[i]/numlights + r_origin[i])/2.0;
		}
	}
}

/*
=============
R_DrawWorldSurfs
=============
*/
void R_DrawWorldSurfs (void)
{
	if (!r_drawworld->integer)
		return;

	if ( r_newrefdef.rdflags & RDF_NOWORLDMODEL )
		return;
	
	if (r_newrefdef.areabits == NULL)
	{
		Com_Printf ("WARN: No area bits!\n");
		return;
	}
	
	qglColor3f (1,1,1);

	BSP_DrawTextureChains ( false );	

	R_InitSun();

	qglDepthMask(0);
	R_DrawSkyBox();
	qglDepthMask(1);
	
	R_DrawRSSurfaces();
}

/*
===============
R_MarkLeaves

Mark the leaves and nodes that are in the PVS for the current
cluster
===============
*/
void R_MarkLeaves (void)
{
	static byte	*vis;
	static byte	fatvis[MAX_MAP_LEAFS/8];
	mnode_t	*node;
	int		i, c;
	mleaf_t	*leaf;
	int		cluster;
	static int minleaf_allareas, maxleaf_allareas;
	int minleaf, maxleaf;
	
	if	(	r_oldviewcluster == r_viewcluster && 
			r_oldviewcluster2 == r_viewcluster2 && 
			!r_novis->integer && r_viewcluster != -1 &&
			!r_newrefdef.areabits_changed)
		return;
	r_newrefdef.areabits_changed = false;
	
	// development aid to let you run around and see exactly where
	// the pvs ends
	if (gl_lockpvs->integer)
		return;

	r_oldviewcluster = r_viewcluster;
	r_oldviewcluster2 = r_viewcluster2;

	if (r_novis->integer || r_viewcluster == -1 || !r_worldmodel->vis)
	{
		r_visframecount++;
		// mark everything
		for (i=0 ; i<r_worldmodel->numleafs ; i++)
			r_worldmodel->leafs[i].visframe = r_visframecount;
		for (i=0 ; i<r_worldmodel->numnodes ; i++)
			r_worldmodel->nodes[i].visframe = r_visframecount;
		return;
	}

	vis = Mod_ClusterPVS (r_viewcluster, r_worldmodel);
	minleaf_allareas = r_viewleaf->minPVSleaf;
	maxleaf_allareas = r_viewleaf->maxPVSleaf;
	// may have to combine two clusters because of solid water boundaries
	if (r_viewcluster2 != r_viewcluster)
	{
		if (r_viewleaf2->minPVSleaf < minleaf_allareas)
			minleaf_allareas = r_viewleaf2->minPVSleaf;
		if (r_viewleaf2->maxPVSleaf > maxleaf_allareas)
			maxleaf_allareas = r_viewleaf2->maxPVSleaf;
		memcpy (fatvis, vis, (r_worldmodel->numleafs+7)/8);
		vis = Mod_ClusterPVS (r_viewcluster2, r_worldmodel);
		c = (r_worldmodel->numleafs+31)/32;
		for (i=0 ; i<c ; i++)
			((int *)fatvis)[i] |= ((int *)vis)[i];
		vis = fatvis;
	}
	
	r_visframecount++;
	
	minleaf = minleaf_allareas;
	maxleaf = maxleaf_allareas;
	
	//restrict leaf range further to the range leafs in connected areas.
	{
		int areamax = 0;
		int areamin = r_worldmodel->numleafs;
		for (i = 0; i < r_worldmodel->num_areas; i++)
		{
			if (r_newrefdef.areabits[i>>3] & (1<<(i&7)))
			{
				if (r_worldmodel->area_max_leaf[i] > areamax)
					areamax = r_worldmodel->area_max_leaf[i];
				if (r_worldmodel->area_min_leaf[i] < areamin)
					areamin = r_worldmodel->area_min_leaf[i];
			}
		}
		if (areamin < areamax)
		{
			if (areamin > minleaf)
				minleaf = areamin;
			if (areamax < maxleaf)
				maxleaf = areamax;
		}
	}
	
	for (i=minleaf,leaf=r_worldmodel->leafs+minleaf ; i<=maxleaf ; i++, leaf++)
	{
		cluster = leaf->cluster;
		if (cluster == -1)
			continue;
		if (vis[cluster>>3] & (1<<(cluster&7)))
		{
			node = (mnode_t *)leaf;
			do
			{
				if (node->visframe == r_visframecount)
					break;
				node->visframe = r_visframecount;
				node = node->parent;
			} while (node);
		}
	}
}

/*
===============
R_MarkWorldSurfs

Mark all surfaces that will need to be drawn this frame
===============
*/
void R_MarkWorldSurfs (void)
{
	static entity_t	ent;
	static int		old_visframecount, old_dlightcount, last_bsp_time;
	static vec3_t	old_origin, old_angle;
	vec3_t			delta_origin, delta_angle;
	qboolean		do_bsp;
	int				cur_ms;
	
	if (!r_drawworld->integer)
		return;

	if ( r_newrefdef.rdflags & RDF_NOWORLDMODEL )
		return;
	
	if (r_newrefdef.areabits == NULL)
	{
		Com_Printf ("WARN: No area bits!\n");
		return;
	}
	
	R_MarkLeaves ();

	currentmodel = r_worldmodel;

	VectorCopy (r_newrefdef.vieworg, modelorg);

	// auto cycle the world frame for texture animation
	memset (&ent, 0, sizeof(ent));
	ent.frame = (int)(r_newrefdef.time*2);
	currententity = &ent;

	gl_state.currenttextures[0] = gl_state.currenttextures[1] = -1;

	R_CalcWorldLights();

	// r_optimize: only re-recurse the BSP tree if the player has moved enough
	// to matter.
	VectorSubtract (r_origin, old_origin, delta_origin);
	VectorSubtract (r_newrefdef.viewangles, old_angle, delta_angle);
	do_bsp =	old_visframecount != r_visframecount ||
				VectorLength (delta_origin) > 5.0 ||
				VectorLength (delta_angle) > 2.0 ||
				r_newrefdef.num_dlights != 0 ||
				old_dlightcount != 0 ||
				!r_optimize->integer || draw_sun;
	
	cur_ms = Sys_Milliseconds ();
	
	// After a certain amount of time, increase the sensitivity to movement
	// and angle. If we go too long without re-recursing the BSP tree, it 
	// means the player is either moving very slowly or not moving at all. If
	// the player is moving slowly enough, it can catch the r_optimize code
	// napping and cause artefacts, so we should be extra vigilant just in 
	// case. Something you basically have to do on purpose, but we go the 
	// extra mile.
	if (r_optimize->integer && !do_bsp)
	{
		// be sure to handle integer overflow of the millisecond counter
		if (cur_ms < last_bsp_time || last_bsp_time+100 < cur_ms)
			do_bsp =	VectorLength (delta_origin) > 0.5 ||
						VectorLength (delta_angle) > 0.2;
			
	}
	
	if (do_bsp)
	{
		R_ClearSkyBox ();
		BSP_ClearWorldTextureChains ();
		BSP_RecursiveWorldNode (r_worldmodel->nodes, 15);
		
		old_visframecount = r_visframecount;
		VectorCopy (r_origin, old_origin);
		VectorCopy (r_newrefdef.viewangles, old_angle);
		last_bsp_time = cur_ms;
	}
	old_dlightcount = r_newrefdef.num_dlights;
}



/*
=============================================================================

  LIGHTMAP ALLOCATION
  
  TODO: move this to another file, this is load-time stuff that doesn't run
  every frame.

=============================================================================
*/

static void LM_InitBlock (void)
{
	memset( gl_lms.allocated, 0, sizeof( gl_lms.allocated ) );
}

// Upload the current lightmap data to OpenGL, then clear it and prepare for
// the next lightmap texture to be filled with data. 
// TODO: With HD lightmaps, are mipmaps a good idea here?
// TODO: Opportunistically make lightmap texture smaller if not all is used?
// Will require delaying lightmap texcoords for all surfaces until after
// upload.
static void LM_UploadBlock (void)
{
	int texture = gl_lms.current_lightmap_texture;

	GL_SelectTexture (0);
	GL_Bind( gl_state.lightmap_textures + texture );
	qglTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
	qglTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
	
	qglTexImage2D( GL_TEXTURE_2D,
				   0,
				   GL_RGBA,
				   LIGHTMAP_SIZE, LIGHTMAP_SIZE,
				   0,
				   GL_LIGHTMAP_FORMAT,
				   GL_UNSIGNED_INT_8_8_8_8_REV,
				   gl_lms.lightmap_buffer );
	
#if 0
	height = 0;
	for (i = 0; i < LIGHTMAP_SIZE; i++)
	{
		if (gl_lms.allocated[i] > height)
			height = gl_lms.allocated[i];
	}
	
	Com_Printf (" LIGHTMAP %d HEIGHT %d\n", gl_lms.current_lightmap_texture, height);
#endif
	
	if ( ++gl_lms.current_lightmap_texture == MAX_LIGHTMAPS )
		Com_Error( ERR_DROP, "LM_UploadBlock() - MAX_LIGHTMAPS exceeded\n" );
}

// LM_AllocBlock - given a certain size rectangle, allocates space inside the
// lightmap texture atlas.
// Returns a texture number and the position inside it.
// TODO: there are some clever tricks I can think of to pack these more
// tightly: opportunistically rotating lightmap textures by 90 degrees, 
// wrapping the lightmap textures around the edges of the atlas, and even
// recognizing that not all surfaces are rectangular.
static qboolean LM_AllocBlock (int w, int h, int *x, int *y)
{
	int		i, j;
	int		best, best2;

	best = LIGHTMAP_SIZE;

	for (i=0 ; i<LIGHTMAP_SIZE-w ; i++)
	{
		best2 = 0;

		for (j=0 ; j<w ; j++)
		{
			if (gl_lms.allocated[i+j] >= best)
				break;
			if (gl_lms.allocated[i+j] > best2)
				best2 = gl_lms.allocated[i+j];
		}
		if (j == w)
		{	// this is a valid spot
			*x = i;
			*y = best = best2;
		}
	}

	if (best + h > LIGHTMAP_SIZE)
		return false;

	for (i=0 ; i<w ; i++)
		gl_lms.allocated[*x + i] = best + h;

	return true;
}

/*
================
BSP_BuildPolygonFromSurface
================
*/
void BSP_BuildPolygonFromSurface(msurface_t *fa, float xscale, float yscale, int light_s, int light_t, int firstedge, int lnumverts)
{
	int			i, lindex;
	medge_t		*r_pedge;
	float		*vec;
	float		s, t;
	glpoly_t	*poly;

	vec3_t surfmaxs = {-99999999, -99999999, -99999999};
	vec3_t surfmins = {99999999, 99999999, 99999999};

	//
	// draw texture
	//
	poly = Hunk_Alloc (sizeof(glpoly_t) + (lnumverts-4) * VERTEXSIZE*sizeof(float));
	poly->next = fa->polys;
	poly->numverts = lnumverts;
	fa->polys = poly;

	for (i=0 ; i<lnumverts ; i++)
	{
		lindex = currentmodel->surfedges[firstedge + i];

		if (lindex > 0)
		{
			r_pedge = &currentmodel->edges[lindex];
			vec = currentmodel->vertexes[r_pedge->v[0]].position;
		}
		else
		{
			r_pedge = &currentmodel->edges[-lindex];
			vec = currentmodel->vertexes[r_pedge->v[1]].position;
		}

		s = DotProduct (vec, fa->texinfo->vecs[0]) + fa->texinfo->vecs[0][3];
		s /= fa->texinfo->image->width;

		t = DotProduct (vec, fa->texinfo->vecs[1]) + fa->texinfo->vecs[1][3];
		t /= fa->texinfo->image->height;

		VectorCopy (vec, poly->verts[i]);
		poly->verts[i][3] = s;
		poly->verts[i][4] = t;

		// set bbox for the surf used for culling
		if (vec[0] > surfmaxs[0]) surfmaxs[0] = vec[0];
		if (vec[1] > surfmaxs[1]) surfmaxs[1] = vec[1];
		if (vec[2] > surfmaxs[2]) surfmaxs[2] = vec[2];

		if (vec[0] < surfmins[0]) surfmins[0] = vec[0];
		if (vec[1] < surfmins[1]) surfmins[1] = vec[1];
		if (vec[2] < surfmins[2]) surfmins[2] = vec[2];

		//
		// lightmap texture coordinates
		//
		s = DotProduct (vec, fa->texinfo->vecs[0]) + fa->texinfo->vecs[0][3];
		s -= fa->texturemins[0];
		s += light_s*xscale;
		s += xscale/2.0;
		s /= LIGHTMAP_SIZE*xscale;

		t = DotProduct (vec, fa->texinfo->vecs[1]) + fa->texinfo->vecs[1][3];
		t -= fa->texturemins[1];
		t += light_t*yscale;
		t += yscale/2.0;
		t /= LIGHTMAP_SIZE*yscale;

		poly->verts[i][5] = s;
		poly->verts[i][6] = t;

		//to do - check if needed
		s = DotProduct (vec, fa->texinfo->vecs[0]) + fa->texinfo->vecs[0][3];
		s /= 128;

		t = DotProduct (vec, fa->texinfo->vecs[1]) + fa->texinfo->vecs[1][3];
		t /= 128;

		poly->verts[i][7] = s;
		poly->verts[i][8] = t;
		
	}
#if 0 //SO PRETTY and useful for checking how much lightmap data is used up
	if (lnumverts == 4)
	{
		poly->verts[0][5] = 1;
		poly->verts[0][6] = 1;
		poly->verts[1][5] = 1;
		poly->verts[1][6] = 0;
		poly->verts[2][5] = 0;
		poly->verts[2][6] = 0;
		poly->verts[3][5] = 0;
		poly->verts[3][6] = 1;
	}
#endif

	// store out the completed bbox
	VectorCopy (surfmins, fa->mins);
	VectorCopy (surfmaxs, fa->maxs);
}

/*
========================
BSP_CreateSurfaceLightmap
========================
*/
void BSP_CreateSurfaceLightmap (msurface_t *surf, int smax, int tmax, int *light_s, int *light_t)
{
	byte	*base;

	if (!surf->samples)
		return;

	if (surf->texinfo->flags & (SURF_SKY|SURF_WARP))
		return; //may not need this?

	if ( !LM_AllocBlock( smax, tmax, light_s, light_t ) )
	{
		LM_UploadBlock( );
		LM_InitBlock();
		if ( !LM_AllocBlock( smax, tmax, light_s, light_t ) )
		{
			Com_Error( ERR_FATAL, "Consecutive calls to LM_AllocBlock(%d,%d) failed\n", smax, tmax );
		}
	}

	surf->lightmaptexturenum = gl_lms.current_lightmap_texture;
	
	surf->lightmins[0] = *light_s;
	surf->lightmins[1] = *light_t;
	surf->lightmaxs[0] = smax;
	surf->lightmaxs[1] = tmax;

	base = gl_lms.lightmap_buffer;
	base += ((*light_t) * LIGHTMAP_SIZE + *light_s) * LIGHTMAP_BYTES;

	R_SetCacheState( surf );
	R_BuildLightMap (surf, base, smax, tmax, LIGHTMAP_SIZE*LIGHTMAP_BYTES);
}

void BSP_UpdateSurfaceLightmap (msurface_t *surf)
{
	R_SetCacheState (surf);
	R_BuildLightMap (surf, gl_lms.lightmap_buffer, surf->lightmaxs[0], surf->lightmaxs[1], surf->lightmaxs[0]*LIGHTMAP_BYTES);
	
	GL_SelectTexture (0);
	GL_Bind( gl_state.lightmap_textures + surf->lightmaptexturenum );
	qglTexSubImage2D( GL_TEXTURE_2D, 
					  0,
					  surf->lightmins[0], surf->lightmins[1],
					  surf->lightmaxs[0], surf->lightmaxs[1],
					  GL_LIGHTMAP_FORMAT,
					  GL_UNSIGNED_INT_8_8_8_8_REV,
					  gl_lms.lightmap_buffer );
}

/*
==================
BSP_BeginBuildingLightmaps
==================
*/
void BSP_BeginBuildingLightmaps (model_t *m)
{
	static lightstyle_t	lightstyles[MAX_LIGHTSTYLES];
	int				i;

	memset( gl_lms.allocated, 0, sizeof(gl_lms.allocated) );

	r_framecount = 1;		// no dlightcache

	GL_EnableMultitexture( true );
	GL_SelectTexture (1);

	/*
	** setup the base lightstyles so the lightmaps won't have to be regenerated
	** the first time they're seen
	*/
	for (i=0 ; i<MAX_LIGHTSTYLES ; i++)
	{
		lightstyles[i].rgb[0] = 1;
		lightstyles[i].rgb[1] = 1;
		lightstyles[i].rgb[2] = 1;
		lightstyles[i].white = 3;
	}
	r_newrefdef.lightstyles = lightstyles;

	if (!gl_state.lightmap_textures)
		gl_state.lightmap_textures	= TEXNUM_LIGHTMAPS;

	gl_lms.current_lightmap_texture = 1;

}

/*
=======================
BSP_EndBuildingLightmaps
=======================
*/
void BSP_EndBuildingLightmaps (void)
{
	LM_UploadBlock( );
	GL_EnableMultitexture( false );
}



/*
=============================================================================

  MINI MAP
  
  Draws a little 2D map in the corner of the HUD.
  TODO: do a bit more calculation at load time, right now this is a huge FPS
  hit.

=============================================================================
*/

void R_RecursiveRadarNode (mnode_t *node)
{
	int			c, side;
	cplane_t	*plane;
	msurface_t	*surf, **mark;
	mleaf_t		*pleaf;
	float		dot,distance;
	glpoly_t	*p;
	float		*v;
	int			i;

	if (node->contents == CONTENTS_SOLID)	return;		// solid

	if(r_minimap_zoom->value>=0.1) {
		distance = 1024.0/r_minimap_zoom->value;
	} else {
		distance = 1024.0;
	}


	if ( r_origin[0]+distance < node->minmaxs[0] ||
		 r_origin[0]-distance > node->minmaxs[3] ||
		 r_origin[1]+distance < node->minmaxs[1] ||
		 r_origin[1]-distance > node->minmaxs[4] ||
		 r_origin[2]+256 < node->minmaxs[2] ||
		 r_origin[2]-256 > node->minmaxs[5]) return;

	// if a leaf node, draw stuff
	if (node->contents != -1) {
		pleaf = (mleaf_t *)node;
		// check for door connected areas
		if (! (r_newrefdef.areabits[pleaf->area>>3] & (1<<(pleaf->area&7)) ) )
			return; // not visible
		mark = pleaf->firstmarksurface;
		c = pleaf->nummarksurfaces;

		if (c) {
			do {
				(*mark)->visframe = r_framecount;
				mark++;
			} while (--c);
		}
		return;
	}

// node is just a decision point, so go down the apropriate sides
// find which side of the node we are on
	plane = node->plane;

	switch (plane->type) {
	case PLANE_X:
		dot = modelorg[0] - plane->dist;
		break;
	case PLANE_Y:
		dot = modelorg[1] - plane->dist;
		break;
	case PLANE_Z:
		dot = modelorg[2] - plane->dist;
		break;
	default:
		dot = DotProduct (modelorg, plane->normal) - plane->dist;
		break;
	}

	if (dot >= 0) {
		side = 0;
	} else {
		side = 1;
	}

// recurse down the children, front side first
	R_RecursiveRadarNode (node->children[side]);

  if(plane->normal[2]) {
		// draw stuff
		if(plane->normal[2]>0) for ( c = node->numsurfaces, surf = r_worldmodel->surfaces + node->firstsurface; c ; c--, surf++)
		{
			if (surf->texinfo->flags & SURF_SKY){
				continue;
			}


		}
	} else {
			qglDisable(GL_TEXTURE_2D);
		for ( c = node->numsurfaces, surf = r_worldmodel->surfaces + node->firstsurface; c ; c--, surf++) {
			float sColor,C[4];
			if (surf->texinfo->flags & SURF_SKY) continue;

			if (surf->texinfo->flags & (SURF_WARP|SURF_FLOWING|SURF_TRANS33|SURF_TRANS66)) {
				sColor=0.5;
			} else {
				sColor=0;
			}

			for ( p = surf->polys; p; p = p->chain ) {
				v = p->verts[0];
				qglBegin(GL_LINE_STRIP);
				for (i=0 ; i< p->numverts; i++, v+= VERTEXSIZE) {
					C[3]= (v[2]-r_origin[2])/512.0;
					if (C[3]>0) {

						C[0]=0.5;
						C[1]=0.5+sColor;
						C[2]=0.5;
						C[3]=1-C[3];

					}
					   else
					{
						C[0]=0.5;
						C[1]=sColor;
						C[2]=0;
						C[3]+=1;

					}

					if(C[3]<0) {
						C[3]=0;

					}
					qglColor4fv(C);
					qglVertex3fv (v);
				}

				qglEnd();
			}
		}
		qglEnable(GL_TEXTURE_2D);

  }
	// recurse down the back side
	R_RecursiveRadarNode (node->children[!side]);


}

int			numRadarEnts=0;
RadarEnt_t	RadarEnts[MAX_RADAR_ENTS];

void R_DrawRadar(void)
{
	int		i;
	float	fS[4]={0,0,-1.0/512.0,0};

	if ( r_newrefdef.rdflags & RDF_NOWORLDMODEL ) return;
	if(!r_minimap->integer) return;
	
	if (!r_newrefdef.areabits) return;

	qglViewport	(	r_newrefdef.x+r_newrefdef.width-r_minimap_size->value,
					vid.height-r_newrefdef.y-r_newrefdef.height, 
					r_minimap_size->value, r_minimap_size->value);

	qglDisable(GL_DEPTH_TEST);
  	qglMatrixMode(GL_PROJECTION);
	qglPushMatrix();
	qglLoadIdentity ();


	if (r_minimap_style->integer) {
		qglOrtho(-1024,1024,-1024,1024,-256,256);
	} else {
		qglOrtho(-1024,1024,-512,1536,-256,256);
	}

	qglMatrixMode(GL_MODELVIEW);
	qglPushMatrix();
	qglLoadIdentity ();


		{
		qglStencilMask(255);
		qglClear(GL_STENCIL_BUFFER_BIT);
		qglEnable(GL_STENCIL_TEST);
		qglStencilFunc(GL_ALWAYS,4,4);
		qglStencilOp(GL_KEEP,GL_KEEP,GL_REPLACE);


		GLSTATE_ENABLE_ALPHATEST;
		qglAlphaFunc(GL_LESS,0.1);
		qglColorMask(0,0,0,0);

		qglColor4f(1,1,1,1);
		if(r_around)
			GL_Bind(r_around->texnum);
		qglBegin(GL_TRIANGLE_FAN);
		if (r_minimap_style->integer){
			qglTexCoord2f(0,1); qglVertex3f(1024,-1024,1);
			qglTexCoord2f(1,1); qglVertex3f(-1024,-1024,1);
			qglTexCoord2f(1,0); qglVertex3f(-1024,1024,1);
			qglTexCoord2f(0,0); qglVertex3f(1024,1024,1);
		} else {
			qglTexCoord2f(0,1); qglVertex3f(1024,-512,1);
			qglTexCoord2f(1,1); qglVertex3f(-1024,-512,1);
			qglTexCoord2f(1,0); qglVertex3f(-1024,1536,1);
			qglTexCoord2f(0,0); qglVertex3f(1024,1536,1);
		}
		qglEnd();

		qglColorMask(1,1,1,1);
		GLSTATE_DISABLE_ALPHATEST;
		qglAlphaFunc(GL_GREATER, 0.5);
		qglStencilOp(GL_KEEP,GL_KEEP,GL_REPLACE);
		qglStencilFunc(GL_NOTEQUAL,4,4);

	}

	if(r_minimap_zoom->value>=0.1) {
		qglScalef(r_minimap_zoom->value,r_minimap_zoom->value,r_minimap_zoom->value);
	}

	if (r_minimap_style->integer) {
		qglPushMatrix();
		qglRotatef (90-r_newrefdef.viewangles[1],  0, 0, -1);

		qglDisable(GL_TEXTURE_2D);
		qglBegin(GL_TRIANGLES);
		qglColor4f(1,1,1,0.5);
		qglVertex3f(0,32,0);
		qglColor4f(1,1,0,0.5);
		qglVertex3f(24,-32,0);
		qglVertex3f(-24,-32,0);
		qglEnd();

		qglPopMatrix();
	} else {
		qglDisable(GL_TEXTURE_2D);
		qglBegin(GL_TRIANGLES);
		qglColor4f(1,1,1,0.5);
		qglVertex3f(0,32,0);
		qglColor4f(1,1,0,0.5);
		qglVertex3f(24,-32,0);
		qglVertex3f(-24,-32,0);
		qglEnd();

		qglRotatef (90-r_newrefdef.viewangles[1],  0, 0, 1);
	}
	qglTranslatef (-r_newrefdef.vieworg[0],  -r_newrefdef.vieworg[1],  -r_newrefdef.vieworg[2]);

	qglBegin(GL_QUADS);
	for(i=0;i<numRadarEnts;i++){
		float x=RadarEnts[i].org[0];
		float y=RadarEnts[i].org[1];
		float z=RadarEnts[i].org[2];
		qglColor4fv(RadarEnts[i].color);

		qglVertex3f(x+9, y+9, z);
		qglVertex3f(x+9, y-9, z);
		qglVertex3f(x-9, y-9, z);
		qglVertex3f(x-9, y+9, z);
	}
	qglEnd();

	qglEnable(GL_TEXTURE_2D);

	if(r_radarmap)
		GL_Bind(r_radarmap->texnum);
	qglBlendFunc(GL_SRC_ALPHA, GL_ONE);
	GLSTATE_ENABLE_BLEND;
	qglColor3f(1,1,1);

	fS[3]=0.5+ r_newrefdef.vieworg[2]/512.0;
	qglTexGenf(GL_S,GL_TEXTURE_GEN_MODE,GL_OBJECT_LINEAR);

	GLSTATE_ENABLE_TEXGEN;
	qglTexGenfv(GL_S,GL_OBJECT_PLANE,fS);

	// draw the stuff
	R_RecursiveRadarNode (r_worldmodel->nodes);

	qglBlendFunc(GL_SRC_ALPHA,GL_ONE_MINUS_SRC_ALPHA);
	GLSTATE_DISABLE_TEXGEN;

	qglPopMatrix();

	qglViewport(0,0,vid.width,vid.height);

	qglMatrixMode(GL_PROJECTION);
	qglPopMatrix();
	qglMatrixMode(GL_MODELVIEW);
	qglDisable(GL_STENCIL_TEST);
	GL_TexEnv( GL_REPLACE );
	GLSTATE_DISABLE_BLEND;
	qglEnable(GL_DEPTH_TEST);
	qglColor4f(1,1,1,1);

}
