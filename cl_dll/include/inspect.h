// LOCAL ADDITION (Silver Saint): weapon inspect (plays the weapon's own idle). See inspect.cpp.
#pragma once

struct cl_entity_s;

void Inspect_Init( void );
bool Inspect_Active( void );
// Each frame, before the view model is drawn: plays the idle, handles stopping.
void Inspect_Update( struct cl_entity_s *view );
