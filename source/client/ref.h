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
#ifndef __REF_H
#define __REF_H

#include "qcommon/qcommon.h"

#define DIV254BY255 (0.9960784313725490196078431372549f)
#define DIV255 (0.003921568627450980392156862745098f)
#define DIV256 (0.00390625f)
#define DIV512 (0.001953125f)

#define	MAX_DLIGHTS			32
#define	MAX_ENTITIES		256 //was 128 - use of static meshes necessitates an increase.
#define	MAX_PARTICLES		8192
#define	MAX_LIGHTSTYLES		256

#define MAX_FLARES      512
#define MAX_GRASSES		2048
#define MAX_BEAMS		128

typedef struct
{
	vec3_t origin;
	vec3_t color;
	int size;
	int style;
	float alpha;
	float time;
	int leafnum;
} flare_t;

typedef struct
{
	int type;
	vec3_t origin;
	vec3_t color;
	float size;
	int texsize;
	int texnum;
	char name[MAX_QPATH];
	int leafnum;
	qboolean sunVisible; //can cast shadows in sunlight
	vec3_t static_light;
} grass_t;

typedef struct
{
	int type;
	vec3_t origin;
	vec3_t color;
	float size;
	float xang;
	float yang;
	qboolean rotating;
	int texsize;
	int texnum;
	char name[MAX_QPATH];
	int leafnum;
	int leafnum2;
} beam_t;

#define SHELL_RED_COLOR		0xF2
#define SHELL_GREEN_COLOR	0xD0
#define SHELL_BLUE_COLOR	0xF3

#define SHELL_RG_COLOR		0xDC
//#define SHELL_RB_COLOR		0x86
#define SHELL_RB_COLOR		0x68
#define SHELL_BG_COLOR		0x78

//ROGUE
#define SHELL_DOUBLE_COLOR	0xDF // 223
#define	SHELL_HALF_DAM_COLOR	0x90
#define SHELL_CYAN_COLOR	0x72
//ROGUE

#define SHELL_WHITE_COLOR	0xD7

#define PARTICLE_NONE				0
#define PARTICLE_STANDARD			1
#define PARTICLE_BEAM				2
#define PARTICLE_DECAL				3
#define PARTICLE_FLAT				4
#define PARTICLE_WEATHER			5
#define PARTICLE_FLUTTERWEATHER     6
#define PARTICLE_RAISEDDECAL		7
#define PARTICLE_ROTATINGYAW		8
#define PARTICLE_ROTATINGROLL		9
#define PARTICLE_ROTATINGYAWMINUS   10
#define PARTICLE_VERT				11
#define PARTICLE_CHAINED            12

#define RDF_BLOOM         4      //BLOOMS

#define	MAX_VERTEX_CACHES	((3+MAX_FRAMES*3)*MAX_MODELS)
#define MAX_VBO_XYZs		65536

typedef enum {
	VBO_STATIC,
	VBO_DYNAMIC
} vertCacheMode_t;

typedef enum {
	VBO_STORE_ANY,
	VBO_STORE_INDICES,
	VBO_STORE_BINORMAL,
	VBO_STORE_ST,
	// MD2 models will use, for example, VBO_STORE_XYZ+3 for the 3rd frame's 
	// vertex data. IQM models are skeletal and don't need to do this.
	VBO_STORE_XYZ,
	VBO_STORE_NORMAL = VBO_STORE_XYZ+MAX_FRAMES,
	VBO_STORE_TANGENT = VBO_STORE_NORMAL+MAX_FRAMES
} vertStoreMode_t;

typedef struct vertCache_s
{
	struct vertCache_s	*prev;
	struct vertCache_s	*next;

	vertCacheMode_t		mode;

	int					size;

	void				*pointer;

	vertStoreMode_t		store;
	struct model_s		*mod;

	unsigned			id;
} vertCache_t;

typedef struct {
	vertCache_t		*freeVertCache;
	vertCache_t		activeVertCache;
	vertCache_t		vertCacheList[MAX_VERTEX_CACHES];
} vertCacheManager_t;

vertCacheManager_t	vcm;

typedef struct entity_s
{
	char	name[MAX_QPATH];

	struct model_s		*model;			// opaque type outside refresh
	struct model_s		*lod1;
	struct model_s		*lod2;

	float				angles[3];

	/*
	** most recent data
	*/
	float				origin[3];
	int					frame;

	/*
	** previous data for lerping
	*/
	float				oldorigin[3];
	int					oldframe;

	/*
	** frame and timestamp (iqm lerping)
	*/
	float				frametime;
	int					prevframe;

	/*
	** misc
	*/
	float	backlerp;				// 0.0 = current, 1.0 = old
	int		skinnum;

	int		lightstyle;				// for flashing entities
	float	alpha;					// ignore if RF_TRANSLUCENT isn't set

	struct image_s	*skin;			// NULL for inline skin
	int		flags;

	int		team;

	float	bob;
	
	int		number; //edict number;

	struct rscript_s *script;

} entity_t;

//for saving persistent data about entities across frames
typedef struct {
	/*
	** for saving non-dynamic lighting of static meshes
	*/
	qboolean			setlightstuff;
	float				oldnumlights;
	vec3_t				oldlightadd;
	vec3_t				oldorigin;
	float				oldlightintens;
} cl_entity_pers_t;

cl_entity_pers_t	cl_persistent_ents[MAX_EDICTS];


#define ENTITY_FLAGS  68

typedef struct
{
	vec3_t	origin;
	vec3_t	color;
	float	intensity;
} dlight_t;

typedef struct
{
	float	strength;
	vec3_t	direction;
	vec3_t	color;
} m_dlight_t;

// ========
// PGM
typedef struct
{
	qboolean	isactive;

	vec3_t		lightcol;
	float		light;
	float		lightvel;
} cplight_t;

#define P_LIGHTS_MAX 8

typedef struct particle_s
{
	struct particle_s	*next;

	cplight_t	lights[P_LIGHTS_MAX];

	float		time;

	vec3_t		org;
	vec3_t		angle;
	vec3_t		vel;
	vec3_t		accel;
	vec3_t		end;
	float		color;
	float		colorvel;
	float		alpha;
	float		alphavel;
	int			type;		// 0 standard, 1 smoke, etc etc...
	struct image_s *image;
	int			blenddst;
	int			blendsrc;
	float		scale;
	float		scalevel;
	qboolean	fromsustainedeffect;
	float       dist;

	// These are computed from the foo and foovel values-- for example,
	// current_color is computed from color and colorvel and its value
	// changes every frame.
	vec3_t	current_origin;
	int		current_color;
	float	current_alpha;
	float	current_scale;

	// For particle chains-- the particle renderer will automatically play
	// connect-the-dots with these.
	struct particle_s	*chain_prev;
	vec3_t				current_pspan;

} particle_t;


typedef struct
{
	float		rgb[3];			// 0.0 - 2.0
	float		white;			// highest of rgb
} lightstyle_t;

typedef struct
{
	int			x, y, width, height;// in virtual screen coordinates
	float		fov_x, fov_y;
	float		vieworg[3];
	float		viewangles[3];
	float		blend[4];			// rgba 0-1 full screen blend
	float		time;				// time is uesed to auto animate
	int			rdflags;			// RDF_UNDERWATER, etc

	byte		*areabits;			// must not be NULL if you want to do BSP rendering
	qboolean	areabits_changed;

	lightstyle_t	*lightstyles;	// [MAX_LIGHTSTYLES]

	int			num_entities;
	entity_t	*entities;

	int			num_viewentities;
	entity_t	*viewentities;

	int			num_dlights;
	dlight_t	*dlights;
	qboolean	dlights_changed;
	int			num_dlight_surfaces;

	int			num_particles;
	particle_t	**particles;

	int			num_grasses;
	grass_t		*grasses;

	int			num_beams;
	beam_t		*beams;
	
	// Because the mirror texture doesn't have to updatea more than 60 times
	// per second.
	int			last_mirrorupdate_time; // in ms

} refdef_t;

//
// view origin moved from r_local.h. -M.
//
extern	vec3_t	vup;
extern	vec3_t	vpn;
extern	vec3_t	vright;
extern	vec3_t	r_origin;

// Knightmare- added Psychospaz's menu cursor
//cursor - psychospaz
#define MENU_CURSOR_BUTTON_MAX 2

typedef struct
{
	//only 2 buttons for menus
	float		buttontime[MENU_CURSOR_BUTTON_MAX];
	int			buttonclicks[MENU_CURSOR_BUTTON_MAX];
	qboolean	buttonused[MENU_CURSOR_BUTTON_MAX];
	qboolean	buttondown[MENU_CURSOR_BUTTON_MAX];

	qboolean	mouseaction;

	//this is the active item that cursor is on.
	int			menulayer;
	struct _tag_menuitem
				*menuitem;
	//this is whatever menuitem it was on when a click-and-drag maneuver was
	//begun.
	struct _tag_menuitem
				*click_menuitem;
	qboolean	suppress_drag; //started clicking with nothing selected

	//coords
	int		x;
	int		y;

	int		oldx;
	int		oldy;
} cursor_t;

float *RGBA (float r, float g, float b, float a);
#define RGBA8(a,b,c,d) RGBA((a)/255.0f, (b)/255.0f, (c)/255.0f, (d)/255.0f)

qboolean	Draw_PicExists (const char *name);
void	Draw_GetPicSize (int *w, int *h, const char *name);
void	Draw_Pic (float x, float y, const char *name);
void	Draw_ScaledPic (float x, float y, float scale, const char *pic);
void	Draw_StretchPic (float x, float y, float w, float h, const char *name);
void	Draw_AlphaStretchTilingPic (float x, float y, float w, float h, const char *name, float alphaval);
void	Draw_AlphaStretchPic (float x, float y, float w, float h, const char *name, float alphaval);
void	Draw_AlphaStretchPlayerIcon (int x, int y, int w, int h, const char *pic, float alphaval);
void	Draw_Fill (float x, float y, float w, float h, const float rgba[]);
void	Draw_FadeScreen (void);

void	R_BeginFrame( float camera_separation );
void	R_SwapBuffers( int );
void	R_SetPalette ( const unsigned char *palette);

struct model_s	*R_RegisterModel (char *name);
struct image_s	*R_RegisterSkin (char *name);
struct image_s	*R_RegisterPic (const char *name);

void	R_SetSky (char *name, float rotate, vec3_t axis);

void	R_RegisterBasePlayerModels(void);
void	R_RegisterCustomPlayerModels(void);
void	S_RegisterSoundsForPlayer (char *playername);
void	R_BeginRegistration (char *map);
void	R_EndRegistration (void);

void	R_RenderFrame (refdef_t *fd);
void	R_RenderFramePlayerSetup (refdef_t *fd);
void	R_EndFrame (void);

int		R_Init( void *hinstance, void *hWnd );
void	R_Shutdown (void);

void	R_AppActivate( qboolean active );

// TrueType
struct ttf_font_s;
typedef struct ttf_font_s * ttf_font_t;

ttf_font_t TTF_GetFont( const char * name , unsigned int size );
void TTF_GetSize( ttf_font_t font , const char * text , unsigned int * width , unsigned int * height );
void TTF_RawPrint( ttf_font_t font , const char * text , float x , float y , const float color[4] );

#define MAX_RADAR_ENTS 512
typedef struct RadarEnt_s{
  float color[4];
  vec3_t org;
}RadarEnt_t;


extern int numRadarEnts;
extern RadarEnt_t RadarEnts[MAX_RADAR_ENTS];

extern cursor_t cursor;


qboolean is_localhost; //because ref_gl can't access cls.servername.

#endif // __REF_H
