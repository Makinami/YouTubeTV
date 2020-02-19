#include "pch.h"

#include "ImageManager.h"

#include "YouTubeCore.h"

using namespace YouTube;
using namespace std::string_literals;

pplx::task<std::shared_ptr<SDL_Texture>> ImageManager::get_image(const std::string& url)
{
	if (auto it = images.find(url); it != images.end())
		return it->second;
	else
	{
		load_image(url);
		return get_image(url);
	}
}

void ImageManager::load_image(const std::string& url)
{
	auto [domain, uri] = parse_url(url);

	auto lc = std::scoped_lock{ map_write };
	if (auto it = images.find(url); it != images.end()) return;

	auto request = browser_request();
	request.set_request_uri(str_to_wstr(uri));

	images.insert({ url, get_client(domain).request(request).then([=](web::http::http_response response) {
		return response.extract_vector();
	}).then([=](const std::vector<unsigned char>& data) {
		auto reader = SDL_RWFromConstMem(data.data(), data.size());
		return std::shared_ptr<SDL_Texture>(g_Renderer.LoadTexture(reader));
	}) });
}

std::pair<std::string, std::string> ImageManager::parse_url(const std::string& url)
{
	static std::regex reg(R"=((https?://[^\/]+)(\/?.*))=");
	std::smatch matches;
	if (!std::regex_match(url, matches, reg))
		return { "", "" };

	return std::pair<std::string, std::string>{
		matches[1],
		matches[2] != "" ? std::string{ matches[2] } : "/"s
	};
}

web::http::client::http_client ImageManager::get_client(std::string domain)
{
	if (auto it = clients.find(domain); it != clients.end())
		return it->second;

	return clients.insert({ domain, web::http::client::http_client(str_to_wstr(domain)) }).first->second;
}