extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libswscale/swscale.h>
#include <libavutil/imgutils.h>
}

#pragma comment(lib, "avcodec.lib")
#pragma comment(lib, "avformat.lib")
#pragma comment(lib, "swscale.lib")
#pragma comment(lib, "avutil.lib")

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

class GuardedRenderer
{
public:
	GuardedRenderer(SDL_Window* window)
	{
		renderer = std::unique_ptr<SDL_Renderer>(SDL_CreateRenderer(window, -1, 0));
	}
	auto get_renderer()
	{
		return std::tuple<std::unique_lock<std::mutex>, SDL_Renderer*>(renderer_mtx, renderer.get());
	}
private:
	std::unique_ptr<SDL_Renderer> renderer;
	std::mutex renderer_mtx;
};

class Clock
{
public:
	auto time() const
	{
		return std::chrono::steady_clock::now() - start;
	}
	//virtual double pause() = 0;
private:
	std::chrono::steady_clock::time_point start = std::chrono::steady_clock::now();
};

class VideoStream
{
public:
	VideoStream(const std::string& _url, GuardedRenderer& _renderer, const Clock& _clock);
	~VideoStream() { stop(); };
	void start();
	void stop();

	auto get_frame()
	{
		return std::tuple<std::unique_lock<std::mutex>, SDL_Texture*>(frame_mtx, current_frame.get());
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
	//const Clock& clock;
};

void SaveFrame(AVFrame* pFrame, int width, int height, int iFrame) {
	FILE* pFile;
	char szFilename[32];
	int  y;

	// Open file
	sprintf_s(szFilename, "frame%d.ppm", iFrame);
	fopen_s(&pFile, szFilename, "wb");
	if (pFile == NULL)
		return;

	// Write header
	fprintf(pFile, "P6\n%d %d\n255\n", width, height);

	// Write pixel data
	for (y = 0; y < height; y++)
		fwrite(pFrame->data[0] + y * pFrame->linesize[0], 1, width * 3, pFile);

	// Close file
	fclose(pFile);
}

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

	vs.start();

	SDL_Event event;
	while (true)
	{
		{
			// won't lock 'cause this is the only thread that needs both at the same time
			auto [flc, frame_ptr] = vs.get_frame();
			auto [rlc, renderer_ptr] = renderer.get_renderer();
			SDL_RenderCopy(renderer_ptr, frame_ptr, nullptr, nullptr);
			flc.unlock();

			SDL_RenderPresent(renderer_ptr);
		}

		SDL_PollEvent(&event);
		switch (event.type)
		{
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
