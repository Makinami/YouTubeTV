#include "pch.h"

#include "YouTubeCore.h"

#include <SDL2/SDL.h>
#include <SDL2/SDL_image.h>

#include "YouTubeVideo.h"

#define USERAGENT "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/81.0.4044.0 Safari/537.36 Edg/81.0.416.3"

#include <cpprest/http_client.h>

#include <nlohmann/json.hpp>

#if defined(_WIN32) || defined(_WIN64)
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#endif

#include <regex>
#include <variant>

#include "ImageManager.h"
#include "YouTubeAPI.h"
#include "YouTubeUI.h"

using namespace std::chrono_literals;
using namespace std::string_literals;

using namespace YouTube;

#undef main

SDL_Rect calculate_display_rect(int scr_width, int scr_height,
								int dst_width, int dst_height, AVRational pic_sar)
{
	auto aspect_ratio = av_make_q(dst_width, dst_height);

	/* XXX: we suppose the screen has a 1.0 pixel ratio */
	auto height = scr_height;
	auto width = int{av_rescale(height, aspect_ratio.num, aspect_ratio.den) & ~1};
	if (width > scr_width)
	{
		width = scr_width;
		height = av_rescale(width, aspect_ratio.den, aspect_ratio.num) & ~1;
	}
	auto x = (scr_width - width) / 2;
	auto y = (scr_height - height) / 2;
	return {x, y, std::max(width, 1), std::max(height, 1)};
}

int main(int argc, char *argv[])
{
	YouTube::YouTubeCoreRAII yt_core;

	YouTube::UI::MainMenu main_menu;

	SDL_Event event;
	while (true)
	{
		if (SDL_PollEvent(&event))
		{
			switch (event.type)
			{
			case SDL_WINDOWEVENT:
				switch (event.window.event)
				{
				case SDL_WINDOWEVENT_SIZE_CHANGED:
					g_Renderer.UpdateSize();
					break;
				}
				break;
			case SDL_QUIT:
				return 0;
				break;
			default:
				break;
			}
		}

		g_Renderer.Clear();
		auto dim = g_Renderer.GetSize();
		main_menu.display({{0, 0}, {dim.actual_width, dim.actual_height}});
		g_Renderer.Present();
	}

	return 0;

	//YouTubeVideo media("B084zv59WwA", renderer);

	//bool paused = false;
	//media.start();

	//SDL_Event event;
	//while (true)
	//{
	//	{
	//		// won't deadlock 'cause this is the only thread that needs both at the same time
	//		auto [flc, frame_ptr] = media.get_video_frame();
	//		auto [rlc, renderer_ptr] = renderer.get_renderer();
	//		auto [width, height, sar] = media.get_video_size();
	//		int wnd_width, wnd_height;
	//		SDL_GetWindowSize(window.get(), &wnd_width, &wnd_height);
	//		auto rect = calculate_display_rect(wnd_width, wnd_height, width, height, sar);
	//		SDL_RenderCopy(renderer_ptr, frame_ptr, nullptr, &rect);
	//		flc.unlock();

	//		SDL_RenderPresent(renderer_ptr);
	//	}

	//	if (SDL_PollEvent(&event) == 0)
	//		continue;
	//	switch (event.type)
	//	{
	//	case SDL_KEYDOWN:
	//		switch (event.key.keysym.sym)
	//		{
	//		case SDLK_SPACE:
	//			if (paused)
	//			{
	//				media.unpause();
	//				paused = false;
	//			}
	//			else
	//			{
	//				media.pause();
	//				paused = true;
	//			}
	//			break;
	//		case SDLK_RIGHT:
	//		{
	//			auto new_time = media.get_time() + std::chrono::duration<double>{5};
	//			media.seek(new_time);
	//			break;
	//		}
	//		case SDLK_LEFT:
	//		{
	//			auto new_time = media.get_time() - std::chrono::duration<double>{5};
	//			media.seek(new_time);
	//			break;
	//		}
	//		}
	//		break;
	//	case SDL_QUIT:
	//		SDL_Quit();
	//		return 0;
	//		break;
	//	default:
	//		break;
	//	}
	//}

	return 0;
}