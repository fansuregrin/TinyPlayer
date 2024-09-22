#include <cstring>
#include "anw_render.h"
#include "log.h"

ANWRender::ANWRender(): native_window(nullptr), width(0), height(0) {}

void ANWRender::init(ANativeWindow *window) {
    native_window = window;
}

int ANWRender::setBuffers(int videoWidth, int videoHeight) {
    width = videoWidth;
    height = videoHeight;
    if (native_window == nullptr) return -1;
    return ANativeWindow_setBuffersGeometry(native_window, videoWidth,
        videoHeight, WINDOW_FORMAT_RGBA_8888);
}

int ANWRender::render(uint8_t* rgba) {
    if (native_window == nullptr || rgba == nullptr)
        return -1;

    ANativeWindow_Buffer out_buffer;
    ANativeWindow_lock(native_window, &out_buffer, nullptr);
    int srcLineSize = width * 4;
    int dstLineSize = out_buffer.stride * 4;
    auto* dstBuffer = static_cast<uint8_t *>(out_buffer.bits);
    for (int i = 0; i < height; ++i) {
        memcpy(dstBuffer + i * dstLineSize, rgba + i * srcLineSize, srcLineSize);
    }
    ANativeWindow_unlockAndPost(native_window);

    return 0;
}
