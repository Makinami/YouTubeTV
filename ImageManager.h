#pragma once

#include <unordered_map>
#include <string>

#include <SDL2/SDL.h>
#include <SDL2/SDL_image.h>

#include <cpprest/http_client.h>

#include "Deleters.h"
#include "Renderer.h"

inline auto browser_request()
{
	auto request = web::http::http_request();
	request.headers().add(web::http::header_names::user_agent, "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/81.0.4044.0 Safari/537.36 Edg/81.0.416.3");
	return request;
}

inline std::wstring str_to_wstr(const std::string& str)
{
	return { str.begin(), str.end() };
}

class ImageManager
{
	using img_ptr = std::shared_ptr<SDL_Texture>;
	using img_task = pplx::task<img_ptr>;

public:
	ImageManager(const GuardedRenderer& _renderer);
	img_task get_image(const std::string& url);
	img_task load_image(const std::string& url);

private:
	std::pair<std::string, std::string> parse_url(const std::string& url);
	web::http::client::http_client get_client(std::string domain);

private:
	const GuardedRenderer& renderer;
	std::unordered_map<std::string, img_task> images;
	std::unordered_map<std::string, web::http::client::http_client> clients;

	std::mutex map_write;
};

