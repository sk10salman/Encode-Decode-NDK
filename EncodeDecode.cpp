#include <jni.h>
#include <android/log.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <media/NdkMediaCodec.h>
#include <media/NdkMediaFormat.h>
#include <media/NdkMediaExtractor.h>
#include <media/NdkMediaMuxer.h>

extern "C" {

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

void encodeVideo(const char* inputPath, const char* outputPath) {
    AMediaExtractor *extractor = nullptr;
    AMediaCodec *encoder = nullptr;
    AMediaCodec *decoder = nullptr;
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

    // Initialize MediaCodec encoder
    encoder = AMediaCodec_createEncoderByType("video/avc");
    if (!encoder) {
        __android_log_print(ANDROID_LOG_ERROR, "MediaCodec", "Failed to create encoder");
        close(inputFd);
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

    // Initialize MediaCodec decoder
    decoder = AMediaCodec_createDecoderByType("video/avc");
    if (!decoder) {
        __android_log_print(ANDROID_LOG_ERROR, "MediaCodec", "Failed to create decoder");
        close(inputFd);
        AMediaCodec_stop(encoder);
        AMediaCodec_delete(encoder);
        AMediaExtractor_delete(extractor);
        AMediaFormat_delete(format);
        return;
    }

    AMediaCodec_configure(decoder, trackFormat, nullptr, nullptr, 0);
    AMediaCodec_start(decoder);

    // Open output file and get file descriptor
    int outputFd = openOutputFile(outputPath);
    if (outputFd < 0) {
        __android_log_print(ANDROID_LOG_ERROR, "MediaCodec", "Failed to open output file");
        close(inputFd);
        AMediaCodec_stop(encoder);
        AMediaCodec_delete(encoder);
        AMediaCodec_stop(decoder);
        AMediaCodec_delete(decoder);
        AMediaExtractor_delete(extractor);
        AMediaFormat_delete(format);
        return;
    }

    // Initialize MediaMuxer from FD for output MP4 file
    muxer = AMediaMuxer_newFromFd(outputFd, AMEDIAMUXER_OUTPUT_FORMAT_MPEG_4);
    if (!muxer) {
        __android_log_print(ANDROID_LOG_ERROR, "MediaCodec", "Failed to create muxer");
        close(inputFd);
        close(outputFd);
        AMediaCodec_stop(encoder);
        AMediaCodec_delete(encoder);
        AMediaCodec_stop(decoder);
        AMediaCodec_delete(decoder);
        AMediaExtractor_delete(extractor);
        AMediaFormat_delete(format);
        return;
    }

    // Read and decode frames, then encode and write to muxer
    ssize_t inputIndex = -1;
    ssize_t outputIndex = -1;
    int trackIndex = -1;

    AMediaCodecBufferInfo info;
    bool sawInputEOS = false;
    bool sawOutputEOS = false;

    while (!sawOutputEOS) {
        if (!sawInputEOS) {
            ssize_t inputBufferIndex = AMediaCodec_dequeueInputBuffer(decoder, 10000);  // Timeout in microseconds
            if (inputBufferIndex >= 0) {
                uint8_t *inputBuffer = AMediaCodec_getInputBuffer(decoder, inputBufferIndex, &inputIndex);

                // Get sample size and time
                ssize_t sampleSize = AMediaExtractor_getSampleSize(extractor);
                if (sampleSize < 0) {
                    sawInputEOS = true;
                    sampleSize = 0;
                }
                int64_t sampleTime = AMediaExtractor_getSampleTime(extractor);
                uint32_t sampleFlags = AMediaExtractor_getSampleFlags(extractor);

                // Copy sample data directly
                if (sampleSize > 0) {
                    ssize_t bytesRead = AMediaExtractor_readSampleData(extractor, inputBuffer, sampleSize);
                    if (bytesRead != sampleSize) {
                        __android_log_print(ANDROID_LOG_ERROR, "MediaCodec", "Error reading sample data: %zd", bytesRead);
                        break;
                    }
                }

                // Queue input buffer
                AMediaCodec_queueInputBuffer(decoder, inputBufferIndex, 0, sampleSize, sampleTime, sampleFlags);

                // Advance to the next sample
                AMediaExtractor_advance(extractor);
            }
        }

        ssize_t outputBufferIndex = AMediaCodec_dequeueOutputBuffer(decoder, &info, 10000);
        if (outputBufferIndex >= 0) {
            uint8_t *outputBuffer = AMediaCodec_getOutputBuffer(decoder, outputBufferIndex, &outputIndex);

            ssize_t inputBufferIndex = AMediaCodec_dequeueInputBuffer(encoder, 10000);  // Timeout in microseconds
            if (inputBufferIndex >= 0) {
                uint8_t *inputBuffer = AMediaCodec_getInputBuffer(encoder, inputBufferIndex, &inputIndex);
                memcpy(inputBuffer, outputBuffer, info.size);
                AMediaCodec_queueInputBuffer(encoder, inputBufferIndex, 0, info.size, info.presentationTimeUs, 0);

                // Write encoded frame to muxer
                AMediaCodecBufferInfo encodeInfo;
                ssize_t encodeOutputIndex = AMediaCodec_dequeueOutputBuffer(encoder, &encodeInfo, 10000);
                if (encodeOutputIndex >= 0) {
                    size_t encodedDataSize;
                    uint8_t *encodedData = AMediaCodec_getOutputBuffer(encoder, encodeOutputIndex, &encodedDataSize);
                    AMediaMuxer_writeSampleData(muxer, trackIndex, encodedData, &encodeInfo);
                    AMediaCodec_releaseOutputBuffer(encoder, encodeOutputIndex, false);
                }
            }

            AMediaCodec_releaseOutputBuffer(decoder, outputBufferIndex, false);
            if (info.flags & AMEDIACODEC_BUFFER_FLAG_END_OF_STREAM) {
                sawOutputEOS = true;
            }
        } else if (outputBufferIndex == AMEDIACODEC_INFO_OUTPUT_FORMAT_CHANGED) {
            AMediaFormat *format = AMediaCodec_getOutputFormat(decoder);
            trackIndex = AMediaMuxer_addTrack(muxer, format);
            AMediaMuxer_start(muxer);
        } else if (outputBufferIndex == AMEDIACODEC_INFO_TRY_AGAIN_LATER) {
            // Handle timeout
        }
    }

    // Clean up
    close(inputFd);
    close(outputFd);
    AMediaCodec_stop(encoder);
    AMediaCodec_delete(encoder);
    AMediaCodec_stop(decoder);
    AMediaCodec_delete(decoder);
    AMediaExtractor_delete(extractor);
    AMediaFormat_delete(format);
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
