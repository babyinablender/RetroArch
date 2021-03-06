/*  RetroArch - A frontend for libretro.
 *  Copyright (C) 2011-2017 - Daniel De Matteis
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

#include <windows.h>

#include "../input_driver.h"
#include "../input_keymaps.h"

#include "../../configuration.h"
#include "../../gfx/video_driver.h"
#include "../../verbosity.h"

typedef struct
{
   uint8_t keys[256];
} winraw_keyboard_t;

typedef struct
{
   HANDLE hnd;
   LONG x, y, dlt_x, dlt_y;
   LONG whl_u, whl_d;
   bool btn_l, btn_m, btn_r;
} winraw_mouse_t;

typedef struct
{
   bool kbd_mapp_block;
   bool mouse_grab;
   winraw_keyboard_t keyboard;
   HWND window;
   winraw_mouse_t *mice;
   const input_device_driver_t *joypad;
} winraw_input_t;

static winraw_keyboard_t *g_keyboard = NULL;
static winraw_mouse_t *g_mice        = NULL;
static unsigned g_mouse_cnt          = 0;
static bool g_mouse_xy_mapping_ready = false;
static double g_view_abs_ratio_x     = 0.0;
static double g_view_abs_ratio_y     = 0.0;

static HWND winraw_create_window(WNDPROC wnd_proc)
{
   HWND wnd;
   WNDCLASSA wc = {0};

   wc.hInstance = GetModuleHandleA(NULL);

   if (!wc.hInstance)
   {
      RARCH_ERR("[WINRAW]: GetModuleHandleA failed with error %lu.\n", GetLastError());
      return NULL;
   }

   wc.lpfnWndProc   = wnd_proc;
   wc.lpszClassName = "winraw-input";
   if (!RegisterClassA(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS)
   {
      RARCH_ERR("[WINRAW]: RegisterClassA failed with error %lu.\n", GetLastError());
      return NULL;
   }

   wnd = CreateWindowExA(0, wc.lpszClassName, NULL, 0, 0, 0, 0, 0,
         HWND_MESSAGE, NULL, NULL, NULL);
   if (!wnd)
   {
      RARCH_ERR("[WINRAW]: CreateWindowExA failed with error %lu.\n", GetLastError());
      goto error;
   }

   return wnd;

error:
   UnregisterClassA(wc.lpszClassName, NULL);
   return NULL;
}

static void winraw_destroy_window(HWND wnd)
{
   BOOL r;

   if (!wnd)
      return;

   r = DestroyWindow(wnd);

   if (!r)
   {
      RARCH_WARN("[WINRAW]: DestroyWindow failed with error %lu.\n", GetLastError());
   }

   r = UnregisterClassA("winraw-input", NULL);

   if (!r)
   {
      RARCH_WARN("[WINRAW]: UnregisterClassA failed with error %lu.\n", GetLastError());
   }
}

static bool winraw_set_keyboard_input(HWND window)
{
   RAWINPUTDEVICE rid;
   BOOL r;

   rid.dwFlags     = window ? 0 : RIDEV_REMOVE;
   rid.hwndTarget  = window;
   rid.usUsagePage = 0x01; /* generic desktop */
   rid.usUsage     = 0x06; /* keyboard */

   r               = RegisterRawInputDevices(&rid, 1, sizeof(RAWINPUTDEVICE));

   if (!r)
   {
      RARCH_ERR("[WINRAW]: RegisterRawInputDevices failed with error %lu.\n", GetLastError());
      return false;
   }

   return true;
}

static void winraw_log_mice_info(winraw_mouse_t *mice, unsigned mouse_cnt)
{
   UINT r;
   unsigned i;
   char name[256];
   UINT name_size = sizeof(name);

   for (i = 0; i < mouse_cnt; ++i)
   {
      r = GetRawInputDeviceInfoA(mice[i].hnd, RIDI_DEVICENAME, name, &name_size);
      if (r == (UINT)-1 || r == 0)
         name[0] = '\0';
      RARCH_LOG("[WINRAW]: Mouse #%u %s.\n", i, name);
   }
}

static bool winraw_init_devices(winraw_mouse_t **mice, unsigned *mouse_cnt)
{
   UINT i;
   POINT crs_pos;
   winraw_mouse_t *mice_r   = NULL;
   unsigned mouse_cnt_r     = 0;
   RAWINPUTDEVICELIST *devs = NULL;
   UINT dev_cnt             = 0;
   UINT r                   = GetRawInputDeviceList(
         NULL, &dev_cnt, sizeof(RAWINPUTDEVICELIST));

   if (r == (UINT)-1)
   {
      RARCH_ERR("[WINRAW]: GetRawInputDeviceList failed with error %lu.\n", GetLastError());
      goto error;
   }

   devs = (RAWINPUTDEVICELIST*)malloc(dev_cnt * sizeof(RAWINPUTDEVICELIST));
   if (!devs)
      goto error;

   dev_cnt = GetRawInputDeviceList(devs, &dev_cnt, sizeof(RAWINPUTDEVICELIST));
   if (dev_cnt == (UINT)-1)
   {
      RARCH_ERR("[WINRAW]: GetRawInputDeviceList failed with error %lu.\n", GetLastError());
      goto error;
   }

   for (i = 0; i < dev_cnt; ++i)
      mouse_cnt_r += devs[i].dwType == RIM_TYPEMOUSE ? 1 : 0;

   if (mouse_cnt_r)
   {
      mice_r = (winraw_mouse_t*)calloc(1, mouse_cnt_r * sizeof(winraw_mouse_t));
      if (!mice_r)
         goto error;

      if (!GetCursorPos(&crs_pos))
         goto error;

      for (i = 0; i < mouse_cnt_r; ++i)
      {
         mice_r[i].x = crs_pos.x;
         mice_r[i].y = crs_pos.y;
      }
   }

   /* count is already checked, so this is safe */
   for (i = mouse_cnt_r = 0; i < dev_cnt; ++i)
   {
      if (devs[i].dwType == RIM_TYPEMOUSE)
         mice_r[mouse_cnt_r++].hnd = devs[i].hDevice;
   }

   winraw_log_mice_info(mice_r, mouse_cnt_r);

   *mice      = mice_r;
   *mouse_cnt = mouse_cnt_r;

   return true;

error:
   free(devs);
   free(mice_r);
   *mice = NULL;
   *mouse_cnt = 0;
   return false;
}

static bool winraw_set_mouse_input(HWND window, bool grab)
{
   RAWINPUTDEVICE rid;
   BOOL r;

   if (window)
      rid.dwFlags  = grab ? RIDEV_CAPTUREMOUSE : 0;
   else
      rid.dwFlags  = RIDEV_REMOVE;

   rid.hwndTarget  = window;
   rid.usUsagePage = 0x01; /* generic desktop */
   rid.usUsage     = 0x02; /* mouse */

   r               = RegisterRawInputDevices(&rid, 1, sizeof(RAWINPUTDEVICE));

   if (!r)
   {
      RARCH_ERR("[WINRAW]: RegisterRawInputDevice failed with error %lu.\n", GetLastError());
      return false;
   }

   return true;
}

static int16_t winraw_mouse_state(winraw_input_t *wr,
      unsigned port, bool abs, unsigned id)
{
   unsigned i;
   settings_t *settings  = config_get_ptr();
   winraw_mouse_t *mouse = NULL;

   if (port >= MAX_USERS)
      return 0;

   for (i = 0; i < g_mouse_cnt; ++i)
   {
      if (i == settings->uints.input_mouse_index[port])
      {
         mouse = &wr->mice[i];
         break;
      }
   }

   if (!mouse)
      return 0;

   switch (id)
   {
      case RETRO_DEVICE_ID_MOUSE_X:
         return abs ? mouse->x : mouse->dlt_x;
      case RETRO_DEVICE_ID_MOUSE_Y:
         return abs ? mouse->y : mouse->dlt_y;
      case RETRO_DEVICE_ID_MOUSE_LEFT:
         return mouse->btn_l ? 1 : 0;
      case RETRO_DEVICE_ID_MOUSE_RIGHT:
         return mouse->btn_r ? 1 : 0;
      case RETRO_DEVICE_ID_MOUSE_WHEELUP:
         return mouse->whl_u ? 1 : 0;
      case RETRO_DEVICE_ID_MOUSE_WHEELDOWN:
         return mouse->whl_d ? 1 : 0;
      case RETRO_DEVICE_ID_MOUSE_MIDDLE:
         return mouse->btn_m ? 1 : 0;
   }

   return 0;
}

static int16_t winraw_joypad_state(winraw_input_t *wr,
      rarch_joypad_info_t joypad_info,
      const struct retro_keybind *binds,
      unsigned port, unsigned id)
{
   const struct retro_keybind *bind = &binds[id];
   unsigned key = rarch_keysym_lut[(enum retro_key)bind->key];

   if (!wr->kbd_mapp_block && (bind->key < RETROK_LAST) && wr->keyboard.keys[key])
      return 1;

   return input_joypad_pressed(wr->joypad, joypad_info, port, binds, id);
}

static void winraw_init_mouse_xy_mapping()
{
   struct video_viewport viewport;
   int center_x;
   int center_y;
   unsigned i;

   if (video_driver_get_viewport_info(&viewport))
   {
      center_x = viewport.x + viewport.width / 2;
      center_y = viewport.y + viewport.height / 2;

      for (i = 0; i < g_mouse_cnt; ++i)
      {
         g_mice[i].x = center_x;
         g_mice[i].y = center_y;
      }

      g_view_abs_ratio_x = (double)viewport.full_width / 65535.0;
      g_view_abs_ratio_y = (double)viewport.full_height / 65535.0;

      g_mouse_xy_mapping_ready = true;
   }
}

static void winraw_update_mouse_state(winraw_mouse_t *mouse, RAWMOUSE *state)
{
   POINT crs_pos;

   if (state->usFlags & MOUSE_MOVE_ABSOLUTE)
   {
      if (g_mouse_xy_mapping_ready)
      {
         state->lLastX = (LONG)(g_view_abs_ratio_x * state->lLastX);
         state->lLastY = (LONG)(g_view_abs_ratio_y * state->lLastY);
         InterlockedExchangeAdd(&mouse->dlt_x, state->lLastX - mouse->x);
         InterlockedExchangeAdd(&mouse->dlt_y, state->lLastY - mouse->y);
         mouse->x = state->lLastX;
         mouse->y = state->lLastY;
      }
      else
         winraw_init_mouse_xy_mapping();
   }
   else if (state->lLastX || state->lLastY)
   {
      InterlockedExchangeAdd(&mouse->dlt_x, state->lLastX);
      InterlockedExchangeAdd(&mouse->dlt_y, state->lLastY);

      if (!GetCursorPos(&crs_pos))
      {
         RARCH_WARN("[WINRAW]: GetCursorPos failed with error %lu.\n", GetLastError());
      }
      else if (!ScreenToClient((HWND)video_driver_window_get(), &crs_pos))
      {
         RARCH_WARN("[WINRAW]: ScreenToClient failed with error %lu.\n", GetLastError());
      }
      else
      {
         mouse->x = crs_pos.x;
         mouse->y = crs_pos.y;
      }
   }

   if (state->usButtonFlags & RI_MOUSE_LEFT_BUTTON_DOWN)
      mouse->btn_l = true;
   else if (state->usButtonFlags & RI_MOUSE_LEFT_BUTTON_UP)
      mouse->btn_l = false;

   if (state->usButtonFlags & RI_MOUSE_MIDDLE_BUTTON_DOWN)
      mouse->btn_m = true;
   else if (state->usButtonFlags & RI_MOUSE_MIDDLE_BUTTON_UP)
      mouse->btn_m = false;

   if (state->usButtonFlags & RI_MOUSE_RIGHT_BUTTON_DOWN)
      mouse->btn_r = true;
   else if (state->usButtonFlags & RI_MOUSE_RIGHT_BUTTON_UP)
      mouse->btn_r = false;

   if (state->usButtonFlags & RI_MOUSE_WHEEL)
   {
      if ((SHORT)state->usButtonData > 0)
         InterlockedExchange(&mouse->whl_u, 1);
      else if ((SHORT)state->usButtonData < 0)
         InterlockedExchange(&mouse->whl_d, 1);
   }
}

static LRESULT CALLBACK winraw_callback(HWND wnd, UINT msg, WPARAM wpar, LPARAM lpar)
{
   static uint8_t data[1024];
   UINT r;
   unsigned i;
   RAWINPUT *ri = (RAWINPUT*)data;
   UINT size    = sizeof(data);

   if (msg != WM_INPUT)
      return DefWindowProcA(wnd, msg, wpar, lpar);

   /* app is in the background */
   if (GET_RAWINPUT_CODE_WPARAM(wpar) != RIM_INPUT) 
      goto end;

   r = GetRawInputData((HRAWINPUT)lpar, RID_INPUT,
         data, &size, sizeof(RAWINPUTHEADER));
   if (r == (UINT)-1)
   {
      RARCH_WARN("[WINRAW]: GetRawInputData failed with error %lu.\n",
            GetLastError());
      goto end;
   }

   if (ri->header.dwType == RIM_TYPEKEYBOARD)
   {
      if (ri->data.keyboard.Message == WM_KEYDOWN)
         g_keyboard->keys[ri->data.keyboard.VKey] = 1;
      else if (ri->data.keyboard.Message == WM_KEYUP)
         g_keyboard->keys[ri->data.keyboard.VKey] = 0;
   }
   else if (ri->header.dwType == RIM_TYPEMOUSE)
   {
      for (i = 0; i < g_mouse_cnt; ++i)
      {
         if (g_mice[i].hnd == ri->header.hDevice)
         {
            winraw_update_mouse_state(&g_mice[i], &ri->data.mouse);
            break;
         }
      }
   }

end:
   DefWindowProcA(wnd, msg, wpar, lpar);
   return 0;
}

static void *winraw_init(const char *joypad_driver)
{
   bool r;
   winraw_input_t *wr = (winraw_input_t *)
      calloc(1, sizeof(winraw_input_t));
   g_keyboard         = (winraw_keyboard_t*)
      calloc(1, sizeof(winraw_keyboard_t));

   if (!wr || !g_keyboard)
      goto error;

   RARCH_LOG("[WINRAW]: Initializing input driver... \n");

   input_keymaps_init_keyboard_lut(rarch_key_map_winraw);

   wr->window = winraw_create_window(winraw_callback);
   if (!wr->window)
      goto error;

   r = winraw_init_devices(&g_mice, &g_mouse_cnt);
   if (!r)
      goto error;

   if (!g_mouse_cnt)
   {
      RARCH_LOG("[WINRAW]: Mouse unavailable.\n");
   }
   else
   {
      wr->mice = (winraw_mouse_t*)
         malloc(g_mouse_cnt * sizeof(winraw_mouse_t));
      if (!wr->mice)
         goto error;

      memcpy(wr->mice, g_mice, g_mouse_cnt * sizeof(winraw_mouse_t));
   }

   r = winraw_set_keyboard_input(wr->window);
   if (!r)
      goto error;

   r = winraw_set_mouse_input(wr->window, false);
   if (!r)
      goto error;

   wr->joypad = input_joypad_init_driver(joypad_driver, wr);

   return wr;

error:
   if (wr && wr->window)
   {
      winraw_set_mouse_input(NULL, false);
      winraw_set_keyboard_input(NULL);
      winraw_destroy_window(wr->window);
   }
   free(g_keyboard);
   free(g_mice);
   if (wr)
      free(wr->mice);
   free(wr);
   return NULL;
}

static void winraw_poll(void *d)
{
   unsigned i;
   winraw_input_t *wr = (winraw_input_t*)d;

   memcpy(&wr->keyboard, g_keyboard, sizeof(winraw_keyboard_t));

   /* following keys are not handled by windows raw input api */
   wr->keyboard.keys[VK_LCONTROL] = GetAsyncKeyState(VK_LCONTROL) >> 1 ? 1 : 0;
   wr->keyboard.keys[VK_RCONTROL] = GetAsyncKeyState(VK_RCONTROL) >> 1 ? 1 : 0;
   wr->keyboard.keys[VK_LMENU]    = GetAsyncKeyState(VK_LMENU)    >> 1 ? 1 : 0;
   wr->keyboard.keys[VK_RMENU]    = GetAsyncKeyState(VK_RMENU)    >> 1 ? 1 : 0;
   wr->keyboard.keys[VK_LSHIFT]   = GetAsyncKeyState(VK_LSHIFT)   >> 1 ? 1 : 0;
   wr->keyboard.keys[VK_RSHIFT]   = GetAsyncKeyState(VK_RSHIFT)   >> 1 ? 1 : 0;

   for (i = 0; i < g_mouse_cnt; ++i)
   {
      wr->mice[i].x     = g_mice[i].x;
      wr->mice[i].y     = g_mice[i].y;
      wr->mice[i].dlt_x = InterlockedExchange(&g_mice[i].dlt_x, 0);
      wr->mice[i].dlt_y = InterlockedExchange(&g_mice[i].dlt_y, 0);
      wr->mice[i].whl_u = InterlockedExchange(&g_mice[i].whl_u, 0);
      wr->mice[i].whl_d = InterlockedExchange(&g_mice[i].whl_d, 0);
      wr->mice[i].btn_l = g_mice[i].btn_l;
      wr->mice[i].btn_m = g_mice[i].btn_m;
      wr->mice[i].btn_r = g_mice[i].btn_r;
   }

   if (wr->joypad)
      wr->joypad->poll();
}

static int16_t winraw_input_state(void *d,
      rarch_joypad_info_t joypad_info,
      const struct retro_keybind **binds,
      unsigned port, unsigned device, unsigned index, unsigned id)
{
   winraw_input_t *wr = (winraw_input_t*)d;

   switch (device)
   {
      case RETRO_DEVICE_KEYBOARD:
         if (id < RETROK_LAST)
         {
            unsigned key = rarch_keysym_lut[(enum retro_key)id];
            return wr->keyboard.keys[key];
         }
         break;
      case RETRO_DEVICE_MOUSE:
         return winraw_mouse_state(wr, port, false, id);
      case RARCH_DEVICE_MOUSE_SCREEN:
         return winraw_mouse_state(wr, port, true, id);
      case RETRO_DEVICE_JOYPAD:
         return winraw_joypad_state(wr, joypad_info, binds[port], port, id);
      case RETRO_DEVICE_ANALOG:
         if (binds[port])
            return input_joypad_analog(wr->joypad, joypad_info,
                  port, index, id, binds[port]);
   }

   return 0;
}

static bool winraw_meta_key_pressed(void *u1, int u2)
{
   return false;
}

static void winraw_free(void *d)
{
   winraw_input_t *wr = (winraw_input_t*)d;

   if (wr->joypad)
      wr->joypad->destroy();
   winraw_set_mouse_input(NULL, false);
   winraw_set_keyboard_input(NULL);
   winraw_destroy_window(wr->window);
   free(g_mice);
   free(g_keyboard);
   free(wr->mice);
   free(wr);

   g_mouse_xy_mapping_ready = false;
}

static uint64_t winraw_get_capabilities(void *u)
{
   return (1 << RETRO_DEVICE_KEYBOARD) |
          (1 << RETRO_DEVICE_MOUSE) |
          (1 << RETRO_DEVICE_JOYPAD) |
          (1 << RETRO_DEVICE_ANALOG);
}

static void winraw_grab_mouse(void *d, bool grab)
{
   bool r             = false;
   winraw_input_t *wr = (winraw_input_t*)d;

   if (grab == wr->mouse_grab)
      return;

   r = winraw_set_mouse_input(wr->window, grab);
   if (!r)
      return;

   wr->mouse_grab = grab;
}

static bool winraw_set_rumble(void *d, unsigned port,
      enum retro_rumble_effect effect, uint16_t strength)
{
   winraw_input_t *wr = (winraw_input_t*)d;

   return input_joypad_set_rumble(wr->joypad, port, effect, strength);
}

static const input_device_driver_t *winraw_get_joypad_driver(void *d)
{
   winraw_input_t *wr = (winraw_input_t*)d;

   return wr->joypad;
}

static bool winraw_keyboard_mapping_is_blocked(void *d)
{
   winraw_input_t *wr = (winraw_input_t*)d;

   return wr->kbd_mapp_block;
}

static void winraw_keyboard_mapping_set_block(void *d, bool block)
{
   winraw_input_t *wr = (winraw_input_t*)d;

   wr->kbd_mapp_block = block;
}

input_driver_t input_winraw = {
   winraw_init,
   winraw_poll,
   winraw_input_state,
   winraw_meta_key_pressed,
   winraw_free,
   NULL,
   NULL,
   winraw_get_capabilities,
   "raw",
   winraw_grab_mouse,
   NULL,
   winraw_set_rumble,
   winraw_get_joypad_driver,
   NULL,
   winraw_keyboard_mapping_is_blocked,
   winraw_keyboard_mapping_set_block,
};
