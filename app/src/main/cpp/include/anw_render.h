#ifndef TINY_PLAYER_ANW_RENDER_H
#define TINY_PLAYER_ANW_RENDER_H

#include <cstdint>
#include <android/native_window.h>
#include <android/native_window_jni.h>

class ANWRender{
public:
    ANWRender();
    void init(ANativeWindow *window);
    int setBuffers(int videoWidth, int videoHeight);
    int render(uint8_t* rgba);

private:
    ANativeWindow *native_window;
    int width;
    int height;
};

#endif //TINY_PLAYER_ANW_RENDER_H