// LOCAL ADDITION (Silver Saint): weapon inspect.
//
// "inspect" (the launcher binds it to I) plays the weapon's own built-in animation; nothing is
// posed or bent by code, it's the model's animation, cross-faded in and out.
//  - Knives: their idle from the start -- the flip CS 1.6 plays by itself after a while.
//  - Guns: CS 1.6 guns' idles are a still frame, so they play the second half of their own
//    draw animation instead -- the bolt pull, slide rack or charging handle, with its sounds --
//    starting once the gun is already up, so it never looks like it's being pulled out again.
//    Guns whose draw has no such motion use the end of a shot or reload instead.
//
// Everything is client-side: nothing is sent to the server, so it works online too.
// Firing, alt-fire, reloading, switching weapons or any new weapon animation stops it.

#include <string.h>
#include <math.h>
#include "hud.h"
#include "cl_util.h"
#include "com_model.h"
#include "studio.h"
#include "studio_util.h"
#include "kbutton.h"
#include "r_studioint.h"
#include "inspect.h"
#include "StudioModelRenderer.h"
#include "GameStudioModelRenderer.h"

extern engine_studio_api_t IEngineStudio;
extern CGameStudioModelRenderer g_StudioRenderer;
extern kbutton_t in_attack, in_attack2, in_reload;

namespace
{
// Shorter idles are a held pose, not an animation worth playing.
const float minLength = 1.5f;

// Guns: which animation holds the handling, and roughly where it starts (refined per model by
// SettledFrame). `sound` plays at the start for clips whose only sound event comes earlier.
struct Clip
{
	const char *weapon;   // models/v_<weapon>.mdl
	const char *sequence; // "_unsil" variants are used when the silencer is off
	int from;
	const char *sound;
};

const Clip clips[] =
{
	{ "ak47",      "draw",    7, NULL },
	{ "aug",       "draw",    6, NULL },
	{ "famas",     "draw",    7, NULL },
	{ "galil",     "draw",    7, NULL },
	{ "glock18",   "draw",    9, NULL },
	{ "m3",        "draw",    8, NULL },
	{ "m4a1",      "draw",    5, NULL },
	{ "mp5",       "draw",    7, NULL },
	{ "p228",      "draw",    7, NULL },
	{ "p90",       "draw",    6, NULL },
	{ "sg552",     "draw",    9, NULL },
	{ "usp",       "draw",   10, NULL },
	{ "fiveseven", "draw",    3, NULL },
	{ "m249",      "draw",   14, NULL },
	{ "tmp",       "draw",    7, NULL },
	{ "deagle",    "draw",    9, "weapons/de_deploy.wav" },
	{ "elite",     "draw",   16, "weapons/elite_deploy.wav" },
	{ "xm1014",    "draw",   12, "weapons/de_deploy.wav" },
	{ "awp",       "shoot1",  8, NULL },   // bolt up, back, forward -- after the shot
	{ "scout",     "shoot",   8, NULL },
	{ "sg550",     "reload", 76, NULL },   // the charging handle at the end of a reload
	{ "g3sg1",     "reload",108, NULL },
	{ "ump45",     "reload", 80, NULL },
	{ "mac10",     "reload", 80, NULL },
};

bool active;
struct model_s *model;
int sequence;            // the animation being played
float from;              // its first frame
float startTime, length;
int engineSequence;      // what the game was playing, to notice when it plays something new
float engineAnimtime;

mstudioseqdesc_t *Sequences( studiohdr_t *hdr )
{
	return (mstudioseqdesc_t *)( (byte *)hdr + hdr->seqindex );
}

float Length( mstudioseqdesc_t *s )
{
	return s->numframes > 1 && s->fps > 0 ? ( s->numframes - 1 ) / s->fps : 0;
}

// Where the game's animation is right now, in frames (as the renderer works it out).
float EngineFrame( cl_entity_t *view, mstudioseqdesc_t *s, float now )
{
	if( s->numframes <= 1 )
		return 0;
	float f = view->curstate.frame * ( s->numframes - 1 ) / 256.0f
		+ ( now - view->curstate.animtime ) * view->curstate.framerate * s->fps;
	if( s->flags & STUDIO_LOOPING )
		f -= (int)( f / ( s->numframes - 1 )) * ( s->numframes - 1 );
	return f < 0 ? 0 : f > s->numframes - 1 ? s->numframes - 1 : f;
}

// "models/v_ak47.mdl" -> "ak47"
const char *WeaponName( struct model_s *m )
{
	const char *slash = strrchr( m->name, '/' );
	const char *base = slash ? slash + 1 : m->name;
	return strncmp( base, "v_", 2 ) ? base : base + 2;
}

bool IsWeapon( const char *weapon, const char *name )
{
	size_t n = strlen( name );
	return !strncmp( weapon, name, n ) && !strcmp( weapon + n, ".mdl" );
}

int FindSequence( studiohdr_t *hdr, const char *name )
{
	mstudioseqdesc_t *seq = Sequences( hdr );
	for( int i = 0; i < hdr->numseq; i++ )
		if( !strcmp( seq[i].label, name )) return i;
	return -1;
}

// A knife's longest idle that really moves (the flip).
int KnifeIdle( studiohdr_t *hdr )
{
	mstudioseqdesc_t *seq = Sequences( hdr );
	int best = -1;
	for( int i = 0; i < hdr->numseq; i++ )
	{
		if( !strstr( seq[i].label, "idle" ) || strstr( seq[i].label, "shield" )) continue;
		if( best < 0 || Length( seq + i ) > Length( seq + best )) best = i;
	}
	return best >= 0 && Length( seq + best ) >= minLength ? best : -1;
}

// A bone's placement in model space at a frame of a sequence, decoded by the renderer.
bool BoneAt( cl_entity_t *view, studiohdr_t *hdr, int sequence, float frame, int bone, float out[3][4] )
{
	static float pos[MAXSTUDIOBONES][3];
	static vec4_t q[MAXSTUDIOBONES];
	mstudioseqdesc_t *seq = Sequences( hdr ) + sequence;
	cl_entity_t *was = g_StudioRenderer.m_pCurrentEntity;
	g_StudioRenderer.m_pCurrentEntity = view;   // for bone controllers
	mstudioanim_t *anim = g_StudioRenderer.StudioGetAnim( view->model, seq );
	if( anim ) g_StudioRenderer.StudioCalcRotations( pos, q, seq, anim, frame );
	g_StudioRenderer.m_pCurrentEntity = was;
	if( !anim ) return false;

	mstudiobone_t *pbones = (mstudiobone_t *)( (byte *)hdr + hdr->boneindex );
	int chain[MAXSTUDIOBONES], n = 0;
	for( int j = bone; j >= 0 && n < MAXSTUDIOBONES; j = pbones[j].parent ) chain[n++] = j;
	float m[3][4] = { { 1, 0, 0, 0 }, { 0, 1, 0, 0 }, { 0, 0, 1, 0 } };
	for( int k = n - 1; k >= 0; k-- )
	{
		float local[3][4], res[3][4];
		QuaternionMatrix( q[chain[k]], local );
		local[0][3] = pos[chain[k]][0]; local[1][3] = pos[chain[k]][1]; local[2][3] = pos[chain[k]][2];
		ConcatTransforms( m, local, res );
		memcpy( m, res, sizeof( m ));
	}
	memcpy( out, m, sizeof( m ));
	return true;
}

// Where the gun has finished coming up into view: after the last frame where the gun is still
// far from where it rests (travelling up from below the screen). Everything after that -- the
// click, the rack, the sway -- is kept; `from` (measured per model) is the minimum. Works for
// any model.
float SettledFrame( cl_entity_t *view, studiohdr_t *hdr, int sequence, float from )
{
	const float offScreen = 10;   // units from the rest position
	mstudioseqdesc_t *s = Sequences( hdr ) + sequence;
	int last = s->numframes - 1;

	// The gun: the bone that carries the most vertices.
	static int vertsPerBone[MAXSTUDIOBONES];
	memset( vertsPerBone, 0, sizeof( vertsPerBone ));
	mstudiobodyparts_t *parts = (mstudiobodyparts_t *)( (byte *)hdr + hdr->bodypartindex );
	for( int b = 0; b < hdr->numbodyparts; b++ )
	{
		mstudiomodel_t *sub = (mstudiomodel_t *)( (byte *)hdr + parts[b].modelindex );
		byte *info = (byte *)hdr + sub->vertinfoindex;
		for( int v = 0; v < sub->numverts; v++ )
			if( info[v] < MAXSTUDIOBONES ) vertsPerBone[info[v]]++;
	}
	int gun = -1, most = 0;
	for( int i = 0; i < hdr->numbones; i++ )
		if( vertsPerBone[i] > most ) { most = vertsPerBone[i]; gun = i; }
	if( gun < 0 ) return from;

	float rest[3][4], at[3][4];
	if( !BoneAt( view, hdr, sequence, last, gun, rest )) return from;
	float settled = from;
	for( int f = 0; f < last; f++ )
	{
		if( !BoneAt( view, hdr, sequence, f, gun, at )) return from;
		float dx = at[0][3] - rest[0][3], dy = at[1][3] - rest[1][3], dz = at[2][3] - rest[2][3];
		if( sqrtf( dx * dx + dy * dy + dz * dz ) > offScreen && f + 1 > settled ) settled = f + 1;
	}
	return settled < last - 2 ? settled : from;
}

// Hand the view model back to the game, cross-fading from where the clip got to.
void Stop( cl_entity_t *view, float now )
{
	if( active && view && view->model == model )
	{
		studiohdr_t *hdr = (studiohdr_t *)IEngineStudio.Mod_Extradata( view->model );
		if( hdr && sequence < hdr->numseq )
		{
			mstudioseqdesc_t *s = Sequences( hdr ) + sequence;
			float f = from + ( now - startTime ) * s->fps;
			view->latched.prevsequence = sequence;
			view->latched.prevframe = f > s->numframes - 1 ? s->numframes - 1 : f;
			view->latched.sequencetime = now;
		}
	}
	active = false;
	model = NULL;
}

void Inspect_f( void )
{
	cl_entity_t *view = gEngfuncs.GetViewModel();
	if( !view || !view->model || view->model->type != mod_studio || active )
		return;
	studiohdr_t *hdr = (studiohdr_t *)IEngineStudio.Mod_Extradata( view->model );
	if( !hdr )
		return;

	const char *weapon = WeaponName( view->model );
	int current = view->curstate.sequence;
	int seq = -1;
	float first = 0;
	const char *sound = NULL;
	if( strstr( weapon, "knife" ))
		seq = KnifeIdle( hdr );
	else
	{
		for( size_t i = 0; i < sizeof( clips ) / sizeof( clips[0] ); i++ )
		{
			if( !IsWeapon( weapon, clips[i].weapon )) continue;
			bool unsil = current >= 0 && current < hdr->numseq && strstr( Sequences( hdr )[current].label, "unsil" );
			if( unsil )
			{
				char name[64];
				snprintf( name, sizeof( name ), "%s_unsil", clips[i].sequence );
				seq = FindSequence( hdr, name );
			}
			if( seq < 0 ) seq = FindSequence( hdr, clips[i].sequence );
			if( seq >= 0 )
			{
				first = SettledFrame( view, hdr, seq, clips[i].from );
				sound = clips[i].sound;
			}
			break;
		}
	}
	if( seq < 0 )
	{
		gEngfuncs.Con_DPrintf( "inspect: nothing to play for %s\n", view->model->name );
		return;
	}
	mstudioseqdesc_t *s = Sequences( hdr ) + seq;
	float len = ( s->numframes - 1 - first ) / ( s->fps > 0 ? s->fps : 30 );
	if( len <= 0.1f )
		return;

	float now = gEngfuncs.GetClientTime();
	active = true;
	model = view->model;
	sequence = seq;
	from = first;
	startTime = now;
	length = len;
	engineSequence = current;
	engineAnimtime = view->curstate.animtime;

	// Cross-fade in from the frame the game is showing.
	if( current >= 0 && current < hdr->numseq )
	{
		view->latched.prevsequence = current;
		view->latched.prevframe = EngineFrame( view, Sequences( hdr ) + current, now );
		// Just before animtime: equal times make the engine fire every event from frame 0,
		// such as the AWP's muzzle flash, which the clip starts after.
		view->latched.sequencetime = now - 0.001f;
	}
	if( sound )
		gEngfuncs.pfnPlaySoundByName( (char *)sound, 0.8f );
	gEngfuncs.Con_DPrintf( "inspect: %s plays %s from frame %.0f (%.1f s)\n", view->model->name, s->label, first, length );
}
}

void Inspect_Init( void )
{
	gEngfuncs.pfnAddCommand( "inspect", Inspect_f );
}

bool Inspect_Active( void )
{
	return active;
}

void Inspect_Update( cl_entity_t *view )
{
	if( !active )
		return;
	float now = gEngfuncs.GetClientTime();

	// A different gun, or none (death, scope, spectating).
	if( !view || !view->model || view->model != model )
	{
		active = false;
		model = NULL;
		return;
	}

	// The engine re-applies the game's weapon animation every frame before this runs, so
	// these are the game's values. Firing, reloading or any new weapon animation takes over.
	bool input = ( in_attack.state & 1 ) || ( in_attack2.state & 1 ) || ( in_reload.state & 1 );
	bool newAnim = view->curstate.sequence != engineSequence || view->curstate.animtime != engineAnimtime;
	if( newAnim )
	{
		active = false;   // the game's animation blends in by itself
		model = NULL;
		return;
	}
	if( input || now - startTime >= length )
	{
		Stop( view, now );
		return;
	}

	view->curstate.sequence = sequence;
	view->curstate.animtime = startTime;
	view->curstate.framerate = 1;
	mstudioseqdesc_t *s = Sequences( (studiohdr_t *)IEngineStudio.Mod_Extradata( view->model )) + sequence;
	view->curstate.frame = s->numframes > 1 ? from * 256.0f / ( s->numframes - 1 ) : 0;
}
