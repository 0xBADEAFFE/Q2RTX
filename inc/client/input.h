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

#ifndef INPUT_H
#define INPUT_H

//
// input.h -- external (non-keyboard) input devices
//

typedef enum {
    IN_FREE,
    IN_SHOW,
    IN_HIDE,
    IN_GRAB
} grab_t;

typedef struct inputAPI_s {
    qboolean (*Init)(void);
    void (*Shutdown)(void);
    void (*Grab)(grab_t grab);
    void (*Warp)(int x, int y);
    void (*GetEvents)(void);
    qboolean(*GetMotion)(int *dx, int *dy);
} inputAPI_t;

void VID_FillInputAPI(inputAPI_t *api);

#if USE_DINPUT
void DI_FillAPI(inputAPI_t *api);
#endif

void IN_Frame(void);
void IN_Activate(void);
void IN_MouseEvent(int x, int y);
void IN_WarpMouse(int x, int y);

#endif // INPUT_H
