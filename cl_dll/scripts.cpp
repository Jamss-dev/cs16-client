// LOCAL ADDITION (Silver Saint): Lua mod scripts.
//
// Mods can be code: Lua 5.4 scripts that run inside the client, loaded from
// cstrike/silversaint/scripts/<name>.lua for every name listed in autoload.txt there (the
// launcher writes it from the player's enabled mods). A script sees the game through the `ss`
// table and reacts to events:
//
//   ss.on("frame", function(dt) end)             every frame
//   ss.on("hud", function(w, h) end)             drawing: ss.fill/box/line/circle/text
//   ss.on("move", function(cmd) end)             before input goes to the server: cmd.forward,
//                                                side, up, buttons; ss.setAngles to aim
//   ss.on("key", function(name, down) end)       return true to take the key from the game
//   ss.on("death", function(e) end)              e.killer, victim, headshot, weapon, killerIsMe...
//   ss.on("shot", function(e) end)               every bullet: e.shooter, from, to, muzzle, me
//
// plus ss.players(), ss.me(), ss.toScreen, ss.aimAngles, ss.trace, effects (ss.beam, light,
// sparks, sound), ss.fov, cvars, console commands and ss.cmd. See docs/MOD-SCRIPTS.md.
//
// Scripts run in a sandbox (no files, no OS, no loading other code) with a time limit per
// call; a script that errors has that handler switched off and the error written to the
// console and to silversaint/scripts.log, which Silver AI reads to fix its scripts.
//
// Fair play: on someone else's server, scripts can't aim for you, press buttons for you or
// see enemies through walls -- ss.setAngles errors, changes to the move command are dropped
// and ss.players() only reports enemies you can actually see. Those work in games you host
// (bots, practice, your own server), where nobody else is affected.

#include <string.h>
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <math.h>
#include <map>
#include <string>
#include "hud.h"
#include "cl_util.h"
#include "draw_util.h"
#include "com_model.h"
#include "cl_entity.h"
#include "event_api.h"
#include "r_efx.h"
#include "triangleapi.h"
#include "pm_defs.h"
#include "usercmd.h"
#include "in_buttons.h"
#include "keydefs.h"
#include "net_api.h"
#include "scripts.h"

extern "C" {
#include "lua.h"
#include "lualib.h"
#include "lauxlib.h"
}

extern bool CL_IsDead();

namespace
{
lua_State *L;
double lastFrame;
bool inHud, inMove;
usercmd_s *moveCmd;
int fovOverride;
long budget;                 // hook ticks left for the current call
const long budgetTicks = 20000;   // x1000 instructions: a runaway loop stops in well under a second
bool keyState[256];

struct Head { float pos[3]; float time; };
Head heads[MAX_PLAYERS + 1];

// ---------------------------------------------------------------------------------------
// Log

FILE *logFile;

void Log( const char *fmt, ... )
{
	char buf[1024];
	va_list ap;
	va_start( ap, fmt );
	vsnprintf( buf, sizeof( buf ), fmt, ap );
	va_end( ap );
	gEngfuncs.Con_Printf( "[scripts] %s\n", buf );
	if( logFile ) { fprintf( logFile, "%s\n", buf ); fflush( logFile ); }
}

void OpenLog( void )
{
	const char *base = getenv( "XASH3D_BASEDIR" );
	if( !base || logFile ) return;
	char path[1024];
	snprintf( path, sizeof( path ), "%s/cstrike/silversaint/scripts.log", base );
	logFile = fopen( path, "w" );
}

// ---------------------------------------------------------------------------------------
// Helpers

void Hook( lua_State *l, lua_Debug * )
{
	if( --budget < 0 )
		luaL_error( l, "took too long (a loop that never ends?)" );
}

float Num( lua_State *l, int i, float def ) { return (float)luaL_optnumber( l, i, def ); }

void PushVec( lua_State *l, const float *v )
{
	lua_createtable( l, 0, 3 );
	lua_pushnumber( l, v[0] ); lua_setfield( l, -2, "x" );
	lua_pushnumber( l, v[1] ); lua_setfield( l, -2, "y" );
	lua_pushnumber( l, v[2] ); lua_setfield( l, -2, "z" );
}

// A position given as a table {x=,y=,z=} or {1,2,3} at `i`, or as three numbers from `i`.
// Returns the next argument index.
int GetVec( lua_State *l, int i, float *out )
{
	if( lua_istable( l, i ))
	{
		const char *k[3] = { "x", "y", "z" };
		for( int j = 0; j < 3; j++ )
		{
			lua_getfield( l, i, k[j] );
			if( lua_isnil( l, -1 )) { lua_pop( l, 1 ); lua_rawgeti( l, i, j + 1 ); }
			out[j] = (float)luaL_checknumber( l, -1 );
			lua_pop( l, 1 );
		}
		return i + 1;
	}
	for( int j = 0; j < 3; j++ ) out[j] = (float)luaL_checknumber( l, i + j );
	return i + 3;
}

const char *TeamName( int team ) { return team == 1 ? "T" : team == 2 ? "CT" : "SPEC"; }

const char *KeyName( int k )
{
	static char one[2];
	switch( k )
	{
	case K_TAB: return "tab"; case K_ENTER: return "enter"; case K_ESCAPE: return "escape";
	case K_SPACE: return "space"; case K_BACKSPACE: return "backspace";
	case K_UPARROW: return "up"; case K_DOWNARROW: return "down"; case K_LEFTARROW: return "left"; case K_RIGHTARROW: return "right";
	case K_ALT: return "alt"; case K_CTRL: return "ctrl"; case K_SHIFT: return "shift";
	case K_INS: return "ins"; case K_DEL: return "del"; case K_PGDN: return "pgdn"; case K_PGUP: return "pgup";
	case K_HOME: return "home"; case K_END: return "end"; case K_CAPSLOCK: return "capslock";
	case K_MOUSE1: return "mouse1"; case K_MOUSE2: return "mouse2"; case K_MOUSE3: return "mouse3";
	case K_MOUSE4: return "mouse4"; case K_MOUSE5: return "mouse5";
	case K_MWHEELUP: return "mwheelup"; case K_MWHEELDOWN: return "mwheeldown";
	}
	if( k >= K_F1 && k <= K_F12 ) { static char f[4]; snprintf( f, sizeof( f ), "f%d", k - K_F1 + 1 ); return f; }
	if( k > 32 && k < 127 ) { one[0] = (char)k; one[1] = 0; return one; }
	return "";
}

void EyePosition( float *eye )
{
	cl_entity_t *me = gEngfuncs.GetLocalPlayer();
	float ofs[3] = { 0, 0, 0 };
	gEngfuncs.pEventAPI->EV_LocalPlayerViewheight( ofs );
	for( int i = 0; i < 3; i++ ) eye[i] = ( me ? me->origin[i] : 0 ) + ofs[i];
}

// Fraction of the way from `a` to `b` before hitting the world (1 = clear line of sight).
float TraceWorld( float *a, float *b )
{
	pmtrace_t tr;
	gEngfuncs.pEventAPI->EV_SetUpPlayerPrediction( false, true );
	gEngfuncs.pEventAPI->EV_PushPMStates();
	gEngfuncs.pEventAPI->EV_SetSolidPlayers( -1 );
	gEngfuncs.pEventAPI->EV_SetTraceHull( 2 );
	gEngfuncs.pEventAPI->EV_PlayerTrace( a, b, PM_WORLD_ONLY | PM_GLASS_IGNORE, -1, &tr );
	gEngfuncs.pEventAPI->EV_PopPMStates();
	return tr.fraction;
}

// A game this client hosts (bots, practice, the player's own listen server)?
bool LocalGame( void )
{
	net_status_s st;
	memset( &st, 0, sizeof( st ));
	gEngfuncs.pNetAPI->Status( &st );
	return st.remote_address.type == NA_LOOPBACK;
}

bool Fresh( cl_entity_t *e )
{
	cl_entity_t *me = gEngfuncs.GetLocalPlayer();
	return e && me && e->curstate.messagenum >= me->curstate.messagenum;
}

void HeadOf( int i, cl_entity_t *e, float *out )
{
	if( heads[i].time > 0 && gEngfuncs.GetClientTime() - heads[i].time < 0.1f )
	{
		VectorCopy( heads[i].pos, out );
		return;
	}
	VectorCopy( e->origin, out );
	out[2] += e->curstate.usehull == 1 ? 12 : 26;   // ducking : standing
}

// ---------------------------------------------------------------------------------------
// The ss table

int l_on( lua_State *l )
{
	const char *ev = luaL_checkstring( l, 1 );
	luaL_checktype( l, 2, LUA_TFUNCTION );
	lua_getfield( l, LUA_REGISTRYINDEX, "ss_handlers" );
	if( lua_getfield( l, -1, ev ) != LUA_TTABLE )
	{
		lua_pop( l, 1 );
		lua_newtable( l );
		lua_pushvalue( l, -1 );
		lua_setfield( l, -3, ev );
	}
	lua_createtable( l, 0, 2 );
	lua_pushvalue( l, 2 ); lua_setfield( l, -2, "fn" );
	lua_getfield( l, LUA_REGISTRYINDEX, "ss_current" ); lua_setfield( l, -2, "script" );
	lua_rawseti( l, -2, luaL_len( l, -2 ) + 1 );
	return 0;
}

int l_print( lua_State *l )
{
	int n = lua_gettop( l );   // before the buffer, which may use a stack slot
	luaL_Buffer b;
	luaL_buffinit( l, &b );
	for( int i = 1; i <= n; i++ )
	{
		if( i > 1 ) luaL_addchar( &b, ' ' );
		luaL_tolstring( l, i, NULL );
		luaL_addvalue( &b );
	}
	luaL_pushresult( &b );
	lua_getfield( l, LUA_REGISTRYINDEX, "ss_current" );
	const char *who = lua_tostring( l, -1 ) ? lua_tostring( l, -1 ) : "script";
	gEngfuncs.Con_Printf( "[%s] %s\n", who, lua_tostring( l, -2 ));
	if( logFile ) { fprintf( logFile, "[%s] %s\n", who, lua_tostring( l, -2 )); fflush( logFile ); }
	return 0;
}

int l_time( lua_State *l ) { lua_pushnumber( l, gEngfuncs.GetClientTime()); return 1; }
int l_screen( lua_State *l ) { lua_pushinteger( l, ScreenWidth ); lua_pushinteger( l, ScreenHeight ); return 2; }

int l_players( lua_State *l )
{
	cl_entity_t *me = gEngfuncs.GetLocalPlayer();
	float eye[3];
	EyePosition( eye );
	lua_newtable( l );
	int n = 0;
	bool local = LocalGame();
	int max = gEngfuncs.GetMaxClients();
	for( int i = 1; i <= max && i <= MAX_PLAYERS; i++ )
	{
		gEngfuncs.pfnGetPlayerInfo( i, &g_PlayerInfoList[i] );
		if( !g_PlayerInfoList[i].name || !g_PlayerInfoList[i].name[0] ) continue;
		cl_entity_t *e = gEngfuncs.GetEntityByIndex( i );
		if( !e ) continue;

		lua_newtable( l );
		lua_pushinteger( l, i ); lua_setfield( l, -2, "id" );
		lua_pushstring( l, g_PlayerInfoList[i].name ); lua_setfield( l, -2, "name" );
		lua_pushstring( l, TeamName( g_PlayerExtraInfo[i].teamnumber )); lua_setfield( l, -2, "team" );
		lua_pushboolean( l, !g_PlayerExtraInfo[i].dead ); lua_setfield( l, -2, "alive" );
		lua_pushboolean( l, me && e->index == me->index ); lua_setfield( l, -2, "me" );
		lua_pushboolean( l, g_PlayerInfoList[i].ping == 0 && !g_PlayerInfoList[i].thisplayer ); lua_setfield( l, -2, "bot" );
		bool fresh = Fresh( e );
		// On other people's servers, an enemy you can't see has no position (no wallhacks).
		bool enemy = me && me->index > 0 && me->index <= MAX_PLAYERS && e->index != me->index
			&& g_PlayerExtraInfo[i].teamnumber != g_PlayerExtraInfo[me->index].teamnumber;
		if( enemy && !local )
		{
			float h[3];
			if( fresh ) HeadOf( i, e, h );
			if( !fresh || TraceWorld( eye, h ) < 0.99f )
			{
				lua_pushboolean( l, 0 ); lua_setfield( l, -2, "inView" );
				lua_pushboolean( l, 0 ); lua_setfield( l, -2, "visible" );
				lua_pushboolean( l, g_PlayerExtraInfo[i].has_c4 ); lua_setfield( l, -2, "hasBomb" );
				lua_pushinteger( l, g_PlayerExtraInfo[i].frags ); lua_setfield( l, -2, "kills" );
				lua_pushinteger( l, g_PlayerExtraInfo[i].deaths ); lua_setfield( l, -2, "deaths" );
				lua_rawseti( l, -2, ++n );
				continue;
			}
		}
		lua_pushboolean( l, fresh ); lua_setfield( l, -2, "inView" );   // position known this frame
		lua_pushboolean( l, g_PlayerExtraInfo[i].has_c4 ); lua_setfield( l, -2, "hasBomb" );
		lua_pushinteger( l, g_PlayerExtraInfo[i].frags ); lua_setfield( l, -2, "kills" );
		lua_pushinteger( l, g_PlayerExtraInfo[i].deaths ); lua_setfield( l, -2, "deaths" );
		PushVec( l, fresh ? (float *)e->origin : (float *)g_PlayerExtraInfo[i].origin ); lua_setfield( l, -2, "origin" );
		float head[3];
		if( fresh ) HeadOf( i, e, head ); else { VectorCopy( g_PlayerExtraInfo[i].origin, head ); head[2] += 26; }
		PushVec( l, head ); lua_setfield( l, -2, "head" );
		float d[3]; VectorSubtract( head, eye, d );
		lua_pushnumber( l, sqrtf( d[0] * d[0] + d[1] * d[1] + d[2] * d[2] )); lua_setfield( l, -2, "distance" );
		lua_pushboolean( l, fresh && TraceWorld( eye, head ) >= 0.99f ); lua_setfield( l, -2, "visible" );
		lua_pushboolean( l, e->curstate.usehull == 1 ); lua_setfield( l, -2, "ducking" );
		lua_rawseti( l, -2, ++n );
	}
	return 1;
}

int l_me( lua_State *l )
{
	cl_entity_t *me = gEngfuncs.GetLocalPlayer();
	if( !me ) { lua_pushnil( l ); return 1; }
	int i = me->index;
	float eye[3], ang[3];
	EyePosition( eye );
	gEngfuncs.GetViewAngles( ang );
	lua_newtable( l );
	lua_pushinteger( l, i ); lua_setfield( l, -2, "id" );
	lua_pushstring( l, i > 0 && i <= MAX_PLAYERS ? TeamName( g_PlayerExtraInfo[i].teamnumber ) : "SPEC" ); lua_setfield( l, -2, "team" );
	lua_pushboolean( l, !CL_IsDead()); lua_setfield( l, -2, "alive" );
	PushVec( l, me->origin ); lua_setfield( l, -2, "origin" );
	PushVec( l, eye ); lua_setfield( l, -2, "eye" );
	lua_pushnumber( l, ang[0] ); lua_setfield( l, -2, "pitch" );
	lua_pushnumber( l, ang[1] ); lua_setfield( l, -2, "yaw" );
	cl_entity_t *vm = gEngfuncs.GetViewModel();
	const char *w = vm && vm->model ? vm->model->name : "";
	const char *slash = strrchr( w, '/' );
	char weapon[32] = "";
	if( slash && !strncmp( slash + 1, "v_", 2 ))
	{
		strlcpy( weapon, slash + 3, sizeof( weapon ));
		char *dot = strrchr( weapon, '.' );
		if( dot ) *dot = 0;
	}
	lua_pushstring( l, weapon ); lua_setfield( l, -2, "weapon" );
	lua_pushinteger( l, gHUD.m_iFOV ); lua_setfield( l, -2, "fov" );
	return 1;
}

int l_toScreen( lua_State *l )
{
	float v[3], s[3];
	GetVec( l, 1, v );
	if( gEngfuncs.pTriAPI->WorldToScreen( v, s )) return 0;   // behind the camera
	lua_pushnumber( l, XPROJECT( s[0] ));
	lua_pushnumber( l, YPROJECT( s[1] ));
	return 2;
}

// View angles (pitch, yaw) that look from the eye at a point. GoldSrc view pitch is + down.
int l_aimAngles( lua_State *l )
{
	float v[3], eye[3];
	GetVec( l, 1, v );
	EyePosition( eye );
	float d[3]; VectorSubtract( v, eye, d );
	float yaw = atan2f( d[1], d[0] ) * 180.0f / (float)M_PI;
	float pitch = -atan2f( d[2], sqrtf( d[0] * d[0] + d[1] * d[1] )) * 180.0f / (float)M_PI;
	lua_pushnumber( l, pitch );
	lua_pushnumber( l, yaw );
	return 2;
}

int l_angles( lua_State *l )
{
	float a[3];
	gEngfuncs.GetViewAngles( a );
	lua_pushnumber( l, a[0] ); lua_pushnumber( l, a[1] ); lua_pushnumber( l, a[2] );
	return 3;
}

int l_setAngles( lua_State *l )
{
	if( !LocalGame())
		return luaL_error( l, "ss.setAngles only works in games you host (bots, practice, your own server)" );
	float a[3];
	gEngfuncs.GetViewAngles( a );
	a[0] = Num( l, 1, a[0] );
	a[1] = Num( l, 2, a[1] );
	if( a[0] > 89 ) a[0] = 89;
	if( a[0] < -89 ) a[0] = -89;
	a[1] = fmodf( a[1], 360.0f );
	gEngfuncs.SetViewAngles( a );
	return 0;
}

int l_isLocal( lua_State *l ) { lua_pushboolean( l, LocalGame()); return 1; }

int l_trace( lua_State *l )
{
	float a[3], b[3];
	int next = GetVec( l, 1, a );
	GetVec( l, next, b );
	lua_pushnumber( l, TraceWorld( a, b ));
	return 1;
}

// Drawing -- only inside a "hud" handler.
bool CanDraw( lua_State *l ) { if( !inHud ) luaL_error( l, "drawing only works inside ss.on(\"hud\")" ); return true; }

void Fill( int x, int y, int w, int h, int r, int g, int b, int a )
{
	if( w <= 0 || h <= 0 ) return;
	FillRGBABlend( x, y, w, h, r, g, b, a );
}

int l_fill( lua_State *l )
{
	CanDraw( l );
	Fill( (int)Num( l, 1, 0 ), (int)Num( l, 2, 0 ), (int)Num( l, 3, 0 ), (int)Num( l, 4, 0 ),
		(int)Num( l, 5, 255 ), (int)Num( l, 6, 255 ), (int)Num( l, 7, 255 ), (int)Num( l, 8, 255 ));
	return 0;
}

int l_box( lua_State *l )
{
	CanDraw( l );
	int x = (int)Num( l, 1, 0 ), y = (int)Num( l, 2, 0 ), w = (int)Num( l, 3, 0 ), h = (int)Num( l, 4, 0 );
	int r = (int)Num( l, 5, 255 ), g = (int)Num( l, 6, 255 ), b = (int)Num( l, 7, 255 ), a = (int)Num( l, 8, 255 );
	int t = (int)Num( l, 9, 1 );
	Fill( x, y, w, t, r, g, b, a );
	Fill( x, y + h - t, w, t, r, g, b, a );
	Fill( x, y + t, t, h - 2 * t, r, g, b, a );
	Fill( x + w - t, y + t, t, h - 2 * t, r, g, b, a );
	return 0;
}

void Line( float x1, float y1, float x2, float y2, int r, int g, int b, int a, int t )
{
	float dx = x2 - x1, dy = y2 - y1;
	int steps = (int)fmaxf( fabsf( dx ), fabsf( dy ));
	if( steps > 4000 ) steps = 4000;
	if( steps < 1 ) { Fill( (int)x1, (int)y1, t, t, r, g, b, a ); return; }
	// Runs of pixels along the major axis, one rectangle each.
	bool horizontal = fabsf( dx ) >= fabsf( dy );
	int runStart = 0;
	int lastMinor = horizontal ? (int)y1 : (int)x1;
	for( int i = 1; i <= steps + 1; i++ )
	{
		float f = (float)( i > steps ? steps : i ) / steps;
		int minor = horizontal ? (int)( y1 + dy * f ) : (int)( x1 + dx * f );
		if( minor != lastMinor || i > steps )
		{
			float f0 = (float)runStart / steps;
			int len = i - runStart;
			if( horizontal )
			{
				int x = (int)( x1 + dx * f0 );
				Fill( dx >= 0 ? x : x - len + 1, lastMinor, len, t, r, g, b, a );
			}
			else
			{
				int y = (int)( y1 + dy * f0 );
				Fill( lastMinor, dy >= 0 ? y : y - len + 1, t, len, r, g, b, a );
			}
			runStart = i;
			lastMinor = minor;
		}
	}
}

int l_line( lua_State *l )
{
	CanDraw( l );
	Line( Num( l, 1, 0 ), Num( l, 2, 0 ), Num( l, 3, 0 ), Num( l, 4, 0 ), (int)Num( l, 5, 255 ), (int)Num( l, 6, 255 ),
		(int)Num( l, 7, 255 ), (int)Num( l, 8, 255 ), (int)Num( l, 9, 1 ));
	return 0;
}

int l_circle( lua_State *l )
{
	CanDraw( l );
	float cx = Num( l, 1, 0 ), cy = Num( l, 2, 0 ), rad = Num( l, 3, 10 );
	int r = (int)Num( l, 4, 255 ), g = (int)Num( l, 5, 255 ), b = (int)Num( l, 6, 255 ), a = (int)Num( l, 7, 255 );
	int segs = (int)fminf( fmaxf( rad * 0.6f, 16 ), 96 );
	for( int i = 0; i < segs; i++ )
	{
		float a0 = i * 2 * (float)M_PI / segs, a1 = ( i + 1 ) * 2 * (float)M_PI / segs;
		Line( cx + cosf( a0 ) * rad, cy + sinf( a0 ) * rad, cx + cosf( a1 ) * rad, cy + sinf( a1 ) * rad, r, g, b, a, 1 );
	}
	return 0;
}

int l_text( lua_State *l )
{
	CanDraw( l );
	int x = (int)Num( l, 1, 0 ), y = (int)Num( l, 2, 0 );
	const char *s = luaL_checkstring( l, 3 );
	int end = DrawUtils::DrawHudString( x, y, ScreenWidth, s, (int)Num( l, 4, 255 ), (int)Num( l, 5, 255 ), (int)Num( l, 6, 255 ));
	lua_pushinteger( l, end );
	return 1;
}

int l_textWidth( lua_State *l )
{
	lua_pushinteger( l, DrawUtils::HudStringLen( luaL_checkstring( l, 1 )));
	return 1;
}

// Effects.
int beamSprite = -1;

int l_beam( lua_State *l )
{
	float a[3], b[3];
	int i = GetVec( l, 1, a );
	i = GetVec( l, i, b );
	float r = Num( l, i, 255 ) / 255, g = Num( l, i + 1, 255 ) / 255, bl = Num( l, i + 2, 255 ) / 255;
	float life = Num( l, i + 3, 0.15f ), width = Num( l, i + 4, 1.5f ), bright = Num( l, i + 5, 1 );
	if( beamSprite < 0 ) beamSprite = gEngfuncs.pEventAPI->EV_FindModelIndex( "sprites/laserbeam.spr" );
	if( beamSprite <= 0 ) return 0;
	gEngfuncs.pEfxAPI->R_BeamPoints( a, b, beamSprite, life, width, 0, bright, 0, 0, 0, r, g, bl );
	return 0;
}

int l_light( lua_State *l )
{
	float p[3];
	int i = GetVec( l, 1, p );
	dlight_t *d = gEngfuncs.pEfxAPI->CL_AllocDlight( 0 );
	if( !d ) return 0;
	VectorCopy( p, d->origin );
	d->color.r = (byte)Num( l, i, 255 ); d->color.g = (byte)Num( l, i + 1, 255 ); d->color.b = (byte)Num( l, i + 2, 255 );
	d->radius = Num( l, i + 3, 120 );
	float life = Num( l, i + 4, 0.2f );
	d->die = gEngfuncs.GetClientTime() + life;
	d->decay = life > 0 ? d->radius / life : 0;
	return 0;
}

int l_sparks( lua_State *l )
{
	float p[3];
	GetVec( l, 1, p );
	gEngfuncs.pEfxAPI->R_SparkShower( p );
	return 0;
}

int l_sound( lua_State *l )
{
	const char *s = luaL_checkstring( l, 1 );
	if( strstr( s, ".." )) return luaL_error( l, "bad sound name" );
	float vol = Num( l, 2, 1 );
	if( lua_gettop( l ) >= 3 )
	{
		float p[3];
		GetVec( l, 3, p );
		gEngfuncs.pfnPlaySoundByNameAtLocation( (char *)s, vol, p );
	}
	else
		gEngfuncs.pfnPlaySoundByName( (char *)s, vol );
	return 0;
}

int l_fov( lua_State *l )
{
	int v = (int)Num( l, 1, 0 );
	fovOverride = v <= 0 ? 0 : v < 10 ? 10 : v > 170 ? 170 : v;
	return 0;
}

bool SafeName( const char *s )
{
	for( ; *s; s++ )
		if( !( *s == '_' || ( *s >= 'a' && *s <= 'z' ) || ( *s >= 'A' && *s <= 'Z' ) || ( *s >= '0' && *s <= '9' ))) return false;
	return true;
}

int l_cvar( lua_State *l )
{
	cvar_t *c = gEngfuncs.pfnGetCvarPointer( luaL_checkstring( l, 1 ));
	if( !c ) return 0;
	lua_pushstring( l, c->string );
	return 1;
}

int l_setcvar( lua_State *l )
{
	const char *name = luaL_checkstring( l, 1 );
	const char *value = luaL_tolstring( l, 2, NULL );
	if( !SafeName( name ) || strpbrk( value, "\";\n" )) return luaL_error( l, "bad cvar name or value" );
	char cmd[512];
	snprintf( cmd, sizeof( cmd ), "%s \"%s\"\n", name, value );
	gEngfuncs.pfnClientCmd( cmd );
	return 0;
}

int l_cmd( lua_State *l )
{
	const char *c = luaL_checkstring( l, 1 );
	char buf[512];
	snprintf( buf, sizeof( buf ), "%s\n", c );
	gEngfuncs.pfnClientCmd( buf );
	return 0;
}

int l_keyDown( lua_State *l )
{
	const char *name = luaL_checkstring( l, 1 );
	for( int k = 0; k < 256; k++ )
		if( keyState[k] && !strcmp( KeyName( k ), name )) { lua_pushboolean( l, 1 ); return 1; }
	lua_pushboolean( l, 0 );
	return 1;
}

// Console commands: one engine callback looks the command up by name.
void RunCommand( void );

int l_command( lua_State *l )
{
	const char *name = luaL_checkstring( l, 1 );
	luaL_checktype( l, 2, LUA_TFUNCTION );
	if( !SafeName( name )) return luaL_error( l, "command names are letters, digits and _" );
	lua_getfield( l, LUA_REGISTRYINDEX, "ss_commands" );
	lua_pushvalue( l, 2 );
	lua_setfield( l, -2, name );
	// The engine keeps commands for the whole session, so each name is added once; after a
	// reload RunCommand finds the new function by name.
	static char *added[256];
	static int numAdded;
	for( int i = 0; i < numAdded; i++ ) if( !strcmp( added[i], name )) return 0;
	if( numAdded < 256 )
	{
		added[numAdded] = strdup( name );
		gEngfuncs.pfnAddCommand( added[numAdded++], RunCommand );
	}
	return 0;
}

const luaL_Reg api[] =
{
	{ "on", l_on }, { "print", l_print }, { "time", l_time }, { "screen", l_screen },
	{ "players", l_players }, { "me", l_me }, { "toScreen", l_toScreen }, { "aimAngles", l_aimAngles },
	{ "angles", l_angles }, { "setAngles", l_setAngles }, { "trace", l_trace }, { "isLocal", l_isLocal },
	{ "fill", l_fill }, { "box", l_box }, { "line", l_line }, { "circle", l_circle },
	{ "text", l_text }, { "textWidth", l_textWidth },
	{ "beam", l_beam }, { "light", l_light }, { "sparks", l_sparks }, { "sound", l_sound },
	{ "fov", l_fov }, { "cvar", l_cvar }, { "setcvar", l_setcvar }, { "cmd", l_cmd },
	{ "keyDown", l_keyDown }, { "command", l_command },
	{ NULL, NULL }
};

// ---------------------------------------------------------------------------------------
// Running handlers

// Calls the function on top of the stack with `nargs` arguments below it; logs and returns
// false on error. Leaves `nres` results.
bool Call( int nargs, int nres, const char *what )
{
	budget = budgetTicks;
	if( lua_pcall( L, nargs, nres, 0 ) == LUA_OK ) return true;
	Log( "%s: %s", what, lua_tostring( L, -1 ));
	lua_pop( L, 1 );
	return false;
}

// Runs every handler of `ev`; `push` pushes the arguments and returns how many. Returns true
// if any handler returned true.
template<class F> bool Emit( const char *ev, F push )
{
	if( !L ) return false;
	bool taken = false;
	int top = lua_gettop( L );
	lua_getfield( L, LUA_REGISTRYINDEX, "ss_handlers" );
	if( lua_getfield( L, -1, ev ) != LUA_TTABLE ) { lua_settop( L, top ); return false; }
	int list = lua_gettop( L );
	int n = (int)luaL_len( L, list );
	for( int i = 1; i <= n; i++ )
	{
		lua_rawgeti( L, list, i );
		int h = lua_gettop( L );
		lua_getfield( L, h, "fn" );
		if( lua_isnil( L, -1 )) { lua_settop( L, h - 1 ); continue; }   // switched off after an error
		lua_getfield( L, h, "script" );
		lua_setfield( L, LUA_REGISTRYINDEX, "ss_current" );
		int nargs = push();
		lua_getfield( L, h, "script" );
		char what[96];
		snprintf( what, sizeof( what ), "%s (%s)", lua_tostring( L, -1 ) ? lua_tostring( L, -1 ) : "?", ev );
		lua_pop( L, 1 );
		if( Call( nargs, 1, what ))
		{
			taken |= lua_toboolean( L, -1 );
			lua_pop( L, 1 );
		}
		else
		{
			Log( "%s: switched off until the scripts are reloaded", what );
			lua_pushnil( L );
			lua_setfield( L, h, "fn" );
		}
		lua_settop( L, h - 1 );
	}
	lua_settop( L, top );
	return taken;
}

void RunCommand( void )
{
	if( !L ) return;
	const char *name = gEngfuncs.Cmd_Argv( 0 );
	lua_getfield( L, LUA_REGISTRYINDEX, "ss_commands" );
	if( lua_getfield( L, -1, name ) != LUA_TFUNCTION ) { lua_pop( L, 2 ); gEngfuncs.Con_Printf( "%s: no script provides this\n", name ); return; }
	int argc = gEngfuncs.Cmd_Argc();
	for( int i = 1; i < argc; i++ ) lua_pushstring( L, gEngfuncs.Cmd_Argv( i ));
	Call( argc - 1, 0, name );
	lua_pop( L, 1 );
}

// ---------------------------------------------------------------------------------------
// Loading

void Sandbox( void )
{
	// Only the safe parts of the standard library.
	luaL_requiref( L, LUA_GNAME, luaopen_base, 1 );
	luaL_requiref( L, LUA_TABLIBNAME, luaopen_table, 1 );
	luaL_requiref( L, LUA_STRLIBNAME, luaopen_string, 1 );
	luaL_requiref( L, LUA_MATHLIBNAME, luaopen_math, 1 );
	luaL_requiref( L, LUA_UTF8LIBNAME, luaopen_utf8, 1 );
	luaL_requiref( L, LUA_COLIBNAME, luaopen_coroutine, 1 );
	lua_pop( L, 6 );
	const char *remove[] = { "dofile", "loadfile", "load", "require", "collectgarbage", NULL };
	for( int i = 0; remove[i]; i++ ) { lua_pushnil( L ); lua_setglobal( L, remove[i] ); }
	lua_pushcfunction( L, l_print ); lua_setglobal( L, "print" );

	luaL_newlib( L, api );
	const struct { const char *n; int v; } buttons[] = {
		{ "IN_ATTACK", IN_ATTACK }, { "IN_ATTACK2", IN_ATTACK2 }, { "IN_JUMP", IN_JUMP }, { "IN_DUCK", IN_DUCK },
		{ "IN_RELOAD", IN_RELOAD }, { "IN_USE", IN_USE }, { "IN_FORWARD", IN_FORWARD }, { "IN_BACK", IN_BACK },
		{ "IN_MOVELEFT", IN_MOVELEFT }, { "IN_MOVERIGHT", IN_MOVERIGHT }, { "IN_RUN", IN_RUN } };
	for( auto &b : buttons ) { lua_pushinteger( L, b.v ); lua_setfield( L, -2, b.n ); }
	lua_setglobal( L, "ss" );

	lua_newtable( L ); lua_setfield( L, LUA_REGISTRYINDEX, "ss_handlers" );
	lua_newtable( L ); lua_setfield( L, LUA_REGISTRYINDEX, "ss_commands" );
	lua_sethook( L, Hook, LUA_MASKCOUNT, 1000 );
}

bool LoadScript( const char *name )
{
	if( !SafeName( name ) && !strchr( name, '-' )) { Log( "%s: bad script name", name ); return false; }
	char path[256];
	snprintf( path, sizeof( path ), "silversaint/scripts/%s.lua", name );
	int len = 0;
	byte *data = gEngfuncs.COM_LoadFile( path, 5, &len );
	if( !data ) { Log( "%s: not found (%s)", name, path ); return false; }
	char chunk[300];
	snprintf( chunk, sizeof( chunk ), "=%s", name );
	int err = luaL_loadbufferx( L, (const char *)data, len, chunk, "t" );   // text only, no bytecode
	gEngfuncs.COM_FreeFile( data );
	if( err != LUA_OK ) { Log( "%s", lua_tostring( L, -1 )); lua_pop( L, 1 ); return false; }
	lua_pushstring( L, name );
	lua_setfield( L, LUA_REGISTRYINDEX, "ss_current" );
	bool ok = Call( 0, 0, name );
	if( ok ) Log( "loaded %s", name );
	return ok;
}

void LoadAll( void )
{
	int len = 0;
	byte *data = gEngfuncs.COM_LoadFile( "silversaint/scripts/autoload.txt", 5, &len );
	if( !data ) return;
	char *list = (char *)malloc( len + 1 );
	memcpy( list, data, len );
	list[len] = 0;
	gEngfuncs.COM_FreeFile( data );
	for( char *line = strtok( list, "\r\n" ); line; line = strtok( NULL, "\r\n" ))
	{
		while( *line == ' ' ) line++;
		if( *line && *line != '#' ) LoadScript( line );
	}
	free( list );
}

void Start( void )
{
	OpenLog();
	L = luaL_newstate();
	if( !L ) return;
	Sandbox();
	LoadAll();
}

void Stop( void )
{
	if( L ) lua_close( L );
	L = NULL;
	fovOverride = 0;
	inHud = inMove = false;
}

void Reload_f( void ) { Stop(); Start(); }
void Load_f( void ) { if( !L ) Start(); if( gEngfuncs.Cmd_Argc() > 1 ) LoadScript( gEngfuncs.Cmd_Argv( 1 )); }
}

// ---------------------------------------------------------------------------------------
// Match stats for the player's Silver Saint profile: kills, deaths, headshots, shots, time in a
// game and maps played, in total and on other people's servers ("online"), plus kills per
// weapon. Kept in cstrike/silversaint/stats.txt as "key value" lines; the launcher reads it and
// shows / shares it. Counted here because every kill, death and shot already passes through.

struct StatPart { long kills, deaths, headshots, shots, seconds, matches; };
static struct
{
	StatPart all, online;
	std::map<std::string, long> weapons;
	std::string lastMap, level;
	bool loaded, dirty;
	double partSecond, partSecondOnline, lastSave;
} stats;

static bool StatsPath( char *out, size_t size )
{
	const char *base = getenv( "XASH3D_BASEDIR" );
	if( !base ) return false;
	snprintf( out, size, "%s/cstrike/silversaint/stats.txt", base );
	return true;
}

static void StatsLoad( void )
{
	if( stats.loaded ) return;
	stats.loaded = true;
	char path[1024];
	if( !StatsPath( path, sizeof( path ))) return;
	FILE *f = fopen( path, "r" );
	if( !f ) return;
	char key[96]; long v;
	char mapName[64];
	while( fscanf( f, "%95s", key ) == 1 )
	{
		if( !strcmp( key, "last_map" )) { if( fscanf( f, "%63s", mapName ) == 1 ) stats.lastMap = mapName; continue; }
		if( fscanf( f, "%ld", &v ) != 1 ) break;
		StatPart *part = !strncmp( key, "online.", 7 ) ? &stats.online : &stats.all;
		const char *k = part == &stats.online ? key + 7 : key;
		if( !strncmp( key, "weapon.", 7 )) { stats.weapons[key + 7] = v; continue; }
		if( !strcmp( k, "kills" )) part->kills = v;
		else if( !strcmp( k, "deaths" )) part->deaths = v;
		else if( !strcmp( k, "headshots" )) part->headshots = v;
		else if( !strcmp( k, "shots" )) part->shots = v;
		else if( !strcmp( k, "seconds" )) part->seconds = v;
		else if( !strcmp( k, "matches" )) part->matches = v;
	}
	fclose( f );
}

static void StatsSave( void )
{
	if( !stats.dirty ) return;
	char path[1024], tmp[1100];
	if( !StatsPath( path, sizeof( path ))) return;
	snprintf( tmp, sizeof( tmp ), "%s.tmp", path );
	FILE *f = fopen( tmp, "w" );
	if( !f ) return;
	const StatPart *parts[2] = { &stats.all, &stats.online };
	const char *prefix[2] = { "", "online." };
	for( int i = 0; i < 2; i++ )
		fprintf( f, "%skills %ld\n%sdeaths %ld\n%sheadshots %ld\n%sshots %ld\n%sseconds %ld\n%smatches %ld\n",
			prefix[i], parts[i]->kills, prefix[i], parts[i]->deaths, prefix[i], parts[i]->headshots,
			prefix[i], parts[i]->shots, prefix[i], parts[i]->seconds, prefix[i], parts[i]->matches );
	for( auto &w : stats.weapons ) fprintf( f, "weapon.%s %ld\n", w.first.c_str(), w.second );
	if( !stats.lastMap.empty()) fprintf( f, "last_map %s\n", stats.lastMap.c_str());
	fclose( f );
	rename( tmp, path );
	stats.dirty = false;
}

static void StatsFrame( double time, float dt )
{
	StatsLoad();
	const char *level = gEngfuncs.pfnGetLevelName();
	cl_entity_t *me = gEngfuncs.GetLocalPlayer();
	bool inGame = level && *level && me;
	bool online = inGame && !LocalGame();
	if( inGame && stats.level != level )
	{
		stats.level = level;
		const char *slash = strrchr( level, '/' );
		std::string name = slash ? slash + 1 : level;
		if( name.size() > 4 && name.compare( name.size() - 4, 4, ".bsp" ) == 0 ) name.resize( name.size() - 4 );
		stats.lastMap = name;
		stats.all.matches++;
		if( online ) stats.online.matches++;
		stats.dirty = true;
	}
	else if( !inGame ) stats.level.clear();
	// Time in a game (a hitch or a pause doesn't count).
	if( inGame && dt > 0 && dt < 1 )
	{
		stats.partSecond += dt;
		if( online ) stats.partSecondOnline += dt;
		if( stats.partSecond >= 1 ) { long s = (long)stats.partSecond; stats.all.seconds += s; stats.partSecond -= s; stats.dirty = true; }
		if( stats.partSecondOnline >= 1 ) { long s = (long)stats.partSecondOnline; stats.online.seconds += s; stats.partSecondOnline -= s; }
	}
	if( time - stats.lastSave > 60 || time < stats.lastSave ) { stats.lastSave = time; StatsSave(); }
}

static void StatsDeath( int killer, int victim, int headshot, const char *weapon, int self )
{
	if( !self ) return;
	StatsLoad();
	bool online = !LocalGame();
	if( killer == self && victim != self )
	{
		stats.all.kills++; if( online ) stats.online.kills++;
		if( headshot ) { stats.all.headshots++; if( online ) stats.online.headshots++; }
		std::string w;
		for( const char *c = weapon ? weapon : ""; *c && w.size() < 20; c++ )
			if(( *c >= 'a' && *c <= 'z' ) || ( *c >= '0' && *c <= '9' ) || *c == '_' ) w += *c;
		if( !w.empty()) stats.weapons[w]++;
		stats.dirty = true;
	}
	if( victim == self ) { stats.all.deaths++; if( online ) stats.online.deaths++; stats.dirty = true; }
}

// ---------------------------------------------------------------------------------------
// Hooks

void Scripts_Init( void )
{
	static bool commands;
	if( !commands )
	{
		gEngfuncs.pfnAddCommand( "ss_reload", Reload_f );
		gEngfuncs.pfnAddCommand( "ss_load", Load_f );
		commands = true;
	}
	if( !L ) Start();
}

void Scripts_Shutdown( void ) { StatsSave(); Stop(); if( logFile ) { fclose( logFile ); logFile = NULL; } }

void Scripts_Frame( double time )
{
	float dt = lastFrame > 0 ? (float)( time - lastFrame ) : 0;
	lastFrame = time;
	StatsFrame( time, dt );
	Emit( "frame", [&] { lua_pushnumber( L, dt ); return 1; } );
}

void Scripts_Hud( float )
{
	inHud = true;
	Emit( "hud", [&] { lua_pushinteger( L, ScreenWidth ); lua_pushinteger( L, ScreenHeight ); return 2; } );
	inHud = false;
}

void Scripts_CreateMove( float, usercmd_s *cmd )
{
	if( !L ) return;
	moveCmd = cmd;
	inMove = true;
	// One table shared by every handler, so they can build on each other.
	lua_createtable( L, 0, 4 );
	lua_pushnumber( L, cmd->forwardmove ); lua_setfield( L, -2, "forward" );
	lua_pushnumber( L, cmd->sidemove ); lua_setfield( L, -2, "side" );
	lua_pushnumber( L, cmd->upmove ); lua_setfield( L, -2, "up" );
	lua_pushinteger( L, cmd->buttons ); lua_setfield( L, -2, "buttons" );
	int t = lua_gettop( L );
	Emit( "move", [&] { lua_pushvalue( L, t ); return 1; } );
	if( !LocalGame())
	{
		// Scripts may read the input on other people's servers, but not play for you.
		lua_pop( L, 1 );
		inMove = false;
		moveCmd = NULL;
		return;
	}
	lua_getfield( L, t, "forward" ); cmd->forwardmove = (float)lua_tonumber( L, -1 ); lua_pop( L, 1 );
	lua_getfield( L, t, "side" ); cmd->sidemove = (float)lua_tonumber( L, -1 ); lua_pop( L, 1 );
	lua_getfield( L, t, "up" ); cmd->upmove = (float)lua_tonumber( L, -1 ); lua_pop( L, 1 );
	lua_getfield( L, t, "buttons" ); cmd->buttons = (unsigned short)lua_tointeger( L, -1 ); lua_pop( L, 1 );
	lua_pop( L, 1 );
	inMove = false;
	moveCmd = NULL;
}

int Scripts_Key( int down, int keynum )
{
	if( keynum >= 0 && keynum < 256 ) keyState[keynum] = down != 0;
	const char *name = KeyName( keynum );
	if( !*name ) return 1;
	bool taken = Emit( "key", [&] { lua_pushstring( L, name ); lua_pushboolean( L, down ); return 2; } );
	return taken ? 0 : 1;
}

void Scripts_Death( int killer, int victim, int headshot, const char *weapon )
{
	cl_entity_t *me = gEngfuncs.GetLocalPlayer();
	int self = me ? me->index : 0;
	StatsDeath( killer, victim, headshot, weapon, self );
	Emit( "death", [&] {
		lua_createtable( L, 0, 6 );
		lua_pushinteger( L, killer ); lua_setfield( L, -2, "killer" );
		lua_pushinteger( L, victim ); lua_setfield( L, -2, "victim" );
		lua_pushboolean( L, headshot ); lua_setfield( L, -2, "headshot" );
		lua_pushstring( L, weapon ); lua_setfield( L, -2, "weapon" );
		lua_pushboolean( L, killer == self ); lua_setfield( L, -2, "killerIsMe" );
		lua_pushboolean( L, victim == self ); lua_setfield( L, -2, "victimIsMe" );
		cl_entity_t *v = gEngfuncs.GetEntityByIndex( victim );
		if( v ) { PushVec( L, v->origin ); lua_setfield( L, -2, "where" ); }
		return 1;
	} );
}

void Scripts_Shot( int shooter, const float *from, const float *to, int bulletType )
{
	cl_entity_t *me = gEngfuncs.GetLocalPlayer();
	bool mine = me && shooter == me->index;
	if( mine ) { StatsLoad(); stats.all.shots++; if( !LocalGame()) stats.online.shots++; stats.dirty = true; }
	float muzzle[3];
	VectorCopy( from, muzzle );
	cl_entity_t *src = mine ? gEngfuncs.GetViewModel() : gEngfuncs.GetEntityByIndex( shooter );
	if( src ) VectorCopy( src->attachment[0], muzzle );
	Emit( "shot", [&] {
		lua_createtable( L, 0, 6 );
		lua_pushinteger( L, shooter ); lua_setfield( L, -2, "shooter" );
		lua_pushboolean( L, mine ); lua_setfield( L, -2, "me" );
		PushVec( L, from ); lua_setfield( L, -2, "from" );
		PushVec( L, to ); lua_setfield( L, -2, "to" );
		PushVec( L, muzzle ); lua_setfield( L, -2, "muzzle" );
		lua_pushinteger( L, bulletType ); lua_setfield( L, -2, "bullet" );
		return 1;
	} );
}

void Scripts_PlayerBones( int player, const float *head )
{
	if( player < 1 || player > MAX_PLAYERS ) return;
	VectorCopy( head, heads[player].pos );
	heads[player].time = gEngfuncs.GetClientTime();
}

int Scripts_Fov( int fov )
{
	// Scopes zoom below 90; leave those alone.
	return fovOverride > 0 && fov >= 90 ? fovOverride : fov;
}
