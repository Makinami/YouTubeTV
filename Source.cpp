#include "pch.h"

#include "YouTubeCore.h"

#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/sinks/basic_file_sink.h>

#include <SDL2/SDL.h>
#include <SDL2/SDL_image.h>

#include "YouTubeVideo.h"

#define USERAGENT "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/81.0.4044.0 Safari/537.36 Edg/81.0.416.3"

#include <cpprest/http_client.h>

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
#include "TextRenderer.h"
#include "FontManager.h"

using namespace std::chrono_literals;
using namespace std::string_literals;

using namespace YouTube;

#undef main

int main(int argc, char *argv[])
{
	/*pplx::cancellation_token_source ctx;

	auto task = pplx::task<void>([] {
		std::cout << "one\n";
	}, ctx.get_token()).then([&] {
		std::cout << "two\n";
		ctx.cancel();
	}).then([] {
		std::cout << "three\n";
	});

	ctx.cancel();

	ctx = {};

	task = pplx::task<void>([] {
		std::cout << "one\n";
	}, ctx.get_token()).then([&] {
		std::cout << "two\n";
		ctx.cancel();
	}).then([] {
		std::cout << "three\n";
	});

	task.wait();

	return 0;*/

	{
		std::vector<spdlog::sink_ptr> sinks;
		sinks.emplace_back([]() {
			auto sink = std::make_unique<spdlog::sinks::stdout_color_sink_mt>();
			sink->set_level(spdlog::level::debug);
			return sink;
		}());
		sinks.emplace_back([]() {
			auto sink = std::make_shared<spdlog::sinks::basic_file_sink_mt>("logs/last_run.log", true);
			sink->set_level(spdlog::level::trace);
			return sink;
		}());
		auto logger = std::make_unique<spdlog::logger>("", std::begin(sinks), std::end(sinks));
		spdlog::set_default_logger(std::move(logger));
		spdlog::set_level(spdlog::level::trace);
	}

	av_log_set_level(AV_LOG_VERBOSE);

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
					// Needs to destroy all textures using SDL_TEXTUREACCESS_TARGET which TextRenderer ahs plentiful
					// https://forums.libsdl.org/viewtopic.php?p=40894
					g_TextRenderer.ClearAll();
					g_Renderer.UpdateSize();
					break;
				}
				break;
			case SDL_KEYDOWN:
				for (auto it = g_KeyboardCallbacks.rbegin(); it != g_KeyboardCallbacks.rend(); ++it)
				{
					if (it->operator()(event.key))
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

		g_KeyboardCallbacks.clear();

		g_RendererQueue.execute_one(g_Renderer);

		g_Renderer.Clear();

		if (g_PlayingVideo)
		{
			g_KeyboardCallbacks.emplace_back([](SDL_KeyboardEvent event) {
				if (event.keysym.sym == SDLK_ESCAPE)
				{
					g_PlayingVideo = nullptr;
					return true;
				}
				return false;
			});

			// display video here
			auto [flc, frame_ptr] = g_PlayingVideo->get_video_frame();
			auto [rlc, renderer_ptr] = g_Renderer.get_renderer();
			auto [width, height, sar] = g_PlayingVideo->get_video_size();
			auto rect = calculate_projection_rect(g_Renderer.GetSize().actual_width, g_Renderer.GetSize().actual_height, width, height);
			SDL_RenderCopy(renderer_ptr, frame_ptr, nullptr, &rect);
			flc.unlock();
		}
		else
		{
			auto dim = g_Renderer.GetSize();
			main_menu.display({{0, 0}, {dim.actual_width, dim.actual_height}});
		}
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