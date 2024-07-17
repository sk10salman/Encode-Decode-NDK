#include <jni.h>
#include <android/log.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <media/NdkMediaCodec.h>
#include <media/NdkMediaFormat.h>
#include <media/NdkMediaExtractor.h>
#include <media/NdkMediaMuxer.h>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <queue>

extern "C" {

// Global variables for synchronization
std::mutex gDecoderMutex;
std::condition_variable gDecoderCV;
std::queue<ssize_t> gDecodedFrames;  // Queue to hold decoded frame indices

std::mutex gEncoderMutex;
std::condition_variable gEncoderCV;
std::queue<ssize_t> gEncodedFrames;  // Queue to hold encoded frame indices

// Function prototypes
void encodeVideo(const char* inputPath, const char* outputPath);
void decodeVideo(const char* inputPath, const char* outputPath);
int openOutputFile(const char* outputPath);
size_t getFileSize(const char* filePath);

JNIEXPORT void JNICALL
Java_com_example_mediaprocessing_MediaCodecHelper_nativeEncodeVideo(JNIEnv *env, jobject /* this */,
                                                                    jstring inputPath_,
                                                                    jstring outputPath_) {
    const char *inputPath = env->GetStringUTFChars(inputPath_, nullptr);
    const char *outputPath = env->GetStringUTFChars(outputPath_, nullptr);

    encodeVideo(inputPath, outputPath);

    env->ReleaseStringUTFChars(inputPath_, inputPath);
    env->ReleaseStringUTFChars(outputPath_, outputPath);
}

JNIEXPORT void JNICALL
Java_com_example_mediaprocessing_MediaCodecHelper_nativeDecodeVideo(JNIEnv *env, jobject /* this */,
                                                                    jstring inputPath_,
                                                                    jstring outputPath_) {
    const char *inputPath = env->GetStringUTFChars(inputPath_, nullptr);
    const char *outputPath = env->GetStringUTFChars(outputPath_, nullptr);

    decodeVideo(inputPath, outputPath);

    env->ReleaseStringUTFChars(inputPath_, inputPath);
    env->ReleaseStringUTFChars(outputPath_, outputPath);
}

// Decoder thread function
void decodeThread(AMediaExtractor *extractor, AMediaCodec *decoder, bool *sawInputEOS) {
    while (!(*sawInputEOS)) {
        std::unique_lock<std::mutex> lock(gDecoderMutex);
        gDecoderCV.wait(lock, []{ return !gDecodedFrames.empty(); });  // Wait for frames to decode
        ssize_t outputBufferIndex = gDecodedFrames.front();
        gDecodedFrames.pop();
        lock.unlock();

        // Process the decoded frame
        AMediaCodec_releaseOutputBuffer(decoder, outputBufferIndex, false);

        // Signal availability of decoded frame to encoder
        gEncoderCV.notify_one();

        // Advance to the next sample
        AMediaExtractor_advance(extractor);
    }
}

// Encoder thread function
void encodeThread(AMediaCodec *encoder, AMediaMuxer *muxer, int trackIndex, bool *sawOutputEOS) {
    while (!(*sawOutputEOS)) {
        std::unique_lock<std::mutex> lock(gEncoderMutex);
        gEncoderCV.wait(lock, []{ return !gEncodedFrames.empty(); });  // Wait for frames to encode
        ssize_t outputBufferIndex = gEncodedFrames.front();
        gEncodedFrames.pop();
        lock.unlock();

        // Process the encoded frame
        AMediaCodecBufferInfo info;
        ssize_t inputBufferIndex = AMediaCodec_dequeueInputBuffer(encoder, 10000);  // Timeout in microseconds
        if (inputBufferIndex >= 0) {
            uint8_t *inputBuffer = AMediaCodec_getInputBuffer(encoder, inputBufferIndex, nullptr);
            ssize_t bytesRead = AMediaCodec_getOutputBuffer(encoder, outputBufferIndex, nullptr);

            AMediaMuxer_writeSampleData(muxer, trackIndex, inputBuffer, &info);
            AMediaCodec_releaseOutputBuffer(encoder, outputBufferIndex, false);

            if (info.flags & AMEDIACODEC_BUFFER_FLAG_END_OF_STREAM) {
                *sawOutputEOS = true;
            }
        }
    }
}

void encodeVideo(const char* inputPath, const char* outputPath) {
    AMediaExtractor *extractor = nullptr;
    AMediaCodec *decoder = nullptr;
    AMediaCodec *encoder = nullptr;
    AMediaFormat *format = nullptr;
    AMediaMuxer *muxer = nullptr;

    // Open input file and get file descriptor
    int inputFd = open(inputPath, O_RDONLY);
    if (inputFd < 0) {
        __android_log_print(ANDROID_LOG_ERROR, "MediaCodec", "Failed to open input file: %s", strerror(errno));
        return;
    }

    // Initialize MediaExtractor from FD
    extractor = AMediaExtractor_new();
    media_status_t status = AMediaExtractor_setDataSourceFd(extractor, inputFd, 0, getFileSize(inputPath));
    if (status != AMEDIA_OK) {
        __android_log_print(ANDROID_LOG_ERROR, "MediaCodec", "Failed to set data source for %s", inputPath);
        close(inputFd);
        return;
    }

    // Get video track format from extractor
    int trackCount = AMediaExtractor_getTrackCount(extractor);
    AMediaFormat *trackFormat = nullptr;
    int videoTrackIndex = -1;
    for (int i = 0; i < trackCount; ++i) {
        trackFormat = AMediaExtractor_getTrackFormat(extractor, i);
        const char *mime;
        if (AMediaFormat_getString(trackFormat, AMEDIAFORMAT_KEY_MIME, &mime) && !strncmp(mime, "video/", 6)) {
            videoTrackIndex = i;
            break;
        }
    }

    if (videoTrackIndex < 0) {
        __android_log_print(ANDROID_LOG_ERROR, "MediaCodec", "No video track found");
        close(inputFd);
        AMediaExtractor_delete(extractor);
        return;
    }

    AMediaExtractor_selectTrack(extractor, videoTrackIndex);

    // Initialize MediaCodec decoder
    decoder = AMediaCodec_createDecoderByType("video/avc");
    if (!decoder) {
        __android_log_print(ANDROID_LOG_ERROR, "MediaCodec", "Failed to create decoder");
        close(inputFd);
        AMediaExtractor_delete(extractor);
        return;
    }

    AMediaCodec_configure(decoder, trackFormat, nullptr, nullptr, 0);
    AMediaCodec_start(decoder);

    // Initialize MediaCodec encoder
    encoder = AMediaCodec_createEncoderByType("video/avc");
    if (!encoder) {
        __android_log_print(ANDROID_LOG_ERROR, "MediaCodec", "Failed to create encoder");
        close(inputFd);
        AMediaCodec_stop(decoder);
        AMediaCodec_delete(decoder);
        AMediaExtractor_delete(extractor);
        return;
    }

    format = AMediaFormat_new();
    AMediaFormat_setString(format, AMEDIAFORMAT_KEY_MIME, "video/avc");
    AMediaFormat_setInt32(format, AMEDIAFORMAT_KEY_WIDTH, 1280);  // Specify your video width
    AMediaFormat_setInt32(format, AMEDIAFORMAT_KEY_HEIGHT, 720);  // Specify your video height
    AMediaFormat_setInt32(format, AMEDIAFORMAT_KEY_BIT_RATE, 2000000);  // Specify your video bit rate
    AMediaFormat_setInt32(format, AMEDIAFORMAT_KEY_FRAME_RATE, 30);  // Specify your video frame rate
    AMediaFormat_setInt32(format, AMEDIAFORMAT_KEY_COLOR_FORMAT, 19);  // COLOR_FormatYUV420Planar (NV12)

    AMediaCodec_configure(encoder, format, nullptr, nullptr, AMEDIACODEC_CONFIGURE_FLAG_ENCODE);
    AMediaCodec_start(encoder);

    // Start decoder thread
    bool sawInputEOS = false;
    std::thread decoderThread(decodeThread, extractor, decoder, &sawInputEOS);

    // Start encoder thread
    bool sawOutputEOS = false;
    AMediaFormat *encoderFormat = AMediaCodec_getOutputFormat(encoder);
    int trackIndex = AMediaMuxer_addTrack(muxer, encoderFormat);
    AMediaMuxer_start(muxer);
    std::thread encoderThread(encodeThread, encoder, muxer, trackIndex, &sawOutputEOS);

    // Main decoding loop
    while (!sawInputEOS) {
        ssize_t inputBufferIndex = AMediaCodec_dequeueInputBuffer(decoder, 10000);  // Timeout in microseconds
        if (inputBufferIndex >= 0) {
            uint8_t *inputBuffer = AMediaCodec_getInputBuffer(decoder, inputBufferIndex, nullptr);

            // Read sample data from extractor
            ssize_t sampleSize = AMediaExtractor_readSampleData(extractor, inputBuffer, inputBufferIndex);
            if (sampleSize < 0) {
                sawInputEOS = true;
                sampleSize = 0;
            }
            int64_t sampleTime = AMediaExtractor_getSampleTime(extractor);
            uint32_t sampleFlags = AMediaExtractor_getSampleFlags(extractor);

            // Queue input buffer to decoder
            AMediaCodec_queueInputBuffer(decoder, inputBufferIndex, 0, sampleSize, sampleTime, sampleFlags);

            // Advance extractor to next sample
            AMediaExtractor_advance(extractor);
        }

        // Handle decoder output buffers
        AMediaCodecBufferInfo info;
        ssize_t outputBufferIndex = AMediaCodec_dequeueOutputBuffer(decoder, &info, 10000);
        if (outputBufferIndex >= 0) {
            std::unique_lock<std::mutex> lock(gDecoderMutex);
            gDecodedFrames.push(outputBufferIndex);
            lock.unlock();
            gDecoderCV.notify_one();
        }
    }

    // Signal end of input to decoder
    decoderThread.join();  // Wait for decoder thread to finish

    // Signal end of input to encoder
    AMediaCodec_signalEndOfInputStream(encoder);

    // Wait for encoder thread to finish
    encoderThread.join();

    // Clean up
    close(inputFd);
    AMediaCodec_stop(decoder);
    AMediaCodec_delete(decoder);
    AMediaCodec_stop(encoder);
    AMediaCodec_delete(encoder);
    AMediaExtractor_delete(extractor);
    AMediaMuxer_stop(muxer);
    AMediaMuxer_delete(muxer);
}

void decodeVideo(const char* inputPath, const char* outputPath) {
    // Function not implemented in this example
}

// Helper function to open output file and return file descriptor
int openOutputFile(const char* outputPath) {
    int outputFd = open(outputPath, O_CREAT | O_WRONLY | O_TRUNC, S_IRUSR | S_IWUSR);
    if (outputFd < 0) {
        __android_log_print(ANDROID_LOG_ERROR, "MediaCodec", "Failed to open output file: %s", strerror(errno));
    }
    return outputFd;
}

// Helper function to get file size
size_t getFileSize(const char* filePath) {
    struct stat st;
    if (stat(filePath, &st) == 0) {
        return st.st_size;
    }
    return 0;
}

} // extern "C"
