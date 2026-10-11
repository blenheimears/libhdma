/* Copyright (C) 2026 the contributors to the libhdma project. SPDX-License-Identifier: LGPL-2.1-or-later. */
#include "sndfile_input.h"
#include <stdio.h>
int pcm_file_input_open(PcmFileInput **out,const char *filename,const char *layout){(void)filename;(void)layout;if(out)*out=0;fprintf(stderr,"libsndfile input is disabled; use --raw-pcm with explicitly specified PCM format\n");return -1;}
int pcm_file_input_rate(const PcmFileInput*s){(void)s;return 0;}
int pcm_file_input_channels(const PcmFileInput*s){(void)s;return 0;}
int pcm_file_input_layout21(const PcmFileInput*s){(void)s;return 0;}
int pcm_file_input_depth(const PcmFileInput*s){(void)s;return 0;}
int64_t pcm_file_input_frames(const PcmFileInput*s){(void)s;return -1;}
int pcm_file_input_read(PcmFileInput*s,int32_t*p,int n){(void)s;(void)p;(void)n;return -1;}
const char*pcm_file_input_error(const PcmFileInput*s){(void)s;return "libsndfile input disabled";}
void pcm_file_input_close(PcmFileInput*s){(void)s;}
