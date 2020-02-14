#pragma once

#include <memory>

extern "C" {
#include <libavformat/avformat.h>
#include <libswresample/swresample.h>
}

#include <SDL2/SDL.h>

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

	template<> struct default_delete<SDL_Surface> {
		void operator()(SDL_Surface* ptr)
		{
			SDL_FreeSurface(ptr);
		}
	};

	template<> struct default_delete<SwrContext> {
		void operator()(SwrContext* ptr)
		{
			swr_free(&ptr);
		}
	};
}