/*****************************************************************************
  Copyright (C) 2018-2020 John William

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
#include <sys/socket.h>
#include <netdb.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/poll.h>
#include <sys/time.h>
#include <arpa/inet.h>
#include <ifaddrs.h>
#include <math.h>
#include <syslog.h>
#include "fillet.h"
#include "tsdecode.h"
#include "mempool.h"
#include "dataqueue.h"
#include "udpsource.h"
#include "tsreceive.h"
#include "esignal.h"

static pthread_mutex_t start_lock = PTHREAD_MUTEX_INITIALIZER;

void *udp_source_thread(void *context)
{
    transport_data_struct *tsdata;
    int udp_socket;
    int timeout_ms = 1000;
    fd_set sockset;
    uint8_t *udp_buffer;
    int udp_buffer_size;
    int i;
    int active_source_index;
    int64_t no_signal_counter = 0;
    int scanned;
    int num_ipaddr0;
    int num_ipaddr1;
    int num_ipaddr2;
    int num_ipaddr3;
    int mcast_flag = 0;
    char signal_msg[MAX_STR_SIZE];
    udp_thread_data_struct *avudp;
    int source_count;
    char udp_source_ipaddr[MAX_STR_SIZE];
    int udp_source_port = 0;

    avudp = (udp_thread_data_struct*)context;
    fillet_app_struct *core = (fillet_app_struct*)avudp->core;
    source_count = avudp->source_index;
    fprintf(stderr,"SESSION:%d SOURCE:%d\n",
            core->session_id,
            source_count);
    snprintf(udp_source_ipaddr, MAX_STR_SIZE-1, "%s", avudp->udp_source_ipaddr);
    udp_source_port = avudp->udp_source_port;
    free(avudp);
    avudp = NULL;

#define MAX_UDP_BUFFER_READ 2048
    pthread_mutex_lock(&start_lock);

    udp_buffer = (uint8_t*)malloc(MAX_UDP_BUFFER_READ);
    udp_buffer_size = MAX_UDP_BUFFER_READ;
    tsdata = (transport_data_struct*)malloc(sizeof(transport_data_struct));

    memset(tsdata, 0, sizeof(transport_data_struct));

    for (i = 0; i< MAX_ACTUAL_PIDS; i++) {
         tsdata->initial_pcr_base[i] = -1;
    }
    tsdata->pat_program_count = -1;
    tsdata->pat_version_number = -1;
    tsdata->pat_transport_stream_id = -1;
    tsdata->source = source_count;
    core->input_signal = 0;
    core->source_interruptions = 0;

    fprintf(stderr,"SESSION:%d SOURCE:%d (TSRECEIVE) STATUS: SOURCE ADDRESS PROVIDED: %s\n",
            core->session_id, source_count, udp_source_ipaddr);

    scanned = sscanf(udp_source_ipaddr,"%3d.%3d.%3d.%3d",
                     &num_ipaddr0,
                     &num_ipaddr1,
                     &num_ipaddr2,
                     &num_ipaddr3);

    fprintf(stderr,"SESSION:%d (TSRECEIVE) STATUS: PARSING SOURCE ADDRESS %3d.%3d.%3d.%3d  SCANNED:%d\n",
            core->session_id,
            num_ipaddr0,
            num_ipaddr1,
            num_ipaddr2,
            num_ipaddr3,
            scanned);

    if (num_ipaddr0 >= 224) {
        mcast_flag = 1;
    }

    udp_socket = socket_udp_open(core->fillet_video_input[source_count].interface,  // interface is the same for video and audio
                                 udp_source_ipaddr,
                                 udp_source_port,
                                 mcast_flag, UDP_FLAG_INPUT, 1);
    active_source_index = source_count;

    source_count++;
    memset(tsdata->pmt_version, -1, sizeof(tsdata->pmt_version));

    pthread_mutex_unlock(&start_lock);

    if (udp_socket < 0) {
        core->source_running = 0;
        goto _cleanup_udp_source_thread;
    }

    syslog(LOG_INFO,"SESSION:%d (TSRECEIVE) STATUS: NETWORK THREAD IS STARTING - SOCKET:%d\n",
           core->session_id,
           udp_socket);

    while (1) {
        int is_thread_running;
        int anysignal;

        is_thread_running = core->source_running;
        if (!is_thread_running || udp_socket < 0) {
            core->source_running = 0;
            core->video_receive_time_set = 0;
            syslog(LOG_INFO,"SESSION:%d (TSRECEIVE) STATUS: NETWORK THREAD IS EXITING: FLAG=%d SOCKET=%d\n",
                   core->session_id,
                   is_thread_running,
                   udp_socket);
            goto _cleanup_udp_source_thread;
        }

        if (no_signal_counter >= 3) {
            if (udp_socket > 0) {
                socket_udp_close(udp_socket);
            }
            udp_socket = socket_udp_open(core->fillet_video_input[source_count].interface,  // interface is the same for video and audio
                                         udp_source_ipaddr,
                                         udp_source_port,
                                         mcast_flag, UDP_FLAG_INPUT, 1);
            no_signal_counter = 0;
            continue;
        }

        anysignal = socket_udp_ready(udp_socket, timeout_ms, &sockset);
        if (anysignal == 0) {
            syslog(LOG_WARNING,"SESSION:%d (TSRECEIVE) WARNING: NO SOURCE SIGNAL PRESENT (SOCKET:%d) %s:%d:%s (%ld)\n",
                   core->session_id,
                   udp_socket,
                   udp_source_ipaddr,
                   udp_source_port,
                   core->fillet_video_input[active_source_index].interface,
                   no_signal_counter);

            fprintf(stderr,"SESSION:%d (TSRECEIVE) WARNING: NO SOURCE SIGNAL PRESENT (SOCKET:%d) %s:%d:%s (%ld)\n",
                    core->session_id,
                    udp_socket,
                    udp_source_ipaddr,
                    udp_source_port,
                    core->fillet_video_input[active_source_index].interface,
                    no_signal_counter);

            int audio_stream;
            for (audio_stream = 0; audio_stream < MAX_AUDIO_STREAMS; audio_stream++) {
                core->decoded_source_info.decoded_audio_channels_input[audio_stream] = 0;
                core->decoded_source_info.decoded_audio_channels_output[audio_stream] = 0;
                core->decoded_source_info.decoded_audio_sample_rate[audio_stream] = 0;
            }

            if (core->input_signal == 1) {
                core->source_interruptions++;
            }
            core->input_signal = 0;
            no_signal_counter++;

            snprintf(signal_msg, MAX_STR_SIZE-1, "%s:%d:%s",
                     udp_source_ipaddr,
                     udp_source_port,
                     core->fillet_video_input[active_source_index].interface);
            send_signal(core, SIGNAL_NO_INPUT_SIGNAL, signal_msg);

            continue;
        }

        if (FD_ISSET(udp_socket, &sockset)) {
            int bytes = socket_udp_read(udp_socket, udp_buffer, udp_buffer_size);
            if (bytes > 0) {
                no_signal_counter = 0;
                int total_packets = bytes / 188;
                if (total_packets > 0) {
                    if (core->input_signal == 0) {
                        snprintf(signal_msg, MAX_STR_SIZE-1, "%s:%d:%s",
                                 udp_source_ipaddr,
                                 udp_source_port,
                                 core->fillet_video_input[active_source_index].interface);

                        send_signal(core, SIGNAL_INPUT_SIGNAL_LOCKED, signal_msg);
                        if (core->video_receive_time_set == 0) {
                            core->video_receive_time_set = 1;
                            clock_gettime(CLOCK_MONOTONIC, &core->video_receive_time);
                        }
                    }
                    core->input_signal = 1;
                    decode_packets(udp_buffer, total_packets, tsdata, core->cd->stream_select);
                }
            }
        }
    }

_cleanup_udp_source_thread:
    if (udp_socket > 0) {
        socket_udp_close(udp_socket);
    }
    free(tsdata);

    return NULL;
}
