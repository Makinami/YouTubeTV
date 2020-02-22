#include "pch.h"

#include "ImageManager.h"

#include "YouTubeCore.h"

using namespace YouTube;
using namespace std::string_literals;

pplx::task<std::shared_ptr<SDL_Texture>> ImageManager::get_image(const utility::string_t& url)
{
	if (auto it = images.find(url); it != images.end())
		return it->second;
	else
	{
		load_image(url);
		return get_image(url);
	}
}

void ImageManager::load_image(const utility::string_t& url)
{
	auto [domain, uri] = parse_url(url);

	auto lc = std::scoped_lock{ map_write };
	if (auto it = images.find(url); it != images.end()) return;

	auto request = browser_request();
	request.set_request_uri(uri);

	images.insert({ url, get_client(domain).request(request).then([=](web::http::http_response response) {
		return response.extract_vector();
	}).then([=](const std::vector<unsigned char>& data) {
		auto reader = SDL_RWFromConstMem(data.data(), data.size());
		return std::shared_ptr<SDL_Texture>(g_Renderer.LoadTexture(reader));
	}) });
}

std::pair<utility::string_t, utility::string_t> ImageManager::parse_url(const utility::string_t& url)
{
	static std::regex reg(R"=((https?://[^\/]+)(\/?.*))=");
	std::match_results<utility::string_t::const_iterator> matches;
	if (!std::regex_match(url.begin(), url.end(), matches, reg))
		return { U(""), U("") };

	return std::pair<utility::string_t, utility::string_t>{
		matches[1],
		matches[2] != U("") ? utility::string_t{ matches[2] }  : U("/")
	};
}

web::http::client::http_client ImageManager::get_client(utility::string_t domain)
{
	if (auto it = clients.find(domain); it != clients.end())
		return it->second;

	return clients.insert({ domain, web::http::client::http_client{domain} }).first->second;
}
