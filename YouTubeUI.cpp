#include "pch.h"
#include "YouTubeUI.h"

#include <cpprest/json.h>

#include "YouTubeCore.h"
#include "Renderer.h"
#include "YouTubeAPI.h"
#include "FontManager.h"

using namespace Renderer::Dimensions;

namespace YouTube::UI
{
	class HomeTab;
	class Shelf;

	class HomeView : public BasicElement
	{
	public:
		virtual auto display(Position origin) -> Dimensions;

	private:
		void Initialize();

		std::vector<HomeTab> tabs;
	};

	class HomeTab : public BasicElement
	{
	public:
		HomeTab(const web::json::object& data);

		virtual auto display(Position origin) -> Dimensions;

	private:
		utf8string title;

		std::vector<Shelf> shelfs;
	};


	class Shelf : public BasicElement
	{
	public:
		Shelf(const web::json::object& data);
		virtual auto display(Position origin)->Dimensions;

	private:
		utf8string title_str;
		std::shared_ptr<SDL_Texture> title_texture;
		int title_w, title_h;
	};
}

auto YouTube::UI::HomeView::display(Position origin) -> Dimensions
{
	switch (state)
	{
		case YouTube::UI::BasicElement::State::Uninitialized:
			Initialize();
			break;
		case YouTube::UI::BasicElement::State::Loading:
			/* display placeholder */
			break;
		case YouTube::UI::BasicElement::State::Loaded:
			tabs[0].display(origin);
			break;
	}

	return Dimensions();
}

void YouTube::UI::HomeView::Initialize()
{
	loading_task = g_API.get(U("default")).then([this](web::json::value data) {
		auto tabs_data = data.at(U("contents")).at(U("tvBrowseRenderer")).at(U("content"))
			.at(U("tvSecondaryNavRenderer")).at(U("sections")).as_array()[0]
			.at(U("tvSecondaryNavSectionRenderer")).at(U("tabs")).as_array();

		for (const auto& tab_data : tabs_data)
		{
			tabs.emplace_back(tab_data.as_object());
			break;
		}

		state = State::Loaded;
	});
	state = State::Loading;
}

YouTube::UI::HomeTab::HomeTab(const web::json::object& data)
{
	ASSERT(data.begin()->first == U("tabRenderer"), "Home tab data is not defined by tabRenderer object");
	title = utility::conversions::to_utf8string(data.at(U("tabRenderer")).at(U("title")).as_string());

	auto sections_data = data.at(U("tabRenderer")).at(U("content"))
		.at(U("tvSurfaceContentRenderer")).at(U("content")).at(U("sectionListRenderer"))
		.at(U("contents")).as_array();

	for (const auto& section_data : sections_data)
	{
		shelfs.emplace_back(section_data.as_object());
	}
}

auto YouTube::UI::HomeTab::display(Position origin) -> Dimensions
{
	for (auto& shelf : shelfs)
	{
		shelf.display(origin);
		origin.y += 50;
	}

	return Dimensions();
}

YouTube::UI::MainMenu::MainMenu()
{
	main_content = std::make_unique<HomeView>();
}

YouTube::UI::Shelf::Shelf(const web::json::object& data)
{
	ASSERT(data.begin()->first == U("shelfRenderer"));

	title_str = utility::conversions::to_utf8string(data.at(U("shelfRenderer")).at(U("title")).at(U("runs")).at(0).at(U("text")).as_string());

	title_texture = g_Renderer.RenderTextToNewTexture(title_str, g_FontManager.get_font("Roboto-Regular.ttf", 27), { 1.f, 1.f, 1.f });
	TTF_SizeUTF8(g_FontManager.get_font("Roboto-Regular.ttf", 27), title_str.c_str(), &title_w, &title_h);
}

auto YouTube::UI::Shelf::display(Position origin) -> Dimensions
{
	auto dst = SDL_Rect{ static_cast<int>(origin.x), static_cast<int>(origin.y), title_w, title_h };
	g_Renderer.Copy(title_texture.get(), nullptr, &dst);
	return Dimensions();
}

auto YouTube::UI::MainMenu::display(Position origin) -> Dimensions
{
	origin.x += g_Renderer.GetSize().scaled_width * 0.1;

	ScaledPercentageRectangle rect_scaled;
	rect_scaled.pos.x = 0.1;
	rect_scaled.pos.y = 0;
	auto rect = ActualPercentageRectangle{ rect_scaled };
	rect.size.w = 1.0;
	rect.size.h = 1.0;

	g_Renderer.DrawBox(rect, { 47, 47, 47 });

	main_content->display(origin);

	return Dimensions();
}
