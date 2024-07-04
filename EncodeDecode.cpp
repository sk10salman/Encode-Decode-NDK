#include <jni.h>
#include <string>
#include <android/log.h>
#include <unistd.h>
#include <fcntl.h>
#include <media/NdkMediaCodec.h>
#include <media/NdkMediaFormat.h>

extern "C" {

// Function prototypes
void encodeVideo(const char* inputPath, const char* outputPath);
void decodeVideo(const char* inputPath, const char* outputPath);

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
    AMediaCodec *encoder = nullptr;
    AMediaCodec *decoder = nullptr;
    AMediaFormat *format = nullptr;
    FILE *inputFile = nullptr;
    FILE *outputFile = nullptr;

    // Open input file (to read decoded frames)
    inputFile = fopen(inputPath, "rb");
    if (!inputFile) {
        __android_log_print(ANDROID_LOG_ERROR, "MediaCodec", "Failed to open input file");
        return;
    }

    // Open output file (to write encoded frames)
    outputFile = fopen(outputPath, "wb");
    if (!outputFile) {
        __android_log_print(ANDROID_LOG_ERROR, "MediaCodec", "Failed to open output file");
        fclose(inputFile);
        return;
    }

    // Initialize MediaCodec encoder
    encoder = AMediaCodec_createEncoderByType("video/avc");
    if (!encoder) {
        __android_log_print(ANDROID_LOG_ERROR, "MediaCodec", "Failed to create encoder");
        fclose(inputFile);
        fclose(outputFile);
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
        fclose(inputFile);
        fclose(outputFile);
        AMediaCodec_stop(encoder);
        AMediaCodec_delete(encoder);
        AMediaFormat_delete(format);
        return;
    }

    AMediaCodec_configure(decoder, format, nullptr, nullptr, 0);
    AMediaCodec_start(decoder);

    // Read and decode frames
    size_t bufferSize = 1024 * 1024;  // Adjust buffer size as needed
    uint8_t *buffer = new uint8_t[bufferSize];
    size_t inputBufferSize = 0;
    size_t outputBufferSize = 0;
    ssize_t inputIndex = -1;
    ssize_t outputIndex = -1;

    while (true) {
        ssize_t inputBufferIndex = AMediaCodec_dequeueInputBuffer(decoder, 10000);  // Timeout in microseconds
        if (inputBufferIndex >= 0) {
            size_t bytesRead = fread(buffer, 1, bufferSize, inputFile);
            if (bytesRead > 0) {
                inputBufferSize = bytesRead;
                uint8_t *inputBuffer = AMediaCodec_getInputBuffer(decoder, inputBufferIndex, &inputIndex);
                memcpy(inputBuffer, buffer, inputBufferSize);
                AMediaCodec_queueInputBuffer(decoder, inputBufferIndex, 0, inputBufferSize, 0, 0);
            } else {
                AMediaCodec_queueInputBuffer(decoder, inputBufferIndex, 0, 0, 0, AMEDIACODEC_BUFFER_FLAG_END_OF_STREAM);
                break;
            }
        }

        AMediaCodecBufferInfo info;
        ssize_t outputBufferIndex = AMediaCodec_dequeueOutputBuffer(decoder, &info, 10000);
        if (outputBufferIndex >= 0) {
            uint8_t *outputBuffer = AMediaCodec_getOutputBuffer(decoder, outputBufferIndex, &outputIndex);
            // Encode decoded frames
            ssize_t inputBufferIndex = AMediaCodec_dequeueInputBuffer(encoder, 10000);  // Timeout in microseconds
            if (inputBufferIndex >= 0) {
                uint8_t *inputBuffer = AMediaCodec_getInputBuffer(encoder, inputBufferIndex, &inputIndex);
                memcpy(inputBuffer, outputBuffer, info.size);
                AMediaCodec_queueInputBuffer(encoder, inputBufferIndex, 0, info.size, 0, 0);
            }

            AMediaCodec_releaseOutputBuffer(decoder, outputBufferIndex, false);
            if (info.flags & AMEDIACODEC_BUFFER_FLAG_END_OF_STREAM) {
                break;
            }
        } else if (outputBufferIndex == AMEDIACODEC_INFO_OUTPUT_FORMAT_CHANGED) {
            AMediaFormat *format = AMediaCodec_getOutputFormat(decoder);
            // Handle output format change if needed
        } else if (outputBufferIndex == AMEDIACODEC_INFO_TRY_AGAIN_LATER) {
            // Handle timeout
        }
    }

    delete[] buffer;

    // Clean up
    fclose(inputFile);
    fclose(outputFile);
    AMediaCodec_stop(encoder);
    AMediaCodec_delete(encoder);
    AMediaCodec_stop(decoder);
    AMediaCodec_delete(decoder);
    AMediaFormat_delete(format);
}

void decodeVideo(const char* inputPath, const char* outputPath) {
    // Function not implemented in this scenario, as decoding will be done in encodeVideo function.
}

} // extern "C"
