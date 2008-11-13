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

//
// sv_mvd.c - GTV server and local MVD recorder
//

#include "sv_local.h"
#include "q_fifo.h"
#include "net_stream.h"
#include "mvd_gtv.h"

#define FOR_EACH_GTV( client ) \
    LIST_FOR_EACH( gtv_client_t, client, &gtv_client_list, entry )

#define FOR_EACH_ACTIVE_GTV( client ) \
    LIST_FOR_EACH( gtv_client_t, client, &gtv_active_list, active )

typedef struct {
    list_t      entry;
    list_t      active;
    clstate_t   state;
    netstream_t stream;
#if USE_ZLIB
    z_stream    z;
#endif
    unsigned    msglen;
    unsigned    lastmessage; 

    unsigned    flags;
    unsigned    maxbuf;
    unsigned    bufcount;

    byte        buffer[MAX_GTC_MSGLEN+4]; // recv buffer
    byte        *data; // send buffer

    char        name[MAX_CLIENT_NAME];
    char        version[MAX_QPATH];
} gtv_client_t;

typedef struct {
    qboolean        active;
    client_t        *dummy;
    unsigned        layout_time;
    unsigned        clients_active;
    unsigned        players_active;

    // reliable data, may not be discarded
    sizebuf_t       message;

    // unreliable data, may be discarded
    sizebuf_t       datagram;

    // delta compressor buffers
    player_state_t  *players;  // [maxclients]
    entity_state_t  *entities; // [MAX_EDICTS]

    // local recorder
    fileHandle_t    recording; 
    int             numlevels; // stop after that many levels
    int             numframes; // stop after that many frames

    // TCP client pool
    gtv_client_t    *clients; // [sv_mvd_maxclients]
} mvd_server_t;

static mvd_server_t     mvd;

// TCP client lists
static LIST_DECL( gtv_client_list );
static LIST_DECL( gtv_active_list );

static LIST_DECL( gtv_host_list );

static cvar_t   *sv_mvd_enable;
static cvar_t   *sv_mvd_maxclients;
static cvar_t   *sv_mvd_bufsize;
static cvar_t   *sv_mvd_password;
static cvar_t   *sv_mvd_noblend;
static cvar_t   *sv_mvd_nogun;
static cvar_t   *sv_mvd_nomsgs;
static cvar_t   *sv_mvd_maxsize;
static cvar_t   *sv_mvd_maxtime;
static cvar_t   *sv_mvd_maxmaps;
static cvar_t   *sv_mvd_begincmd;
static cvar_t   *sv_mvd_scorecmd;
static cvar_t   *sv_mvd_autorecord;
static cvar_t   *sv_mvd_capture_flags;
static cvar_t   *sv_mvd_disconnect_time;
static cvar_t   *sv_mvd_suspend_time;
static cvar_t   *sv_mvd_allow_stufftext;

static qboolean mvd_enable( void );
static void     mvd_disable( void );
static void     mvd_drop( gtv_serverop_t op );

static void     write_stream( gtv_client_t *client, void *data, size_t len );
static void     write_message( gtv_client_t *client, gtv_serverop_t op );
#if USE_ZLIB
static void     flush_stream( gtv_client_t *client, int flush );
#endif

static void     rec_stop( void ); 
static qboolean rec_allowed( void );
static void     rec_start( fileHandle_t demofile );
static void     rec_write( void );


/*
==============================================================================

DUMMY MVD CLIENT

MVD dummy is a fake client maintained entirely server side.
Representing MVD observers, this client is used to obtain base playerstate
for freefloat observers, receive scoreboard updates and text messages, etc.

==============================================================================
*/

static cmdbuf_t    dummy_buffer;
static char        dummy_buffer_text[MAX_STRING_CHARS];

static void dummy_wait_f( void ) {
    int count = atoi( Cmd_Argv( 1 ) );

    if( count < 1 ) {
        count = 1;
    }
    dummy_buffer.waitCount = count;
}

static void dummy_command( void ) {
    sv_client = mvd.dummy;
    sv_player = sv_client->edict;
    ge->ClientCommand( sv_player );
    sv_client = NULL;
    sv_player = NULL;
}

static void dummy_forward_f( void ) {
    Cmd_Shift();
    Com_DPrintf( "dummy cmd: %s %s\n", Cmd_Argv( 0 ), Cmd_Args() );
    dummy_command();
}

static void dummy_record_f( void ) {
    char buffer[MAX_OSPATH];
    fileHandle_t demofile;
    size_t len;

    if( !sv_mvd_autorecord->integer ) {
        return;
    }

    if( Cmd_Argc() < 2 ) {
        Com_Printf( "Usage: %s <filename>\n", Cmd_Argv( 0 ) );
        return;
    }

    if( !rec_allowed() ) {
        return;
    }

    len = Q_concat( buffer, sizeof( buffer ), "demos/", Cmd_Argv( 1 ), ".mvd2", NULL );
    if( len >= sizeof( buffer ) ) {
        Com_EPrintf( "Oversize filename specified.\n" );
        return;
    }

    FS_FOpenFile( buffer, &demofile, FS_MODE_WRITE );
    if( !demofile ) {
        Com_EPrintf( "Couldn't open %s for writing\n", buffer );
        return;
    }

    if( !mvd_enable() ) {
        FS_FCloseFile( demofile );
        return;
    }

    rec_start( demofile );

    Com_Printf( "Auto-recording local MVD to %s\n", buffer );
}

static void dummy_stop_f( void ) {
    if( !sv_mvd_autorecord->integer ) {
        return;
    }

    if( !mvd.recording ) {
        Com_Printf( "Not recording a local MVD.\n" );
        return;
    }

    Com_Printf( "Stopped local MVD auto-recording.\n" );
    rec_stop();
}

static const ucmd_t dummy_cmds[] = {
    { "cmd", dummy_forward_f },
    { "set", Cvar_Set_f },
    { "alias", Cmd_Alias_f },
    { "play", NULL },
    { "stopsound", NULL },
    { "exec", NULL },
    { "screenshot", NULL },
    { "wait", dummy_wait_f },
    { "record", dummy_record_f },
    { "stop", dummy_stop_f },
    { NULL, NULL }
};

static void dummy_exec_string( const char *line ) {
    char *cmd, *alias;
    const ucmd_t *u;
    cvar_t *v;

    if( !line[0] ) {
        return;
    }

    Cmd_TokenizeString( line, qtrue );

    cmd = Cmd_Argv( 0 );
    if( !cmd[0] ) {
        return;
    }
    if( ( u = Com_Find( dummy_cmds, cmd ) ) != NULL ) {
        if( u->func ) {
            u->func();
        }
        return;
    }

    alias = Cmd_AliasCommand( cmd );
    if( alias ) {
        if( ++dummy_buffer.aliasCount == ALIAS_LOOP_COUNT ) {
            Com_WPrintf( "%s: runaway alias loop\n", __func__ );
            return;
        }
        Cbuf_InsertTextEx( &dummy_buffer, alias );
        return;
    }

    v = Cvar_FindVar( cmd );
    if( v ) {
        Cvar_Command( v );
        return;
    }

    Com_DPrintf( "dummy forward: %s\n", line );
    dummy_command();
}

static void dummy_add_message( client_t *client, byte *data,
                              size_t length, qboolean reliable )
{
    char *text;

    if( !length || !reliable || data[0] != svc_stufftext ) {
        return; // not interesting
    }

    if( sv_mvd_allow_stufftext->integer <= 0 ) {
        return; // not allowed
    }

    data[length] = 0;
    text = ( char * )( data + 1 );
    Com_DPrintf( "dummy stufftext: %s\n", text );
    Cbuf_AddTextEx( &dummy_buffer, text );
}

static void dummy_spawn( void ) {
    sv_client = mvd.dummy;
    sv_player = sv_client->edict;
    ge->ClientBegin( sv_player );
    sv_client = NULL;
    sv_player = NULL;

    if( sv_mvd_begincmd->string[0] ) {
        Cbuf_AddTextEx( &dummy_buffer, sv_mvd_begincmd->string );
    }

    mvd.layout_time = svs.realtime;

    mvd.dummy->state = cs_spawned;
}

static client_t *dummy_find_slot( void ) {
    client_t *c;
    int i, j;

    // first check if there is a free reserved slot
    j = sv_maxclients->integer - sv_reserved_slots->integer;
    for( i = j; i < sv_maxclients->integer; i++ ) {
        c = &svs.udp_client_pool[i];
        if( !c->state ) {
            return c;
        }
    }

    // then check regular slots
    for( i = 0; i < j; i++ ) {
        c = &svs.udp_client_pool[i];
        if( !c->state ) {
            return c;
        }
    }

    return NULL;
}

static qboolean dummy_create( void ) {
    client_t *newcl;
    char userinfo[MAX_INFO_STRING];
    char *s;
    qboolean allow;
    int number;

    // find a free client slot
    newcl = dummy_find_slot();
    if( !newcl ) {
        Com_EPrintf( "No slot for dummy MVD client\n" );
        return qfalse;
    }

    memset( newcl, 0, sizeof( *newcl ) );
    number = newcl - svs.udp_client_pool;
    newcl->number = newcl->slot = number;
    newcl->protocol = -1;
    newcl->state = cs_connected;
    newcl->AddMessage = dummy_add_message;
    newcl->edict = EDICT_NUM( number + 1 );
    newcl->netchan = SV_Mallocz( sizeof( netchan_t ) );
    newcl->netchan->remote_address.type = NA_LOOPBACK;

    List_Init( &newcl->entry );

    Q_snprintf( userinfo, sizeof( userinfo ),
        "\\name\\[MVDSPEC]\\skin\\male/grunt\\mvdspec\\%d\\ip\\loopback",
        PROTOCOL_VERSION_MVD_CURRENT );

    mvd.dummy = newcl;

    // get the game a chance to reject this connection or modify the userinfo
    sv_client = newcl;
    sv_player = newcl->edict;
    allow = ge->ClientConnect( newcl->edict, userinfo );
    sv_client = NULL;
    sv_player = NULL;
    if ( !allow ) {
        s = Info_ValueForKey( userinfo, "rejmsg" );
        if( *s ) {
            Com_EPrintf( "Dummy MVD client rejected by game DLL: %s\n", s );
        }
        mvd.dummy = NULL;
        return qfalse;
    }

    // parse some info from the info strings
    strcpy( newcl->userinfo, userinfo );
    SV_UserinfoChanged( newcl );

    return qtrue;
}

static void dummy_run( void ) {
    usercmd_t cmd;

    Cbuf_ExecuteEx( &dummy_buffer );
    if( dummy_buffer.waitCount > 0 ) {
        dummy_buffer.waitCount--;
    }

    // run ClientThink to prevent timeouts, etc
    memset( &cmd, 0, sizeof( cmd ) );
    cmd.msec = 100;
    sv_client = mvd.dummy;
    sv_player = sv_client->edict;
    ge->ClientThink( sv_player, &cmd );
    sv_client = NULL;
    sv_player = NULL;

    // check if the layout is constantly updated. if not,
    // game mod has probably closed the scoreboard, open it again
    if( mvd.active && sv_mvd_scorecmd->string[0] ) {
        if( svs.realtime - mvd.layout_time > 9000 ) {
            Cbuf_AddTextEx( &dummy_buffer, sv_mvd_scorecmd->string );
            mvd.layout_time = svs.realtime;
        }
    }
}

/*
==============================================================================

FRAME UPDATES

As MVD stream operates over reliable transport, there is no concept of
"baselines" and delta compression is always performed from the last
state seen on the map. There is also no support for "nodelta" frames
(except the very first frame sent as part of the gamestate).

This allows building only one update per frame and multicasting it to
several destinations at once.

Additional bandwidth savings are performed by filtering out origin and
angles updates on player entities, as MVD client can easily recover them
from corresponding player states, assuming those are kept in sync by the
game mod. This assumption should be generally true for moving players,
as vanilla Q2 server performs PVS/PHS culling for them using origin from
entity states, but not player states.

==============================================================================
*/

/*
==================
player_is_active

Attempts to determine if the given player entity is active,
and the given player state should be captured into MVD stream.

Entire function is a nasty hack. Ideally a compatible game DLL
should do it for us by providing some SVF_* flag or something.
==================
*/
static qboolean player_is_active( const edict_t *ent ) {
    int num;

    if( ( g_features->integer & GMF_PROPERINUSE ) && !ent->inuse ) {
        return qfalse;
    }

    // not a client at all?
    if( !ent->client ) {
        return qfalse;
    }

    num = NUM_FOR_EDICT( ent ) - 1;
    if( num < 0 || num >= sv_maxclients->integer ) {
        return qfalse;
    }

    // by default, check if client is actually connected
    // it may not be the case for bots!
    if( sv_mvd_capture_flags->integer & 1 ) {
        if( svs.udp_client_pool[num].state != cs_spawned ) {
            return qfalse;
        }
    }

    // first of all, make sure player_state_t is valid
    if( !ent->client->ps.fov ) {
        return qfalse;
    }

    // always capture dummy MVD client
    if( ent == mvd.dummy->edict ) {
        return qtrue;
    }

    // never capture spectators
    if( ent->client->ps.pmove.pm_type == PM_SPECTATOR ) {
        return qfalse;
    }

    // check entity visibility
    if( ( ent->svflags & SVF_NOCLIENT ) || !ES_INUSE( &ent->s ) ) {
        // never capture invisible entities
        if( sv_mvd_capture_flags->integer & 2 ) {
            return qfalse;
        }
    } else {
        // always capture visible entities (default)
        if( sv_mvd_capture_flags->integer & 4 ) {
            return qtrue;
        }
    }

    // they are likely following someone in case of PM_FREEZE
    if( ent->client->ps.pmove.pm_type == PM_FREEZE ) {
        return qfalse;
    }

    // they are likely following someone if PMF_NO_PREDICTION is set 
    if( ent->client->ps.pmove.pm_flags & PMF_NO_PREDICTION ) {
        return qfalse;
    }

    return qtrue;
}

/*
==================
build_gamestate

Initialize MVD delta compressor for the first time on the given map.
==================
*/
static void build_gamestate( void ) {
    player_state_t *ps;
    entity_state_t *es;
    edict_t *ent;
    int i;

    memset( mvd.players, 0, sizeof( player_state_t ) * sv_maxclients->integer );
    memset( mvd.entities, 0, sizeof( entity_state_t ) * MAX_EDICTS );

    // set base player states
    for( i = 0; i < sv_maxclients->integer; i++ ) {
        ent = EDICT_NUM( i + 1 );

        if( !player_is_active( ent ) ) {
            continue;
        }

        ps = &mvd.players[i];
        *ps = ent->client->ps;
        PPS_INUSE( ps ) = qtrue;
    }

    // set base entity states
    for( i = 1; i < ge->num_edicts; i++ ) {
        ent = EDICT_NUM( i );

        if( ( ent->svflags & SVF_NOCLIENT ) || !ES_INUSE( &ent->s ) ) {
            continue;
        }

        es = &mvd.entities[i];
        *es = ent->s;
        es->number = i;
    }
}


/*
==================
emit_gamestate

Writes a single giant message with all the startup info,
followed by an uncompressed (baseline) frame.
==================
*/
static void emit_gamestate( void ) {
    char        *string;
    int         i, j;
    player_state_t  *ps;
    entity_state_t  *es;
    size_t      length;
    int         flags, extra, portalbytes;
    byte        portalbits[MAX_MAP_AREAS/8];

    // pack MVD stream flags into extra bits
    extra = 0;
    if( sv_mvd_nomsgs->integer ) {
        extra |= MVF_NOMSGS << SVCMD_BITS;
    }

    // send the serverdata
    MSG_WriteByte( mvd_serverdata | extra );
    MSG_WriteLong( PROTOCOL_VERSION_MVD );
    MSG_WriteShort( PROTOCOL_VERSION_MVD_CURRENT );
    MSG_WriteLong( sv.spawncount );
    MSG_WriteString( fs_game->string );
    MSG_WriteShort( mvd.dummy->number );

    // send configstrings
    for( i = 0; i < MAX_CONFIGSTRINGS; i++ ) {
        string = sv.configstrings[i];
        if( !string[0] ) {
            continue;
        }
        length = strlen( string );
        if( length > MAX_QPATH ) {
            length = MAX_QPATH;
        }

        MSG_WriteShort( i );
        MSG_WriteData( string, length );
        MSG_WriteByte( 0 );
    }
    MSG_WriteShort( MAX_CONFIGSTRINGS );

    // send baseline frame
    portalbytes = CM_WritePortalBits( &sv.cm, portalbits );
    MSG_WriteByte( portalbytes );
    MSG_WriteData( portalbits, portalbytes );
    
    // send player states
    flags = 0;
    if( sv_mvd_noblend->integer ) {
        flags |= MSG_PS_IGNORE_BLEND;
    }
    if( sv_mvd_nogun->integer ) {
        flags |= MSG_PS_IGNORE_GUNINDEX|MSG_PS_IGNORE_GUNFRAMES;
    }
    for( i = 0, ps = mvd.players; i < sv_maxclients->integer; i++, ps++ ) {
        extra = 0;
        if( !PPS_INUSE( ps ) ) {
            extra |= MSG_PS_REMOVE;
        }
        MSG_WriteDeltaPlayerstate_Packet( NULL, ps, i, flags | extra );
    }
    MSG_WriteByte( CLIENTNUM_NONE );

    // send entity states
    for( i = 1, es = mvd.entities + 1; i < ge->num_edicts; i++, es++ ) {
        flags = 0;
        if( ( j = es->number ) != 0 ) {
            if( i <= sv_maxclients->integer ) {
                ps = &mvd.players[ i - 1 ];
                if( PPS_INUSE( ps ) && ps->pmove.pm_type == PM_NORMAL ) {
                    flags |= MSG_ES_FIRSTPERSON;
                }
            }
        } else {
            flags |= MSG_ES_REMOVE;
        }
        es->number = i;
        MSG_WriteDeltaEntity( NULL, es, flags );
        es->number = j;
    }
    MSG_WriteShort( 0 );
}


static void copy_entity_state( entity_state_t *dst, const entity_state_t *src, int flags ) {
    if( !( flags & MSG_ES_FIRSTPERSON ) ) {
        VectorCopy( src->origin, dst->origin );
        VectorCopy( src->angles, dst->angles );
        VectorCopy( src->old_origin, dst->old_origin );
    }
    dst->modelindex = src->modelindex;
    dst->modelindex2 = src->modelindex2;
    dst->modelindex3 = src->modelindex3;
    dst->modelindex4 = src->modelindex4;
    dst->frame = src->frame;
    dst->skinnum = src->skinnum;
    dst->effects = src->effects;
    dst->renderfx = src->renderfx;
    dst->solid = src->solid;
    dst->sound = src->sound;
    dst->event = 0;
}

/*
==================
emit_frame

Builds new MVD frame by capturing all entity and player states
and calculating portalbits. The same frame is used for all MVD
clients, as well as local recorder.
==================
*/
static void emit_frame( void ) {
    player_state_t *oldps, *newps;
    entity_state_t *oldes, *newes;
    edict_t *ent;
    int flags, portalbytes;
    byte portalbits[MAX_MAP_AREAS/8];
    int i;

    MSG_WriteByte( mvd_frame );

    // send portal bits
    portalbytes = CM_WritePortalBits( &sv.cm, portalbits );
    MSG_WriteByte( portalbytes );
    MSG_WriteData( portalbits, portalbytes );
    
    flags = MSG_PS_IGNORE_PREDICTION|MSG_PS_IGNORE_DELTAANGLES;
    if( sv_mvd_noblend->integer ) {
        flags |= MSG_PS_IGNORE_BLEND;
    }
    if( sv_mvd_nogun->integer ) {
        flags |= MSG_PS_IGNORE_GUNINDEX|MSG_PS_IGNORE_GUNFRAMES;
    }

    // send player states
    for( i = 0; i < sv_maxclients->integer; i++ ) {
        ent = EDICT_NUM( i + 1 );

        oldps = &mvd.players[i];
        newps = &ent->client->ps;

        if( !player_is_active( ent ) ) {
            if( PPS_INUSE( oldps ) ) {
                // the old player isn't present in the new message
                MSG_WriteDeltaPlayerstate_Packet( NULL, NULL, i, flags );
                PPS_INUSE( oldps ) = qfalse;
            }
            continue;
        }

        if( PPS_INUSE( oldps ) ) {
            // delta update from old position
            // because the force parm is false, this will not result
            // in any bytes being emited if the player has not changed at all
            MSG_WriteDeltaPlayerstate_Packet( oldps, newps, i, flags );
        } else {
            // this is a new player, send it from the last state
            MSG_WriteDeltaPlayerstate_Packet( oldps, newps, i,
                flags | MSG_PS_FORCE );
        }

        // shuffle current state to previous
        *oldps = *newps;
        PPS_INUSE( oldps ) = qtrue;
    }

    MSG_WriteByte( CLIENTNUM_NONE );    // end of packetplayers

    // send entity states
    for( i = 1; i < ge->num_edicts; i++ ) {
        ent = EDICT_NUM( i );

        oldes = &mvd.entities[i];
        newes = &ent->s;

        if( ( ent->svflags & SVF_NOCLIENT ) || !ES_INUSE( newes ) ) {
            if( oldes->number ) {
                // the old entity isn't present in the new message
                MSG_WriteDeltaEntity( oldes, NULL, MSG_ES_FORCE );
                oldes->number = 0;
            }
            continue;
        }

        // calculate flags
        flags = 0;
        if( i <= sv_maxclients->integer ) {
            oldps = &mvd.players[ i - 1 ];
            if( PPS_INUSE( oldps ) && oldps->pmove.pm_type == PM_NORMAL ) {
                // do not waste bandwidth on origin/angle updates,
                // client will recover them from player state
                flags |= MSG_ES_FIRSTPERSON;
            }
        }
        
        if( !oldes->number ) {
            // this is a new entity, send it from the last state
            flags |= MSG_ES_FORCE|MSG_ES_NEWENTITY;
        }
        
        MSG_WriteDeltaEntity( oldes, newes, flags );

        // shuffle current state to previous
        copy_entity_state( oldes, newes, flags );
        oldes->number = i;
    }

    MSG_WriteShort( 0 );    // end of packetentities
}

static void suspend_streams( void ) {
    gtv_client_t *client;

    FOR_EACH_ACTIVE_GTV( client ) {
        // send stream suspend marker
        write_message( client, GTS_STREAM_DATA );
#if USE_ZLIB
        flush_stream( client, Z_SYNC_FLUSH );
#endif
    }
    Com_DPrintf( "Suspending MVD streams.\n" );
    mvd.active = qfalse;
}

static void resume_streams( void ) {
    gtv_client_t *client;

    // build and emit gamestate
    build_gamestate();
    emit_gamestate();

    FOR_EACH_ACTIVE_GTV( client ) {
        // send gamestate
        write_message( client, GTS_STREAM_DATA );
#if USE_ZLIB
        flush_stream( client, Z_SYNC_FLUSH );
#endif
    }

    // write it to demofile
    if( mvd.recording ) {
        rec_write();
    }

    // clear gamestate
    SZ_Clear( &msg_write );

    SZ_Clear( &mvd.datagram );
    SZ_Clear( &mvd.message );

    Com_DPrintf( "Resuming MVD streams.\n" );
    mvd.active = qtrue;
}

static qboolean players_active( void ) {
    int i;
    edict_t *ent;

    for( i = 0; i < sv_maxclients->integer; i++ ) {
        ent = EDICT_NUM( i + 1 );
        if( ent != mvd.dummy->edict && player_is_active( ent ) ) {
            return qtrue;
        }
    }
    return qfalse;
}

/*
==================
SV_MvdBeginFrame
==================
*/
void SV_MvdBeginFrame( void ) {
    unsigned delta;

    // do nothing if not enabled
    if( !mvd.dummy ) {
        return;
    }

    delta = sv_mvd_disconnect_time->value * 60 * 1000;

    // disconnect MVD dummy if no MVD clients are active for some time
    // FIXME: should not really count unauthenticated/zombie clients
    if( !delta || mvd.recording || !LIST_EMPTY( &gtv_client_list ) ) {
        mvd.clients_active = svs.realtime;
    } else if( svs.realtime - mvd.clients_active > delta ) {
        Com_DPrintf( "Disconnecting dummy MVD client.\n" );
        SV_DropClient( mvd.dummy, NULL );
        return;
    }

    delta = sv_mvd_suspend_time->value * 60 * 1000;

    // suspend/resume MVD streams depending on players activity
    if( !delta || players_active() ) {
        mvd.players_active = svs.realtime;
        if( !mvd.active ) {
            resume_streams();
        }
    } else if( mvd.active ) {
        if( svs.realtime - mvd.players_active > delta ) {
            suspend_streams();
        }
    }
}

/*
==================
SV_MvdEndFrame
==================
*/
void SV_MvdEndFrame( void ) {
    gtv_client_t *client;
    size_t total;
    byte header[3];

    // do nothing if not enabled
    if( !mvd.dummy ) {
        return;
    }

    dummy_run();

    // do nothing if not active
    if( !mvd.active ) {
        return;
    }

    // if reliable message overflowed, kick all clients
    if( mvd.message.overflowed ) {
        Com_EPrintf( "Reliable MVD message overflowed!\n" );
        SV_DropClient( mvd.dummy, NULL );
        return;
    }

    if( mvd.datagram.overflowed ) {
        Com_WPrintf( "Unreliable MVD datagram overflowed.\n" );
        SZ_Clear( &mvd.datagram );
    }

    // emit a delta update common to all clients
    emit_frame();

    // if reliable message and frame update don't fit, kick all clients
    if( mvd.message.cursize + msg_write.cursize >= MAX_MSGLEN ) {
        Com_EPrintf( "MVD frame overflowed!\n" );
        SZ_Clear( &msg_write );
        SV_DropClient( mvd.dummy, NULL );
        return;
    }

    // check if unreliable datagram fits
    if( mvd.message.cursize + msg_write.cursize + mvd.datagram.cursize >= MAX_MSGLEN ) {
        Com_WPrintf( "Dumping unreliable MVD datagram.\n" );
        SZ_Clear( &mvd.datagram );
    }

    // build message header
    total = mvd.message.cursize + msg_write.cursize + mvd.datagram.cursize + 1;
    header[0] = total & 255;
    header[1] = ( total >> 8 ) & 255;
    header[2] = GTS_STREAM_DATA;

    // send frame to clients
    FOR_EACH_ACTIVE_GTV( client ) {
        write_stream( client, header, sizeof( header ) );
        write_stream( client, mvd.message.data, mvd.message.cursize );
        write_stream( client, msg_write.data, msg_write.cursize );
        write_stream( client, mvd.datagram.data, mvd.datagram.cursize );
#if USE_ZLIB
        if( ++client->bufcount > client->maxbuf ) {
            flush_stream( client, Z_SYNC_FLUSH );
        }
#endif
    }

    // write frame to demofile
    if( mvd.recording ) {
        uint16_t msglen;

        msglen = LittleShort( total - 1 );
        FS_Write( &msglen, 2, mvd.recording );
        FS_Write( mvd.message.data, mvd.message.cursize, mvd.recording );
        FS_Write( msg_write.data, msg_write.cursize, mvd.recording );
        FS_Write( mvd.datagram.data, mvd.datagram.cursize, mvd.recording );

        if( sv_mvd_maxsize->value > 0 ) {
            int numbytes = FS_Tell( mvd.recording );

            if( numbytes > sv_mvd_maxsize->value * 1000 ) {
                Com_Printf( "Stopping MVD recording, maximum size reached.\n" );
                rec_stop();
            }
        } else if( sv_mvd_maxtime->value > 0 &&
            ++mvd.numframes > sv_mvd_maxtime->value * 600 )
        {
            Com_Printf( "Stopping MVD recording, maximum duration reached.\n" );
            rec_stop();
        }
    }

    // clear frame
    SZ_Clear( &msg_write );

    // clear datagrams
    SZ_Clear( &mvd.datagram );
    SZ_Clear( &mvd.message );
}



/*
==============================================================================

GAME API HOOKS

These hooks are called from PF_* functions to add additional
out-of-band data into the MVD stream.

==============================================================================
*/

/*
==============
SV_MvdMulticast

TODO: would be better to combine identical unicast/multicast messages
into one larger message to save space (useful for shotgun patterns
as they often occur in the same BSP leaf)
==============
*/
void SV_MvdMulticast( int leafnum, multicast_t to ) {
    mvd_ops_t   op;
    sizebuf_t   *buf;
    int         bits;
    
    // do nothing if not active
    if( !mvd.active ) {
        return;
    }

    op = mvd_multicast_all + to;
    buf = to < MULTICAST_ALL_R ? &mvd.datagram : &mvd.message;
    bits = ( msg_write.cursize >> 8 ) & 7;

    SZ_WriteByte( buf, op | ( bits << SVCMD_BITS ) );
    SZ_WriteByte( buf, msg_write.cursize & 255 );

    if( op != mvd_multicast_all && op != mvd_multicast_all_r ) {
        SZ_WriteShort( buf, leafnum );
    }
    
    SZ_Write( buf, msg_write.data, msg_write.cursize );
}

/*
==============
SV_MvdUnicast

Performs some basic filtering of the unicast data that would be
otherwise discarded by the MVD client.
==============
*/
void SV_MvdUnicast( edict_t *ent, int clientNum, qboolean reliable ) {
    mvd_ops_t   op;
    sizebuf_t   *buf;
    int         bits;

    // do nothing if not active
    if( !mvd.active ) {
        return;
    }

    // discard any data to players not in the game
    if( !player_is_active( ent ) ) {
        return;
    }

    switch( msg_write.data[0] ) {
    case svc_layout:
        if( ent == mvd.dummy->edict ) {
            // special case, send to all observers
            mvd.layout_time = svs.realtime;
        } else {
            // discard any layout updates to players
            return;
        }
        break;
    case svc_stufftext:
        if( memcmp( msg_write.data + 1, "play ", 5 ) ) {
            // discard any stufftexts, except of play sound hacks
            return;
        }
        break;
    case svc_print:
        if( ent != mvd.dummy->edict && sv_mvd_nomsgs->integer ) {
            // optionally discard text messages to players
            return;
        }
        break;
    }

    // decide where should it go
    if( reliable ) {
        op = mvd_unicast_r;
        buf = &mvd.message;
    } else {
        op = mvd_unicast;
        buf = &mvd.datagram;
    }

    // write it
    bits = ( msg_write.cursize >> 8 ) & 7;
    SZ_WriteByte( buf, op | ( bits << SVCMD_BITS ) );
    SZ_WriteByte( buf, msg_write.cursize & 255 );
    SZ_WriteByte( buf, clientNum );
    SZ_Write( buf, msg_write.data, msg_write.cursize );
}

/*
==============
SV_MvdConfigstring
==============
*/
void SV_MvdConfigstring( int index, const char *string ) {
    if( mvd.active ) {
        SZ_WriteByte( &mvd.message, mvd_configstring );
        SZ_WriteShort( &mvd.message, index );
        SZ_WriteString( &mvd.message, string );
    }
}

/*
==============
SV_MvdBroadcastPrint
==============
*/
void SV_MvdBroadcastPrint( int level, const char *string ) {
    if( mvd.active ) {
        SZ_WriteByte( &mvd.message, mvd_print );
        SZ_WriteByte( &mvd.message, level );
        SZ_WriteString( &mvd.message, string );
    }
}

/*
==============
SV_MvdStartSound

FIXME: origin will be incorrect on entities not captured this frame
==============
*/
void SV_MvdStartSound( int entnum, int channel, int flags,
                        int soundindex, int volume,
                        int attenuation, int timeofs )
{
    int extrabits, sendchan;

    // do nothing if not active
    if( !mvd.active ) {
        return;
    }

    extrabits = 0;
    if( channel & CHAN_NO_PHS_ADD ) {
        extrabits |= 1 << SVCMD_BITS;
    }
    if( channel & CHAN_RELIABLE ) {
        // FIXME: write to mvd.message
        extrabits |= 2 << SVCMD_BITS;
    }

    SZ_WriteByte( &mvd.datagram, mvd_sound | extrabits );
    SZ_WriteByte( &mvd.datagram, flags );
    SZ_WriteByte( &mvd.datagram, soundindex );

    if( flags & SND_VOLUME )
        SZ_WriteByte( &mvd.datagram, volume );
    if( flags & SND_ATTENUATION )
        SZ_WriteByte( &mvd.datagram, attenuation );
    if( flags & SND_OFFSET )
        SZ_WriteByte( &mvd.datagram, timeofs );

    sendchan = ( entnum << 3 ) | ( channel & 7 );
    SZ_WriteShort( &mvd.datagram, sendchan );
}


/*
==============================================================================

TCP CLIENTS HANDLING

==============================================================================
*/


static void remove_client( gtv_client_t *client ) {
    NET_Close( &client->stream );
    List_Remove( &client->entry );
    if( client->data ) {
        Z_Free( client->data );
        client->data = NULL;
    }
    client->state = cs_free;
}

#if USE_ZLIB
static void flush_stream( gtv_client_t *client, int flush ) {
    fifo_t *fifo = &client->stream.send;
    z_streamp z = &client->z;
    byte *data;
    size_t len;
    int ret;

    if( client->state <= cs_zombie ) {
        return;
    }
    if( !z->state ) {
        return;
    }

    z->next_in = NULL;
    z->avail_in = 0;

    do {
        data = FIFO_Reserve( fifo, &len );
        if( !len ) {
            // FIXME: this is not an error when flushing
            return;
        }

        z->next_out = data;
        z->avail_out = ( uInt )len;

        ret = deflate( z, flush );

        len -= z->avail_out;
        if( len ) {
            FIFO_Commit( fifo, len );
            client->bufcount = 0;
        }
    } while( ret == Z_OK );
}
#endif

static void drop_client( gtv_client_t *client, const char *error ) {
    if( client->state <= cs_zombie ) {
        return;
    }

    if( error ) {
        // notify console
        Com_Printf( "TCP client %s[%s] dropped: %s\n", client->name,
            NET_AdrToString( &client->stream.address ), error );
    }

#if USE_ZLIB
    if( client->z.state ) {
        // finish zlib stream
        flush_stream( client, Z_FINISH );
        deflateEnd( &client->z );
    }
#endif

    List_Remove( &client->active );
    client->state = cs_zombie;
    client->lastmessage = svs.realtime;
}


static void write_stream( gtv_client_t *client, void *data, size_t len ) {
    fifo_t *fifo = &client->stream.send;

    if( client->state <= cs_zombie ) {
        return;
    }

    if( !len ) {
        return;
    }

#if USE_ZLIB
    if( client->z.state ) {
        z_streamp z = &client->z;

        z->next_in = data;
        z->avail_in = ( uInt )len;

        do {
            data = FIFO_Reserve( fifo, &len );
            if( !len ) {
                drop_client( client, "overflowed" );
                return;
            }

            z->next_out = data;
            z->avail_out = ( uInt )len;

            if( deflate( z, Z_NO_FLUSH ) != Z_OK ) {
                drop_client( client, "deflate() failed" );
                return;
            }

            len -= z->avail_out;
            if( len ) {
                FIFO_Commit( fifo, len );
                client->bufcount = 0;
            }
        } while( z->avail_in );
    } else
#endif

    if( FIFO_Write( fifo, data, len ) != len ) {
        drop_client( client, "overflowed" );
    }
}

static void write_message( gtv_client_t *client, gtv_serverop_t op ) {
    byte header[3];
    size_t len = msg_write.cursize + 1;

    header[0] = len & 255;
    header[1] = ( len >> 8 ) & 255;
    header[2] = op;
    write_stream( client, header, sizeof( header ) );

    write_stream( client, msg_write.data, msg_write.cursize );
}

static qboolean auth_client( gtv_client_t *client, const char *password ) {
    if( SV_MatchAddress( &gtv_host_list, &client->stream.address ) ) {
        return qtrue; // dedicated GTV hosts don't need password
    }
    if( !sv_mvd_password->string[0] ) {
        return qfalse; // no password set on the server
    }
    if( strcmp( sv_mvd_password->string, password ) ) {
        return qfalse; // password doesn't match
    }
    return qtrue;
}

static void parse_hello( gtv_client_t *client ) {
    char password[MAX_QPATH];
    int protocol, flags;
    size_t size;
    byte *data;

    if( client->state >= cs_primed ) {
        drop_client( client, "duplicated hello message" );
        return;
    }

    // client should have already consumed the magic
    if( FIFO_Usage( &client->stream.send ) ) {
        drop_client( client, "send buffer not empty" );
        return;
    }

    protocol = MSG_ReadWord();
    if( protocol != GTV_PROTOCOL_VERSION ) {
        write_message( client, GTS_BADREQUEST );
        drop_client( client, "bad protocol version" );
        return;
    }

    flags = MSG_ReadLong();
    MSG_ReadLong();
    MSG_ReadString( client->name, sizeof( client->name ) );
    MSG_ReadString( password, sizeof( password ) );
    MSG_ReadString( client->version, sizeof( client->version ) );

    // authorize access
    if( !auth_client( client, password ) ) {
        write_message( client, GTS_NOACCESS );
        drop_client( client, "not authorized" );
        return;
    }

    if( sv_mvd_allow_stufftext->integer >= 0 ) {
        flags &= ~GTF_STRINGCMDS;
    }

#if !USE_ZLIB
    flags &= ~GTF_DEFLATE;
#endif

    Cvar_ClampInteger( sv_mvd_bufsize, 1, 4 );

    // allocate larger send buffer
    size = MAX_GTS_MSGLEN * sv_mvd_bufsize->integer;
    data = SV_Malloc( size );
    client->stream.send.data = data;
    client->stream.send.size = size;
    client->data = data;
    client->flags = flags;
    client->state = cs_primed;

    // send hello
    MSG_WriteLong( flags );
    write_message( client, GTS_HELLO );
    SZ_Clear( &msg_write );

#if USE_ZLIB
    // the rest of the stream will be deflated
    if( flags & GTF_DEFLATE ) {
        client->z.zalloc = SV_Zalloc;
        client->z.zfree = SV_Zfree;
        if( deflateInit( &client->z, Z_DEFAULT_COMPRESSION ) != Z_OK ) {
            drop_client( client, "deflateInit failed" );
            return;
        }
    }
#endif

    Com_Printf( "Accepted MVD client %s[%s]\n", client->name,
        NET_AdrToString( &client->stream.address ) );
}

static void parse_ping( gtv_client_t *client ) {
    if( client->state < cs_primed ) {
        return;
    }

    // send ping reply
    write_message( client, GTS_PONG );

#if USE_ZLIB
    flush_stream( client, Z_SYNC_FLUSH );
#endif
}

static void parse_stream_start( gtv_client_t *client ) {
    int maxbuf;

    if( client->state != cs_primed ) {
        drop_client( client, "unexpected stream start message" );
        return;
    }

    if( !mvd_enable() ) {
        write_message( client, GTS_ERROR );
        drop_client( client, "couldn't create MVD dummy" );
        return;
    }

    maxbuf = MSG_ReadShort();
    if( maxbuf < 10 ) {
        maxbuf = 10;
    }

    client->maxbuf = maxbuf;
    client->state = cs_spawned;

    List_Append( &gtv_active_list, &client->active );

    // send ack to client
    write_message( client, GTS_STREAM_START );

    // send gamestate if active
    if( mvd.active ) {
        emit_gamestate();
        write_message( client, GTS_STREAM_DATA );
        SZ_Clear( &msg_write );
    } else {
        // send stream suspend marker
        write_message( client, GTS_STREAM_DATA );
    }

#if USE_ZLIB
    flush_stream( client, Z_SYNC_FLUSH );
#endif
}

static void parse_stream_stop( gtv_client_t *client ) {
    if( client->state != cs_spawned ) {
        drop_client( client, "unexpected stream stop message" );
        return;
    }

    client->state = cs_primed;

    List_Delete( &client->active );

    // send ack to client
    write_message( client, GTS_STREAM_STOP );
#if USE_ZLIB
    flush_stream( client, Z_SYNC_FLUSH );
#endif
}

static void parse_stringcmd( gtv_client_t *client ) {
    char string[MAX_GTC_MSGLEN];

    if( client->state < cs_primed ) {
        drop_client( client, "unexpected stringcmd message" );
        return;
    }

    if( !mvd.dummy || !( client->flags & GTF_STRINGCMDS ) ) {
        Com_DPrintf( "ignored stringcmd from %s[%s]\n", client->name,
            NET_AdrToString( &client->stream.address ) );
        return;
    }

    MSG_ReadString( string, sizeof( string ) );

    Cmd_TokenizeString( string, qfalse );

    Com_DPrintf( "dummy stringcmd from %s[%s]: %s\n", client->name,
        NET_AdrToString( &client->stream.address ), string );
    dummy_command();
}

static qboolean parse_message( gtv_client_t *client ) {
    uint32_t magic;
    uint16_t msglen;
    int cmd;

    if( client->state <= cs_zombie ) {
        return qfalse;
    }

    // check magic
    if( client->state < cs_connected ) {
        if( !FIFO_TryRead( &client->stream.recv, &magic, 4 ) ) {
            return qfalse;
        }
        if( magic != MVD_MAGIC ) {
            drop_client( client, "not a MVD/GTV stream" );
            return qfalse;
        }
        client->state = cs_connected;

        // send it back
        write_stream( client, &magic, 4 );
        return qfalse;
    }

    // parse msglen
    if( !client->msglen ) {
        if( !FIFO_TryRead( &client->stream.recv, &msglen, 2 ) ) {
            return qfalse;
        }
        msglen = LittleShort( msglen );
        if( !msglen ) {
            drop_client( client, "end of stream" );
            return qfalse;
        }
        if( msglen > MAX_GTC_MSGLEN ) {
            drop_client( client, "oversize message" );
            return qfalse;
        }
        client->msglen = msglen;
    }

    // read this message
    if( !FIFO_ReadMessage( &client->stream.recv, client->msglen ) ) {
        return qfalse;
    }

    client->msglen = 0;

    cmd = MSG_ReadByte();
    switch( cmd ) {
    case GTC_HELLO:
        parse_hello( client );
        break;
    case GTC_PING:
        parse_ping( client );
        break;
    case GTC_STREAM_START:
        parse_stream_start( client );
        break;
    case GTC_STREAM_STOP:
        parse_stream_stop( client );
        break;
    case GTC_STRINGCMD:
        parse_stringcmd( client );
        break;
    default:
        drop_client( client, "unknown command byte" );
        return qfalse;
    }

    if( msg_read.readcount > msg_read.cursize ) {
        drop_client( client, "read past end of message" );
        return qfalse;
    }
    
    client->lastmessage = svs.realtime; // don't timeout
    return qtrue;
}

static gtv_client_t *find_slot( void ) {
    gtv_client_t *client;
    int i;

    for( i = 0; i < sv_mvd_maxclients->integer; i++ ) {
        client = &mvd.clients[i];
        if( !client->state ) {
            return client;
        }
    }
    return NULL;
}

static void accept_client( netstream_t *stream ) {
    gtv_client_t *client;
    netstream_t *s;

    // limit number of connections from single IP
    if( sv_iplimit->integer > 0 ) {
        int count = 0;

        FOR_EACH_GTV( client ) {
            if( NET_IsEqualBaseAdr( &client->stream.address, &stream->address ) ) {
                count++;
            }
        }
        if( count >= sv_iplimit->integer ) {
            Com_Printf( "TCP client [%s] rejected: too many connections\n",
                NET_AdrToString( &stream->address ) );
            NET_Close( stream );
            return;
        }
    }

    // find a free client slot
    client = find_slot();
    if( !client ) {
        Com_Printf( "TCP client [%s] rejected: no free slots\n",
            NET_AdrToString( &stream->address ) );
        NET_Close( stream );
        return;
    }

    memset( client, 0, sizeof( *client ) );

    s = &client->stream;
    s->recv.data = client->buffer;
    s->recv.size = MAX_GTC_MSGLEN;
    s->send.data = client->buffer + MAX_GTC_MSGLEN;
    s->send.size = 4; // need no more than that initially
    s->socket = stream->socket;
    s->address = stream->address;
    s->state = stream->state;

    client->lastmessage = svs.realtime;
    client->state = cs_assigned;
    List_SeqAdd( &gtv_client_list, &client->entry );
    List_Init( &client->active );

    Com_DPrintf( "TCP client [%s] accepted\n",
        NET_AdrToString( &stream->address ) );
}

void SV_MvdRunClients( void ) {
    gtv_client_t *client;
    neterr_t    ret;
    netstream_t stream;
    unsigned    zombie_time = 1000 * sv_zombietime->value;
    unsigned    drop_time   = 1000 * sv_timeout->value;
    unsigned    ghost_time  = 1000 * sv_ghostime->value;
    unsigned    delta;

    // accept new connections
    ret = NET_Accept( &stream );
    if( ret == NET_ERROR ) {
        Com_DPrintf( "%s from %s, ignored\n", NET_ErrorString(),
            NET_AdrToString( &net_from ) );
    } else if( ret == NET_OK ) {
        accept_client( &stream );
    }

    // run existing connections
    FOR_EACH_GTV( client ) {
        // check timeouts
        delta = svs.realtime - client->lastmessage;
        switch( client->state ) {
        case cs_zombie:
            if( delta > zombie_time || !FIFO_Usage( &client->stream.send ) ) {
                remove_client( client );
                continue;
            }
            break;
        case cs_assigned:
        case cs_connected:
            if( delta > ghost_time || delta > drop_time ) {
                drop_client( client, "request timed out" );
                remove_client( client );
                continue;
            }
            break;
        default:
            if( delta > drop_time ) {
                drop_client( client, "connection timed out" );
                remove_client( client );
                continue;
            }
            break;
        }

        // run network stream
        ret = NET_RunStream( &client->stream );
        switch( ret ) {
        case NET_AGAIN:
            break;
        case NET_OK:
            // parse the message
            while( parse_message( client ) )
                ;
            break;
        case NET_CLOSED:
            drop_client( client, "EOF from client" );
            remove_client( client );
            break;
        case NET_ERROR:
            drop_client( client, "connection reset by peer" );
            remove_client( client );
            break;
        }
    }
}

static void dump_clients( void ) {
    gtv_client_t    *client;
    int count;

    Com_Printf(
"num name             buf lastmsg address               state\n"
"--- ---------------- --- ------- --------------------- -----\n" );
    count = 0;
    FOR_EACH_GTV( client ) {
        Com_Printf( "%3d %-16.16s %3"PRIz" %7u %-21s ",
            count, client->name, FIFO_Usage( &client->stream.send ),
            svs.realtime - client->lastmessage,
            NET_AdrToString( &client->stream.address ) );

        switch( client->state ) {
        case cs_zombie:
            Com_Printf( "ZMBI " );
            break;
        case cs_assigned:
            Com_Printf( "ASGN " );
            break;
        case cs_connected:
            Com_Printf( "CNCT " );
            break;
        case cs_primed:
            Com_Printf( "PRIM " );
            break;
        default:
            Com_Printf( "SEND " );
            break;
        }
        Com_Printf( "\n" );

        count++;
    }
}

static void dump_versions( void ) {
    gtv_client_t *client;
    int count;

    Com_Printf(
"num name             version\n"
"--- ---------------- -----------------------------------------\n" );

    FOR_EACH_GTV( client ) {
    count = 0;
        Com_Printf( "%3i %-16.16s %-40.40s\n",
            count, client->name, client->version );
        count++;
    }
}

void SV_MvdStatus_f( void ) {
    if( LIST_EMPTY( &gtv_client_list ) ) {
        Com_Printf( "No TCP clients.\n" );
    } else {
        if( Cmd_Argc() > 1 ) {
            dump_versions();
        } else {
            dump_clients();
        }
    }
    Com_Printf( "\n" );
}

static void mvd_disable( void ) {
    // remove MVD dummy
    if( mvd.dummy ) {
        SV_RemoveClient( mvd.dummy );
        mvd.dummy = NULL;
    }

    SZ_Clear( &mvd.datagram );
    SZ_Clear( &mvd.message );

    mvd.active = qfalse;
}

// something bad happened, remove all clients
static void mvd_drop( gtv_serverop_t op ) {
    gtv_client_t *client;

    // stop recording
    rec_stop();

    // drop GTV clients
    FOR_EACH_GTV( client ) {
        switch( client->state ) {
        case cs_spawned:
        case cs_primed:
            write_message( client, op );
            drop_client( client, NULL );
            NET_RunStream( &client->stream );
            NET_RunStream( &client->stream );
            remove_client( client );
            break;
        default:
            drop_client( client, NULL );
            remove_client( client );
            break;
        }
    }

    mvd_disable();
}

// if dummy is not yet connected, create and spawn it
static qboolean mvd_enable( void ) {
    if( !mvd.dummy ) {
        if( !dummy_create() ) {
            return qfalse;
        }
        dummy_spawn();

        // check for activation
        SV_MvdBeginFrame();
    }
    return qtrue;
}


/*
==============================================================================

SERVER HOOKS

These hooks are called by server code when some event occurs.

==============================================================================
*/

/*
==================
SV_MvdMapChanged

Server has just changed the map, spawn the MVD dummy and go!
==================
*/
void SV_MvdMapChanged( void ) {
    gtv_client_t *client;

    if( !sv_mvd_enable->integer ) {
        return; // do noting if disabled
    }

    if( !mvd.dummy ) {
        if( !sv_mvd_autorecord->integer ) {
            return; // not listening for autorecord command
        }
        if( !dummy_create() ) {
            return;
        }
        Com_Printf( "Spawning MVD dummy for auto-recording\n" );
        Cvar_Set( "sv_mvd_suspend_time", "0" );
    }

    dummy_spawn();

    if( mvd.active ) {
        // build and emit gamestate
        build_gamestate();
        emit_gamestate();

        // send gamestate to all MVD clients
        FOR_EACH_ACTIVE_GTV( client ) {
            write_message( client, GTS_STREAM_DATA );
        }
    }

    if( mvd.recording ) {
        int maxlevels = sv_mvd_maxmaps->integer;
    
        // check if it is time to stop recording
        if( maxlevels > 0 && ++mvd.numlevels >= maxlevels ) {
            Com_Printf( "Stopping MVD recording, "
                "maximum number of level changes reached.\n" );
            rec_stop();
        } else if( mvd.active ) {
            // write gamestate to demofile
            rec_write();
        }
    }

    // clear gamestate
    SZ_Clear( &msg_write );

    SZ_Clear( &mvd.datagram );
    SZ_Clear( &mvd.message );
}

/*
==================
SV_MvdClientDropped

Server has just dropped a client, check if that was our MVD dummy client.
==================
*/
void SV_MvdClientDropped( client_t *client, const char *reason ) {
    if( client == mvd.dummy ) {
        mvd_drop( GTS_ERROR );
    }
}

/*
==================
SV_MvdInit

Server is initializing, prepare MVD server for this game.
==================
*/
void SV_MvdInit( void ) {
    if( !sv_mvd_enable->integer ) {
        return; // do nothing if disabled
    }

    // allocate buffers
    Z_TagReserve( sizeof( player_state_t ) * sv_maxclients->integer +
        sizeof( entity_state_t ) * MAX_EDICTS + MAX_MSGLEN * 2, TAG_SERVER );
    SZ_Init( &mvd.message, Z_ReservedAlloc( MAX_MSGLEN ), MAX_MSGLEN );
    SZ_Init( &mvd.datagram, Z_ReservedAlloc( MAX_MSGLEN ), MAX_MSGLEN );
    mvd.players = Z_ReservedAlloc( sizeof( player_state_t ) * sv_maxclients->integer );
    mvd.entities = Z_ReservedAlloc( sizeof( entity_state_t ) * MAX_EDICTS );

    // reserve the slot for dummy MVD client
    if( !sv_reserved_slots->integer ) {
        Cvar_Set( "sv_reserved_slots", "1" );
    }

    Cvar_ClampInteger( sv_mvd_maxclients, 1, 256 );

    // open server TCP socket
    if( sv_mvd_enable->integer > 1 ) {
        if( NET_Listen( qtrue ) == NET_OK ) {
            mvd.clients = SV_Mallocz( sizeof( gtv_client_t ) * sv_mvd_maxclients->integer );
        } else {
            Com_EPrintf( "%s while opening server TCP port.\n", NET_ErrorString() );
            Cvar_Set( "sv_mvd_enable", "1" );
        }
    }

    dummy_buffer.text = dummy_buffer_text;
    dummy_buffer.maxsize = sizeof( dummy_buffer_text );
    dummy_buffer.exec = dummy_exec_string;
}

/*
==================
SV_MvdShutdown

Server is shutting down, clean everything up.
==================
*/
void SV_MvdShutdown( killtype_t type ) {
    // drop all clients
    mvd_drop( type == KILL_RESTART ? GTS_RECONNECT : GTS_DISCONNECT );

    // free static data
    Z_Free( mvd.message.data );
    Z_Free( mvd.clients );

    // close server TCP socket
    NET_Listen( qfalse );

    memset( &mvd, 0, sizeof( mvd ) );

    memset( &dummy_buffer, 0, sizeof( dummy_buffer ) );
}


/*
==============================================================================

LOCAL MVD RECORDER

==============================================================================
*/

static void rec_write( void ) {
    uint16_t msglen;

    msglen = LittleShort( msg_write.cursize );
    FS_Write( &msglen, 2, mvd.recording );
    FS_Write( msg_write.data, msg_write.cursize, mvd.recording );
}

/*
==============
rec_stop

Stops server local MVD recording.
==============
*/
static void rec_stop( void ) {
    uint16_t msglen;

    if( !mvd.recording ) {
        return;
    }
    
    // write demo EOF marker
    msglen = 0;
    FS_Write( &msglen, 2, mvd.recording );

    FS_FCloseFile( mvd.recording );
    mvd.recording = 0;
}

static qboolean rec_allowed( void ) {
    if( !mvd.entities ) {
        Com_Printf( "MVD recording is disabled on this server.\n" );
        return qfalse;
    }
    if( mvd.recording ) {
        Com_Printf( "Already recording a local MVD.\n" );
        return qfalse;
    }
    return qtrue;
}

static void rec_start( fileHandle_t demofile ) {
    uint32_t magic;

    mvd.recording = demofile;
    mvd.numlevels = 0;
    mvd.numframes = 0;
    mvd.clients_active = svs.realtime;
    
    magic = MVD_MAGIC;
    FS_Write( &magic, 4, demofile );

    if( mvd.active ) {
        emit_gamestate();
        rec_write();
        SZ_Clear( &msg_write );
    }
}


static void SV_MvdRecord_c( genctx_t *ctx, int argnum ) {
#if USE_MVD_CLIENT
    // TODO
    if( argnum == 1 ) {
        MVD_File_g( ctx );
    }
#endif
}

/*
==============
MVD_Record_f

Begins server MVD recording.
Every entity, every playerinfo and every message will be recorded.
==============
*/
static void SV_MvdRecord_f( void ) {
    char buffer[MAX_OSPATH];
    fileHandle_t demofile;
    qboolean gzip = qfalse;
    int c;
    size_t len;

    if( sv.state != ss_game ) {
#if USE_MVD_CLIENT
        if( sv.state == ss_broadcast ) {
            MVD_StreamedRecord_f();
        } else
#endif
        {
            Com_Printf( "No server running.\n" );
        }
        return;
    }

    while( ( c = Cmd_ParseOptions( o_record ) ) != -1 ) {
        switch( c ) {
        case 'h':
            Cmd_PrintUsage( o_record, "[/]<filename>" );
            Com_Printf( "Begin local MVD recording.\n" );
            Cmd_PrintHelp( o_record );
            return;
        case 'z':
            gzip = qtrue;
            break;
        }
    }

    if( !cmd_optarg[0] ) {
        Com_Printf( "Missing filename argument.\n" );
        Cmd_PrintHint();
        return;
    }

    if( !rec_allowed() ) {
        return;
    }

    //
    // open the demo file
    //
    len = Q_concat( buffer, sizeof( buffer ), "demos/", cmd_optarg,
        gzip ? ".mvd2.gz" : ".mvd2", NULL );
    if( len >= sizeof( buffer ) ) {
        Com_EPrintf( "Oversize filename specified.\n" );
        return;
    }

    FS_FOpenFile( buffer, &demofile, FS_MODE_WRITE );
    if( !demofile ) {
        Com_EPrintf( "Couldn't open %s for writing\n", buffer );
        return;
    }

    if( !mvd_enable() ) {
        FS_FCloseFile( demofile );
        return;
    }

    if( gzip ) {
        FS_FilterFile( demofile );
    }

    rec_start( demofile );

    Com_Printf( "Recording local MVD to %s\n", buffer );
}


/*
==============
MVD_Stop_f

Ends server MVD recording
==============
*/
static void SV_MvdStop_f( void ) {
#if USE_MVD_CLIENT
    if( sv.state == ss_broadcast ) {
        MVD_StreamedStop_f();
        return;
    }
#endif
    if( !mvd.recording ) {
        Com_Printf( "Not recording a local MVD.\n" );
        return;
    }

    Com_Printf( "Stopped local MVD recording.\n" );
    rec_stop();
}

static void SV_MvdStuff_f( void ) {
    if( mvd.dummy ) {
        Cbuf_AddTextEx( &dummy_buffer, Cmd_RawArgs() );
        Cbuf_AddTextEx( &dummy_buffer, "\n" );
    } else {
        Com_Printf( "Can't '%s', dummy MVD client is not active\n", Cmd_Argv( 0 ) );
    }
}

static void SV_AddGtvHost_f( void ) {
    SV_AddMatch_f( &gtv_host_list );
}
static void SV_DelGtvHost_f( void ) {
    SV_DelMatch_f( &gtv_host_list );
}
static void SV_ListGtvHosts_f( void ) {
    SV_ListMatches_f( &gtv_host_list );
}

static const cmdreg_t c_svmvd[] = {
    { "mvdrecord", SV_MvdRecord_f, SV_MvdRecord_c },
    { "mvdstop", SV_MvdStop_f },
    { "mvdstuff", SV_MvdStuff_f },
    { "addgtvhost", SV_AddGtvHost_f },
    { "delgtvhost", SV_DelGtvHost_f },
    { "listgtvhosts", SV_ListGtvHosts_f },

    { NULL }
};

void SV_MvdRegister( void ) {
    sv_mvd_enable = Cvar_Get( "sv_mvd_enable", "0", CVAR_LATCH );
    sv_mvd_maxclients = Cvar_Get( "sv_mvd_maxclients", "8", CVAR_LATCH );
    sv_mvd_bufsize = Cvar_Get( "sv_mvd_bufsize", "2", CVAR_LATCH );
    sv_mvd_password = Cvar_Get( "sv_mvd_password", "", CVAR_PRIVATE );
    sv_mvd_maxsize = Cvar_Get( "sv_mvd_maxsize", "0", 0 );
    sv_mvd_maxtime = Cvar_Get( "sv_mvd_maxtime", "0", 0 );
    sv_mvd_maxmaps = Cvar_Get( "sv_mvd_maxmaps", "1", 0 );
    sv_mvd_noblend = Cvar_Get( "sv_mvd_noblend", "0", CVAR_LATCH );
    sv_mvd_nogun = Cvar_Get( "sv_mvd_nogun", "1", CVAR_LATCH );
    sv_mvd_nomsgs = Cvar_Get( "sv_mvd_nomsgs", "1", CVAR_LATCH );
    sv_mvd_begincmd = Cvar_Get( "sv_mvd_begincmd",
        "wait 50; putaway; wait 10; help;", 0 );
    sv_mvd_scorecmd = Cvar_Get( "sv_mvd_scorecmd",
        "putaway; wait 10; help;", 0 );
    sv_mvd_autorecord = Cvar_Get( "sv_mvd_autorecord", "0", CVAR_LATCH );
    sv_mvd_capture_flags = Cvar_Get( "sv_mvd_capture_flags", "5", 0 );
    sv_mvd_disconnect_time = Cvar_Get( "sv_mvd_disconnect_time", "15", 0 );
    sv_mvd_suspend_time = Cvar_Get( "sv_mvd_suspend_time", "5", 0 );
    sv_mvd_allow_stufftext = Cvar_Get( "sv_mvd_allow_stufftext", "0", CVAR_LATCH );

    Cmd_Register( c_svmvd );
}

