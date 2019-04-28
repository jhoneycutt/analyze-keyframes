/*
 * analyze-keyframes:
 *   A program that uses the libraries provided by ffmpeg to analyze
 *   keyframes from a video file.
 *
 *  Copyright:
 *    Leandro Moreira (2017) <https://github.com/leandromoreira>>
 *    Jon Honeycutt   (2019) <jhoneycutt@gmail.com>
 *
 *  License:
 *    BSD 3-clause; see LICENSE.
 */

#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <inttypes.h>

extern "C"
{

#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>

}

#define UNUSED_PARAM(x) (void)(x);

static void logging(const char* format, ...);
static int decodePacket(const AVPacket*, AVCodecContext*, AVFrame*);
static bool outputGrayscaleKeyframe(const unsigned char* buffer, int lineSize, int width, int height, const char* filename);

static const char* AVError(int code)
{
    static char errorString[AV_ERROR_MAX_STRING_SIZE];
    av_make_error_string(errorString, AV_ERROR_MAX_STRING_SIZE, code);
    return errorString;
}

int main(int argc, const char* argv[])
{
    if (argc < 2) {
        logging("Usage: %s <video file>\n", argv[0]);
        return -1;
    }

    auto inputFile = argv[1];

    AVFormatContext* formatContext = avformat_alloc_context();

    logging("Opening input file %s...", inputFile);
    int result = avformat_open_input(&formatContext, inputFile, NULL, NULL);
    if (result) {
        logging("Error: Failed to open input file: %s", AVError(result));
        return -1;
    }

    logging("    Format %s, duration %lld us, bit_rate %lld", formatContext->iformat->name, formatContext->duration, formatContext->bit_rate);

    result = avformat_find_stream_info(formatContext, NULL);
    if (result) {
        logging("Error: Failed to find stream info: %s", AVError(result));
        return -1;
    }

    AVCodec* videoCodec = nullptr;
    AVCodecParameters* videoCodecParameters = nullptr;
    int videoStreamIndex;

    // Loop though all streams, and print some information about them.
    for (unsigned i = 0; i < formatContext->nb_streams; i++) {
        AVStream* stream = formatContext->streams[i];
        logging("Stream #%u", i);
        logging("    AVStream->time_base before open coded %d/%d", stream->time_base.num, stream->time_base.den);
        logging("    AVStream->r_frame_rate before open coded %d/%d", stream->r_frame_rate.num, stream->r_frame_rate.den);
        logging("    AVStream->start_time %" PRId64, stream->start_time);
        logging("    AVStream->duration %" PRId64, stream->duration);

        AVCodecParameters* codecParameters = stream->codecpar;
        AVCodec* codec = avcodec_find_decoder(codecParameters->codec_id);
        if (!codec) {
            logging("Error: No codec found for stream.");
            continue;
        }

        if (!videoCodec && codecParameters->codec_type == AVMEDIA_TYPE_VIDEO) {
            videoStreamIndex = i;
            videoCodec = codec;
            videoCodecParameters = codecParameters;
            logging("    Video Codec: resolution %d x %d", codecParameters->width, codecParameters->height);
        } else if (codecParameters->codec_type == AVMEDIA_TYPE_AUDIO)
            logging("    Audio Codec: %d channels, sample rate %d", codecParameters->channels, codecParameters->sample_rate);

        logging("        Codec %s ID %d bit_rate %lld", codec->name, codec->id, codecParameters->bit_rate);
    }

    if (!videoCodec) {
        logging("Error: Failed to find a decodable video stream in input file.");
        return -1;
    }

    AVCodecContext* codecContext = avcodec_alloc_context3(videoCodec);
    result = avcodec_parameters_to_context(codecContext, videoCodecParameters);
    if (result < 0) {
        logging("Error: Failed to copy codec parameters to codec context: %s", AVError(result));
         return -1;
    }

    result = avcodec_open2(codecContext, videoCodec, NULL);
    if (result < 0) {
        logging("Error: Failed to open codec: %s", AVError(result));
        return -1;
    }

    // https://ffmpeg.org/doxygen/trunk/structAVFrame.html
    AVFrame* frame = av_frame_alloc();

    // https://ffmpeg.org/doxygen/trunk/structAVPacket.html
    AVPacket* packet = av_packet_alloc();

    int response = 0;

    codecContext->skip_frame = AVDISCARD_NONKEY;

    // fill the Packet with data from the Stream
    // https://ffmpeg.org/doxygen/trunk/group__lavf__decoding.html#ga4fdb3084415a82e3810de6ee60e46a61
    while (av_read_frame(formatContext, packet) >= 0) {
        if (packet->stream_index != videoStreamIndex)
            continue;

        logging("AVPacket->pts %" PRId64, packet->pts);
        response = decodePacket(packet, codecContext, frame);
        if (response < 0)
            break;

        // https://ffmpeg.org/doxygen/trunk/group__lavc__packet.html#ga63d5a489b419bd5d45cfd09091cbcbc2
        av_packet_unref(packet);
    }

    logging("releasing all the resources");

    avformat_close_input(&formatContext);
    avformat_free_context(formatContext);
    av_packet_free(&packet);
    av_frame_free(&frame);
    avcodec_free_context(&codecContext);
    return 0;
}

static void logging(const char* fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    vfprintf(stderr, fmt, args);
    va_end(args);
    fprintf(stderr, "\n");
}

static int decodePacket(const AVPacket* packet, AVCodecContext* codecContext, AVFrame* frame)
{
    int result = avcodec_send_packet(codecContext, packet);
    if (result < 0) {
        logging("Error: Failed sending packet to the decoder: %s", AVError(result));
        return result;
    }

    while (true) {
        // Process a single frame from the decoder. If the decoder returns EAGAIN, more input data is needed to decode 
        // the next frame. If it returns EOF, we've reached the end of the stream.
        result = avcodec_receive_frame(codecContext, frame);
        if (result == AVERROR(EAGAIN) || result == AVERROR_EOF)
            return 0;

        if (result < 0) {
            logging("Error: Failed to receive a frame from the decoder: %s", AVError(result));
            return result;
        }

        logging("Frame %d (type=%c, size=%d bytes) pts %d key_frame %d [DTS %d]", codecContext->frame_number, av_get_picture_type_char(frame->pict_type), frame->pkt_size, frame->pts, frame->key_frame, frame->coded_picture_number);

        char frameFilename[1024];
        snprintf(frameFilename, sizeof(frameFilename), "frame-%d.pgm", codecContext->frame_number);

        if (!outputGrayscaleKeyframe(frame->data[0], frame->linesize[0], frame->width, frame->height, frameFilename))
            logging("Error: Failed to write frame contents to %s.", frameFilename);

        av_frame_unref(frame);
    }
    return 0;
}

static bool outputGrayscaleKeyframe(const unsigned char* buffer, int lineSize, int width, int height, const char* filename)
{
    FILE* f = fopen(filename, "w");
    if (!f)
        return false;

    // For format description, see <https://en.wikipedia.org/wiki/Netpbm_format#PGM_example>
    fprintf(f, "P5\n%d %d\n%d\n", width, height, 255);

    // We cannot write the entire contents of buffer, because each horizontal line may contain additional padding bytes
    // for performance reasons. lineSize includes this padding, so use it to determine the start of each row. See
    // <https://libav.org/documentation/doxygen/master/structAVFrame.html#aa52bfc6605f6a3059a0c3226cc0f6567>.
    for (int i = 0; i < height; ++i) {
        auto lineStart = &buffer[i * lineSize * sizeof(buffer[0])];
        fwrite(lineStart, sizeof(buffer[0]), width, f);
    }

    fclose(f);
    return true;
}
