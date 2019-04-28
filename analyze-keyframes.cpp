/*
 * http://ffmpeg.org/doxygen/trunk/index.html
 *
 * Main components
 *
 * Format (Container) - a wrapper, providing sync, metadata and muxing for the streams.
 * Stream - a continuous stream (audio or video) of data over time.
 * Codec  - defines how data are enCOded (from Frame to Packet)
 *          and DECoded (from Packet to Frame).
 * Packet - are the data (kind of slices of the stream data) to be decoded as raw frames.
 * Frame  - a decoded raw frame (to be encoded or filtered).
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
    UNUSED_PARAM(argc);

    logging("initializing all the containers, codecs and protocols.");

    // AVFormatContext holds the header information from the format (Container)
    // Allocating memory for this component
    // http://ffmpeg.org/doxygen/trunk/structAVFormatContext.html
    AVFormatContext* pFormatContext = avformat_alloc_context();
    if (!pFormatContext) {
        logging("ERROR could not allocate memory for Format Context");
        return -1;
    }

    logging("opening the input file (%s) and loading format (container) header", argv[1]);
    // Open the file and read its header. The codecs are not opened.
    // The function arguments are:
    // AVFormatContext (the component we allocated memory for),
    // url (filename),
    // AVInputFormat (if you pass NULL it'll do the auto detect)
    // and AVDictionary (which are options to the demuxer)
    // http://ffmpeg.org/doxygen/trunk/group__lavf__decoding.html#ga31d601155e9035d5b0e7efedc894ee49
    if (avformat_open_input(&pFormatContext, argv[1], NULL, NULL) != 0) {
        logging("ERROR could not open the file");
        return -1;
    }

    // now we have access to some information about our file
    // since we read its header we can say what format (container) it's
    // and some other information related to the format itself.
    logging("format %s, duration %lld us, bit_rate %lld", pFormatContext->iformat->name, pFormatContext->duration, pFormatContext->bit_rate);

    logging("finding stream info from format");
    // read Packets from the Format to get stream information
    // this function populates pFormatContext->streams
    // (of size equals to pFormatContext->nb_streams)
    // the arguments are:
    // the AVFormatContext
    // and options contains options for codec corresponding to i-th stream.
    // On return each dictionary will be filled with options that were not found.
    // https://ffmpeg.org/doxygen/trunk/group__lavf__decoding.html#gad42172e27cddafb81096939783b157bb
    if (avformat_find_stream_info(pFormatContext, NULL) < 0) {
        logging("ERROR could not get the stream info");
        return -1;
    }

    // the component that knows how to enCOde and DECode the stream
    // it's the codec (audio or video)
    // http://ffmpeg.org/doxygen/trunk/structAVCodec.html
    AVCodec* pCodec = NULL;
    // this component describes the properties of a codec used by the stream i
    // https://ffmpeg.org/doxygen/trunk/structAVCodecParameters.html
    AVCodecParameters* pCodecParameters = NULL;
    int video_stream_index = -1;

    // loop though all the streams and print its main information
    for (unsigned i = 0; i < pFormatContext->nb_streams; i++) {
        AVCodecParameters* pLocalCodecParameters = NULL;
        pLocalCodecParameters = pFormatContext->streams[i]->codecpar;
        logging("AVStream->time_base before open coded %d/%d", pFormatContext->streams[i]->time_base.num, pFormatContext->streams[i]->time_base.den);
        logging("AVStream->r_frame_rate before open coded %d/%d", pFormatContext->streams[i]->r_frame_rate.num, pFormatContext->streams[i]->r_frame_rate.den);
        logging("AVStream->start_time %" PRId64, pFormatContext->streams[i]->start_time);
        logging("AVStream->duration %" PRId64, pFormatContext->streams[i]->duration);

        logging("finding the proper decoder (CODEC)");

        AVCodec* pLocalCodec = NULL;

        // finds the registered decoder for a codec ID
        // https://ffmpeg.org/doxygen/trunk/group__lavc__decoding.html#ga19a0ca553277f019dd5b0fec6e1f9dca
        pLocalCodec = avcodec_find_decoder(pLocalCodecParameters->codec_id);

        if (pLocalCodec == NULL) {
            logging("ERROR unsupported codec!");
            return -1;
        }

        // when the stream is a video we store its index, codec parameters and codec
        if (pLocalCodecParameters->codec_type == AVMEDIA_TYPE_VIDEO) {
            video_stream_index = i;
            pCodec = pLocalCodec;
            pCodecParameters = pLocalCodecParameters;

            logging("Video Codec: resolution %d x %d", pLocalCodecParameters->width, pLocalCodecParameters->height);
        }
        else if (pLocalCodecParameters->codec_type == AVMEDIA_TYPE_AUDIO) {
            logging("Audio Codec: %d channels, sample rate %d", pLocalCodecParameters->channels, pLocalCodecParameters->sample_rate);
        }

        // print its name, id and bitrate
        logging("\tCodec %s ID %d bit_rate %lld", pLocalCodec->name, pLocalCodec->id, pLocalCodecParameters->bit_rate);
    }
    // https://ffmpeg.org/doxygen/trunk/structAVCodecContext.html
    AVCodecContext* pCodecContext = avcodec_alloc_context3(pCodec);
    if (!pCodecContext) {
        logging("failed to allocated memory for AVCodecContext");
        return -1;
    }

    // Fill the codec context based on the values from the supplied codec parameters
    // https://ffmpeg.org/doxygen/trunk/group__lavc__core.html#gac7b282f51540ca7a99416a3ba6ee0d16
    if (avcodec_parameters_to_context(pCodecContext, pCodecParameters) < 0) {
        logging("failed to copy codec params to codec context");
        return -1;
    }

    // Initialize the AVCodecContext to use the given AVCodec.
    // https://ffmpeg.org/doxygen/trunk/group__lavc__core.html#ga11f785a188d7d9df71621001465b0f1d
    if (avcodec_open2(pCodecContext, pCodec, NULL) < 0) {
        logging("failed to open codec through avcodec_open2");
        return -1;
    }

    // https://ffmpeg.org/doxygen/trunk/structAVFrame.html
    AVFrame* pFrame = av_frame_alloc();
    if (!pFrame) {
        logging("failed to allocated memory for AVFrame");
        return -1;
    }
    // https://ffmpeg.org/doxygen/trunk/structAVPacket.html
    AVPacket* pPacket = av_packet_alloc();
    if (!pPacket) {
        logging("failed to allocated memory for AVPacket");
        return -1;
    }

    int response = 0;

    pCodecContext->skip_frame = AVDISCARD_NONKEY;

    // fill the Packet with data from the Stream
    // https://ffmpeg.org/doxygen/trunk/group__lavf__decoding.html#ga4fdb3084415a82e3810de6ee60e46a61
    while (av_read_frame(pFormatContext, pPacket) >= 0) {
        if (pPacket->stream_index != video_stream_index)
            continue;

        logging("AVPacket->pts %" PRId64, pPacket->pts);
        response = decode_packet(pPacket, pCodecContext, pFrame);
        if (response < 0)
            break;

        // https://ffmpeg.org/doxygen/trunk/group__lavc__packet.html#ga63d5a489b419bd5d45cfd09091cbcbc2
        av_packet_unref(pPacket);
    }

    logging("releasing all the resources");

    avformat_close_input(&pFormatContext);
    avformat_free_context(pFormatContext);
    av_packet_free(&pPacket);
    av_frame_free(&pFrame);
    avcodec_free_context(&pCodecContext);
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
