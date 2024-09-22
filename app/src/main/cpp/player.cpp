#include "player.h"

Player * Player::getInstance() {
    static Player player;
    return &player;
}

void Player::init(ANativeWindow *w) {
    lock_guard lck(mtx);
    if (isInit) return;
    videoRender.init(w);
    audioRender.setCallback([] (AAudioStreamStruct *stream, void *userData,
        void *audioData, int32_t numFrames) -> int {
        aaudio_result_t ret;
        memcpy(audioData, userData, sizeof(userData)*numFrames);
        return 0;
    }, audioUserData);
    isInit = true;
}

void Player::startPlay() {
    audioPacketQ.resume();
    videoPacketQ.resume();
    videoFrameQ.resume();
    audioRender.start();
    audioRender.flush();
    {
        lock_guard lck(mtx);
        startTime = av_gettime(); // in microseconds
        startPosition = currPosition = 0.0;  // in seconds
        m_speed = 1.0;
    }
}

void Player::resume() {
    audioPacketQ.resume();
    videoPacketQ.resume();
    videoFrameQ.resume();
    audioRender.pause(false);
    {
        lock_guard lck(mtx);
        startTime = av_gettime();
        startPosition = currPosition;
    }
}

void Player::pause() {
    audioPacketQ.pause();
    audioRender.pause(true);
    videoFrameQ.pause();
    videoPacketQ.pause();
}

int Player::seek(double position) {
    unique_lock lck(mtx);
    if (!isOpen) return -1;
    audioRender.flush();
    int ret;
    position = position * static_cast<double>(pFormatCtx->duration) / AV_TIME_BASE;
    uint64_t ts = position * av_q2d(av_inv_q(pFormatCtx->streams[videoStreamId]->time_base));
    ret = av_seek_frame(pFormatCtx, videoStreamId, ts, AVSEEK_FLAG_ANY);
    if (ret >= 0) {
        startTime = av_gettime();
        startPosition = currPosition = position;
    }
    return ret;
}

bool Player::open(const std::string &filepath) {
    unique_lock lck(mtx);
    if (isOpen) return true;
    // 打开封装格式
    int ret = avformat_open_input(&pFormatCtx, filepath.c_str(),
                                  nullptr, nullptr);
    char errBuf[BUFF_SIZE]{};
    if (ret < 0) {
        av_strerror(ret, errBuf, sizeof(errBuf) - 1);
        LOGE(LOGTAG, "打开 %s 失败, ffmpeg avformat_open_input error: %s", filepath.c_str(), errBuf);
        return false;
    }

    LOGD(LOGTAG, "打开 %s 成功", filepath.c_str());
    ret = avformat_find_stream_info(pFormatCtx, nullptr);
    if (ret < 0) {
        av_strerror(ret, errBuf, sizeof(errBuf) - 1);
        LOGE(LOGTAG, "获取流信息失败, ffmpeg avformat_find_stream_info error: %s", errBuf);
        avformat_free_context(pFormatCtx);
        return false;
    }

    LOGD(LOGTAG, "Format %s, duration %ld us", pFormatCtx->iformat->long_name,
         pFormatCtx->duration);

    if (!openVideoDecoder()) return false;
    if (!openAudioDecoder()) return false;

    isOpen = true;
    lck.unlock();
    worker.notify_all();
    return true;
}

void Player::stop() {
    unique_lock lck(mtx);
    isOpen = false;
    isInit = false;
    startPosition = currPosition = 0;
    audioRender.flush();
    audioRender.pause(true);
    avformat_close_input(&pFormatCtx);
    avcodec_close(pVideoCodecCtx);
    avcodec_close(pAudioCodecCtx);
    lck.unlock();
    videoPacketQ.clear();
    videoFrameQ.clear();
    audioPacketQ.clear();
    worker.notify_all();
}

int Player::setSpeed(float speed) {
    lock_guard lck(mtx);
    if (!isOpen) return -1;
    m_speed = speed;
    return 0;
}

Player::Player():
videoPacketQ(5), audioPacketQ(5), videoFrameQ(5) {
    isInit = false;
    isOpen = false;
    closed = false;
    pFormatCtx = nullptr;
    pVideoCodec = nullptr;
    pAudioCodec = nullptr;
    pVideoCodecCtx = nullptr;
    pAudioCodecCtx = nullptr;
    startTime = 0;
    startPosition = 0.0;
    currPosition = 0.0;
    m_speed = 1;
    demuxing = std::thread([this] { addPacket(); });
    videoDecoding = std::thread([this] { decodeVideoPacket(); });
    videoRendering = std::thread([this] { renderVideo(); });
    audioDecoding = std::thread([this] { decodeAudioPacket(); });
}

Player::~Player() {
    unique_lock lck(mtx);
    closed = true;
    lck.unlock();
    worker.notify_all();
    demuxing.join();
    videoDecoding.join();
    videoRendering.join();
    audioDecoding.join();
    avformat_close_input(&pFormatCtx);
    avcodec_close(pVideoCodecCtx);
    avcodec_close(pAudioCodecCtx);
}

void Player::addPacket() {
    char errBuf[BUFF_SIZE]{};
    while (true) {
        unique_lock lck(mtx);
        if (closed) break;
        worker.wait(lck, [this]{ return isOpen; });
        auto pFormatCtx_ = pFormatCtx;
        auto videoStreamId_ = videoStreamId;
        lck.unlock();

        AVPacket *pkt = av_packet_alloc();
        int ret = av_read_frame(pFormatCtx_, pkt);
        if (ret < 0) {
            av_strerror(ret, errBuf, sizeof(errBuf)-1);
            LOGE(LOGTAG, "ffmpeg av_read_frame error: %s", errBuf);
            if (AVERROR_EOF == ret) {
                lck.lock();
                isOpen = false;
                lck.unlock();
            }
        }

        if (pkt->stream_index == videoStreamId_) {
            videoPacketQ.push(pkt);
            LOGD(LOGTAG, "添加一个 raw packet 到 videoPacketQ: dts=%ld, pts=%ld, duration=%ld",
                 pkt->dts, pkt->pts, pkt->duration);
        } else if (pkt->stream_index == audioStreamId) {
             audioPacketQ.push(pkt);
             LOGD(LOGTAG, "添加一个 raw packet 到 audioPacketQ: dts=%ld, pts = %ld, duration=%ld",
                  pkt->dts, pkt->pts, pkt->duration);
        }
    }
}

void Player::decodeVideoPacket() {
    char errBuf[BUFF_SIZE]{};
    while (true) {
        unique_lock lck(mtx);
        if (closed) break;
        worker.wait(lck, [this]{ return isOpen; });
        auto pVideoCodecCtx_ = pVideoCodecCtx;
        lck.unlock();

        AVPacket *pkt = nullptr;
        videoPacketQ.pop(pkt);
        if (pkt == nullptr) continue;
        LOGD(LOGTAG, "从 videoPacketQ 获取到一个 raw package: dts=%ld, pts=%ld, duration=%ld",
             pkt->dts, pkt->pts, pkt->duration);
        int ret = avcodec_send_packet(pVideoCodecCtx_, pkt);
        av_packet_free(&pkt);
        if (ret < 0) {
            av_strerror(ret, errBuf, sizeof(errBuf)-1);
            LOGE(LOGTAG, "ffmpeg avcodec_send_packet error: %s", errBuf);
            if (AVERROR_EOF == ret) {
                lck.lock();
                isOpen = false;
                lck.unlock();
            }
        }

        AVFrame *frame = av_frame_alloc();
        ret = avcodec_receive_frame(pVideoCodecCtx_, frame);
        if (ret == 0) {
            videoFrameQ.push(frame);
            LOGD(LOGTAG, "添加一个 video frame 到 videoFrameQ: pts=%ld, width=%d, height=%d",
                frame->pts, frame->width, frame->height);
        } else {
            av_strerror(ret, errBuf, sizeof(errBuf)-1);
            LOGE(LOGTAG, "ffmpeg avcodec_receive_frame error: %s", errBuf);
            if (AVERROR_EOF == ret) {
                lck.lock();
                isOpen = false;
                lck.unlock();
            }
        }
    }
}

void Player::decodeAudioPacket() {
    char errBuf[BUFF_SIZE]{};
    while (true) {
        unique_lock lck(mtx);
        worker.wait(lck, [this]{ return isOpen; });
        auto pAudioCodecCtx_ = pAudioCodecCtx;
        lck.unlock();

        AVPacket *pkt = nullptr;
        audioPacketQ.pop(pkt);
        if (pkt == nullptr) continue;
        LOGD(LOGTAG, "从 audioPacketQ 获取到一个 raw package: dts=%ld, pts=%ld, duration=%ld",
             pkt->dts, pkt->pts, pkt->duration);
        int ret = avcodec_send_packet(pAudioCodecCtx_, pkt);
        av_packet_free(&pkt);
        if (ret < 0) {
            av_strerror(ret, errBuf, sizeof(errBuf)-1);
            LOGE(LOGTAG, "ffmpeg avcodec_send_packet error: %s", errBuf);
            if (AVERROR_EOF == ret) {
                lck.lock();
                isOpen = false;
                lck.unlock();
            }
        }

        AVFrame *frame = av_frame_alloc();
        ret = avcodec_receive_frame(pAudioCodecCtx_, frame);
        if (ret == 0) {
            LOGD(LOGTAG, "audio frame format: %d", frame->format);

            // 设置音频重采样上下文
            AVSampleFormat inSampleFmt = pAudioCodecCtx_->sample_fmt;
            uint64_t inChannelLayout = pAudioCodecCtx_->channel_layout;
            int inSampleRate = pAudioCodecCtx_->sample_rate;
            AVSampleFormat outSampleFmt = AV_SAMPLE_FMT_S16;
            uint64_t outChannelLayout = AV_CH_LAYOUT_STEREO;
            int outSampleRate = inSampleRate;
            SwrContext *swrCtx = swr_alloc();
            swr_alloc_set_opts(swrCtx, outChannelLayout, outSampleFmt, outSampleRate,
                               inChannelLayout, inSampleFmt, inSampleRate, 0, nullptr);
            swr_init(swrCtx);

            // 音频缓存
            int nbChannels = av_get_channel_layout_nb_channels(outChannelLayout);
            int pcmBufSize = nbChannels * inSampleRate;
            uint8_t *pcmBuf = static_cast<uint8_t *>(av_malloc(pcmBufSize));

            swr_convert(swrCtx, &pcmBuf, pcmBufSize,
                (const uint8_t* *)frame->data, frame->nb_samples);
            lck.lock();
            memcpy(audioUserData, pcmBuf, pcmBufSize);

            // 根据每一帧的 duration 进行延时
            AVRational timebase = pFormatCtx->streams[audioStreamId]->time_base;
            double duration = frame->pkt_duration * av_q2d(timebase);
            av_usleep(duration * 1000000 / m_speed);
            lck.unlock();

            swr_free(&swrCtx);
            av_free(pcmBuf);
            av_frame_free(&frame);
        } else {
            av_strerror(ret, errBuf, sizeof(errBuf)-1);
            LOGE(LOGTAG, "ffmpeg avcodec_receive_frame error: %s", errBuf);
            if (AVERROR_EOF == ret) {
                lck.lock();
                isOpen = false;
                lck.unlock();
            }
        }
    }
}

void Player::renderVideo() {
    while (true) {
        unique_lock lck(mtx);
        if (closed) break;
        worker.wait(lck, [this]{ return isOpen; });
        auto pVideoCodecCtx_ = pVideoCodecCtx;
        auto pFormatCtx_ = pFormatCtx;
        auto speed = m_speed;
        lck.unlock();

        AVFrame *frame = nullptr;
        videoFrameQ.pop(frame);
        if (frame == nullptr) continue;
        LOGD(LOGTAG, "从 videoFrameQ 获取到一个 frame: pts=%ld, width: %d, height: %d",
             frame->pts, frame->width, frame->height);

        // SRC_PIX_FMT 转 RGBA
        SwsContext *swsCtx = sws_getContext(
        pVideoCodecCtx_->width, pVideoCodecCtx_->height, pVideoCodecCtx_->pix_fmt,
        pVideoCodecCtx_->width, pVideoCodecCtx_->height, AV_PIX_FMT_RGBA,
        SWS_BICUBIC, nullptr, nullptr, nullptr);
        uint8_t* dstData[4];
        int dstLineSize[4];
        av_image_alloc(dstData, dstLineSize,
           pVideoCodecCtx_->width, pVideoCodecCtx_->height, AV_PIX_FMT_RGBA, 1);
        sws_scale(swsCtx, frame->data, frame->linesize, 0, frame->height,
            dstData, dstLineSize);

        // 渲染画面与时钟同步
//        while ((av_gettime() - startTime)*m_speed < (currPosition - startPosition) * 1000000) {
//            av_usleep(100);
//        }
        lck.lock();
        videoRender.render(dstData[0]);
        AVRational timebase = pFormatCtx_->streams[videoStreamId]->time_base;
        currPosition = frame->pts * static_cast<double>(timebase.num) / timebase.den; // in seconds
        lck.unlock();

        // 根据每一帧的 duration 进行延时
        double duration = frame->pkt_duration * av_q2d(timebase);
        av_usleep(duration * 1000000 / speed);

        // 根据帧率延时
        // AVRational frameRate = pFormatCtx_->streams[videoStreamId]->avg_frame_rate;
        // double duration = av_q2d(av_inv_q(frameRate));
        // av_usleep(duration * 1000000 / speed);

        av_freep(&dstData[0]);
        sws_freeContext(swsCtx);
        av_frame_free(&frame);
    }
}

double Player::getDuration() {
    lock_guard lck(mtx);
    return static_cast<double>(pFormatCtx->duration) / AV_TIME_BASE;
}

double Player::getPosition() const {
    lock_guard lck(mtx);
    return currPosition;
}

AVStream * Player::getVideoStream() {
    for (int i = 0; i < pFormatCtx->nb_streams; ++i) {
        if (pFormatCtx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
            videoStreamId = i;
            return pFormatCtx->streams[i];
        }
    }
    return nullptr;
}

AVStream * Player::getAudioStream() {
    for (int i = 0; i < pFormatCtx->nb_streams; ++i) {
        if (pFormatCtx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_AUDIO) {
            audioStreamId = i;
            return pFormatCtx->streams[i];
        }
    }
    return nullptr;
}

bool Player::openVideoDecoder() {
    char errBuf[BUFF_SIZE]{};
    auto vs = getVideoStream();
    if (vs == nullptr) return false;

    auto pCodecParameters = vs->codecpar;
    pVideoCodec = avcodec_find_decoder(pCodecParameters->codec_id);
    if (pVideoCodec == nullptr) {
        // 没有找到解码器
        LOGE(LOGTAG, "没有找到视频解码器");
        return false;
    }

    pVideoCodecCtx = avcodec_alloc_context3(pVideoCodec);
    if (pVideoCodecCtx == nullptr) {
        return false;
    }

    int ret = avcodec_parameters_to_context(pVideoCodecCtx, pCodecParameters);
    if (ret < 0) {
        av_strerror(ret, errBuf, sizeof(errBuf) - 1);
        LOGE(LOGTAG, "使用流的参数来填充上下文失败, ffmpeg avcodec_parameters_to_context error: %s", errBuf);
        avformat_free_context(pFormatCtx);
        avcodec_free_context(&pVideoCodecCtx);
        return false;
    }

    ret = avcodec_open2(pVideoCodecCtx, pVideoCodec, nullptr);
    if (ret < 0) {
        av_strerror(ret, errBuf, sizeof(errBuf) - 1);
        LOGE(LOGTAG, "打开视频解码器失败, ffmpeg avcodec_open2 error: %s", errBuf);
        avformat_free_context(pFormatCtx);
        avcodec_free_context(&pVideoCodecCtx);
        return false;
    }

    LOGD(LOGTAG, "Video Codec: resolution %dx%d, bit rate: %ld",
         pCodecParameters->width, pCodecParameters->height,
         pCodecParameters->bit_rate);

    videoRender.setBuffers(pCodecParameters->width, pCodecParameters->height);

    return true;
}

bool Player::openAudioDecoder() {
    char errBuf[BUFF_SIZE]{};
    auto as = getAudioStream();
    if (as == nullptr) return false;

    auto pCodecParameters = as->codecpar;
    pAudioCodec = avcodec_find_decoder(pCodecParameters->codec_id);
    if (pAudioCodec == nullptr) {
        LOGE(LOGTAG, "没有找到音频解码器");
        return false;
    }

    pAudioCodecCtx = avcodec_alloc_context3(pAudioCodec);
    if (pAudioCodecCtx == nullptr) {
        return false;
    }

    int ret = avcodec_parameters_to_context(pAudioCodecCtx, pCodecParameters);
    if (ret < 0) {
        av_strerror(ret, errBuf, sizeof(errBuf) - 1);
        LOGE(LOGTAG, "使用流的参数来填充上下文失败, ffmpeg avcodec_parameters_to_context error: %s",
             errBuf);
        avformat_free_context(pFormatCtx);
        avcodec_free_context(&pAudioCodecCtx);
        return false;
    }

    ret = avcodec_open2(pAudioCodecCtx, pAudioCodec, nullptr);
    if (ret < 0) {
        av_strerror(ret, errBuf, sizeof(errBuf) - 1);
        LOGE(LOGTAG, "打开音频解码器失败, ffmpeg avcodec_open2 error: %s", errBuf);
        avformat_free_context(pFormatCtx);
        avcodec_free_context(&pAudioCodecCtx);
        return false;
    }

    LOGD(LOGTAG, "Audio Codec: %d channels, sample rate: %d", pCodecParameters->channels,
         pCodecParameters->sample_rate);

    return true;
}