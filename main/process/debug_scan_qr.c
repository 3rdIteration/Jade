#include "../camera.h"
#include "../jade_assert.h"
#include "../process.h"
#include "../qrscan.h"
#include "../utils/malloc_ext.h"
#include "process_utils.h"

#include <rom/miniz.h>

#ifdef CONFIG_DEBUG_MODE

static const size_t CBOR_OVERHEAD = 64;

typedef struct {
    jade_process_t* process;
    bool check_qr; // check captured image is a valid qr code
} image_capture_into_t;

static size_t compress(const uint8_t* data, size_t data_len, uint8_t* output, size_t output_len)
{
    JADE_ASSERT(data);
    JADE_ASSERT(data_len);
    JADE_ASSERT(output);
    JADE_ASSERT(output_len);

    tdefl_compressor* const ctx_tdefl = JADE_MALLOC_PREFER_SPIRAM(sizeof(tdefl_compressor));
    tdefl_status status = tdefl_init(ctx_tdefl, NULL, NULL, TDEFL_DEFAULT_MAX_PROBES);
    JADE_ASSERT(status == TDEFL_STATUS_OKAY);

    status = tdefl_compress(ctx_tdefl, data, &data_len, output, &output_len, TDEFL_FINISH);
    free(ctx_tdefl);

    return status == TDEFL_STATUS_DONE ? output_len : 0;
}

static size_t decompress(const uint8_t* data, size_t data_len, uint8_t* output, size_t output_len)
{
    JADE_ASSERT(data);
    JADE_ASSERT(data_len);
    JADE_ASSERT(output);
    JADE_ASSERT(output_len);

    tinfl_decompressor* const ctx_tinfl = JADE_MALLOC_PREFER_SPIRAM(sizeof(tinfl_decompressor));
    tinfl_init(ctx_tinfl);

    const tinfl_status status = tinfl_decompress(
        ctx_tinfl, data, &data_len, output, output, &output_len, TINFL_FLAG_USING_NON_WRAPPING_OUTPUT_BUF);
    free(ctx_tinfl);

    return status == TINFL_STATUS_DONE ? output_len : 0;
}

// Camera callback function for scaling, compressing and returning image data
static bool return_image_data(const size_t width, const size_t height, const uint8_t* data, const size_t len, void* ctx)
{
    JADE_ASSERT(data);
    JADE_ASSERT(len);
    JADE_ASSERT(ctx);

    image_capture_into_t* info = (image_capture_into_t*)ctx;

    ASSERT_CURRENT_MESSAGE(info->process, "debug_capture_image_data");
    bool ret = false;

    // If wanting an image of a qr code, only accept frame if scans ok
    if (info->check_qr) {
        qr_data_t qr_data = { .len = 0 };
        if (!scan_qr(width, height, data, len, &qr_data)) {
            JADE_LOGW("Ignoring as failed to scan a valid qr code from camera image");
            return false;
        }
    }

    // Compress image data
    const size_t compressed_buflen = len;
    uint8_t* const compressed = JADE_MALLOC_PREFER_SPIRAM(compressed_buflen);
    const size_t compressed_len = compress(data, len, compressed, compressed_buflen);
    if (!compressed_len) {
        JADE_LOGW("Compressing image data failed");
        goto cleanup;
    }

    // Decompress and verify is same
    const size_t decompressed_buflen = len;
    uint8_t* const decompressed = JADE_MALLOC_PREFER_SPIRAM(decompressed_buflen);
    const size_t decompressed_len = decompress(compressed, compressed_len, decompressed, decompressed_buflen);
    ret = (decompressed_len == len) && !memcmp(data, decompressed, len);
    free(decompressed);

    if (!ret) {
        JADE_LOGW("Decompressing / verification failed");
        goto cleanup;
    }

    // All good, reply with the compressed image data
    const size_t buflen = compressed_len + CBOR_OVERHEAD;
    uint8_t* buffer = JADE_MALLOC_PREFER_SPIRAM(buflen);
    jade_process_reply_to_message_bytes(info->process->ctx, compressed, compressed_len, buffer, buflen);
    free(buffer);

    // Free the input message (to signal that we have been called and sent the reply)
    jade_process_free_current_message(info->process);
    JADE_LOGI("Success");

cleanup:
    free(compressed);
    return ret;
}

void debug_capture_image_data_process(void* process_ptr)
{
    JADE_LOGI("Starting: %u", xPortGetFreeHeapSize());
    jade_process_t* process = process_ptr;

    // We expect a current message to be present
    ASSERT_CURRENT_MESSAGE(process, "debug_capture_image_data");
    GET_MSG_PARAMS(process);

    // Caller may want to restrict to images which contain a valid qr code
    bool check_qr = false;
    const bool ret = rpc_get_boolean("check_qr", &params, &check_qr);

    // Launch the camera with the 'click' callback function set to
    // return the captured image data in the reply message
    image_capture_into_t info = { .process = process, .check_qr = ret && check_qr };
    jade_camera_process_images(return_image_data, &info, "Image Capture", "Point and\n    Click!", "Capture");

    // Send a 'user cancelled' error reply if the callback was not invoked
    // (We can detect as the callback frees the 'current message' on successful completion)
    if (HAS_CURRENT_MESSAGE(process)) {
        // The camera callback was not called - ie. camera screen was 'Exit'-ed.
        jade_process_reject_message(process, CBOR_RPC_USER_CANCELLED, "User declined to capture image", NULL);
    }

cleanup:
    return;
}
#endif // CONFIG_DEBUG_MODE