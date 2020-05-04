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

#include "avfilter.h"
#include "internal.h"
#include "libavutil/imgutils.h"
#include "libavutil/opt.h"
#include "libavutil/pixdesc.h"
#include "vf_wpeview.h"
#include <libavutil/pixfmt.h>

typedef struct _WPEContext {
  const AVClass *class;
  void *view;
  int draw_background;
  char *uri;
  int w, h;
  int64_t pts;
} WPEContext;

#define OFFSET(x) offsetof(WPEContext, x)
#define FLAGS AV_OPT_FLAG_FILTERING_PARAM|AV_OPT_FLAG_VIDEO_PARAM
static const AVOption wpe_options[] = {{"uri",
                                        "Website URI to load",
                                        OFFSET(uri),
                                        AV_OPT_TYPE_STRING,
                                        {.str = NULL},
                                        0,
                                        0,
                                        FLAGS},
                                       {"draw-background",
                                        "draw opaque web-view background",
                                        OFFSET(draw_background),
                                        AV_OPT_TYPE_BOOL,
                                        {.i64 = 0},
                                        0,
                                        1,
                                        FLAGS},
                                       {NULL}};

AVFILTER_DEFINE_CLASS(wpe);

static int query_formats(AVFilterContext *ctx)
{
  static const enum AVPixelFormat pix_fmts[] = {AV_PIX_FMT_BGRA,
                                                AV_PIX_FMT_NONE};

  AVFilterFormats *fmts_list = ff_make_format_list(pix_fmts);
  if (!fmts_list)
    return AVERROR(ENOMEM);
  return ff_set_common_formats(ctx, fmts_list);
}

static int config_props_output(AVFilterLink *outlink)
{
  AVFilterContext *ctx = outlink->src;
  WPEContext *s = ctx->priv;

  outlink->w = s->w;
  outlink->h = s->h;
  return 0;
}

static av_cold int init(AVFilterContext *ctx) {
  WPEContext *context = ctx->priv;

  context->w = 1920;
  context->h = 1080;
  context->pts = 0;
  context->view = malloc(sizeof_wpe_threaded_view());
  create_wpe_threaded_view(context->view, context->uri, context->w, context->h, context->draw_background);
  if (context->view == NULL)
    return -1;

  return 0;
}

static av_cold void uninit(AVFilterContext *ctx)
{
  WPEContext *s = ctx->priv;
  destroy_wpe_threaded_view(s->view);
  free(s->view);
}

static int request_frame(AVFilterLink *outlink)
{
    WPEContext *context = outlink->src->priv;
    AVFrame *frame = ff_get_video_buffer(outlink,  context->w, context->h);

    if (!frame)
        return AVERROR(ENOMEM);
    frame->pts                 = context->pts;
    frame->key_frame           = 1;
    frame->interlaced_frame    = 0;
    frame->pict_type           = AV_PICTURE_TYPE_I;

    wpe_threaded_view_fill_frame(context->view, frame);
    context->pts++;
    return ff_filter_frame(outlink, frame);
}

static const AVFilterPad wpe_outputs[] = {
    {
        .name = "default",
        .type = AVMEDIA_TYPE_VIDEO,
        .request_frame = request_frame,
        .config_props = config_props_output,
    },
    {NULL}};

AVFilter ff_vf_wpe = {
    .name          = "wpe",
    .description   = NULL_IF_CONFIG_SMALL("WPE input video fields into frames."),
    .priv_size     = sizeof(WPEContext),
    .priv_class    = &wpe_class,
    .query_formats = query_formats,
    .init          = init,
    .uninit        = uninit,
    .inputs        = NULL,
    .outputs       = wpe_outputs,
};
