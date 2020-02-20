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
	auto width = int{ av_rescale(height, aspect_ratio.num, aspect_ratio.den) & ~1 };
	if (width > scr_width)
	{
		width = scr_width;
		height = av_rescale(width, aspect_ratio.den, aspect_ratio.num) & ~1;
	}
	auto x = (scr_width - width) / 2;
	auto y = (scr_height - height) / 2;
	return {x, y, std::max(width, 1), std::max(height, 1)};
}

class MediaItem
{
public:
	enum class Type { tvMusicVideoRenderer, gridPlaylistRenderer, unknownRenderer };

private:
	static const std::unordered_map<utility::string_t, Type> type_mapping;

public:
	MediaItem(const web::json::object& data);

	std::string thumbnail_url() const;
	auto thumbnail() const -> decltype(std::declval<ImageManager>().get_image(std::declval<std::string>()));

private:
	std::string video_id;
	std::string playlist_id;
	Type type;
};

const std::unordered_map<utility::string_t, MediaItem::Type> MediaItem::type_mapping {
	{ U("tvMusicVideoRenderer"), Type::tvMusicVideoRenderer },
	{ U("gridPlaylistRenderer"), Type::gridPlaylistRenderer },
	{ U("unknownRenderer"), Type::unknownRenderer }
};

ImageManager* img_mgr_ptr;

int main(int argc, char *argv[])
{
	YouTube::YouTubeCoreRAII yt_core;

	std::vector<MediaItem> videos;
	g_API.get_home_data().then([&](const std::optional<web::json::value>& data) {
		if (data)
		{
			auto sections = data->at(U("contents")).at(U("tvBrowseRenderer")).at(U("content")).at(U("tvSecondaryNavRenderer")).at(U("sections")).as_array();

			auto section = sections[0];

			auto tabs = section.at(U("tvSecondaryNavSectionRenderer")).at(U("tabs")).as_array();

			auto tab = tabs[0];

			auto lists = tab.at(U("tabRenderer")).at(U("content")).at(U("tvSurfaceContentRenderer")).at(U("content")).at(U("sectionListRenderer")).at(U("contents")).as_array();

			auto list = lists[0];

			auto items = list.at(U("shelfRenderer")).at(U("content")).at(U("horizontalListRenderer")).at(U("items")).as_array();

			for (const auto& item : items)
			{
				try {
					videos.emplace_back(item.as_object());
				}
				catch (const web::json::json_exception & err) {
					std::cerr << err.what();
				}
			}
		}
	});

	YouTube::UI::MainMenu main_menu;

	SDL_Event event;
	while (true)
	{
		if (SDL_PollEvent(&event))
		{
			switch (event.type)
			{
				case SDL_WINDOWEVENT:
					switch (event.window.event) {
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
		main_menu.display({ 0, 0 });
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

MediaItem::MediaItem(const web::json::object& data)
{
	if (auto it = type_mapping.find(data.begin()->first); it != type_mapping.end())
		type = it->second;
	else
		type = type_mapping.at(U("unknownRenderer"));

	auto& watchEndpoint = data.begin()->second.at(U("navigationEndpoint")).at(U("watchEndpoint")).as_object();
	video_id = wstr_to_str(watchEndpoint.at(U("videoId")).as_string());

	g_ImageManager.load_image(thumbnail_url());
}

std::string MediaItem::thumbnail_url() const
{
	return "https://i.ytimg.com/vi/"s + video_id + "/hqdefault.jpg";
}

auto MediaItem::thumbnail() const -> decltype(std::declval<ImageManager>().get_image(std::declval<std::string>()))
{
	return g_ImageManager.get_image(thumbnail_url());
}
