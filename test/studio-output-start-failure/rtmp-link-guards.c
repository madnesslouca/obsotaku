/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Link-only guards for setup/connect/destruction paths retained by MSVC.
 * Every entry aborts: this fixture must never execute an RTMP handshake or
 * create/destroy a full production connector. Tested sender/protocol injections
 * in rtmp-check.c are unchanged. No fake successful implementation. */
#ifdef _WIN32
#include <winsock2.h>
#else
#include <sys/socket.h>
#include <netinet/in.h>
#endif
#include <stdio.h>
#include <stdlib.h>
#include "librtmp/rtmp.h"
static void forbidden(const char *name){fprintf(stderr,"FORBIDDEN retained link path: %s\n",name);fflush(stderr);abort();}
int RTMP_SetupURL(RTMP *r,char *url){(void)r;(void)url;forbidden("RTMP_SetupURL");return 0;}
int RTMP_AddStream(RTMP *r,const char *path){(void)r;(void)path;forbidden("RTMP_AddStream");return 0;}
int RTMP_Connect(RTMP *r,RTMPPacket *packet){(void)r;(void)packet;forbidden("RTMP_Connect");return 0;}
int RTMP_ConnectStream(RTMP *r,int seek){(void)r;(void)seek;forbidden("RTMP_ConnectStream");return 0;}
void RTMP_Init(RTMP *r){(void)r;forbidden("RTMP_Init");}
void RTMP_TLS_Free(RTMP *r){(void)r;forbidden("RTMP_TLS_Free");}
void RTMP_EnableWrite(RTMP *r){(void)r;forbidden("RTMP_EnableWrite");}
