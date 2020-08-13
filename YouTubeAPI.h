#pragma once

#include <regex>
#include <optional>

#include <cpprest/http_client.h>
#include <nlohmann/json.hpp>

#include <spdlog/spdlog.h>

using namespace std::string_literals;

class YouTubeAPI
{
public:
	auto get(utility::string_t browseId, pplx::cancellation_token token = pplx::cancellation_token::none()) -> pplx::task<nlohmann::json>
	{
		return client.request(browse_request(browseId), token).then([](web::http::http_response response) {
			return nlohmann::json::parse(response.extract_utf8string().get());
		});
	}

private:
	auto browser_request() -> web::http::http_request
	{
		auto request = web::http::http_request();
		request.headers().add(web::http::header_names::user_agent, "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/81.0.4044.0 Safari/537.36 Edg/81.0.416.3");
		return request;
	}

	auto browse_request(utility::string_t browseId) -> web::http::http_request
	{
		auto request = browser_request();
		request.set_request_uri(U("/youtubei/v1/browse?key=AIzaSyDCU8hByM-4DrUqRUYnGn-3llEO78bcxq8"));
		request.set_method(web::http::methods::POST);
		request.set_body(web::json::value::object({
			{ U("context"), context_json() },
			{ U("browseId"), web::json::value(browseId) }
		}));
		return request;
	}

	auto home_request() -> web::http::http_request
	{
		auto request = browser_request();
		request.set_request_uri(U("/youtubei/v1/browse?key=AIzaSyDCU8hByM-4DrUqRUYnGn-3llEO78bcxq8"));
		request.set_method(web::http::methods::POST);
		request.set_body(web::json::value::object({
			{ U("context"), context_json() },
			{ U("browseId"), web::json::value(U("default")) }
			}));
		return request;
	}

	auto context_json() -> web::json::value
	{
		return web::json::value::object({
			{ U("client"), client_json() }
		});
	}

	auto client_json() -> web::json::value
	{
		return web::json::value::object({
			{ U("clientName"), web::json::value(clientName) },
			{ U("clientVersion"), web::json::value(clientVersion) },
			{ U("acceptRegion"), web::json::value(acceptRegion) },
			{ U("acceptLanguage"), web::json::value(acceptLanguage) }
		});
	}

private:
	web::http::client::http_client client{ U("https://www.youtube.com") };

	utility::string_t clientName = U("TVHTML5");
	utility::string_t clientVersion = U("6.20180913");
	utility::string_t acceptRegion = U("GB");
	utility::string_t acceptLanguage = U("en-GB");
};

