#include "pch.h"
#include "YouTubeUI.h"

#include "YouTubeCore.h"
#include "Renderer.h"
#include "YouTubeAPI.h"

#include <cpprest/json.h>

namespace YouTube::UI
{

	class HomeTab;

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
		virtual auto display(Position origin) -> Dimensions;
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
			/* display content */
			break;
	}

	return Dimensions();
}

void YouTube::UI::HomeView::Initialize()
{
	loading_task = g_API.get(U("FFtopics")).then([](web::json::value data) {

	});
	state = State::Loading;
}

auto YouTube::UI::HomeTab::display(Position origin) -> Dimensions
{
	return Dimensions();
}

YouTube::UI::MainMenu::MainMenu()
{
	main_content = std::make_unique<HomeView>();
}

auto YouTube::UI::MainMenu::display(Position origin) -> Dimensions
{
	origin.x += g_Renderer.GetSize().scaled_width * 0.1;

	auto render_size = g_Renderer.GetSize();

	g_Renderer.DrawBox({ origin.x, origin.y, render_size.actual_width - origin.x, render_size.actual_height - origin.y }, { 47, 47, 47 });

	main_content->display(origin);

	return Dimensions();
}
