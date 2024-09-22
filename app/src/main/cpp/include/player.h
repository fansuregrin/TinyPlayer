#ifndef TINY_PLAYER_PLAYER_H
#define TINY_PLAYER_PLAYER_H

#include <thread>
#include <mutex>
#include <condition_variable>
#include "anw_render.h"
#include "aaudio_render.h"
#include "queue.hpp"
#include "log.h"

extern "C" {
#include "libavcodec/avcodec.h"
#include "libavformat/avformat.h"
#include "libswscale/swscale.h"
#include "libswresample/swresample.h"
#include "libavutil/imgutils.h"
#include "libavutil/time.h"
}

#define BUFF_SIZE 1024

class Player {
public:
    static Player *getInstance();

    void init(ANativeWindow *w);
    bool open(const std::string &filepath);
    void stop();
    void startPlay();
    void resume();
    void pause();
    int setSpeed(float speed);
    int seek(double position);
    double getDuration();
    double getPosition() const;
private:
    using lock_guard = std::lock_guard<std::mutex>;
    using unique_lock = std::unique_lock<std::mutex>;
private:
    Player();
    virtual ~Player();

    void addPacket();
    void decodeVideoPacket();
    void renderVideo();
    void decodeAudioPacket();
    void renderAudio();
    AVStream* getVideoStream();
    AVStream* getAudioStream();
    bool openVideoDecoder();
    bool openAudioDecoder();

    mutable std::mutex mtx;
    std::condition_variable worker;
    bool isInit;
    bool isOpen;
    bool closed;
    uint64_t startTime;
    float m_speed;
    AVFormatContext *pFormatCtx;
    AVCodec *pVideoCodec;
    AVCodec *pAudioCodec;
    AVCodecContext  *pVideoCodecCtx;
    AVCodecContext  *pAudioCodecCtx;
    int videoStreamId{};
    int audioStreamId{};
    double startPosition;
    double currPosition;
    ANWRender videoRender;
    AAudioRender audioRender;
    Queue<AVPacket *> videoPacketQ;
    Queue<AVPacket *> audioPacketQ;
    Queue<AVFrame *> videoFrameQ;
    uint8_t audioUserData[88200]{};
    std::thread demuxing;           // 解复用线程
    std::thread videoDecoding;      // 视频解码线程
    std::thread videoRendering;     // 视频渲染线程
    std::thread audioDecoding;      // 音频解码线程
    AAudioCallback aAudioCb{};
};

#endif //TINY_PLAYER_PLAYER_H
