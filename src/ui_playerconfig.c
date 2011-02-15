/*
Copyright (C) 2003-2008 Andrey Nazarov

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

#include "ui_local.h"


/*
=============================================================================

PLAYER CONFIG MENU

=============================================================================
*/

#define ID_MODEL 103
#define ID_SKIN    104

typedef struct m_player_s {
    menuFrameWork_t     menu;
    menuField_t         name;
    menuSpinControl_t   model;
    menuSpinControl_t   skin;
    menuSpinControl_t   hand;

    refdef_t    refdef;
    entity_t    entities[2];

    int        time;
    int        oldTime;

    char *pmnames[MAX_PLAYERMODELS];
} m_player_t;

static m_player_t    m_player;

static const char *handedness[] = {
    "right",
    "left",
    "center",
    0
};

static void ReloadMedia( void ) {
    char scratch[MAX_QPATH];
    char *model = uis.pmi[m_player.model.curvalue].directory;
    char *skin = uis.pmi[m_player.model.curvalue].skindisplaynames[m_player.skin.curvalue];

    Q_concat( scratch, sizeof( scratch ), "players/", model, "/tris.md2", NULL );
    m_player.entities[0].model = R_RegisterModel( scratch );

    Q_concat( scratch, sizeof( scratch ), "players/", model, "/", skin, ".pcx", NULL );
    m_player.entities[0].skin = R_RegisterSkin( scratch );

    if( uis.weaponModel[0] ) {
        Q_concat( scratch, sizeof( scratch ), "players/", model, "/", uis.weaponModel, NULL );
        m_player.entities[1].model = R_RegisterModel( scratch );
    }
}

static void RunFrame( void ) {
    int frame;
    int i;

    if( m_player.time < uis.realtime ) {
        m_player.oldTime = m_player.time;

        m_player.time += 120;
        if( m_player.time < uis.realtime ) {
            m_player.time = uis.realtime;
        }

        frame = ( m_player.time / 120 ) % 40;

        for( i = 0; i < m_player.refdef.num_entities; i++ ) {
            m_player.entities[i].oldframe = m_player.entities[i].frame;
            m_player.entities[i].frame = frame;
        }
    }
}

static void Draw( menuFrameWork_t *self ) {
    float backlerp;
    int i;

    m_player.refdef.time = uis.realtime * 0.001f;

    RunFrame();

    if( m_player.time == m_player.oldTime ) {
        backlerp = 0;
    } else {
        backlerp = 1 - ( float )( uis.realtime - m_player.oldTime ) /
            ( float )( m_player.time - m_player.oldTime );
    }

    for( i = 0; i < m_player.refdef.num_entities; i++ ) {
        m_player.entities[i].backlerp = backlerp;
    }

    Menu_Draw( self );    

    R_RenderFrame( &m_player.refdef );
}

static void Size( menuFrameWork_t *self ) {
    int x = uis.width / 2 - 130;
    int y = uis.height / 2 - 97;

    m_player.refdef.x = uis.width / uis.scale / 2;
    m_player.refdef.y = 60;
    m_player.refdef.width = uis.width / uis.scale / 2;
    m_player.refdef.height = uis.height / uis.scale - 122;

    m_player.refdef.fov_x = 40;
    m_player.refdef.fov_y = V_CalcFov( m_player.refdef.fov_x,
        m_player.refdef.width, m_player.refdef.height );

    m_player.name.generic.x        = x;
    m_player.name.generic.y        = y;
    y += 32;

    m_player.model.generic.x    = x;
    m_player.model.generic.y    = y;
    y += 16;

    m_player.skin.generic.x    = x;
    m_player.skin.generic.y    = y;
    y += 16;

    m_player.hand.generic.x    = x;
    m_player.hand.generic.y    = y;
}

static menuSound_t Change( menuCommon_t *self ) {
    switch( self->id ) {
    case ID_MODEL:
        m_player.skin.itemnames =
            uis.pmi[m_player.model.curvalue].skindisplaynames;
        m_player.skin.curvalue = 0;
        SpinControl_Init( &m_player.skin );
        // fall through
    case ID_SKIN:
        ReloadMedia();
        break;
    default:
        break;
    }
    return QMS_MOVE;
}

static void Pop( menuFrameWork_t *self ) {
    char scratch[MAX_OSPATH];

    Cvar_SetEx( "name", m_player.name.field.text, FROM_CONSOLE );

    Q_concat( scratch, sizeof( scratch ),
        uis.pmi[m_player.model.curvalue].directory, "/",
        uis.pmi[m_player.model.curvalue].skindisplaynames[m_player.skin.curvalue], NULL );

    Cvar_SetEx( "skin", scratch, FROM_CONSOLE );

    Cvar_SetEx( "hand", va( "%d", m_player.hand.curvalue ), FROM_CONSOLE );
}

static qboolean Push( menuFrameWork_t *self ) {
    char currentdirectory[MAX_QPATH];
    char currentskin[MAX_QPATH];
    int i, j;
    int currentdirectoryindex = 0;
    int currentskinindex = 0;
    char *p;

    // find and register all player models
    if( !uis.numPlayerModels ) {
        PlayerModel_Load();
        if( !uis.numPlayerModels ) {
            return qfalse;
        }
    }

    Cvar_VariableStringBuffer( "skin", currentdirectory, sizeof( currentdirectory ) );

    if( ( p = strchr( currentdirectory, '/' ) ) || ( p = strchr( currentdirectory, '\\' ) ) ) {
        *p++ = 0;
        Q_strlcpy( currentskin, p, sizeof( currentskin ) );
    } else {
        strcpy( currentdirectory, "male" );
        strcpy( currentskin, "grunt" );
    }

    for( i = 0 ; i < uis.numPlayerModels ; i++ ) {
        m_player.pmnames[i] = uis.pmi[i].directory;
        if( Q_stricmp( uis.pmi[i].directory, currentdirectory ) == 0 ) {
            currentdirectoryindex = i;

            for( j = 0 ; j < uis.pmi[i].nskins ; j++ ) {
                if( Q_stricmp( uis.pmi[i].skindisplaynames[j], currentskin ) == 0 ) {
                    currentskinindex = j;
                    break;
                }
            }
        }
    }

    IF_Init( &m_player.name.field, 15, 15 );
    IF_Replace( &m_player.name.field, Cvar_VariableString( "name" ) );

    m_player.model.curvalue = currentdirectoryindex;
    m_player.model.itemnames = m_player.pmnames;

    m_player.skin.curvalue = currentskinindex;
    m_player.skin.itemnames = uis.pmi[currentdirectoryindex].skindisplaynames;

    m_player.hand.curvalue = Cvar_VariableInteger( "hand" );
    clamp( m_player.hand.curvalue, 0, 2 );

    ReloadMedia();

    // set up oldframe correctly
    m_player.time = uis.realtime - 120;
    m_player.oldTime = m_player.time;
    RunFrame();

    return qtrue;
}

static void Free( menuFrameWork_t *self ) {
    memset( &m_player, 0, sizeof( m_player ) );
}

void M_Menu_PlayerConfig( void ) {
    static const vec3_t origin = { 80.0f, 5.0f, 0.0f };
    static const vec3_t angles = { 0.0f, 260.0f, 0.0f };

    m_player.menu.name = "players";
    m_player.menu.title = "Player Setup";
    m_player.menu.push = Push;
    m_player.menu.pop = Pop;
    m_player.menu.size = Size;
    m_player.menu.draw = Draw;
    m_player.menu.free = Free;
    m_player.menu.image = uis.backgroundHandle;
    FastColorCopy( uis.color.background, m_player.menu.color );

    m_player.entities[0].flags = RF_FULLBRIGHT;
    VectorCopy( angles, m_player.entities[0].angles );
    VectorCopy( origin, m_player.entities[0].origin );
    VectorCopy( origin, m_player.entities[0].oldorigin );

    m_player.entities[1].flags = RF_FULLBRIGHT;
    VectorCopy( angles, m_player.entities[1].angles );
    VectorCopy( origin, m_player.entities[1].origin );
    VectorCopy( origin, m_player.entities[1].oldorigin );

    m_player.refdef.num_entities = 1;
    if( uis.weaponModel[0] ) {
        m_player.refdef.num_entities++;
    }

    m_player.refdef.entities = m_player.entities;
    m_player.refdef.rdflags = RDF_NOWORLDMODEL;

    m_player.name.generic.type = MTYPE_FIELD;
    m_player.name.generic.flags = QMF_HASFOCUS;
    m_player.name.generic.name = "name";

    m_player.model.generic.type = MTYPE_SPINCONTROL;
    m_player.model.generic.id = ID_MODEL;
    m_player.model.generic.name = "model";
    m_player.model.generic.change = Change;

    m_player.skin.generic.type = MTYPE_SPINCONTROL;
    m_player.skin.generic.id = ID_SKIN;
    m_player.skin.generic.name = "skin";
    m_player.skin.generic.change = Change;

    m_player.hand.generic.type = MTYPE_SPINCONTROL;
    m_player.hand.generic.name = "handedness";
    m_player.hand.itemnames = ( char ** )handedness;

    Menu_AddItem( &m_player.menu, &m_player.name );
    Menu_AddItem( &m_player.menu, &m_player.model );
    Menu_AddItem( &m_player.menu, &m_player.skin );
    Menu_AddItem( &m_player.menu, &m_player.hand );

    List_Append( &ui_menus, &m_player.menu.entry );
}

