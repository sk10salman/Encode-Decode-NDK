#include <jni.h>
#include <android/log.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <media/NdkMediaCodec.h>
#include <media/NdkMediaFormat.h>
#include <media/NdkMediaExtractor.h>
#include <media/NdkMediaMuxer.h>
#include <EGL/egl.h>
#include <GLES2/gl2.h>

extern "C" {

// Function prototypes
void decodeAndEncodeVideo(const char* inputPath, const char* outputPath);
int openOutputFile(const char* outputPath);
size_t getFileSize(const char* filePath);

JNIEXPORT void JNICALL
Java_com_example_mediaprocessing_MediaCodecHelper_nativeDecodeAndEncodeVideo(JNIEnv *env, jobject /* this */,
                                                                            jstring inputPath_,
                                                                            jstring outputPath_) {
    const char *inputPath = env->GetStringUTFChars(inputPath_, nullptr);
    const char *outputPath = env->GetStringUTFChars(outputPath_, nullptr);

    decodeAndEncodeVideo(inputPath, outputPath);

    env->ReleaseStringUTFChars(inputPath_, inputPath);
    env->ReleaseStringUTFChars(outputPath_, outputPath);
}

void decodeAndEncodeVideo(const char* inputPath, const char* outputPath) {
    AMediaExtractor *extractor = nullptr;
    AMediaCodec *decoder = nullptr;
    AMediaCodec *encoder = nullptr;
    EGLDisplay display = EGL_NO_DISPLAY;
    EGLSurface surface = EGL_NO_SURFACE;
    EGLContext context = EGL_NO_CONTEXT;

    // Initialize EGL
    display = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    EGLBoolean initialized = eglInitialize(display, NULL, NULL);
    if (initialized != EGL_TRUE) {
        __android_log_print(ANDROID_LOG_ERROR, "MediaCodec", "Failed to initialize EGL");
        return;
    }

    // Setup EGL context
    EGLConfig config;
    EGLint numConfigs;
    const EGLint attribs[] = {
        EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
        EGL_BLUE_SIZE, 8,
        EGL_GREEN_SIZE, 8,
        EGL_RED_SIZE, 8,
        EGL_SURFACE_TYPE, EGL_WINDOW_BIT,
        EGL_NONE
    };
    eglChooseConfig(display, attribs, &config, 1, &numConfigs);

    surface = eglCreateWindowSurface(display, config, nullptr, NULL);
    EGLint contextAttribs[] = { EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE };
    context = eglCreateContext(display, config, EGL_NO_CONTEXT, contextAttribs);
    eglMakeCurrent(display, surface, surface, context);

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

    AMediaFormat *encoderFormat = AMediaFormat_new();
    AMediaFormat_setString(encoderFormat, AMEDIAFORMAT_KEY_MIME, "video/avc");
    AMediaFormat_setInt32(encoderFormat, AMEDIAFORMAT_KEY_WIDTH, 1280);  // Specify your video width
    AMediaFormat_setInt32(encoderFormat, AMEDIAFORMAT_KEY_HEIGHT, 720);  // Specify your video height
    AMediaFormat_setInt32(encoderFormat, AMEDIAFORMAT_KEY_BIT_RATE, 2000000);  // Specify your video bit rate
    AMediaFormat_setInt32(encoderFormat, AMEDIAFORMAT_KEY_FRAME_RATE, 30);  // Specify your video frame rate

    AMediaCodec_configure(encoder, encoderFormat, nullptr, nullptr, AMEDIACODEC_CONFIGURE_FLAG_ENCODE);
    AMediaCodec_start(encoder);

    // Buffer info for MediaCodec operations
    AMediaCodecBufferInfo decoderBufferInfo;
    AMediaCodecBufferInfo encoderBufferInfo;
    bool sawInputEOS = false;
    bool sawOutputEOS = false;

    // Setup OpenGL ES rendering
    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);

    while (!sawOutputEOS) {
        // Decode input frames
        ssize_t decoderInputBufferIndex = AMediaCodec_dequeueInputBuffer(decoder, 10000);  // Timeout in microseconds
        if (decoderInputBufferIndex >= 0) {
            size_t bufferSize;
            uint8_t *decoderInputBuffer = AMediaCodec_getInputBuffer(decoder, decoderInputBufferIndex, &bufferSize);
            ssize_t sampleSize = AMediaExtractor_readSampleData(extractor, decoderInputBuffer, bufferSize);
            if (sampleSize < 0) {
                sampleSize = 0;
                sawInputEOS = true;
            }
            int64_t presentationTimeUs = AMediaExtractor_getSampleTime(extractor);
            uint32_t flags = AMediaExtractor_getSampleFlags(extractor);
            AMediaCodec_queueInputBuffer(decoder, decoderInputBufferIndex, 0, sampleSize, presentationTimeUs, flags);
            AMediaExtractor_advance(extractor);
        }

        // Decode output frames
        ssize_t decoderOutputBufferIndex = AMediaCodec_dequeueOutputBuffer(decoder, &decoderBufferInfo, 10000);
        if (decoderOutputBufferIndex >= 0) {
            // Render the decoded frame onto the OpenGL ES surface
            glClear(GL_COLOR_BUFFER_BIT);
            // Sample rendering of a red triangle (replace with actual rendering logic)
            GLfloat vertices[] = {
                -0.5f, -0.5f, 0.0f,
                 0.5f, -0.5f, 0.0f,
                 0.0f,  0.5f, 0.0f
            };
            // Compile shader, link program, etc. (OpenGL ES initialization steps)
            // Draw frame logic
            glDrawArrays(GL_TRIANGLES, 0, 3);
            eglSwapBuffers(display, surface);

            AMediaCodec_releaseOutputBuffer(decoder, decoderOutputBufferIndex, false);
            if ((decoderBufferInfo.flags & AMEDIACODEC_BUFFER_FLAG_END_OF_STREAM) != 0) {
                sawOutputEOS = true;
            }
        } else if (decoderOutputBufferIndex == AMEDIACODEC_INFO_OUTPUT_FORMAT_CHANGED) {
            AMediaFormat *newFormat = AMediaCodec_getOutputFormat(decoder);
            // Do something with the new format (optional)
        }

        // Encode frames from OpenGL ES surface
        ssize_t encoderInputBufferIndex = AMediaCodec_dequeueInputBuffer(encoder, 10000);
        if (encoderInputBufferIndex >= 0) {
            // Read from OpenGL ES surface and encode here (not implemented in this snippet)
            AMediaCodec_queueInputBuffer(encoder, encoderInputBufferIndex, 0, /* size */, /* presentationTimeUs */, /* flags */);
        }

        // Encode output frames
        ssize_t encoderOutputBufferIndex = AMediaCodec_dequeueOutputBuffer(encoder, &encoderBufferInfo, 10000);
        if (encoderOutputBufferIndex >= 0) {
            uint8_t *encoderOutputBuffer = AMediaCodec_getOutputBuffer(encoder, encoderOutputBufferIndex, nullptr);
            AMediaCodec_releaseOutputBuffer(encoder, encoderOutputBufferIndex, false);
            if ((encoderBufferInfo.flags & AMEDIACODEC_BUFFER_FLAG_END_OF_STREAM) != 0) {
                sawOutputEOS = true;
            }
        } else if (encoderOutputBufferIndex == AMEDIACODEC_INFO_OUTPUT_FORMAT_CHANGED) {
            AMediaFormat *newFormat = AMediaCodec_getOutputFormat(encoder);
            // Do something with the new format (optional)
        }
    }

    // Clean up
    eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    eglDestroySurface(display, surface);
    eglDestroyContext(display, context);
    eglTerminate(display);

    close(inputFd);
    AMediaCodec_stop(decoder);
    AMediaCodec_delete(decoder);
    AMediaCodec_stop(encoder);
    AMediaCodec_delete(encoder);
    AMediaExtractor_delete(extractor);
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
