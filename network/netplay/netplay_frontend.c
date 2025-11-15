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

#if defined(__linux__)
#include <unistd.h>
#include <limits.h>
#include <dlfcn.h>
#endif

#ifdef HAVE_CONFIG_H
#include "../../config.h"
#endif

#include "../../config.def.h"

#include <libretro.h>
#include <lists/string_list.h>
#include <encodings/crc32.h>
#include <string/stdstring.h>

#include "../../configuration.h"
#include "../../core.h"
#include "../../paths.h"
#include "../../runloop.h"
#include "../../runahead.h"
#include "../../verbosity.h"
#include "../../msg_hash.h"
#include "../../audio/audio_driver.h"
#include "../../gfx/video_driver.h"

#ifdef HAVE_NETWORKING
#include <net/net_socket.h>
#endif

#ifdef HAVE_GFX_WIDGETS
#include "../../gfx/gfx_widgets.h"
#endif

#include "netplay.h"
#include "netplay_private.h"

#include <file/file_path.h>
#include <streams/file_stream.h>

#if defined(_WIN32) && !defined(GEKKONET_FORCE_STATIC_LINK)
/* Use the runtime loader on Windows builds unless the build system
 * explicitly requests a static link. Suppress __declspec import/export
 * annotations so we can bind symbols at runtime. */
#ifndef GEKKONET_STATIC
#define GEKKONET_STATIC
#endif
#endif

#if defined(_WIN32)
#include "../../gekkonet/windows/include/gekkonet.h"
#elif defined(__APPLE__)
#include "../../gekkonet/mac/include/gekkonet.h"
#else
#include "../../gekkonet/linux/include/gekkonet.h"
#endif

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <wchar.h>
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

static net_driver_state_t networking_driver_st;

static const char *netplay_diag_last_error_string(void);

#ifdef HAVE_NETWORKING
static bool netplay_udp_port_available(unsigned short port, bool *verified)
{
   struct addrinfo *addr = NULL;
   int fd                = -1;
   bool available        = true;

   if (verified)
      *verified = false;

   fd = socket_init((void**)&addr, port, NULL, SOCKET_TYPE_DATAGRAM, AF_INET);
   if (fd < 0 || !addr)
      goto cleanup;

   if (verified)
      *verified = true;

   available = socket_bind(fd, addr);

cleanup:
   if (fd >= 0)
      socket_close(fd);

   if (addr)
      freeaddrinfo_retro(addr);

   return available;
}
#else
static bool netplay_udp_port_available(unsigned short port, bool *verified)
{
   (void)port;
   if (verified)
      *verified = false;
   return true;
}
#endif

#define NETPLAY_DIAG_PATH_MAX 512

typedef struct netplay_host_diagnostics
{
   unsigned requested_port;
   unsigned resolved_port;
   bool     port_probe_supported;
   bool     initial_probe_available;
   bool     initial_probe_verified;
   bool     fallback_scan_attempted;
   bool     fallback_succeeded;
   unsigned fallback_attempts;
   bool     fallback_aborted_on_wrap;
   bool     fallback_aborted_on_unverified;
   bool     netplay_driver_enabled;
   bool     netplay_driver_auto_enabled;
   bool     netplay_driver_request_client;
   bool     netplay_state_allocated;
   bool     core_callbacks_ready;
   bool     netplay_callbacks_ready;
   bool     serialization_ready;
   bool     session_created;
   bool     settings_applied;
   bool     adapter_acquired;
   bool     session_started;
   bool     local_actor_registered;
   bool     gekkonet_dynamic_load_attempted;
   bool     gekkonet_module_loaded;
   bool     gekkonet_symbols_resolved;
   bool     diagnosis_written;
   char     gekkonet_module_path[NETPLAY_DIAG_PATH_MAX];
   char     diagnosis_path[NETPLAY_DIAG_PATH_MAX];
   char     failure_stage[64];
   char     failure_reason[128];
} netplay_host_diagnostics_t;

#define NETPLAY_DIAG_LOG(...) \
   do { \
      if (verbosity_is_enabled()) \
         RARCH_LOG("[GekkoNet][Diag] " __VA_ARGS__); \
   } while (0)

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

#if (defined(_WIN32) || defined(__linux__)) && !defined(GEKKONET_FORCE_STATIC_LINK)
/* Enable the libGekkoNet dynamic loader on platforms where runtime
 * symbol resolution is supported. Toolchains that prefer static linking
 * can opt out by defining GEKKONET_FORCE_STATIC_LINK. */
#define GEKKONET_DYNAMIC_LOAD 1
#endif

static const char *gekkonet_api_last_error_string(void);

#if defined(GEKKONET_DYNAMIC_LOAD)
#if defined(_WIN32)
#define GEKKONET_MAX_PATH_UTF8 (MAX_PATH * 3)
#define GEKKONET_CALL __cdecl
typedef HMODULE gekkonet_module_handle_t;
#else
#ifndef PATH_MAX
#define PATH_MAX 4096
#endif
#define GEKKONET_MAX_PATH_UTF8 PATH_MAX
#define GEKKONET_CALL
typedef void *gekkonet_module_handle_t;
#endif

typedef bool (GEKKONET_CALL *gekkonet_create_proc_t)(GekkoSession **session);
typedef bool (GEKKONET_CALL *gekkonet_destroy_proc_t)(GekkoSession *session);
typedef void (GEKKONET_CALL *gekkonet_start_proc_t)(GekkoSession *session, GekkoConfig *config);
typedef void (GEKKONET_CALL *gekkonet_net_adapter_set_proc_t)(GekkoSession *session, GekkoNetAdapter *adapter);
typedef int  (GEKKONET_CALL *gekkonet_add_actor_proc_t)(GekkoSession *session, GekkoPlayerType player_type, GekkoNetAddress *addr);
typedef void (GEKKONET_CALL *gekkonet_add_local_input_proc_t)(GekkoSession *session, int player, void *input);
typedef GekkoGameEvent **(GEKKONET_CALL *gekkonet_update_session_proc_t)(GekkoSession *session, int *count);
typedef GekkoSessionEvent **(GEKKONET_CALL *gekkonet_session_events_proc_t)(GekkoSession *session, int *count);
typedef void (GEKKONET_CALL *gekkonet_network_stats_proc_t)(GekkoSession *session, int player, GekkoNetworkStats *stats);
typedef void (GEKKONET_CALL *gekkonet_network_poll_proc_t)(GekkoSession *session);
typedef GekkoNetAdapter *(GEKKONET_CALL *gekkonet_default_adapter_proc_t)(unsigned short port);
typedef const char *(GEKKONET_CALL *gekkonet_last_error_proc_t)(void);

typedef struct gekkonet_dynamic_api
{
   gekkonet_module_handle_t           module;
   bool                              attempted_load;
   bool                              load_failed;
   char                              module_path_utf8[GEKKONET_MAX_PATH_UTF8];
   gekkonet_create_proc_t            create;
   gekkonet_destroy_proc_t           destroy;
   gekkonet_start_proc_t             start;
   gekkonet_net_adapter_set_proc_t   net_adapter_set;
   gekkonet_add_actor_proc_t         add_actor;
   gekkonet_add_local_input_proc_t   add_local_input;
   gekkonet_update_session_proc_t    update_session;
   gekkonet_session_events_proc_t    session_events;
   gekkonet_network_stats_proc_t     network_stats;
   gekkonet_network_poll_proc_t      network_poll;
   gekkonet_default_adapter_proc_t   default_adapter;
   gekkonet_last_error_proc_t        last_error;
} gekkonet_dynamic_api_t;

static gekkonet_dynamic_api_t g_gekkonet_api;

#if defined(GEKKONET_DYNAMIC_LOAD)

static void gekkonet_reset_module_path(void)
{
   g_gekkonet_api.module_path_utf8[0] = '\0';
}

#if defined(_WIN32)

static bool gekkonet_wide_to_utf8(const wchar_t *src, char *dst, size_t dst_size)
{
   if (!src || !dst || !dst_size)
      return false;

   if (!WideCharToMultiByte(CP_UTF8, 0, src, -1,
         dst, (int)dst_size, NULL, NULL))
   {
      dst[0] = '\0';
      return false;
   }

   return true;
}

static void gekkonet_store_module_path(HMODULE module)
{
   wchar_t wide_path[MAX_PATH];
   DWORD   len = GetModuleFileNameW(module, wide_path, MAX_PATH);

   if (!len || len >= MAX_PATH)
   {
      gekkonet_reset_module_path();
      return;
   }

   if (!gekkonet_wide_to_utf8(wide_path,
         g_gekkonet_api.module_path_utf8,
         sizeof(g_gekkonet_api.module_path_utf8)))
      gekkonet_reset_module_path();
}

static void gekkonet_trim_newlines(char *str)
{
   size_t len;

   if (!str)
      return;

   len = strlen(str);
   while (len && (str[len - 1] == '\r' || str[len - 1] == '\n'))
   {
      str[len - 1] = '\0';
      len--;
   }
}

static void gekkonet_log_win32_error(const char *context, DWORD error_code)
{
   LPWSTR wide_msg = NULL;
   DWORD  flags    = FORMAT_MESSAGE_ALLOCATE_BUFFER
      | FORMAT_MESSAGE_FROM_SYSTEM
      | FORMAT_MESSAGE_IGNORE_INSERTS;
   DWORD  len      = FormatMessageW(flags, NULL, error_code,
         MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
         (LPWSTR)&wide_msg, 0, NULL);

   if (len && wide_msg)
   {
      char utf8_msg[512];
      if (WideCharToMultiByte(CP_UTF8, 0, wide_msg, -1,
               utf8_msg, sizeof(utf8_msg), NULL, NULL))
      {
         gekkonet_trim_newlines(utf8_msg);
         RARCH_ERR("%s: %s (0x%lx)\n", context, utf8_msg,
               (unsigned long)error_code);
      }
      else
         RARCH_ERR("%s: Win32 error 0x%lx\n", context,
               (unsigned long)error_code);
      LocalFree(wide_msg);
   }
   else
      RARCH_ERR("%s: Win32 error 0x%lx\n", context,
            (unsigned long)error_code);

   if (error_code == ERROR_MOD_NOT_FOUND)
      RARCH_ERR("[GekkoNet] The DLL or one of its dependencies was not found. "
            "Ensure libGekkoNet.dll ships with all required runtimes.\n");
   else if (error_code == ERROR_BAD_EXE_FORMAT)
      RARCH_ERR("[GekkoNet] The DLL is built for a different architecture. "
            "Use the 64-bit build of libGekkoNet with 64-bit RetroArch.\n");
}

static bool gekkonet_file_exists(const wchar_t *path)
{
   DWORD attrs;

   if (!path)
      return false;

   attrs = GetFileAttributesW(path);
   if (attrs == INVALID_FILE_ATTRIBUTES)
      return false;

   return (attrs & FILE_ATTRIBUTE_DIRECTORY) == 0;
}

static void gekkonet_log_load_context(const wchar_t *path,
      const char *path_utf8, DWORD error)
{
   if (!path || !path_utf8 || !path_utf8[0])
      return;

   if (error == ERROR_MOD_NOT_FOUND)
   {
      if (gekkonet_file_exists(path))
      {
         RARCH_ERR("[GekkoNet] libGekkoNet.dll exists at %s but a required "
               "dependency is missing. Use a dependency checker (e.g. "
               "Dependencies or Dependency Walker) to identify the missing "
               "runtime.\n", path_utf8);
      }
      else
      {
         RARCH_ERR("[GekkoNet] libGekkoNet.dll was not found at %s. Confirm "
               "the file is present and readable.\n", path_utf8);
      }
   }
}

static bool gekkonet_build_module_path(const wchar_t *filename,
      wchar_t *buffer, size_t capacity)
{
   DWORD length;

   if (!filename || !buffer || !capacity)
      return false;

   length = GetModuleFileNameW(NULL, buffer, (DWORD)capacity);

   if (!length || length >= capacity)
      return false;

   {
      wchar_t *last_sep = wcsrchr(buffer, L'\\');
      if (last_sep)
      {
         size_t base_len      = (size_t)(last_sep + 1 - buffer);
         size_t filename_len  = wcslen(filename);
         size_t required_size = base_len + filename_len;

         if (required_size + 1 < capacity)
         {
            wmemcpy(last_sep + 1, filename, filename_len + 1);
            return true;
         }
      }
   }

   return false;
}

static bool gekkonet_load_library(void)
{
   HMODULE module;
   wchar_t module_path[MAX_PATH];
   char    module_path_utf8[GEKKONET_MAX_PATH_UTF8];
   bool    have_module_path = false;
   DWORD   primary_error    = 0;
   DWORD   fallback_error   = 0;

   if (g_gekkonet_api.module)
      return true;

   if (g_gekkonet_api.attempted_load)
      return false;

   g_gekkonet_api.attempted_load = true;
   g_gekkonet_api.load_failed    = false;
   g_gekkonet_api.last_error     = NULL;
   gekkonet_reset_module_path();

   module = NULL;
   module_path_utf8[0] = '\0';

   if (gekkonet_build_module_path(L"libGekkoNet.dll",
         module_path, ARRAY_SIZE(module_path)))
   {
      have_module_path = true;
      if (!gekkonet_wide_to_utf8(module_path,
            module_path_utf8, sizeof(module_path_utf8)))
         module_path_utf8[0] = '\0';

      module = LoadLibraryW(module_path);
      if (!module)
      {
         primary_error = GetLastError();
         if (module_path_utf8[0])
            RARCH_ERR("[GekkoNet] Attempted to load: %s\n",
                  module_path_utf8);
      }
   }

   if (!module)
   {
      module = LoadLibraryW(L"libGekkoNet.dll");
      if (!module)
      {
         fallback_error = GetLastError();
         RARCH_ERR("[GekkoNet] Failed to load libGekkoNet.dll\n");
         if (!have_module_path)
         {
            wchar_t fallback_path[MAX_PATH];
            if (GetModuleFileNameW(NULL, fallback_path,
                     ARRAY_SIZE(fallback_path)))
            {
               wchar_t *last_sep = wcsrchr(fallback_path, L'\\');
               if (last_sep)
               {
                  *(last_sep + 1) = L'\0';
                  if (gekkonet_wide_to_utf8(fallback_path,
                           module_path_utf8,
                           sizeof(module_path_utf8)))
                     RARCH_ERR("[GekkoNet] RetroArch executable directory: %s\n",
                           module_path_utf8);
               }
            }
         }
         {
            DWORD error = primary_error ? primary_error : fallback_error;

            if (have_module_path && module_path_utf8[0])
               gekkonet_log_load_context(module_path, module_path_utf8,
                     primary_error ? primary_error : error);
            else if (!have_module_path)
            {
               wchar_t located_path[MAX_PATH];
               DWORD  located_len = SearchPathW(NULL, L"libGekkoNet.dll", NULL,
                     ARRAY_SIZE(located_path), located_path, NULL);

               if (located_len > 0 && located_len < ARRAY_SIZE(located_path))
               {
                  char located_utf8[GEKKONET_MAX_PATH_UTF8];
                  if (gekkonet_wide_to_utf8(located_path, located_utf8,
                           sizeof(located_utf8)))
                  {
                     RARCH_ERR("[GekkoNet] Attempted to load: %s\n",
                           located_utf8);
                     gekkonet_log_load_context(located_path, located_utf8, error);
                  }
               }
            }

            if (error)
               gekkonet_log_win32_error("[GekkoNet] LoadLibraryW", error);
         }
         g_gekkonet_api.attempted_load = false;
         g_gekkonet_api.load_failed    = true;
         return false;
      }
   }

   g_gekkonet_api.module = module;
   gekkonet_store_module_path(module);

#define GEKKONET_RESOLVE(symbol) \
   do { \
      g_gekkonet_api.symbol = (gekkonet_##symbol##_proc_t)GetProcAddress(module, "gekko_" #symbol); \
      if (!g_gekkonet_api.symbol) \
      { \
         RARCH_ERR("[GekkoNet] Missing symbol: gekko_" #symbol "\n"); \
         FreeLibrary(module); \
         g_gekkonet_api.module = NULL; \
         g_gekkonet_api.last_error = NULL; \
         g_gekkonet_api.attempted_load = false; \
         g_gekkonet_api.load_failed    = true; \
         gekkonet_reset_module_path(); \
         return false; \
      } \
   } while (0)

   GEKKONET_RESOLVE(create);
   GEKKONET_RESOLVE(destroy);
   GEKKONET_RESOLVE(start);
   GEKKONET_RESOLVE(net_adapter_set);
   GEKKONET_RESOLVE(add_actor);
   GEKKONET_RESOLVE(add_local_input);
   GEKKONET_RESOLVE(update_session);
   GEKKONET_RESOLVE(session_events);
   GEKKONET_RESOLVE(network_stats);
   GEKKONET_RESOLVE(network_poll);
   GEKKONET_RESOLVE(default_adapter);

#undef GEKKONET_RESOLVE

   g_gekkonet_api.last_error = (gekkonet_last_error_proc_t)
      GetProcAddress(module, "gekko_last_error");
   if (!g_gekkonet_api.last_error)
      g_gekkonet_api.last_error = (gekkonet_last_error_proc_t)
         GetProcAddress(module, "gekko_get_last_error");

   return true;
}

static void gekkonet_log_session_create_failure(void)
{
   const char *path   = gekkonet_loaded_module_path();
   const char *reason = netplay_diag_last_error_string();

   if (path)
      RARCH_ERR("[GekkoNet] Loaded library: %s\n", path);
   else if (g_gekkonet_api.load_failed)
      RARCH_ERR("[GekkoNet] libGekkoNet.dll could not be located or failed to initialise.\n");

   if (reason && reason[0])
      RARCH_ERR("[GekkoNet] Library error: %s\n", reason);

   RARCH_ERR("[GekkoNet] Ensure the DLL matches this RetroArch build (64-bit) and includes the required exports.\n");
}

#elif defined(__linux__)

static bool gekkonet_build_module_path(const char *filename,
      char *buffer, size_t capacity)
{
   ssize_t len;
   char   *last_sep;
   size_t  base_len;
   size_t  filename_len;

   if (!filename || !buffer || capacity == 0)
      return false;

   len = readlink("/proc/self/exe", buffer, capacity - 1);
   if (len <= 0 || (size_t)len >= capacity - 1)
      return false;

   buffer[len] = '\0';

   last_sep = strrchr(buffer, '/');
   if (!last_sep)
      return false;

   base_len     = (size_t)(last_sep - buffer + 1);
   filename_len = strlen(filename);

   if (base_len + filename_len >= capacity)
      return false;

   memcpy(last_sep + 1, filename, filename_len + 1);
   return true;
}

static bool gekkonet_load_library(void)
{
   void       *module;
   char        module_path[GEKKONET_MAX_PATH_UTF8];
   const char *selected_path = NULL;

   if (g_gekkonet_api.module)
      return true;

   if (g_gekkonet_api.attempted_load)
      return false;

   g_gekkonet_api.attempted_load = true;
   g_gekkonet_api.load_failed    = false;
   g_gekkonet_api.last_error     = NULL;
   gekkonet_reset_module_path();

   module = NULL;
   module_path[0] = '\0';

   if (gekkonet_build_module_path("libGekkoNet.so", module_path,
         sizeof(module_path)))
   {
      dlerror();
      module = dlopen(module_path, RTLD_NOW | RTLD_LOCAL);
      if (module)
         selected_path = module_path;
      else
      {
         const char *error = dlerror();
         if (error)
            RARCH_ERR("[GekkoNet] Failed to load %s: %s\n", module_path, error);
      }
   }

   if (!module)
   {
      dlerror();
      module = dlopen("libGekkoNet.so", RTLD_NOW | RTLD_LOCAL);
      if (module)
         selected_path = "libGekkoNet.so";
      else
      {
         const char *error = dlerror();
         RARCH_ERR("[GekkoNet] Failed to load libGekkoNet.so\n");
         if (error)
            RARCH_ERR("[GekkoNet] dlopen error: %s\n", error);
         g_gekkonet_api.attempted_load = false;
         g_gekkonet_api.load_failed    = true;
         return false;
      }
   }

   g_gekkonet_api.module = module;

   if (selected_path)
      strlcpy(g_gekkonet_api.module_path_utf8, selected_path,
            sizeof(g_gekkonet_api.module_path_utf8));
   else
      gekkonet_reset_module_path();

#define GEKKONET_RESOLVE(symbol) \
   do { \
      dlerror(); \
      g_gekkonet_api.symbol = (gekkonet_##symbol##_proc_t) \
         dlsym(module, "gekko_" #symbol); \
      const char *sym_error = dlerror(); \
      if (!g_gekkonet_api.symbol || sym_error) { \
         RARCH_ERR("[GekkoNet] Missing symbol: gekko_" #symbol "\n"); \
         if (sym_error) \
            RARCH_ERR("[GekkoNet] dlsym error: %s\n", sym_error); \
         dlclose(module); \
         g_gekkonet_api.module = NULL; \
         g_gekkonet_api.last_error = NULL; \
         g_gekkonet_api.attempted_load = false; \
         g_gekkonet_api.load_failed    = true; \
         gekkonet_reset_module_path(); \
         return false; \
      } \
   } while (0)

   GEKKONET_RESOLVE(create);
   GEKKONET_RESOLVE(destroy);
   GEKKONET_RESOLVE(start);
   GEKKONET_RESOLVE(net_adapter_set);
   GEKKONET_RESOLVE(add_actor);
   GEKKONET_RESOLVE(add_local_input);
   GEKKONET_RESOLVE(update_session);
   GEKKONET_RESOLVE(session_events);
   GEKKONET_RESOLVE(network_stats);
   GEKKONET_RESOLVE(network_poll);
   GEKKONET_RESOLVE(default_adapter);

#undef GEKKONET_RESOLVE

   dlerror();
   g_gekkonet_api.last_error = (gekkonet_last_error_proc_t)
      dlsym(module, "gekko_last_error");
   if (!g_gekkonet_api.last_error)
   {
      dlerror();
      g_gekkonet_api.last_error = (gekkonet_last_error_proc_t)
         dlsym(module, "gekko_get_last_error");
      dlerror();
   }

   return true;
}

static void gekkonet_log_session_create_failure(void)
{
   const char *path   = gekkonet_loaded_module_path();
   const char *reason = netplay_diag_last_error_string();

   if (path)
      RARCH_ERR("[GekkoNet] Loaded library: %s\n", path);
   else if (g_gekkonet_api.load_failed)
      RARCH_ERR("[GekkoNet] libGekkoNet.so could not be located or failed to initialise.\n");

   if (reason && reason[0])
      RARCH_ERR("[GekkoNet] Library error: %s\n", reason);

   RARCH_ERR("[GekkoNet] Ensure libGekkoNet.so matches this RetroArch build and exports the required symbols.\n");
}

#else

static bool gekkonet_load_library(void)
{
   g_gekkonet_api.attempted_load = false;
   g_gekkonet_api.load_failed    = true;
   return false;
}

static void gekkonet_log_session_create_failure(void) { }

#endif

static const char *gekkonet_loaded_module_path(void)
{
   return g_gekkonet_api.module_path_utf8[0]
      ? g_gekkonet_api.module_path_utf8 : NULL;
}
static bool gekkonet_api_create(GekkoSession **session)
{
   if (!session)
      return false;
   *session = NULL;

   if (!gekkonet_load_library())
      return false;
   return g_gekkonet_api.create(session);
}

static bool gekkonet_api_destroy(GekkoSession *session)
{
   if (!session)
      return true;
   if (!gekkonet_load_library())
      return false;
   return g_gekkonet_api.destroy(session);
}

static void gekkonet_api_start(GekkoSession *session, GekkoConfig *config)
{
   if (!gekkonet_load_library())
      return;
   g_gekkonet_api.start(session, config);
}

static void gekkonet_api_net_adapter_set(GekkoSession *session, GekkoNetAdapter *adapter)
{
   if (!gekkonet_load_library())
      return;
   g_gekkonet_api.net_adapter_set(session, adapter);
}

static int gekkonet_api_add_actor(GekkoSession *session, GekkoPlayerType player_type, GekkoNetAddress *addr)
{
   if (!gekkonet_load_library())
      return -1;
   return g_gekkonet_api.add_actor(session, player_type, addr);
}

static void gekkonet_api_add_local_input(GekkoSession *session, int player, void *input)
{
   if (!gekkonet_load_library())
      return;
   g_gekkonet_api.add_local_input(session, player, input);
}

static GekkoGameEvent **gekkonet_api_update_session(GekkoSession *session, int *count)
{
   if (!gekkonet_load_library())
      return NULL;
   return g_gekkonet_api.update_session(session, count);
}

static GekkoSessionEvent **gekkonet_api_session_events(GekkoSession *session, int *count)
{
   if (!gekkonet_load_library())
      return NULL;
   return g_gekkonet_api.session_events(session, count);
}

static void gekkonet_api_network_stats(GekkoSession *session, int player, GekkoNetworkStats *stats)
{
   if (!gekkonet_load_library())
      return;
   g_gekkonet_api.network_stats(session, player, stats);
}

static void gekkonet_api_network_poll(GekkoSession *session)
{
   if (!gekkonet_load_library())
      return;
   g_gekkonet_api.network_poll(session);
}

static GekkoNetAdapter *gekkonet_api_default_adapter(unsigned short port)
{
   if (!gekkonet_load_library())
      return NULL;
   return g_gekkonet_api.default_adapter(port);
}

#ifdef GEKKONET_CALL
#undef GEKKONET_CALL
#endif

#else

static void gekkonet_log_session_create_failure(void)
{
   RARCH_ERR("[GekkoNet] Failed to initialise a session. "
         "Ensure libGekkoNet is available and built for this platform.\n");
}

static bool gekkonet_api_create(GekkoSession **session)
{
   return gekko_create(session);
}

static bool gekkonet_api_destroy(GekkoSession *session)
{
   return !session || gekko_destroy(session);
}

static void gekkonet_api_start(GekkoSession *session, GekkoConfig *config)
{
   gekko_start(session, config);
}

static void gekkonet_api_net_adapter_set(GekkoSession *session, GekkoNetAdapter *adapter)
{
   gekko_net_adapter_set(session, adapter);
}

static int gekkonet_api_add_actor(GekkoSession *session, GekkoPlayerType player_type, GekkoNetAddress *addr)
{
   return gekko_add_actor(session, player_type, addr);
}

static void gekkonet_api_add_local_input(GekkoSession *session, int player, void *input)
{
   gekko_add_local_input(session, player, input);
}

static GekkoGameEvent **gekkonet_api_update_session(GekkoSession *session, int *count)
{
   return gekko_update_session(session, count);
}

static GekkoSessionEvent **gekkonet_api_session_events(GekkoSession *session, int *count)
{
   return gekko_session_events(session, count);
}

static void gekkonet_api_network_stats(GekkoSession *session, int player, GekkoNetworkStats *stats)
{
   gekko_network_stats(session, player, stats);
}

static void gekkonet_api_network_poll(GekkoSession *session)
{
   gekko_network_poll(session);
}

static GekkoNetAdapter *gekkonet_api_default_adapter(unsigned short port)
{
   return gekko_default_adapter(port);
}

#endif

static const char *netplay_diag_last_error_string(void)
{
#if defined(GEKKONET_DYNAMIC_LOAD)
   if (!g_gekkonet_api.last_error)
      return NULL;

   return g_gekkonet_api.last_error();
#else
   return NULL;
#endif
}

static void netplay_host_diag_capture_gekkonet_state(netplay_host_diagnostics_t *diag)
{
   if (!diag)
      return;

#if defined(GEKKONET_DYNAMIC_LOAD)
   diag->gekkonet_dynamic_load_attempted =
         g_gekkonet_api.module != NULL || g_gekkonet_api.attempted_load ||
         g_gekkonet_api.load_failed;
   diag->gekkonet_module_loaded = g_gekkonet_api.module != NULL;
   diag->gekkonet_symbols_resolved =
         g_gekkonet_api.module != NULL && !g_gekkonet_api.load_failed;

   if (g_gekkonet_api.module_path_utf8[0])
      strlcpy(diag->gekkonet_module_path,
            g_gekkonet_api.module_path_utf8,
            sizeof(diag->gekkonet_module_path));
   else
      diag->gekkonet_module_path[0] = '\0';
#else
   diag->gekkonet_dynamic_load_attempted = false;
   diag->gekkonet_module_loaded          = true;
   diag->gekkonet_symbols_resolved       = true;
   strlcpy(diag->gekkonet_module_path, "builtin",
         sizeof(diag->gekkonet_module_path));
#endif
}

static void netplay_host_diag_describe_loader(
      const netplay_host_diagnostics_t *diag,
      char *buffer, size_t size)
{
   const char *status = "not used";

   if (!buffer || size == 0)
      return;

   buffer[0] = '\0';

   if (!diag)
   {
      strlcpy(buffer, "unknown", size);
      return;
   }

   if (diag->gekkonet_dynamic_load_attempted)
      status = diag->gekkonet_module_loaded ? "loaded" : "failed";
   else if (!string_is_empty(diag->gekkonet_module_path))
   {
      if (strcmp(diag->gekkonet_module_path, "builtin") == 0)
         status = "builtin (static link)";
      else
         status = diag->gekkonet_module_path;
   }
   else if (diag->gekkonet_module_loaded && diag->gekkonet_symbols_resolved)
      status = "builtin (static link)";

   strlcpy(buffer, status, size);
}

static void netplay_host_diag_write_file(netplay_host_diagnostics_t *diag,
      const char *last_error)
{
   char path[NETPLAY_DIAG_PATH_MAX];
   char base_dir[NETPLAY_DIAG_PATH_MAX];
   char loader_status[64];
   const char *config_path = NULL;
   RFILE *file             = NULL;

   if (!diag)
      return;

   diag->diagnosis_written = false;
   diag->diagnosis_path[0] = '\0';

   config_path = path_get(RARCH_PATH_CONFIG);
   path[0]     = '\0';
   base_dir[0] = '\0';

   if (!string_is_empty(config_path))
   {
      size_t len = fill_pathname_basedir(base_dir, config_path,
            sizeof(base_dir));
      if (len > 0 && !string_is_empty(base_dir))
         fill_pathname_join(path, base_dir, "diagnosis.text",
               sizeof(path));
   }

   if (string_is_empty(path))
      strlcpy(path, "diagnosis.text", sizeof(path));

   strlcpy(diag->diagnosis_path, path, sizeof(diag->diagnosis_path));

   file = filestream_open(path, RETRO_VFS_FILE_ACCESS_WRITE,
         RETRO_VFS_FILE_ACCESS_HINT_NONE);
   if (!file)
      return;

   filestream_printf(file,
         "RetroArch GekkoNet host diagnostics\n");
   filestream_printf(file,
         "-----------------------------------\n");
   netplay_host_diag_describe_loader(diag, loader_status,
         sizeof(loader_status));
   filestream_printf(file,
         "Netplay driver mode            : %s\n",
         diag->netplay_driver_request_client ? "client" : "server");
   filestream_printf(file,
         "Netplay driver enabled         : %s\n",
         diag->netplay_driver_enabled ? "yes" : "no");
   filestream_printf(file,
         "Driver auto-enabled            : %s\n",
         diag->netplay_driver_auto_enabled ? "yes" : "no");
   filestream_printf(file,
         "Netplay state allocation       : %s\n",
         diag->netplay_state_allocated ? "success" : "failed");
   filestream_printf(file,
         "Core callbacks ready           : %s\n",
         diag->core_callbacks_ready ? "yes" : "no");
   filestream_printf(file,
         "Netplay callbacks ready        : %s\n",
         diag->netplay_callbacks_ready ? "yes" : "no");
   filestream_printf(file,
         "Serialization buffer prepared  : %s\n",
         diag->serialization_ready ? "yes" : "no");
   filestream_printf(file,
         "Requested UDP port             : %u\n",
         diag->requested_port);
   filestream_printf(file,
         "Resolved UDP port              : %u%s\n",
         diag->resolved_port,
         (diag->fallback_succeeded &&
          diag->requested_port != diag->resolved_port)
            ? " (fallback)" : "");
   filestream_printf(file,
         "Port probe supported           : %s\n",
         diag->port_probe_supported ? "yes" : "no");
   filestream_printf(file,
         "Initial probe result           : %s%s\n",
         diag->initial_probe_available ? "available" : "in use",
         diag->initial_probe_verified ? "" : " (unverified)");
   if (diag->fallback_scan_attempted)
   {
      filestream_printf(file,
            "Fallback attempts              : %u\n",
            diag->fallback_attempts);
      filestream_printf(file,
            "Fallback result                : %s\n",
            diag->fallback_succeeded ? "port selected" : "failed");
      filestream_printf(file,
            "Fallback aborted on wrap       : %s\n",
            diag->fallback_aborted_on_wrap ? "yes" : "no");
      filestream_printf(file,
            "Fallback aborted on unverified : %s\n",
            diag->fallback_aborted_on_unverified ? "yes" : "no");
   }
   filestream_printf(file,
         "Stage session create           : %s\n",
         diag->session_created ? "success" : "not reached");
   filestream_printf(file,
         "Stage apply settings           : %s\n",
         diag->settings_applied ? "success" : "failed");
   filestream_printf(file,
         "Stage adapter setup            : %s\n",
         diag->adapter_acquired ? "success" : "failed");
   filestream_printf(file,
         "Stage session start            : %s\n",
         diag->session_started ? "success" : "not reached");
   filestream_printf(file,
         "Stage local actor              : %s\n",
         diag->local_actor_registered ? "success" : "failed");
   filestream_printf(file,
         "libGekkoNet dynamic loader     : %s\n",
         loader_status[0] ? loader_status : "not used");
   filestream_printf(file,
         "libGekkoNet symbols resolved   : %s\n",
         diag->gekkonet_symbols_resolved ? "yes" : "no");
   if (diag->gekkonet_module_path[0])
      filestream_printf(file,
            "libGekkoNet module path        : %s\n",
            diag->gekkonet_module_path);
   if (diag->failure_stage[0])
      filestream_printf(file,
            "Failure stage                  : %s\n",
            diag->failure_stage);
   if (diag->failure_reason[0])
      filestream_printf(file,
            "Failure reason                 : %s\n",
            diag->failure_reason);
   if (last_error && last_error[0])
      filestream_printf(file,
            "libGekkoNet reported           : %s\n",
            last_error);

   filestream_close(file);
   diag->diagnosis_written = true;
}

static void netplay_host_diag_dump(netplay_host_diagnostics_t *diag)
{
   const char *last_error = NULL;
   bool verbose           = false;
   char loader_status[64];

   if (!diag)
      return;

   last_error = netplay_diag_last_error_string();
   verbose    = verbosity_is_enabled();
   netplay_host_diag_describe_loader(diag, loader_status,
         sizeof(loader_status));

   if (verbose)
   {
      RARCH_LOG("[GekkoNet][Diag] ----- Host Session Diagnostics -----\n");
      RARCH_LOG("[GekkoNet][Diag] Netplay driver mode : %s\n",
            diag->netplay_driver_request_client ? "client" : "server");
      RARCH_LOG("[GekkoNet][Diag] Driver enabled      : %s\n",
            diag->netplay_driver_enabled ? "yes" : "no");
      RARCH_LOG("[GekkoNet][Diag] Driver auto-enabled : %s\n",
            diag->netplay_driver_auto_enabled ? "yes" : "no");
      RARCH_LOG("[GekkoNet][Diag] State allocation    : %s\n",
            diag->netplay_state_allocated ? "success" : "failed");
      RARCH_LOG("[GekkoNet][Diag] Core callbacks      : %s\n",
            diag->core_callbacks_ready ? "ready" : "failed");
      RARCH_LOG("[GekkoNet][Diag] Netplay callbacks   : %s\n",
            diag->netplay_callbacks_ready ? "ready" : "failed");
      RARCH_LOG("[GekkoNet][Diag] Serialization buffer: %s\n",
            diag->serialization_ready ? "ready" : "unavailable");
      RARCH_LOG("[GekkoNet][Diag] Requested UDP port  : %u\n",
            diag->requested_port);
      RARCH_LOG("[GekkoNet][Diag] Resolved UDP port   : %u%s\n",
            diag->resolved_port,
            (diag->fallback_succeeded &&
             diag->requested_port != diag->resolved_port)
               ? " (fallback)" : "");
      RARCH_LOG("[GekkoNet][Diag] Port probe supported: %s\n",
            diag->port_probe_supported ? "yes" : "no");
      RARCH_LOG("[GekkoNet][Diag] Initial probe       : %s%s\n",
            diag->initial_probe_available ? "available" : "in use",
            diag->initial_probe_verified ? "" : " (unverified)");

      if (diag->fallback_scan_attempted)
      {
         RARCH_LOG("[GekkoNet][Diag] Fallback attempts   : %u\n",
               diag->fallback_attempts);
         RARCH_LOG("[GekkoNet][Diag] Fallback result     : %s\n",
               diag->fallback_succeeded ? "port selected" : "failed");
         if (diag->fallback_aborted_on_wrap)
            RARCH_LOG("[GekkoNet][Diag] Fallback aborted because search wrapped past port 65535.\n");
         if (diag->fallback_aborted_on_unverified)
            RARCH_LOG("[GekkoNet][Diag] Fallback aborted because the platform could not verify candidate ports.\n");
      }

      RARCH_LOG("[GekkoNet][Diag] Stage session create: %s\n",
            diag->session_created ? "success" : "not reached");
      RARCH_LOG("[GekkoNet][Diag] Stage apply settings: %s\n",
            diag->settings_applied ? "success" : "failed");
      RARCH_LOG("[GekkoNet][Diag] Stage adapter setup  : %s\n",
            diag->adapter_acquired ? "success" : "failed");
      RARCH_LOG("[GekkoNet][Diag] Stage session start  : %s\n",
            diag->session_started ? "success" : "not reached");
      RARCH_LOG("[GekkoNet][Diag] Stage local actor    : %s\n",
            diag->local_actor_registered ? "success" : "failed");
      RARCH_LOG("[GekkoNet][Diag] libGekkoNet loader   : %s\n",
            loader_status[0] ? loader_status : "not used");
      RARCH_LOG("[GekkoNet][Diag] libGekkoNet symbols  : %s\n",
            diag->gekkonet_symbols_resolved ? "resolved" : "missing");
      if (diag->gekkonet_module_path[0])
         RARCH_LOG("[GekkoNet][Diag] libGekkoNet module  : %s\n",
               diag->gekkonet_module_path);

      if (diag->failure_stage[0])
         RARCH_LOG("[GekkoNet][Diag] Failure stage       : %s\n",
               diag->failure_stage);
      if (diag->failure_reason[0])
         RARCH_LOG("[GekkoNet][Diag] Failure reason      : %s\n",
               diag->failure_reason);
      if (last_error && last_error[0])
         RARCH_LOG("[GekkoNet][Diag] libGekkoNet reported: %s\n",
               last_error);
      RARCH_LOG("[GekkoNet][Diag] ------------------------------------\n");
   }

   netplay_host_diag_write_file(diag, last_error);

   if (!diag->diagnosis_written)
   {
      RARCH_WARN("[GekkoNet][Diag] Failed to write diagnostics to %s.\n",
            diag->diagnosis_path[0] ? diag->diagnosis_path : "diagnosis.text");
   }
   else
      NETPLAY_DIAG_LOG("Diagnostics written to %s.", diag->diagnosis_path);
}

static void netplay_free(netplay_t *netplay)
{
   if (!netplay)
      return;

   if (netplay->session)
      gekkonet_api_destroy(netplay->session);

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
   {
      RARCH_ERR("[Netplay] Core did not report a save state size; rollback netplay requires save-state capable content.\n");
      return false;
   }

   if (size != netplay->state_size)
   {
      uint8_t *new_buf = (uint8_t*)realloc(netplay->state_buffer, size);
      if (!new_buf)
      {
         RARCH_ERR("[Netplay] Failed to allocate %zu bytes for the serialization buffer.\n", size);
         return false;
      }
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
   size_t per_player;
   size_t offset;
   uint16_t mask;

   if (!netplay || !netplay->authoritative_valid)
      return 0;

   if (!netplay->authoritative_input)
      return 0;

   if (netplay->authoritative_size < sizeof(uint16_t))
      return 0;

   per_player = sizeof(uint16_t);
   offset     = per_player * port;

   if (offset + per_player > netplay->authoritative_size)
      return 0;

   memcpy(&mask, netplay->authoritative_input + offset, sizeof(mask));
   return mask;
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
      gekkonet_api_add_local_input(netplay->session, netplay->local_handle,
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

   payload = (unsigned int)info.size;

   if (event->data.save.state && event->data.save.state_len)
   {
      unsigned int copy_size = payload;
      if (copy_size > *event->data.save.state_len)
         copy_size = *event->data.save.state_len;
      memcpy(event->data.save.state, netplay->state_buffer, copy_size);
      *event->data.save.state_len = copy_size;
   }

   if (event->data.save.checksum)
      *event->data.save.checksum = encoding_crc32(
            0, netplay->state_buffer, payload);
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

   events = gekkonet_api_update_session(netplay->session, &count);
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

   events = gekkonet_api_session_events(netplay->session, &count);
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
   gekkonet_api_network_stats(netplay->session, netplay->local_handle, &stats);
   net_st->latest_ping = stats.last_ping;
}

static bool netplay_pre_frame(netplay_t *netplay)
{
   /* When netplay is not initialised we should not block the core.
    * Returning false here prevents the main runloop from advancing,
    * which manifests as a permanent black screen on local play.
    */
   if (!netplay)
      return true;

   if (!netplay->running)
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
      gekkonet_api_network_poll(netplay->session);
}

static bool netplay_apply_settings(netplay_t *netplay,
      const settings_t *settings,
      netplay_host_diagnostics_t *diag)
{
   const char *desync_mode = NULL;

   if (!netplay || !settings)
      return false;

   netplay->allow_pausing = settings->bools.netplay_allow_pausing;

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

   if (!netplay_refresh_serialization(netplay))
   {
      RARCH_ERR("[Netplay] Unable to prepare serialization buffers; ensure the current core and content support save states.\n");
      return false;
   }

   if (diag)
      diag->serialization_ready = true;

   return true;
}

static bool netplay_setup_session(netplay_t *netplay,
      settings_t *settings, unsigned *port_in_out,
      netplay_host_diagnostics_t *diag)
{
   GekkoConfig cfg;
   unsigned short udp_port       = 0;
   unsigned       requested_port = 0;

   if (!netplay || !settings || !diag)
      return false;

   if (port_in_out && *port_in_out)
      requested_port = *port_in_out;
   else
      requested_port = settings ? settings->uints.netplay_port : 0;

   udp_port               = (unsigned short)requested_port;
   diag->requested_port   = requested_port;
   diag->resolved_port    = udp_port;

   NETPLAY_DIAG_LOG("Preparing host session using requested UDP port %u.",
         requested_port);

   if (!netplay->session && !gekkonet_api_create(&netplay->session))
   {
      RARCH_ERR("[GekkoNet] Failed to create a session with libGekkoNet.\n");
      gekkonet_log_session_create_failure();
      netplay_host_diag_capture_gekkonet_state(diag);
      strlcpy(diag->failure_stage, "session_create",
            sizeof(diag->failure_stage));
      strlcpy(diag->failure_reason,
            "libGekkoNet session handle creation failed",
            sizeof(diag->failure_reason));
      goto netplay_host_fail;
   }

   diag->session_created = true;
   netplay_host_diag_capture_gekkonet_state(diag);
   NETPLAY_DIAG_LOG("Created libGekkoNet session handle.");

   if (!netplay_apply_settings(netplay, settings, diag))
   {
      strlcpy(diag->failure_stage, "apply_settings",
            sizeof(diag->failure_stage));
      strlcpy(diag->failure_reason,
            "netplay_apply_settings returned false",
            sizeof(diag->failure_reason));
      goto netplay_host_fail;
   }

   diag->settings_applied = true;
   NETPLAY_DIAG_LOG("Applied RetroArch netplay settings to session.");

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

   {
      bool port_verified          = false;
      bool port_available         = false;
      bool fallback_port_selected = false;
      const unsigned max_probes   = 16;

      NETPLAY_DIAG_LOG("Probing UDP port %u for availability.", udp_port);
      port_available = netplay_udp_port_available(udp_port, &port_verified);

      diag->initial_probe_available = port_available;
      diag->initial_probe_verified  = port_verified;
      if (port_verified)
         diag->port_probe_supported = true;

      NETPLAY_DIAG_LOG("Probe result for port %u: %s (verified=%s).",
            udp_port,
            port_available ? "available" : "in use",
            port_verified ? "yes" : "no");

      if (!port_available && port_verified)
      {
         unsigned       probe_index;
         unsigned short probe_port = udp_port;

         diag->fallback_scan_attempted = true;
         NETPLAY_DIAG_LOG(
               "Initial port %u unavailable. Scanning up to %u fallback ports.",
               udp_port, max_probes);

         for (probe_index = 0; probe_index < max_probes; probe_index++)
         {
            bool candidate_verified = false;

            probe_port = (unsigned short)(probe_port + 1);
            if (!probe_port)
            {
               diag->fallback_aborted_on_wrap = true;
               NETPLAY_DIAG_LOG(
                     "Stopping fallback scan after wrapping past port 65535.");
               break;
            }

            diag->fallback_attempts = probe_index + 1;
            NETPLAY_DIAG_LOG(
                  "Probing fallback port %u (attempt %u of %u).",
                  probe_port, probe_index + 1, max_probes);

            if (netplay_udp_port_available(probe_port, &candidate_verified) && candidate_verified)
            {
               udp_port               = probe_port;
               port_verified          = true;
               port_available         = true;
               fallback_port_selected = true;
               diag->fallback_succeeded = true;
               diag->port_probe_supported = true;
               NETPLAY_DIAG_LOG("Selected fallback port %u.", udp_port);
               break;
            }

            if (!candidate_verified)
            {
               port_verified = false;
               diag->fallback_aborted_on_unverified = true;
               NETPLAY_DIAG_LOG(
                     "Aborting fallback scan because candidate port %u could not be verified.",
                     probe_port);
               break;
            }
         }
      }

      if (!port_available && port_verified)
      {
         RARCH_ERR("[GekkoNet] UDP port %u is already in use. Close the conflicting application or configure a different port.\n",
               requested_port);
         strlcpy(diag->failure_stage, "port_selection",
               sizeof(diag->failure_stage));
         strlcpy(diag->failure_reason,
               "no verified UDP ports available within fallback window",
               sizeof(diag->failure_reason));
         goto netplay_host_fail;
      }

      if (!port_verified)
         RARCH_WARN("[GekkoNet] Unable to verify availability of UDP port %u. Continuing without a preflight check.\n",
               requested_port);
      else if (fallback_port_selected)
      {
         RARCH_WARN("[GekkoNet] UDP port %u is already in use. Falling back to port %u. Update forwarding rules or configure a different port if needed.\n",
               requested_port, udp_port);
         configuration_set_uint(settings, settings->uints.netplay_port, udp_port);
         NETPLAY_DIAG_LOG("Persisted fallback port %u to configuration.", udp_port);
      }

      if (port_in_out)
         *port_in_out = udp_port;
      requested_port          = udp_port;
      netplay->tcp_port       = udp_port;
      netplay->ext_tcp_port   = udp_port;
      diag->resolved_port     = udp_port;
      netplay->adapter        = gekkonet_api_default_adapter(udp_port);
   }

   if (!netplay->adapter)
   {
      RARCH_ERR("[GekkoNet] Unable to create the default UDP adapter on port %u. Check firewall rules or choose a different port.\n",
            requested_port);
      strlcpy(diag->failure_stage, "adapter_initialisation",
            sizeof(diag->failure_stage));
      strlcpy(diag->failure_reason,
            "gekkonet_api_default_adapter returned NULL",
            sizeof(diag->failure_reason));
      goto netplay_host_fail;
   }

   diag->adapter_acquired = true;
   NETPLAY_DIAG_LOG("Initialised libGekkoNet UDP adapter on port %u.",
         requested_port);

   gekkonet_api_net_adapter_set(netplay->session, netplay->adapter);
   gekkonet_api_start(netplay->session, &cfg);

   diag->session_started = true;
   NETPLAY_DIAG_LOG("Started libGekkoNet session thread.");

   netplay->local_handle = gekkonet_api_add_actor(netplay->session,
         LocalPlayer, NULL);
   if (netplay->local_handle < 0)
   {
      RARCH_ERR("[GekkoNet] Failed to register the local player with the current session.\n");
      strlcpy(diag->failure_stage, "register_local_actor",
            sizeof(diag->failure_stage));
      strlcpy(diag->failure_reason,
            "gekkonet_api_add_actor returned a negative handle",
            sizeof(diag->failure_reason));
      goto netplay_host_fail;
   }

   diag->local_actor_registered = true;
   NETPLAY_DIAG_LOG("Registered local player handle %d with libGekkoNet.",
         netplay->local_handle);

   netplay_host_diag_capture_gekkonet_state(diag);
   return true;

netplay_host_fail:
   netplay_host_diag_capture_gekkonet_state(diag);
   return false;
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
      const char *server, unsigned port,
      netplay_host_diagnostics_t *diag)
{
   settings_t *settings = config_get_ptr();

   if (!settings || !netplay)
      return false;

   if (!netplay_setup_session(netplay, settings, &port, diag))
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
   netplay_t *netplay               = NULL;
   struct retro_callbacks cbs;
   net_driver_state_t *net_st       = &networking_driver_st;
   netplay_host_diagnostics_t diag;
   bool callbacks_set               = false;
   bool success                     = false;
   bool want_client                 = false;

   (void)mitm_session;

   memset(&diag, 0, sizeof(diag));

   if (server && !string_is_empty(server))
      want_client = true;
   else if (net_st->flags & NET_DRIVER_ST_FLAG_NETPLAY_IS_CLIENT)
      want_client = true;

   diag.netplay_driver_request_client = want_client;
   diag.netplay_driver_enabled        = netplay_can_start();

   if (net_st->data)
   {
      RARCH_ERR("[Netplay] Unable to start a new session because one is already active. Disconnect before hosting or joining again.\n");
      strlcpy(diag.failure_stage, "preflight_active_session",
            sizeof(diag.failure_stage));
      strlcpy(diag.failure_reason,
            "net_st->data already set",
            sizeof(diag.failure_reason));
      goto cleanup;
   }

   if (!diag.netplay_driver_enabled)
   {
      bool auto_enabled = false;

      if (want_client)
         auto_enabled = netplay_driver_ctl(RARCH_NETPLAY_CTL_ENABLE_CLIENT, NULL);
      else
         auto_enabled = netplay_driver_ctl(RARCH_NETPLAY_CTL_ENABLE_SERVER, NULL);

      diag.netplay_driver_auto_enabled = auto_enabled;
      diag.netplay_driver_enabled      = netplay_can_start();

      if (!auto_enabled || !diag.netplay_driver_enabled)
      {
         RARCH_ERR("[Netplay] Netplay driver is disabled; enable it from Settings > Network > Netplay or use the host/client menu entries before starting a session.\n");
         strlcpy(diag.failure_stage, "enable_driver",
               sizeof(diag.failure_stage));
         strlcpy(diag.failure_reason,
               "failed to enable requested netplay driver",
               sizeof(diag.failure_reason));
         goto cleanup;
      }
   }

   if (!core_set_default_callbacks(&cbs))
   {
      RARCH_ERR("[Netplay] Failed to configure core callbacks required for netplay.\n");
      strlcpy(diag.failure_stage, "core_callbacks",
            sizeof(diag.failure_stage));
      strlcpy(diag.failure_reason,
            "core_set_default_callbacks returned false",
            sizeof(diag.failure_reason));
      goto cleanup;
   }
   diag.core_callbacks_ready = true;

   if (!core_set_netplay_callbacks())
   {
      RARCH_ERR("[Netplay] Core does not provide netplay callbacks; rollback netplay cannot be initialised.\n");
      strlcpy(diag.failure_stage, "netplay_callbacks",
            sizeof(diag.failure_stage));
      strlcpy(diag.failure_reason,
            "core_set_netplay_callbacks returned false",
            sizeof(diag.failure_reason));
      goto cleanup;
   }
   diag.netplay_callbacks_ready = true;
   callbacks_set = true;

   netplay = netplay_new();
   if (!netplay)
   {
      RARCH_ERR("[Netplay] Failed to allocate netplay state.\n");
      strlcpy(diag.failure_stage, "allocate_state",
            sizeof(diag.failure_stage));
      strlcpy(diag.failure_reason,
            "netplay_new returned NULL",
            sizeof(diag.failure_reason));
      goto cleanup;
   }
   diag.netplay_state_allocated = true;

   netplay->cbs       = cbs;
   netplay->running   = true;
   netplay->spectator = false;

   if (!netplay_begin_session(netplay, server, port, &diag))
   {
      if (!diag.failure_stage[0])
         strlcpy(diag.failure_stage, "session_init",
               sizeof(diag.failure_stage));
      if (!diag.failure_reason[0])
         strlcpy(diag.failure_reason,
               "netplay_begin_session returned false",
               sizeof(diag.failure_reason));
      goto cleanup;
   }

   net_st->data        = netplay;
   net_st->latest_ping = -1;
   net_st->flags      &= ~NET_DRIVER_ST_FLAG_NETPLAY_CLIENT_DEFERRED;

   success = true;

cleanup:
   netplay_host_diag_dump(&diag);

   if (!success)
   {
      if (netplay)
         netplay_free(netplay);
      if (callbacks_set)
         core_unset_netplay_callbacks();
   }

   return success;
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
         return netplay ? netplay->allow_pausing : false;
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
      case RARCH_NETPLAY_CTL_GET_SESSION_STATUS:
         if (!data)
            return false;
         else
         {
            netplay_session_status_info_t *status =
               (netplay_session_status_info_t*)data;
            if (!status)
               return false;
            strlcpy(status->message, net_st->session_status,
                  sizeof(status->message));
            status->session_sync_current = net_st->session_sync_current;
            status->session_sync_total   = net_st->session_sync_total;
         }
         return true;
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

void video_frame_net(const void *data,
      unsigned width, unsigned height, size_t pitch)
{
   net_driver_state_t *net_st  = &networking_driver_st;
   netplay_t          *netplay = net_st ? net_st->data : NULL;

   if (netplay && netplay->cbs.frame_cb)
      netplay->cbs.frame_cb(data, width, height, pitch);
   else
      video_driver_frame(data, width, height, pitch);
}

void audio_sample_net(int16_t left, int16_t right)
{
   net_driver_state_t *net_st  = &networking_driver_st;
   netplay_t          *netplay = net_st ? net_st->data : NULL;

   if (netplay && netplay->cbs.sample_cb)
      netplay->cbs.sample_cb(left, right);
   else
      audio_driver_sample(left, right);
}

size_t audio_sample_batch_net(const int16_t *data, size_t frames)
{
   net_driver_state_t *net_st  = &networking_driver_st;
   netplay_t          *netplay = net_st ? net_st->data : NULL;

   if (netplay && netplay->cbs.sample_batch_cb)
      return netplay->cbs.sample_batch_cb(data, frames);

   return audio_driver_sample_batch(data, frames);
}
