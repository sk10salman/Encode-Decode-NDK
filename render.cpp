// Include necessary headers
#include <jni.h>
#include <android/log.h>
#include <media/NdkMediaCodec.h>
#include <GLES3/gl3.h>

// Define logging tag
#define LOG_TAG "VideoPlayer"

// Define logging macros
#define LOGI(...) ((void)__android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__))
#define LOGE(...) ((void)__android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__))

// Function to dump frame to file
void dumpFrame(uint8_t* frameData, int width, int height, int frameNumber) {
    // Construct file name
    char filename[256];
    snprintf(filename, sizeof(filename), "/sdcard/frame_%d.raw", frameNumber);

    // Open file
    FILE* file = fopen(filename, "wb");
    if (!file) {
        LOGE("Failed to open file %s", filename);
        return;
    }

    // Write frame data to file
    fwrite(frameData, sizeof(uint8_t), width * height * 4, file);

    // Close file
    fclose(file);

    LOGI("Frame %d dumped to %s", frameNumber, filename);
}

// Function to render OpenGL frame
void renderFrame(uint8_t* frameData, int width, int height) {
    // Clear the color buffer
    glClear(GL_COLOR_BUFFER_BIT);

    // Set viewport dimensions
    glViewport(0, 0, width, height);

    // Sample vertex and fragment shader for rendering a textured quad
    const char* vertexShaderCode =
        "#version 300 es\n"
        "layout(location = 0) in vec4 a_position;\n"
        "layout(location = 1) in vec2 a_texCoord;\n"
        "out vec2 v_texCoord;\n"
        "void main() {\n"
        "    gl_Position = a_position;\n"
        "    v_texCoord = a_texCoord;\n"
        "}\n";

    const char* fragmentShaderCode =
        "#version 300 es\n"
        "precision mediump float;\n"
        "in vec2 v_texCoord;\n"
        "out vec4 fragColor;\n"
        "uniform sampler2D u_texture;\n"
        "void main() {\n"
        "    fragColor = texture(u_texture, v_texCoord);\n"
        "}\n";

    // Compile shaders
    GLuint vertexShader = glCreateShader(GL_VERTEX_SHADER);
    glShaderSource(vertexShader, 1, &vertexShaderCode, nullptr);
    glCompileShader(vertexShader);

    GLuint fragmentShader = glCreateShader(GL_FRAGMENT_SHADER);
    glShaderSource(fragmentShader, 1, &fragmentShaderCode, nullptr);
    glCompileShader(fragmentShader);

    // Create shader program
    GLuint shaderProgram = glCreateProgram();
    glAttachShader(shaderProgram, vertexShader);
    glAttachShader(shaderProgram, fragmentShader);
    glLinkProgram(shaderProgram);

    // Delete shaders (no longer needed after linking)
    glDeleteShader(vertexShader);
    glDeleteShader(fragmentShader);

    // Set up vertex data and attributes for a textured quad
    GLfloat vertices[] = {
        -1.0f, -1.0f, 0.0f, 1.0f,  // Position 0
         0.0f,  1.0f,             // TexCoord 0
         1.0f, -1.0f, 0.0f, 1.0f,  // Position 1
         1.0f,  1.0f,             // TexCoord 1
        -1.0f,  1.0f, 0.0f, 1.0f,  // Position 2
         0.0f,  0.0f,             // TexCoord 2
         1.0f,  1.0f, 0.0f, 1.0f,  // Position 3
         1.0f,  0.0f              // TexCoord 3
    };

    // Create VBO and VAO
    GLuint vbo, vao;
    glGenBuffers(1, &vbo);
    glBindBuffer(GL_ARRAY_BUFFER, vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(vertices), vertices, GL_STATIC_DRAW);

    glGenVertexArrays(1, &vao);
    glBindVertexArray(vao);

    // Position attribute
    glVertexAttribPointer(0, 4, GL_FLOAT, GL_FALSE, 6 * sizeof(GLfloat), (void*)0);
    glEnableVertexAttribArray(0);

    // TexCoord attribute
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 6 * sizeof(GLfloat), (void*)(4 * sizeof(GLfloat)));
    glEnableVertexAttribArray(1);

    // Set active texture unit
    glActiveTexture(GL_TEXTURE0);

    // Generate texture
    GLuint textureId;
    glGenTextures(1, &textureId);
    glBindTexture(GL_TEXTURE_2D, textureId);

    // Set texture parameters
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);

    // Load frame data into texture
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, frameData);

    // Bind shader program and set uniform
    glUseProgram(shaderProgram);
    GLint uTextureLoc = glGetUniformLocation(shaderProgram, "u_texture");
    glUniform1i(uTextureLoc, 0); // Texture unit 0

    // Draw the quad
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);

    // Cleanup
    glBindTexture(GL_TEXTURE_2D, 0);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    glBindVertexArray(0);
    glUseProgram(0);

    glDeleteTextures(1, &textureId);
    glDeleteProgram(shaderProgram);
    glDeleteBuffers(1, &vbo);
    glDeleteVertexArrays(1, &vao);

    // Dump frame to file
    static int frameCount = 0;
    dumpFrame(frameData, width, height, frameCount++);
}

// JNI function to decode video, render frames, and dump each frame
extern "C" JNIEXPORT void JNICALL
Java_your_package_name_VideoRendererActivity_renderAndDumpFrames(JNIEnv *env, jobject instance, jstring videoPath) {
    // Convert Java string to C string
    const char *path = env->GetStringUTFChars(videoPath, nullptr);

    // Initialize MediaExtractor to get video format
    AMediaExtractor* extractor = AMediaExtractor_new();
    AMediaCodec* codec = nullptr;

    // Set data source for MediaExtractor
    AMediaExtractor_setDataSource(extractor, path);

    // Find and select a track (assume first video track)
    int trackCount = AMediaExtractor_getTrackCount(extractor);
    for (int i = 0; i < trackCount; ++i) {
        AMediaFormat* format = AMediaExtractor_getTrackFormat(extractor, i);
        const char *mime;
        AMediaFormat_getString(format, AMEDIAFORMAT_KEY_MIME, &mime);
        if (strncmp(mime, "video/", 6) == 0) {
            AMediaExtractor_selectTrack(extractor, i);
            codec = AMediaCodec_createDecoderByType(mime);
            break;
        }
        AMediaFormat_delete(format);
    }

    if (codec == nullptr) {
        LOGE("Failed to create MediaCodec");
        return;
    }

    // Start codec
    AMediaCodec_start(codec);

    // Main loop to decode and render frames
    AMediaCodecBufferInfo info;
    bool sawInputEOS = false;
    bool sawOutputEOS = false;

    while (!sawOutputEOS) {
        if (!sawInputEOS) {
            ssize_t bufIdx = AMediaCodec_dequeueInputBuffer(codec, -1);
            if (bufIdx >= 0) {
                size_t bufsize;
                uint8_t *buf = AMediaCodec_getInputBuffer(codec, bufIdx, &bufsize);
                ssize_t sampleSize = AMediaExtractor_readSampleData(extractor, buf, bufsize);
                if (sampleSize < 0) {
                    sampleSize = 0;
                    sawInputEOS = true;
                }
                int64_t presentationTimeUs = AMediaExtractor_getSampleTime(extractor);
                AMediaCodec_queueInputBuffer(codec, bufIdx, 0, sampleSize, presentationTimeUs,
                                             sawInputEOS ? AMEDIACODEC_BUFFER_FLAG_END_OF_STREAM : 0);
                if (!sawInputEOS) {
                    AMediaExtractor_advance(extractor);
                }
            }
        }

        ssize_t outIdx = AMediaCodec_dequeueOutputBuffer(codec, &info, 0);
        if (outIdx >= 0) {
            size_t out_size;
            uint8_t *out_data = AMediaCodec_getOutputBuffer(codec, outIdx, &out_size);
            if (out_size > 0) {
                // Render the frame and dump it
                renderFrame(out_data, info.width, info.height);
            }
            AMediaCodec_releaseOutputBuffer(codec, outIdx, false);
            if ((info.flags & AMEDIACODEC_BUFFER_FLAG_END_OF_STREAM) != 0) {
                sawOutputEOS = true;
            }
        } else if (outIdx == AMEDIACODEC_INFO_OUTPUT_FORMAT_CHANGED) {
            AMediaFormat* newFormat = AMediaCodec_getOutputFormat(codec);
            // Update width and height from new format
            int width, height;
            AMediaFormat_getInt32(newFormat, AMEDIAFORMAT_KEY_WIDTH, &width);
            AMediaFormat_getInt32(newFormat, AMEDIAFORMAT_KEY_HEIGHT, &height);
            // Handle format change (if needed)
            AMediaFormat_delete(newFormat);
        }
    }

    // Release resources
    AMediaCodec_stop(codec);
    AMediaCodec_delete(codec);
    AMediaExtractor_delete(extractor);

    // Release Java string
    env->ReleaseStringUTFChars(videoPath, path);
}
