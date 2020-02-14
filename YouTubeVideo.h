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
#include "Renderer.h"

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

template<AVMediaType MEDIA_TYPE>
class MediaStream
{
public:
	MediaStream(const std::string& _url, const Clock& _clock);

	virtual void start() = 0;
	virtual void stop() = 0;
	virtual void pause() = 0;
	virtual void unpause() = 0;

	void seek(std::chrono::duration<double> _new_time)
	{
		new_time = _new_time;
		seek_requested = true;
	};

protected:
	const std::string url;
	std::unique_ptr<AVFormatContext> format_ctx;
	std::unique_ptr<AVCodecContext> codec_ctx;
	std::unique_ptr<AVFrame> working_frame;

	int stream_index;
	double timebase;

	const Clock& clock;

	bool seek_requested = false;
	std::chrono::duration<double> new_time;
};

class VideoStream : public MediaStream<AVMEDIA_TYPE_VIDEO>
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

	std::tuple<int, int, AVRational> get_size()
	{
		return { codec_ctx->width, codec_ctx->height, codec_ctx->sample_aspect_ratio };
	}

private:
	void decode_frame();

private:
	std::unique_ptr<SDL_Texture> back_buffer;
	std::unique_ptr<SDL_Texture> current_frame;
	std::mutex frame_mtx;

	GuardedRenderer& renderer;

	bool paused = false;
	std::jthread decode_thread;
	std::mutex continue_mtx;
	std::condition_variable continue_cv;
};

class AudioStream : public MediaStream<AVMEDIA_TYPE_AUDIO>
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

	friend void sdl_callback(void* ptr, Uint8* stream, int len);

private:
	int decode_frame();
	int synchronize(int nb_samples);

private:
	std::array<uint8_t, MAX_AUDIO_FRAME_SIZE> audio_buffer;
	int buffer_size = 0;
	int buffer_index = 0;

	int device_id;

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

inline std::unique_ptr<AVFormatContext> avformat_open_input(std::string_view filename)
{
	AVFormatContext* ic = nullptr;

	if (avformat_open_input(&ic, filename.data(), nullptr, nullptr) < 0)
		throw std::runtime_error("Could not open format input");

	if (avformat_find_stream_info(ic, nullptr) < 0)
		throw std::runtime_error("Could not read stream info");

	return std::unique_ptr<AVFormatContext>(ic);
}

inline std::unique_ptr<AVCodecContext> make_codec_context(const AVCodecParameters* const codecpar)
{
	auto ctx = std::unique_ptr<AVCodecContext>{ avcodec_alloc_context3(nullptr) };

	if (0 > avcodec_parameters_to_context(ctx.get(), codecpar))
		return nullptr;

	return ctx;
}

template<AVMediaType MEDIA_TYPE>
inline MediaStream<MEDIA_TYPE>::MediaStream(const std::string& _url, const Clock& _clock)
	: url{ _url }, clock{ _clock }
{
	using namespace std::string_literals;

	format_ctx = avformat_open_input(url);
	av_dump_format(format_ctx.get(), 0, url.c_str(), 0);

	stream_index = av_find_best_stream(format_ctx.get(), MEDIA_TYPE, -1, -1, nullptr, 0);
	timebase = av_q2d(format_ctx->streams[stream_index]->time_base);
	codec_ctx = make_codec_context(format_ctx->streams[stream_index]->codecpar);

	auto codec = avcodec_find_decoder(codec_ctx->codec_id);
	if (!codec)
		throw std::runtime_error("Unsupported codec: "s + avcodec_get_name(codec_ctx->codec_id));

	avcodec_open2(codec_ctx.get(), codec, nullptr);

	working_frame = std::unique_ptr<AVFrame>{ av_frame_alloc() };
}
