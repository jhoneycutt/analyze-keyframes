/*
 * analyze-keyframes:
 *   A program that uses the libraries provided by ffmpeg to analyze
 *   keyframes from a video file.
 *
 *  Copyright:
 *    Leandro Moreira (2017) <https://github.com/leandromoreira>
 *    Jon Honeycutt   (2019) <jhoneycutt@gmail.com>
 *
 *  License:
 *    BSD 3-clause; see LICENSE.
 */

#include "analyze-keyframes.h"

#include <algorithm>
#include <chrono>
#include <array>
#include <cinttypes>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <list>
#include <mutex>
#include <set>
#include <thread>
#include <vector>

static const unsigned VerticalCellCount = 3;
static const unsigned HorizontalCellCount = 3;
static const char* FrameAnalysisCSVFile = "frame-analysis.csv";
static const unsigned MaxUnprocessedFrameCount = 100;

// Outputs each keyframe as an 8bpp grayscale bitmap, named like frame-0.pgm. To convert them to JPEG, you can use
// mogrify from ImageMagick, like: mogrify -format jpeg *.pgm
static const bool OutputKeyframeImages = false;

using std::array;
using std::endl;
using std::ios;
using std::ofstream;
using std::list;
using std::lock_guard;
using std::mutex;
using std::set;
using std::string;
using std::thread;
using std::unique_ptr;
using std::vector;

using namespace std::chrono;

static void logging(const char* format, ...);
static bool processPacket(const AVPacket*, AVCodecContext*);
static bool outputGrayscaleFrame(AVFrame*, const char* filename);
static void analyzeGrayscaleFrame(AVFrame*);
static void processKeyframe(AVFrame*);
static string AVError(int errorCode);
static void workerThread();
static bool outputFrameAnalysis();

struct FrameAnalysis
{
    float timestamp;
    int frameNumber;
    array<float, VerticalCellCount * HorizontalCellCount> values;
};

bool processingComplete = false;

mutex unprocessedFramesMutex;
list<AVFramePtr> unprocessedFrames;

mutex processedFrameDataMutex;
auto comparator = [](const unique_ptr<FrameAnalysis>& a, const unique_ptr<FrameAnalysis>& b) { return a->frameNumber < b->frameNumber; };
set<unique_ptr<FrameAnalysis>, decltype(comparator)> processedFrameData(comparator);

AVRational videoTimeBase;

int main(int argc, const char* argv[])
{
    if (argc < 2) {
        logging("Usage: %s <video file>", argv[0]);
        return -1;
    }

    auto inputFile = argv[1];

    logging("Opening input file %s...", inputFile);

    // It's not possible to get a pointer to a unique_ptr's internal pointer, but avformat_open_input takes a pointer
    // to the dest pointer, so we pass a raw pointer and then "adopt" it into the AVInputFileFormatContextPtr.
    AVFormatContext* formatContextRawPointer = nullptr;
    int result = avformat_open_input(&formatContextRawPointer, inputFile, nullptr, nullptr);
    AVInputFileFormatContextPtr formatContext(formatContextRawPointer);
    if (result) {
        logging("Error: Failed to open input file: %s", AVError(result).c_str());
        return -1;
    }

    logging("    Format %s, duration %lld us, bit_rate %lld\n", formatContext->iformat->name, formatContext->duration, formatContext->bit_rate);

    result = avformat_find_stream_info(formatContext.get(), nullptr);
    if (result) {
        logging("Error: Failed to find stream info: %s", AVError(result).c_str());
        return -1;
    }

    AVCodec* videoCodec = nullptr;
    AVCodecParameters* videoCodecParameters = nullptr;
    int videoStreamIndex = 0;

    // Loop though all streams, and print some information about them. Find the first video stream for which we have a
    // decoder installed; this is the stream we'll analyze.
    for (unsigned i = 0; i < formatContext->nb_streams; i++) {
        AVStream* stream = formatContext->streams[i];
        logging("Stream #%u", i);
        logging("    AVStream->time_base before open coded %d/%d", stream->time_base.num, stream->time_base.den);
        logging("    AVStream->r_frame_rate before open coded %d/%d", stream->r_frame_rate.num, stream->r_frame_rate.den);
        logging("    AVStream->start_time %" PRId64, stream->start_time);
        logging("    AVStream->duration %" PRId64, stream->duration);
        logging("");

        AVCodecParameters* codecParameters = stream->codecpar;
        AVCodec* codec = avcodec_find_decoder(codecParameters->codec_id);
        if (!codec) {
            logging("Warning: No codec found for stream.");
            continue;
        }

        if (!videoCodec && codecParameters->codec_type == AVMEDIA_TYPE_VIDEO) {
            // FIXME: Is it possible to have multiple video streams? What should the behavior be in that case?
            videoStreamIndex = i;
            videoCodec = codec;
            videoCodecParameters = codecParameters;
            videoTimeBase = stream->time_base;
            logging("    Video Codec: resolution %d x %d", codecParameters->width, codecParameters->height);
        } else if (codecParameters->codec_type == AVMEDIA_TYPE_AUDIO)
            logging("    Audio Codec: channels %d, sample rate %d", codecParameters->channels, codecParameters->sample_rate);

        logging("        Codec name %s, ID %d, bit_rate %lld\n", codec->name, codec->id, codecParameters->bit_rate);
    }

    if (!videoCodec) {
        logging("Error: Failed to find a decodable video stream in input file.");
        return -1;
    }

    int width = videoCodecParameters->width;
    int height = videoCodecParameters->height;
    if (width <= 0 || static_cast<unsigned>(width) < HorizontalCellCount ||
        height <= 0 || static_cast<unsigned>(height) < VerticalCellCount) {
        logging("Error: Width and/or height of video stream is less than desired cell count.");
        return -1;
    }

    AVCodecContextPtr codecContext(avcodec_alloc_context3(videoCodec));
    result = avcodec_parameters_to_context(codecContext.get(), videoCodecParameters);
    if (result < 0) {
        logging("Error: Failed to copy codec parameters to codec context: %s", AVError(result).c_str());
        return -1;
    }

    result = avcodec_open2(codecContext.get(), videoCodec, nullptr);
    if (result < 0) {
        logging("Error: Failed to open codec: %s", AVError(result).c_str());
        return -1;
    }

    // Skip non-keyframes when processing.
    codecContext->skip_frame = AVDISCARD_NONKEY;

    // Remove the existing analysis file, if any.
    remove(FrameAnalysisCSVFile);

    unsigned threadCount = thread::hardware_concurrency();
    threadCount = threadCount ? threadCount : 4;
    vector<thread> threads;
    for (unsigned i = 0; i < threadCount; ++i)
        threads.push_back(thread(workerThread));

    while (true) {
        unsigned unprocessedFrameCount;
        {
            lock_guard<mutex> lock(unprocessedFramesMutex);
            unprocessedFrameCount = unprocessedFrames.size();
        }
        if (unprocessedFrameCount >= MaxUnprocessedFrameCount) {
            logging("Unprocessed frame buffer is full, sleeping...");
            std::this_thread::sleep_for(milliseconds(10));
            continue;
        }

        AVPacketPtr packet(av_packet_alloc());
        result = av_read_frame(formatContext.get(), packet.get());
        if (result == AVERROR_EOF) {
            // If we reach the end of the stream, exit cleanly.
            break;
        }

        if (result < 0) {
            logging("Error: Failed to read packet from stream: %s", AVError(result).c_str());
            return -1;
        }

        if (packet->stream_index != videoStreamIndex)
            continue;

        if (!processPacket(packet.get(), codecContext.get())) {
            logging("Error: Failed to process packet.");
            return -1;
        }
    }


    processingComplete = true;
    for (auto&& t : threads)
        t.join();

    // FIXME: We should write out the frame data during processing rather than letting it accumulate. 
    outputFrameAnalysis();

    logging("Processing complete.");

    return 0;
}

static string AVError(int errorCode)
{
    char errorString[AV_ERROR_MAX_STRING_SIZE];
    av_make_error_string(errorString, AV_ERROR_MAX_STRING_SIZE, errorCode);
    return errorString;
}

mutex loggingMutex;
static void logging(const char* fmt, ...)
{
    lock_guard<mutex> lock(loggingMutex);

    va_list args;
    va_start(args, fmt);
    vfprintf(stderr, fmt, args);
    va_end(args);
    fprintf(stderr, "\n");
}

static bool processPacket(const AVPacket* packet, AVCodecContext* codecContext)
{
    int result = avcodec_send_packet(codecContext, packet);
    if (result < 0) {
        logging("Error: Failed sending packet to the decoder: %s", AVError(result).c_str());
        return false;
    }

    while (true) {
        // Process a single frame from the decoder. If the decoder returns EAGAIN, more input data is needed to decode
        // the next frame. If it returns EOF, we've reached the end of the stream.
        AVFramePtr frame(av_frame_alloc());
        result = avcodec_receive_frame(codecContext, frame.get());
        if (result == AVERROR(EAGAIN) || result == AVERROR_EOF)
            return true;

        if (result < 0) {
            logging("Error: Failed to receive a frame from the decoder: %s", AVError(result).c_str());
            return false;
        }

        lock_guard<mutex> lock(unprocessedFramesMutex);
        unprocessedFrames.push_back(std::move(frame));
    }

    return true;
}

static void workerThread()
{
    while (true) {
        AVFramePtr frame;
        {
            lock_guard<mutex> lock(unprocessedFramesMutex);
            if (unprocessedFrames.size()) {
                frame.swap(unprocessedFrames.front());
                unprocessedFrames.pop_front();
            }
        }

        if (frame)
            processKeyframe(frame.get());

        if (processingComplete)
            return;
    }
}


static void processKeyframe(AVFrame* frame)
{
    logging("Processing keyframe pts %d dts %d...", frame->pts, frame->coded_picture_number);

    AVFramePtr frameGrayscale(av_frame_clone(frame));
    int width = frame->width;
    int height = frame->height;
    int result = av_image_alloc(frameGrayscale->data, frameGrayscale->linesize, width, height, AV_PIX_FMT_GRAY8, 32);
    if (result < 0) {
        logging("Error: Failed to allocate grayscale image for frame: %s", AVError(result).c_str());
        return;
    }

    AVPixelFormat srcFormat = static_cast<AVPixelFormat>(frame->format);
    AVPixelFormat destFormat = AV_PIX_FMT_GRAY8;
    int startRow = 0;
    int rowCount = height;
    SwsContextPtr conversionContext(sws_getContext(width, height, srcFormat, width, height, destFormat, SWS_BILINEAR, nullptr, nullptr, nullptr));
    sws_scale(conversionContext.get(), frame->data, frame->linesize, startRow, rowCount, frameGrayscale->data, frameGrayscale->linesize);

    if (OutputKeyframeImages) {
        char frameFilename[1024];
        snprintf(frameFilename, sizeof(frameFilename), "frame-%d.pgm", frame->coded_picture_number);
        outputGrayscaleFrame(frameGrayscale.get(), frameFilename);
    }

    analyzeGrayscaleFrame(frameGrayscale.get());

    // It's necessary to manually free the data pointer after calling av_image_alloc. See
    // <https://ffmpeg.org/doxygen/4.1/group__lavu__picture.html#ga841e0a89a642e24141af1918a2c10448>.
    av_freep(&frameGrayscale->data[0]);
}

static inline float median(vector<uint8_t>& v)
{
    int middle = v.size() / 2;
    nth_element(v.begin(), v.begin() + middle, v.end());
    float median = v[middle];

    // For sets with an odd number of items, the median is the middle element.
    if (v.size() & 1)
        return median;

    // For sets with an even number of items, the median is the average of the middle two elements. It is more efficient
    // to call max_element on the lower half of the list than it is to call nth_element() again.
    // FIXME: Although this gives the true median, it's probably unnecessarily precise. For our purposes, it's probably
    // fine to just return the first value found above and avoid another O(n) operation.
    auto iter = max_element(v.begin(), v.begin() + middle - 1);
    return (median + *iter) / 2;
}

static inline float cellMedian(const uint8_t* data, int lineSize, unsigned xOffset, unsigned xPixels, unsigned yOffset, unsigned yPixels)
{
    vector<uint8_t> pixels(xPixels * yPixels);
    for (unsigned y = 0; y < yPixels; ++y) {
        // Horizontal lines may contain additional padding bytes. lineSize includes this padding, so use it to determine
        // the start of each row. See <https://ffmpeg.org/doxygen/trunk/structAVFrame.html#aa52bfc6605f6a3059a0c3226cc0f6567>.
        memcpy(&pixels[y * xPixels], &data[(yOffset + y) * lineSize + xOffset], xPixels);
    }

    return median(pixels);
}

static bool outputFrameAnalysis()
{
    ofstream outputFile(FrameAnalysisCSVFile, ios::app);
    if (!outputFile.good()) {
        logging("Error: Failed to open CSV file.");
        return false;
    }

    for (auto&& analysis : processedFrameData) {
        outputFile << analysis->timestamp;
        for (unsigned i = 0; i < analysis->values.size(); ++i)
            outputFile << "," << analysis->values[i];
        outputFile << endl;
    }

    return true;
}

static void analyzeGrayscaleFrame(AVFrame* frame)
{
    int remainingHeight = frame->height;
    int lineSize = frame->linesize[0];
    auto data = frame->data[0];

    unique_ptr<FrameAnalysis> analysis(new FrameAnalysis);
    analysis->timestamp = frame->best_effort_timestamp * av_q2d(videoTimeBase);
    analysis->frameNumber = frame->coded_picture_number;

    for (unsigned y = 0; y < VerticalCellCount; ++y) {
        unsigned yOffset = frame->height - remainingHeight;
        unsigned yPixels = remainingHeight / (VerticalCellCount - y);
        remainingHeight -= yPixels;

        int remainingWidth = frame->width;
        for (unsigned x = 0; x < HorizontalCellCount; ++x) {
            unsigned xOffset = frame->width - remainingWidth;
            unsigned xPixels = remainingWidth / (HorizontalCellCount - x);
            remainingWidth -= xPixels;

            float median = cellMedian(data, lineSize, xOffset, xPixels, yOffset, yPixels);
            analysis->values[y * HorizontalCellCount + x] = median;
        }
    }

    lock_guard<mutex> lock(processedFrameDataMutex);
    processedFrameData.insert(std::move(analysis));
}

static bool outputGrayscaleFrame(AVFrame* frame, const char* filename)
{
    ofstream outputFile(filename, ios::binary);
    if (!outputFile.good())
        return false;

    int width = frame->width;
    int height = frame->height;

    // For format description, see <https://en.wikipedia.org/wiki/Netpbm_format#PGM_example>
    outputFile << "P5\n" << width << " " << height << "\n" << 255 << "\n";

    // We cannot write the entire contents of buffer, because each horizontal line may contain additional padding bytes
    // for performance reasons. lineSize includes this padding, so use it to determine the start of each row. See
    // <https://ffmpeg.org/doxygen/trunk/structAVFrame.html#aa52bfc6605f6a3059a0c3226cc0f6567>.
    int lineSize = frame->linesize[0];
    auto data = frame->data[0];
    for (int i = 0; i < height; ++i) {
        auto lineStart = &data[i * lineSize];
        outputFile.write(reinterpret_cast<char*>(lineStart), width);
    }

    return true;
}
