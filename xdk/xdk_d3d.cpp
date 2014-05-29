﻿/*  RetroArch - A frontend for libretro.
 *  Copyright (C) 2010-2014 - Hans-Kristian Arntzen
 *  Copyright (C) 2011-2014 - Daniel De Matteis
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

#ifdef _XBOX
#include <xtl.h>
#include <xgraphics.h>
#endif

#include "../driver.h"
#include "xdk_d3d.h"

#ifdef HAVE_HLSL
#include "../gfx/shader_hlsl.h"
#endif

#include "./../gfx/gfx_context.h"
#include "../general.h"
#include "../message_queue.h"

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "../xdk/xdk_resources.h"

static bool d3d_init_shader(void *data)
{
   d3d_video_t *d3d = (d3d_video_t*)data;
   const gl_shader_backend_t *backend = NULL;

   const char *shader_path = g_settings.video.shader_path;
   enum rarch_shader_type type = gfx_shader_parse_type(shader_path, DEFAULT_SHADER_TYPE);
   
   switch (type)
   {
      case RARCH_SHADER_HLSL:
#ifdef HAVE_HLSL
         RARCH_LOG("[D3D]: Using HLSL shader backend.\n");
         backend = &hlsl_backend;
#endif
         break;
   }

   if (!backend)
   {
      RARCH_ERR("[GL]: Didn't find valid shader backend. Continuing without shaders.\n");
      return true;
   }


   d3d->shader = backend;
   return d3d->shader->init(d3d, shader_path);
}

static void d3d_deinit_shader(void *data)
{
   d3d_video_t *d3d = (d3d_video_t*)data;

   if (d3d->shader && d3d->shader->deinit)
      d3d->shader->deinit();
   d3d->shader = NULL;
}

static void d3d_deinit_chain(void *data)
{
   d3d_video_t *d3d = (d3d_video_t*)data;
   if (d3d->tex)
      d3d->tex->Release();
   d3d->tex = NULL;

   if (d3d->vertex_buf)
      d3d->vertex_buf->Release();
   d3d->vertex_buf = NULL;

#ifndef _XBOX1
   if (d3d->v_decl)
      d3d->v_decl->Release();
   d3d->v_decl = NULL;
#endif
}

static void d3d_deinitialize(void *data)
{
   d3d_video_t *d3d = (d3d_video_t*)data;

   if (d3d->font_ctx && d3d->font_ctx->deinit)
      d3d->font_ctx->deinit(d3d);
   d3d_deinit_chain(d3d);
#ifdef HAVE_SHADERS
   d3d_deinit_shader(d3d);
#endif

   d3d->needs_restore = false;
}


static void d3d_free(void *data)
{
   d3d_video_t *d3d = (d3d_video_t*)data;
   d3d_deinitialize(d3d);
#ifdef HAVE_OVERLAY
   //d3d_free_overlays(d3d);
#endif
#ifdef HAVE_MENU
   //d3d_free_overlay(d3d, d3d->rgui);
#endif
   if (d3d->dev)
      d3d->dev->Release();
   if (d3d->g_pD3D)
      d3d->g_pD3D->Release();

#ifdef HAVE_MONITOR
   Monitor::last_hm = MonitorFromWindow(d3d->hWnd, MONITOR_DEFAULTTONEAREST);
   DestroyWindow(d3d->hWnd);
#endif

   if (d3d)
      free(d3d);

#ifndef _XBOX
   UnregisterClass("RetroArch", GetModuleHandle(NULL));
#endif
}

static void d3d_set_viewport(void *data, int x, int y, unsigned width, unsigned height)
{
   d3d_video_t *d3d = (d3d_video_t*)data;
   D3DVIEWPORT viewport;

   // D3D doesn't support negative X/Y viewports ...
   if (x < 0)
      x = 0;
   if (y < 0)
      y = 0;

   viewport.Width  = width;
   viewport.Height = height;
   viewport.X      = x;
   viewport.Y      = y;
   viewport.MinZ   = 0.0f;
   viewport.MaxZ   = 1.0f;
   d3d->final_viewport = viewport;

   //d3d_set_font_rect(d3d, NULL);
}

static void d3d_calculate_rect(void *data, unsigned width, unsigned height,
   bool keep, float desired_aspect)
{
   d3d_video_t *d3d = (d3d_video_t*)data;
   LPDIRECT3DDEVICE d3dr = d3d->dev;

   if (g_settings.video.scale_integer)
   {
      struct rarch_viewport vp = {0};
      gfx_scale_integer(&vp, width, height, desired_aspect, keep);
      d3d_set_viewport(d3d, vp.x, vp.y, vp.width, vp.height);
   }
   else if (!keep)
      d3d_set_viewport(d3d, 0, 0, width, height);
   else
   {
      if (g_settings.video.aspect_ratio_idx == ASPECT_RATIO_CUSTOM)
      {
         const rarch_viewport_t &custom = g_extern.console.screen.viewports.custom_vp;
         d3d_set_viewport(d3d, custom.x, custom.y, custom.width, custom.height);
      }
      else
      {
         float device_aspect = static_cast<float>(width) / static_cast<float>(height);
         if (fabsf(device_aspect - desired_aspect) < 0.0001f)
            d3d_set_viewport(d3d, 0, 0, width, height);
         else if (device_aspect > desired_aspect)
         {
            float delta = (desired_aspect / device_aspect - 1.0f) / 2.0f + 0.5f;
            d3d_set_viewport(d3d, int(roundf(width * (0.5f - delta))), 0, unsigned(roundf(2.0f * width * delta)), height);
         }
         else
         {
            float delta = (device_aspect / desired_aspect - 1.0f) / 2.0f + 0.5f;
            d3d_set_viewport(d3d, 0, int(roundf(height * (0.5f - delta))), width, unsigned(roundf(2.0f * height * delta)));
         }
      }
   }
}

static void d3d_set_rotation(void *data, unsigned rot)
{
   d3d_video_t *d3d = (d3d_video_t*)data;
   d3d->dev_rotation = rot;
}

static void renderchain_set_final_viewport(void *data, const D3DVIEWPORT *final_viewport)
{
   //renderchain_t *chain = (renderchain_t*)data;
   d3d_video_t *chain = (d3d_video_t*)data;
   chain->final_viewport = *final_viewport;
}

static _inline void renderchain_set_viewport(void *data, D3DVIEWPORT *vp)
{
   d3d_video_t *chain = (d3d_video_t*)data;
   LPDIRECT3DDEVICE d3dr = (LPDIRECT3DDEVICE)chain->dev;
   RD3DDevice_SetViewport(d3dr, vp);
}

static void renderchain_set_mvp(void *data,
      unsigned vp_width, unsigned vp_height,
      unsigned rotation)
{
   d3d_video_t *d3d = (d3d_video_t*)data;
   LPDIRECT3DDEVICE d3dr = (LPDIRECT3DDEVICE)d3d->dev;
#if defined(_XBOX360) && defined(HAVE_SHADERS)
   hlsl_set_proj_matrix(XMMatrixRotationZ(rotation * (M_PI / 2.0)));
   if (d3d->shader && d3d->shader->set_mvp)
      d3d->shader->set_mvp(d3d, NULL);
#elif defined(_XBOX1)
   D3DXMATRIX p_out, ortho, rot;
   D3DXMatrixOrthoOffCenterLH(&ortho, 0, vp_width,  vp_height, 0, 0.0f, 1.0f);
   D3DXMatrixIdentity(&p_out);

   if (rotation)
      D3DXMatrixRotationZ(&rot, rotation * (M_PI / 2.0));

   RD3DDevice_SetTransform(d3dr, D3DTS_WORLD, &rot);
   RD3DDevice_SetTransform(d3dr, D3DTS_VIEW, &p_out);
   RD3DDevice_SetTransform(d3dr, D3DTS_PROJECTION, &p_out);
#endif
}

static bool d3d_set_shader(void *data, enum rarch_shader_type type, const char *path)
{
   /* TODO - stub */
   d3d_video_t *d3d = (d3d_video_t*)data;

   switch (type)
   {
      case RARCH_SHADER_CG:
#ifdef HAVE_HLSL
         d3d->shader = &hlsl_backend;
         break;
#endif
      default:
         d3d->shader = NULL;
         break;
   }

   if (!d3d->shader)
   {
      RARCH_ERR("[D3D]: Cannot find shader core for path: %s.\n", path);
      return false;
   }

   return true;
}

static bool renderchain_add_pass(void *data, const video_info_t *info)
{
   HRESULT ret;
   d3d_video_t *d3d = (d3d_video_t*)data;
   D3DVIEWPORT vp = {0};
   LPDIRECT3DDEVICE d3dr = (LPDIRECT3DDEVICE)d3d->dev;

   d3d->last_width  = 0;
   d3d->last_height = 0;

   ret = d3dr->CreateVertexBuffer(
               4 * sizeof(DrawVerticeFormats), 
               D3DUSAGE_WRITEONLY,
               D3DFVF_CUSTOMVERTEX,
               D3DPOOL_MANAGED,
               &d3d->vertex_buf
#ifndef _XBOX1
               ,NULL
#endif
               );
   if (FAILED(ret))
      return false;

   ret = d3dr->CreateTexture(d3d->tex_w, d3d->tex_h, 1,
               0,
               info->rgb32 ? D3DFMT_LIN_X8R8G8B8 : D3DFMT_LIN_R5G6B5,
               D3DPOOL_DEFAULT,
               &d3d->tex
#ifndef _XBOX1
               ,NULL
#endif
               );
   if (FAILED(ret))
      return false;

   d3dr->SetTexture(0, d3d->tex);
   RD3DDevice_SetSamplerState_AddressU(d3dr, D3DSAMP_ADDRESSU, D3DTADDRESS_BORDER);
   RD3DDevice_SetSamplerState_AddressV(d3dr, D3DSAMP_ADDRESSV, D3DTADDRESS_BORDER);
   d3dr->SetTexture(0, NULL);

   vp.Width  = d3d->screen_width;
   vp.Height = d3d->screen_height;

   vp.MinZ   = 0.0f;
   vp.MaxZ   = 1.0f;
   RD3DDevice_SetViewport(d3dr, &vp);

   if (g_extern.console.screen.viewports.custom_vp.width == 0)
      g_extern.console.screen.viewports.custom_vp.width = vp.Width;

   if (g_extern.console.screen.viewports.custom_vp.height == 0)
      g_extern.console.screen.viewports.custom_vp.height = vp.Height;

   return true;
}

static const gfx_ctx_driver_t *d3d_get_context(void *data)
{
   d3d_video_t *d3d = (d3d_video_t*)data;
   enum gfx_ctx_api api;
   unsigned major, minor;
#if defined(_XBOX1)
   api = GFX_CTX_DIRECT3D8_API;
   major = 8;
#elif defined(_XBOX360)
   api = GFX_CTX_DIRECT3D9_API;
   major = 9;
#endif
   minor = 0;
   return gfx_ctx_init_first(d3d, api, major, minor, false);
}

static bool d3d_init_base(void *data, const video_info_t *info)
{
   d3d_video_t *d3d = (d3d_video_t*)data;
   D3DPRESENT_PARAMETERS d3dpp;
   d3d_make_d3dpp(d3d, info, &d3dpp);

   d3d->g_pD3D = direct3d_create_ctx(D3D_SDK_VERSION);
   if (!d3d->g_pD3D)
   {
      RARCH_ERR("Failed to create D3D interface.\n");
      return false;
   }

   RARCH_LOG("d3d is NULL: %d\n", d3d == NULL);
   RARCH_LOG("d3d g_pD3D is NULL: %d\n", d3d->g_pD3D == NULL);
   RARCH_LOG("d3d->dev is NULL: %d\n", d3d->dev == NULL);

   if (FAILED(d3d->d3d_err = d3d->g_pD3D->CreateDevice(
            d3d->cur_mon_id,
            D3DDEVTYPE_HAL,
            d3d->hWnd,
            D3DCREATE_HARDWARE_VERTEXPROCESSING,
            &d3dpp,
            &d3d->dev)))
   {
      RARCH_WARN("[D3D]: Failed to init device with hardware vertex processing (code: 0x%x). Trying to fall back to software vertex processing.\n",
                 (unsigned)d3d->d3d_err);

      if (FAILED(d3d->d3d_err = d3d->g_pD3D->CreateDevice(
                  d3d->cur_mon_id,
                  D3DDEVTYPE_HAL,
                  d3d->hWnd,
                  D3DCREATE_SOFTWARE_VERTEXPROCESSING,
                  &d3dpp,
                  &d3d->dev)))
      {
         RARCH_ERR("Failed to initialize device.\n");
         return false;
      }
   }


   return true;
}

bool renderchain_init(void *data, const video_info_t *video_info,
      LPDIRECT3DDEVICE dev_,
      /*CGcontext cgCtx_,*/
      const D3DVIEWPORT *final_viewport_
      /*, const LinkInfo *info, PixelFormat fmt*/)
{
   //TODO/FIXME - horribly incomplete right now
#if 0
   renderchain_t *chain = (renderchain_t*)data;
#else
   d3d_video_t *chain = (d3d_video_t*)data;
#endif

   if (!chain)
      return false;

   // TODO - give chain its own state
   chain->pixel_size   = video_info->rgb32 ? 4 : 2;

   return true;
}

static bool d3d_init_chain(void *data, const video_info_t *video_info)
{
   d3d_video_t *d3d = (d3d_video_t*)data;
   LPDIRECT3DDEVICE d3dr = d3d->dev;

   //TODO - change to link_info
   d3d->tex_w = d3d->tex_h = video_info->input_scale * RARCH_SCALE_BASE;

   d3d_deinit_chain(d3d);
   //TODO - new renderchain

   if (!renderchain_init(d3d/*->chain*/, &d3d->video_info, d3dr, /*d3d->cgCtx,*/ &d3d->final_viewport/*&link_info, d3d->video_info.rgb32 ? ARGB : RGB565*/))
   {
      RARCH_ERR("[D3D]: Failed to init render chain.\n");
      return false;
   }

   //TODO - add for loop with passes
   //TODO - d3d should become d3d->chain and video_info should become &link_info
   if (!renderchain_add_pass(d3d, video_info))
   {
      RARCH_ERR("[D3D]: Failed to add pass.\n");
      return false;
   }

#if defined(_XBOX1)
   const DrawVerticeFormats init_verts[] = {
      { -1.0f, -1.0f, 1.0f, 0.0f, 1.0f },
      {  1.0f, -1.0f, 1.0f, 1.0f, 1.0f },
      { -1.0f,  1.0f, 1.0f, 0.0f, 0.0f },
      {  1.0f,  1.0f, 1.0f, 1.0f, 0.0f },
   };

   BYTE *verts_ptr;
#elif defined(_XBOX360)
   static const DrawVerticeFormats init_verts[] = {
      { -1.0f, -1.0f, 0.0f, 1.0f },
      {  1.0f, -1.0f, 1.0f, 1.0f },
      { -1.0f,  1.0f, 0.0f, 0.0f },
      {  1.0f,  1.0f, 1.0f, 0.0f },
   };

   void *verts_ptr;
#endif

   d3d->vertex_buf->Lock(0, 0, &verts_ptr, 0);
   memcpy(verts_ptr, init_verts, sizeof(init_verts));
   d3d->vertex_buf->Unlock();

#if defined(_XBOX1)
   d3d->dev->SetVertexShader(D3DFVF_XYZ | D3DFVF_TEX1);
#elif defined(_XBOX360)
   static const D3DVERTEXELEMENT VertexElements[] =
   {
      { 0, 0 * sizeof(float), D3DDECLTYPE_FLOAT2, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_POSITION, 0 },
      { 0, 2 * sizeof(float), D3DDECLTYPE_FLOAT2, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_TEXCOORD, 0 },
      D3DDECL_END()
   };

   d3dr->CreateVertexDeclaration(VertexElements, &d3d->v_decl);
#endif

   return true;
}

static bool d3d_initialize(void *data, const video_info_t *info)
{
   d3d_video_t *d3d = (d3d_video_t*)data;
   bool ret = true;

   if (!d3d->g_pD3D)
      ret = d3d_init_base(d3d, info);
   else if (d3d->needs_restore)
   {
      D3DPRESENT_PARAMETERS d3dpp;
      d3d_make_d3dpp(d3d, info, &d3dpp);
      if (d3d->dev->Reset(&d3dpp) != D3D_OK)
      {
#ifndef _XBOX
         HRESULT res = d3d->dev->TestCooperativeLevel();
         const char *err;
         switch (res)
         {
            case D3DERR_DEVICELOST:
               err = "DEVICELOST";
               break;
            case D3DERR_DEVICENOTRESET:
               err = "DEVICENOTRESET";
               break;
            case D3DERR_DRIVERINTERNALERROR:
               err = "DRIVERINTERNALERROR";
               break;
            default:
               err = "Unknown";
         }
         // Try to recreate the device completely ...
         RARCH_WARN("[D3D]: Attempting to recover from dead state (%s).\n", err);
#endif
         d3d_deinitialize(d3d); 
         d3d->g_pD3D->Release();
         d3d->g_pD3D = NULL;
         ret = d3d_init_base(d3d, info);
         if (ret)
            RARCH_LOG("[D3D]: Recovered from dead state.\n");
         else
            return ret;
      }
   }

   if (!ret)
      return ret;

   d3d_calculate_rect(d3d, d3d->screen_width, d3d->screen_height, info->force_aspect,g_extern.system.aspect_ratio);

#ifdef HAVE_SHADERS
   if (!d3d_init_shader(d3d))
   {
      RARCH_ERR("Failed to initialize HLSL.\n");
      return false;
   }
#endif

   if (!d3d_init_chain(d3d, info))
   {
      RARCH_ERR("Failed to initialize render chain.\n");
      return false;
   }

#if defined(_XBOX360)
   strlcpy(g_settings.video.font_path, "game:\\media\\Arial_12.xpr", sizeof(g_settings.video.font_path));
#endif
   d3d->font_ctx = d3d_font_init_first(d3d, g_settings.video.font_path, 0);
   if (!d3d->font_ctx)
   {
      RARCH_ERR("Failed to initialize font.\n");
      return false;
   }

   return true;
}

bool d3d_restore(void *data)
{
   d3d_video_t *d3d = (d3d_video_t*)data;
   d3d_deinitialize(d3d);
   d3d->needs_restore = !d3d_initialize(d3d, &d3d->video_info);

   if (d3d->needs_restore)
      RARCH_ERR("[D3D]: Restore error.\n");

   return !d3d->needs_restore;
}

// Delay constructor due to lack of exceptions.
static bool d3d_construct(void *data, const video_info_t *info, const input_driver_t **input,
                          void **input_data)
{
   d3d_video_t *d3d = (d3d_video_t*)data;
   d3d->should_resize = false;
#ifndef _XBOX
   gfx_set_dwm();
#endif

#ifdef HAVE_WINDOW
   memset(&d3d->windowClass, 0, sizeof(d3d->windowClass));
   d3d->windowClass.cbSize        = sizeof(d3d->windowClass);
   d3d->windowClass.style         = CS_HREDRAW | CS_VREDRAW;
   d3d->windowClass.lpfnWndProc   = WindowProc;
   d3d->windowClass.hInstance     = NULL;
   d3d->windowClass.hCursor       = LoadCursor(NULL, IDC_ARROW);
   d3d->windowClass.lpszClassName = "RetroArch";
   d3d->windowClass.hIcon = LoadIcon(GetModuleHandle(NULL), MAKEINTRESOURCE(IDI_ICON));
   d3d->windowClass.hIconSm = (HICON)LoadImage(GetModuleHandle(NULL), MAKEINTRESOURCE(IDI_ICON), IMAGE_ICON, 16, 16, 0);
   if (!info->fullscreen)
      d3d->windowClass.hbrBackground = (HBRUSH)COLOR_WINDOW;

   RegisterClassEx(&d3d->windowClass);
#endif

#ifdef HAVE_MONITOR
   RECT mon_rect = d3d_monitor_rect(d3d);

   bool windowed_full = g_settings.video.windowed_fullscreen;

   unsigned full_x = (windowed_full || info->width  == 0) ? (mon_rect.right  - mon_rect.left) : info->width;
   unsigned full_y = (windowed_full || info->height == 0) ? (mon_rect.bottom - mon_rect.top)  : info->height;
   RARCH_LOG("[D3D]: Monitor size: %dx%d.\n", (int)(mon_rect.right  - mon_rect.left), (int)(mon_rect.bottom - mon_rect.top));

   d3d->screen_width  = info->fullscreen ? full_x : info->width;
   d3d->screen_height = info->fullscreen ? full_y : info->height;
#else
   unsigned full_x, full_y;

   if (d3d->ctx_driver && d3d->ctx_driver->get_video_size)
      d3d->ctx_driver->get_video_size(d3d, &full_x, &full_y);

   d3d->screen_width  = info->fullscreen ? full_x : info->width;
   d3d->screen_height = info->fullscreen ? full_y : info->height;
#endif

   unsigned win_width  = d3d->screen_width;
   unsigned win_height = d3d->screen_height;

#ifdef HAVE_WINDOW
   if (!info->fullscreen)
   {
      RECT rect   = {0};
      rect.right  = d3d->screen_width;
      rect.bottom = d3d->screen_height;
      AdjustWindowRect(&rect, WS_OVERLAPPEDWINDOW, FALSE);
      win_width  = rect.right - rect.left;
      win_height = rect.bottom - rect.top;
   }

   char buffer[128];
   gfx_get_fps(buffer, sizeof(buffer), NULL, 0);
   std::string title = buffer;
   title += " || Direct3D";

   d3d->hWnd = CreateWindowEx(0, "RetroArch", title.c_str(),
         info->fullscreen ?
         (WS_EX_TOPMOST | WS_POPUP) : WS_OVERLAPPEDWINDOW,
         info->fullscreen ? mon_rect.left : CW_USEDEFAULT,
         info->fullscreen ? mon_rect.top  : CW_USEDEFAULT,
         win_width, win_height,
         NULL, NULL, NULL, d3d);
#endif

#ifdef HAVE_WINDOW
   ShowWindow(d3d->hWnd, SW_RESTORE);
   UpdateWindow(d3d->hWnd);
   SetForegroundWindow(d3d->hWnd);
   SetFocus(d3d->hWnd);
#endif

   d3d->video_info = *info;
   if (!d3d_initialize(data, &d3d->video_info))
      return false;

   if (input && input_data &&
      d3d->ctx_driver && d3d->ctx_driver->input_driver)
      d3d->ctx_driver->input_driver(d3d, input, input_data);

   RARCH_LOG("[D3D]: Init complete.\n");
   return true;
}

static void *d3d_init(const video_info_t *info, const input_driver_t **input, void **input_data)
{
   d3d_video_t *vid = new d3d_video_t();
   if (!vid)
      return NULL;

   vid->ctx_driver = d3d_get_context(vid);
   if (!vid->ctx_driver)
   {
      free(vid);
      return NULL;
   }

   //default values
   vid->g_pD3D               = NULL;
   vid->dev                  = NULL;
   vid->dev_rotation         = 0;
   vid->needs_restore        = false;
#ifdef HAVE_CG
   vid->cgCtx                = NULL;
#endif
   vid->should_resize        = false;
   vid->vsync                = info->vsync;
   //vid->chain              = NULL;

   if (!d3d_construct(vid, info, input, input_data))
   {
      RARCH_ERR("[D3D]: Failed to init D3D.\n");
      delete vid;
      return NULL;
   }

   return vid;
}

#ifdef HAVE_RMENU
extern struct texture_image *menu_texture;
#endif

#ifdef _XBOX1
static bool texture_image_render(void *data, struct texture_image *out_img,
                          int x, int y, int w, int h, bool force_fullscreen)
{
   d3d_video_t *d3d = (d3d_video_t*)data;
   LPDIRECT3DDEVICE d3dr = (LPDIRECT3DDEVICE)d3d->dev;

   if (out_img->pixels == NULL || out_img->vertex_buf == NULL)
      return false;

   float fX = static_cast<float>(x);
   float fY = static_cast<float>(y);

   // create the new vertices
   DrawVerticeFormats newVerts[] =
   {
      // x,           y,              z,     color, u ,v
      {fX,            fY,             0.0f,  0,     0, 0},
      {fX + w,        fY,             0.0f,  0,     1, 0},
      {fX + w,        fY + h,         0.0f,  0,     1, 1},
      {fX,            fY + h,         0.0f,  0,     0, 1}
   };

   // load the existing vertices
   DrawVerticeFormats *pCurVerts;

   HRESULT ret = out_img->vertex_buf->Lock(0, 0, (unsigned char**)&pCurVerts, 0);

   if (FAILED(ret))
   {
      RARCH_ERR("Error occurred during m_pVertexBuffer->Lock().\n");
      return false;
   }

   // copy the new verts over the old verts
   memcpy(pCurVerts, newVerts, 4 * sizeof(DrawVerticeFormats));
   out_img->vertex_buf->Unlock();

   d3d->dev->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
   d3d->dev->SetRenderState(D3DRS_SRCBLEND,  D3DBLEND_SRCALPHA);
   d3d->dev->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA);

   // also blend the texture with the set alpha value
   d3d->dev->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_MODULATE);
   d3d->dev->SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_DIFFUSE);
   d3d->dev->SetTextureStageState(0, D3DTSS_ALPHAARG2, D3DTA_TEXTURE);

   // draw the quad
   d3dr->SetTexture(0, out_img->pixels);
   d3dr->SetStreamSource(0, out_img->vertex_buf, sizeof(DrawVerticeFormats));
   d3dr->SetVertexShader(D3DFVF_CUSTOMVERTEX);

   if (force_fullscreen)
   {
      D3DVIEWPORT vp = {0};
      vp.Width  = w;
      vp.Height = h;
      vp.X      = 0;
      vp.Y      = 0;
      vp.MinZ   = 0.0f;
      vp.MaxZ   = 1.0f;
      d3dr->SetViewport(&vp);
   }
   d3dr->DrawPrimitive(D3DPT_QUADLIST, 0, 1);

   return true;
}
#endif

#ifdef HAVE_MENU
static void d3d_draw_texture(void *data)
{
   d3d_video_t *d3d = (d3d_video_t*)data;
   menu_texture->x = 0;
   menu_texture->y = 0;

   if (d3d->rgui_texture_enable)
   {
      d3d->dev->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_ONE);
      d3d->dev->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA);
      d3d->dev->SetRenderState(D3DRS_ALPHABLENDENABLE, true);
      texture_image_render(d3d, menu_texture, menu_texture->x, menu_texture->y,
         d3d->screen_width, d3d->screen_height, true);
      d3d->dev->SetRenderState(D3DRS_ALPHABLENDENABLE, false);
   }
}
#endif

static void renderchain_clear_texture(void *data)
{
   d3d_video_t *d3d = (d3d_video_t*)data;
   LPDIRECT3DDEVICE d3dr = d3d->dev;
   D3DLOCKED_RECT d3dlr;

   D3DTexture_LockRect(d3d->tex, 0, &d3dlr, NULL, D3DLOCK_NOSYSLOCK);
   memset(d3dlr.pBits, 0, d3d->tex_w * d3dlr.Pitch);
}

static void renderchain_blit_to_texture(void *data, const void *frame,
   unsigned width, unsigned height,
   unsigned pitch)
{
   d3d_video_t *d3d = (d3d_video_t*)data;

   if (d3d->last_width != width || d3d->last_height != height)
      renderchain_clear_texture(data);

   D3DLOCKED_RECT d3dlr;
   D3DTexture_LockRect(d3d->tex, 0, &d3dlr, NULL, D3DLOCK_NOSYSLOCK);

#if defined(_XBOX360)
   D3DSURFACE_DESC desc;
   d3d->tex->GetLevelDesc(0, &desc);
      XGCopySurface(d3dlr.pBits, d3dlr.Pitch, width, height, desc.Format, NULL, frame,
                                        pitch, desc.Format, NULL, 0, 0);
#elif defined(_XBOX1)
   for (unsigned y = 0; y < height; y++)
   {
      const uint8_t *in = (const uint8_t*)frame + y * pitch;
      uint8_t *out = (uint8_t*)d3dlr.pBits + y * d3dlr.Pitch;
      memcpy(out, in, width * d3d->pixel_size);
   }
#endif
}

static void renderchain_set_shader_params(void *data, unsigned pass/*Pass &pass*/,
            unsigned video_w, unsigned video_h,
            unsigned tex_w, unsigned tex_h,
            unsigned viewport_w, unsigned viewport_h)
{
#ifdef _XBOX360
   d3d_video_t *d3d = (d3d_video_t*)data;
   if (!d3d || !d3d->shader)
      return;

   if (d3d->shader->use)
      d3d->shader->use(d3d, pass);
   if (d3d->shader->set_params)
      d3d->shader->set_params(d3d, video_w, video_h, tex_w, tex_h, viewport_w,
            viewport_h, g_extern.frame_count,
            NULL, NULL, NULL, 0);
#endif
}

static void renderchain_set_vertices(void *data, unsigned pass,
      unsigned width, unsigned height)
{
   d3d_video_t *d3d = (d3d_video_t*)data;

   if (d3d->last_width != width || d3d->last_height != height)
   {
      d3d->last_width = width;
      d3d->last_height = height;

#if defined(_XBOX1)
      float tex_w = width;
      float tex_h = height;

      DrawVerticeFormats verts[] = {
         { -1.0f, -1.0f, 1.0f, 0.0f,  tex_h },
         {  1.0f, -1.0f, 1.0f, tex_w, tex_h },
         { -1.0f,  1.0f, 1.0f, 0.0f,  0.0f },
         {  1.0f,  1.0f, 1.0f, tex_w, 0.0f },
      };
#elif defined(_XBOX360)
      float tex_w = width / ((float)d3d->tex_w);
      float tex_h = height / ((float)d3d->tex_h);

      DrawVerticeFormats verts[] = {
         { -1.0f, -1.0f, 0.0f,  tex_h },
         {  1.0f, -1.0f, tex_w, tex_h },
         { -1.0f,  1.0f, 0.0f,  0.0f },
         {  1.0f,  1.0f, tex_w, 0.0f },
      };
#endif

      // Align texels and vertices.
      for (unsigned i = 0; i < 4; i++)
      {
         verts[i].x -= 0.5f / ((float)d3d->tex_w);
         verts[i].y += 0.5f / ((float)d3d->tex_h);
      }

#if defined(_XBOX1)
      BYTE *verts_ptr;
#elif defined(_XBOX360)
      void *verts_ptr;
#endif
      RD3DVertexBuffer_Lock(d3d->vertex_buf, 0, 0, &verts_ptr, 0);
      memcpy(verts_ptr, verts, sizeof(verts));
      RD3DVertexBuffer_Unlock(d3d->vertex_buf);
   }

   renderchain_set_mvp(d3d, d3d->screen_width, d3d->screen_height, d3d->dev_rotation);
   renderchain_set_shader_params(d3d, pass, width, height, d3d->tex_w, d3d->tex_h, d3d->screen_width,
         d3d->screen_height);
}

static void renderchain_start_render(void *data)
{
#if 0
   renderchain_t *chain = (renderchain_t*)data;

   chain->passes[0].tex         = chain->prev.tex[chain->prev.ptr];
   chain->passes[0].vertex_buf  = chain->prev.vertex_buf[chain->prev.ptr];
   chain->passes[0].last_width  = chain->prev.last_width[chain->prev.ptr];
   chain->passes[0].last_height = chain->prev.last_height[chain->prev.ptr];
#else
   d3d_video_t *chain = (d3d_video_t*)data;
#endif
}

void renderchain_end_render(void *data)
{
#if 0
   renderchain_t *chain = (renderchain_t*)data;
   chain->prev.last_width[chain->prev.ptr]  = chain->passes[0].last_width;
   chain->prev.last_height[chain->prev.ptr] = chain->passes[0].last_height;
   chain->prev.ptr                          = (chain->prev.ptr + 1) & TEXTURESMASK;
#else
   d3d_video_t *chain = (d3d_video_t*)data;
#endif
}

static void renderchain_render_pass(void *data, const void *frame, unsigned width, unsigned height,
                        unsigned pitch, unsigned rotation)
{

   d3d_video_t *chain = (d3d_video_t*)data;
   LPDIRECT3DDEVICE d3dr = (LPDIRECT3DDEVICE)chain->dev;

#ifdef _XBOX
   if (g_extern.frame_count)
   {
#ifdef _XBOX1
      d3dr->SwitchTexture(0, chain->tex);
#elif defined _XBOX360
      d3dr->SetTextureFetchConstant(0, chain->tex);
#endif
   }
   else
#endif
      if (chain->tex) { RD3DDevice_SetTexture(d3dr, 0, chain->tex); }
   // TODO - use translate_filter on last param - and create pass->filter
   RD3DDevice_SetSamplerState_MinFilter(d3dr, 0, g_settings.video.smooth ? D3DTEXF_LINEAR : D3DTEXF_POINT);
   RD3DDevice_SetSamplerState_MagFilter(d3dr, 0, g_settings.video.smooth ? D3DTEXF_LINEAR : D3DTEXF_POINT);

   //TODO - move outside
   RD3DDevice_SetSamplerState_AddressU(d3dr, D3DSAMP_ADDRESSU, D3DTADDRESS_BORDER);
   RD3DDevice_SetSamplerState_AddressV(d3dr, D3DSAMP_ADDRESSV, D3DTADDRESS_BORDER);

#if defined(_XBOX1)
   RD3DDevice_SetVertexShader(d3dr, D3DFVF_XYZ | D3DFVF_TEX1);
   IDirect3DDevice8_SetStreamSource(d3dr, 0, chain->vertex_buf, sizeof(DrawVerticeFormats));
#elif defined(_XBOX360)
   D3DDevice_SetVertexDeclaration(d3dr, d3d->v_decl);
   D3DDevice_SetStreamSource_Inline(d3dr, 0, chain->vertex_buf, 0, sizeof(DrawVerticeFormats));
#endif

#ifdef _XBOX
   d3dr->DrawPrimitive(D3DPT_TRIANGLESTRIP, 0, 2);
#else
   if (SUCCEEDED(d3dr->BeginScene()))
   {
      d3dr->DrawPrimitive(D3DPT_TRIANGLESTRIP, 0, 2);
      d3dr->EndScene();
   }
#endif
}

static bool renderchain_render(void *chain_data, const void *data,
                               unsigned width, unsigned height, unsigned pitch, unsigned rotation)
{
#ifdef _XBOX360
   DWORD fetchConstant;
   UINT64 pendingMask3;
#endif
   d3d_video_t *chain = (d3d_video_t*)chain_data;
   LPDIRECT3DDEVICE d3dr = (LPDIRECT3DDEVICE)chain->dev;
   renderchain_start_render(chain);

   unsigned current_width = width;
   unsigned current_height = height;
   unsigned out_width = 0;
   unsigned out_height = 0;
   //renderchain_convert_geometry(chain, &chain->passes[0].info, out_width, out_height, current_width, current_height, chain->final_viewport);
#ifdef _XBOX1
   d3dr->SetFlickerFilter(g_extern.console.screen.flicker_filter_index);
   d3dr->SetSoftDisplayFilter(g_extern.lifecycle_state & (1ULL << MODE_VIDEO_SOFT_FILTER_ENABLE));
#endif
   renderchain_blit_to_texture(chain, data, width, height, pitch);

   // TODO - for loop going over passes

   // Final pass
   renderchain_set_viewport(chain, &chain->final_viewport);
   renderchain_set_vertices(chain, /*last_pass*/1, width, height);
   renderchain_render_pass(chain, data, current_width, current_height, pitch, rotation);

   // TODO - replace with chain->frame_count
   g_extern.frame_count++;

   renderchain_end_render(chain);
   renderchain_set_shader_params(chain, /*pass*/1, width, height, chain->tex_w, chain->tex_h, chain->screen_width, chain->screen_height);
   renderchain_set_mvp(chain, chain->final_viewport.Width, chain->final_viewport.Height, chain->dev_rotation);
   return true;
}

static bool d3d_frame(void *data, const void *frame,
      unsigned width, unsigned height, unsigned pitch, const char *msg)
{
   D3DVIEWPORT screen_vp;
   d3d_video_t *d3d = (d3d_video_t*)data;
   LPDIRECT3DDEVICE d3dr = (LPDIRECT3DDEVICE)d3d->dev;

   if (!frame)
      return true;

   RARCH_PERFORMANCE_INIT(d3d_frame);
   RARCH_PERFORMANCE_START(d3d_frame);

#ifndef _XBOX
   // We cannot recover in fullscreen.
   if (d3d->needs_restore && IsIconic(d3d->hWnd))
      return true;
#endif

   if (d3d->needs_restore && !d3d_restore(d3d))
   {
      RARCH_ERR("[D3D]: Failed to restore.\n");
      return false;
   }

   if (d3d && d3d->should_resize)
   {
      d3d_calculate_rect(d3d, d3d->screen_width, d3d->screen_height, d3d->video_info.force_aspect, g_extern.system.aspect_ratio);
      renderchain_set_final_viewport(d3d/*->chain*/, &d3d->final_viewport);
      //d3d_recompute_pass_sizes(d3d);
      
      d3d->should_resize = false;
   }

   // render_chain() only clears out viewport, clear out everything
   screen_vp.X = 0;
   screen_vp.Y = 0;
   screen_vp.MinZ = 0;
   screen_vp.MaxZ = 1;
   screen_vp.Width = d3d->screen_width;
   screen_vp.Height = d3d->screen_height;
   d3dr->SetViewport(&screen_vp);
   d3dr->Clear(0, 0, D3DCLEAR_TARGET, 0, 1, 0);

   // Insert black frame first, so we can screenshot, etc.
   if (g_settings.video.black_frame_insertion)
   {
      if (d3dr->Present(NULL, NULL, NULL, NULL) != D3D_OK)
      {
         RARCH_ERR("[D3D]: Present() failed.\n");
         d3d->needs_restore = true;
         return true;
      }
      d3dr->Clear(0, 0, D3DCLEAR_TARGET, 0, 1, 0);
   }

   if (!renderchain_render(d3d, frame, width, height, pitch, d3d->dev_rotation))
   {
      RARCH_ERR("[D3D]: Failed to render scene.\n");
      return false;
   }

#ifdef HAVE_MENU
   if (g_extern.lifecycle_state & (1ULL << MODE_MENU) && driver.menu_ctx && driver.menu_ctx->frame)
      driver.menu_ctx->frame(d3d);

   if (d3d && d3d->rgui_texture_enable)
      d3d_draw_texture(d3d);
#endif


   if (d3d->font_ctx && d3d->font_ctx->render_msg && msg)
   {
      font_params_t font_parms = {0};
#ifdef _XBOX
#if defined(_XBOX1)
      float msg_width  = 60;
      float msg_height = 365;
#elif defined(_XBOX360)
      float msg_width  = (g_extern.lifecycle_state & (1ULL << MODE_MENU_HD)) ? 160 : 100;
      float msg_height = 90;
#endif
      font_parms.x = msg_width;
      font_parms.y = msg_height;
      font_parms.scale = 21;
#endif
      d3d->font_ctx->render_msg(d3d, msg, &font_parms);
   }

   RARCH_PERFORMANCE_STOP(d3d_frame);

   if (d3d && d3d->ctx_driver && d3d->ctx_driver->update_window_title)
      d3d->ctx_driver->update_window_title(d3d);

   if (d3d && d3d->ctx_driver && d3d->ctx_driver->swap_buffers)
      d3d->ctx_driver->swap_buffers(d3d);

   return true;
}

static void d3d_set_nonblock_state(void *data, bool state)
{
   d3d_video_t *d3d = (d3d_video_t*)data;
   d3d->video_info.vsync = !state;

   if (d3d->ctx_driver && d3d->ctx_driver->swap_interval)
      d3d->ctx_driver->swap_interval(d3d, state ? 0 : 1);
   d3d_restore(d3d);
}

static bool d3d_alive(void *data)
{
   d3d_video_t *d3d = (d3d_video_t*)data;
   bool quit, resize;

   quit = false;
   resize = false;

   if (d3d->ctx_driver && d3d->ctx_driver->check_window)
      d3d->ctx_driver->check_window(d3d, &quit, &resize, &d3d->screen_width,
      &d3d->screen_height, g_extern.frame_count);

#ifdef _XBOX
   // TODO - see if this can apply for PC as well
   if (quit && d3d)
      d3d->quitting = quit;
#endif
   else if (resize)
      d3d->should_resize = true;

   return !d3d->quitting;
}

static bool d3d_focus(void *data)
{
   d3d_video_t *d3d = (d3d_video_t*)data;
   if (d3d && d3d->ctx_driver && d3d->ctx_driver->has_focus)
      return d3d->ctx_driver->has_focus(d3d);
   return false;
}

static void d3d_set_aspect_ratio(void *data, unsigned aspect_ratio_idx)
{
   d3d_video_t *d3d = (d3d_video_t*)data;

   switch (aspect_ratio_idx)
   {
      case ASPECT_RATIO_SQUARE:
         gfx_set_square_pixel_viewport(g_extern.system.av_info.geometry.base_width, g_extern.system.av_info.geometry.base_height);
         break;

      case ASPECT_RATIO_CORE:
         gfx_set_core_viewport();
         break;

      case ASPECT_RATIO_CONFIG:
         gfx_set_config_viewport();
         break;

      default:
         break;
   }

   g_extern.system.aspect_ratio  = aspectratio_lut[aspect_ratio_idx].value;
   d3d->video_info.force_aspect = true;
   d3d->should_resize = true;
}

static void d3d_apply_state_changes(void *data)
{
   d3d_video_t *d3d = (d3d_video_t*)data;
   d3d->should_resize = true;
}

static void d3d_set_filtering(void *data, unsigned index, bool set_smooth) { }

#ifdef HAVE_MENU
static void d3d_set_texture_frame(void *data,
   const void *frame, bool rgb32, unsigned width, unsigned height,
   float alpha)
{
   (void)frame;
   (void)rgb32;
   (void)width;
   (void)height;
   (void)alpha;
}

static void d3d_set_texture_enable(void *data, bool state, bool full_screen)
{
   d3d_video_t *d3d = (d3d_video_t*)data;

   if (d3d)
   {
      d3d->rgui_texture_enable = state;
      d3d->rgui_texture_full_screen = full_screen;
   }
}
#endif

static void d3d_set_osd_msg(void *data, const char *msg, void *userdata)
{
   d3d_video_t *d3d = (d3d_video_t*)data;
   font_params_t *params = (font_params_t*)userdata;

   if (d3d && d3d->font_ctx && d3d->font_ctx->render_msg)
      d3d->font_ctx->render_msg(d3d, msg, params);
}

static const video_poke_interface_t d3d_poke_interface = {
   d3d_set_filtering,
#ifdef HAVE_FBO
   NULL,
   NULL,
#endif
   d3d_set_aspect_ratio,
   d3d_apply_state_changes,
#ifdef HAVE_MENU
   d3d_set_texture_frame,
   d3d_set_texture_enable,
#endif
   d3d_set_osd_msg,
};

static void d3d_get_poke_interface(void *data, const video_poke_interface_t **iface)
{
   (void)data;
   *iface = &d3d_poke_interface;
}

static void d3d_restart(void)
{
   d3d_video_t *d3d = (d3d_video_t*)driver.video_data;
   LPDIRECT3DDEVICE d3dr = d3d->dev;

   if (!d3d)
      return;

   D3DPRESENT_PARAMETERS d3dpp;
   video_info_t video_info = {0};

   video_info.vsync = g_settings.video.vsync;
   video_info.force_aspect = false;
   video_info.smooth = g_settings.video.smooth;
   video_info.input_scale = 2;
   video_info.fullscreen = true;
   video_info.rgb32 = (d3d->pixel_size == sizeof(uint32_t)) ? true : false;
   d3d_make_d3dpp(d3d, &video_info, &d3dpp);

   d3dr->Reset(&d3dpp);
}

const video_driver_t video_d3d = {
   d3d_init,
   d3d_frame,
   d3d_set_nonblock_state,
   d3d_alive,
   d3d_focus,
   d3d_set_shader,
   d3d_free,
   "d3d",
   d3d_restart,
   d3d_set_rotation,
   NULL, /* viewport_info */
   NULL, /* read_viewport */
#ifdef HAVE_OVERLAY
   NULL, /* overlay_interface */
#endif
   d3d_get_poke_interface,
};
