/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <obs-module.h>
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/avutil.h>
#include <libswscale/swscale.h>
#include <srt/srt.h>
#include <librist/librist.h>
#include <stdio.h>
#include <string.h>
int main(int argc, char **argv)
{
	if (argc != 2)
		return 2;
	FILE *probe = fopen(argv[1], "wx");
	if (!probe)
		return 3;
	fclose(probe);
	if (remove(argv[1]))
		return 4;
	if ((avformat_version() >> 16) != LIBAVFORMAT_VERSION_MAJOR ||
	    (avcodec_version() >> 16) != LIBAVCODEC_VERSION_MAJOR ||
	    (avutil_version() >> 16) != LIBAVUTIL_VERSION_MAJOR ||
	    (swscale_version() >> 16) != LIBSWSCALE_VERSION_MAJOR)
		return 5;
	AVFormatContext *f = NULL;
	if (avformat_alloc_output_context2(&f, NULL, "mpegts", argv[1]) < 0 || !f)
		return 6;
	AVIOContext *io = NULL;
	if (avio_open(&io, argv[1], AVIO_FLAG_WRITE) < 0 || !io) {
		avformat_free_context(f);
		return 7;
	}
	const unsigned char bytes[] = "native AVIO preflight\n";
	avio_write(io, bytes, sizeof(bytes) - 1);
	avio_flush(io);
	int error = io->error, close_result = avio_close(io);
	avformat_free_context(f);
	if (error || close_result)
		return 8;
	printf("PASS SDK ABI major versions and native AVIO file close\nOBS=%u FFmpeg=%u/%u/%u/%u SRT=%u RIST=%s\n",
	       obs_get_version(), avformat_version(), avcodec_version(), avutil_version(), swscale_version(),
	       srt_getversion(), librist_version());
	return 0;
}
