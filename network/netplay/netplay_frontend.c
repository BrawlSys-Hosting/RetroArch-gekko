/*  RetroArch - A frontend for libretro.
 *  Copyright (C) 2010-2014 - Hans-Kristian Arntzen
 *  Copyright (C) 2011-2021 - Daniel De Matteis
 *  Copyright (C) 2024-2025 - Jamie Meyer
 *
 *  RetroArch is free software: you can redistribute it and/or modify it under the terms
 *  of the GNU General Public License as published by the Free Software Found-
 *  ation, either version 3 of the License, or (at your option) any later version.
 *
 *  RetroArch is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY;
 *  without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR
 *  PURPOSE.  See the GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License along with RetroArch.
 *  If not, see <http://www.gnu.org/licenses/>.
 */

#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>

#ifdef HAVE_CONFIG_H
#include "../../config.h"
#endif

#include "../../config.def.h"

#include <libretro.h>
#include <lists/string_list.h>
#include <lrc_hash.h>
#include <string/stdstring.h>

#include "../../configuration.h"
#include "../../core.h"
#include "../../runloop.h"
#include "../../runahead.h"
#include "../../verbosity.h"
#include "../../msg_hash.h"

#include "netplay.h"

#if defined(_WIN32)
#include "../../gekkonet/windows/include/gekkonet.h"
#elif defined(__APPLE__)
#include "../../gekkonet/mac/include/gekkonet.h"
#else
#include "../../gekkonet/linux/include/gekkonet.h"
#endif

#define NETPLAY_BUTTON_COUNT 16

static const unsigned netplay_button_map[NETPLAY_BUTTON_COUNT] = {
   RETRO_DEVICE_ID_JOYPAD_B,
   RETRO_DEVICE_ID_JOYPAD_Y,
   RETRO_DEVICE_ID_JOYPAD_SELECT,
   RETRO_DEVICE_ID_JOYPAD_START,
   RETRO_DEVICE_ID_JOYPAD_UP,
   RETRO_DEVICE_ID_JOYPAD_DOWN,
   RETRO_DEVICE_ID_JOYPAD_LEFT,
   RETRO_DEVICE_ID_JOYPAD_RIGHT,
   RETRO_DEVICE_ID_JOYPAD_A,
   RETRO_DEVICE_ID_JOYPAD_X,
   RETRO_DEVICE_ID_JOYPAD_L,
   RETRO_DEVICE_ID_JOYPAD_R,
   RETRO_DEVICE_ID_JOYPAD_L2,
   RETRO_DEVICE_ID_JOYPAD_R2,
   RETRO_DEVICE_ID_JOYPAD_L3,
   RETRO_DEVICE_ID_JOYPAD_R3
};

typedef struct netplay
{
   GekkoSession *session;
   GekkoNetAdapter *adapter;
   int local_handle;
   unsigned char num_players;
   unsigned char input_prediction_window;
   unsigned char spectator_delay;
   size_t state_size;
   uint8_t *state_buffer;
   uint8_t *authoritative_input;
   size_t authoritative_size;
   bool authoritative_valid;
   uint16_t local_input_mask;
   bool running;
   bool connected;
   bool session_started;
   bool spectator;
   bool allow_pause;
   bool allow_timeskip;
   unsigned current_frame;
   struct retro_callbacks cbs;
} netplay_t;

static net_driver_state_t networking_driver_st;

static void netplay_session_status_set(const char *status,
      unsigned current, unsigned total)
{
   net_driver_state_t *net_st = &networking_driver_st;

   if (!net_st)
      return;

   if (status)
      strlcpy(net_st->session_status, status,
            sizeof(net_st->session_status));
   else
      net_st->session_status[0] = '\0';

   net_st->session_sync_current = current;
   net_st->session_sync_total   = total;
}

static void netplay_session_status_reset(void)
{
   const char *fallback = msg_hash_to_str(MENU_ENUM_LABEL_VALUE_NOT_AVAILABLE);

   if (!fallback)
      fallback = "";

   netplay_session_status_set(fallback, 0, 0);
}

static void netplay_free(netplay_t *netplay)
{
   if (!netplay)
      return;

   if (netplay->session)
      gekko_destroy(netplay->session);

   (void)netplay->adapter;

   free(netplay->state_buffer);
   free(netplay->authoritative_input);
   free(netplay);
}

net_driver_state_t *networking_state_get_ptr(void)
{
   return &networking_driver_st;
}

static bool netplay_refresh_serialization(netplay_t *netplay)
{
   size_t size;

   if (!netplay)
      return false;

   size = core_serialize_size_special();
   if (!size)
      return false;

   if (size != netplay->state_size)
   {
      uint8_t *new_buf = (uint8_t*)realloc(netplay->state_buffer, size);
      if (!new_buf)
         return false;
      netplay->state_buffer = new_buf;
      netplay->state_size   = size;
   }

   return true;
}

static void netplay_copy_authoritative_input(netplay_t *netplay,
      const unsigned char *data, unsigned int len)
{
   if (!len || !data || !netplay)
   {
      if (netplay)
         netplay->authoritative_valid = false;
      return;
   }

   if (netplay->authoritative_size < len)
   {
      uint8_t *new_buf = (uint8_t*)realloc(netplay->authoritative_input, len);
      if (!new_buf)
      {
         netplay->authoritative_valid = false;
         return;
      }
      netplay->authoritative_input = new_buf;
      netplay->authoritative_size  = len;
   }

   memcpy(netplay->authoritative_input, data, len);
   netplay->authoritative_valid = true;
}

static uint16_t netplay_get_port_mask(const netplay_t *netplay, unsigned port)
{
   if (!netplay || !netplay->authoritative_valid)
      return 0;

   if (!netplay->authoritative_input)
      return 0;

   if (netplay->authoritative_size < sizeof(uint16_t))
      return 0;

   {
      size_t per_player = sizeof(uint16_t);
      size_t offset     = per_player * port;

      if (offset + per_player > netplay->authoritative_size)
         return 0;

      uint16_t mask;
      memcpy(&mask, netplay->authoritative_input + offset, sizeof(mask));
      return mask;
   }
}

static void netplay_collect_local_input(netplay_t *netplay)
{
   unsigned i;
   uint16_t mask = 0;

   if (!netplay)
      return;

   for (i = 0; i < NETPLAY_BUTTON_COUNT; i++)
   {
      int16_t value = netplay->cbs.state_cb(0,
            RETRO_DEVICE_JOYPAD, 0, netplay_button_map[i]);
      if (value)
         mask |= (1U << i);
   }

   netplay->local_input_mask = mask;
   if (netplay->session && netplay->local_handle >= 0)
      gekko_add_local_input(netplay->session, netplay->local_handle,
            &netplay->local_input_mask);
}

static void netplay_handle_save_event(netplay_t *netplay,
      const GekkoGameEvent *event)
{
   retro_ctx_serialize_info_t info;
   unsigned int payload = 0;

   if (!netplay || !event)
      return;

   if (!netplay_refresh_serialization(netplay))
      return;

   info.data = netplay->state_buffer;
   info.size = netplay->state_size;

   if (!core_serialize_special(&info))
      return;

   payload = (unsigned int)info.actual_size;

   if (event->data.save.state && event->data.save.state_len)
   {
      unsigned int copy_size = payload;
      if (copy_size > *event->data.save.state_len)
         copy_size = *event->data.save.state_len;
      memcpy(event->data.save.state, netplay->state_buffer, copy_size);
      *event->data.save.state_len = copy_size;
   }

   if (event->data.save.checksum)
      *event->data.save.checksum = crc32_calculate(
            netplay->state_buffer, payload);
}

static void netplay_handle_load_event(netplay_t *netplay,
      const GekkoGameEvent *event)
{
   retro_ctx_serialize_info_t info;

   if (!netplay || !event)
      return;

   if (!event->data.load.state || !event->data.load.state_len)
      return;

   info.data = event->data.load.state;
   info.size = event->data.load.state_len;

   if (!core_unserialize_special(&info))
      RARCH_WARN("[Netplay] Failed to load state requested by GekkoNet.\n");
}

static void netplay_handle_game_events(netplay_t *netplay)
{
   int count = 0;
   GekkoGameEvent **events;

   if (!netplay || !netplay->session)
      return;

   events = gekko_update_session(netplay->session, &count);
   for (; count > 0 && events; count--, events++)
   {
      GekkoGameEvent *event = *events;
      if (!event)
         continue;

      switch (event->type)
      {
         case AdvanceEvent:
            netplay->current_frame = (unsigned)event->data.adv.frame;
            netplay_copy_authoritative_input(netplay,
                  event->data.adv.inputs,
                  event->data.adv.input_len);
            break;
         case SaveEvent:
            netplay_handle_save_event(netplay, event);
            break;
         case LoadEvent:
            netplay_handle_load_event(netplay, event);
            break;
         default:
            break;
      }
   }
}

static void netplay_handle_session_events(netplay_t *netplay)
{
   int count = 0;
   GekkoSessionEvent **events;
   char status_buf[128];

   if (!netplay || !netplay->session)
      return;

   events = gekko_session_events(netplay->session, &count);
   for (; count > 0 && events; count--, events++)
   {
      GekkoSessionEvent *event = *events;
      if (!event)
         continue;

      switch (event->type)
      {
         case PlayerSyncing:
         {
            unsigned current = (unsigned)event->data.syncing.current;
            unsigned total   = (unsigned)event->data.syncing.max;

            netplay->connected = true;

            snprintf(status_buf, sizeof(status_buf),
                  "Syncing players (%u/%u)", current, total);
            netplay_session_status_set(status_buf, current, total);
            break;
         }
         case SessionStarted:
            netplay->session_started = true;
            netplay->connected       = true;
            netplay_session_status_set(
                  msg_hash_to_str(MSG_NETPLAY_STATUS_PLAYING), 0, 0);
            break;
         case PlayerConnected:
            netplay->connected       = true;
            snprintf(status_buf, sizeof(status_buf),
                  "Peer connected (handle %d)",
                  event->data.connected.handle);
            netplay_session_status_set(status_buf, 0, 0);
            break;
         case PlayerDisconnected:
            if (event->data.disconnected.handle == netplay->local_handle)
               netplay->connected = false;
            snprintf(status_buf, sizeof(status_buf),
                  "Peer disconnected (handle %d)",
                  event->data.disconnected.handle);
            netplay_session_status_set(status_buf, 0, 0);
            break;
         case SpectatorPaused:
            netplay->spectator = true;
            netplay_session_status_set(
                  msg_hash_to_str(MSG_NETPLAY_STATUS_SPECTATING), 0, 0);
            break;
         case SpectatorUnpaused:
            netplay->spectator = false;
            netplay_session_status_set(
                  msg_hash_to_str(MSG_NETPLAY_STATUS_PLAYING), 0, 0);
            break;
         case DesyncDetected:
            RARCH_WARN("[Netplay] Desync detected at frame %d (local 0x%08x remote 0x%08x).\n",
                  event->data.desynced.frame,
                  event->data.desynced.local_checksum,
                  event->data.desynced.remote_checksum);
            snprintf(status_buf, sizeof(status_buf),
                  "Desync detected (frame %d)",
                  event->data.desynced.frame);
            netplay_session_status_set(status_buf, 0, 0);
            break;
         default:
            break;
      }
   }
}

static void netplay_pump_events(netplay_t *netplay)
{
   if (!netplay)
      return;

   netplay_handle_game_events(netplay);
   netplay_handle_session_events(netplay);
}

static void netplay_update_network_stats(netplay_t *netplay)
{
   net_driver_state_t *net_st;
   GekkoNetworkStats stats;

   if (!netplay || !netplay->session)
      return;

   net_st = &networking_driver_st;
   gekko_network_stats(netplay->session, netplay->local_handle, &stats);
   net_st->latest_ping = stats.last_ping;
}

static bool netplay_pre_frame(netplay_t *netplay)
{
   if (!netplay || !netplay->running)
      return false;

   netplay_collect_local_input(netplay);
   netplay_pump_events(netplay);
   return true;
}

static void netplay_post_frame(netplay_t *netplay)
{
   if (!netplay || !netplay->running)
      return;

   netplay_pump_events(netplay);
   netplay_update_network_stats(netplay);
   if (netplay->session)
      gekko_network_poll(netplay->session);
}

static bool netplay_apply_settings(netplay_t *netplay,
      const settings_t *settings)
{
   const char *desync_mode = NULL;

   if (!netplay || !settings)
      return false;

   netplay->allow_pause = settings->bools.netplay_allow_pausing;

   desync_mode = settings->arrays.netplay_desync_handling;
   if (string_is_empty(desync_mode))
      desync_mode = DEFAULT_NETPLAY_DESYNC_HANDLING;

   netplay->allow_timeskip =
      string_is_equal_noncase(desync_mode, "auto") ||
      string_is_equal_noncase(desync_mode, "rollback");

   netplay->num_players = (unsigned char)
      (settings->uints.input_max_users <= 255
         ? settings->uints.input_max_users : 255);
   netplay->input_prediction_window = (unsigned char)
      (settings->uints.netplay_prediction_window <= 255
         ? settings->uints.netplay_prediction_window : 255);
   netplay->spectator_delay = (unsigned char)
      (settings->uints.netplay_local_delay <= 255
         ? settings->uints.netplay_local_delay : 255);

   return netplay_refresh_serialization(netplay);
}

static bool netplay_setup_session(netplay_t *netplay,
      const settings_t *settings, unsigned port)
{
   GekkoConfig cfg;

   if (!netplay)
      return false;

   if (!netplay->session && !gekko_create(&netplay->session))
      return false;

   if (!netplay_apply_settings(netplay, settings))
      return false;

   cfg.num_players             = netplay->num_players;
   cfg.max_spectators          = (unsigned char)
      (settings->uints.netplay_spectator_limit <= 255
         ? settings->uints.netplay_spectator_limit : 255);
   cfg.input_prediction_window = netplay->input_prediction_window;
   cfg.spectator_delay         = netplay->spectator_delay;
   cfg.input_size              = sizeof(uint16_t);
   cfg.state_size              = (unsigned int)netplay->state_size;
   cfg.limited_saving          = false;
   cfg.post_sync_joining       = true;
   cfg.desync_detection        = true;

   netplay->adapter = gekko_default_adapter((unsigned short)port);
   if (!netplay->adapter)
      return false;

   gekko_net_adapter_set(netplay->session, netplay->adapter);
   gekko_start(netplay->session, &cfg);

   netplay->local_handle = gekko_add_actor(netplay->session,
         LocalPlayer, NULL);
   if (netplay->local_handle < 0)
      return false;

   return true;
}

static netplay_t *netplay_new(void)
{
   netplay_t *netplay = (netplay_t*)calloc(1, sizeof(*netplay));
   if (!netplay)
      return NULL;

   netplay->local_handle = -1;
   netplay->running      = true;
   netplay->spectator    = false;

   return netplay;
}

static void netplay_reset_state(netplay_t *netplay)
{
   if (!netplay)
      return;

   netplay->connected           = false;
   netplay->session_started     = false;
   netplay->authoritative_valid = false;
   netplay->current_frame       = 0;
   netplay_session_status_reset();
}

static bool netplay_begin_session(netplay_t *netplay,
      const char *server, unsigned port)
{
   settings_t *settings = config_get_ptr();

   if (!settings || !netplay)
      return false;

   if (!netplay_setup_session(netplay, settings, port))
      return false;

   (void)server;
   netplay_reset_state(netplay);
   return true;
}

static bool netplay_can_start(void)
{
   net_driver_state_t *net_st = &networking_driver_st;
   return (net_st->flags & NET_DRIVER_ST_FLAG_NETPLAY_ENABLED) != 0;
}

bool init_netplay(const char *server, unsigned port, const char *mitm_session)
{
   netplay_t *netplay;
   struct retro_callbacks cbs;
   net_driver_state_t *net_st  = &networking_driver_st;

   (void)mitm_session;

   if (net_st->data || !netplay_can_start())
      return false;

   if (!core_set_default_callbacks(&cbs))
      return false;

   if (!core_set_netplay_callbacks())
      return false;

   netplay = netplay_new();
   if (!netplay)
   {
      core_unset_netplay_callbacks();
      return false;
   }

   netplay->cbs = cbs;
   netplay->running = true;
   netplay->spectator = false;

   if (!netplay_begin_session(netplay, server, port))
   {
      netplay_free(netplay);
      core_unset_netplay_callbacks();
      return false;
   }

   net_st->data        = netplay;
   net_st->latest_ping = -1;
   net_st->flags      &= ~NET_DRIVER_ST_FLAG_NETPLAY_CLIENT_DEFERRED;

   return true;
}

bool init_netplay_deferred(const char *server, unsigned port,
      const char *mitm_session)
{
   net_driver_state_t *net_st = &networking_driver_st;

   (void)mitm_session;

   if (!server)
      return false;

   strlcpy(net_st->server_address_deferred, server,
         sizeof(net_st->server_address_deferred));
   net_st->server_port_deferred = port;
   net_st->flags               |= NET_DRIVER_ST_FLAG_NETPLAY_CLIENT_DEFERRED;

   return true;
}

void deinit_netplay(void)
{
   net_driver_state_t *net_st  = &networking_driver_st;
   netplay_t          *netplay = net_st->data;

   if (netplay)
   {
      netplay_free(netplay);
      net_st->data = NULL;
   }

   net_st->flags &= ~(NET_DRIVER_ST_FLAG_NETPLAY_ENABLED
         | NET_DRIVER_ST_FLAG_NETPLAY_IS_CLIENT);
   net_st->latest_ping = -1;
   netplay_session_status_reset();

#if HAVE_RUNAHEAD
   preempt_init(runloop_state_get_ptr());
#endif

   free(net_st->client_info);
   net_st->client_info       = NULL;
   net_st->client_info_count = 0;

   core_unset_netplay_callbacks();
}

bool netplay_driver_ctl(enum rarch_netplay_ctl_state state, void *data)
{
   net_driver_state_t *net_st  = &networking_driver_st;
   netplay_t          *netplay = net_st->data;

   switch (state)
   {
      case RARCH_NETPLAY_CTL_ENABLE_SERVER:
         net_st->flags |= NET_DRIVER_ST_FLAG_NETPLAY_ENABLED;
         net_st->flags &= ~NET_DRIVER_ST_FLAG_NETPLAY_IS_CLIENT;
         return true;
      case RARCH_NETPLAY_CTL_ENABLE_CLIENT:
         net_st->flags |= (NET_DRIVER_ST_FLAG_NETPLAY_ENABLED
               | NET_DRIVER_ST_FLAG_NETPLAY_IS_CLIENT);
         return true;
      case RARCH_NETPLAY_CTL_DISABLE:
         if (netplay)
            return false;
         net_st->flags &= ~NET_DRIVER_ST_FLAG_NETPLAY_ENABLED;
         return true;
      case RARCH_NETPLAY_CTL_PRE_FRAME:
         return netplay_pre_frame(netplay);
      case RARCH_NETPLAY_CTL_POST_FRAME:
         netplay_post_frame(netplay);
         return true;
      case RARCH_NETPLAY_CTL_IS_ENABLED:
         return netplay && netplay->running;
      case RARCH_NETPLAY_CTL_IS_CONNECTED:
         return netplay && netplay->connected;
      case RARCH_NETPLAY_CTL_IS_SERVER:
         return netplay && netplay->local_handle >= 0
            && !(net_st->flags & NET_DRIVER_ST_FLAG_NETPLAY_IS_CLIENT);
      case RARCH_NETPLAY_CTL_IS_PLAYING:
         return netplay && netplay->connected && !netplay->spectator;
      case RARCH_NETPLAY_CTL_IS_SPECTATING:
         return netplay && netplay->spectator;
      case RARCH_NETPLAY_CTL_IS_DATA_INITED:
         return netplay && netplay->session_started;
      case RARCH_NETPLAY_CTL_ALLOW_PAUSE:
         return netplay ? netplay->allow_pause : false;
      case RARCH_NETPLAY_CTL_ALLOW_TIMESKIP:
         return netplay ? netplay->allow_timeskip : false;
      case RARCH_NETPLAY_CTL_PAUSE:
      case RARCH_NETPLAY_CTL_UNPAUSE:
      case RARCH_NETPLAY_CTL_GAME_WATCH:
      case RARCH_NETPLAY_CTL_PLAYER_CHAT:
      case RARCH_NETPLAY_CTL_REFRESH_CLIENT_INFO:
      case RARCH_NETPLAY_CTL_IS_REPLAYING:
      case RARCH_NETPLAY_CTL_LOAD_SAVESTATE:
      case RARCH_NETPLAY_CTL_RESET:
      case RARCH_NETPLAY_CTL_DISCONNECT:
         if (!netplay)
            return false;
         deinit_netplay();
         return true;
      case RARCH_NETPLAY_CTL_FINISHED_NAT_TRAVERSAL:
      case RARCH_NETPLAY_CTL_DESYNC_PUSH:
      case RARCH_NETPLAY_CTL_DESYNC_POP:
      case RARCH_NETPLAY_CTL_KICK_CLIENT:
      case RARCH_NETPLAY_CTL_BAN_CLIENT:
         return false;
#ifndef HAVE_DYNAMIC
      case RARCH_NETPLAY_CTL_ADD_FORK_ARG:
      case RARCH_NETPLAY_CTL_GET_FORK_ARGS:
      case RARCH_NETPLAY_CTL_CLEAR_FORK_ARGS:
         return false;
#endif
      case RARCH_NETPLAY_CTL_SET_CORE_PACKET_INTERFACE:
         if (net_st->core_netpacket_interface)
         {
            free(net_st->core_netpacket_interface);
            net_st->core_netpacket_interface = NULL;
         }
         if (data)
         {
            net_st->core_netpacket_interface = (struct retro_netpacket_callback*)
               malloc(sizeof(*net_st->core_netpacket_interface));
            if (!net_st->core_netpacket_interface)
               return false;
            *net_st->core_netpacket_interface =
               *(struct retro_netpacket_callback*)data;
         }
         return true;
      case RARCH_NETPLAY_CTL_USE_CORE_PACKET_INTERFACE:
         return net_st->core_netpacket_interface != NULL;
      case RARCH_NETPLAY_CTL_NONE:
      default:
         break;
   }

   return false;
}

bool netplay_reinit_serialization(void)
{
   netplay_t *netplay = networking_driver_st.data;
   if (!netplay)
      return false;
   return netplay_refresh_serialization(netplay);
}

bool netplay_is_spectating(void)
{
   netplay_t *netplay = networking_driver_st.data;
   return netplay && netplay->spectator;
}

void netplay_force_send_savestate(void)
{
   /* No-op for the GekkoNet frontend. */
}

bool netplay_compatible_version(const char *version)
{
   static const uint64_t min_version = 0x0001000900010000ULL; /* 1.9.1 */
   size_t   version_parts            = 0;
   uint64_t version_value            = 0;
   char     *version_end             = NULL;
   bool     loop                     = true;

   if (!version)
      return false;

   /* Convert the version string to an integer first. */
   do
   {
      uint16_t version_part = (uint16_t)strtoul(version, &version_end, 10);

      if (version_end == version) /* Nothing to convert */
         return false;

      switch (*version_end)
      {
         case '\0': /* End of version string */
            loop = false;
            break;
         case '.':
            version = (const char*)version_end + 1;
            break;
         default: /* Invalid version string */
            return false;
      }

      /* We only want enough bits as to fit into version_value. */
      if (version_parts++ < (sizeof(version_value) / sizeof(version_part)))
         version_value |= (uint64_t)version_part <<
            ((sizeof(version_value) << 3) -
               ((sizeof(version_part) << 3) * version_parts));
   } while (loop);

   return version_value >= min_version;
}

bool netplay_decode_hostname(const char *hostname,
      char *address, unsigned *port, char *session, size_t len)
{
   struct string_list hostname_data;

   if (string_is_empty(hostname))
      return false;
   if (!string_list_initialize(&hostname_data))
      return false;
   if (!string_split_noalloc(&hostname_data, hostname, "|"))
   {
      string_list_deinitialize(&hostname_data);
      return false;
   }

   if (hostname_data.size >= 1 &&
         !string_is_empty(hostname_data.elems[0].data))
   {
      if (address)
         strlcpy(address, hostname_data.elems[0].data, len);
   }
   if (hostname_data.size >= 2 &&
         !string_is_empty(hostname_data.elems[1].data))
   {
      if (port)
      {
         unsigned long int tmp_port = (unsigned long int)
            strtoul(hostname_data.elems[1].data, NULL, 10);

         if (tmp_port && tmp_port <= 65535)
            *port = (unsigned)tmp_port;
      }
   }
   if (hostname_data.size >= 3 &&
         !string_is_empty(hostname_data.elems[2].data))
   {
      if (session)
         strlcpy(session, hostname_data.elems[2].data, len);
   }

   string_list_deinitialize(&hostname_data);

   return true;
}

#ifdef HAVE_NETPLAYDISCOVERY
bool init_netplay_discovery(void)
{
   return false;
}

void deinit_netplay_discovery(void)
{
}

bool netplay_discovery_driver_ctl(enum rarch_netplay_discovery_ctl_state state,
      void *data)
{
   (void)state;
   (void)data;
   return false;
}
#endif

int16_t input_state_net(unsigned port, unsigned device,
      unsigned idx, unsigned id)
{
   net_driver_state_t *net_st  = &networking_driver_st;
   netplay_t          *netplay = net_st->data;

   if (!netplay || !netplay->running)
      return 0;

   if (device == RETRO_DEVICE_JOYPAD && idx == 0)
   {
      unsigned i;
      uint16_t mask = netplay_get_port_mask(netplay, port);

      for (i = 0; i < NETPLAY_BUTTON_COUNT; i++)
      {
         if (netplay_button_map[i] == id)
            return (mask & (1U << i)) ? 1 : 0;
      }
   }

   return netplay->cbs.state_cb(port, device, idx, id);
}

#ifdef HAVE_GFX_WIDGETS
static void gfx_widget_netplay_chat_iterate(void *user_data,
      unsigned width, unsigned height, bool fullscreen,
      const char *dir_assets, char *font_path, bool is_threaded)
{
   (void)user_data;
   (void)width;
   (void)height;
   (void)fullscreen;
   (void)dir_assets;
   (void)font_path;
   (void)is_threaded;
}

static void gfx_widget_netplay_chat_frame(void *data, void *userdata)
{
   (void)data;
   (void)userdata;
}

static void gfx_widget_netplay_ping_iterate(void *user_data,
      unsigned width, unsigned height, bool fullscreen,
      const char *dir_assets, char *font_path, bool is_threaded)
{
   (void)user_data;
   (void)width;
   (void)height;
   (void)fullscreen;
   (void)dir_assets;
   (void)font_path;
   (void)is_threaded;
}

static void gfx_widget_netplay_ping_frame(void *data, void *userdata)
{
   (void)data;
   (void)userdata;
}

const gfx_widget_t gfx_widget_netplay_chat = {
   NULL,
   NULL,
   NULL,
   NULL,
   NULL,
   &gfx_widget_netplay_chat_iterate,
   &gfx_widget_netplay_chat_frame
};

const gfx_widget_t gfx_widget_netplay_ping = {
   NULL,
   NULL,
   NULL,
   NULL,
   NULL,
   &gfx_widget_netplay_ping_iterate,
   &gfx_widget_netplay_ping_frame
};
#endif
