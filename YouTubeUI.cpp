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
		virtual auto display(ActualPixelsRectangle clipping) -> ActualPixelsSize;

	private:
		void Initialize();

		std::vector<HomeTab> tabs;
	};

	class HomeTab : public BasicElement
	{
	public:
		HomeTab(const web::json::object& data);

		virtual auto display(ActualPixelsRectangle clipping) -> ActualPixelsSize;
		auto display_top_navigation(ActualPixelsRectangle clipping) ->ActualPixelsSize;

	private:
		utf8string title;

		std::vector<Shelf> shelfs;
	};


	class Shelf : public BasicElement
	{
	public:
		Shelf(const web::json::object& data);
		virtual auto display(ActualPixelsRectangle clipping)->ActualPixelsSize;

	private:
		void prerender_title();

	private:
		utf8string title_str;
		std::shared_ptr<SDL_Texture> title_texture;
		int title_w, title_h;
		int font_size;
	};
}

auto YouTube::UI::HomeView::display(ActualPixelsRectangle clipping) -> ActualPixelsSize
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
			tabs[0].display(clipping);
			break;
	}

	return ActualPixelsSize();
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

auto YouTube::UI::HomeTab::display(ActualPixelsRectangle clipping) -> ActualPixelsSize
{
	g_Renderer.DrawBox(clipping, { 37, 37, 37 });

	clipping.pos.y += display_top_navigation(clipping).h;

	for (auto& shelf : shelfs)
	{
		shelf.display(clipping);
		clipping.pos.y += 50;
	}

	return ActualPixelsSize();
}

auto YouTube::UI::HomeTab::display_top_navigation(ActualPixelsRectangle clipping) -> ActualPixelsSize
{
	clipping.size.h = 6.5_rem;
	g_Renderer.DrawBox(clipping, { 57,57, 57 });

	return clipping.size;
}

YouTube::UI::MainMenu::MainMenu()
{
	main_content = std::make_unique<HomeView>();
}

YouTube::UI::Shelf::Shelf(const web::json::object& data)
{
	ASSERT(data.begin()->first == U("shelfRenderer"));

	title_str = utility::conversions::to_utf8string(data.at(U("shelfRenderer")).at(U("title")).at(U("runs")).at(0).at(U("text")).as_string());

	prerender_title();
}

auto YouTube::UI::Shelf::display(ActualPixelsRectangle clipping) -> ActualPixelsSize
{
	if (font_size != 1.5_rem)
		prerender_title();

	auto dst = SDL_Rect{ static_cast<int>(clipping.pos.x), static_cast<int>(clipping.pos.y), static_cast<int>(title_w), static_cast<int>(title_h) };
	g_Renderer.Copy(title_texture.get(), nullptr, &dst);
	return ActualPixelsSize();
}

void YouTube::UI::Shelf::prerender_title()
{
	font_size = 1.5_rem;
	title_texture = g_Renderer.RenderTextToNewTexture(title_str, g_FontManager.get_font("Roboto-Regular.ttf", font_size), { 1.f, 1.f, 1.f });
	SDL_QueryTexture(title_texture.get(), nullptr, nullptr, &title_w, &title_h);
}

auto YouTube::UI::MainMenu::display(ActualPixelsRectangle clipping) -> ActualPixelsSize
{
	clipping.pos.x += 8.5_rem;

	main_content->display(clipping);

	return ActualPixelsSize();
}
