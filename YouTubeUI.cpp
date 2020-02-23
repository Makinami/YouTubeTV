#include "pch.h"
#include "YouTubeUI.h"

#include <variant>
#include <utility>
#include <algorithm>
#include <cpprest/json.h>

#include "YouTubeCore.h"
#include "Renderer.h"
#include "YouTubeAPI.h"
#include "FontManager.h"
#include "ImageManager.h"

using namespace Renderer::Dimensions;

namespace YouTube::UI
{
	class HomeTab;
	class Shelf;
	class MediaItem;

	class Text : public BasicElement
	{
	public:
		Text() = default;
		Text(utility::string_t text, Rem _font_size);
		Text(const Text&) = default;
		Text& operator=(const Text&) = default;
		Text(Text&& other) = default;
		Text& operator=(Text&& other) = default;

		virtual auto display(ActualPixelsRectangle clipping)->ActualPixelsSize;

	private:
		void render();

	private:
		utf8string text_str;
		Rem font_size{ 1 };

		std::shared_ptr<SDL_Texture> title_texture;
		ActualPixelsSize size;
		int current_font_size{ -1 };
	};

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

		HomeTab() = default;
		HomeTab(const HomeTab&) = delete;
		HomeTab& operator = (const HomeTab&) = delete;
		HomeTab(HomeTab&&) = default;
		HomeTab& operator = (HomeTab&&) = default;

		virtual auto display(ActualPixelsRectangle clipping) -> ActualPixelsSize;
		auto display_top_navigation(ActualPixelsRectangle clipping) ->ActualPixelsSize;

	private:
		utf8string title;

		std::vector<Shelf> shelfs;
	};

	struct NonCopyable
	{
		NonCopyable() = default;
		NonCopyable(const NonCopyable&) = delete;
		NonCopyable& operator = (const NonCopyable&) = delete;
		NonCopyable(NonCopyable&&) = default;
		NonCopyable& operator = (NonCopyable&&) = default;
	};

	class Shelf : public BasicElement
	{
	public:
		Shelf(const web::json::object& data);

		Shelf() = default;
		Shelf(const Shelf&) = delete;
		Shelf& operator = (const Shelf&) = delete;
		Shelf(Shelf&&) = default;
		Shelf& operator = (Shelf&&) = default;
		
		virtual auto display(ActualPixelsRectangle clipping)->ActualPixelsSize;

	private:
		Text title;
		std::vector<std::unique_ptr<MediaItem>> items;
	};


	class MediaItem : public BasicElement
	{
	public:
		enum class Type { MusicVideo, Video };

	private:
		static const std::unordered_map<utility::string_t, Type> type_mapping;

	protected:
		MediaItem(const web::json::object& data);

	public:
		MediaItem() = default;
		MediaItem(const MediaItem&) = delete;
		MediaItem& operator = (const MediaItem&) = delete;
		MediaItem(MediaItem&&) = default;
		MediaItem& operator = (MediaItem&&) = default;

		static auto create(const web::json::object& data) -> std::unique_ptr<MediaItem>;

		virtual auto display(ActualPixelsRectangle clipping)->ActualPixelsSize;

	protected:
		Type type;
		Text title;
		Text secondary;
		std::variant<ImageManager::img_ptr, pplx::task<void>> thumbnail;
	};

	const std::unordered_map<utility::string_t, MediaItem::Type> MediaItem::type_mapping{
		{ U("tvMusicVideoRenderer"), Type::MusicVideo },
		{ U("gridVideoRenderer"), Type::Video }
	};

	class MusicVideo : public MediaItem
	{
	public:
		MusicVideo(const web::json::object& data);

		//virtual auto display(ActualPixelsRectangle clipping)->ActualPixelsSize;

	private:
		Text length_text;
	};

	class Video : public MediaItem
	{
	public:
		Video(const web::json::object& data);

		//virtual auto display(ActualPixelsRectangle clipping)->ActualPixelsSize;
	private:
		Text length_text;
	};
}

auto build_text(const web::json::object& data) -> utility::string_t
{
	ASSERT(data.find(U("runs")) != data.end() || !data.at(U("runs")).is_array(), "JSON object passed to text builder doesn't contain 'runs' array.");
	
	auto runs = data.at(U("runs")).as_array();
	return std::accumulate(runs.begin(), runs.end(), utility::string_t{}, [](auto prev, auto curr) {
		return prev + curr.at(U("text")).as_string();
	});
};

auto build_text(const web::json::value& data) -> utility::string_t
{
	ASSERT(data.is_object(), "JSON value passed to text builder is not an object");
	return build_text(data.as_object());
}


YouTube::UI::Text::Text(utility::string_t text, Rem _font_size)
	: font_size{ _font_size }
{
	text_str = utility::conversions::to_utf8string(std::move(text));
	render();
}

auto YouTube::UI::Text::display(ActualPixelsRectangle clipping) -> ActualPixelsSize
{
	if (static_cast<int>(font_size) != current_font_size)
		render();

	auto dst = SDL_Rect{ static_cast<int>(clipping.pos.x), static_cast<int>(clipping.pos.y), static_cast<int>(size.w), static_cast<int>(size.h) };
	g_Renderer.CopyTexture(title_texture.get(), nullptr, &dst);

	return ActualPixelsSize();
}

void YouTube::UI::Text::render()
{
	title_texture = g_Renderer.RenderTextToNewTexture(text_str, g_FontManager.get_font("Roboto-Regular.ttf", font_size), { 1.f, 1.f, 1.f });
	SDL_QueryTexture(title_texture.get(), nullptr, nullptr, &size.w, &size.h);
	current_font_size = font_size;
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
	g_Renderer.DrawBox(clipping, { 47, 47, 47 });

	clipping.pos.y += display_top_navigation(clipping).h;

	for (auto& shelf : shelfs)
	{
		clipping.pos.y += shelf.display(clipping).h;
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
	title = Text{ build_text(data.at(U("shelfRenderer")).at(U("title"))), 1.5_rem };

	auto items_data = data.at(U("shelfRenderer")).at(U("content")).at(U("horizontalListRenderer")).at(U("items")).as_array();
	for (auto item_data : items_data)
	{
		if (auto item = MediaItem::create(item_data.as_object()); item)
			items.push_back(std::move(item));
	}
}

auto YouTube::UI::Shelf::display(ActualPixelsRectangle clipping) -> ActualPixelsSize
{
	title.display(clipping);

	clipping.pos.y += 2_rem;

	for (auto& item : items)
	{
		item->display(clipping);
		clipping.pos.x += 22_rem;
	}

	return RemSize{ 0., 25.375 };
}

auto YouTube::UI::MainMenu::display(ActualPixelsRectangle clipping) -> ActualPixelsSize
{
	clipping.pos.x += 8.5_rem;

	main_content->display(clipping);

	return ActualPixelsSize();
}

YouTube::UI::MediaItem::MediaItem(const web::json::object& data)
{
	thumbnail = g_ImageManager.get_image(data.at(U("thumbnail")).at(U("thumbnails")).at(0).at(U("url")).as_string())
		.then([&](ImageManager::img_ptr image) {
			thumbnail = image;
		});
}

auto YouTube::UI::MediaItem::create(const web::json::object& data) -> std::unique_ptr<MediaItem>
{
	auto it = type_mapping.find(data.begin()->first);
	if (it == type_mapping.end())
	{
		std::wcout << "Unsupported media item type: " << data.begin()->first << '\n';
		return nullptr;
	}

	switch (it->second)
	{
		case Type::MusicVideo:
			return std::make_unique<MusicVideo>(data.at(U("tvMusicVideoRenderer")).as_object());
		case Type::Video:
			return std::make_unique<Video>(data.at(U("gridVideoRenderer")).as_object());
	}

	return nullptr;
}

auto YouTube::UI::MediaItem::display(ActualPixelsRectangle clipping) -> ActualPixelsSize
{
	if (std::holds_alternative<ImageManager::img_ptr>(thumbnail))
	{
		auto dstrect = RemRectangle{ clipping.pos, RemSize{21, 11.75} };
		auto srcrect = ActualPixelsRectangle{ {0, 12}, {120, 66} };
		g_Renderer.CopyTexture(std::get<ImageManager::img_ptr>(thumbnail).get(), srcrect, dstrect);
	}

	return ActualPixelsSize();
}

YouTube::UI::MusicVideo::MusicVideo(const web::json::object& data)
	: MediaItem(data)
{
	title = Text{ build_text(data.at(U("primaryText"))), 1.5_rem };
	
	secondary = Text{ build_text(data.at(U("secondaryText"))) + U('\n') + build_text(data.at(U("tertiaryText"))), 1_rem };
	length_text = Text{ build_text(data.at(U("lengthText"))), 0.875_rem };
}

YouTube::UI::Video::Video(const web::json::object& data)
	: MediaItem(data)
{
	title = Text{ build_text(data.at(U("title"))), 1.5_rem };

	secondary = Text{ build_text(data.at(U("shortBylineText"))) + U('\n') + build_text(data.at(U("shortViewCountText"))) + U(" • ") + build_text(data.at(U("publishedTimeText"))), 1_rem };
	length_text = Text{ build_text(data.at(U("lengthText"))), 0.875_rem };
}