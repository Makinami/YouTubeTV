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

using namespace std::chrono_literals;
using namespace std::string_literals;

using namespace YouTube;

#undef main

SDL_Rect calculate_display_rect(int scr_width, int scr_height,
								int dst_width, int dst_height, AVRational pic_sar)
{
	AVRational aspect_ratio = pic_sar;
	int width, height, x, y;

	if (av_cmp_q(aspect_ratio, av_make_q(0, 1)) <= 0)
		aspect_ratio = av_make_q(1, 1);

	aspect_ratio = av_mul_q(aspect_ratio, av_make_q(dst_width, dst_height));

	/* XXX: we suppose the screen has a 1.0 pixel ratio */
	height = scr_height;
	width = av_rescale(height, aspect_ratio.num, aspect_ratio.den) & ~1;
	if (width > scr_width)
	{
		width = scr_width;
		height = av_rescale(width, aspect_ratio.den, aspect_ratio.num) & ~1;
	}
	x = (scr_width - width) / 2;
	y = (scr_height - height) / 2;
	return {x, y, std::max(width, 1), std::max(height, 1)};
}

auto homepage_data(web::http::client::http_client client)
{
	auto request = browser_request();
	request.set_method(web::http::methods::GET);
	request.set_request_uri(U("/"));

	Concurrency::streams::stringstreambuf resultContainer;
	return client.request(request).then([=](web::http::http_response response) {
		return response.body().read_to_end(resultContainer);
	}).then([=](int /* bytes read */) {
		std::smatch matches;
		std::basic_regex reg(R"=(window\["ytInitialData"\].=.(.*);)=");

		if (std::regex_search(resultContainer.collection(), matches, reg))
		{
			return nlohmann::json::parse(matches[1].first, matches[1].second);
		}
		else
		{
			return nlohmann::json();
		}
	});
}

class MediaItem
{
public:
	enum class Type { tvMusicVideoRenderer, gridPlaylistRenderer, unknownRenderer };

private:
	static const std::unordered_map<std::string, Type> type_mapping;

public:
	MediaItem(const nlohmann::json& data);

	std::string thumbnail_url() const;
	auto thumbnail() const -> decltype(std::declval<ImageManager>().load_image(std::declval<std::string>()));

private:
	std::string video_id;
	std::string playlist_id;
	Type type;
};

const std::unordered_map<std::string, MediaItem::Type> MediaItem::type_mapping {
	{ "tvMusicVideoRenderer", Type::tvMusicVideoRenderer },
	{ "gridPlaylistRenderer", Type::gridPlaylistRenderer },
	{ "unknownRenderer", Type::unknownRenderer }
};

ImageManager* img_mgr_ptr;

int main(int argc, char *argv[])
{
	YouTube::YouTubeCoreRAII yt_core;

	SDL_Event event;
	while (true)
	{
		auto [rlc, renderer_ptr] = g_Renderer.get_renderer();
		if (auto img = g_ImageManager.get_image("https://i.ytimg.com/vi/iR1Ol6TOggk/hq720.jpg"); img.is_done())
		{
			// won't deadlock 'cause this is the only thread that needs both at the same time
			SDL_Rect rect = { 50, 50, 490, 275 };
			SDL_RenderCopy(renderer_ptr, img.get().get(), nullptr, &rect);

		}
		SDL_RenderPresent(renderer_ptr);

		if (SDL_PollEvent(&event) == 0)
			continue;
		switch (event.type)
		{
			case SDL_QUIT:
				return 0;
				break;
			default:
				break;
		}
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

MediaItem::MediaItem(const nlohmann::json& data)
{
	if (auto it = type_mapping.find(data.begin().key()); it != type_mapping.end())
		type = it->second;
	else
		type = type_mapping.at("unknownRenderer");

	auto& watchEndpoint = data.begin().value()["navigationEndpoint"]["watchEndpoint"];
	watchEndpoint["videoId"].get_to(video_id);
	watchEndpoint["playlistId"].get_to(playlist_id);

	img_mgr_ptr->load_image(thumbnail_url());
}

std::string MediaItem::thumbnail_url() const
{
	return "https://i.ytimg.com/vi/"s + video_id + "/hqdefault.jpg";
}

auto MediaItem::thumbnail() const -> decltype(std::declval<ImageManager>().load_image(std::declval<std::string>()))
{
	return img_mgr_ptr->get_image(thumbnail_url());
}
