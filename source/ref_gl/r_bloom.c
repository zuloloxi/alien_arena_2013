/*
Copyright (C) 1997-2001 Id Software, Inc.

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
// r_bloom.c: 2D lighting post process effect

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "r_local.h"

/*
==============================================================================

						LIGHT BLOOMS

==============================================================================
*/

static float Diamond8x[8][8] = {
		{0.0f, 0.0f, 0.0f, 0.1f, 0.1f, 0.0f, 0.0f, 0.0f},
		{0.0f, 0.0f, 0.2f, 0.3f, 0.3f, 0.2f, 0.0f, 0.0f},
		{0.0f, 0.2f, 0.4f, 0.6f, 0.6f, 0.4f, 0.2f, 0.0f},
		{0.1f, 0.3f, 0.6f, 0.9f, 0.9f, 0.6f, 0.3f, 0.1f},
		{0.1f, 0.3f, 0.6f, 0.9f, 0.9f, 0.6f, 0.3f, 0.1f},
		{0.0f, 0.2f, 0.4f, 0.6f, 0.6f, 0.4f, 0.2f, 0.0f},
		{0.0f, 0.0f, 0.2f, 0.3f, 0.3f, 0.2f, 0.0f, 0.0f},
		{0.0f, 0.0f, 0.0f, 0.1f, 0.1f, 0.0f, 0.0f, 0.0f} };

static float Diamond6x[6][6] = {
		{0.0f, 0.0f, 0.1f, 0.1f, 0.0f, 0.0f},
		{0.0f, 0.3f, 0.5f, 0.5f, 0.3f, 0.0f},
		{0.1f, 0.5f, 0.9f, 0.9f, 0.5f, 0.1f},
		{0.1f, 0.5f, 0.9f, 0.9f, 0.5f, 0.1f},
		{0.0f, 0.3f, 0.5f, 0.5f, 0.3f, 0.0f},
		{0.0f, 0.0f, 0.1f, 0.1f, 0.0f, 0.0f} };

static float Diamond4x[4][4] = {
		{0.3f, 0.4f, 0.4f, 0.3f},
		{0.4f, 0.9f, 0.9f, 0.4f},
		{0.4f, 0.9f, 0.9f, 0.4f},
		{0.3f, 0.4f, 0.4f, 0.3f} };


static int		BLOOM_SIZE;

cvar_t		*r_bloom_alpha;
cvar_t		*r_bloom_diamond_size;
cvar_t		*r_bloom_intensity;
cvar_t		*r_bloom_darken;
cvar_t		*r_bloom_sample_size;
cvar_t		*r_bloom_fast_sample;

image_t	*r_bloomscratchtexture;
image_t	*r_bloomeffecttexture;
image_t	*r_bloomdownsamplingtexture;
static GLuint bloomscratchFBO, bloomdownsamplingFBO, bloomeffectFBO;

static int		r_screendownsamplingtexture_size;
static int		screen_texture_width, screen_texture_height;


//current refdef size:
static int	curView_x;
static int	curView_y;
static int	curView_width;
static int	curView_height;

//texture coordinates of screen data inside screentexture
static float screenText_tcw;
static float screenText_tch;

static int	sample_width;
static int	sample_height;

//texture coordinates of adjusted textures
static float sampleText_tcw;
static float sampleText_tch;

//this macro is in sample size workspace coordinates
#define R_Bloom_SamplePass( xpos, ypos )							\
	qglBegin(GL_QUADS);												\
	qglTexCoord2f(	0,						sampleText_tch);		\
	qglVertex2f(	xpos,					ypos);					\
	qglTexCoord2f(	0,						0);						\
	qglVertex2f(	xpos,					ypos+sample_height);	\
	qglTexCoord2f(	sampleText_tcw,			0);						\
	qglVertex2f(	xpos+sample_width,		ypos+sample_height);	\
	qglTexCoord2f(	sampleText_tcw,			sampleText_tch);		\
	qglVertex2f(	xpos+sample_width,		ypos);					\
	qglEnd();

#define R_Bloom_Quad( x, y, width, height, textwidth, textheight )	\
	qglBegin(GL_QUADS);												\
	qglTexCoord2f(	0,			textheight);						\
	qglVertex2f(	x,			y);									\
	qglTexCoord2f(	0,			0);									\
	qglVertex2f(	x,			y+height);							\
	qglTexCoord2f(	textwidth,	0);									\
	qglVertex2f(	x+width,	y+height);							\
	qglTexCoord2f(	textwidth,	textheight);						\
	qglVertex2f(	x+width,	y);									\
	qglEnd();



/*
=================
R_Bloom_InitEffectTexture
=================
*/
void R_Bloom_InitEffectTexture( void )
{
	byte	*data;
	float	bloomsizecheck;

	if( r_bloom_sample_size->integer < 32 )
		Cvar_SetValue ("r_bloom_sample_size", 32);

	//make sure bloom size is a power of 2
	BLOOM_SIZE = r_bloom_sample_size->integer;
	bloomsizecheck = (float)BLOOM_SIZE;
	while(bloomsizecheck > 1.0f) bloomsizecheck /= 2.0f;
	if( bloomsizecheck != 1.0f )
	{
		BLOOM_SIZE = 32;
		while( BLOOM_SIZE < r_bloom_sample_size->integer )
			BLOOM_SIZE *= 2;
	}

	//make sure bloom size doesn't have stupid values
	if( BLOOM_SIZE > screen_texture_width ||
		BLOOM_SIZE > screen_texture_height )
		BLOOM_SIZE = min( screen_texture_width, screen_texture_height );

	if( BLOOM_SIZE != r_bloom_sample_size->integer )
		Cvar_SetValue ("r_bloom_sample_size", BLOOM_SIZE);

	data = malloc( BLOOM_SIZE * BLOOM_SIZE * 4 );
	memset( data, 0, BLOOM_SIZE * BLOOM_SIZE * 4 );

	r_bloomeffecttexture = GL_LoadPic( "***r_bloomeffecttexture***", (byte *)data, BLOOM_SIZE, BLOOM_SIZE, it_pic, 3 );

	free ( data );
	
	qglGenFramebuffersEXT(1, &bloomeffectFBO);
	qglBindFramebufferEXT(GL_FRAMEBUFFER_EXT,bloomeffectFBO);
	qglFramebufferTexture2DEXT(GL_FRAMEBUFFER_EXT, GL_COLOR_ATTACHMENT0_EXT, GL_TEXTURE_2D, r_bloomeffecttexture->texnum, 0);
	qglBindFramebufferEXT(GL_FRAMEBUFFER_EXT,0);
}

/*
=================
R_Bloom_InitTextures
=================
*/
void checkFBOExtensions (void);
void R_Bloom_InitTextures( void )
{
	byte	*data;
	int		size;
	
	if (!gl_state.fbo || !gl_state.hasFBOblit)
	{
		Com_Printf ("FBO Failed, disabling bloom.\n");
		Cvar_SetValue ("r_bloom", 0);
		return;
	}

	//find closer power of 2 to screen size
	for (screen_texture_width = 1;screen_texture_width < viddef.width;screen_texture_width *= 2);
	for (screen_texture_height = 1;screen_texture_height < viddef.height;screen_texture_height *= 2);

	//validate bloom size and init the bloom effect texture
	R_Bloom_InitEffectTexture();

	//init the "scratch" texture
	size = BLOOM_SIZE * BLOOM_SIZE * 4;
	data = malloc( size );
	memset( data, 255, size );
	r_bloomscratchtexture = GL_LoadPic( "***r_bloomscratchtexture***", (byte *)data, BLOOM_SIZE, BLOOM_SIZE, it_pic, 3 );
	free ( data );
	qglGenFramebuffersEXT(1, &bloomscratchFBO);
	qglBindFramebufferEXT(GL_FRAMEBUFFER_EXT,bloomscratchFBO);
	qglFramebufferTexture2DEXT(GL_FRAMEBUFFER_EXT, GL_COLOR_ATTACHMENT0_EXT, GL_TEXTURE_2D, r_bloomscratchtexture->texnum, 0);
	qglBindFramebufferEXT(GL_FRAMEBUFFER_EXT,0);

	//if screensize is more than 2x the bloom effect texture, set up for stepped downsampling
	r_bloomdownsamplingtexture = NULL;
	r_screendownsamplingtexture_size = 0;
	if( viddef.width > (BLOOM_SIZE * 2) && !r_bloom_fast_sample->integer )
		r_screendownsamplingtexture_size = (int)(BLOOM_SIZE * 2);
	else
		r_screendownsamplingtexture_size = BLOOM_SIZE;
	data = malloc( r_screendownsamplingtexture_size * r_screendownsamplingtexture_size * 4 );
	memset( data, 0, r_screendownsamplingtexture_size * r_screendownsamplingtexture_size * 4 );
	r_bloomdownsamplingtexture = GL_LoadPic( "***r_bloomdownsamplingtexture***", (byte *)data, r_screendownsamplingtexture_size, r_screendownsamplingtexture_size, it_pic, 3 );
	free ( data );
	qglGenFramebuffersEXT(1, &bloomdownsamplingFBO);
	qglBindFramebufferEXT(GL_FRAMEBUFFER_EXT,bloomdownsamplingFBO);
	qglFramebufferTexture2DEXT(GL_FRAMEBUFFER_EXT, GL_COLOR_ATTACHMENT0_EXT, GL_TEXTURE_2D, r_bloomdownsamplingtexture->texnum, 0);
	qglBindFramebufferEXT(GL_FRAMEBUFFER_EXT,0);

}

/*
=================
R_InitBloomTextures
=================
*/
void R_InitBloomTextures( void )
{

	r_bloom = Cvar_Get( "r_bloom", "0", CVAR_ARCHIVE );
	r_bloom_alpha = Cvar_Get( "r_bloom_alpha", "0.2", CVAR_ARCHIVE );
	r_bloom_diamond_size = Cvar_Get( "r_bloom_diamond_size", "8", CVAR_ARCHIVE );
	r_bloom_intensity = Cvar_Get( "r_bloom_intensity", "0.5", CVAR_ARCHIVE );
	r_bloom_darken = Cvar_Get( "r_bloom_darken", "8", CVAR_ARCHIVE );
	r_bloom_sample_size = Cvar_Get( "r_bloom_sample_size", "128", CVAR_ARCHIVE );
	r_bloom_fast_sample = Cvar_Get( "r_bloom_fast_sample", "0", CVAR_ARCHIVE );

	BLOOM_SIZE = 0;
	if( !r_bloom->integer )
		return;

	R_Bloom_InitTextures ();
}


/*
=================
R_Bloom_DrawEffect
=================
*/
void R_Bloom_DrawEffect( void )
{
	GL_Bind(r_bloomeffecttexture->texnum);
	qglEnable(GL_BLEND);
	qglBlendFunc(GL_ONE, GL_ONE);
	qglColor4f(r_bloom_alpha->value, r_bloom_alpha->value, r_bloom_alpha->value, 1.0f);
	GL_TexEnv(GL_MODULATE);
	qglBegin(GL_QUADS);
	qglTexCoord2f(	0,							sampleText_tch	);
	qglVertex2f(	curView_x,					curView_y	);
	qglTexCoord2f(	0,							0	);
	qglVertex2f(	curView_x,					curView_y + curView_height	);
	qglTexCoord2f(	sampleText_tcw,				0	);
	qglVertex2f(	curView_x + curView_width,	curView_y + curView_height	);
	qglTexCoord2f(	sampleText_tcw,				sampleText_tch	);
	qglVertex2f(	curView_x + curView_width,	curView_y	);
	qglEnd();

	qglDisable(GL_BLEND);
}


/*
=================
R_Bloom_GeneratexDiamonds
=================
*/
void R_Bloom_GeneratexDiamonds( void )
{
	int			i, j;
	static float intensity;
	GLenum err;

	//set up sample size workspace
	qglViewport( 0, 0, sample_width, sample_height );
	qglMatrixMode( GL_PROJECTION );
	qglLoadIdentity ();
	qglOrtho(0, sample_width, sample_height, 0, -10, 100);
	qglMatrixMode( GL_MODELVIEW );
	
	qglBindFramebufferEXT(GL_FRAMEBUFFER_EXT, bloomscratchFBO);
	GL_Bind(r_bloomeffecttexture->texnum);

	//start modifying the small scene corner
	qglColor4f( 1.0f, 1.0f, 1.0f, 1.0f );
	qglEnable(GL_BLEND);

	//darkening passes
	if( r_bloom_darken->integer )
	{
		qglBlendFunc(GL_DST_COLOR, GL_ZERO);
		GL_TexEnv(GL_MODULATE);
		
		for(i=0; i<r_bloom_darken->integer ;i++) {
			R_Bloom_SamplePass( 0, 0 );
		}
		
		qglCopyTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 0, 0, sample_width, sample_height);
	}

	//bluring passes
	//qglBlendFunc(GL_ONE, GL_ONE);
	qglBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_COLOR);
	
	if( r_bloom_diamond_size->integer > 7 || r_bloom_diamond_size->integer <= 3)
	{
		if( r_bloom_diamond_size->integer != 8 ) Cvar_SetValue( "r_bloom_diamond_size", 8 );

		for(i=0; i<r_bloom_diamond_size->integer; i++) {
			for(j=0; j<r_bloom_diamond_size->integer; j++) {
				intensity = r_bloom_intensity->value * 0.3 * Diamond8x[i][j];
				if( intensity < 0.01f ) continue;
				qglColor4f( intensity, intensity, intensity, 1.0);
				R_Bloom_SamplePass( i-4, j-4 );
			}
		}
	} else if( r_bloom_diamond_size->integer > 5 ) {

		if( r_bloom_diamond_size->integer != 6 ) Cvar_SetValue( "r_bloom_diamond_size", 6 );

		for(i=0; i<r_bloom_diamond_size->integer; i++) {
			for(j=0; j<r_bloom_diamond_size->integer; j++) {
				intensity = r_bloom_intensity->value * 0.5 * Diamond6x[i][j];
				if( intensity < 0.01f ) continue;
				qglColor4f( intensity, intensity, intensity, 1.0);
				R_Bloom_SamplePass( i-3, j-3 );
			}
		}
	} else if( r_bloom_diamond_size->integer > 3 ) {

		if( r_bloom_diamond_size->integer != 4 ) Cvar_SetValue( "r_bloom_diamond_size", 4 );

		for(i=0; i<r_bloom_diamond_size->integer; i++) {
			for(j=0; j<r_bloom_diamond_size->integer; j++) {
				intensity = r_bloom_intensity->value * 0.8f * Diamond4x[i][j];
				if( intensity < 0.01f ) continue;
				qglColor4f( intensity, intensity, intensity, 1.0);
				R_Bloom_SamplePass( i-2, j-2 );
			}
		}
	}
	
	qglCopyTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 0, 0, sample_width, sample_height);
	
	//restore full screen workspace
	qglViewport( 0, 0, viddef.width, viddef.height );
	qglMatrixMode( GL_PROJECTION );
	qglLoadIdentity ();
	qglOrtho(0, viddef.width, viddef.height, 0, -10, 100);
	qglMatrixMode( GL_MODELVIEW );
	qglLoadIdentity ();

	qglBindFramebufferEXT(GL_FRAMEBUFFER_EXT, 0);
}

/*
=================
R_Bloom_DownsampleView
=================
*/
void R_Bloom_DownsampleView( refdef_t *fd )
{
	GLenum err;
	qglDisable( GL_BLEND );
	qglColor4f( 1.0f, 1.0f, 1.0f, 1.0f );
	
	GL_SelectTexture (GL_TEXTURE0);
	// FIXME: OH FFS this is so stupid: tell the GL_Bind batching mechanism 
	// that texture unit 0 has been re-bound, as it most certainly has been.
	gl_state.currenttextures[gl_state.currenttmu] = -1;
	
	//stepped downsample
	if( r_screendownsamplingtexture_size )
	{
		int      midsample_width = (r_screendownsamplingtexture_size * sampleText_tcw);
		int      midsample_height = (r_screendownsamplingtexture_size * sampleText_tch);

		// copy into small sized FBO (equivalent to copying into full screen
		// sized FBO and then drawing that onto the small sized FBO later.)
		qglBindFramebufferEXT(GL_DRAW_FRAMEBUFFER_EXT, bloomscratchFBO);
		qglBlitFramebufferEXT(0, 0, vid.width, vid.height, 0, 0, BLOOM_SIZE, BLOOM_SIZE,
			GL_COLOR_BUFFER_BIT, GL_LINEAR);
		// copy into downsampling (mid-sized) FBO
		qglBindFramebufferEXT(GL_DRAW_FRAMEBUFFER_EXT, bloomdownsamplingFBO);
		qglBlitFramebufferEXT(0, 0, vid.width, vid.height, 0, 0, r_screendownsamplingtexture_size, r_screendownsamplingtexture_size,
			GL_COLOR_BUFFER_BIT, GL_LINEAR);
		
		
		// create the finished downsampled version of the texture by blending 
		// the small-sized FBO and the mid-sized FBO onto a small-sized FBO,
		// hoping it adds some blur.
		
		// Store first of all in the bloom effect texture, since we don't want
		// to draw the scratch texture onto itself. 
		qglBindFramebufferEXT(GL_DRAW_FRAMEBUFFER_EXT, bloomeffectFBO);
		
		// mid-size
		GL_Bind(r_bloomdownsamplingtexture->texnum);
		qglColor4f( 0.5f, 0.5f, 0.5f, 1.0f );
		R_Bloom_Quad( 0,  viddef.height-sample_height, sample_width, sample_height, 1.0, 1.0 );
		// small-size
		qglEnable( GL_BLEND );
		qglBlendFunc(GL_ONE, GL_ONE);
		qglColor4f( 0.5f, 0.5f, 0.5f, 1.0f );
		GL_Bind(r_bloomscratchtexture->texnum);
		R_Bloom_Quad( 0,  viddef.height-sample_height, sample_width, sample_height, 1.0, 1.0 );
		qglColor4f( 1.0f, 1.0f, 1.0f, 1.0f );
		qglDisable( GL_BLEND );
		
	} else {	//downsample simple

		qglBindFramebufferEXT(GL_DRAW_FRAMEBUFFER_EXT, bloomeffectFBO);
		qglBlitFramebufferEXT(0, 0, vid.width, vid.height, 0, 0, sample_width, sample_height,
			GL_COLOR_BUFFER_BIT, GL_LINEAR);
		
	}
	
	// Blit the finished downsampled texture onto a second FBO. We end up with
	// with two copies, which GenerateDiamonds will take advantage of.
	qglBindFramebufferEXT(GL_DRAW_FRAMEBUFFER_EXT, bloomscratchFBO);
	qglBindFramebufferEXT(GL_READ_FRAMEBUFFER_EXT, bloomeffectFBO);
	qglBlitFramebufferEXT(0, 0, sample_width, sample_height, 0, 0, sample_width, sample_height,
		GL_COLOR_BUFFER_BIT, GL_NEAREST);
	
	qglBindFramebufferEXT(GL_DRAW_FRAMEBUFFER_EXT, 0);
	qglBindFramebufferEXT(GL_READ_FRAMEBUFFER_EXT, 0);
}

/*
=================
R_BloomBlend
=================
*/
void R_BloomBlend ( refdef_t *fd )
{

	if( !(fd->rdflags & RDF_BLOOM) || !r_bloom->integer )
		return;

	if( !BLOOM_SIZE )
		R_Bloom_InitTextures();
	
	// previous function can set this if there's no FBO
	if (!r_bloom->integer)
		return;

	if( screen_texture_width < BLOOM_SIZE ||
		screen_texture_height < BLOOM_SIZE )
		return;

	//set up full screen workspace
	qglViewport( 0, 0, viddef.width, viddef.height );
	qglDisable( GL_DEPTH_TEST );
	qglMatrixMode( GL_PROJECTION );
	qglLoadIdentity ();
	qglOrtho(0, viddef.width, viddef.height, 0, -10, 100);
	qglMatrixMode( GL_MODELVIEW );
	qglLoadIdentity ();
	qglDisable(GL_CULL_FACE);

	qglDisable( GL_BLEND );
	qglEnable( GL_TEXTURE_2D );

	qglColor4f( 1, 1, 1, 1 );

	//set up current sizes
	// TODO: get rid of these nasty globals
	curView_x = fd->x;
	curView_y = fd->y;
	curView_width = fd->width;
	curView_height = fd->height;
	screenText_tcw = ((float)curView_width / (float)screen_texture_width);
	screenText_tch = ((float)curView_height / (float)screen_texture_height);
	if( fd->height > fd->width ) {
		sampleText_tcw = ((float)curView_width / (float)curView_height);
		sampleText_tch = 1.0f;
	} else {
		sampleText_tcw = 1.0f;
		sampleText_tch = ((float)curView_height / (float)curView_width);
	}
	sample_width = (BLOOM_SIZE * sampleText_tcw);
	sample_height = (BLOOM_SIZE * sampleText_tch);

	//create the bloom image
	R_Bloom_DownsampleView(fd);
	R_Bloom_GeneratexDiamonds();

	R_Bloom_DrawEffect();

	qglColor3f (1,1,1);
	qglDisable (GL_BLEND);
	qglEnable (GL_TEXTURE_2D);
	qglBlendFunc (GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
	qglDepthMask (1);
}

