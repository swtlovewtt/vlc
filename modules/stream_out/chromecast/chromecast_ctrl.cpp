/*****************************************************************************
 * chromecast_ctrl.cpp: Chromecast module for vlc
 *****************************************************************************
 * Copyright © 2014-2015 VideoLAN
 *
 * Authors: Adrien Maglo <magsoft@videolan.org>
 *          Jean-Baptiste Kempf <jb@videolan.org>
 *          Steve Lhomme <robux4@videolabs.io>
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation; either version 2.1 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this program; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin Street, Fifth Floor, Boston MA 02110-1301, USA.
 *****************************************************************************/

/*****************************************************************************
 * Preamble
 *****************************************************************************/

#ifdef HAVE_CONFIG_H
# include "config.h"
#endif

#include "chromecast.h"

#include <cassert>
#include <cerrno>

#include "../../misc/webservices/json.h"

/* deadline regarding pings sent from receiver */
#define PING_WAIT_TIME 6000
#define PING_WAIT_RETRIES 0

static const mtime_t SEEK_FORWARD_OFFSET = 1000000;

/*****************************************************************************
 * intf_sys_t: class definition
 *****************************************************************************/
intf_sys_t::intf_sys_t(vlc_object_t * const p_this, int port, std::string device_addr, int device_port, vlc_interrupt_t *p_interrupt)
 : p_module(p_this)
 , i_port(port)
 , i_target_port(device_port)
 , targetIP(device_addr)
 , receiverState(RECEIVER_IDLE)
 , m_communication( p_this )
 , requested_stop(false)
 , requested_seek(false)
 , conn_status(CHROMECAST_DISCONNECTED)
 , cmd_status(NO_CMD_PENDING)
 , has_input(false)
 , p_ctl_thread_interrupt(p_interrupt)
 , m_time_playback_started( VLC_TS_INVALID )
 , i_ts_local_start( VLC_TS_INVALID )
 , i_length( VLC_TS_INVALID )
 , m_chromecast_start_time( VLC_TS_INVALID )
 , m_seek_request_time( VLC_TS_INVALID )
{
    vlc_mutex_init(&lock);
    vlc_cond_init(&loadCommandCond);
    vlc_cond_init(&seekCommandCond);

    common.p_opaque = this;
    common.pf_get_position     = get_position;
    common.pf_get_time         = get_time;
    common.pf_set_length       = set_length;
    common.pf_wait_app_started = wait_app_started;
    common.pf_request_seek     = request_seek;
    common.pf_wait_seek_done   = wait_seek_done;
    common.pf_set_pause_state  = set_pause_state;
    common.pf_set_artwork      = set_artwork;
    common.pf_set_title        = set_title;

    assert( var_Type( p_module->obj.parent->obj.parent, CC_SHARED_VAR_NAME) == 0 );
    if (var_Create( p_module->obj.parent->obj.parent, CC_SHARED_VAR_NAME, VLC_VAR_ADDRESS ) == VLC_SUCCESS )
        var_SetAddress( p_module->obj.parent->obj.parent, CC_SHARED_VAR_NAME, &common );

    // Start the Chromecast event thread.
    if (vlc_clone(&chromecastThread, ChromecastThread, this,
                  VLC_THREAD_PRIORITY_LOW))
    {
        msg_Err( p_module, "Could not start the Chromecast talking thread");
    }
}

intf_sys_t::~intf_sys_t()
{
    setHasInput( false );

    var_Destroy( p_module->obj.parent->obj.parent, CC_SHARED_VAR_NAME );

    switch ( conn_status )
    {
    case CHROMECAST_APP_STARTED:
        // Generate the close messages.
        m_communication.msgReceiverClose(appTransportId);
        // ft
    case CHROMECAST_TLS_CONNECTED:
    case CHROMECAST_AUTHENTICATED:
        m_communication.msgReceiverClose(DEFAULT_CHOMECAST_RECEIVER);
        // ft
    case CHROMECAST_DISCONNECTED:
    case CHROMECAST_CONNECTION_DEAD:
    default:
        break;
    }

    vlc_interrupt_kill( p_ctl_thread_interrupt );

    vlc_join(chromecastThread, NULL);

    vlc_interrupt_destroy( p_ctl_thread_interrupt );

    // make sure we unblock the demuxer
    m_seek_request_time = VLC_TS_INVALID;
    vlc_cond_signal(&seekCommandCond);

    vlc_cond_destroy(&seekCommandCond);
    vlc_cond_destroy(&loadCommandCond);
    vlc_mutex_destroy(&lock);
}

void intf_sys_t::setHasInput( bool b_has_input, const std::string mime_type )
{
    vlc_mutex_locker locker(&lock);
    msg_Dbg( p_module, "setHasInput %s device:%s session:%s",b_has_input ? "true":"false",
             targetIP.c_str(), mediaSessionId.c_str() );

    this->has_input = b_has_input;
    this->mime = mime_type;

    if( this->has_input )
    {
        mutex_cleanup_push(&lock);
        while (conn_status != CHROMECAST_APP_STARTED && conn_status != CHROMECAST_CONNECTION_DEAD)
        {
            msg_Dbg( p_module, "setHasInput waiting for Chromecast connection, current %d", conn_status);
            vlc_cond_wait(&loadCommandCond, &lock);
        }
        vlc_cleanup_pop();

        if (conn_status == CHROMECAST_CONNECTION_DEAD)
        {
            msg_Warn( p_module, "no Chromecast hook possible");
            return;
        }

        if ( receiverState == RECEIVER_IDLE )
        {
            // we cannot start a new load when the last one is still processing
            i_ts_local_start = VLC_TS_0;
            m_communication.msgPlayerLoad( appTransportId, i_port, title, artwork, mime_type );
            setPlayerStatus(CMD_LOAD_SENT);
        }
    }
}

/**
 * @brief Disconnect from the Chromecast
 */
void intf_sys_t::disconnectChromecast()
{
    m_communication.disconnect();
    setConnectionStatus(CHROMECAST_DISCONNECTED);
    appTransportId = "";
    mediaSessionId = ""; // this session is not valid anymore
    setPlayerStatus(NO_CMD_PENDING);
    receiverState = RECEIVER_IDLE;
}

/**
 * @brief Process a message received from the Chromecast
 * @param msg the CastMessage to process
 * @return 0 if the message has been successfuly processed else -1
 */
void intf_sys_t::processMessage(const castchannel::CastMessage &msg)
{
    const std::string & namespace_ = msg.namespace_();

#ifndef NDEBUG
    msg_Dbg( p_module, "processMessage: %s->%s %s", namespace_.c_str(), msg.destination_id().c_str(), msg.payload_utf8().c_str());
#endif

    if (namespace_ == NAMESPACE_DEVICEAUTH)
    {
        castchannel::DeviceAuthMessage authMessage;
        authMessage.ParseFromString(msg.payload_binary());

        if (authMessage.has_error())
        {
            msg_Err( p_module, "Authentification error: %d", authMessage.error().error_type());
        }
        else if (!authMessage.has_response())
        {
            msg_Err( p_module, "Authentification message has no response field");
        }
        else
        {
            vlc_mutex_locker locker(&lock);
            setConnectionStatus(CHROMECAST_AUTHENTICATED);
            m_communication.msgConnect(DEFAULT_CHOMECAST_RECEIVER);
            m_communication.msgReceiverGetStatus();
        }
    }
    else if (namespace_ == NAMESPACE_HEARTBEAT)
    {
        json_value *p_data = json_parse(msg.payload_utf8().c_str());
        std::string type((*p_data)["type"]);

        if (type == "PING")
        {
            msg_Dbg( p_module, "PING received from the Chromecast");
            m_communication.msgPong();
        }
        else if (type == "PONG")
        {
            msg_Dbg( p_module, "PONG received from the Chromecast");
        }
        else
        {
            msg_Warn( p_module, "Heartbeat command not supported: %s", type.c_str());
        }

        json_value_free(p_data);
    }
    else if (namespace_ == NAMESPACE_RECEIVER)
    {
        json_value *p_data = json_parse(msg.payload_utf8().c_str());
        std::string type((*p_data)["type"]);

        if (type == "RECEIVER_STATUS")
        {
            json_value applications = (*p_data)["status"]["applications"];
            const json_value *p_app = NULL;

            vlc_mutex_locker locker(&lock);
            for (unsigned i = 0; i < applications.u.array.length; ++i)
            {
                std::string appId(applications[i]["appId"]);
                if (appId == APP_ID)
                {
                    const char *pz_transportId = applications[i]["transportId"];
                    if (pz_transportId != NULL)
                    {
                        appTransportId = std::string(pz_transportId);
                        p_app = &applications[i];
                    }
                    break;
                }
            }

            if ( p_app )
            {
                if (!appTransportId.empty()
                        && conn_status == CHROMECAST_AUTHENTICATED)
                {
                    m_communication.msgConnect( appTransportId );
                    setPlayerStatus(NO_CMD_PENDING);
                    setConnectionStatus(CHROMECAST_APP_STARTED);
                }
            }
            else
            {
                switch( conn_status )
                {
                /* If the app is no longer present */
                case CHROMECAST_APP_STARTED:
                    msg_Warn( p_module, "app is no longer present. closing");
                    m_communication.msgReceiverClose(appTransportId);
                    setConnectionStatus(CHROMECAST_CONNECTION_DEAD);
                    break;

                case CHROMECAST_AUTHENTICATED:
                    msg_Dbg( p_module, "Chromecast was running no app, launch media_app");
                    appTransportId = "";
                    mediaSessionId = ""; // this session is not valid anymore
                    receiverState = RECEIVER_IDLE;
                    m_communication.msgReceiverLaunchApp();
                    break;

                default:
                    break;
                }

            }
        }
        else if (type == "LAUNCH_ERROR")
        {
            json_value reason = (*p_data)["reason"];
            msg_Err( p_module, "Failed to start the MediaPlayer: %s",
                    (const char *)reason);
        }
        else
        {
            msg_Warn( p_module, "Receiver command not supported: %s",
                    msg.payload_utf8().c_str());
        }

        json_value_free(p_data);
    }
    else if (namespace_ == NAMESPACE_MEDIA)
    {
        json_value *p_data = json_parse(msg.payload_utf8().c_str());
        std::string type((*p_data)["type"]);

        if (type == "MEDIA_STATUS")
        {
            json_value status = (*p_data)["status"];
            msg_Dbg( p_module, "Player state: %s sessionId:%d",
                    status[0]["playerState"].operator const char *(),
                    (int)(json_int_t) status[0]["mediaSessionId"]);

            vlc_mutex_locker locker(&lock);
            receiver_state oldPlayerState = receiverState;
            std::string newPlayerState = status[0]["playerState"].operator const char *();
            std::string idleReason = status[0]["idleReason"].operator const char *();

            if (newPlayerState == "IDLE")
                receiverState = RECEIVER_IDLE;
            else if (newPlayerState == "PLAYING")
                receiverState = RECEIVER_PLAYING;
            else if (newPlayerState == "BUFFERING")
                receiverState = RECEIVER_BUFFERING;
            else if (newPlayerState == "PAUSED")
                receiverState = RECEIVER_PAUSED;
            else if (!newPlayerState.empty())
                msg_Warn( p_module, "Unknown Chromecast state %s", newPlayerState.c_str());

            if (receiverState == RECEIVER_IDLE)
            {
                mediaSessionId = ""; // this session is not valid anymore
            }
            else
            {
                char session_id[32];
                if( snprintf( session_id, sizeof(session_id), "%" PRId64, (json_int_t) status[0]["mediaSessionId"] ) >= (int)sizeof(session_id) )
                {
                    msg_Err( p_module, "snprintf() truncated string for mediaSessionId" );
                    session_id[sizeof(session_id) - 1] = '\0';
                }
                if (session_id[0] && mediaSessionId != session_id) {
                    if (!mediaSessionId.empty())
                        msg_Warn( p_module, "different mediaSessionId detected %s was %s", session_id, this->mediaSessionId.c_str());
                    mediaSessionId = session_id;
                }
            }

            if (receiverState != oldPlayerState)
            {
#ifndef NDEBUG
                msg_Dbg( p_module, "change Chromecast player state from %d to %d", oldPlayerState, receiverState);
#endif
                switch( receiverState )
                {
                case RECEIVER_BUFFERING:
                    if ( double(status[0]["currentTime"]) == 0.0 )
                    {
                        receiverState = oldPlayerState;
                        msg_Dbg( p_module, "Invalid buffering time, keep previous state %d", oldPlayerState);
                    }
                    else
                    {
                        m_chromecast_start_time = (1 + mtime_t( double( status[0]["currentTime"] ) ) ) * 1000000L;
                        msg_Dbg( p_module, "Playback pending with an offset of %" PRId64, m_chromecast_start_time);
                    }
                    m_time_playback_started = VLC_TS_INVALID;
                    break;

                case RECEIVER_PLAYING:
                    /* TODO reset demux PCR ? */
                    if (unlikely(m_chromecast_start_time == VLC_TS_INVALID)) {
                        msg_Warn( p_module, "start playing without buffering" );
                        m_chromecast_start_time = (1 + mtime_t( double( status[0]["currentTime"] ) ) ) * 1000000L;
                    }
                    setPlayerStatus(CMD_PLAYBACK_SENT);
                    m_time_playback_started = mdate();
#ifndef NDEBUG
                    msg_Dbg( p_module, "Playback started with an offset of %" PRId64 " now:%" PRId64 " i_ts_local_start:%" PRId64, m_chromecast_start_time, m_time_playback_started, i_ts_local_start);
#endif
                    break;

                case RECEIVER_PAUSED:
                    m_chromecast_start_time = (1 + mtime_t( double( status[0]["currentTime"] ) ) ) * 1000000L;
#ifndef NDEBUG
                    msg_Dbg( p_module, "Playback paused with an offset of %" PRId64 " date_play_start:%" PRId64, m_chromecast_start_time, m_time_playback_started);
#endif

                    if ( m_time_playback_started != VLC_TS_INVALID && oldPlayerState == RECEIVER_PLAYING )
                    {
                        /* this is a pause generated remotely, adjust the playback time */
                        i_ts_local_start += mdate() - m_time_playback_started;
#ifndef NDEBUG
                        msg_Dbg( p_module, "updated i_ts_local_start:%" PRId64, i_ts_local_start);
#endif
                    }
                    m_time_playback_started = VLC_TS_INVALID;
                    break;

                case RECEIVER_IDLE:
                    if ( has_input )
                        setPlayerStatus(NO_CMD_PENDING);
                    m_time_playback_started = VLC_TS_INVALID;
                    break;
                }
            }

            if (receiverState == RECEIVER_BUFFERING && m_seek_request_time != VLC_TS_INVALID)
            {
                msg_Dbg( p_module, "Chromecast seeking possibly done");
                vlc_cond_signal( &seekCommandCond );
            }

            if ( (cmd_status != CMD_LOAD_SENT || idleReason == "CANCELLED") && receiverState == RECEIVER_IDLE && has_input )
            {
                msg_Dbg( p_module, "the device missed the LOAD command");
                i_ts_local_start = VLC_TS_0;
                m_communication.msgPlayerLoad( appTransportId, i_port, title, artwork, mime );
                setPlayerStatus(CMD_LOAD_SENT);
            }
        }
        else if (type == "LOAD_FAILED")
        {
            msg_Err( p_module, "Media load failed");
            vlc_mutex_locker locker(&lock);
            /* close the app to restart it */
            if ( conn_status == CHROMECAST_APP_STARTED )
                m_communication.msgReceiverClose(appTransportId);
            else
                m_communication.msgReceiverGetStatus();
        }
        else if (type == "LOAD_CANCELLED")
        {
            msg_Dbg( p_module, "LOAD canceled by another command");
        }
        else if (type == "INVALID_REQUEST")
        {
            msg_Dbg( p_module, "We sent an invalid request reason:%s", (*p_data)["reason"].operator const char *());
        }
        else
        {
            msg_Warn( p_module, "Media command not supported: %s",
                    msg.payload_utf8().c_str());
        }

        json_value_free(p_data);
    }
    else if (namespace_ == NAMESPACE_CONNECTION)
    {
        json_value *p_data = json_parse(msg.payload_utf8().c_str());
        std::string type((*p_data)["type"]);
        json_value_free(p_data);

        if (type == "CLOSE")
        {
            msg_Warn( p_module, "received close message");
            setHasInput( false );
            vlc_mutex_locker locker(&lock);
            setConnectionStatus(CHROMECAST_CONNECTION_DEAD);
            // make sure we unblock the demuxer
            m_seek_request_time = VLC_TS_INVALID;
            vlc_cond_signal(&seekCommandCond);
        }
        else
        {
            msg_Warn( p_module, "Connection command not supported: %s",
                    type.c_str());
        }
    }
    else
    {
        msg_Err( p_module, "Unknown namespace: %s", msg.namespace_().c_str());
    }
}



/*****************************************************************************
 * Chromecast thread
 *****************************************************************************/
void* intf_sys_t::ChromecastThread(void* p_data)
{
    intf_sys_t *p_sys = reinterpret_cast<intf_sys_t*>(p_data);
    p_sys->setConnectionStatus( CHROMECAST_DISCONNECTED );

    if ( p_sys->m_communication.connect( p_sys->targetIP.c_str(), p_sys->i_target_port ) == false )
    {
        msg_Err( p_sys->p_module, "Could not connect the Chromecast" );
        vlc_mutex_locker locker(&p_sys->lock);
        p_sys->setConnectionStatus(CHROMECAST_CONNECTION_DEAD);
        return NULL;
    }

    vlc_interrupt_set( p_sys->p_ctl_thread_interrupt );

    vlc_mutex_lock(&p_sys->lock);
    p_sys->setConnectionStatus(CHROMECAST_TLS_CONNECTED);
    vlc_mutex_unlock(&p_sys->lock);

    p_sys->m_communication.msgAuth();

    while ( !vlc_killed() && p_sys->handleMessages() );

    return NULL;
}

bool intf_sys_t::handleMessages()
{
    unsigned i_received = 0;
    uint8_t p_packet[PACKET_MAX_LEN];
    bool b_pingTimeout = false;

    int i_waitdelay = PING_WAIT_TIME;
    int i_retries = PING_WAIT_RETRIES;

    bool b_msgReceived = false;
    uint32_t i_payloadSize = 0;

    if ( requested_stop.exchange(false) && !mediaSessionId.empty() )
    {
        m_communication.msgPlayerStop( appTransportId, mediaSessionId );
    }

    if ( requested_seek.exchange(false) && !mediaSessionId.empty() )
    {
        char current_time[32];
        m_seek_request_time = mdate() + SEEK_FORWARD_OFFSET;
        if( snprintf( current_time, sizeof(current_time), "%.3f", double( m_seek_request_time ) / 1000000.0 ) >= (int)sizeof(current_time) )
        {
            msg_Err( p_module, "snprintf() truncated string for mediaSessionId" );
            current_time[sizeof(current_time) - 1] = '\0';
        }
        vlc_mutex_locker locker(&lock);
        setPlayerStatus(CMD_SEEK_SENT);
        /* send a fake time to seek to, to make sure the device flushes its buffers */
        m_communication.msgPlayerSeek( appTransportId, mediaSessionId, current_time );
    }

    int i_ret = m_communication.recvPacket( &b_msgReceived, i_payloadSize,
                            &i_received, p_packet, &b_pingTimeout,
                            &i_waitdelay, &i_retries);


#if defined(_WIN32)
    if ((i_ret < 0 && WSAGetLastError() != WSAEWOULDBLOCK) || (i_ret == 0))
#else
    if ((i_ret < 0 && errno != EAGAIN) || i_ret == 0)
#endif
    {
        msg_Err( p_module, "The connection to the Chromecast died (receiving).");
        vlc_mutex_locker locker(&lock);
        setConnectionStatus(CHROMECAST_CONNECTION_DEAD);
    }

    if (b_pingTimeout)
    {
        m_communication.msgPing();
        m_communication.msgReceiverGetStatus();
    }

    if (b_msgReceived)
    {
        castchannel::CastMessage msg;
        msg.ParseFromArray(p_packet + PACKET_HEADER_LEN, i_payloadSize);
        processMessage(msg);
    }

    vlc_mutex_locker locker(&lock);
    return conn_status != CHROMECAST_CONNECTION_DEAD;
}

void intf_sys_t::notifySendRequest()
{
    vlc_interrupt_raise( p_ctl_thread_interrupt );
}

void intf_sys_t::requestPlayerStop()
{
    requested_stop = true;
    setHasInput(false);
    notifySendRequest();
}

void intf_sys_t::requestPlayerSeek(mtime_t pos)
{
    vlc_mutex_locker locker(&lock);
    if ( pos != VLC_TS_INVALID )
        i_ts_local_start = pos;
    requested_seek = true;
    notifySendRequest();
}

void intf_sys_t::setPauseState(bool paused)
{
    msg_Dbg( p_module, "%s state for %s", paused ? "paused" : "playing", title.c_str() );
    if ( !paused )
    {
        if ( !mediaSessionId.empty() && receiverState != RECEIVER_IDLE )
        {
            m_communication.msgPlayerPlay( appTransportId, mediaSessionId );
            setPlayerStatus(CMD_PLAYBACK_SENT);
        }
    }
    else
    {
        if ( !mediaSessionId.empty() && receiverState != RECEIVER_IDLE )
        {
            m_communication.msgPlayerPause( appTransportId, mediaSessionId );
            setPlayerStatus(CMD_PLAYBACK_SENT);
        }
    }
}

void intf_sys_t::waitAppStarted()
{
    vlc_mutex_locker locker(&lock);
    mutex_cleanup_push(&lock);
    while ( conn_status != CHROMECAST_APP_STARTED &&
            conn_status != CHROMECAST_CONNECTION_DEAD )
        vlc_cond_wait(&loadCommandCond, &lock);
    vlc_cleanup_pop();
}

void intf_sys_t::waitSeekDone()
{
    vlc_mutex_locker locker(&lock);
    if ( m_seek_request_time != VLC_TS_INVALID )
    {
        mutex_cleanup_push(&lock);
        while ( m_chromecast_start_time < m_seek_request_time &&
                conn_status == CHROMECAST_APP_STARTED )
        {
#ifndef NDEBUG
            msg_Dbg( p_module, "waiting for Chromecast seek" );
#endif
            vlc_cond_wait(&seekCommandCond, &lock);
#ifndef NDEBUG
            msg_Dbg( p_module, "finished waiting for Chromecast seek" );
#endif
        }
        vlc_cleanup_pop();
        m_seek_request_time = VLC_TS_INVALID;
    }
}

mtime_t intf_sys_t::get_time(void *pt)
{
    intf_sys_t *p_this = reinterpret_cast<intf_sys_t*>(pt);
    vlc_mutex_locker locker( &p_this->lock );
    return p_this->getPlaybackTimestamp();
}

double intf_sys_t::get_position(void *pt)
{
    intf_sys_t *p_this = reinterpret_cast<intf_sys_t*>(pt);
    vlc_mutex_locker locker( &p_this->lock );
    return p_this->getPlaybackPosition();
}

void intf_sys_t::set_length(void *pt, mtime_t length)
{
    intf_sys_t *p_this = reinterpret_cast<intf_sys_t*>(pt);
    p_this->i_length = length;
}

void intf_sys_t::wait_app_started(void *pt)
{
    intf_sys_t *p_this = reinterpret_cast<intf_sys_t*>(pt);
    p_this->waitAppStarted();
}

void intf_sys_t::request_seek(void *pt, mtime_t pos)
{
    intf_sys_t *p_this = reinterpret_cast<intf_sys_t*>(pt);
    p_this->requestPlayerSeek(pos);
}

void intf_sys_t::wait_seek_done(void *pt)
{
    intf_sys_t *p_this = reinterpret_cast<intf_sys_t*>(pt);
    p_this->waitSeekDone();
}

void intf_sys_t::set_pause_state(void *pt, bool paused)
{
    intf_sys_t *p_this = reinterpret_cast<intf_sys_t*>(pt);
    p_this->setPauseState( paused );
}

void intf_sys_t::set_title(void *pt, const char *psz_title)
{
    intf_sys_t *p_this = reinterpret_cast<intf_sys_t*>(pt);
    p_this->setTitle( psz_title );
}

void intf_sys_t::set_artwork(void *pt, const char *psz_artwork)
{
    intf_sys_t *p_this = reinterpret_cast<intf_sys_t*>(pt);
    p_this->setArtwork( psz_artwork );
}
