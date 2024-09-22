#include <jni.h>
#include "player.h"

extern "C" {
JNIEXPORT jint JNICALL
Java_com_example_tinyplayer_Player_nativePlay(
    JNIEnv *env, jobject thiz,
    jstring file, jobject surface
) {
    const char *filepath = env->GetStringUTFChars(file, nullptr);
    Player::getInstance()->init(ANativeWindow_fromSurface(env, surface));

    if (!Player::getInstance()->open(filepath)) {
        return -1;
    }

    Player::getInstance()->startPlay();

    return 0;
}

JNIEXPORT void JNICALL
Java_com_example_tinyplayer_Player_nativePause(
    JNIEnv *env, jobject thiz,
    jboolean p
) {
    if (p) {
        Player::getInstance()->pause();
    } else {
        Player::getInstance()->resume();
    }
}

JNIEXPORT jint JNICALL
Java_com_example_tinyplayer_Player_nativeSeek(
    JNIEnv *env, jobject thiz,
    jdouble position
) {
    return Player::getInstance()->seek(position);
}

JNIEXPORT void JNICALL
Java_com_example_tinyplayer_Player_nativeStop(
    JNIEnv *env, jobject thiz
) {
    Player::getInstance()->stop();
}

JNIEXPORT jint JNICALL
Java_com_example_tinyplayer_Player_nativeSetSpeed(
JNIEnv *env, jobject thiz, jfloat speed) {
    return Player::getInstance()->setSpeed(speed);
}

JNIEXPORT jdouble JNICALL
Java_com_example_tinyplayer_Player_nativeGetPosition(JNIEnv *env, jobject thiz) {
    return Player::getInstance()->getPosition();
}

JNIEXPORT jdouble JNICALL
Java_com_example_tinyplayer_Player_nativeGetDuration(JNIEnv *env, jobject thiz) {
    return Player::getInstance()->getDuration();
}

}