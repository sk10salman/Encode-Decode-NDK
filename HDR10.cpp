#include <jni.h>
#include <android/native_activity.h>
#include <android/log.h>
#include <media/NdkMediaCodec.h>
#include <media/NdkMediaFormat.h>
#include <fcntl.h>
#include <unistd.h>

// Function to extract HDR10+ metadata from HEVC video stream
bool extractHDR10PlusMetadata(const char* videoPath, uint8_t* metadataBuffer, size_t bufferSize) {
    // Open video file
    int fd = open(videoPath, O_RDONLY);
    if (fd < 0) {
        __android_log_print(ANDROID_LOG_ERROR, "HDR10+ Decoder", "Failed to open video file: %s", videoPath);
        return false;
    }

    // Seek to the position where HDR10+ metadata is located in the video stream
    // Example: Use a HEVC parser to locate and extract the metadata
    // For simplicity, assume metadata is directly read into metadataBuffer
    ssize_t bytesRead = read(fd, metadataBuffer, bufferSize);

    close(fd);
    
    if (bytesRead <= 0) {
        __android_log_print(ANDROID_LOG_ERROR, "HDR10+ Decoder", "Failed to read HDR10+ metadata from video file");
        return false;
    }

    return true;
}

void decodeHDR10PlusVideo(const char* videoPath) {
    AMediaCodec* codec = nullptr;
    AMediaFormat* format = nullptr;
    int fd = -1;

    // Open video file
    fd = open(videoPath, O_RDONLY);
    if (fd < 0) {
        // Handle file open error
        __android_log_print(ANDROID_LOG_ERROR, "HDR10+ Decoder", "Failed to open video file: %s", videoPath);
        return;
    }

    // Initialize MediaCodec
    codec = AMediaCodec_createDecoderByType("video/hevc"); // Use correct MIME type for HDR10+
    if (!codec) {
        __android_log_print(ANDROID_LOG_ERROR, "HDR10+ Decoder", "Failed to create MediaCodec");
        close(fd);
        return;
    }

    format = AMediaFormat_new();
    AMediaFormat_setString(format, AMEDIAFORMAT_KEY_MIME, "video/hevc");
    // Set other format parameters as needed
    AMediaFormat_setInt32(format, AMEDIAFORMAT_KEY_WIDTH, 1920);
    AMediaFormat_setInt32(format, AMEDIAFORMAT_KEY_HEIGHT, 1080);
    AMediaFormat_setInt32(format, AMEDIAFORMAT_KEY_COLOR_FORMAT, OMX_COLOR_FormatYUV420Flexible);

    // Extract HDR10+ metadata
    const size_t metadataBufferSize = 1024; // Adjust size as per your metadata requirements
    uint8_t hdr10PlusMetadata[metadataBufferSize];
    bool metadataExtracted = extractHDR10PlusMetadata(videoPath, hdr10PlusMetadata, metadataBufferSize);
    if (metadataExtracted) {
        AMediaFormat_setBuffer(format, AMEDIAFORMAT_KEY_HDR10_PLUS_INFO, hdr10PlusMetadata, metadataBufferSize);
    } else {
        __android_log_print(ANDROID_LOG_ERROR, "HDR10+ Decoder", "Failed to extract HDR10+ metadata");
        AMediaCodec_delete(codec);
        AMediaFormat_delete(format);
        close(fd);
        return;
    }

    if (AMediaCodec_configure(codec, format, nullptr, nullptr, 0) != AMEDIA_OK) {
        __android_log_print(ANDROID_LOG_ERROR, "HDR10+ Decoder", "Failed to configure MediaCodec");
        close(fd);
        AMediaCodec_delete(codec);
        AMediaFormat_delete(format);
        return;
    }

    if (AMediaCodec_start(codec) != AMEDIA_OK) {
        __android_log_print(ANDROID_LOG_ERROR, "HDR10+ Decoder", "Failed to start MediaCodec");
        close(fd);
        AMediaCodec_stop(codec);
        AMediaCodec_delete(codec);
        AMediaFormat_delete(format);
        return;
    }

    // Decode frames
    ssize_t bufIdx = AMediaCodec_dequeueInputBuffer(codec, -1);
    if (bufIdx >= 0) {
        size_t bufsize;
        uint8_t* buf = AMediaCodec_getInputBuffer(codec, bufIdx, &bufsize);
        ssize_t nread = read(fd, buf, bufsize);
        if (nread > 0) {
            AMediaCodec_queueInputBuffer(codec, bufIdx, 0, nread, 0);
        }
    }

    AMediaCodecBufferInfo info;
    ssize_t status = AMediaCodec_dequeueOutputBuffer(codec, &info, -1);
    if (status >= 0) {
        uint8_t* outputData = AMediaCodec_getOutputBuffer(codec, status, nullptr);
        // Process or dump outputData (e.g., save as image file)
        // Example: saveFrameToFile(outputData, info.size);
        AMediaCodec_releaseOutputBuffer(codec, status, false);
    }

    // Clean up
    AMediaCodec_stop(codec);
    AMediaCodec_delete(codec);
    AMediaFormat_delete(format);
    close(fd);
}
