/*
 * Copyright (c) 2020 Philippe Normand
 *
 * This file is part of FFmpeg.
 *
 * FFmpeg is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * FFmpeg is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with FFmpeg; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */

#include "vf_wpeview.h"

#include <cstdio>
#include <mutex>
#include <stdint.h>
#include <wayland-server.h>
#include <wpe/unstable/fdo-shm.h>

class GMutexHolder {
public:
    GMutexHolder(GMutex& mutex)
        : m(mutex)
    {
        g_mutex_lock(&m);
    }
    ~GMutexHolder()
    {
        g_mutex_unlock(&m);
    }

private:
    GMutex& m;
};

WPEThreadedView::WPEThreadedView()
{
    g_mutex_init(&threading.mutex);
    g_cond_init(&threading.cond);
    g_mutex_init(&threading.ready_mutex);
    g_cond_init(&threading.ready_cond);

    g_mutex_init(&images_mutex);

    {
        GMutexHolder lock(threading.mutex);
        threading.thread = g_thread_new("WPEThreadedView",
            s_viewThread, this);
        g_cond_wait(&threading.cond, &threading.mutex);
    }
}

WPEThreadedView::~WPEThreadedView()
{
    {
        GMutexHolder lock(threading.mutex);
        wpe_view_backend_exportable_fdo_destroy(wpe.exportable);
    }

    if (threading.thread) {
        g_thread_unref(threading.thread);
        threading.thread = nullptr;
    }

    g_mutex_clear(&threading.mutex);
    g_cond_clear(&threading.cond);
    g_mutex_clear(&threading.ready_mutex);
    g_cond_clear(&threading.ready_cond);
    g_mutex_clear(&images_mutex);
}

gpointer WPEThreadedView::s_viewThread(gpointer data)
{
    auto& view = *static_cast<WPEThreadedView*>(data);

    view.glib.context = g_main_context_new();
    view.glib.loop = g_main_loop_new(view.glib.context, FALSE);

    g_main_context_push_thread_default(view.glib.context);

    {
        GSource* source = g_idle_source_new();
        g_source_set_callback(source,
            [](gpointer data) -> gboolean {
                auto& view = *static_cast<WPEThreadedView*>(data);
                GMutexHolder lock(view.threading.mutex);
                g_cond_signal(&view.threading.cond);
                return G_SOURCE_REMOVE;
            },
            &view, nullptr);
        g_source_attach(source, view.glib.context);
        g_source_unref(source);
    }

    g_main_loop_run(view.glib.loop);

    g_main_loop_unref(view.glib.loop);
    view.glib.loop = nullptr;

    if (view.webkit.view) {
        g_object_unref(view.webkit.view);
        view.webkit.view = nullptr;
    }
    if (view.webkit.uri) {
        g_free(view.webkit.uri);
        view.webkit.uri = nullptr;
    }

    g_main_context_pop_thread_default(view.glib.context);
    g_main_context_unref(view.glib.context);
    view.glib.context = nullptr;
    return nullptr;
}

void WPEThreadedView::s_loadEvent(WebKitWebView*, WebKitLoadEvent event, gpointer data)
{
    if (event == WEBKIT_LOAD_COMMITTED) {
        auto& view = *static_cast<WPEThreadedView*>(data);
        GMutexHolder lock(view.threading.ready_mutex);
        g_cond_signal(&view.threading.ready_cond);
    }
}

bool WPEThreadedView::initialize(int width, int height)
{
    static std::once_flag s_loaderFlag;
    std::call_once(s_loaderFlag, [] {
        wpe_loader_init("libWPEBackend-fdo-1.0.so");
    });

    struct InitializeContext {
        WPEThreadedView& view;
        int width;
        int height;
        bool result;
    } initializeContext { *this, width, height, FALSE };

    GSource* source = g_idle_source_new();
    g_source_set_callback(source,
        [](gpointer data) -> gboolean {
            auto& initializeContext = *static_cast<InitializeContext*>(data);
            auto& view = initializeContext.view;

            GMutexHolder lock(view.threading.mutex);

            view.wpe.width = initializeContext.width;
            view.wpe.height = initializeContext.height;
            initializeContext.result = wpe_fdo_initialize_shm();

            if (!initializeContext.result) {
              g_cond_signal(&view.threading.cond);
              return G_SOURCE_REMOVE;
            }

            view.wpe.exportable = wpe_view_backend_exportable_fdo_create(&s_exportableClient,
                &view, view.wpe.width, view.wpe.height);
            auto* wpeViewBackend = wpe_view_backend_exportable_fdo_get_view_backend(view.wpe.exportable);
            auto* viewBackend = webkit_web_view_backend_new(wpeViewBackend, nullptr, nullptr);
#if defined(WPE_BACKEND_CHECK_VERSION) && WPE_BACKEND_CHECK_VERSION(1, 1, 0)
            wpe_view_backend_add_activity_state(wpeViewBackend, wpe_view_activity_state_visible | wpe_view_activity_state_focused | wpe_view_activity_state_in_window);
#endif

            view.webkit.view = WEBKIT_WEB_VIEW(g_object_new(WEBKIT_TYPE_WEB_VIEW,
                "backend", viewBackend, nullptr));

            g_signal_connect(view.webkit.view, "load-changed", G_CALLBACK(s_loadEvent), &view);
            g_cond_signal(&view.threading.cond);
            return G_SOURCE_REMOVE;
        },
        &initializeContext, nullptr);
    g_source_set_priority(source, G_PRIORITY_DEFAULT);

    {
        GMutexHolder lock(threading.mutex);
        g_source_attach(source, glib.context);
        g_cond_wait(&threading.cond, &threading.mutex);
    }

    g_source_unref(source);

    if (initializeContext.result && webkit.uri) {
        GMutexHolder lock(threading.ready_mutex);
        g_cond_wait(&threading.ready_cond, &threading.ready_mutex);
    }
    return initializeContext.result;
}

void WPEThreadedView::setDrawBackground(gboolean drawsBackground) {
  WebKitColor color;
  webkit_color_parse(&color, drawsBackground ? "white" : "transparent");
  webkit_web_view_set_background_color(webkit.view, &color);
}

void WPEThreadedView::resize(int width, int height)
{
    wpe.width = width;
    wpe.height = height;

    GSource* source = g_idle_source_new();
    g_source_set_callback(source,
        [](gpointer data) -> gboolean {
            auto& view = *static_cast<WPEThreadedView*>(data);
            GMutexHolder lock(view.threading.mutex);

            if (view.wpe.exportable && wpe_view_backend_exportable_fdo_get_view_backend(view.wpe.exportable))
                wpe_view_backend_dispatch_set_size(wpe_view_backend_exportable_fdo_get_view_backend(view.wpe.exportable), view.wpe.width, view.wpe.height);

            g_cond_signal(&view.threading.cond);
            return G_SOURCE_REMOVE;
        },
        this, nullptr);
    g_source_set_priority(source, G_PRIORITY_DEFAULT);

    {
        GMutexHolder lock(threading.mutex);
        g_source_attach(source, glib.context);
        g_cond_wait(&threading.cond, &threading.mutex);
    }

    g_source_unref(source);
}

void WPEThreadedView::frameComplete()
{
    GSource* source = g_idle_source_new();
    g_source_set_callback(source,
        [](gpointer data) -> gboolean {
            auto& view = *static_cast<WPEThreadedView*>(data);
            GMutexHolder lock(view.threading.mutex);

            wpe_view_backend_exportable_fdo_dispatch_frame_complete(view.wpe.exportable);

            g_cond_signal(&view.threading.cond);
            return G_SOURCE_REMOVE;
        },
        this, nullptr);
    g_source_set_priority(source, G_PRIORITY_DEFAULT);

    {
        GMutexHolder lock(threading.mutex);
        g_source_attach(source, glib.context);
        g_cond_wait(&threading.cond, &threading.mutex);
    }

    g_source_unref(source);
}

void WPEThreadedView::loadUriUnlocked(const gchar* uri)
{
    if (webkit.uri)
        g_free(webkit.uri);

    webkit.uri = g_strdup_printf("https://%s", uri);
    webkit_web_view_load_uri(webkit.view, webkit.uri);
}

void WPEThreadedView::loadUri(const gchar* uri)
{
    struct UriContext {
        WPEThreadedView& view;
        const gchar* uri;
    } uriContext { *this, uri };

    GSource* source = g_idle_source_new();
    g_source_set_callback(source,
        [](gpointer data) -> gboolean {
            auto& uriContext = *static_cast<UriContext*>(data);
            auto& view = uriContext.view;
            GMutexHolder lock(view.threading.mutex);

            view.loadUriUnlocked(uriContext.uri);

            g_cond_signal(&view.threading.cond);
            return G_SOURCE_REMOVE;
        },
        &uriContext, nullptr);
    g_source_set_priority(source, G_PRIORITY_DEFAULT);

    {
        GMutexHolder lock(threading.mutex);
        g_source_attach(source, glib.context);
        g_cond_wait(&threading.cond, &threading.mutex);
    }

    g_source_unref(source);
}

void WPEThreadedView::fillFrame(AVFrame* frame)
{
  bool dispatchFrameComplete = false;

  {
    GMutexHolder lock(images_mutex);

    if (shm.pending) {
      auto* previousImage = shm.committed;
      shm.committed = shm.pending;
      shm.pending = nullptr;

      if (previousImage)
        releaseSHMBuffer(previousImage);
      dispatchFrameComplete = true;
    }

    if (shm.committed) {
      struct wl_shm_buffer *shmBuffer =
          wpe_fdo_shm_exported_buffer_get_shm_buffer(shm.committed);
      int32_t width = wl_shm_buffer_get_width(shmBuffer);
      int32_t height = wl_shm_buffer_get_height(shmBuffer);
      gint stride = wl_shm_buffer_get_stride(shmBuffer);
      auto *data = static_cast<uint8_t *>(wl_shm_buffer_get_data(shmBuffer));
      int32_t x, y;
      const int linesize = frame->linesize[0];
      uint8_t *line = frame->data[0];

      for (y = 0; y < height; y++) {
        uint8_t *dst = line;

        for (x = 0; x < width; x++) {
          *dst++ = data[stride * y + 4 * x + 0];
          *dst++ = data[stride * y + 4 * x + 1];
          *dst++ = data[stride * y + 4 * x + 2];
          *dst++ = data[stride * y + 4 * x + 3];
        }
        line += linesize;
      }
    }
  }

  if (dispatchFrameComplete)
    frameComplete();
}

void WPEThreadedView::releaseSHMBuffer(struct wpe_fdo_shm_exported_buffer* buffer) {
  struct ReleaseBufferContext {
    WPEThreadedView &view;
    struct wpe_fdo_shm_exported_buffer *buffer;
  } releaseImageContext{*this, buffer};

  GSource *source = g_idle_source_new();
  g_source_set_callback(
      source,
      [](gpointer data) -> gboolean {
        auto &releaseBufferContext = *static_cast<ReleaseBufferContext *>(data);
        auto &view = releaseBufferContext.view;
        GMutexHolder lock(view.threading.mutex);

        struct wpe_fdo_shm_exported_buffer *buffer = releaseBufferContext.buffer;
        wpe_view_backend_exportable_fdo_dispatch_release_shm_exported_buffer(
            view.wpe.exportable, buffer);
        g_cond_signal(&view.threading.cond);
        return G_SOURCE_REMOVE;
      },
      &releaseImageContext, nullptr);
  g_source_set_priority(source, G_PRIORITY_DEFAULT);

  {
    GMutexHolder lock(threading.mutex);
    g_source_attach(source, glib.context);
    g_cond_wait(&threading.cond, &threading.mutex);
  }

  g_source_unref(source);
}

void WPEThreadedView::handleExportedBuffer(struct wpe_fdo_shm_exported_buffer* buffer)
{
    struct wl_shm_buffer* shmBuffer = wpe_fdo_shm_exported_buffer_get_shm_buffer(buffer);
    auto format = wl_shm_buffer_get_format(shmBuffer);
    if (format != WL_SHM_FORMAT_ARGB8888 && format != WL_SHM_FORMAT_XRGB8888) {
        return;
    }

    {
        GMutexHolder lock(images_mutex);
        shm.pending = buffer;
    }
}

struct wpe_view_backend_exportable_fdo_client WPEThreadedView::s_exportableClient = {
    nullptr,
    nullptr,
    // export_shm_buffer
    [](void* data, struct wpe_fdo_shm_exported_buffer* buffer) {
        auto& view = *static_cast<WPEThreadedView*>(data);
        view.handleExportedBuffer(buffer);
    },
    nullptr,
    nullptr,
};

#ifdef __cplusplus
extern "C" {
size_t sizeof_wpe_threaded_view()
{
  return sizeof(WPEThreadedView);
}

void *create_wpe_threaded_view(void *ptr, const char *uri, int width,
                               int height, int draw_background) {
  WPEThreadedView *self = new (ptr) WPEThreadedView();
  if (!self->initialize(width, height)) {
    self->~WPEThreadedView();
    return NULL;
  }

  self->setDrawBackground(draw_background);
  self->loadUri(uri);
  return self;
}

void wpe_threaded_view_fill_frame(void* ptr, AVFrame *frame)
{
  WPEThreadedView *self = reinterpret_cast<WPEThreadedView*>(ptr);
  self->fillFrame(frame);
}

void destroy_wpe_threaded_view(void *ptr)
{
  WPEThreadedView *self = reinterpret_cast<WPEThreadedView*>(ptr);
  self->~WPEThreadedView();
}

}
#endif
