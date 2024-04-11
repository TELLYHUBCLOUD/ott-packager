/*****************************************************************************
  Copyright (C) 2018-2024 John William

  This program is free software; you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation; either version 2 of the License, or
  (at your option) any later version.

  This program is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with this program; if not, write to the Free Software
  Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02111, USA.

  This program is also available with customization/support packages.
  For more information, please contact me at cannonbeachgoonie@gmail.com

******************************************************************************/

#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>
#include <string.h>
#include <semaphore.h>
#include <pthread.h>
#include <time.h>
#include <signal.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/poll.h>
#include <sys/time.h>
#include <math.h>
#include <syslog.h>
#include <error.h>
#include "fillet.h"
#include "dataqueue.h"
#include "transvideo.h"
#include "esignal.h"

#if defined(ENABLE_TRANSCODE)

#include "../cbffmpeg/libavcodec/avcodec.h"
#include "../cbffmpeg/libswscale/swscale.h"
#include "../cbffmpeg/libavutil/pixfmt.h"
#include "../cbffmpeg/libavutil/log.h"
#include "../cbffmpeg/libavutil/opt.h"
#include "../cbffmpeg/libavutil/imgutils.h"
#include "../cbffmpeg/libavformat/avformat.h"
#include "../cbffmpeg/libavfilter/buffersink.h"
#include "../cbffmpeg/libavfilter/buffersrc.h"

#if !defined(ENABLE_GPU)
#include "../cbx264/x264.h"
#include "../x265_3.0/source/x265.h"
#endif // ENABLE_GPU

#if defined(ENABLE_GPU)
//#define ENABLE_GPU_DECODE
#define GPU_BUFFERS  1
#define MAX_B_FRAMES 3
#endif

static volatile int video_decode_thread_running = 0;
static volatile int video_scale_thread_running = 0;
static volatile int video_monitor_thread_running = 0;
static volatile int video_prepare_thread_running = 0;
static volatile int video_encode_thread_running = 0;
static volatile int video_thumbnail_thread_running = 0;
static pthread_t video_decode_thread_id;
static pthread_t video_scale_thread_id;
static pthread_t video_monitor_thread_id;
static pthread_t video_prepare_thread_id;
static pthread_t video_encode_thread_id[MAX_TRANS_OUTPUTS];
static pthread_t video_thumbnail_thread_id;

#if !defined(ENABLE_GPU)
typedef struct _x264_encoder_struct_ {
    x264_t           *h;
    x264_nal_t       *nal;
    x264_param_t     param;
    x264_picture_t   pic;
    x264_picture_t   pic_out;
    int              i_nal;
    int              y_size;
    int              uv_size;
    int              width;
    int              height;
    int64_t          frame_count_pts;
    int64_t          frame_count_dts;
} x264_encoder_struct;

typedef struct _x265_encoder_struct_ {
    x265_stats       stats;
    x265_picture     pic_orig;
    x265_picture     pic_out;
    x265_param       *param;
    x265_picture     *pic_in;
    x265_picture     *pic_recon;
    x265_api         *api;
    x265_encoder     *encoder;
    x265_nal         *p_nal;
    x265_nal         *p_current_nal;
    int64_t          frame_count_pts;
    int64_t          frame_count_dts;
} x265_encoder_struct;
#endif // ENABLE_GPU

typedef struct _gpu_encoder_struct_ {
    int               initialized;
    AVCodecContext    *encode_avctx;
    const AVCodec           *encode_codec;
    AVPacket          *encode_pkt;
    AVFrame           *encode_av_frame;
    AVFrame           *encode_surface;
    AVBufferRef       *hw_device_ctx;
    AVHWFramesContext *hw_frames_ctx;
    int64_t           frame_count_pts;
    int64_t           frame_count_dts;
} gpu_encoder_struct;

typedef struct _scale_struct_ {
    uint8_t          *output_data[4];
    int              output_stride[4];
} scale_struct;

typedef struct _opaque_struct_ {
    uint8_t          *caption_buffer;
    int              caption_size;
    int64_t          caption_timestamp;
    int64_t          splice_duration;
    int              splice_point;
    int64_t          splice_duration_remaining;
} opaque_struct;

typedef struct _encoder_opaque_struct_ {
    int              splice_point;
    int64_t          splice_duration;
    int64_t          splice_duration_remaining;
    int64_t          frame_count_pts;
} encoder_opaque_struct;

typedef struct _signal_struct_ {
    int64_t          pts;
    int              scte35_ready;
    int64_t          scte35_duration;
    int64_t          scte35_duration_remaining;
} signal_struct;

typedef struct _thread_start_struct_ {
    fillet_app_struct   *core;
    int                 index;
} thread_start_struct;

#define THUMBNAIL_WIDTH   176
#define THUMBNAIL_HEIGHT  144
#define MAX_FILENAME_SIZE 256
#define MAX_MESSAGE_SIZE  256

int video_sink_frame_callback(fillet_app_struct *core, uint8_t *new_buffer, int sample_size, int64_t pts, int64_t dts, int source, int splice_point, int64_t splice_duration, int64_t splice_duration_remaining);

int save_frame_as_jpeg(fillet_app_struct *core, AVFrame *pFrame)
{
    const AVCodec *jpegCodec = avcodec_find_encoder(AV_CODEC_ID_MJPEG);
    AVCodecContext *jpegContext = avcodec_alloc_context3(jpegCodec);
    FILE *JPEG = NULL;

    char filename[MAX_FILENAME_SIZE];
    AVPacket *packet = NULL;
    int encodedFrame = 0;

    jpegContext->bit_rate = 250000;
    jpegContext->width = THUMBNAIL_WIDTH;
    jpegContext->height = THUMBNAIL_HEIGHT;
    jpegContext->time_base = (AVRational){1001,30000};
    jpegContext->framerate = (AVRational){30000,1001};
    jpegContext->pix_fmt = AV_PIX_FMT_YUVJ420P;

    avcodec_open2(jpegContext, jpegCodec, NULL);

    packet = av_packet_alloc();

    avcodec_send_frame(jpegContext, pFrame);
    avcodec_receive_packet(jpegContext, packet);

    snprintf(filename, MAX_FILENAME_SIZE-1, "/var/www/html/thumbnail%d.jpg", core->cd->identity);
    JPEG = fopen(filename, "wb");
    if (JPEG) {
        fwrite(packet->data, 1, packet->size, JPEG);
        fclose(JPEG);
    }
    av_packet_unref(packet);

    avcodec_free_context(&jpegContext);
    av_packet_free(&packet);

    return 0;
}

void *video_thumbnail_thread(void *context)
{
    fillet_app_struct *core = (fillet_app_struct*)context;
    dataqueue_message_struct *msg;

    while (video_thumbnail_thread_running) {
        msg = (dataqueue_message_struct*)dataqueue_take_back(core->encodevideo->thumbnail_queue);
        while (!msg && video_thumbnail_thread_running) {
            usleep(10000);
            msg = (dataqueue_message_struct*)dataqueue_take_back(core->encodevideo->thumbnail_queue);
        }

        scale_struct *thumbnail_output = (scale_struct*)msg->buffer;

        if (thumbnail_output) {
            AVFrame *jpeg_frame;

            jpeg_frame = av_frame_alloc();

            jpeg_frame->data[0] = thumbnail_output->output_data[0];
            jpeg_frame->data[1] = thumbnail_output->output_data[1];
            jpeg_frame->data[2] = thumbnail_output->output_data[2];
            jpeg_frame->data[3] = thumbnail_output->output_data[3];
            jpeg_frame->linesize[0] = thumbnail_output->output_stride[0];
            jpeg_frame->linesize[1] = thumbnail_output->output_stride[1];
            jpeg_frame->linesize[2] = thumbnail_output->output_stride[2];
            jpeg_frame->linesize[3] = thumbnail_output->output_stride[3];
            jpeg_frame->pts = AV_NOPTS_VALUE;
            jpeg_frame->pkt_dts = AV_NOPTS_VALUE;
            jpeg_frame->pkt_duration = 0;
            jpeg_frame->pkt_pos = -1;
            jpeg_frame->pkt_size = -1;
            jpeg_frame->key_frame = -1;
            jpeg_frame->sample_aspect_ratio = (AVRational){1,1};
            jpeg_frame->format = 0;
            jpeg_frame->extended_data = NULL;
            jpeg_frame->color_primaries = AVCOL_PRI_BT709;
            jpeg_frame->color_trc = AVCOL_TRC_BT709;
            jpeg_frame->colorspace = AVCOL_SPC_BT709;
            jpeg_frame->color_range = AVCOL_RANGE_JPEG;
            jpeg_frame->chroma_location = AVCHROMA_LOC_UNSPECIFIED;
            jpeg_frame->flags = 0;
            jpeg_frame->channels = 0;
            jpeg_frame->channel_layout = 0;
            jpeg_frame->width = THUMBNAIL_WIDTH;
            jpeg_frame->height = THUMBNAIL_HEIGHT;
            jpeg_frame->interlaced_frame = 0;
            jpeg_frame->top_field_first = 0;

            save_frame_as_jpeg(core, jpeg_frame);

            av_frame_free(&jpeg_frame);
            av_freep(&thumbnail_output->output_data[0]);
            free(thumbnail_output);
            thumbnail_output = NULL;
        }
        memory_return(core->fillet_msg_pool, msg);
    }
    return NULL;
}

#if defined(ENABLE_GPU)
void *video_encode_thread_nvenc(void *context)
{
    thread_start_struct *start = (thread_start_struct*)context;
    fillet_app_struct *core = (fillet_app_struct*)start->core;
    dataqueue_message_struct *msg;
    int current_encoder = start->index;
    gpu_encoder_struct gpu_data[MAX_TRANS_OUTPUTS];

#define MAX_ENCODE_SIGNAL_WINDOW 180
    encoder_opaque_struct signal_data[MAX_ENCODE_SIGNAL_WINDOW];
    int signal_write_index = 0;

    free(start);

    gpu_data[current_encoder].initialized = 0;

    // so after updating ffmpeg libraries
    // I've had to add in this hack
    // otherwise it is crazy out of sync
    // ugh
    // previous ffmpeg libs didn't have this problem

    gpu_data[current_encoder].encode_avctx = NULL;
    gpu_data[current_encoder].encode_codec = NULL;
    gpu_data[current_encoder].encode_pkt = NULL;
    gpu_data[current_encoder].encode_av_frame = NULL;
    gpu_data[current_encoder].encode_surface = NULL;

    while (video_encode_thread_running) {
        msg = (dataqueue_message_struct*)dataqueue_take_back(core->encodevideo->input_queue[current_encoder]);
        while (!msg && video_encode_thread_running) {
            usleep(100);
            msg = (dataqueue_message_struct*)dataqueue_take_back(core->encodevideo->input_queue[current_encoder]);
        }

        int output_width = core->cd->transvideo_info[current_encoder].width;
        int output_height = core->cd->transvideo_info[current_encoder].height;
        uint32_t sar_width;
        uint32_t sar_height;

        if (!gpu_data[current_encoder].initialized) {
            enum AVHWDeviceType type;
            enum AVPixelFormat hw_pix_fmt;
            int ret;
            char gpu_select_string[MAX_FILENAME_SIZE];

            type = AV_HWDEVICE_TYPE_CUDA;
            hw_pix_fmt = AV_PIX_FMT_CUDA;

            memset(signal_data,0,sizeof(signal_data));

            fprintf(stderr,"\n\n\nInitializing NVENC Hardware\n\n\n");

            if (core->cd->transvideo_info[0].video_codec == STREAM_TYPE_HEVC) {
                gpu_data[current_encoder].encode_codec = avcodec_find_encoder_by_name("hevc_nvenc");
            } else if (core->cd->transvideo_info[0].video_codec == STREAM_TYPE_H264) {
                gpu_data[current_encoder].encode_codec = avcodec_find_encoder_by_name("h264_nvenc");
            }

            fprintf(stderr,"NVENC: encode_codec=%p\n", gpu_data[current_encoder].encode_codec);
            gpu_data[current_encoder].encode_avctx = avcodec_alloc_context3(gpu_data[current_encoder].encode_codec);
            fprintf(stderr,"NVENC: encode_avctx=%p\n", gpu_data[current_encoder].encode_avctx);

            snprintf(gpu_select_string,MAX_FILENAME_SIZE-1,"/dev/dri/card%d",core->cd->gpu);

            ret = av_hwdevice_ctx_create(&gpu_data[current_encoder].hw_device_ctx,
                                         type, gpu_select_string, NULL, 0);

            fprintf(stderr,"NVENC: av_hwdevice_ctx_create=%d\n", ret);

            gpu_data[current_encoder].encode_avctx->hw_device_ctx = av_buffer_ref(gpu_data[current_encoder].hw_device_ctx);
            gpu_data[current_encoder].encode_avctx->hw_frames_ctx = av_hwframe_ctx_alloc(gpu_data[current_encoder].hw_device_ctx);
            gpu_data[current_encoder].hw_frames_ctx = (AVHWFramesContext*)gpu_data[current_encoder].encode_avctx->hw_frames_ctx->data;

            fprintf(stderr,"NVENC: hw_device_ctx=%p\n", gpu_data[current_encoder].hw_device_ctx);
            fprintf(stderr,"NVENC: hw_frames_ctx=%p\n", gpu_data[current_encoder].hw_frames_ctx);

            gpu_data[current_encoder].encode_pkt = av_packet_alloc();

            fprintf(stderr,"NVENC: encode_pkt=%p\n", gpu_data[current_encoder].encode_pkt);

            gpu_data[current_encoder].encode_avctx->bit_rate = core->cd->transvideo_info[current_encoder].video_bitrate * 1000;
            gpu_data[current_encoder].encode_avctx->rc_max_rate = gpu_data[current_encoder].encode_avctx->bit_rate;
            gpu_data[current_encoder].encode_avctx->width = output_width;
            gpu_data[current_encoder].encode_avctx->height = output_height;

            gpu_data[current_encoder].encode_avctx->time_base = (AVRational){msg->fps_den,msg->fps_num};
            gpu_data[current_encoder].encode_avctx->framerate = (AVRational){msg->fps_num,msg->fps_den};

            fprintf(stderr,"NVENC: fps=%d/%d\n", msg->fps_num, msg->fps_den);

            double fps = 30.0;
            if (msg->fps_den > 0) {
                fps = (double)((double)msg->fps_num / (double)msg->fps_den);
                int fpsup = (int)((double)fps + 0.5f);
                /*if (fps > 30) {
                    fpsup = -16;
                } else {
                    fpsup = 30;
                }*/
                if (core->cd->transvideo_info[0].video_codec == STREAM_TYPE_H264) {
                    fpsup = 30;
                    gpu_data[current_encoder].frame_count_pts = fpsup + MAX_B_FRAMES;  // b-frame distance?
                    gpu_data[current_encoder].frame_count_dts = fpsup;
                } else if (core->cd->transvideo_info[0].video_codec == STREAM_TYPE_HEVC) {
                    fpsup = 0;
                    gpu_data[current_encoder].frame_count_pts = fpsup + MAX_B_FRAMES;  // b-frame distance?
                    gpu_data[current_encoder].frame_count_dts = fpsup;
                } else { // AV1
                }
            }
            gpu_data[current_encoder].encode_avctx->gop_size = (int)((double)fps + 0.5);
            gpu_data[current_encoder].encode_avctx->max_b_frames = MAX_B_FRAMES;
            gpu_data[current_encoder].encode_avctx->compression_level = 7;
            gpu_data[current_encoder].encode_avctx->pix_fmt = AV_PIX_FMT_CUDA;
            gpu_data[current_encoder].encode_avctx->sw_pix_fmt = AV_PIX_FMT_NV12;

            // settings are from nvenc_h264.c
            av_opt_set(gpu_data[current_encoder].encode_avctx->priv_data,"no-scenecut","1",0);
            av_opt_set_int(gpu_data[current_encoder].encode_avctx->priv_data,"forced-idr",1,0);
            if (core->cd->transvideo_info[0].video_codec == STREAM_TYPE_HEVC) {
                av_opt_set_int(gpu_data[current_encoder].encode_avctx->priv_data,"b_ref_mode",1,0);
            }
            av_opt_set(gpu_data[current_encoder].encode_avctx->priv_data,"preset","slow",0);
            if (core->cd->transvideo_info[0].video_codec == STREAM_TYPE_HEVC) {
                av_opt_set(gpu_data[current_encoder].encode_avctx->priv_data,"profile","main",0);
            } else {
                if (core->cd->transvideo_info[current_encoder].encoder_profile == ENCODER_PROFILE_BASE) {
                    av_opt_set(gpu_data[current_encoder].encode_avctx->priv_data,"profile","baseline",0);
                    gpu_data[current_encoder].encode_avctx->max_b_frames = 0;
                } else if (core->cd->transvideo_info[current_encoder].encoder_profile == ENCODER_PROFILE_MAIN) {
                    av_opt_set(gpu_data[current_encoder].encode_avctx->priv_data,"profile","main",0);
                } else {
                    av_opt_set(gpu_data[current_encoder].encode_avctx->priv_data,"profile","high",0);
                }
            }
            av_opt_set(gpu_data[current_encoder].encode_avctx->priv_data,"aud","1",0);

            gpu_data[current_encoder].hw_frames_ctx->format = AV_PIX_FMT_CUDA;
            gpu_data[current_encoder].hw_frames_ctx->sw_format = AV_PIX_FMT_NV12;
            gpu_data[current_encoder].hw_frames_ctx->width = output_width;
            gpu_data[current_encoder].hw_frames_ctx->height = output_height;
            gpu_data[current_encoder].hw_frames_ctx->initial_pool_size = GPU_BUFFERS*2;

            ret = av_hwframe_ctx_init(gpu_data[current_encoder].encode_avctx->hw_frames_ctx);
            fprintf(stderr,"NVENC: av_hwframe_ctx_init=%d\n", ret);
            ret = avcodec_open2(gpu_data[current_encoder].encode_avctx,
                                gpu_data[current_encoder].encode_codec,
                                NULL);
            fprintf(stderr,"NVENC: avcodec_open2=%d\n", ret);

            gpu_data[current_encoder].encode_av_frame = av_frame_alloc();
            gpu_data[current_encoder].encode_surface = av_frame_alloc();

            gpu_data[current_encoder].encode_av_frame->hw_frames_ctx = NULL;
            gpu_data[current_encoder].encode_av_frame->format = AV_PIX_FMT_NV12;
            gpu_data[current_encoder].encode_av_frame->width = output_width;
            gpu_data[current_encoder].encode_av_frame->height = output_height;

            gpu_data[current_encoder].encode_surface->format = AV_PIX_FMT_NV12;
            gpu_data[current_encoder].encode_surface->width = output_width;
            gpu_data[current_encoder].encode_surface->height = output_height;

            ret = av_frame_get_buffer(gpu_data[current_encoder].encode_av_frame, GPU_BUFFERS);
            fprintf(stderr,"NVENC: av_frame_get_buffer=%d\n", ret);
            ret = av_hwframe_get_buffer(gpu_data[current_encoder].encode_avctx->hw_frames_ctx,
                                        gpu_data[current_encoder].encode_surface,
                                        GPU_BUFFERS);
            fprintf(stderr,"NVENC: av_hwframe_get_buffer=%d\n", ret);

            gpu_data[current_encoder].initialized = 1;

            fprintf(stderr,"\n\n\nDone initializing NVENC Hardware\n\n\n");
        } // !initialized

        if (!video_encode_thread_running) {
            while (msg) {
                if (msg) {
                    memory_return(core->raw_video_pool, msg->buffer);
                    msg->buffer = NULL;
                    if (msg->caption_buffer) {
                        free(msg->caption_buffer);
                        msg->caption_buffer = NULL;
                        msg->caption_size = 0;
                    }
                    memory_return(core->fillet_msg_pool, msg);
                    msg = NULL;
                }
                msg = (dataqueue_message_struct*)dataqueue_take_back(core->encodevideo->input_queue[current_encoder]);
            }
            goto cleanup_nvenc_video_encode_thread;
        }

        uint8_t *video;
        int output_size;
        int64_t pts;
        int64_t dts;
        int source_splice_point = 0;
        int64_t source_splice_duration = 0;
        int64_t source_splice_duration_remaining = 0;
        int owhalf = output_width / 2;
        int ohhalf = output_height / 2;
        int frames;
        uint32_t nal_count;
        int ret;

        video = msg->buffer;
        source_splice_point = msg->splice_point;
        source_splice_duration = msg->splice_duration;
        source_splice_duration_remaining = msg->splice_duration_remaining;

        // if splice point - force intra frame
        // when duration is done - also force intra frame
        // keep track of splice duration remaining
        if (source_splice_duration_remaining > 0) {
            fprintf(stderr,"video_encode_thread_nvenc: incoming, splice_point=%d, splice_duration=%ld, splice_duration_remaining=%ld\n",
                    source_splice_point,
                    source_splice_duration,
                    source_splice_duration_remaining);

            syslog(LOG_INFO,"video_encode_thread_nvenc: incoming, splice_point=%d, splice_duration=%ld, splice_duration_remaining=%ld\n",
                   source_splice_point,
                   source_splice_duration,
                   source_splice_duration_remaining);
        }

        ret = av_frame_make_writable(gpu_data[current_encoder].encode_av_frame);

        if (source_splice_point == SPLICE_CUE_IN || source_splice_point == SPLICE_CUE_OUT) {
            char splicemsg[MAX_MESSAGE_SIZE];

            syslog(LOG_INFO,"video_encode_thread_nvenc: scte35(%d), inserting IDR frame during splice point=%d\n",
                   current_encoder, source_splice_point);

            if (source_splice_point == SPLICE_CUE_IN) {
                snprintf(splicemsg, MAX_MESSAGE_SIZE-1, "Inserting IDR video frame for SCTE35 CUE IN splice point, encoder=%d",
                         current_encoder);
                send_signal(core, SIGNAL_FRAME_VIDEO_SPLICE, splicemsg);
            } else if (source_splice_point == SPLICE_CUE_OUT) {
                snprintf(splicemsg, MAX_MESSAGE_SIZE-1, "Inserting IDR video frame for SCTE35 CUE OUT splice point, encoder=%d",
                         current_encoder);
                send_signal(core, SIGNAL_FRAME_VIDEO_SPLICE, splicemsg);
            }

            gpu_data[current_encoder].encode_surface->pict_type = AV_PICTURE_TYPE_I;
            gpu_data[current_encoder].encode_av_frame->pict_type = AV_PICTURE_TYPE_I;
            gpu_data[current_encoder].encode_surface->key_frame = 1;
            gpu_data[current_encoder].encode_av_frame->key_frame = 1;
        } else {
            gpu_data[current_encoder].encode_surface->pict_type = AV_PICTURE_TYPE_NONE;
            gpu_data[current_encoder].encode_av_frame->pict_type = AV_PICTURE_TYPE_NONE;
        }


        signal_data[signal_write_index].frame_count_pts = gpu_data[current_encoder].frame_count_pts;
        signal_data[signal_write_index].splice_point = source_splice_point;
        signal_data[signal_write_index].splice_duration = source_splice_duration;
        signal_data[signal_write_index].splice_duration_remaining = source_splice_duration_remaining;
        signal_write_index = (signal_write_index + 1) % MAX_ENCODE_SIGNAL_WINDOW;

        /*encoder_opaque_struct *opaque_data = (encoder_opaque_struct*)malloc(sizeof(encoder_opaque_struct));
        if (opaque_data) {
            opaque_data->splice_point = source_splice_point;
            opaque_data->splice_duration = source_splice_duration;
            opaque_data->splice_duration_remaining = source_splice_duration_remaining;
            opaque_data->frame_count_pts = gpu_data[current_encoder].frame_count_pts;
            }*/

        uint8_t *ybuffersrc = video;
        uint8_t *ubuffersrc = ybuffersrc + (output_width*output_height);
        uint8_t *vbuffersrc = ubuffersrc + (owhalf*ohhalf);
        uint8_t *uvbuf;
        int y;
        int x;
        AVFrame *frame;

        frame = gpu_data[current_encoder].encode_av_frame;
        uvbuf = (uint8_t*)frame->data[1];

        for (y = 0; y < output_height; y++) {
            memcpy(&frame->data[0][y * frame->linesize[0]], ybuffersrc, frame->linesize[0]);
            ybuffersrc += frame->width;
        }
        // nv12 format - interleaved chroma
        for (y = 0; y < ohhalf; y++) {
            for (x = 0; x < owhalf; x++) {
                *uvbuf = *ubuffersrc;
                uvbuf++;
                ubuffersrc++;
                *uvbuf = *vbuffersrc;
                uvbuf++;
                vbuffersrc++;
            }
        }

        if (msg->caption_buffer) {
            //dataqueue_message_struct *caption_msg = (dataqueue_message_struct*)memory_take(core->fillet_msg_pool, sizeof(dataqueue_message_struct));
            fprintf(stderr,"video_encode_thread_nvenc: processing caption buffer, size=%d\n", msg->caption_size);

            int caption_size = msg->caption_size - 8;
            AVFrameSideData *caption_data = av_frame_new_side_data(gpu_data[current_encoder].encode_surface,//av_frame,
                                                                   AV_FRAME_DATA_A53_CC,
                                                                   caption_size);

            if (caption_data) {
                caption_data->size = caption_size;
                memcpy(caption_data->data, msg->caption_buffer+7, caption_size);
            } else {
                fprintf(stderr,"unable to save caption buffer\n");
            }

            free(msg->caption_buffer);
            msg->caption_buffer = NULL;
            msg->caption_size = 0;
        }

        gpu_data[current_encoder].encode_surface->pts = gpu_data[current_encoder].frame_count_pts;
        gpu_data[current_encoder].encode_av_frame->pts = gpu_data[current_encoder].frame_count_pts;
        gpu_data[current_encoder].encode_surface->pkt_dts = gpu_data[current_encoder].frame_count_pts;
        //gpu_data[current_encoder].encode_av_frame->opaque = (void*)opaque_data;
        //gpu_data[current_encoder].encode_surface->opaque = (void*)opaque_data;

        //fprintf(stderr,"\n\n\nSending Video Frame to NVENC Hardware\n\n\n");
        ret = av_hwframe_transfer_data(gpu_data[current_encoder].encode_surface,
                                       gpu_data[current_encoder].encode_av_frame,
                                       0);

        //fprintf(stderr,"\n\n\nav_hwframe_transfer_data=%d\n\n\n", ret);
        ret = avcodec_send_frame(gpu_data[current_encoder].encode_avctx,
                                 gpu_data[current_encoder].encode_surface);

        av_frame_remove_side_data(gpu_data[current_encoder].encode_surface, AV_FRAME_DATA_A53_CC);
        //fprintf(stderr,"done sending avcodec_send_frame:%d\n", ret);

        gpu_data[current_encoder].frame_count_pts++;

        while (ret >= 0) {
            int nalsize = 0;
            uint8_t *nal_buffer;
            double output_fps = (double)30000.0/(double)1001.0;
            double ticks_per_frame_double = (double)90000.0/(double)output_fps;
            video_stream_struct *vstream = (video_stream_struct*)core->source_video_stream[0].video_stream;  // only one source stream

            output_fps = (double)((double)msg->fps_num / (double)msg->fps_den);
            if (output_fps > 0) {
                ticks_per_frame_double = (double)90000.0/((double)msg->fps_num / (double)msg->fps_den);
            }

            ret = avcodec_receive_packet(gpu_data[current_encoder].encode_avctx,
                                         gpu_data[current_encoder].encode_pkt);
            if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
                break;
            } else if (ret < 0) {
                fprintf(stderr,"error during encode\n");
                break;
            }

            nalsize = gpu_data[current_encoder].encode_pkt->size;
            nal_buffer = (uint8_t*)memory_take(core->compressed_video_pool, nalsize*2);
            if (!nal_buffer) {
                fprintf(stderr,"FATAL ERROR: unable to obtain nal_buffer!!\n");
                send_direct_error(core, SIGNAL_DIRECT_ERROR_NALPOOL, "Out of NAL Buffers (H264/H265) - Restarting Service");
                _Exit(0);
            }

            memcpy(nal_buffer, gpu_data[current_encoder].encode_pkt->data, nalsize);

            int lookup;
            int output_splice_point = 0;
            int64_t output_splice_duration = 0;
            int64_t output_splice_duration_remaining = 0;
            int found_splice = 0;
            for (lookup = 0; lookup < MAX_ENCODE_SIGNAL_WINDOW; lookup++) {
                if (signal_data[lookup].frame_count_pts == gpu_data[current_encoder].encode_pkt->pts) {
                    output_splice_point = signal_data[lookup].splice_point;
                    output_splice_duration = signal_data[lookup].splice_duration;
                    output_splice_duration_remaining = signal_data[lookup].splice_duration_remaining;
                    found_splice = 1;
                    break;
                }
            }

            if (!found_splice) {
                // for debugging
                _Exit(0);
            }

            double opaque_double = gpu_data[current_encoder].encode_pkt->pts;
            int64_t pts = (int64_t)((double)opaque_double * (double)ticks_per_frame_double) + (int64_t)vstream->first_timestamp;
            int64_t dts = (int64_t)((double)gpu_data[current_encoder].frame_count_dts * (double)ticks_per_frame_double) + (int64_t)vstream->first_timestamp;

            if (pts < dts) {
                fprintf(stderr,"\n\n\n\nvideo_encoder_thread_nvenc: PTS before DTS!!!!\n\n\n\n");
            }

            fprintf(stderr,"video_encode_thread_nvenc: outgoing, splice_point=%d, splice_duration=%ld, splice_duration_remaining=%ld\n",
                    output_splice_point,
                    output_splice_duration,
                    output_splice_duration_remaining);

            syslog(LOG_INFO,"video_encode_thread_nvenc: outgoing, splice_point=%d, splice_duration=%ld, splice_duration_remaining=%ld\n",
                   output_splice_point,
                   output_splice_duration,
                   output_splice_duration_remaining);

            fprintf(stderr,"video_encode_thread_nvenc: receiving compressed Video Frame from NVENC Hardware(%d): %d  sourcepts:%f  pts:%ld  dts:%ld  ticks_per_frame:%f   output_fps:%.2f\n\n\n",
                    current_encoder,
                    gpu_data[current_encoder].encode_pkt->size,
                    opaque_double,
                    pts,
                    dts,
                    ticks_per_frame_double,
                    output_fps);

            av_packet_unref(gpu_data[current_encoder].encode_pkt);

            gpu_data[current_encoder].frame_count_dts++;

#if 0
            {
                int read_pos = 0;
                while (read_pos < nalsize) {
                    if (nal_buffer[read_pos+0] == 0x00 &&
                        nal_buffer[read_pos+1] == 0x00 &&
                        nal_buffer[read_pos+2] == 0x01) {
                        int nal_type = nal_buffer[read_pos+3] & 0x1f;
                        fprintf(stderr,"video_encode_thread_nvenc: outgoing nal type is 0x%x (%d)   0x%x 0x%x 0x%x 0x%x 0x%x 0x%x 0x%x 0x%x\n",
                                nal_type, nal_type,
                                nal_buffer[read_pos+4], nal_buffer[read_pos+5], nal_buffer[read_pos+6], nal_buffer[read_pos+7],
                                nal_buffer[read_pos+8], nal_buffer[read_pos+9], nal_buffer[read_pos+10], nal_buffer[read_pos+11]);
                    }
                    read_pos++;
                }
            }
#endif

            if (core->video_encode_time_set == 0) {
                core->video_encode_time_set = 1;
                clock_gettime(CLOCK_MONOTONIC, &core->video_encode_time);
            }

            video_sink_frame_callback(core, nal_buffer,
                                      nalsize,
                                      pts, dts, current_encoder,
                                      output_splice_point, output_splice_duration, output_splice_duration_remaining);
        }

        if (msg) {
            memory_return(core->raw_video_pool, msg->buffer);
            msg->buffer = NULL;
            memory_return(core->fillet_msg_pool, msg);
            msg = NULL;
        }
    }
cleanup_nvenc_video_encode_thread:
    avcodec_free_context(&gpu_data[current_encoder].encode_avctx);
    av_frame_free(&gpu_data[current_encoder].encode_av_frame);
    av_frame_free(&gpu_data[current_encoder].encode_surface);
    av_buffer_unref(&gpu_data[current_encoder].hw_device_ctx);
    av_packet_free(&gpu_data[current_encoder].encode_pkt);

    return NULL;
}
#endif // ENABLE_GPU

#if !defined(ENABLE_GPU)
void *video_encode_thread_x265(void *context)
{
    thread_start_struct *start = (thread_start_struct*)context;
    fillet_app_struct *core = (fillet_app_struct*)start->core;
    dataqueue_message_struct *msg;
    int current_encoder = start->index;
    x265_encoder_struct x265_data[MAX_TRANS_OUTPUTS];

    free(start);
    x265_data[current_encoder].api = NULL;
    x265_data[current_encoder].encoder = NULL;
    x265_data[current_encoder].param = NULL;
    x265_data[current_encoder].pic_in = (x265_picture*)&x265_data[current_encoder].pic_orig;
    x265_data[current_encoder].pic_recon = (x265_picture*)&x265_data[current_encoder].pic_out;
    x265_data[current_encoder].p_nal = NULL;
    x265_data[current_encoder].p_current_nal = NULL;
    x265_data[current_encoder].frame_count_pts = 1;
    x265_data[current_encoder].frame_count_dts = 0;

    while (video_encode_thread_running) {
        // loop across the input queues and feed the encoders
        {
            msg = (dataqueue_message_struct*)dataqueue_take_back(core->encodevideo->input_queue[current_encoder]);
            while (!msg && video_encode_thread_running) {
                usleep(100);
                msg = (dataqueue_message_struct*)dataqueue_take_back(core->encodevideo->input_queue[current_encoder]);
            }

            int output_width = core->cd->transvideo_info[current_encoder].width;
            int output_height = core->cd->transvideo_info[current_encoder].height;
            uint32_t sar_width;
            uint32_t sar_height;

            if (!x265_data[current_encoder].encoder) {
                x265_data[current_encoder].api = x265_api_get(8); // only 8-bit for now
                if (!x265_data[current_encoder].api) {
                    x265_data[current_encoder].api = x265_api_get(0);
                }
                x265_data[current_encoder].param = x265_data[current_encoder].api->param_alloc();

                memset(x265_data[current_encoder].param, 0, sizeof(x265_param));
                x265_param_default(x265_data[current_encoder].param);

                x265_data[current_encoder].param->bEmitInfoSEI = 0;
                x265_data[current_encoder].param->internalCsp = X265_CSP_I420; //8-bit/420
                x265_data[current_encoder].param->internalBitDepth = 8;
                x265_data[current_encoder].param->bHighTier = 0;
                x265_data[current_encoder].param->bRepeatHeaders = 1;
                x265_data[current_encoder].param->bAnnexB = 1;
                x265_data[current_encoder].param->sourceWidth = output_width;
                x265_data[current_encoder].param->sourceHeight = output_height;
                x265_data[current_encoder].param->limitTU = 0;
                x265_data[current_encoder].param->logLevel = X265_LOG_DEBUG;
                x265_data[current_encoder].param->bEnableWavefront = 1;
                x265_data[current_encoder].param->bOpenGOP = 0;
                x265_data[current_encoder].param->fpsNum = msg->fps_num;
                x265_data[current_encoder].param->fpsDenom = msg->fps_den;
                x265_data[current_encoder].param->bDistributeModeAnalysis = 0;
                x265_data[current_encoder].param->bDistributeMotionEstimation = 0;
                x265_data[current_encoder].param->scenecutThreshold = 0;
                x265_data[current_encoder].param->bframes = 2;  // set configuration
                x265_data[current_encoder].param->bBPyramid = 0; // set configuration
                x265_data[current_encoder].param->bFrameAdaptive = 0;
                x265_data[current_encoder].param->rc.rateControlMode = X265_RC_ABR;
                x265_data[current_encoder].param->rc.bitrate = core->cd->transvideo_info[current_encoder].video_bitrate;
                x265_data[current_encoder].param->rc.vbvBufferSize = core->cd->transvideo_info[current_encoder].video_bitrate;
                x265_data[current_encoder].param->bEnableAccessUnitDelimiters = 1;
                x265_data[current_encoder].param->frameNumThreads = 0;
                x265_data[current_encoder].param->interlaceMode = 0; // nope!
                x265_data[current_encoder].param->levelIdc = 0;
                x265_data[current_encoder].param->bEnableRectInter = 0;
                x265_data[current_encoder].param->bEnableAMP = 0;
                x265_data[current_encoder].param->bEnablePsnr = 0;
                x265_data[current_encoder].param->bEnableSsim = 0;
                x265_data[current_encoder].param->bEnableStrongIntraSmoothing = 1;
                x265_data[current_encoder].param->bEnableWeightedPred = 0; // simple
                x265_data[current_encoder].param->bEnableTemporalMvp = 0;
                x265_data[current_encoder].param->bEnableLoopFilter = 1;
                x265_data[current_encoder].param->bEnableSAO = 1;
                x265_data[current_encoder].param->bEnableFastIntra = 1;
                x265_data[current_encoder].param->decodedPictureHashSEI = 0;
                x265_data[current_encoder].param->bLogCuStats = 0;
                x265_data[current_encoder].param->lookaheadDepth = 15;
                x265_data[current_encoder].param->lookaheadSlices = 5;

                if (output_width < 960) {  // sd/hd?
                    x265_data[current_encoder].param->maxCUSize = 16;
                    x265_data[current_encoder].param->minCUSize = 8;
                } else {
                    x265_data[current_encoder].param->maxCUSize = 32;
                    x265_data[current_encoder].param->minCUSize = 16;
                }

                x265_data[current_encoder].param->rdLevel = 1; // very simple... will make quality profiles
                x265_data[current_encoder].param->bEnableEarlySkip = 1;
                x265_data[current_encoder].param->searchMethod = X265_DIA_SEARCH; // simple
                x265_data[current_encoder].param->subpelRefine = 1; // simple
                x265_data[current_encoder].param->maxNumMergeCand = 2;
                x265_data[current_encoder].param->maxNumReferences = 1;

                // these really need to be tuned
                if (core->cd->transvideo_info[current_encoder].encoder_quality == ENCODER_QUALITY_BASIC) {
                    // basic quality is default
                } else if (core->cd->transvideo_info[current_encoder].encoder_quality == ENCODER_QUALITY_STREAMING) {
                    x265_data[current_encoder].param->rdLevel = 3;
                    x265_data[current_encoder].param->bEnableEarlySkip = 1;
                    x265_data[current_encoder].param->searchMethod = X265_DIA_SEARCH;
                    x265_data[current_encoder].param->subpelRefine = 3;
                    x265_data[current_encoder].param->maxNumMergeCand = 2;
                    x265_data[current_encoder].param->maxNumReferences = 2;
                } else if (core->cd->transvideo_info[current_encoder].encoder_quality == ENCODER_QUALITY_BROADCAST) {
                    x265_data[current_encoder].param->rdLevel = 5;
                    x265_data[current_encoder].param->bEnableEarlySkip = 1;
                    x265_data[current_encoder].param->searchMethod = X265_DIA_SEARCH;
                    x265_data[current_encoder].param->subpelRefine = 5;
                    x265_data[current_encoder].param->maxNumMergeCand = 2;
                    x265_data[current_encoder].param->maxNumReferences = 3;
                    x265_data[current_encoder].param->bframes = 3;  // set configuration
                } else {  // ENCODER_QUALITY_PROFESSIONAL
                    x265_data[current_encoder].param->rdLevel = 5;
                    x265_data[current_encoder].param->bEnableEarlySkip = 1;
                    x265_data[current_encoder].param->searchMethod = X265_DIA_SEARCH;
                    x265_data[current_encoder].param->subpelRefine = 5;
                    x265_data[current_encoder].param->maxNumMergeCand = 2;
                    x265_data[current_encoder].param->maxNumReferences = 3;
                    x265_data[current_encoder].param->bframes = 4;
                }

                // vui factors
                x265_data[current_encoder].param->vui.aspectRatioIdc = X265_EXTENDED_SAR;
                x265_data[current_encoder].param->vui.videoFormat = 5;

                if (core->cd->transvideo_info[0].aspect_num > 0 &&
                    core->cd->transvideo_info[0].aspect_den > 0) {
                    sar_width = output_height*core->cd->transvideo_info[0].aspect_num;
                    sar_height = output_width*core->cd->transvideo_info[0].aspect_den;
                } else {
                    sar_width = output_height*msg->aspect_num;
                    sar_height = output_width*msg->aspect_den;
                }

                x265_data[current_encoder].param->vui.sarWidth = sar_width;
                x265_data[current_encoder].param->vui.sarHeight = sar_height;
                x265_data[current_encoder].param->vui.transferCharacteristics = 2;
                x265_data[current_encoder].param->vui.matrixCoeffs = 2;
                x265_data[current_encoder].param->vui.bEnableOverscanAppropriateFlag = 0;
                x265_data[current_encoder].param->vui.bEnableVideoSignalTypePresentFlag = 0;
                x265_data[current_encoder].param->vui.bEnableVideoFullRangeFlag = 1;
                x265_data[current_encoder].param->vui.bEnableColorDescriptionPresentFlag = 1;
                x265_data[current_encoder].param->vui.bEnableDefaultDisplayWindowFlag = 0;
                x265_data[current_encoder].param->vui.bEnableChromaLocInfoPresentFlag = 0;
                x265_data[current_encoder].param->vui.chromaSampleLocTypeTopField = 0;
                x265_data[current_encoder].param->vui.chromaSampleLocTypeBottomField = 0;
                x265_data[current_encoder].param->vui.defDispWinLeftOffset = 0;
                x265_data[current_encoder].param->vui.defDispWinTopOffset = 0;
                x265_data[current_encoder].param->vui.defDispWinRightOffset = output_width;
                x265_data[current_encoder].param->vui.defDispWinBottomOffset = output_height;

                // rate control factors
                x265_data[current_encoder].param->rc.qgSize = 16;
                x265_data[current_encoder].param->rc.vbvBufferInit = 0.9;
                x265_data[current_encoder].param->rc.rfConstant = 26;
                x265_data[current_encoder].param->rc.qpStep = 4;
                x265_data[current_encoder].param->rc.qp = 28;
                x265_data[current_encoder].param->rc.cuTree = 0;
                x265_data[current_encoder].param->rc.complexityBlur = 25;
                x265_data[current_encoder].param->rc.qblur = 0.6;
                x265_data[current_encoder].param->rc.qCompress = 0.65;
                x265_data[current_encoder].param->rc.pbFactor = 1.2f;
                x265_data[current_encoder].param->rc.ipFactor = 1.4f;
                x265_data[current_encoder].param->rc.bStrictCbr = 1;

                // let's put the idr frames at second boundaries - make this configurable later
                double fps = 30.0;
                if (msg->fps_den > 0) {
                    fps = (double)((double)msg->fps_num / (double)msg->fps_den);
                }
                x265_data[current_encoder].param->keyframeMax = (int)((double)fps + 0.5);
                x265_data[current_encoder].param->keyframeMin = 1;

                // start it up
                x265_data[current_encoder].encoder = x265_data[current_encoder].api->encoder_open(x265_data[current_encoder].param);
                x265_data[current_encoder].api->encoder_parameters(x265_data[current_encoder].encoder,
                                                                   x265_data[current_encoder].param);

                x265_data[current_encoder].api->picture_init(x265_data[current_encoder].param,
                                                             x265_data[current_encoder].pic_in);

                fprintf(stderr,"status: hevc encoder initialized\n");
            }

            if (!video_encode_thread_running) {
                while (msg) {
                    if (msg) {
                        memory_return(core->raw_video_pool, msg->buffer);
                        msg->buffer = NULL;
                        memory_return(core->fillet_msg_pool, msg);
                        msg = NULL;
                    }
                    msg = (dataqueue_message_struct*)dataqueue_take_back(core->encodevideo->input_queue[current_encoder]);
                }
                goto cleanup_video_encode_thread;
            }

            uint8_t *video;
            int output_size;
            int64_t pts;
            int64_t dts;
            int splice_point = 0;
            int64_t splice_duration = 0;
            int64_t splice_duration_remaining = 0;
            int owhalf = output_width / 2;
            int ohhalf = output_height / 2;
            int frames;
            uint32_t nal_count;

            video = msg->buffer;
            splice_point = msg->splice_point;
            splice_duration = msg->splice_duration;
            splice_duration_remaining = msg->splice_duration_remaining;

            // if splice point- force intra frame
            // when duration is done- also force intra frame
            // keep track of splice duration remaining
            fprintf(stderr,"VIDEO ENCODER: SPLICE POINT:%d  SPLICE DURATION: %ld  SPLICE DURATION REMAINING: %ld\n",
                    splice_point,
                    splice_duration,
                    splice_duration_remaining);


            x265_data[current_encoder].pic_in->colorSpace = X265_CSP_I420;
            x265_data[current_encoder].pic_in->bitDepth = 8;
            x265_data[current_encoder].pic_in->stride[0] = output_width;
            x265_data[current_encoder].pic_in->stride[1] = owhalf;
            x265_data[current_encoder].pic_in->stride[2] = owhalf;
            x265_data[current_encoder].pic_in->planes[0] = video;
            x265_data[current_encoder].pic_in->planes[1] = video + (output_width * output_height);
            x265_data[current_encoder].pic_in->planes[2] = x265_data[current_encoder].pic_in->planes[1] + (owhalf*ohhalf);
            x265_data[current_encoder].pic_in->pts = x265_data[current_encoder].frame_count_pts;

            nal_count = 0;
            frames = x265_data[current_encoder].api->encoder_encode(x265_data[current_encoder].encoder,
                                                                    &x265_data[current_encoder].p_nal,
                                                                    &nal_count,
                                                                    x265_data[current_encoder].pic_in,
                                                                    x265_data[current_encoder].pic_recon);

            x265_data[current_encoder].frame_count_pts++;

            if (frames > 0) {
                uint8_t *nal_buffer;
                double output_fps = (double)30000.0/(double)1001.0;
                double ticks_per_frame_double = (double)90000.0/(double)output_fps;
                video_stream_struct *vstream = (video_stream_struct*)core->source_video_stream[0].video_stream;  // only one source stream
                int nal_idx;
                x265_nal *nalout;
                int pos = 0;
                int nalsize = 0;

                if (core->video_encode_time_set == 0) {
                    core->video_encode_time_set = 1;
                    clock_gettime(CLOCK_MONOTONIC, &core->video_encode_time);
                }

                nalout = x265_data[current_encoder].p_nal;
                for (nal_idx = 0; nal_idx < nal_count; nal_idx++) {
                    nalsize += nalout->sizeBytes;
                    nalout++;
                }

                nalout = x265_data[current_encoder].p_nal;
                nal_buffer = (uint8_t*)memory_take(core->compressed_video_pool, nalsize*2);
                if (!nal_buffer) {
                    fprintf(stderr,"FATAL ERROR: unable to obtain nal_buffer!!\n");
                    send_direct_error(core, SIGNAL_DIRECT_ERROR_NALPOOL, "Out of NAL Buffers (H265) - Restarting Service");
                    _Exit(0);
                }
                for (nal_idx = 0; nal_idx < nal_count; nal_idx++) {
                    int nocopy = 0;

                    if (nalout->type == 35) {  // AUD-not needed right now
                        nocopy = 1;
                    }

#if defined(DEBUG_NALTYPE)
                    if (nalout->type == 32) {
                        syslog(LOG_INFO,"HEVC VIDEO FRAME: VPS\n");
                    } else if (nalout->type == 33) {
                        syslog(LOG_INFO,"HEVC VIDEO FRAME: SPS\n");
                    } else if (nalout->type == 34) {
                        syslog(LOG_INFO,"HEVC VIDEO FRAME: PPS\n");
                    } else if (nalout->type == 35) {
                        syslog(LOG_INFO,"HEVC VIDEO FRAME: AUD\n");
                        nocopy = 1;
                    } else if (nalout->type == 19 || nalout->type == 20) {
                        syslog(LOG_INFO,"HEVC VIDEO FRAME: IDR\n");
                    } else {
                        syslog(LOG_INFO,"HEVC VIDEO FRAME: 0x%x (SIZE:%d)\n", nalout->type, nalout->sizeBytes);
                    }
#endif // DEBUG_NALTYPE

                    if (!nocopy) {
                        memcpy(nal_buffer + pos, nalout->payload, nalout->sizeBytes);
                        pos += nalout->sizeBytes;
                    } else {
                        // skip aud
                    }

                    nalout++;
                }
                nalsize = pos;
                //nalsize is the size of the nal unit

                if (x265_data[current_encoder].param->fpsDenom > 0) {
                    output_fps = (double)x265_data[current_encoder].param->fpsNum / (double)x265_data[current_encoder].param->fpsDenom;
                    if (output_fps > 0) {
                        ticks_per_frame_double = (double)90000.0/(double)output_fps;
                    }
                }

                /*encoder_opaque_struct *opaque_output = (encoder_opaque_struct*)x264_data[current_encoder].pic_out.opaque;
                int64_t opaque_int64 = 0;
                if (opaque_output) {
                    splice_point = opaque_output->splice_point;
                    splice_duration = opaque_output->splice_duration;
                    splice_duration_remaining = opaque_output->splice_duration_remaining;
                    opaque_int64 = opaque_output->frame_count_pts;
                    //int64_t opaque_int64 = (int64_t)x264_data[current_encoder].pic_out.opaque;
                }
                double opaque_double = (double)opaque_int64;
                */

                double opaque_double = (double)x265_data[current_encoder].pic_recon->pts;
                pts = (int64_t)((double)opaque_double * (double)ticks_per_frame_double) + (int64_t)vstream->first_timestamp;
                dts = (int64_t)((double)x265_data[current_encoder].frame_count_dts * (double)ticks_per_frame_double) + (int64_t)vstream->first_timestamp;

                x265_data[current_encoder].frame_count_dts++;
                //need to pass this data through
                splice_point = 0;
                splice_duration = 0;
                splice_duration_remaining = 0;
                output_size = nalsize;

#if defined(DEBUG_NALTYPE)
                syslog(LOG_INFO,"DELIVERING HEVC ENCODED VIDEO FRAME: %d   PTS:%ld  DTS:%ld\n",
                       output_size,
                       pts, dts);
#endif

                video_sink_frame_callback(core, nal_buffer, output_size, pts, dts, current_encoder, splice_point, splice_duration, splice_duration_remaining);
            }

            if (msg) {
                memory_return(core->raw_video_pool, msg->buffer);
                msg->buffer = NULL;
                memory_return(core->fillet_msg_pool, msg);
                msg = NULL;
            }
        }
    }

cleanup_video_encode_thread:

    //x265_data[current_encoder].api->encoder_close();

    return NULL;
}
#endif // ENABLE_GPU

#if !defined(ENABLE_GPU)
void *video_encode_thread_x264(void *context)
{
    thread_start_struct *start = (thread_start_struct*)context;
    fillet_app_struct *core = (fillet_app_struct*)start->core;
    dataqueue_message_struct *msg;
    int current_encoder = start->index;
    x264_encoder_struct x264_data[MAX_TRANS_OUTPUTS];
#define MAX_SEI_PAYLOAD_SIZE 512

    free(start);
    x264_data[current_encoder].h = NULL;
    x264_data[current_encoder].frame_count_pts = 1;
    x264_data[current_encoder].frame_count_dts = 0;

    while (video_encode_thread_running) {
        // loop across the input queues and feed the encoders
        {
            msg = (dataqueue_message_struct*)dataqueue_take_back(core->encodevideo->input_queue[current_encoder]);
            while (!msg && video_encode_thread_running) {
                usleep(100);
                msg = (dataqueue_message_struct*)dataqueue_take_back(core->encodevideo->input_queue[current_encoder]);
            }

            if (!x264_data[current_encoder].h) {
                int output_width = core->cd->transvideo_info[current_encoder].width;
                int output_height = core->cd->transvideo_info[current_encoder].height;
                uint32_t sar_width;
                uint32_t sar_height;

                x264_data[current_encoder].width = output_width;
                x264_data[current_encoder].height = output_height;
                x264_param_default_preset(&x264_data[current_encoder].param,
                                          "medium",
                                          NULL);

                x264_data[current_encoder].y_size = output_width * output_height;
                x264_data[current_encoder].uv_size = (output_width * output_height) / 4;

                x264_data[current_encoder].param.i_csp = X264_CSP_I420;
                x264_data[current_encoder].param.i_width = output_width;
                x264_data[current_encoder].param.i_height = output_height;
                x264_picture_alloc(&x264_data[current_encoder].pic,
                                   x264_data[current_encoder].param.i_csp,
                                   output_width,
                                   output_height);

                x264_data[current_encoder].param.b_interlaced = 0;
                x264_data[current_encoder].param.b_fake_interlaced = 0;
                x264_data[current_encoder].param.b_deterministic = 1;
                x264_data[current_encoder].param.b_vfr_input = 0;
                x264_data[current_encoder].param.b_repeat_headers = 1;
                x264_data[current_encoder].param.b_annexb = 1;
                x264_data[current_encoder].param.b_aud = 1;
                x264_data[current_encoder].param.b_open_gop = 0;
                x264_data[current_encoder].param.b_sliced_threads = 0;

                x264_data[current_encoder].param.rc.i_lookahead = 15;

                // some people may want the filler bits, but for HLS/DASH, it's just extra baggage to carry around
                x264_data[current_encoder].param.rc.b_filler = 0; // no filler- less bits
                x264_data[current_encoder].param.rc.i_aq_mode = X264_AQ_VARIANCE;
                x264_data[current_encoder].param.rc.f_aq_strength = 1.0;
                x264_data[current_encoder].param.rc.b_mb_tree = 1;
                x264_data[current_encoder].param.rc.i_rc_method = X264_RC_ABR;
                x264_data[current_encoder].param.rc.i_bitrate = core->cd->transvideo_info[current_encoder].video_bitrate;
                x264_data[current_encoder].param.rc.i_vbv_buffer_size = core->cd->transvideo_info[current_encoder].video_bitrate;
                x264_data[current_encoder].param.rc.i_vbv_max_bitrate = core->cd->transvideo_info[current_encoder].video_bitrate;

                x264_data[current_encoder].param.analyse.i_me_method = X264_ME_HEX;
                x264_data[current_encoder].param.analyse.i_subpel_refine = 3;
                x264_data[current_encoder].param.analyse.b_dct_decimate = 1;

                x264_data[current_encoder].param.i_nal_hrd = X264_NAL_HRD_CBR;
                x264_data[current_encoder].param.i_threads = 0;
                x264_data[current_encoder].param.i_lookahead_threads = 0;
                x264_data[current_encoder].param.i_slice_count = 0;
                x264_data[current_encoder].param.i_frame_reference = 1;

                if (core->cd->transvideo_info[0].aspect_num > 0 &&
                    core->cd->transvideo_info[0].aspect_den > 0) {
                    sar_width = output_height*core->cd->transvideo_info[0].aspect_num;
                    sar_height = output_width*core->cd->transvideo_info[0].aspect_den;
                } else {
                    sar_width = output_height*msg->aspect_num;
                    sar_height = output_width*msg->aspect_den;
                }
                if (sar_width > 0 && sar_height > 0) {
                    x264_data[current_encoder].param.vui.i_sar_width = sar_width;
                    x264_data[current_encoder].param.vui.i_sar_height = sar_height;
                }

                // let's put the idr frames at second boundaries - make this configurable later
                double fps = 30.0;
                if (msg->fps_den > 0) {
                    fps = (double)((double)msg->fps_num / (double)msg->fps_den);
                }
                x264_data[current_encoder].param.i_keyint_max = (int)((double)fps + 0.5);
                x264_data[current_encoder].param.i_keyint_min = 1;
                x264_data[current_encoder].param.i_fps_num = msg->fps_num;
                x264_data[current_encoder].param.i_fps_den = msg->fps_den;

                // i was not able to get alignment with scene change detection enabled
                // because at different resolutions it seems that the scene change
                // detector gets triggered and other resolutions it doesn't
                // so we end up with different idr sync points
                // to solve the scenecut issue, we can put the scene detector
                // outside of the encoder and then tell all of the encoders
                // at once to put in an idr sync frame
                // anyone want to write some scene change detection code?
                x264_data[current_encoder].param.i_scenecut_threshold = 0;  // need to keep abr alignment
                x264_data[current_encoder].param.i_bframe_adaptive = 0;
                x264_data[current_encoder].param.i_bframe_pyramid = 0;

                // we set a base quality above and then modify things here
                if (core->cd->transvideo_info[current_encoder].encoder_quality == ENCODER_QUALITY_BASIC) {
                    x264_data[current_encoder].param.analyse.i_me_method = X264_ME_DIA;
                    x264_data[current_encoder].param.analyse.i_subpel_refine = 3;
                    x264_data[current_encoder].param.i_frame_reference = 1;
                    x264_data[current_encoder].param.rc.i_lookahead = (int)(fps+0.5)/2;
                    x264_data[current_encoder].param.i_bframe = 0;
                } else if (core->cd->transvideo_info[current_encoder].encoder_quality == ENCODER_QUALITY_STREAMING) {
                    x264_data[current_encoder].param.analyse.i_me_method = X264_ME_HEX;
                    x264_data[current_encoder].param.analyse.i_subpel_refine = 5;
                    x264_data[current_encoder].param.i_frame_reference = 3;
                    x264_data[current_encoder].param.rc.i_lookahead = (int)(fps+0.5);
                    x264_data[current_encoder].param.i_bframe = 2;
                } else if (core->cd->transvideo_info[current_encoder].encoder_quality == ENCODER_QUALITY_BROADCAST) {
                    x264_data[current_encoder].param.analyse.i_me_method = X264_ME_HEX;
                    x264_data[current_encoder].param.analyse.i_subpel_refine = 7;
                    x264_data[current_encoder].param.i_frame_reference = 3;
                    x264_data[current_encoder].param.rc.i_lookahead = (int)(fps+0.5);
                    x264_data[current_encoder].param.i_bframe = 2;
                } else {  // ENCODER_QUALITY_CRAZY
                    x264_data[current_encoder].param.analyse.i_me_method = X264_ME_UMH;
                    x264_data[current_encoder].param.analyse.i_subpel_refine = 8;
                    x264_data[current_encoder].param.i_frame_reference = 5;
                    x264_data[current_encoder].param.rc.i_lookahead = (int)(fps+0.5)*2;
                    x264_data[current_encoder].param.i_bframe = 3;
                }

                x264_data[current_encoder].param.b_sliced_threads = 0;

//#define ENABLE_SLICED_THREADS
#if defined(ENABLE_SLICED_THREADS)
                x264_data[current_encoder].param.b_sliced_threads = 1;

                // these values were derived from testing across several different cloud instances- seem
                if (output_width > 960 && output_height > 540) {
                    x264_data[current_encoder].param.i_threads = 12;
                    x264_data[current_encoder].param.i_slice_count = 8;
                } else {
                    x264_data[current_encoder].param.i_threads = 6;
                    x264_data[current_encoder].param.i_slice_count = 4;
                }
#else
                //tradeoff-quality vs performance
                if (output_width > 960 && output_height > 540) {
                    x264_data[current_encoder].param.i_threads = 12;
                } else {
                    x264_data[current_encoder].param.i_threads = 6;
                }
#endif

                if (core->cd->transvideo_info[current_encoder].encoder_profile == ENCODER_PROFILE_BASE) {
                    x264_param_apply_profile(&x264_data[current_encoder].param,"baseline");
                    x264_data[current_encoder].param.b_cabac = 0;
                    x264_data[current_encoder].param.i_cqm_preset = X264_CQM_FLAT;
                    x264_data[current_encoder].param.i_bframe = 0;
                    x264_data[current_encoder].param.analyse.i_weighted_pred = 0;
                    x264_data[current_encoder].param.analyse.b_weighted_bipred = 0;
                    x264_data[current_encoder].param.analyse.b_transform_8x8 = 0;
                } else if (core->cd->transvideo_info[current_encoder].encoder_profile == ENCODER_PROFILE_MAIN) {
                    x264_param_apply_profile(&x264_data[current_encoder].param,"main");
                    x264_data[current_encoder].param.i_cqm_preset = X264_CQM_FLAT;
                    x264_data[current_encoder].param.analyse.b_transform_8x8 = 0;
                } else {
                    x264_param_apply_profile(&x264_data[current_encoder].param,"high");
                    x264_data[current_encoder].param.analyse.b_transform_8x8 = 1;
                }

                x264_data[current_encoder].h = x264_encoder_open(&x264_data[current_encoder].param);
            }

            if (!video_encode_thread_running) {
                while (msg) {
                    if (msg) {
                        memory_return(core->raw_video_pool, msg->buffer);
                        msg->buffer = NULL;
                        memory_return(core->fillet_msg_pool, msg);
                        msg = NULL;
                    }
                    msg = (dataqueue_message_struct*)dataqueue_take_back(core->encodevideo->input_queue[current_encoder]);
                }
                goto cleanup_video_encode_thread;
            }

            uint8_t *video;
            int output_size;
            int64_t pts;
            int64_t dts;
            int splice_point = 0;
            int64_t splice_duration = 0;
            int64_t splice_duration_remaining = 0;

            video = msg->buffer;
            splice_point = msg->splice_point;
            splice_duration = msg->splice_duration;
            splice_duration_remaining = msg->splice_duration_remaining;

            // if splice point- force intra frame
            // when duration is done- also force intra frame
            // keep track of splice duration remaining
            fprintf(stderr,"VIDEO ENCODER: SPLICE POINT:%d  SPLICE DURATION: %ld  SPLICE DURATION REMAINING: %ld\n",
                    splice_point,
                    splice_duration,
                    splice_duration_remaining);

            x264_data[current_encoder].pic.i_pts = msg->pts;
            x264_data[current_encoder].pic.i_dts = msg->dts;

            encoder_opaque_struct *opaque_data = (encoder_opaque_struct*)malloc(sizeof(encoder_opaque_struct));
            if (opaque_data) {
                opaque_data->splice_point = splice_point;
                opaque_data->splice_duration = splice_duration;
                opaque_data->splice_duration_remaining = splice_duration_remaining;
                opaque_data->frame_count_pts = x264_data[current_encoder].frame_count_pts;
            }

            x264_data[current_encoder].pic.opaque = (void*)opaque_data;

            memcpy(x264_data[current_encoder].pic.img.plane[0],
                   video,
                   x264_data[current_encoder].y_size);
            memcpy(x264_data[current_encoder].pic.img.plane[1],
                   video + x264_data[current_encoder].y_size,
                   x264_data[current_encoder].uv_size);
            memcpy(x264_data[current_encoder].pic.img.plane[2],
                   video + x264_data[current_encoder].y_size + x264_data[current_encoder].uv_size,
                   x264_data[current_encoder].uv_size);

            if (msg->caption_buffer) {
//#define DISABLE_CAPTIONS
#if !defined(DISABLE_CAPTIONS)
                uint8_t *caption_buffer;
                caption_buffer = (uint8_t*)malloc(MAX_SEI_PAYLOAD_SIZE);
                if (caption_buffer) {
                    memset(caption_buffer, 0, MAX_SEI_PAYLOAD_SIZE);
                    x264_data[current_encoder].pic.extra_sei.payloads = (x264_sei_payload_t*)malloc(sizeof(x264_sei_payload_t));
                    //check
                    if (x264_data[current_encoder].pic.extra_sei.payloads) {
                        memset(x264_data[current_encoder].pic.extra_sei.payloads, 0, sizeof(x264_sei_payload_t));
                        caption_buffer[0] = 0xb5;
                        caption_buffer[1] = 0x00;
                        caption_buffer[2] = 0x31;
                        if (msg->caption_size > MAX_SEI_PAYLOAD_SIZE) {
                            msg->caption_size = MAX_SEI_PAYLOAD_SIZE;
                            // log truncation
                        }
                        //fprintf(stderr,"status: encoding caption buffer\n");
                        memcpy(caption_buffer+3, msg->caption_buffer, msg->caption_size);
                        x264_data[current_encoder].pic.extra_sei.payloads[0].payload = caption_buffer;
                        x264_data[current_encoder].pic.extra_sei.payloads[0].payload_size = msg->caption_size+3;
                        x264_data[current_encoder].pic.extra_sei.num_payloads = 1;
                        x264_data[current_encoder].pic.extra_sei.payloads[0].payload_type = 4;
                        x264_data[current_encoder].pic.extra_sei.sei_free = free;
                    } else {
                        free(caption_buffer);
                        // fail
                        fprintf(stderr,"error: unable to allocate memory for sei payload (for captions)\n");
                    }
                } else {
                    // fail
                    fprintf(stderr,"error: unable to allocate memory for caption_buffer\n");
                }
                caption_buffer = NULL;
#endif // DISABLE_CAPTIONS
                free(msg->caption_buffer);
                msg->caption_buffer = NULL;
                msg->caption_size = 0;
            } else {
                x264_data[current_encoder].pic.extra_sei.num_payloads = 0;
                x264_data[current_encoder].pic.extra_sei.payloads = NULL;
            }

            if (splice_point == SPLICE_CUE_OUT || splice_point == SPLICE_CUE_IN) {
                syslog(LOG_INFO,"SCTE35(%d)- INSERTING IDR FRAME DURING SPLICE POINT: %d\n",
                       current_encoder, splice_point);
                x264_data[current_encoder].pic.i_type = X264_TYPE_IDR;
            } else {
                x264_data[current_encoder].pic.i_type = X264_TYPE_AUTO;
            }

            output_size = x264_encoder_encode(x264_data[current_encoder].h,
                                              &x264_data[current_encoder].nal,
                                              &x264_data[current_encoder].i_nal,
                                              &x264_data[current_encoder].pic,
                                              &x264_data[current_encoder].pic_out);

            x264_data[current_encoder].frame_count_pts++;

            if (x264_data[current_encoder].i_nal > 0) {
                uint8_t *nal_buffer;
                double output_fps = (double)30000.0/(double)1001.0;
                double ticks_per_frame_double = (double)90000.0/(double)output_fps;
                video_stream_struct *vstream = (video_stream_struct*)core->source_video_stream[0].video_stream;  // only one source stream

                if (core->video_encode_time_set == 0) {
                    core->video_encode_time_set = 1;
                    clock_gettime(CLOCK_MONOTONIC, &core->video_encode_time);
                }

                nal_buffer = (uint8_t*)memory_take(core->compressed_video_pool, output_size*2);
                if (!nal_buffer) {
                    fprintf(stderr,"FATAL ERROR: unable to obtain nal_buffer!!\n");
                    send_direct_error(core, SIGNAL_DIRECT_ERROR_NALPOOL, "Out of NAL Buffers (H264) - Restarting Service");
                    _Exit(0);
                }
                memcpy(nal_buffer, x264_data[current_encoder].nal->p_payload, output_size);

                if (x264_data[current_encoder].param.i_fps_den > 0) {
                    output_fps = (double)x264_data[current_encoder].param.i_fps_num / (double)x264_data[current_encoder].param.i_fps_den;
                    if (output_fps > 0) {
                        ticks_per_frame_double = (double)90000.0/(double)output_fps;
                    }
                }

                /*
                  fprintf(stderr,"RECEIVED ENCODED FRAME OUTPUT:%d FRAME COUNT DISPLAY ORDER:%ld  CURRENT TS:%ld\n",
                  output_size,
                  (int64_t)x264_data[current_encoder].pic_out.opaque,
                  (int64_t)x264_data[current_encoder].pic_out.opaque * (int64_t)ticks_per_frame_double + (int64_t)vstream->first_timestamp);
                */

                pts = x264_data[current_encoder].pic_out.i_pts;
                dts = x264_data[current_encoder].pic_out.i_dts;

                encoder_opaque_struct *opaque_output = (encoder_opaque_struct*)x264_data[current_encoder].pic_out.opaque;
                int64_t opaque_int64 = 0;
                if (opaque_output) {
                    splice_point = opaque_output->splice_point;
                    splice_duration = opaque_output->splice_duration;
                    splice_duration_remaining = opaque_output->splice_duration_remaining;
                    opaque_int64 = opaque_output->frame_count_pts;
                    //int64_t opaque_int64 = (int64_t)x264_data[current_encoder].pic_out.opaque;
                }
                double opaque_double = (double)opaque_int64;

                pts = (int64_t)((double)opaque_double * (double)ticks_per_frame_double) + (int64_t)vstream->first_timestamp;
                dts = (int64_t)((double)x264_data[current_encoder].frame_count_dts * (double)ticks_per_frame_double) + (int64_t)vstream->first_timestamp;

                x264_data[current_encoder].frame_count_dts++;
                video_sink_frame_callback(core, nal_buffer, output_size, pts, dts, current_encoder, splice_point, splice_duration, splice_duration_remaining);
            }

            if (msg) {
                //check for orphaned caption buffer
                memory_return(core->raw_video_pool, msg->buffer);
                msg->buffer = NULL;
                memory_return(core->fillet_msg_pool, msg);
                msg = NULL;
            }
        }
    }
cleanup_video_encode_thread:
    // since we're using malloc and not buffer pools yet
    // it's possible for some of the caption data to get
    // orphaned if we don't flush out the encoder
    // planning to switch over to pools so this will not become an issue
    if (x264_data[current_encoder].h) {
        x264_encoder_close(x264_data[current_encoder].h);
    }

    return NULL;
}
#endif // ENABLE_GPU

void copy_image_data_to_ffmpeg(uint8_t *source, int source_width, int source_height, AVFrame *source_frame)
{
    int row;
    uint8_t *ysrc = source;
    uint8_t *usrc = ysrc + (source_width * source_height);
    uint8_t *vsrc = usrc + ((source_width/2) * (source_height/2));
    uint8_t *ydst = (uint8_t*)source_frame->data[0];
    uint8_t *udst = (uint8_t*)source_frame->data[1];
    uint8_t *vdst = (uint8_t*)source_frame->data[2];
    int shhalf = source_height / 2;
    int swhalf = source_width / 2;
    for (row = 0; row < source_height; row++) {
        memcpy(ydst, ysrc, source_frame->linesize[0]);
        ysrc += source_width;
        ydst += source_frame->linesize[0];
    }
    for (row = 0; row < shhalf; row++) {
        memcpy(udst, usrc, source_frame->linesize[1]);
        usrc += swhalf;
        udst += source_frame->linesize[1];
    }
    for (row = 0; row < shhalf; row++) {
        memcpy(vdst, vsrc, source_frame->linesize[2]);
        vsrc += swhalf;
        vdst += source_frame->linesize[2];
    }
}

void *video_scale_thread(void *context)
{
    fillet_app_struct *core = (fillet_app_struct*)context;
    dataqueue_message_struct *msg;
    dataqueue_message_struct *encode_msg;
    struct SwsContext *output_scaler[MAX_TRANS_OUTPUTS];
    scale_struct scaled_output[MAX_TRANS_OUTPUTS];
    int num_outputs = core->cd->num_outputs;
    int current_output;
    AVFrame *deinterlaced_frame = NULL;
    int i;

    deinterlaced_frame = av_frame_alloc();

    for (i = 0; i < MAX_TRANS_OUTPUTS; i++) {
        output_scaler[i] = NULL;
        scaled_output[i].output_data[0] = NULL;
    }

    while (video_scale_thread_running) {
        msg = (dataqueue_message_struct*)dataqueue_take_back(core->scalevideo->input_queue);
        while (!msg && video_scale_thread_running) {
            usleep(1000);
            msg = (dataqueue_message_struct*)dataqueue_take_back(core->scalevideo->input_queue);
        }

        if (!video_scale_thread_running) {
            while (msg) {
                if (msg) {
                    memory_return(core->raw_video_pool, msg->buffer);
                    msg->buffer = NULL;
                    memory_return(core->fillet_msg_pool, msg);
                    msg = NULL;
                }
                msg = (dataqueue_message_struct*)dataqueue_take_back(core->scalevideo->input_queue);
            }
            goto cleanup_video_scale_thread;
        }

        if (msg) {
            uint8_t *deinterlaced_input = msg->buffer;
            uint8_t *deinterlaced_buffer;
            int width = msg->width;
            int height = msg->height;

            deinterlaced_frame->data[0] = deinterlaced_input;
            deinterlaced_frame->data[1] = deinterlaced_frame->data[0] + (width*height);
            deinterlaced_frame->data[2] = deinterlaced_frame->data[1] + ((width/2)*(height/2));
            deinterlaced_frame->linesize[0] = width;
            deinterlaced_frame->linesize[1] = width/2;
            deinterlaced_frame->linesize[2] = width/2;

            for (current_output = 0; current_output < num_outputs; current_output++) {
                int output_width = core->cd->transvideo_info[current_output].width;
                int output_height = core->cd->transvideo_info[current_output].height;
                int video_frame_size = 3 * output_height * output_width / 2;
                int row;

                if (output_scaler[current_output] == NULL) {
                    output_scaler[current_output] = sws_getContext(width, height, AV_PIX_FMT_YUV420P,
                                                                   output_width, output_height, AV_PIX_FMT_YUV420P,
                                                                   SWS_BICUBIC, NULL, NULL, NULL);
                    av_image_alloc(scaled_output[current_output].output_data,
                                   scaled_output[current_output].output_stride,
                                   output_width, output_height, AV_PIX_FMT_YUV420P, 1);
                }

                fprintf(stderr,"video_scale_thread: output=%d, scaling output to %d x %d\n",
                        current_output,
                        output_width,
                        output_height);

                sws_scale(output_scaler[current_output],
                          (const uint8_t * const*)deinterlaced_frame->data,
                          deinterlaced_frame->linesize,
                          0, height,
                          scaled_output[current_output].output_data,
                          scaled_output[current_output].output_stride);

                uint8_t *outputy;
                uint8_t *outputu;
                uint8_t *outputv;
                uint8_t *sourcey;
                uint8_t *sourceu;
                uint8_t *sourcev;
                int stridey;
                int strideu;
                int stridev;
                int whalf = output_width/2;
                int hhalf = output_height/2;

                deinterlaced_buffer = (uint8_t*)memory_take(core->raw_video_pool, video_frame_size);
                if (!deinterlaced_buffer) {
                    char errormsg[MAX_MESSAGE_SIZE];
                    int n;
                    int smp = memory_unused(core->fillet_msg_pool);
                    int fmp = memory_unused(core->frame_msg_pool);
                    int cvp = memory_unused(core->compressed_video_pool);
                    int cap = memory_unused(core->compressed_audio_pool);
                    int s35p = memory_unused(core->scte35_pool);
                    int rvp = memory_unused(core->raw_video_pool);
                    int rap = memory_unused(core->raw_audio_pool);
                    int vdecfw = dataqueue_get_size(core->transvideo->input_queue);
                    int vdinfw = dataqueue_get_size(core->preparevideo->input_queue);
                    int vefw = 0;
                    for (n = 0; n < core->cd->num_outputs; n++) {
                        vefw += dataqueue_get_size(core->encodevideo->input_queue[n]);
                    }
                    snprintf(errormsg, MAX_MESSAGE_SIZE-1, "Out of Uncompressed Video Buffers (SCALE), Restarting Service, vefw=%d, vdinfw=%d, vdecfw=%d, rap=%d, rvp=%d, s35p=%d, cap=%d, cvp=%d, fmp=%d, smp=%d",
                             vefw, vdinfw, vdecfw, rap, rvp, s35p, cap, cvp, fmp, smp);
                    send_direct_error(core, SIGNAL_DIRECT_ERROR_RAWPOOL, errormsg);
                    _Exit(0);
                }
                sourcey = (uint8_t*)scaled_output[current_output].output_data[0];
                sourceu = (uint8_t*)scaled_output[current_output].output_data[1];
                sourcev = (uint8_t*)scaled_output[current_output].output_data[2];
                stridey = scaled_output[current_output].output_stride[0];
                strideu = scaled_output[current_output].output_stride[1];
                stridev = scaled_output[current_output].output_stride[2];
                outputy = (uint8_t*)deinterlaced_buffer;
                outputu = (uint8_t*)outputy + (output_width*output_height);
                outputv = (uint8_t*)outputu + (whalf*hhalf);
                for (row = 0; row < output_height; row++) {
                    memcpy(outputy, sourcey, output_width);
                    outputy += output_width;
                    sourcey += stridey;
                }
                for (row = 0; row < hhalf; row++) {
                    memcpy(outputu, sourceu, whalf);
                    outputu += whalf;
                    sourceu += strideu;
                }
                for (row = 0; row < hhalf; row++) {
                    memcpy(outputv, sourcev, whalf);
                    outputv += whalf;
                    sourcev += stridev;
                }

                encode_msg = (dataqueue_message_struct*)memory_take(core->fillet_msg_pool, sizeof(dataqueue_message_struct));
                if (!encode_msg) {
                    fprintf(stderr,"FATAL ERROR: unable to obtain encode_msg!!\n");
                    send_direct_error(core, SIGNAL_DIRECT_ERROR_MSGPOOL, "Out of Message Buffers (SCALE) - Restarting Service");
                    _Exit(0);
                }
                encode_msg->buffer = deinterlaced_buffer;
                encode_msg->buffer_size = video_frame_size;
                encode_msg->pts = msg->pts;
                encode_msg->dts = msg->dts;
                encode_msg->interlaced = 0;
                encode_msg->tff = 1;
                encode_msg->fps_num = msg->fps_num;
                encode_msg->fps_den = msg->fps_den;
                encode_msg->aspect_num = msg->aspect_num;
                encode_msg->aspect_den = msg->aspect_den;
                encode_msg->width = output_width;
                encode_msg->height = output_height;
                encode_msg->stream_index = current_output;
                encode_msg->splice_point = msg->splice_point;
                encode_msg->splice_duration = msg->splice_duration;
                encode_msg->splice_duration_remaining = msg->splice_duration_remaining;
                encode_msg->caption_size = msg->caption_size;
                encode_msg->caption_timestamp = msg->caption_timestamp;
                if (encode_msg->caption_size > 0) {
                    if (current_output == num_outputs-1) {
                        encode_msg->caption_buffer = msg->caption_buffer;
                    } else {
                        encode_msg->caption_buffer = (uint8_t*)malloc(encode_msg->caption_size);
                        memcpy(encode_msg->caption_buffer, msg->caption_buffer, encode_msg->caption_size);
                    }
                } else {
                    encode_msg->caption_buffer = NULL;
                }

                if (encode_msg) {
                    dataqueue_put_front(core->encodevideo->input_queue[current_output], encode_msg);
                    encode_msg = NULL;
                }
            }//current_output loop
            memory_return(core->raw_video_pool, msg->buffer);
            msg->buffer = NULL;
            memory_return(core->fillet_msg_pool, msg);
            msg = NULL;
        }//msg
    }//thread
cleanup_video_scale_thread:
    for (current_output = 0; current_output < num_outputs; current_output++) {
        if (scaled_output[current_output].output_data[0]) {
            av_freep(&scaled_output[current_output].output_data[0]);
        }
        sws_freeContext(output_scaler[current_output]);
    }

    av_frame_free(&deinterlaced_frame);

    return NULL;
}

void *video_monitor_thread(void *context)
{
    fillet_app_struct *core = (fillet_app_struct*)context;
    dataqueue_message_struct *msg;
    dataqueue_message_struct *prepare_msg;
    int monitor_ready = 0;
    int monitor_width = 0;
    int monitor_height = 0;
    int monitor_num = 0;
    int monitor_den = 0;
    struct timespec monitor_start;
    struct timespec monitor_end;
    double video_monitor_time;
    int64_t total_video_monitor_dead_time = 0;
    int64_t dead_frames;
    int64_t total_dead_frames = 0;
    int i;
    uint8_t *output_video_frame = NULL;
    uint8_t *saved_video_frame = NULL;
    int video_frame_size = 0;
    double frame_delta;
    int64_t monitor_anchor_dts = 0;
    int64_t monitor_anchor_pts = 0;
    int monitor_tff = 0;
    int monitor_interlaced = 0;
    int monitor_aspect_num = 0;
    int monitor_aspect_den = 0;
    double dead_frame_drift;
    double expected_dead_frames = 0;
    int dead_frame_adjustment = 0;
    double dead_frames_actual;
    int64_t last_monitor_anchor_pts = 0;
    int64_t last_monitor_anchor_dts = 0;
    int dead_frames_triggered = 0;

    core->reinitialize_decoder = 0;
    while (video_monitor_thread_running) {
        clock_gettime(CLOCK_MONOTONIC, &monitor_start);
        msg = (dataqueue_message_struct*)dataqueue_take_back(core->monitorvideo->input_queue);
        while (!msg && video_monitor_thread_running) {
            usleep(1000);
            if (monitor_ready) {
                clock_gettime(CLOCK_MONOTONIC, &monitor_end);
                video_monitor_time = (double)time_difference(&monitor_end, &monitor_start);
#define VIDEO_MONITOR_DEAD_TIME 1000000
                if (video_monitor_time >= VIDEO_MONITOR_DEAD_TIME) {
                    char fillermsg[MAX_MESSAGE_SIZE];

                    clock_gettime(CLOCK_MONOTONIC, &monitor_start);

                    video_monitor_time = VIDEO_MONITOR_DEAD_TIME;
                    total_video_monitor_dead_time += video_monitor_time;

                    frame_delta = (double)90000.0 / ((double)monitor_num / (double)monitor_den);

                    dead_frame_drift = (double)expected_dead_frames - (double)total_dead_frames;
                    if (dead_frame_drift >= 1) {
                        dead_frame_adjustment = 1;
                    } else {
                        dead_frame_adjustment = 0;
                    }

                    dead_frames_actual = (double)((double)video_monitor_time / ((double)1000000.0 / ((double)monitor_num / (double)monitor_den)));
                    expected_dead_frames += dead_frames_actual;
                    dead_frames = (int64_t)dead_frames_actual + dead_frame_adjustment;

                    dead_frames_triggered = 1;
                    core->reinitialize_decoder = 1;

                    fprintf(stderr,"video_monitor_thread: inserting dead filler frames=%ld  video_monitor_time=%f frame_delta=%f\n",
                            dead_frames, video_monitor_time, frame_delta);

                    snprintf(fillermsg, MAX_MESSAGE_SIZE-1, "Signal Loss Inserting %ld Filler Video Frames, Frame Delta %f, Framerate %d/%d",
                             dead_frames, frame_delta, monitor_num, monitor_den);
                    send_signal(core, SIGNAL_FRAME_VIDEO_FILLER, fillermsg);

                    for (i = 0; i < dead_frames; i++) {
                        total_dead_frames++;

                        video_frame_size = (monitor_width*monitor_height*3)/2;
                        output_video_frame = (uint8_t*)memory_take(core->raw_video_pool, video_frame_size);
                        if (!output_video_frame) {
                            char errormsg[MAX_MESSAGE_SIZE];
                            int n;
                            int smp = memory_unused(core->fillet_msg_pool);
                            int fmp = memory_unused(core->frame_msg_pool);
                            int cvp = memory_unused(core->compressed_video_pool);
                            int cap = memory_unused(core->compressed_audio_pool);
                            int s35p = memory_unused(core->scte35_pool);
                            int rvp = memory_unused(core->raw_video_pool);
                            int rap = memory_unused(core->raw_audio_pool);
                            int vdecfw = dataqueue_get_size(core->transvideo->input_queue);
                            int vdinfw = dataqueue_get_size(core->preparevideo->input_queue);
                            int vefw = 0;
                            for (n = 0; n < core->cd->num_outputs; n++) {
                                vefw += dataqueue_get_size(core->encodevideo->input_queue[n]);
                            }
                            snprintf(errormsg, MAX_MESSAGE_SIZE-1, "Out of Uncompressed Video Buffers (MONITOR), Restarting Service, vefw=%d, vdinfw=%d, vdecfw=%d, rap=%d, rvp=%d, s35p=%d, cap=%d, cvp=%d, fmp=%d, smp=%d",
                                     vefw, vdinfw, vdecfw, rap, rvp, s35p, cap, cvp, fmp, smp);
                            send_direct_error(core, SIGNAL_DIRECT_ERROR_RAWPOOL, errormsg);
                            _Exit(0);
                        }

                        prepare_msg = (dataqueue_message_struct*)memory_take(core->fillet_msg_pool, sizeof(dataqueue_message_struct));
                        if (prepare_msg) {
                            memcpy(output_video_frame, saved_video_frame, video_frame_size);

                            prepare_msg->buffer = output_video_frame;
                            prepare_msg->buffer_size = video_frame_size;

                            prepare_msg->pts = (int64_t)((double)monitor_anchor_pts+((double)total_dead_frames*(double)frame_delta));
                            prepare_msg->dts = (int64_t)((double)monitor_anchor_dts+((double)total_dead_frames*(double)frame_delta));
                            last_monitor_anchor_pts = prepare_msg->pts;
                            last_monitor_anchor_dts = prepare_msg->dts;
                            prepare_msg->tff = monitor_tff;

                            prepare_msg->interlaced = monitor_interlaced;
                            prepare_msg->caption_buffer = NULL;
                            prepare_msg->caption_size = 0;
                            prepare_msg->caption_timestamp = 0;

                            prepare_msg->fps_num = monitor_num;
                            prepare_msg->fps_den = monitor_den;

                            prepare_msg->aspect_num = monitor_aspect_num;
                            prepare_msg->aspect_den = monitor_aspect_den;
                            prepare_msg->width = monitor_width;
                            prepare_msg->height = monitor_height;
                            prepare_msg->source_discontinuity = 0;

                            // fix this grrr....
                            prepare_msg->splice_point = 0;//splice_point;
                            prepare_msg->splice_duration = 0;//splice_duration;
                            prepare_msg->splice_duration_remaining = 0;//splice_duration_remaining;

                            fprintf(stderr,"video_monitor_thread: total_dead_frames=%ld pts=%ld dts=%ld total=%ld\n",
                                    total_dead_frames,
                                    prepare_msg->pts,
                                    prepare_msg->dts,
                                    total_video_monitor_dead_time);
                            dataqueue_put_front(core->preparevideo->input_queue, prepare_msg);
                        } else {
                            fprintf(stderr,"FATAL ERROR: unable to obtain prepare_msg!!\n");
                            send_direct_error(core, SIGNAL_DIRECT_ERROR_MSGPOOL, "Out of Message Video Buffers (MONITOR) - Restarting Service");
                            _Exit(0);
                        }
                    }
                }
            }
            msg = (dataqueue_message_struct*)dataqueue_take_back(core->monitorvideo->input_queue);
        }

        if (!video_monitor_thread_running) {
            while (msg) {
                if (msg) {
                    if (msg->caption_buffer) {
                        free(msg->caption_buffer);
                        msg->caption_buffer = NULL;
                        msg->caption_size = 0;
                    }
                    memory_return(core->raw_video_pool, msg->buffer);
                    msg->buffer = NULL;
                    memory_return(core->fillet_msg_pool, msg);
                    msg = NULL;
                }
                msg = (dataqueue_message_struct*)dataqueue_take_back(core->monitorvideo->input_queue);
            }
            goto cleanup_video_monitor_thread;
        }

        if (msg) {
            int width = msg->width;
            int height = msg->height;

            total_dead_frames = 0;

            if (!monitor_ready || ((width != monitor_width) || (height != monitor_height))) {
                monitor_width = width;
                monitor_height = height;
                monitor_num = msg->fps_num;
                monitor_den = msg->fps_den;
                if (saved_video_frame) {
                    free(saved_video_frame);
                    saved_video_frame = NULL;
                }
                monitor_ready = 1;
            }
            if (monitor_ready) {
                if (!saved_video_frame) {
                    saved_video_frame = (uint8_t*)malloc((width*height*3)/2);
                }
                memcpy(saved_video_frame, msg->buffer, (width*height*3)/2);
            }
            monitor_anchor_dts = msg->dts;
            monitor_anchor_pts = msg->pts;
            monitor_tff = msg->tff;
            monitor_interlaced = msg->interlaced;
            monitor_aspect_num = msg->aspect_num;
            monitor_aspect_den = msg->aspect_den;

            fprintf(stderr,"video_monitor_thread: passing through monitored video frame: %dx%d, pts=%ld, dts=%ld\n",
                    monitor_width,
                    monitor_height,
                    monitor_anchor_pts,
                    monitor_anchor_dts);

            if (dead_frames_triggered) {
                // this will trigger the deinterlacer to flush and reinitialize
                msg->source_discontinuity = 1;
                dead_frames_triggered = 0;
            } else {
                msg->source_discontinuity = 0;
            }

            dataqueue_put_front(core->preparevideo->input_queue, msg);
            last_monitor_anchor_pts = monitor_anchor_pts;
            last_monitor_anchor_dts = monitor_anchor_dts;
        }
    }
cleanup_video_monitor_thread:
    if (saved_video_frame) {
        free(saved_video_frame);
        saved_video_frame = NULL;
    }
    return NULL;
}

void *video_prepare_thread(void *context)
{
    fillet_app_struct *core = (fillet_app_struct*)context;
    dataqueue_message_struct *msg;
    dataqueue_message_struct *scale_msg;
    int deinterlacer_ready = 0;
    AVFilterContext *deinterlacer_source = NULL;
    AVFilterContext *deinterlacer_output = NULL;
    AVFilterGraph *deinterlacer = NULL;

    AVFilter *filter_source = (AVFilter*)avfilter_get_by_name("buffer");
    AVFilter *filter_output = (AVFilter*)avfilter_get_by_name("buffersink");
    AVFilterInOut *filter_inputs = NULL;
    AVFilterInOut *filter_outputs = NULL;
    enum AVPixelFormat pix_fmts[] = { AV_PIX_FMT_YUV420P };
    AVFrame *source_frame = NULL;
    AVFrame *deinterlaced_frame = NULL;
    uint8_t *output_data[4];
    int output_stride[4];
    int row;
    int i;
    uint8_t *deinterlaced_buffer = NULL;
    int num_outputs = core->cd->num_outputs;
    struct SwsContext *thumbnail_scaler = NULL;
    scale_struct *thumbnail_output = NULL;
    int64_t deinterlaced_frame_count = 0;
    int64_t sync_frame_count = 0;
    double fps = 30.0;
    opaque_struct *opaque_data = NULL;
    int thumbnail_count = 0;
    int av_sync_compromised = 0;

    source_frame = av_frame_alloc();
    deinterlaced_frame = av_frame_alloc();

#define MAX_SETTINGS_SIZE 256
    char settings[MAX_SETTINGS_SIZE];

    while (video_prepare_thread_running) {
        msg = (dataqueue_message_struct*)dataqueue_take_back(core->preparevideo->input_queue);
        while (!msg && video_prepare_thread_running) {
            usleep(1000);
            msg = (dataqueue_message_struct*)dataqueue_take_back(core->preparevideo->input_queue);
        }

        if (!video_prepare_thread_running) {
            while (msg) {
                if (msg) {
                    memory_return(core->raw_video_pool, msg->buffer);
                    msg->buffer = NULL;
                    memory_return(core->fillet_msg_pool, msg);
                    msg = NULL;
                }
                msg = (dataqueue_message_struct*)dataqueue_take_back(core->preparevideo->input_queue);
            }
            goto cleanup_video_prepare_thread;
        }

        if (msg) {
            int width = msg->width;
            int height = msg->height;
            int ready = 0;

            if (msg->source_discontinuity) {
                if (deinterlacer_ready) {
                    av_free(deinterlacer->scale_sws_opts);
                    deinterlacer->scale_sws_opts = NULL;
                    avfilter_graph_free(&deinterlacer);
                    deinterlacer_ready = 0;
                    av_freep(&output_data[0]);
                    sws_freeContext(thumbnail_scaler);
                    thumbnail_scaler = NULL;
                }
                msg->source_discontinuity = 0;
            }

            // deinterlace the frame and scale it to the number of outputs
            // required and then feed the encoder input queues
            // for now I'm doing a blanket deinterlace
            // this is based on ffmpeg examples found on various sites online
            // there is not a lot of documentation on using the filter graphs
            if (!deinterlacer_ready) {
                char scaler_params[32];
                char *scaler_flags;

                filter_inputs = avfilter_inout_alloc();
                filter_outputs = avfilter_inout_alloc();

                //!! optimization can be done here to set to the max output height

                snprintf(settings, MAX_SETTINGS_SIZE-1,"video_size=%dx%d:pix_fmt=%d:time_base=%d/%d:pixel_aspect=%d/%d:colorspace=%d:range=%d:frame_rate=%d/%d",
                         width,
                         height,
                         AV_PIX_FMT_YUV420P,
                         msg->fps_den,
                         msg->fps_num,
                         msg->aspect_num,
                         msg->aspect_den,
                         AVCOL_SPC_BT709,
                         AVCOL_RANGE_JPEG,
                         msg->fps_num,
                         msg->fps_den);
                deinterlacer = avfilter_graph_alloc();
                deinterlacer->nb_threads = 4;

                fprintf(stderr,"settings=%s\n", settings);

                avfilter_graph_create_filter(&deinterlacer_source,
                                             filter_source,
                                             "in",
                                             settings,
                                             NULL,
                                             deinterlacer);
                avfilter_graph_create_filter(&deinterlacer_output,
                                             filter_output,
                                             "out",
                                             NULL,
                                             pix_fmts,
                                             deinterlacer);
                //av_free(params);
                //params = NULL;

                filter_inputs->name = av_strdup("out");
                filter_inputs->filter_ctx = deinterlacer_output;
                filter_inputs->pad_idx = 0;
                filter_inputs->next = NULL;

                filter_outputs->name = av_strdup("in");
                filter_outputs->filter_ctx = deinterlacer_source;
                filter_outputs->pad_idx = 0;
                filter_outputs->next = NULL;

                memset(scaler_params,0,sizeof(scaler_params));
                sprintf(scaler_params,"flags=%d",SWS_BICUBIC);
                scaler_flags = av_strdup(scaler_params);

                deinterlacer->scale_sws_opts = av_malloc(strlen(scaler_flags)+1);
                strcpy(deinterlacer->scale_sws_opts, scaler_flags);
                free(scaler_flags);

                if (msg->fps_num == 60000 || msg->fps_num == 50000) {
                    const char *filter_marked = "yadif=0:-1:1"; //best tradeoff performance vs quality
                    avfilter_graph_parse_ptr(deinterlacer,
                                             filter_marked,
                                             &filter_inputs,
                                             &filter_outputs,
                                             NULL);

                } else {
                    const char *filter_all = "yadif=0:-1:0"; //best tradeoff performance vs quality
                    avfilter_graph_parse_ptr(deinterlacer,
                                             filter_all,
                                             &filter_inputs,
                                             &filter_outputs,
                                             NULL);

                }
                avfilter_graph_config(deinterlacer,
                                      NULL);

                avfilter_inout_free(&filter_inputs);
                avfilter_inout_free(&filter_outputs);

                av_image_alloc(output_data,
                               output_stride,
                               width, height,
                               AV_PIX_FMT_YUV420P,
                               1);

                source_frame->pts = AV_NOPTS_VALUE;
                source_frame->pkt_dts = AV_NOPTS_VALUE;
                source_frame->pkt_duration = 0;
                source_frame->pkt_pos = -1;
                source_frame->pkt_size = -1;
                source_frame->key_frame = -1;
                source_frame->sample_aspect_ratio = (AVRational){1,1};
                source_frame->format = 0;
                source_frame->extended_data = NULL;
                source_frame->color_primaries = AVCOL_PRI_BT709;
                source_frame->color_trc = AVCOL_TRC_BT709;
                source_frame->colorspace = AVCOL_SPC_BT709;
                source_frame->color_range = AVCOL_RANGE_JPEG;
                source_frame->chroma_location = AVCHROMA_LOC_UNSPECIFIED;
                source_frame->flags = 0;
                source_frame->data[0] = output_data[0];
                source_frame->data[1] = output_data[1];
                source_frame->data[2] = output_data[2];
                source_frame->data[3] = output_data[3];
                source_frame->linesize[0] = output_stride[0];
                source_frame->linesize[1] = output_stride[1];
                source_frame->linesize[2] = output_stride[2];
                source_frame->linesize[3] = output_stride[3];
                source_frame->channels = 0;
                source_frame->channel_layout = 0;

                deinterlacer_ready = 1;
            }
            source_frame->pts = msg->pts;
            source_frame->pkt_dts = msg->dts;
            fprintf(stderr,"video_prepare_thread: source_frame->pts=%ld source_frame->pkt_dts=%ld\n",
                    source_frame->pts, source_frame->pkt_dts);
            //source_frame->pkt_pts = msg->pts;
            source_frame->width = width;
            source_frame->height = height;
            source_frame->interlaced_frame = msg->interlaced;
            source_frame->top_field_first = msg->tff;

            if (!opaque_data && (msg->caption_buffer || msg->splice_point)) {
                opaque_data = (opaque_struct*)malloc(sizeof(opaque_struct));
                if (msg->caption_buffer) {
                    opaque_data->caption_buffer = msg->caption_buffer;
                    opaque_data->caption_size = msg->caption_size;
                    opaque_data->caption_timestamp = msg->caption_timestamp;
                } else {
                    opaque_data->caption_buffer = NULL;
                    opaque_data->caption_size = 0;
                    opaque_data->caption_timestamp = 0;
                }
                if (msg->splice_point) {
                    opaque_data->splice_point = msg->splice_point;
                    opaque_data->splice_duration = msg->splice_duration;
                    opaque_data->splice_duration_remaining = msg->splice_duration_remaining;
                } else {
                    opaque_data->splice_point = 0;
                    opaque_data->splice_duration = msg->splice_duration;
                    opaque_data->splice_duration_remaining = msg->splice_duration_remaining;
                }
                source_frame->opaque = (void*)opaque_data;
                opaque_data = NULL;
            } else {
                source_frame->opaque = NULL;
                opaque_data = NULL;
            }

            copy_image_data_to_ffmpeg(msg->buffer, width, height, source_frame);

            ready = av_buffersrc_add_frame_flags(deinterlacer_source,
                                                 source_frame,
                                                 AV_BUFFERSRC_FLAG_KEEP_REF);

            while (video_prepare_thread_running) {
                int retcode;
                int output_ready = 1;
                double av_sync_offset = 0;

                retcode = av_buffersink_get_frame(deinterlacer_output, deinterlaced_frame);
                if (retcode == AVERROR(EAGAIN) ||
                    retcode == AVERROR(AVERROR_EOF)) {
                    output_ready = 0;
                    break;
                }

                if (output_ready) {
                    // get deinterlaced frame
                    // deinterlaced_frame is where the video is located now
                    opaque_data = (opaque_struct*)deinterlaced_frame->opaque;
                    deinterlaced_frame->opaque = NULL;

                    if (thumbnail_scaler == NULL) {
                        thumbnail_scaler = sws_getContext(width, height, AV_PIX_FMT_YUV420P,
                                                          THUMBNAIL_WIDTH, THUMBNAIL_HEIGHT, AV_PIX_FMT_YUV420P,
                                                          SWS_BICUBIC, NULL, NULL, NULL);
                    }

                    thumbnail_count++;
                    if (thumbnail_count == 150) {
                        dataqueue_message_struct *thumbnail_msg;

                        thumbnail_output = (scale_struct*)malloc(sizeof(scale_struct));
                        av_image_alloc(thumbnail_output->output_data,
                                       thumbnail_output->output_stride,
                                       THUMBNAIL_WIDTH, THUMBNAIL_HEIGHT, AV_PIX_FMT_YUV420P, 1);

                        sws_scale(thumbnail_scaler,
                                  (const uint8_t * const*)deinterlaced_frame->data,
                                  deinterlaced_frame->linesize,
                                  0, height,
                                  thumbnail_output->output_data,
                                  thumbnail_output->output_stride);

                        thumbnail_msg = (dataqueue_message_struct*)memory_take(core->fillet_msg_pool, sizeof(dataqueue_message_struct));
                        if (!thumbnail_msg) {
                            fprintf(stderr,"FATAL ERROR: unable to obtain thumbnail_msg!!\n");
                            send_direct_error(core, SIGNAL_DIRECT_ERROR_MSGPOOL, "Out of Message Buffers (Thumbnail) - Restarting Service");
                            _Exit(0);
                        }
                        thumbnail_msg->buffer = thumbnail_output;
                        dataqueue_put_front(core->encodevideo->thumbnail_queue, thumbnail_msg);
                        thumbnail_output = NULL;
                        thumbnail_count = 0;
                    }

                    {
                        uint8_t *outputy;
                        uint8_t *outputu;
                        uint8_t *outputv;
                        uint8_t *sourcey;
                        uint8_t *sourceu;
                        uint8_t *sourcev;
                        int stridey;
                        int strideu;
                        int stridev;
                        int whalf = width / 2;
                        int hhalf = height / 2;
                        int video_frame_size = 3 * height * width / 2;

                        deinterlaced_buffer = (uint8_t*)memory_take(core->raw_video_pool, video_frame_size);
                        if (!deinterlaced_buffer) {
                            char errormsg[MAX_MESSAGE_SIZE];
                            int n;
                            int smp = memory_unused(core->fillet_msg_pool);
                            int fmp = memory_unused(core->frame_msg_pool);
                            int cvp = memory_unused(core->compressed_video_pool);
                            int cap = memory_unused(core->compressed_audio_pool);
                            int s35p = memory_unused(core->scte35_pool);
                            int rvp = memory_unused(core->raw_video_pool);
                            int rap = memory_unused(core->raw_audio_pool);
                            int vdecfw = dataqueue_get_size(core->transvideo->input_queue);
                            int vdinfw = dataqueue_get_size(core->preparevideo->input_queue);
                            int vefw = 0;
                            for (n = 0; n < core->cd->num_outputs; n++) {
                                vefw += dataqueue_get_size(core->encodevideo->input_queue[n]);
                            }
                            snprintf(errormsg, MAX_MESSAGE_SIZE-1, "Out of Uncompressed Video Buffers (PREP), Restarting Service, vefw=%d, vdinfw=%d, vdecfw=%d, rap=%d, rvp=%d, s35p=%d, cap=%d, cvp=%d, fmp=%d, smp=%d",
                                     vefw, vdinfw, vdecfw, rap, rvp, s35p, cap, cvp, fmp, smp);
                            send_direct_error(core, SIGNAL_DIRECT_ERROR_RAWPOOL, errormsg);
                            _Exit(0);
                        }
                        sourcey = (uint8_t*)deinterlaced_frame->data[0];
                        sourceu = (uint8_t*)deinterlaced_frame->data[1];
                        sourcev = (uint8_t*)deinterlaced_frame->data[2];
                        stridey = deinterlaced_frame->linesize[0];
                        strideu = deinterlaced_frame->linesize[1];
                        stridev = deinterlaced_frame->linesize[2];
                        outputy = (uint8_t*)deinterlaced_buffer;
                        outputu = (uint8_t*)outputy + (width*height);
                        outputv = (uint8_t*)outputu + (whalf*hhalf);
                        for (row = 0; row < height; row++) {
                            memcpy(outputy, sourcey, width);
                            outputy += width;
                            sourcey += stridey;
                        }
                        for (row = 0; row < hhalf; row++) {
                            memcpy(outputu, sourceu, whalf);
                            outputu += whalf;
                            sourceu += strideu;
                        }
                        for (row = 0; row < hhalf; row++) {
                            memcpy(outputv, sourcev, whalf);
                            outputv += whalf;
                            sourcev += stridev;
                        }

                        if (msg->fps_den > 0) {
                            fps = (double)((double)msg->fps_num / (double)msg->fps_den);
                        }

                        video_stream_struct *vstream = (video_stream_struct*)core->source_video_stream[0].video_stream;  // only one source stream
                        int64_t sync_diff;

                        deinterlaced_frame_count++;  // frames since the video start time
                        sync_frame_count = (int64_t)(((((double)deinterlaced_frame->pkt_dts-(double)vstream->first_timestamp) / (double)90000.0))*(double)fps);
                        av_sync_offset = (((double)deinterlaced_frame_count - (double)sync_frame_count)/(double)fps)*(double)1000.0;
                        sync_diff = (int64_t)deinterlaced_frame_count - (int64_t)sync_frame_count;

                        fprintf(stderr,"video_prepare_thread: deinterlaced_frame_count=%ld sync_frame_count=%ld sync_diff=%ld (%.2fms) pkt_pts=%ld pkt_dts=%ld ft=%ld fps=%.2f\n",
                                deinterlaced_frame_count,
                                sync_frame_count,
                                sync_diff,
                                av_sync_offset,
                                deinterlaced_frame->pts/2,
                                deinterlaced_frame->pkt_dts,
                                vstream->first_timestamp,
                                fps);

                        if (sync_diff > (2*fps) ||
                            sync_diff < (-fps*2)) {
                            av_sync_compromised++;
                            if (av_sync_compromised == 1) {
                                send_direct_error(core, SIGNAL_DIRECT_ERROR_AVSYNC, "Possible A/V Sync Drift, Check Input");
                            }
                            if (av_sync_compromised >= AV_SYNC_TRIGGER_LEVEL) {
                                char errormsg[MAX_MESSAGE_SIZE];
                                syslog(LOG_ERR,"video_prepare_thread: fatal error, a/v sync is compromised!!\n");
                                fprintf(stderr,"video_prepeare_thread: fatal error, a/v sync is compromised  sync_diff=%ld fps=%.2f!!\n", sync_diff, fps);
                                snprintf(errormsg, MAX_MESSAGE_SIZE-1, "A/V Sync Is Compromised (VIDEO) - sync_dff=%ld, fps=%.2f, dts=%ld, Sync_Accounting:%ld/%ld, Restarting Service",
                                         sync_diff,
                                         fps,
                                         deinterlaced_frame->pkt_dts,
                                         deinterlaced_frame_count,
                                         sync_frame_count);
                                send_direct_error(core, SIGNAL_DIRECT_ERROR_AVSYNC, errormsg);
                                _Exit(0);
                            }
                        } else {
                            av_sync_compromised = 0;
                        }

                        if (sync_diff < -1) {
                            int max_repeat_count = (int)(((double)fps/(double)4)+0.5);
                            int sync_diff_abs = abs(sync_diff);
                            int r;
                            if (max_repeat_count == 0) {
                                max_repeat_count = 1;
                            }
                            if (sync_diff_abs > max_repeat_count) {
                                sync_diff_abs = max_repeat_count;
                            }
                            for (r = 0; r < sync_diff_abs; r++) {
                                char errormsg[MAX_MESSAGE_SIZE];
                                snprintf(errormsg, MAX_MESSAGE_SIZE-1, "Repeating Video Frame To Maintain A/V Sync (%d/%d)",
                                         r+1, sync_diff_abs);
                                send_signal(core, SIGNAL_FRAME_REPEAT, errormsg);
                                uint8_t *repeated_buffer = (uint8_t*)memory_take(core->raw_video_pool, video_frame_size);
                                if (repeated_buffer) {
                                    memcpy(repeated_buffer, deinterlaced_buffer, video_frame_size);

                                    scale_msg = (dataqueue_message_struct*)memory_take(core->fillet_msg_pool, sizeof(dataqueue_message_struct));
                                    if (!scale_msg) {
                                        send_direct_error(core, SIGNAL_DIRECT_ERROR_MSGPOOL, "Out of Message Buffers (VIDEO) - Restarting Service");
                                        _Exit(0);
                                    }
                                    scale_msg->buffer = repeated_buffer;
                                    scale_msg->buffer_size = video_frame_size;
                                    scale_msg->pts = 0;
                                    scale_msg->dts = 0;
                                    scale_msg->tff = 1;
                                    scale_msg->interlaced = 0;
                                    scale_msg->fps_num = msg->fps_num;
                                    scale_msg->fps_den = msg->fps_den;
                                    scale_msg->aspect_num = msg->aspect_num;
                                    scale_msg->aspect_den = msg->aspect_den;
                                    scale_msg->width = width;
                                    scale_msg->height = height;
                                    scale_msg->stream_index = -1;
                                    scale_msg->caption_buffer = NULL;
                                    scale_msg->caption_size = 0;
                                    scale_msg->caption_timestamp = 0;

                                    if (opaque_data) {
                                        scale_msg->splice_point = opaque_data->splice_point;
                                        scale_msg->splice_duration = opaque_data->splice_duration;
                                        scale_msg->splice_duration_remaining = opaque_data->splice_duration_remaining;
                                    } else {
                                        scale_msg->splice_point = 0;
                                        scale_msg->splice_duration = 0;
                                        scale_msg->splice_duration_remaining = 0;
                                    }

                                    deinterlaced_frame_count++;  // frames since the video start time
                                    dataqueue_put_front(core->scalevideo->input_queue, scale_msg);
                                } else {
                                    char errormsg[MAX_MESSAGE_SIZE];
                                    int n;
                                    int smp = memory_unused(core->fillet_msg_pool);
                                    int fmp = memory_unused(core->frame_msg_pool);
                                    int cvp = memory_unused(core->compressed_video_pool);
                                    int cap = memory_unused(core->compressed_audio_pool);
                                    int s35p = memory_unused(core->scte35_pool);
                                    int rvp = memory_unused(core->raw_video_pool);
                                    int rap = memory_unused(core->raw_audio_pool);
                                    int vdecfw = dataqueue_get_size(core->transvideo->input_queue);
                                    int vdinfw = dataqueue_get_size(core->preparevideo->input_queue);
                                    int vefw = 0;
                                    for (n = 0; n < core->cd->num_outputs; n++) {
                                        vefw += dataqueue_get_size(core->encodevideo->input_queue[n]);
                                    }
                                    snprintf(errormsg, MAX_MESSAGE_SIZE-1, "Out of Uncompressed Video Buffers (SYNC), Restarting Service, vefw=%d, vdinfw=%d, vdecfw=%d, rap=%d, rvp=%d, s35p=%d, cap=%d, cvp=%d, fmp=%d, smp=%d",
                                             vefw, vdinfw, vdecfw, rap, rvp, s35p, cap, cvp, fmp, smp);
                                    send_direct_error(core, SIGNAL_DIRECT_ERROR_RAWPOOL, errormsg);
                                    _Exit(0);
                                }
                            }
                        }

                        scale_msg = (dataqueue_message_struct*)memory_take(core->fillet_msg_pool, sizeof(dataqueue_message_struct));
                        if (!scale_msg) {
                            send_direct_error(core, SIGNAL_DIRECT_ERROR_MSGPOOL, "Out of Message Buffers (SYNC) - Restarting Service");
                            _Exit(0);
                        }
                        scale_msg->buffer = deinterlaced_buffer;
                        scale_msg->buffer_size = video_frame_size;
                        scale_msg->pts = deinterlaced_frame->pts/2;  // or pkt_pts?
                        scale_msg->dts = deinterlaced_frame->pkt_dts;
                        scale_msg->interlaced = 0;
                        scale_msg->tff = 1;
                        scale_msg->fps_num = msg->fps_num;
                        scale_msg->fps_den = msg->fps_den;
                        scale_msg->aspect_num = msg->aspect_num;
                        scale_msg->aspect_den = msg->aspect_den;
                        scale_msg->width = width;
                        scale_msg->height = height;
                        scale_msg->stream_index = -1;

                        if (opaque_data) {
                            uint8_t *caption_buffer;
                            int caption_size;
                            int64_t caption_timestamp;

                            caption_size = opaque_data->caption_size;
                            caption_timestamp = opaque_data->caption_timestamp;
                            if (caption_size > 0) {
                                caption_buffer = (uint8_t*)malloc(caption_size+1);
                                memcpy(caption_buffer, opaque_data->caption_buffer, caption_size);
                                scale_msg->caption_buffer = caption_buffer;
                                scale_msg->caption_size = caption_size;
                                scale_msg->caption_timestamp = caption_timestamp;
                            } else {
                                scale_msg->caption_buffer = NULL;
                                scale_msg->caption_size = 0;
                                scale_msg->caption_timestamp = 0;
                            }
                            scale_msg->splice_point = opaque_data->splice_point;
                            scale_msg->splice_duration = opaque_data->splice_duration;
                            scale_msg->splice_duration_remaining = opaque_data->splice_duration_remaining;
                        } else {
                            scale_msg->caption_buffer = NULL;
                            scale_msg->caption_size = 0;
                            scale_msg->caption_timestamp = 0;
                            scale_msg->splice_point = 0;
                            scale_msg->splice_duration = 0;
                        }

                        if (sync_diff > 1) {
                            if (scale_msg->splice_point == 0 || scale_msg->splice_duration_remaining == 0) {
                                if (scale_msg->caption_buffer) {
                                    // bug here
                                    // this needs to be saved and tacked onto another frame - for now we will just drop it
                                    free(scale_msg->caption_buffer);
                                    scale_msg->caption_buffer = NULL;
                                    scale_msg->caption_size = 0;
                                }
                                memory_return(core->raw_video_pool, deinterlaced_buffer);
                                deinterlaced_buffer = NULL;
                                deinterlaced_frame_count--;  // frames since the video start time- go back one
                                memory_return(core->fillet_msg_pool, scale_msg);
                                scale_msg = NULL;
                                // dropped!
                            }
                        }

                        if (scale_msg) {
                            fprintf(stderr,"video_prepare_thread: sending video frame to scaler, pts=%ld, dts=%ld, aspect=%d:%d\n",
                                    scale_msg->pts,
                                    scale_msg->dts,
                                    scale_msg->aspect_num,
                                    scale_msg->aspect_den);

                            dataqueue_put_front(core->scalevideo->input_queue, scale_msg);
                        }
                    }

                    if (opaque_data) {
                        if (opaque_data->caption_buffer) {
                            free(opaque_data->caption_buffer);
                        }
                        opaque_data->caption_buffer = NULL;
                        opaque_data->caption_size = 0;
                        free(opaque_data);
                        opaque_data = NULL;
                    }
                } else {
                    break;
                }
                av_frame_unref(deinterlaced_frame);
            }

            memory_return(core->raw_video_pool, msg->buffer);
            msg->buffer = NULL;
            memory_return(core->fillet_msg_pool, msg);
            msg = NULL;
        }
    }

cleanup_video_prepare_thread:
    if (deinterlacer_ready) {
        av_free(deinterlacer->scale_sws_opts);
        deinterlacer->scale_sws_opts = NULL;
        avfilter_graph_free(&deinterlacer);
        deinterlacer_ready = 0;
        av_freep(&output_data[0]);
        av_frame_free(&deinterlaced_frame);
        av_frame_free(&source_frame);
        sws_freeContext(thumbnail_scaler);
    }
    return NULL;
}

void *video_decode_thread(void *context)
{
    fillet_app_struct *core = (fillet_app_struct*)context;
    int video_decoder_ready = 0;
    dataqueue_message_struct *msg = NULL;
    AVCodecParserContext *decode_parser = NULL;
    AVCodecContext *decode_avctx = NULL;
    const AVCodec *decode_codec = NULL;
    AVPacket *decode_pkt = NULL;
    AVFrame *decode_av_frame = NULL;
    int64_t decoder_frame_count = 0;
    enum AVPixelFormat source_format = AV_PIX_FMT_YUV420P;
    enum AVPixelFormat output_format = AV_PIX_FMT_YUV420P;
    struct SwsContext *decode_converter = NULL;
    uint8_t *source_data[4];
    uint8_t *output_data[4];
    int source_stride[4];
    int output_stride[4];
    int last_video_frame_size = -1;
    uint8_t *wbuffer;
    int64_t last_splice_duration = 0;
    double decode_errors_per_second = 0;
    int decoded_framerate = 30;
    int rotating_decode_frame = 0;
    int i;
    int sum_of_decode_errors = 0;
    int output_frames = 0;
#if defined(ENABLE_GPU_DECODE)
    AVHWFramesContext *nvidia_frames_ctx = NULL;
    AVBufferRef *nvidia_device_ctx = NULL;
    AVFrame *nvidia_surface = NULL;
    enum AVHWDeviceType nvidia_type;
    nvidia_type = AV_HWDEVICE_TYPE_CUDA;
#endif

#define MAX_DECODE_SIGNAL_WINDOW 180
    signal_struct signal_data[MAX_DECODE_SIGNAL_WINDOW];
    int decode_error[MAX_DECODE_SIGNAL_WINDOW];
    int signal_write_index = 0;
    video_stream_struct *vstream = (video_stream_struct*)core->source_video_stream[0].video_stream;

    memset(signal_data,0,sizeof(signal_data));
    memset(decode_error,0,sizeof(decode_error));

    output_data[0] = NULL;
    output_data[1] = NULL;
    output_data[2] = NULL;
    output_data[3] = NULL;
    source_data[0] = NULL;
    source_data[1] = NULL;
    source_data[2] = NULL;
    source_data[3] = NULL;

    fprintf(stderr,"status: starting video decode thread: %d\n", video_decode_thread_running);

    wbuffer = (uint8_t*)malloc(MAX_VIDEO_PES_BUFFER);
    while (video_decode_thread_running) {
        msg = (dataqueue_message_struct*)dataqueue_take_back(core->transvideo->input_queue);
        while (!msg && video_decode_thread_running) {
            usleep(1000);
            msg = (dataqueue_message_struct*)dataqueue_take_back(core->transvideo->input_queue);
        }

        if (!video_decode_thread_running) {
            if (msg) {
                sorted_frame_struct *frame = (sorted_frame_struct*)msg->buffer;
                if (frame) {
                    memory_return(core->compressed_video_pool, frame->buffer);
                    frame->buffer = NULL;
                    memory_return(core->frame_msg_pool, frame);
                    frame = NULL;
                }
                memory_return(core->fillet_msg_pool, msg);
                msg = NULL;
            }
            goto cleanup_video_decoder_thread;
        }

        if (msg) {
            sorted_frame_struct *frame = (sorted_frame_struct*)msg->buffer;
            if (frame) {
                int retcode;
                char gpu_select_string[MAX_FILENAME_SIZE];

                if (core->reinitialize_decoder && decode_avctx != NULL) {
                    retcode = avcodec_send_packet(decode_avctx, NULL);
                    while (retcode == 0) {
                        retcode = avcodec_receive_frame(decode_avctx, decode_av_frame);
                    }
                    memset(decode_error,0,sizeof(decode_error));
                    fprintf(stderr,"video_decode_thread: closing decoder\n");
                    avcodec_close(decode_avctx);
                    fprintf(stderr,"video_decode_thread: removing decoder context\n");
                    avcodec_free_context(&decode_avctx);
                    fprintf(stderr,"video_decode_thread: closing parser\n");
                    av_parser_close(decode_parser);
                    fprintf(stderr,"video_decode_thread: freeing decode_av_frame\n");
                    av_frame_free(&decode_av_frame);
                    fprintf(stderr,"video_decode_thread: freeing decode_pkt\n");
                    av_packet_free(&decode_pkt);
#if defined(ENABLE_GPU_DECODE)
                    av_frame_free(&nvidia_surface);
                    av_buffer_unref(&nvidia_device_ctx);
#endif
                    if (decode_converter) {
                        fprintf(stderr,"video_decode_thread: cleaning decode_converter\n");
                        sws_freeContext(decode_converter);
                        decode_converter = NULL;
                    }
                    if (output_data[0]) {
                        fprintf(stderr,"video_decode_thread: cleaning up output_data\n");
                        av_freep(&output_data[0]);
                        output_data[0] = NULL;
                        output_data[1] = NULL;
                        output_data[2] = NULL;
                        output_data[3] = NULL;
                    }
                    core->reinitialize_decoder = 0;
                    video_decoder_ready = 0;
                    output_frames = 0;
                }

                if (!video_decoder_ready) {
                    if (frame->media_type == MEDIA_TYPE_MPEG2) {
                        decode_codec = avcodec_find_decoder(AV_CODEC_ID_MPEG2VIDEO);
                    } else if (frame->media_type == MEDIA_TYPE_H264) {
#if defined(ENABLE_GPU_DECODE)
                        decode_codec = avcodec_find_decoder_by_name("h264_cuvid");
                        snprintf(gpu_select_string,MAX_FILENAME_SIZE-1,"/dev/dri/card%d",core->cd->gpu);
                        av_hwdevice_ctx_create(&nvidia_device_ctx, nvidia_type, gpu_select_string, NULL, 0);
#else
                        decode_codec = avcodec_find_decoder(AV_CODEC_ID_H264);
#endif
                    } else if (frame->media_type == MEDIA_TYPE_HEVC) {
                        decode_codec = avcodec_find_decoder(AV_CODEC_ID_HEVC);
                    } else {
                        fprintf(stderr,"error: unknown source video codec type - failing!\n");
                        send_direct_error(core, SIGNAL_DIRECT_ERROR_UNKNOWN, "Unknown Video Format - Restarting Service");
                        _Exit(0);
                    }
                    decode_avctx = avcodec_alloc_context3(decode_codec);
                    decode_parser = av_parser_init(decode_codec->id);
#if defined(ENABLE_GPU_DECODE)
                    decode_avctx->pix_fmt = AV_PIX_FMT_NV12;
                    decode_avctx->time_base = (AVRational){1001,30000};
                    av_codec_set_pkt_timebase(decode_avctx, (AVRational){1001,30000});

                    av_opt_set(decode_avctx->priv_data,"surfaces","16",0);
                    av_opt_set(decode_avctx->priv_data,"gpu","1",0);
                    decode_avctx->hw_device_ctx = av_buffer_ref(nvidia_device_ctx);
                    decode_avctx->hw_frames_ctx = av_hwframe_ctx_alloc(nvidia_device_ctx);
                    nvidia_frames_ctx = (AVHWFramesContext*)decode_avctx->hw_frames_ctx->data;

                    nvidia_frames_ctx->format = AV_PIX_FMT_CUDA;
                    nvidia_frames_ctx->sw_format = AV_PIX_FMT_NV12;
                    nvidia_frames_ctx->width = 1920;
                    nvidia_frames_ctx->height = 1088;
                    nvidia_frames_ctx->initial_pool_size = 16;

                    av_hwframe_ctx_init(decode_avctx->hw_frames_ctx);
#endif
                    avcodec_open2(decode_avctx, decode_codec, NULL);
                    decode_av_frame = av_frame_alloc();
#if defined(ENABLE_GPU_DECODE)
                    nvidia_surface = av_frame_alloc();
                    nvidia_surface->format = AV_PIX_FMT_CUDA;
#endif
                    decode_pkt = av_packet_alloc();
                    video_decoder_ready = 1;
                }//!video_decoder_ready

                uint8_t *incoming_video_buffer = frame->buffer;
                int incoming_video_buffer_size = frame->buffer_size;

                if (incoming_video_buffer_size > MAX_VIDEO_PES_BUFFER) {
                    incoming_video_buffer_size = MAX_VIDEO_PES_BUFFER;
                }
                memcpy(wbuffer, incoming_video_buffer, incoming_video_buffer_size);
                uint8_t *working_incoming_video_buffer = wbuffer;
                int working_incoming_video_buffer_size = incoming_video_buffer_size;

#if 0
                {
                    int read_pos = 0;
                    while (read_pos < incoming_video_buffer_size) {
                        if (wbuffer[read_pos+0] == 0x00 &&
                            wbuffer[read_pos+1] == 0x00 &&
                            wbuffer[read_pos+2] == 0x01) {
                            int nal_type = wbuffer[read_pos+3] & 0x1f;
                            fprintf(stderr,"video_decode_thread: incoming nal type is 0x%x (%d)   0x%x 0x%x 0x%x 0x%x 0x%x 0x%x 0x%x 0x%x\n",
                                    nal_type, nal_type,
                                    wbuffer[read_pos+4], wbuffer[read_pos+5], wbuffer[read_pos+6], wbuffer[read_pos+7],
                                    wbuffer[read_pos+8], wbuffer[read_pos+9], wbuffer[read_pos+10], wbuffer[read_pos+11]);
                            /*
                              if (nal_type == 6 &&
                              wbuffer[read_pos+4] == 0x01 && //0x04 &&
                              wbuffer[read_pos+5] == 0x01 && //0x47 &&
                              wbuffer[read_pos+7] == 0x80) {//0xb5)
                              working_incoming_video_buffer += (read_pos+7);
                              working_incoming_video_buffer_size -= (read_pos+7);
                              break;
                              }
                            */
                        }
                        read_pos++;
                    }
                }
#endif

                while (working_incoming_video_buffer_size > 0) {
                    int decode_happy;

                    decode_pkt->size = 0;
                    decode_happy = av_parser_parse2(decode_parser, decode_avctx,
                                                    &decode_pkt->data,
                                                    &decode_pkt->size,
                                                    working_incoming_video_buffer,
                                                    working_incoming_video_buffer_size,
                                                    AV_NOPTS_VALUE,
                                                    AV_NOPTS_VALUE, 0);
                    if (decode_happy < 0) {
                        break;
                    }

                    fprintf(stderr,"video_decode_thread: decode happy=%d, remaining=%d, size=%d, frame->pts=%ld, frame->full_time=%ld\n",
                            decode_happy, working_incoming_video_buffer_size, decode_pkt->size,
                            frame->pts, frame->full_time);

                    working_incoming_video_buffer_size -= decode_happy;
                    working_incoming_video_buffer += decode_happy;

                    if (decode_pkt->size == 0) {
                        continue;
                    }

                    decode_pkt->pts = frame->pts;
                    decode_pkt->dts = frame->full_time;  // this is the one that matters

                    syslog(LOG_INFO,"video_decode_thread: incoming, splice_point=%d, splice_duration=%ld, splice_duration_remaining=%ld, timestamp=%ld\n",
                           frame->splice_point,
                           frame->splice_duration,
                           frame->splice_duration_remaining,
                           frame->full_time);

                    /*
                    int c;
                    for (c = 0; c < MAX_SIGNAL_WINDOW; c++) {
                        if (signal_data[c].pts == frame->full_time) {
                            fprintf(stderr,"video_decode_thread: timestamp already in signal_data! timestamp=%ld wtf?\n", frame->full_time);
                            exit(0);
                        }
                    }
                    */

                    signal_data[signal_write_index].pts = frame->full_time;
                    signal_data[signal_write_index].scte35_ready = frame->splice_point;
                    signal_data[signal_write_index].scte35_duration = frame->splice_duration;
                    signal_data[signal_write_index].scte35_duration_remaining = frame->splice_duration_remaining;
                    signal_write_index = (signal_write_index + 1) % MAX_DECODE_SIGNAL_WINDOW;

                    frame->splice_point = 0;  // clear it once we used it

                    retcode = avcodec_send_packet(decode_avctx, decode_pkt);
                    if (retcode < 0) {
                        // error decoding video frame - report!
                        fprintf(stderr,"video_decode_thread: decode not happy, unable to decode video frame - very sorry\n");
                        if (output_frames > 0) {
                            send_signal(core, SIGNAL_DECODE_ERROR, "Video Decode Error");
                            decode_error[rotating_decode_frame] = 1;
                        }
                    } else {
                        decode_error[rotating_decode_frame] = 0;
                    }
                    if (decoded_framerate > 0) {
                        rotating_decode_frame = (rotating_decode_frame + 1) % decoded_framerate;
                    }

                    sum_of_decode_errors = 0;
                    for (i = 0; i < decoded_framerate; i++) {
                        sum_of_decode_errors += decode_error[rotating_decode_frame];
                    }
                    decode_errors_per_second = (double)sum_of_decode_errors;
                    if (decoded_framerate > 0) {
                        if (decode_errors_per_second > (decoded_framerate/2)) {
                            char errormsg[MAX_MESSAGE_SIZE];
                            snprintf(errormsg, MAX_MESSAGE_SIZE-1, "Video Decode Error Rate Per Second Exceeded, Waiting to Stabilize, %.2f Errors Per Second, Frame Window=%d",
                                     decode_errors_per_second, decoded_framerate);
                            send_signal(core, SIGNAL_DECODE_ERROR, errormsg);
                            if (core->reinitialize_decoder == 0) {
                                core->reinitialize_decoder = 1;
                            }
                        }
                    }

                    while (retcode >= 0) {
                        const AVFrameSideData *caption_data;
                        uint8_t *caption_buffer = NULL;
                        int64_t caption_timestamp = 0;
                        int caption_size = 0;
                        int is_frame_interlaced;
                        int is_frame_tff;
                        dataqueue_message_struct *prepare_msg;
                        uint8_t *output_video_frame;
                        int video_frame_size;
                        int frame_height;
                        int frame_height2;
                        int frame_width;
                        int frame_width2;
                        int row;
                        uint8_t *y_output_video_frame;
                        uint8_t *u_output_video_frame;
                        uint8_t *v_output_video_frame;
                        uint8_t *y_source_video_frame;
                        uint8_t *u_source_video_frame;
                        uint8_t *v_source_video_frame;
                        int y_source_stride;
                        int uv_source_stride;

#if defined(ENABLE_GPU_DECODE)
                        retcode = avcodec_receive_frame(decode_avctx, nvidia_surface);
#else
                        retcode = avcodec_receive_frame(decode_avctx, decode_av_frame);
#endif
                        if (retcode == AVERROR(EAGAIN) || retcode == AVERROR_EOF) {
                            break;
                        }
                        if (retcode < 0) {
                            break;
                        }

                        /*if (decode_avctx->framerate.den == 0 || decode_avctx->framerate.num == 0) {
                            break;
                        }*/

#if defined(ENABLE_GPU_DECODE)
                        av_hwframe_transfer_data(decode_av_frame, nvidia_surface, 0);
#endif
                        decoder_frame_count++;

                        if (core->video_decode_time_set == 0) {
                            core->video_decode_time_set = 1;
                            clock_gettime(CLOCK_MONOTONIC, &core->video_decode_time);
                        }

                        is_frame_interlaced = decode_av_frame->interlaced_frame;
                        is_frame_tff = decode_av_frame->top_field_first;
                        source_format = decode_av_frame->format;
                        frame_height = decode_av_frame->height;
                        frame_width = decode_av_frame->width;

                        fprintf(stderr,"video_decode_thread: decoded video frame successfully (%ld), resolution %dx%d (aspect %d:%d) interlaced:%d (tff:%d)\n",
                                decoder_frame_count,
                                frame_width, frame_height,
                                decode_avctx->sample_aspect_ratio.num,
                                decode_avctx->sample_aspect_ratio.den,
                                is_frame_interlaced,
                                is_frame_tff);

                        if (frame_width > 3840 &&
                            frame_height > 2160) {
                            send_signal(core, SIGNAL_DECODE_ERROR, "Video Decode Error");
                            decode_error[rotating_decode_frame] = 1;
                            if (decoded_framerate > 0) {
                                rotating_decode_frame = (rotating_decode_frame + 1) % decoded_framerate;
                            }
                            break;
                        }

                        frame_height2 = frame_height / 2;
                        frame_width2 = frame_width / 2;

                        video_frame_size = 3 * frame_height * frame_width / 2;
                        if (last_video_frame_size != -1) {
                            if (last_video_frame_size != video_frame_size) {
                                // report video frame size change
                                fprintf(stderr,"warning: video frame size changed - new size: %d x %d\n", frame_width, frame_height);
                            }
                        }
                        last_video_frame_size = video_frame_size;

                        output_video_frame = (uint8_t*)memory_take(core->raw_video_pool, video_frame_size);
                        if (!output_video_frame) {
                            char errormsg[MAX_MESSAGE_SIZE];
                            int n;
                            int smp = memory_unused(core->fillet_msg_pool);
                            int fmp = memory_unused(core->frame_msg_pool);
                            int cvp = memory_unused(core->compressed_video_pool);
                            int cap = memory_unused(core->compressed_audio_pool);
                            int s35p = memory_unused(core->scte35_pool);
                            int rvp = memory_unused(core->raw_video_pool);
                            int rap = memory_unused(core->raw_audio_pool);
                            int vdecfw = dataqueue_get_size(core->transvideo->input_queue);
                            int vdinfw = dataqueue_get_size(core->preparevideo->input_queue);
                            int vefw = 0;
                            for (n = 0; n < core->cd->num_outputs; n++) {
                                vefw += dataqueue_get_size(core->encodevideo->input_queue[n]);
                            }
                            snprintf(errormsg, MAX_MESSAGE_SIZE-1, "Out of Uncompressed Video Buffers (DECODE), Restarting Service, vefw=%d, vdinfw=%d, vdecfw=%d, rap=%d, rvp=%d, s35p=%d, cap=%d, cvp=%d, fmp=%d, smp=%d",
                                     vefw, vdinfw, vdecfw, rap, rvp, s35p, cap, cvp, fmp, smp);
                            send_direct_error(core, SIGNAL_DIRECT_ERROR_RAWPOOL, errormsg);
                            _Exit(0);
                        }

                        // 422 to 420 conversion if needed
                        // 10 to 8 bit conversion if needed (no support for HDR to SDR mapping)
                        source_data[0] = decode_av_frame->data[0];
                        source_data[1] = decode_av_frame->data[1];
                        source_data[2] = decode_av_frame->data[2];
                        source_data[3] = decode_av_frame->data[3];
                        source_stride[0] = decode_av_frame->linesize[0];
                        source_stride[1] = decode_av_frame->linesize[1];
                        source_stride[2] = decode_av_frame->linesize[2];
                        source_stride[3] = decode_av_frame->linesize[3];

                        if (source_format != output_format) {
                            if (!decode_converter) {
                                decode_converter = sws_getContext(frame_width, frame_height, source_format,
                                                                  frame_width, frame_height, output_format,
                                                                  SWS_BICUBIC,
                                                                  NULL, NULL, NULL); // no dither specified
                                av_image_alloc(output_data, output_stride, frame_width, frame_height, output_format, 1);
                            }

                            sws_scale(decode_converter,
                                      (const uint8_t * const*)source_data, source_stride, 0,
                                      frame_height, output_data, output_stride);

                            y_source_video_frame = output_data[0];
                            u_source_video_frame = output_data[1];
                            v_source_video_frame = output_data[2];
                            y_source_stride = output_stride[0];
                            uv_source_stride = output_stride[1];
                        } else {
                            y_source_video_frame = source_data[0];
                            u_source_video_frame = source_data[1];
                            v_source_video_frame = source_data[2];
                            y_source_stride = source_stride[0];
                            uv_source_stride = source_stride[1];
                        }

                        y_output_video_frame = output_video_frame;
                        u_output_video_frame = y_output_video_frame + (frame_width * frame_height);
                        v_output_video_frame = u_output_video_frame + (frame_width2 * frame_height2);
                        for (row = 0; row < frame_height; row++) {
                            memcpy(y_output_video_frame, y_source_video_frame, frame_width);
                            y_output_video_frame += frame_width;
                            y_source_video_frame += y_source_stride;
                        }
                        for (row = 0; row < frame_height2; row++) {
                            memcpy(u_output_video_frame, u_source_video_frame, frame_width2);
                            u_output_video_frame += frame_width2;
                            u_source_video_frame += uv_source_stride;
                        }
                        for (row = 0; row < frame_height2; row++) {
                            memcpy(v_output_video_frame, v_source_video_frame, frame_width2);
                            v_output_video_frame += frame_width2;
                            v_source_video_frame += uv_source_stride;
                        }

                        caption_data = (AVFrameSideData*)av_frame_get_side_data(decode_av_frame, AV_FRAME_DATA_A53_CC);
                        if (caption_data) {
                            // caption data is present
                            uint8_t *buffer;
                            int buffer_size;
                            int caption_elements;

                            buffer = (uint8_t*)caption_data->data;
                            if (buffer) {
                                buffer_size = caption_data->size;
                                caption_elements = buffer_size / 3;

                                // ffmpeg doesn't return the caption buffer with the header information
#define CAPTION_HEADER_SIZE 7
                                caption_buffer = (uint8_t*)malloc(buffer_size+CAPTION_HEADER_SIZE);
                                caption_buffer[0] = 0x47;  // this is the GA94 tag
                                caption_buffer[1] = 0x41;
                                caption_buffer[2] = 0x39;
                                caption_buffer[3] = 0x34;
                                caption_buffer[4] = 0x03;
                                caption_buffer[5] = caption_elements | 0xc0;
                                caption_buffer[6] = 0xff;
                                memcpy(caption_buffer+CAPTION_HEADER_SIZE,
                                       buffer,
                                       buffer_size);

                                caption_size = buffer_size+CAPTION_HEADER_SIZE;
                                fprintf(stderr,"video_decode_thread: caption found - %d\n", buffer_size);
                                caption_buffer[caption_size+1] = 0xff;
                                caption_size++;
                                caption_timestamp = decode_av_frame->pkt_dts;
                            } else {
                                caption_buffer = NULL;
                                caption_size = 0;
                                caption_timestamp = 0;
                            }
                        } else {
                            fprintf(stderr,"video_decode_thread: no caption buffer\n");
                            caption_buffer = NULL;
                            caption_size = 0;
                            caption_timestamp = 0;
                        }

                        prepare_msg = (dataqueue_message_struct*)memory_take(core->fillet_msg_pool, sizeof(dataqueue_message_struct));
                        if (prepare_msg) {
                            prepare_msg->buffer = output_video_frame;
                            prepare_msg->buffer_size = video_frame_size;
#if defined(ENABLE_GPU_DECODE)
                            prepare_msg->pts = nvidia_surface->pts;
                            prepare_msg->dts = nvidia_surface->pkt_dts;
#else
                            prepare_msg->pts = decode_av_frame->pts;
                            prepare_msg->dts = decode_av_frame->pkt_dts;
#endif

                            fprintf(stderr,"video_decode_thread: pts=%ld pkt_dts=%ld\n",
                                    decode_av_frame->pts,
                                    decode_av_frame->pkt_dts);

                            if (!is_frame_interlaced) {
                                prepare_msg->tff = 1;
                            } else {
                                prepare_msg->tff = is_frame_tff;
                            }
                            prepare_msg->interlaced = is_frame_interlaced;
                            prepare_msg->caption_buffer = caption_buffer;
                            prepare_msg->caption_size = caption_size;
                            prepare_msg->caption_timestamp = caption_timestamp;
                            caption_buffer = NULL;
                            caption_size = 0;

                            int lookup;
                            int splice_point = 0;
                            int64_t splice_duration = 0;
                            int64_t splice_duration_remaining = 0;
                            int found_splice = 0;
                            for (lookup = 0; lookup < MAX_DECODE_SIGNAL_WINDOW; lookup++) {
                                if (signal_data[lookup].pts == decode_av_frame->pkt_dts) {
                                    splice_point = signal_data[lookup].scte35_ready;
                                    splice_duration = signal_data[lookup].scte35_duration;
                                    splice_duration_remaining = signal_data[lookup].scte35_duration_remaining;
                                    found_splice = 1;
                                    break;
                                }
                            }

                            if (!found_splice) {
                                for (lookup = 0; lookup < MAX_DECODE_SIGNAL_WINDOW; lookup++) {
                                    fprintf(stderr,"unable to find output timestamp, looking for %ld but found %ld\n",
                                            decode_av_frame->pkt_dts, signal_data[lookup].pts);
                                }
                                _Exit(0);
                            } else {
                                for (lookup = 0; lookup < MAX_DECODE_SIGNAL_WINDOW; lookup++) {
                                    if (signal_data[lookup].pts != 0) {
                                        if (signal_data[lookup].pts == decode_av_frame->pkt_dts) {
                                            signal_data[lookup].pts = 0;
                                            signal_data[lookup].scte35_ready = 0;
                                            signal_data[lookup].scte35_duration = 0;
                                            signal_data[lookup].scte35_duration_remaining = 0;
                                        } else {
                                            //fprintf(stderr,"(%d) unable to find output timestamp, looking for %ld but found %ld\n",
                                            //        lookup, decode_av_frame->pkt_dts, signal_data[lookup].pts);
                                        }
                                    }
                                }
                                // this a fix for field frame issues so it doesn't get skipped.
                                if (last_splice_duration == 0 && splice_duration > 0) {
                                    if (splice_point != SPLICE_CUE_OUT) {
                                        splice_point = SPLICE_CUE_OUT;
                                    }
                                }
                                if (last_splice_duration > 0 && splice_duration == 0) {
                                    if (splice_point != SPLICE_CUE_IN) {
                                        splice_point = SPLICE_CUE_IN;
                                    }
                                }
                                syslog(LOG_INFO,"video_decode_thread: outgoing, splice_point=%d, splice_duration=%ld, splice_duration_remaining=%ld, timestamp=%ld\n",
                                       splice_point,
                                       splice_duration,
                                       splice_duration_remaining,
                                       decode_av_frame->pkt_dts);
                            }
                            last_splice_duration = splice_duration;

                            if (!vstream->found_key_frame) {
                                vstream->found_key_frame = 1;
                                vstream->first_timestamp = decode_av_frame->pts;
                                fprintf(stderr,"video_decode_thread: setting first timestamp to %ld\n", vstream->first_timestamp);
                            }

                            // ffmpeg does this inverse because of the 1/X
                            prepare_msg->fps_num = decode_avctx->framerate.num;
                            prepare_msg->fps_den = decode_avctx->framerate.den;
                            if (decode_avctx->framerate.den > 0) {
                                decoded_framerate = (int)(((double)decode_avctx->framerate.num / (double)decode_avctx->framerate.den)+0.5f);
                            } else {
                                decoded_framerate = 30;
                            }

                            fprintf(stderr,"video_decode_thread: fps=%d/%d, framerate.den=%d framerate.num=%d ticks_per_frame=%d\n",
                                    prepare_msg->fps_num, prepare_msg->fps_den,
                                    decode_avctx->framerate.den,
                                    decode_avctx->framerate.num,
                                    decode_avctx->ticks_per_frame);

                            prepare_msg->aspect_num = decode_avctx->sample_aspect_ratio.num;
                            prepare_msg->aspect_den = decode_avctx->sample_aspect_ratio.den;
                            prepare_msg->width = frame_width;
                            prepare_msg->height = frame_height;
                            prepare_msg->splice_point = splice_point;
                            prepare_msg->splice_duration = splice_duration;
                            prepare_msg->splice_duration_remaining = splice_duration_remaining;

                            core->decoded_source_info.decoded_width = frame_width;
                            core->decoded_source_info.decoded_height = frame_height;
                            core->decoded_source_info.decoded_fps_num = prepare_msg->fps_num;
                            core->decoded_source_info.decoded_fps_den = prepare_msg->fps_den;
                            core->decoded_source_info.decoded_aspect_num = prepare_msg->aspect_num;
                            core->decoded_source_info.decoded_aspect_den = prepare_msg->aspect_den;
                            core->decoded_source_info.decoded_video_media_type = frame->media_type;

                            output_frames++;

                            dataqueue_put_front(core->monitorvideo->input_queue, prepare_msg);
                        } else {
                            fprintf(stderr,"FATAL ERROR: unable to obtain prepare_msg!!\n");
                            send_direct_error(core, SIGNAL_DIRECT_ERROR_MSGPOOL, "Out of Message Buffers (DECODE) - Restarting Service");
                            _Exit(0);
                        }
                    }
                    memory_return(core->compressed_video_pool, frame->buffer);
                    frame->buffer = NULL;
                }

                memory_return(core->frame_msg_pool, frame);
                frame = NULL;
                memory_return(core->fillet_msg_pool, msg);
                msg = NULL;
            }
        }
    }
cleanup_video_decoder_thread:

    free(wbuffer);
    core->reinitialize_decoder = 0;
    video_decoder_ready = 0;
    if (decode_avctx) {
        avcodec_close(decode_avctx);
        avcodec_free_context(&decode_avctx);
    }
    if (decode_parser) {
        av_parser_close(decode_parser);
    }
    if (decode_av_frame) {
        av_frame_free(&decode_av_frame);
    }
    if (decode_pkt) {
        av_packet_free(&decode_pkt);
    }
#if defined(ENABLE_GPU_DECODE)
    av_frame_free(&nvidia_surface);
    av_buffer_unref(&nvidia_device_ctx);
#endif
    if (decode_converter) {
        sws_freeContext(decode_converter);
    }
    if (output_data[0]) {
        av_freep(&output_data[0]);
    }

    return NULL;
}

int start_video_transcode_threads(fillet_app_struct *core)
{
    int num_outputs = core->cd->num_outputs;
    int i;

    video_encode_thread_running = 1;

    // for now we don't allow mixing the video codec across the streams
    // mixing will come later once i get some other things figured out
    if (core->cd->transvideo_info[0].video_codec == STREAM_TYPE_HEVC) {
        for (i = 0; i < num_outputs; i++) {
            thread_start_struct *start;
            start = (thread_start_struct*)malloc(sizeof(thread_start_struct));
            start->index = i;
            start->core = core;
#if defined(ENABLE_GPU)
            pthread_create(&video_encode_thread_id[i], NULL, video_encode_thread_nvenc, (void*)start);
#else
            pthread_create(&video_encode_thread_id[i], NULL, video_encode_thread_x265, (void*)start);
#endif
        }
    } else if (core->cd->transvideo_info[0].video_codec == STREAM_TYPE_H264) {
        for (i = 0; i < num_outputs; i++) {
            thread_start_struct *start;
            start = (thread_start_struct*)malloc(sizeof(thread_start_struct));
            start->index = i;
            start->core = core;
#if defined(ENABLE_GPU)
            pthread_create(&video_encode_thread_id[i], NULL, video_encode_thread_nvenc, (void*)start);
#else
            pthread_create(&video_encode_thread_id[i], NULL, video_encode_thread_x264, (void*)start);
#endif
        }
    } else {
        fprintf(stderr,"error: unknown video encoder selected! fail!\n");
        //signal-unsupported video encoder type
        _Exit(0);
    }

    video_thumbnail_thread_running = 1;
    pthread_create(&video_thumbnail_thread_id, NULL, video_thumbnail_thread, (void*)core);

    video_monitor_thread_running = 1;
    pthread_create(&video_monitor_thread_id, NULL, video_monitor_thread, (void*)core);

    video_prepare_thread_running = 1;
    pthread_create(&video_prepare_thread_id, NULL, video_prepare_thread, (void*)core);

    video_scale_thread_running = 1;
    pthread_create(&video_scale_thread_id, NULL, video_scale_thread, (void*)core);

    video_decode_thread_running = 1;
    pthread_create(&video_decode_thread_id, NULL, video_decode_thread, (void*)core);

    return 0;
}

int stop_video_transcode_threads(fillet_app_struct *core)
{
    int num_outputs = core->cd->num_outputs;
    int i;

    video_decode_thread_running = 0;
    pthread_join(video_decode_thread_id, NULL);

    video_scale_thread_running = 0;
    pthread_join(video_scale_thread_id, NULL);

    video_prepare_thread_running = 0;
    pthread_join(video_prepare_thread_id, NULL);

    video_monitor_thread_running = 0;
    pthread_join(video_monitor_thread_id, NULL);

    video_thumbnail_thread_running = 0;
    pthread_join(video_thumbnail_thread_id, NULL);

    video_encode_thread_running = 0;
    for (i = 0; i < num_outputs; i++) {
        pthread_join(video_encode_thread_id[i], NULL);
    }

    return 0;
}

#else // ENABLE_TRANSCODE

void *video_monitor_thread(void *context)
{
    fprintf(stderr,"VIDEO MONITOR NOT ENABLED - PLEASE RECOMPILE IF REQUIRED\n");
    return NULL;
}

void *video_prepare_thread(void *context)
{
    fprintf(stderr,"VIDEO PREPARE NOT ENABLED - PLEASE RECOMPILE IF REQUIRED\n");
    return NULL;
}

void *video_decode_thread(void *context)
{
    fprintf(stderr,"VIDEO DECODING NOT ENABLED - PLEASE RECOMPILE IF REQUIRED\n");
    return NULL;
}

void *video_encode_thread_x264(void *context)
{
    fprintf(stderr,"VIDEO ENCODING NOT ENABLED - PLEASE RECOMPILE IF REQUIRED\n");
    return NULL;
}

void *video_encode_thread_x265(void *context)
{
    fprintf(stderr,"VIDEO ENCODING NOT ENABLED - PLEASE RECOMPILE IF REQUIRED\n");
    return NULL;
}

void *video_encode_thread_nvenc(void *context)
{
    fprintf(stderr,"NVENC VIDEO ENCODING NOT ENABLED - PLEASE RECOMPILE IF REQUIRED\n");
    return NULL;
}

int start_video_transcode_threads(fillet_app_struct *core)
{
    return 0;
}

int stop_video_transcode_threads(fillet_app_struct *core)
{
    return 0;
}

#endif
