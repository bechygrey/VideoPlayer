
#include <stdio.h>
#include <errno.h>
#include <stdbool.h>
#include <stdlib.h>

#include <SDL2/SDL.h>
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libswscale/swscale.h>
#include <libswresample/swresample.h>
#include <libavutil/avutil.h>

#define WINDOW_WIDTH 800
#define WINDOW_HEIGHT 600

static void error(const char* description);
void scaleVideo(int origWidth, int origHeight, int maxWidth, int maxHeight, int* outWidth, int* outHeight);

int main(int argc, char **argv) {
    if (argc == 1) error("Missing filepath Argument");
    AVFormatContext* inputFmtCtx = avformat_alloc_context();
    if (avformat_open_input(&inputFmtCtx, argv[1], NULL, NULL) != 0) error("Failed to open input ");
    if (avformat_find_stream_info(inputFmtCtx, NULL) < 0) error("Failed to find stream info ");

    AVCodec *audioDecoder, *videoDecoder;
    int audioStreamIndex = av_find_best_stream(inputFmtCtx, AVMEDIA_TYPE_AUDIO, -1, -1, &audioDecoder, 0);
    int videoStreamIndex = av_find_best_stream(inputFmtCtx, AVMEDIA_TYPE_VIDEO, -1, -1, &videoDecoder, 0);
    if (audioStreamIndex < 0 && videoStreamIndex < 0) error("No stream found ");
        
    AVCodecContext *audioDecodeCtx = avcodec_alloc_context3(audioDecoder);
    if (avcodec_parameters_to_context(audioDecodeCtx, inputFmtCtx->streams[audioStreamIndex]->codecpar) < 0) return -1;
    if (avcodec_open2(audioDecodeCtx, audioDecoder, NULL) < 0) error("Failed to open Videodecoder ");
    AVCodecContext *videoDecodeCtx = avcodec_alloc_context3(videoDecoder);
    if (avcodec_parameters_to_context(videoDecodeCtx, inputFmtCtx->streams[videoStreamIndex]->codecpar) < 0) return -1;
    if (avcodec_open2(videoDecodeCtx, videoDecoder, NULL) < 0) error("Failed to open Audiodecoder ");

    int scaleWidth, scaleHeight;
    scaleVideo(videoDecodeCtx->width, videoDecodeCtx->height, WINDOW_WIDTH, WINDOW_HEIGHT, &scaleWidth, &scaleHeight);
    struct SwsContext *scaler = sws_getContext(videoDecodeCtx->width, videoDecodeCtx->height, videoDecodeCtx->pix_fmt, scaleWidth, scaleHeight, AV_PIX_FMT_RGB24, SWS_BILINEAR, NULL, NULL, NULL);
    SwrContext *resampler = swr_alloc_set_opts(NULL, audioDecodeCtx->channel_layout, AV_SAMPLE_FMT_S16, audioDecodeCtx->sample_rate, audioDecodeCtx->channel_layout, audioDecodeCtx->sample_fmt, audioDecodeCtx->sample_rate, 0, NULL);
    if (swr_init(resampler) != 0) error("Failed to Init Resampler");

    AVFrame *dstframe = av_frame_alloc();
    dstframe->width = WINDOW_WIDTH;
    dstframe->height = WINDOW_HEIGHT;
    dstframe->format = AV_PIX_FMT_RGB24;
    av_frame_get_buffer(dstframe, 0);

    AVFrame *audioframe = av_frame_alloc();
    audioframe->channel_layout = audioDecodeCtx->channel_layout;
    audioframe->sample_rate = audioDecodeCtx->sample_rate;
    audioframe->format = AV_SAMPLE_FMT_S16;
    av_frame_get_buffer(audioframe, 0);

    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO) < 0) error("Failed to Init SDL2");
    SDL_Window *window = SDL_CreateWindow("Video Player", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, scaleWidth, scaleHeight, 0);
    SDL_Renderer *renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_ACCELERATED);
    SDL_Texture *texture = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_RGB24, SDL_TEXTUREACCESS_STREAMING, scaleWidth, scaleHeight);
    SDL_Rect rect = {0, 0, scaleWidth, scaleHeight};

    SDL_Event *event;
    bool runloop = true;
    uint32_t startTime = SDL_GetTicks();
    SDL_AudioSpec want, have;
    SDL_zero(want);
    SDL_zero(have);
    want.freq = audioDecodeCtx->sample_rate;
    want.channels = audioDecodeCtx->channels;
    want.format = AUDIO_S16SYS;
    SDL_AudioDeviceID dev = SDL_OpenAudioDevice(NULL, 0, &want, &have, 0);
    SDL_PauseAudioDevice(dev, 0);
    
    AVFrame *frame = av_frame_alloc();
    AVPacket *packet = av_packet_alloc();

    while (av_read_frame(inputFmtCtx, packet) == 0 && runloop) {
        while (SDL_PollEvent(event)) {
            if (event->type == SDL_QUIT) runloop = false;
            else if (event->type == SDL_KEYDOWN) {
                if (event->key.keysym.sym == SDLK_ESCAPE) runloop = false;
            }
        }

        if (packet->stream_index == videoStreamIndex) {
            AVStream *stream = inputFmtCtx->streams[packet->stream_index];
            double timebase = av_q2d(stream->time_base);

            int response = avcodec_send_packet(videoDecodeCtx, packet);
            if (response < 0) error("Error submitting the packet to the audio decoder");
        
            while (response >= 0) {
                response = avcodec_receive_frame(videoDecodeCtx, frame);
                if (response == AVERROR(EAGAIN) || response == AVERROR_EOF) break;
                else if (response < 0) error("Error during decoding");

                uint32_t video_time = (double)frame->best_effort_timestamp * timebase * 1000;
                while (true) {
                    uint32_t elapsed = SDL_GetTicks() - startTime;
                    if (elapsed >= video_time) break;
                }
                                
                sws_scale(scaler, (const uint8_t * const *)frame->data, frame->linesize, 0, frame->height, dstframe->data, dstframe->linesize);
                SDL_UpdateTexture(texture, &rect, dstframe->data[0], dstframe->linesize[0]);
                SDL_RenderClear(renderer);
                SDL_RenderCopy(renderer, texture, NULL, &rect);
                SDL_RenderPresent(renderer);
            }
        } else if (packet->stream_index == audioStreamIndex) {
            int response = avcodec_send_packet(audioDecodeCtx, packet);
            if (response < 0) error("Error submitting the packet to the audio decoder");
        
            while (response >= 0) {
                response = avcodec_receive_frame(audioDecodeCtx, frame);
                if (response == AVERROR(EAGAIN) || response == AVERROR_EOF) break;
                else if (response < 0) error("Error during decoding");

                int64_t sgd = swr_get_delay(resampler, frame->sample_rate);
                int dst_samples = frame->channels * av_rescale_rnd(sgd + frame->nb_samples, audioDecodeCtx->sample_rate, frame->sample_rate, AV_ROUND_UP);
                uint8_t *audiobuf = NULL;
                response = av_samples_alloc(&audiobuf, NULL, 1, dst_samples, AV_SAMPLE_FMT_S16, 1);
                dst_samples = frame->channels * swr_convert(resampler, &audiobuf, dst_samples, (const uint8_t**) frame->data, frame->nb_samples);
                response = av_samples_fill_arrays(audioframe->data, audioframe->linesize, audiobuf,1, dst_samples, AV_SAMPLE_FMT_S16, 1);
                SDL_QueueAudio(dev, audioframe->data[0], audioframe->linesize[0]);
            }
        }

        av_packet_unref(packet);
    }

    av_frame_free(&dstframe);
    av_frame_free(&audioframe);
    av_frame_free(&frame);
    av_packet_free(&packet);

    avcodec_close(audioDecodeCtx);
    avcodec_close(videoDecodeCtx);
    avformat_close_input(&inputFmtCtx);

    sws_freeContext(scaler);
    swr_free(&resampler);
    
    SDL_CloseAudioDevice(dev);
    SDL_DestroyTexture(texture);
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    SDL_Quit();

    return 0;
}

static void error(const char* description) {
    fprintf(stderr, "%s\n", description);
    exit(1);
}

void scaleVideo(int origWidth, int origHeight, int maxWidth, int maxHeight, int* outWidth, int* outHeight) {
    double aspectRatio = (double)origWidth / (double)origHeight;
    if (origWidth > maxWidth) {
        *outWidth = maxWidth;
        *outHeight = (int)(*outWidth / aspectRatio);
    } else if (origHeight > maxHeight) {
        *outHeight = maxHeight;
        *outWidth = (int)(*outHeight * aspectRatio);
    } else {
        *outWidth = origWidth;
        *outHeight = origHeight;
    }
}