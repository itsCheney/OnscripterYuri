// Plays the movies ONScripter games start with mpegplay, movie and avi.
//
// Yuri's other ports hand movies to a platform player. On iOS the system
// player can't open the MPEG-1 and old AVI files these games ship, so Mikage
// decodes them with FFmpeg and draws them into the engine's window. Like the
// engine's own waits, the player runs on the engine's stack and hands control
// back to the host after every frame; the game continues when it returns.

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/channel_layout.h>
#include <libavutil/imgutils.h>
#include <libswresample/swresample.h>
#include <libswscale/swscale.h>
}

#include <SDL2/SDL.h>
#include <SDL2/SDL_mixer.h>

#include "ONScripter.h"
#include "Utils.h"

#include <algorithm>
#include <cstring>
#include <deque>
#include <mutex>
#include <string>
#include <vector>

extern ONScripter *mikage_ons_engine;

extern "C" {
void MikageONSDelay(Uint32 ms);
void MikageONSRenderPresent(SDL_Renderer *renderer);
}

namespace {

// How much decoded sound to keep ahead of the speaker.
constexpr double kAudioLead = 0.5;
// Packets waiting for the video decoder before reading stops for audio.
constexpr size_t kMaxQueuedVideoPackets = 600;
// A longer gap between steps means the app was in the background; the movie
// clock doesn't count it.
constexpr Uint64 kPauseGapMs = 250;

// A game file the engine had to read itself (from an archive, or with a
// differently cased name), played from memory.
struct MemorySource {
    unsigned char *data = nullptr;
    size_t length = 0;
    size_t position = 0;
};

int readMemory(void *opaque, uint8_t *buffer, int size)
{
    auto *source = static_cast<MemorySource *>(opaque);
    const size_t left = source->length - source->position;
    if (left == 0)
        return AVERROR_EOF;
    const size_t count = std::min(left, static_cast<size_t>(size));
    memcpy(buffer, source->data + source->position, count);
    source->position += count;
    return static_cast<int>(count);
}

int64_t seekMemory(void *opaque, int64_t offset, int whence)
{
    auto *source = static_cast<MemorySource *>(opaque);
    if (whence == AVSEEK_SIZE)
        return static_cast<int64_t>(source->length);
    int64_t base = 0;
    switch (whence & ~AVSEEK_FORCE) {
    case SEEK_SET: base = 0; break;
    case SEEK_CUR: base = static_cast<int64_t>(source->position); break;
    case SEEK_END: base = static_cast<int64_t>(source->length); break;
    default: return -1;
    }
    const int64_t target = base + offset;
    if (target < 0 || target > static_cast<int64_t>(source->length))
        return -1;
    source->position = static_cast<size_t>(target);
    return target;
}

class Movie {
public:
    ~Movie();
    bool open(const char *path);
    // Returns true when the player saw SDL_QUIT, which the engine must
    // still handle.
    bool play(bool clickToSkip, bool loop);

private:
    bool openCodec(int stream, AVCodecContext **context);
    bool readPacket();
    bool nextVideoFrame();
    void decodeAudio();
    double audioBuffered();
    bool rewind();
    void showFrame(AVFrame *frame);
    void present();
    static void SDLCALL mixAudio(void *userdata, Uint8 *stream, int length);

    MemorySource memory;
    AVIOContext *io = nullptr;
    AVFormatContext *format = nullptr;
    int videoStream = -1;
    int audioStream = -1;
    AVCodecContext *video = nullptr;
    AVCodecContext *audio = nullptr;
    AVPacket *packet = nullptr;
    AVFrame *frame = nullptr;
    AVFrame *audioFrame = nullptr;
    std::deque<AVPacket *> videoPackets;
    bool inputEnded = false;
    bool videoEnded = true;
    double startTime = 0;

    SwsContext *scaler = nullptr;
    std::vector<uint8_t> yuv;
    SDL_Renderer *renderer = nullptr;
    SDL_Texture *texture = nullptr;
    bool haveFrame = false;

    SwrContext *resampler = nullptr;
    int mixFrequency = 0;
    int mixChannels = 0;
    Uint16 mixFormat = 0;
    AVSampleFormat outputFormat = AV_SAMPLE_FMT_NONE;
    int bytesPerSample = 0;
    std::mutex audioMutex;
    std::vector<uint8_t> audioBuffer;
    size_t audioRead = 0;
    bool audioHooked = false;
};

Movie::~Movie()
{
    if (audioHooked)
        Mix_HookMusic(nullptr, nullptr);
    for (AVPacket *queued : videoPackets)
        av_packet_free(&queued);
    if (texture)
        SDL_DestroyTexture(texture);
    sws_freeContext(scaler);
    swr_free(&resampler);
    av_frame_free(&frame);
    av_frame_free(&audioFrame);
    av_packet_free(&packet);
    avcodec_free_context(&video);
    avcodec_free_context(&audio);
    avformat_close_input(&format);
    if (io) {
        av_freep(&io->buffer);
        avio_context_free(&io);
    }
    delete[] memory.data;
}

bool Movie::open(const char *path)
{
    if (avformat_open_input(&format, path, nullptr, nullptr) < 0) {
        // Not a loose file under that exact name: ask the engine, which
        // also reads archives and ignores case.
        format = nullptr;
        const char *root = mikage_ons_engine->getArchivePath();
        const size_t rootLength = root ? strlen(root) : 0;
        const char *name = rootLength && strncmp(path, root, rootLength) == 0 ? path + rootLength : path;
        memory.data = mikage_ons_engine->readGameFile(name, &memory.length);
        if (!memory.data)
            return false;
        auto *buffer = static_cast<unsigned char *>(av_malloc(64 * 1024));
        io = buffer ? avio_alloc_context(buffer, 64 * 1024, 0, &memory, readMemory, nullptr, seekMemory) : nullptr;
        if (!io) {
            av_free(buffer);
            return false;
        }
        format = avformat_alloc_context();
        if (!format)
            return false;
        format->pb = io;
        if (avformat_open_input(&format, nullptr, nullptr, nullptr) < 0) {
            format = nullptr;
            return false;
        }
    }
    if (avformat_find_stream_info(format, nullptr) < 0)
        return false;

    videoStream = av_find_best_stream(format, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
    if (videoStream >= 0 && !openCodec(videoStream, &video))
        videoStream = -1;
    int audioCandidate = av_find_best_stream(format, AVMEDIA_TYPE_AUDIO, -1, videoStream, nullptr, 0);
    if (audioCandidate >= 0 && Mix_QuerySpec(&mixFrequency, &mixFormat, &mixChannels) &&
        openCodec(audioCandidate, &audio))
        audioStream = audioCandidate;
    if (videoStream < 0 && audioStream < 0)
        return false;

    packet = av_packet_alloc();
    frame = av_frame_alloc();
    audioFrame = av_frame_alloc();
    if (!packet || !frame || !audioFrame)
        return false;

    if (format->start_time != AV_NOPTS_VALUE)
        startTime = format->start_time / static_cast<double>(AV_TIME_BASE);
    videoEnded = videoStream < 0;

    if (videoStream >= 0) {
        renderer = SDL_GetRenderer(mikage_ons_engine->getWindow());
        if (!renderer)
            return false;
        texture = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_IYUV, SDL_TEXTUREACCESS_STREAMING,
                                    video->width, video->height);
        if (!texture)
            return false;
    }

    if (audioStream >= 0) {
        switch (mixFormat) {
        case AUDIO_S16SYS: outputFormat = AV_SAMPLE_FMT_S16; bytesPerSample = 2; break;
        case AUDIO_S32SYS: outputFormat = AV_SAMPLE_FMT_S32; bytesPerSample = 4; break;
        case AUDIO_F32SYS: outputFormat = AV_SAMPLE_FMT_FLT; bytesPerSample = 4; break;
        case AUDIO_U8: outputFormat = AV_SAMPLE_FMT_U8; bytesPerSample = 1; break;
        default: break;
        }
        AVChannelLayout outputLayout;
        av_channel_layout_default(&outputLayout, mixChannels);
        if (outputFormat == AV_SAMPLE_FMT_NONE ||
            swr_alloc_set_opts2(&resampler, &outputLayout, outputFormat, mixFrequency,
                                &audio->ch_layout, audio->sample_fmt, audio->sample_rate, 0, nullptr) < 0 ||
            swr_init(resampler) < 0) {
            utils::printInfo("movie: can't convert its sound; playing it silent\n");
            avcodec_free_context(&audio);
            audioStream = -1;
        }
        av_channel_layout_uninit(&outputLayout);
    }
    return true;
}

bool Movie::openCodec(int stream, AVCodecContext **context)
{
    const AVCodecParameters *parameters = format->streams[stream]->codecpar;
    const AVCodec *codec = avcodec_find_decoder(parameters->codec_id);
    if (!codec)
        return false;
    *context = avcodec_alloc_context3(codec);
    if (!*context || avcodec_parameters_to_context(*context, parameters) < 0)
        return false;
    // Decode on FFmpeg's own threads; the engine's stack only collects frames.
    (*context)->thread_count = 0;
    (*context)->pkt_timebase = format->streams[stream]->time_base;
    if (avcodec_open2(*context, codec, nullptr) < 0) {
        avcodec_free_context(context);
        return false;
    }
    return true;
}

// Reads one packet: sound is decoded at once, pictures wait in a queue for
// their turn. Returns false once the file is exhausted.
bool Movie::readPacket()
{
    if (inputEnded)
        return false;
    const int result = av_read_frame(format, packet);
    if (result < 0) {
        inputEnded = true;
        if (audio) {
            avcodec_send_packet(audio, nullptr);
            decodeAudio();
        }
        return false;
    }
    if (packet->stream_index == audioStream) {
        if (avcodec_send_packet(audio, packet) == 0)
            decodeAudio();
    } else if (packet->stream_index == videoStream) {
        AVPacket *queued = av_packet_clone(packet);
        if (queued)
            videoPackets.push_back(queued);
    }
    av_packet_unref(packet);
    return true;
}

bool Movie::nextVideoFrame()
{
    if (videoEnded)
        return false;
    for (;;) {
        const int result = avcodec_receive_frame(video, frame);
        if (result == 0)
            return true;
        if (result == AVERROR_EOF) {
            videoEnded = true;
            return false;
        }
        if (result != AVERROR(EAGAIN)) {
            videoEnded = true;
            return false;
        }
        while (videoPackets.empty() && readPacket()) {
        }
        if (videoPackets.empty()) {
            avcodec_send_packet(video, nullptr);
            continue;
        }
        AVPacket *queued = videoPackets.front();
        videoPackets.pop_front();
        avcodec_send_packet(video, queued);
        av_packet_free(&queued);
    }
}

void Movie::decodeAudio()
{
    while (avcodec_receive_frame(audio, audioFrame) == 0) {
        const int capacity = swr_get_out_samples(resampler, audioFrame->nb_samples);
        if (capacity > 0) {
            std::vector<uint8_t> converted(static_cast<size_t>(capacity) * mixChannels * bytesPerSample);
            uint8_t *output = converted.data();
            const int samples = swr_convert(resampler, &output, capacity,
                                            audioFrame->extended_data,
                                            audioFrame->nb_samples);
            if (samples > 0) {
                std::lock_guard<std::mutex> lock(audioMutex);
                audioBuffer.insert(audioBuffer.end(), converted.begin(),
                                   converted.begin() + static_cast<size_t>(samples) * mixChannels * bytesPerSample);
            }
        }
        av_frame_unref(audioFrame);
    }
}

double Movie::audioBuffered()
{
    std::lock_guard<std::mutex> lock(audioMutex);
    return static_cast<double>(audioBuffer.size() - audioRead) /
           (static_cast<double>(mixFrequency) * mixChannels * bytesPerSample);
}

void SDLCALL Movie::mixAudio(void *userdata, Uint8 *stream, int length)
{
    auto *movie = static_cast<Movie *>(userdata);
    std::lock_guard<std::mutex> lock(movie->audioMutex);
    const size_t available = movie->audioBuffer.size() - movie->audioRead;
    const size_t count = std::min(available, static_cast<size_t>(length));
    if (count)
        memcpy(stream, movie->audioBuffer.data() + movie->audioRead, count);
    if (count < static_cast<size_t>(length))
        memset(stream + count, movie->mixFormat == AUDIO_U8 ? 0x80 : 0, length - count);
    movie->audioRead += count;
    if (movie->audioRead > (1u << 20)) {
        movie->audioBuffer.erase(movie->audioBuffer.begin(), movie->audioBuffer.begin() + movie->audioRead);
        movie->audioRead = 0;
    }
}

bool Movie::rewind()
{
    if (av_seek_frame(format, -1, format->start_time != AV_NOPTS_VALUE ? format->start_time : 0,
                      AVSEEK_FLAG_BACKWARD) < 0)
        return false;
    for (AVPacket *queued : videoPackets)
        av_packet_free(&queued);
    videoPackets.clear();
    if (video)
        avcodec_flush_buffers(video);
    if (audio)
        avcodec_flush_buffers(audio);
    inputEnded = false;
    videoEnded = videoStream < 0;
    return true;
}

void Movie::showFrame(AVFrame *source)
{
    if (source->format == AV_PIX_FMT_YUV420P || source->format == AV_PIX_FMT_YUVJ420P) {
        SDL_UpdateYUVTexture(texture, nullptr, source->data[0], source->linesize[0], source->data[1],
                             source->linesize[1], source->data[2], source->linesize[2]);
        haveFrame = true;
        return;
    }
    scaler = sws_getCachedContext(scaler, source->width, source->height,
                                  static_cast<AVPixelFormat>(source->format), video->width, video->height,
                                  AV_PIX_FMT_YUV420P, SWS_BILINEAR, nullptr, nullptr, nullptr);
    if (!scaler)
        return;
    uint8_t *planes[4];
    int strides[4];
    const int size = av_image_get_buffer_size(AV_PIX_FMT_YUV420P, video->width, video->height, 1);
    if (size <= 0)
        return;
    yuv.resize(static_cast<size_t>(size));
    av_image_fill_arrays(planes, strides, yuv.data(), AV_PIX_FMT_YUV420P, video->width, video->height, 1);
    sws_scale(scaler, source->data, source->linesize, 0, source->height, planes, strides);
    SDL_UpdateYUVTexture(texture, nullptr, planes[0], strides[0], planes[1], strides[1], planes[2], strides[2]);
    haveFrame = true;
}

// Draws the movie over the area the game is drawn in, keeping the movie's
// shape relative to the game screen. When the game is stretched to the
// display, the movie stretches with it.
void Movie::present()
{
    const SDL_Rect view = mikage_ons_engine->getRenderViewRect();
    const int gameWidth = mikage_ons_engine->getWidth();
    const int gameHeight = mikage_ons_engine->getHeight();
    AVRational aspect = video->sample_aspect_ratio;
    double movieWidth = video->width * (aspect.num > 0 && aspect.den > 0 ? av_q2d(aspect) : 1.0);
    double movieHeight = video->height;
    const double fit = std::min(gameWidth / movieWidth, gameHeight / movieHeight);
    movieWidth *= fit;
    movieHeight *= fit;
    const double scaleX = static_cast<double>(view.w) / gameWidth;
    const double scaleY = static_cast<double>(view.h) / gameHeight;
    SDL_Rect target;
    target.w = static_cast<int>(movieWidth * scaleX + 0.5);
    target.h = static_cast<int>(movieHeight * scaleY + 0.5);
    target.x = view.x + (view.w - target.w) / 2;
    target.y = view.y + (view.h - target.h) / 2;

    Uint8 r, g, b, a;
    SDL_GetRenderDrawColor(renderer, &r, &g, &b, &a);
    SDL_SetRenderTarget(renderer, nullptr);
    SDL_SetRenderDrawColor(renderer, 0, 0, 0, 255);
    SDL_RenderClear(renderer);
    if (haveFrame)
        SDL_RenderCopy(renderer, texture, nullptr, &target);
    SDL_SetRenderDrawColor(renderer, r, g, b, a);
    MikageONSRenderPresent(renderer);
}

bool Movie::play(bool clickToSkip, bool loop)
{
    // MIDI music plays outside the mixer, so it is paused by hand; other
    // music stops being mixed while the movie's sound is hooked in.
    const bool pausedMusic = Mix_PlayingMusic() && !Mix_PausedMusic();
    if (pausedMusic)
        Mix_PauseMusic();
    if (audioStream >= 0) {
        Mix_HookMusic(mixAudio, this);
        audioHooked = true;
    }

    bool quit = false;
    bool skipped = false;
    bool pending = false;
    std::vector<SDL_Event> windowEvents;
    double pendingTime = 0;
    Uint64 clockStart = SDL_GetTicks64();
    Uint64 lastStep = clockStart;

    for (;;) {
        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            if (event.type == SDL_WINDOWEVENT) {
                windowEvents.push_back(event);
            } else if (event.type == SDL_QUIT) {
                quit = true;
            } else if (clickToSkip && event.type == SDL_MOUSEBUTTONUP) {
                skipped = true;
            } else if (clickToSkip && event.type == SDL_KEYUP &&
                       (event.key.keysym.sym == SDLK_RETURN || event.key.keysym.sym == SDLK_SPACE ||
                        event.key.keysym.sym == SDLK_ESCAPE)) {
                skipped = true;
            }
        }
        if (quit || skipped)
            break;

        const Uint64 now = SDL_GetTicks64();
        if (now - lastStep > kPauseGapMs)
            clockStart += now - lastStep;
        lastStep = now;
        const double clock = (now - clockStart) / 1000.0;

        // Keep the sound ahead of the speaker.
        while (audioStream >= 0 && !inputEnded && audioBuffered() < kAudioLead &&
               videoPackets.size() < kMaxQueuedVideoPackets)
            readPacket();

        // Show the newest picture that is due; late ones are dropped.
        bool due = false;
        for (;;) {
            if (!pending) {
                if (!nextVideoFrame())
                    break;
                const int64_t timestamp = frame->best_effort_timestamp;
                pendingTime = timestamp == AV_NOPTS_VALUE
                    ? clock
                    : timestamp * av_q2d(format->streams[videoStream]->time_base) - startTime;
                pending = true;
            }
            if (pendingTime > clock)
                break;
            showFrame(frame);
            av_frame_unref(frame);
            pending = false;
            due = true;
        }

        const bool soundDone = audioStream < 0 || (inputEnded && audioBuffered() <= 0);
        if (videoEnded && !pending && soundDone) {
            if (loop && rewind()) {
                clockStart = SDL_GetTicks64();
                lastStep = clockStart;
                continue;
            }
            break;
        }

        if (due)
            present();
        else
            MikageONSDelay(1);
    }

    if (audioHooked) {
        Mix_HookMusic(nullptr, nullptr);
        audioHooked = false;
    }
    if (pausedMusic)
        Mix_ResumeMusic();
    // The engine lays itself out again on window changes it missed.
    for (SDL_Event &missed : windowEvents)
        SDL_PushEvent(&missed);
    return quit;
}

} // namespace

// Called by ONScripter's mpegplay, movie and avi commands.
extern "C" void playVideoIOS(const char *filename, bool click_flag, bool loop_flag)
{
    if (!filename || !mikage_ons_engine || !mikage_ons_engine->getWindow())
        return;
    bool quit = false;
    {
        Movie movie;
        if (!movie.open(filename)) {
            utils::printInfo("movie: can't play %s\n", filename);
            return;
        }
        utils::printInfo("movie: playing %s\n", filename);
        quit = movie.play(click_flag, loop_flag);
    }
    // Bring the game's screen back; mpegplay repaints too, movie doesn't.
    mikage_ons_engine->repaintCommand();
    if (quit) {
        // The engine still has to see the request to quit.
        SDL_Event event;
        SDL_zero(event);
        event.type = SDL_QUIT;
        SDL_PushEvent(&event);
    }
}
