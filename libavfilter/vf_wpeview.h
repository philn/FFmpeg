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

#ifndef VF_WPE_VIEW_H
#define VF_WPE_VIEW_H

#include <libavutil/frame.h>
#include <wpe/fdo.h>
#include <wpe/webkit.h>
#include <glib.h>

typedef struct _WPEContext WPEContext;

#ifdef __cplusplus
class WPEThreadedView {
public:
    WPEThreadedView();
    ~WPEThreadedView();

    bool initialize(int width, int height);
    void resize(int width, int height);
    void loadUri(const gchar*);
    void setDrawBackground(gboolean);
    void fillFrame(AVFrame*);

protected:
    void handleExportedBuffer(struct wpe_fdo_shm_exported_buffer*);

private:
    void frameComplete();
    void loadUriUnlocked(const gchar *);
    void releaseSHMBuffer(struct wpe_fdo_shm_exported_buffer *);

    static void s_loadEvent(WebKitWebView*, WebKitLoadEvent, gpointer);

    static gpointer s_viewThread(gpointer);
    struct {
        GMutex mutex;
        GCond cond;
        GMutex ready_mutex;
        GCond ready_cond;
        GThread* thread { nullptr };
    } threading;

    struct {
        GMainContext* context;
        GMainLoop* loop;
    } glib { nullptr, nullptr };

    static struct wpe_view_backend_exportable_fdo_client s_exportableClient;

    struct {
        struct wpe_view_backend_exportable_fdo* exportable;
        int width;
        int height;
    } wpe { nullptr, 0, 0 };

    struct {
        gchar* uri;
        WebKitWebView* view;
    } webkit = { nullptr, nullptr };

    // This mutex guards access to either egl or shm resources declared below,
    // depending on the runtime behavior.
    GMutex images_mutex;

    struct {
      struct wpe_fdo_shm_exported_buffer *pending;
      struct wpe_fdo_shm_exported_buffer *committed;
    } shm { nullptr, nullptr };
};
extern "C" {
#endif

size_t sizeof_wpe_threaded_view();
void *create_wpe_threaded_view(void* ptr, const char* uri, int width, int height, int draw_background);
void wpe_threaded_view_fill_frame(void* ptr, AVFrame* frame);
void destroy_wpe_threaded_view(void* ptr);

#ifdef __cplusplus
}
#endif

#endif /* VF_WPE_VIEW_H */
