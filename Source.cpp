extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libswscale/swscale.h>
#include <libavutil/imgutils.h>
#include <libswresample/swresample.h>
}

#pragma comment(lib, "avcodec.lib")
#pragma comment(lib, "avformat.lib")
#pragma comment(lib, "swscale.lib")
#pragma comment(lib, "avutil.lib")
#pragma comment(lib, "swresample.lib")

#include <SDL2/SDL.h>
#include <SDL2/SDL_thread.h>

#undef main

#include "jthread/source/jthread.hpp"

#include <stdio.h>
#include <string>
#include <stdexcept>
#include <memory>
#include <assert.h>
#include <thread>
#include <chrono>
#include <array>

#define AV_SYNC_THRESHOLD 0.01
#define AV_NOSYNC_THRESHOLD 10.0

#define SAMPLE_CORRECTION_PERCENT_MAX 10
#define AUDIO_DIFF_AVG_NB 20

using namespace std::chrono_literals;
using namespace std::string_literals;

namespace std
{
	template<> struct default_delete<AVFormatContext> {
		void operator()(AVFormatContext* ptr)
		{
			avformat_close_input(&ptr);
		}
	};

	template<> struct default_delete<AVCodecContext> {
		void operator()(AVCodecContext* ptr)
		{
			avcodec_free_context(&ptr);
		}
	};

	template<> struct default_delete<AVFrame> {
		void operator()(AVFrame* ptr)
		{
			av_frame_free(&ptr);
		}
	};

	template<> struct default_delete<SDL_Texture> {
		void operator()(SDL_Texture* ptr)
		{
			SDL_DestroyTexture(ptr);
		}
	};

	template<> struct default_delete<SDL_Renderer> {
		void operator()(SDL_Renderer* ptr)
		{
			SDL_DestroyRenderer(ptr);
		}
	};

	template<> struct default_delete<SDL_Window> {
		void operator()(SDL_Window* ptr)
		{
			SDL_DestroyWindow(ptr);
		}
	};

	template<> struct default_delete<SwrContext> {
		void operator()(SwrContext* ptr)
		{
			swr_free(&ptr);
		}
	};
}

std::unique_ptr<AVFormatContext> avformat_open_input(std::string_view filename)
{
	AVFormatContext* ic = nullptr;

	if (avformat_open_input(&ic, filename.data(), nullptr, nullptr) < 0)
		throw std::runtime_error("Could not open format input");

	if (avformat_find_stream_info(ic, nullptr) < 0)
		throw std::runtime_error("Could not read stream info");

	return std::unique_ptr<AVFormatContext>(ic);
}

std::unique_ptr<AVCodecContext> make_codec_context(const AVCodecParameters* const codecpar)
{
	auto ctx = std::unique_ptr<AVCodecContext>{ avcodec_alloc_context3(nullptr) };

	if (0 > avcodec_parameters_to_context(ctx.get(), codecpar))
		return nullptr;

	return ctx;
}

template<typename ... Args>
std::unique_ptr<SwrContext> make_swr_context(Args&&... args)
{
	auto swr = std::unique_ptr<SwrContext>(swr_alloc_set_opts(std::forward<Args>(args)...));

	if (!swr || swr_init(swr.get()) < 0)
		return nullptr;

	return swr;
}

class GuardedRenderer
{
public:
	GuardedRenderer(SDL_Window* window)
	{
		renderer = std::unique_ptr<SDL_Renderer>(SDL_CreateRenderer(window, -1, 0));
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
			return (last_paused - start) - paused_time;
		}
		else
		{
			return (std::chrono::steady_clock::now() - start) - paused_time;
		}
	}
	void pause()
	{
		last_paused = std::chrono::steady_clock::now();
		paused = true;
	}
	void unpause()
	{
		paused_time += (std::chrono::steady_clock::now() - last_paused);
		paused = false;
	}
private:
	std::chrono::steady_clock::time_point start = std::chrono::steady_clock::now();
	std::chrono::duration<double> paused_time{ 0 };
	std::chrono::steady_clock::time_point last_paused;

	bool paused = false;
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
};

class AudioStream
{
	static constexpr int SDL_AUDIO_BUFFER_SIZE = 1024;
	static constexpr int MAX_AUDIO_FRAME_SIZE = 192000;

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
};

void sdl_callback(void* ptr, Uint8* stream, int len);

// https://blogs.gentoo.org/lu_zero/2016/03/29/new-avcodec-api/
// The flush packet is a non-NULL packet with size 0 and data NULL
int decode(AVCodecContext* avctx, AVFrame* frame, int* got_frame, AVPacket* pkt)
{
	int ret;

	*got_frame = 0;

	if (pkt) {
		ret = avcodec_send_packet(avctx, pkt);
		// In particular, we don't expect AVERROR(EAGAIN), because we read all
		// decoded frames with avcodec_receive_frame() until done.
		if (ret < 0)
			return ret == AVERROR_EOF ? 0 : ret;
	}

	ret = avcodec_receive_frame(avctx, frame);
	if (ret < 0 && ret != AVERROR(EAGAIN) && ret != AVERROR_EOF)
		return ret;
	if (ret >= 0)
		*got_frame = 1;

	return 0;
}

int main(int argc, char* argv[])
{
	if (argc < 2) {
		fprintf(stderr, "Usage: test <file>\n");
		return -1;
	}

	if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_TIMER))
	{
		fprintf(stderr, "Could not initialize SDL- %s\n", SDL_GetError());
		return -1;
	}

	auto window = std::unique_ptr<SDL_Window>(SDL_CreateWindow("YouTubeTV", SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED, 800, 400, SDL_WINDOW_SHOWN));
	if (!window)
	{
		fprintf(stderr, "SDL: could not create window - exiting\n");
		return -1;
	}

	GuardedRenderer renderer(window.get());

	Clock clock;

	VideoStream vs("video.mp4", renderer, clock);
	AudioStream as("audio.webm", clock);

	vs.start();
	as.start();

	bool paused = false;

	SDL_Event event;
	while (true)
	{
		{
			// won't deadlock 'cause this is the only thread that needs both at the same time
			auto [flc, frame_ptr] = vs.get_frame();
			auto [rlc, renderer_ptr] = renderer.get_renderer();
			SDL_RenderCopy(renderer_ptr, frame_ptr, nullptr, nullptr);
			flc.unlock();

			SDL_RenderPresent(renderer_ptr);
		}

		SDL_PollEvent(&event);
		switch (event.type)
		{
		case SDL_KEYDOWN:
			switch (event.key.keysym.sym) {
			case SDLK_SPACE:
				if (paused)
				{
					clock.unpause();
					vs.unpause();
					as.unpause();
					paused = false;
				}
				else
				{
					clock.pause();
					vs.pause();
					as.pause();
					paused = true;
				}
				break;
			}
			break;
		case SDL_QUIT:
			SDL_Quit();
			return 0;
			break;
		default:
			break;
		}
	}

	return 0;
}

VideoStream::VideoStream(const std::string& _url, GuardedRenderer& _renderer, const Clock& _clock)
	: url{ _url }, renderer{ _renderer }, clock{ _clock }
{
	format_ctx = avformat_open_input(url);
	av_dump_format(format_ctx.get(), 0, url.c_str(), 0);

	stream_index = av_find_best_stream(format_ctx.get(), AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
	codec_ctx = make_codec_context(format_ctx->streams[stream_index]->codecpar);
	timebase = av_q2d(format_ctx->streams[stream_index]->time_base);

	auto codec = avcodec_find_decoder(codec_ctx->codec_id);

	avcodec_open2(codec_ctx.get(), codec, nullptr);

	working_frame = std::unique_ptr<AVFrame>{ av_frame_alloc() };

	auto [lc, renderer_ptr] = renderer.get_renderer();
	current_frame = std::unique_ptr<SDL_Texture>{ SDL_CreateTexture(renderer_ptr, SDL_PIXELFORMAT_YV12, SDL_TEXTUREACCESS_STATIC, codec_ctx->width, codec_ctx->height) };
	back_buffer = std::unique_ptr<SDL_Texture>{ SDL_CreateTexture(renderer_ptr, SDL_PIXELFORMAT_YV12, SDL_TEXTUREACCESS_STATIC, codec_ctx->width, codec_ctx->height) };
}

void VideoStream::start()
{
	if (!decode_thread.joinable())
	{
		decode_thread = std::jthread([=](std::stop_token st) {
			while (!st.stop_requested())
			{
				if (paused)
				{
					std::unique_lock<std::mutex> lc{ continue_mtx };
					continue_cv.wait(lc);
					continue;
				}

				decode_frame();
			}
		});
	}
}

void VideoStream::stop()
{
	if (decode_thread.joinable())
	{
		decode_thread.request_stop();
		continue_cv.notify_one();
		decode_thread.join();
	}
}

void VideoStream::pause()
{
	paused = true;
}

void VideoStream::unpause()
{
	paused = false;
	continue_cv.notify_one();
}

void VideoStream::decode_frame()
{
	AVPacket packet;
	int frameFinnished = 0;

	if (av_read_frame(format_ctx.get(), &packet) < 0)
	{
		stop();
		return;
	}

	if (packet.stream_index == stream_index)
	{
		decode(codec_ctx.get(), working_frame.get(), &frameFinnished, &packet);

		if (frameFinnished)
		{
			auto [rlc, renderer_ptr] = renderer.get_renderer();
			SDL_UpdateYUVTexture(back_buffer.get(), nullptr, working_frame->data[0], working_frame->linesize[0],
				working_frame->data[1], working_frame->linesize[1], working_frame->data[2], working_frame->linesize[2]);
			rlc.unlock();

			std::chrono::duration<double> frame_timestamp{ working_frame->pts * timebase };
			auto current_time = clock.time();

			if (frame_timestamp > current_time)
			{
				std::this_thread::sleep_for(frame_timestamp - current_time);
			}

			std::lock_guard<std::mutex> lc{ frame_mtx };
			std::swap(back_buffer, current_frame);
		}
	}
}

void sdl_callback(void* ptr, Uint8* stream, int len)
{
	AudioStream* as = reinterpret_cast<AudioStream*>(ptr);

	while (len > 0)
	{
		if (as->paused)
		{
			memset(stream, 0, len);
			return;
		}

		if (as->buffer_index >= as->buffer_size)
		{
			if (!as->decode_frame())
				continue;
		}

		auto data_size = std::min(as->buffer_size - as->buffer_index, len);

		memcpy(stream, as->audio_buffer.data() + as->buffer_index, data_size);
		stream += data_size;
		len -= data_size;
		as->buffer_index += data_size;
	}
}

AudioStream::AudioStream(const std::string& _url, const Clock& _clock)
	: url{ _url }, clock{ _clock }
{
	format_ctx = avformat_open_input(url);
	av_dump_format(format_ctx.get(), 0, url.c_str(), 0);

	stream_index = av_find_best_stream(format_ctx.get(), AVMEDIA_TYPE_AUDIO, -1, -1, nullptr, 0);
	codec_ctx = make_codec_context(format_ctx->streams[stream_index]->codecpar);
	timebase = av_q2d(format_ctx->streams[stream_index]->time_base);

	auto codec = avcodec_find_decoder(codec_ctx->codec_id);

	avcodec_open2(codec_ctx.get(), codec, nullptr);

	working_frame = std::unique_ptr<AVFrame>{ av_frame_alloc() };

	SDL_AudioSpec wanted_spec, spec;
	wanted_spec.freq = codec_ctx->sample_rate;
	wanted_spec.format = AUDIO_F32SYS;
	wanted_spec.channels = codec_ctx->channels;
	wanted_spec.samples = SDL_AUDIO_BUFFER_SIZE;
	wanted_spec.callback = sdl_callback;
	wanted_spec.userdata = this;

	device_id = SDL_OpenAudioDevice(nullptr, 0, &wanted_spec, &spec, SDL_AUDIO_ALLOW_CHANNELS_CHANGE | SDL_AUDIO_ALLOW_FREQUENCY_CHANGE);
	if (device_id < 0)
		throw std::runtime_error("Could not open audio device");

	audio_tgt.fmt = AV_SAMPLE_FMT_FLT;
	audio_tgt.freq = spec.freq;
	audio_tgt.channel_layout = av_get_default_channel_layout(spec.channels);
	audio_tgt.channels = spec.channels;
	audio_tgt.frame_size = av_samples_get_buffer_size(nullptr, spec.channels, 1, audio_tgt.fmt, 1);
	audio_tgt.bytes_per_sec = av_samples_get_buffer_size(nullptr, spec.channels, audio_tgt.freq, audio_tgt.fmt, 1);
	
	audio_src = audio_tgt;

	difference_threshold = static_cast<double>(spec.size) / audio_tgt.bytes_per_sec;
}

void AudioStream::start()
{
	SDL_PauseAudioDevice(device_id, 0);
}

void AudioStream::stop()
{
	SDL_PauseAudioDevice(device_id, 1);
}

void AudioStream::pause()
{
	paused = true;
}

void AudioStream::unpause()
{
	paused = false;
}

int AudioStream::decode_frame()
{
	AVPacket packet;
	int frameFinnished = 0;

	if (av_read_frame(format_ctx.get(), &packet) < 0)
	{
		return -1;
	}

	if (packet.stream_index == stream_index)
	{
		decode(codec_ctx.get(), working_frame.get(), &frameFinnished, &packet);

		if (!frameFinnished) return false;

		auto data_size = av_samples_get_buffer_size(nullptr, codec_ctx->channels,
			working_frame->nb_samples, codec_ctx->sample_fmt, 1);

		auto dec_channel_layout =
			(working_frame->channel_layout && working_frame->channels == av_get_channel_layout_nb_channels(working_frame->channel_layout)) ?
			working_frame->channel_layout : av_get_default_channel_layout(working_frame->channels);

		auto wanted_nb_samples = synchronize(working_frame->nb_samples);

		if (working_frame->format != audio_src.fmt ||
			dec_channel_layout != audio_src.channel_layout ||
			working_frame->sample_rate != audio_src.freq ||
			(wanted_nb_samples != working_frame->nb_samples && !swr_ctx))
		{
			swr_ctx = make_swr_context(nullptr, audio_tgt.channel_layout, audio_tgt.fmt, audio_tgt.freq,
				dec_channel_layout, static_cast<AVSampleFormat>(working_frame->format), working_frame->sample_rate, 0, nullptr);
			audio_src.channel_layout = dec_channel_layout;
			audio_src.channels = working_frame->channels;
			audio_src.freq = working_frame->sample_rate;
			audio_src.fmt = static_cast<AVSampleFormat>(working_frame->format);
		}

		if (swr_ctx)
		{
			const uint8_t** in = const_cast<const uint8_t**>(working_frame->extended_data);
			auto out = audio_buffer.data();
			int out_count = wanted_nb_samples * audio_tgt.freq / working_frame->sample_rate + 256;
			int out_size = av_samples_get_buffer_size(nullptr, audio_tgt.channels, out_count, audio_tgt.fmt, 0);
			if (out_size < 0)
			{
				fprintf(stderr, "av_samples_get_buffer_size() failed\n");
				return -1;
			}
			if (wanted_nb_samples != working_frame->nb_samples)
			{
				if (swr_set_compensation(swr_ctx.get(), (wanted_nb_samples - working_frame->nb_samples) * audio_tgt.freq / working_frame->sample_rate,
					wanted_nb_samples * audio_tgt.freq / working_frame->sample_rate) < 0)
				{
					fprintf(stderr, "swr_get_compensation() failed\n");
					return -1;
				}
			}
			// reallocated new buffer if current too small?
			auto len = swr_convert(swr_ctx.get(), &out, out_count, in, working_frame->nb_samples);
			if (len < 0)
			{
				fprintf(stderr, "swr_convert() failed\n");
				return -1;
			}
			if (len == out_count)
			{
				fprintf(stderr, "audio buffer is probably too small\n");
				if (swr_init(swr_ctx.get()) < 0) // reinit to remove buffered result
					swr_ctx = nullptr;
			}
			//is->audio_buf = out; // same thing
			buffer_size = len * audio_tgt.channels * av_get_bytes_per_sample(audio_tgt.fmt);
		}
		else
		{
			memcpy(audio_buffer.data(), working_frame->data[0], data_size);
			buffer_size = data_size;
		}

		buffer_index = 0;
	}

	return true;
}

int AudioStream::synchronize(int nb_samples)
{
	int wanted_nb_samples = nb_samples;

	int channels = format_ctx->streams[stream_index]->codecpar->channels;
	auto n = 4 * channels; // int or float

	double avg_diff;
	int min_nb_samples, max_nb_samples;

	std::chrono::duration<double> frame_timestamp{ working_frame->pts * timebase };
	auto current_time = clock.time();
	auto diff = std::chrono::duration_cast<std::chrono::duration<double>>(frame_timestamp - current_time).count();

	if (fabs(diff) < AV_NOSYNC_THRESHOLD)
	{
		// accumulate the diffs
		cummulative_difference = diff + average_difference_coef * cummulative_difference;
		if (average_differance_count < AUDIO_DIFF_AVG_NB)
		{
			++average_differance_count;
		}
		else
		{
			avg_diff = cummulative_difference * (1.0 - average_difference_coef);

			// Shrinking/expanding buffer code
			if (fabs(avg_diff) >= difference_threshold)
			{
				wanted_nb_samples = nb_samples + diff * format_ctx->streams[stream_index]->codecpar->sample_rate;
				min_nb_samples = nb_samples * ((100 - SAMPLE_CORRECTION_PERCENT_MAX) / 100.);
				max_nb_samples = nb_samples * ((100 + SAMPLE_CORRECTION_PERCENT_MAX) / 100.);

				wanted_nb_samples = std::clamp(wanted_nb_samples, min_nb_samples, max_nb_samples);
			}
		}
	}
	else
	{
		// difference is Too big; reset diff stuff
		average_differance_count = 0.;
		cummulative_difference = 0.;
	}

	return wanted_nb_samples;
}
