#pragma once

#include <regex>
#include <optional>

#include <cpprest/http_client.h>
#include <nlohmann/json.hpp>

class YouTubeAPI
{
public:
	auto get_home_data()
	{
		return pplx::task<std::optional<nlohmann::json>>([this]() -> std::optional<nlohmann::json> {
			auto request = browser_request();
			request.set_method(web::http::methods::GET);
			request.set_request_uri(U("/"));

			Concurrency::streams::stringstreambuf resultContainer;
			try {
				return client.request(request).then([=](web::http::http_response response) {
					return response.body().read_to_end(resultContainer);
				}).then([=](int /* bytes read */) {
					std::smatch matches;
					std::basic_regex reg(R"=(window\["ytInitialData"\].=.(.*);)=");

					if (std::regex_search(resultContainer.collection(), matches, reg))
						return nlohmann::json::parse(matches[1].first, matches[1].second);
					else
						throw std::runtime_error("Data not found");
				}).get();
			}
			catch (const std::exception& err) {
				std::cerr << err.what();
				return {};
			}
		});
	}

private:
	auto browser_request() -> web::http::http_request
	{
		auto request = web::http::http_request();
		request.headers().add(web::http::header_names::user_agent, "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/81.0.4044.0 Safari/537.36 Edg/81.0.416.3");
		return request;
	}

private:
	web::http::client::http_client client{ U("https://www.youtube.com") };
};

