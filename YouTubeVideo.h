#pragma once

#include <string>
#include <memory>
#include <chrono>
#include <mutex>
#include <array>
#include <utility>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libswscale/swscale.h>
#include <libavutil/imgutils.h>
#include <libswresample/swresample.h>
}

#include <SDL2/SDL.h>

#include "jthread/source/jthread.hpp"

#include "Deleters.h"

class GuardedRenderer
{
public:
	GuardedRenderer(SDL_Window* window)
	{
		SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "linear");
		renderer = std::unique_ptr<SDL_Renderer>(SDL_CreateRenderer(window, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC));
	}
	auto get_renderer()
	{
		std::unique_lock<std::mutex> lc{ renderer_mtx };
		return std::tuple<std::unique_lock<std::mutex>, SDL_Renderer*>(std::move(lc), renderer.get());
	}
	std::mutex renderer_mtx;
private:
	std::unique_ptr<SDL_Renderer> renderer;
};

class Clock
{
public:
	auto time() const
	{
		if (paused)
		{
			return (last_paused - start) - time_adjustment;
		}
		else
		{
			return (std::chrono::steady_clock::now() - start) - time_adjustment;
		}
	}
	void pause()
	{
		last_paused = std::chrono::steady_clock::now();
		paused = true;
	}
	void unpause()
	{
		time_adjustment += (std::chrono::steady_clock::now() - last_paused);
		paused = false;
	}
	void seek(std::chrono::duration<double> new_time)
	{
		time_adjustment -= (new_time - time());
	}

private:
	std::chrono::steady_clock::time_point start = std::chrono::steady_clock::now();
	std::chrono::duration<double> time_adjustment{ 0 };
	std::chrono::steady_clock::time_point last_paused = std::chrono::steady_clock::now();

	bool paused = true;
};

class VideoStream
{
public:
	VideoStream(const std::string& _url, GuardedRenderer& _renderer, const Clock& _clock);
	~VideoStream() { stop(); };
	void start();
	void stop();
	void pause();
	void unpause();

	auto get_frame()
	{
		std::unique_lock<std::mutex> lc{ frame_mtx };
		return std::tuple<std::unique_lock<std::mutex>, SDL_Texture*>(std::move(lc), current_frame.get());
	}

	void seek(std::chrono::duration<double> _new_time);

	std::tuple<int, int, AVRational> get_size()
	{
		return { codec_ctx->width, codec_ctx->height, codec_ctx->sample_aspect_ratio };
	}

private:
	void decode_frame();

private:
	std::string url;
	std::unique_ptr<AVFormatContext> format_ctx;
	std::unique_ptr<AVCodecContext> codec_ctx;

	std::unique_ptr<AVFrame> working_frame;
	std::unique_ptr<SDL_Texture> back_buffer;
	std::unique_ptr<SDL_Texture> current_frame;
	std::mutex frame_mtx;

	int stream_index;
	double timebase;

	GuardedRenderer& renderer;

	const Clock& clock;

	bool paused = false;
	std::jthread decode_thread;
	std::mutex continue_mtx;
	std::condition_variable continue_cv;

	bool seek_requested = false;
	std::chrono::duration<double> new_time;
};

class AudioStream
{
	static constexpr int SDL_AUDIO_BUFFER_SIZE = 1024;
	static constexpr int MAX_AUDIO_FRAME_SIZE = 192000;
	static constexpr int AUDIO_DIFF_AVG_NB = 20;
	static constexpr double AV_NOSYNC_THRESHOLD = 10.0;
	static constexpr int SAMPLE_CORRECTION_PERCENT_MAX = 10;

	struct AudioParams {
		int freq;
		int channels;
		int64_t channel_layout;
		enum AVSampleFormat fmt;
		int frame_size;
		int bytes_per_sec;
	};
public:
	AudioStream(const std::string& _url, const Clock& _clock);
	~AudioStream()
	{
		SDL_CloseAudioDevice(device_id);
		stop();
	};
	void start();
	void stop();
	void pause();
	void unpause();

	void seek(std::chrono::duration<double> _new_time);

	friend void sdl_callback(void* ptr, Uint8* stream, int len);

private:
	int decode_frame();
	int synchronize(int nb_samples);

private:
	std::string url;
	std::unique_ptr<AVFormatContext> format_ctx;
	std::unique_ptr<AVCodecContext> codec_ctx;

	std::unique_ptr<AVFrame> working_frame;

	int stream_index;
	double timebase;

	std::array<uint8_t, MAX_AUDIO_FRAME_SIZE> audio_buffer;
	int buffer_size = 0;
	int buffer_index = 0;

	int device_id;

	const Clock& clock;

	struct AudioParams audio_src;
	struct AudioParams audio_tgt;
	std::unique_ptr<SwrContext> swr_ctx;

	double cummulative_difference = 0.;
	const double average_difference_coef = std::exp(std::log(0.01) / AUDIO_DIFF_AVG_NB); // extract 
	int average_differance_count = 0;
	double difference_threshold;

	bool paused = false;
	std::mutex continue_mtx;
	std::condition_variable continue_cv;

	bool seek_requested = false;
	std::chrono::duration<double> new_time;
};

class YouTubeVideo
{
public:
	YouTubeVideo(std::string id, GuardedRenderer& _renderer, int media_type = Video | Audio);

	void start();
	void stop();
	void pause();
	void unpause();

	void seek(std::chrono::duration<double> _new_time);

	auto get_video_frame() -> decltype(std::declval<VideoStream>().get_frame());
	auto get_video_size()
	{
		if (video_stream)
			return video_stream->get_size();
		throw std::runtime_error("Media does not have active video stream");
	}
	auto get_time()
	{
		return clock.time();
	}

public:
	enum MediaType { Video = 0x1, Audio = 0x2 };
private:
	std::unique_ptr<VideoStream> video_stream;
	std::unique_ptr<AudioStream> audio_stream;
	Clock clock;

	bool paused = true;
};