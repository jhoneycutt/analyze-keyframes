/*
 * analyze-keyframes.h
 *
 *  Copyright:
 *    Jon Honeycutt   (2019) <jhoneycutt@gmail.com>
 *
 *  License:
 *    BSD 3-clause; see LICENSE.
 */

#include <memory>

extern "C" {
    #include <libavcodec/avcodec.h>
    #include <libavformat/avformat.h>
    #include <libavutil/imgutils.h>
    #include <libswscale/swscale.h>
}


// Custom unique_ptrs for AV types.
struct SwsContextDeleter { void operator()(SwsContext* context) { sws_freeContext(context); } };
using SwsContextPtr = std::unique_ptr<SwsContext, SwsContextDeleter>;

struct AVFrameDeleter { void operator()(AVFrame* frame) { av_frame_free(&frame); } };
using AVFramePtr = std::unique_ptr<AVFrame, AVFrameDeleter>;

struct AVInputFileFormatContextDeleter { void operator()(AVFormatContext* context) { avformat_close_input(&context); } };
using AVInputFileFormatContextPtr = std::unique_ptr<AVFormatContext, AVInputFileFormatContextDeleter>;

struct AVCodecContextDeleter { void operator()(AVCodecContext* context) { avcodec_free_context(&context); } };
using AVCodecContextPtr = std::unique_ptr<AVCodecContext, AVCodecContextDeleter>;

struct AVPacketDeleter { void operator()(AVPacket* packet) { av_packet_free(&packet); } };
using AVPacketPtr = std::unique_ptr<AVPacket, AVPacketDeleter>;

