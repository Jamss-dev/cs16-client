// LOCAL ADDITION (Silver Saint): Lua mod scripts. See scripts.cpp.
#pragma once

struct usercmd_s;

void Scripts_Init( void );             // HUD_Init: load the enabled scripts
void Scripts_Shutdown( void );
void Scripts_Frame( double time );     // HUD_Frame
void Scripts_Hud( float time );        // after the HUD is drawn
void Scripts_CreateMove( float frametime, struct usercmd_s *cmd );   // may change aim/buttons
int  Scripts_Key( int down, int keynum );                            // 0 = a script took the key
void Scripts_Death( int killer, int victim, int headshot, const char *weapon );
void Scripts_Shot( int shooter, const float *from, const float *to, int bulletType );
void Scripts_ViewModelMuzzle( const float *attachment, const float *gunOrigin );   // the first-person gun was just drawn
struct cl_entity_s *Scripts_UserEntity( int index );   // HUD_GetUserEntity: 1 = the first-person muzzle
void Scripts_PlayerBones( int player, const float *head );          // head position, per frame
int  Scripts_Fov( int fov );           // a script's FOV, or `fov` unchanged
