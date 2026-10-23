/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <stdio.h>
#include <stdint.h>
int main(int argc,char **argv){if(argc!=2)return 2;uint8_t data[4096];unsigned long long bytes=0;size_t n;while((n=fread(data,1,sizeof(data),stdin))>0)bytes+=n;if(ferror(stdin))return 3;FILE *f=fopen(argv[1],"wx");if(!f)return 4;fprintf(f,"%llu\n",bytes);return fclose(f)?5:0;}
