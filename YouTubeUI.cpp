#include "pch.h"
#include "YouTubeUI.h"

#include <variant>
#include <utility>
#include <algorithm>
#include <mutex>
#include <cpprest/json.h>

#include "YouTubeCore.h"
#include "Renderer.h"
#include "YouTubeAPI.h"
#include "FontManager.h"
#include "ImageManager.h"
#include "YouTubeVideo.h"

using namespace Renderer::Dimensions;

namespace YouTube::UI
{
class HomeTab;
class Shelf;
class MediaItem;

class Text
{
public:
	Text() = default;
	Text(utility::string_t text, Rem _font_size);
	Text(const Text &) = delete;
	Text &operator=(const Text &) = delete;
	Text(Text &&other) noexcept = default;
	Text &operator=(Text &&other) noexcept = default;

	auto display(ActualPixelsRectangle clipping) -> ActualPixelsSize;

private:
	void render();

private:
	utf8string text_str;
	Rem font_size{1};

	std::shared_ptr<SDL_Texture> title_texture;
	ActualPixelsSize size;
	int current_font_size{-1};
};

class Thumbnail
{
public:
	Thumbnail() = default;
	Thumbnail(const Thumbnail &) = delete;
	Thumbnail &operator=(const Thumbnail &) = delete;
	Thumbnail(Thumbnail &&) = default;
	Thumbnail &operator=(Thumbnail &&) = default;

	Thumbnail(const web::json::object &data);

	auto display(ActualPixelsRectangle clipping) -> ActualPixelsSize;

private:
	ImageManager::img_ptr thumbnail;
	ActualPixelsSize size;
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
	HomeTab(const web::json::object &data);

	HomeTab() = default;
	HomeTab(const HomeTab &) = delete;
	HomeTab &operator=(const HomeTab &) = delete;
	HomeTab(HomeTab &&) = default;
	HomeTab &operator=(HomeTab &&) = default;

	virtual auto display(ActualPixelsRectangle clipping) -> ActualPixelsSize;
	auto display_top_navigation(ActualPixelsRectangle clipping) -> ActualPixelsSize;

	bool keyboard_callback(SDL_KeyboardEvent event);

private:
	utf8string title;
	std::vector<Shelf> shelfs;

	int selected_shelf = 1;
};

class Shelf
{
public:
	Shelf(const web::json::object &data);

	Shelf() = default;
	Shelf(const Shelf &) = delete;
	Shelf &operator=(const Shelf &) = delete;
	Shelf(Shelf &&) = default;
	Shelf &operator=(Shelf &&) = default;

	virtual auto display(ActualPixelsRectangle clipping, bool selected) -> ActualPixelsSize;

	bool keyboard_callback(SDL_KeyboardEvent event);

private:
	Text title;
	std::vector<std::unique_ptr<MediaItem>> items;

	int selected_item = 1;
};

class MediaItem
{
public:
	enum class Type
	{
		MusicVideo,
		Video
	};

private:
	static const std::unordered_map<utility::string_t, Type> type_mapping;

protected:
	MediaItem(const web::json::object &data);

public:
	static auto create(const web::json::object &data) -> std::unique_ptr<MediaItem>;

	virtual auto display(ActualPixelsRectangle clipping, bool selected) -> ActualPixelsSize;

	bool keyboard_callback(SDL_KeyboardEvent event);

protected:
	Type type;
	Text title;
	Text secondary;
	Thumbnail thumbnail;
	utf8string video_id;
	//std::variant<ImageManager::img_ptr, pplx::task<void>> thumbnail;
};

const std::unordered_map<utility::string_t, MediaItem::Type> MediaItem::type_mapping{
	{U("tvMusicVideoRenderer"), Type::MusicVideo},
	{U("gridVideoRenderer"), Type::Video}};

class MusicVideo : public MediaItem
{
public:
	MusicVideo(const web::json::object &data);

	//virtual auto display(ActualPixelsRectangle clipping)->ActualPixelsSize;

private:
	Text length_text;
};

class Video : public MediaItem
{
public:
	Video(const web::json::object &data);

	//virtual auto display(ActualPixelsRectangle clipping)->ActualPixelsSize;
private:
	Text length_text;
};
} // namespace YouTube::UI

auto build_text(const web::json::object &data) -> utility::string_t
{
	ASSERT(data.find(U("runs")) != data.end() || !data.at(U("runs")).is_array(), "JSON object passed to text builder doesn't contain 'runs' array.");

	auto runs = data.at(U("runs")).as_array();
	return std::accumulate(runs.begin(), runs.end(), utility::string_t{}, [](auto prev, auto curr) {
		return prev + curr.at(U("text")).as_string();
	});
};

auto build_text(const web::json::value &data) -> utility::string_t
{
	ASSERT(data.is_object(), "JSON value passed to text builder is not an object");
	return build_text(data.as_object());
}

YouTube::UI::Text::Text(utility::string_t text, Rem _font_size)
	: font_size{_font_size}
{
	text_str = utility::conversions::to_utf8string(std::move(text));
	render();
}

auto YouTube::UI::Text::display(ActualPixelsRectangle clipping) -> ActualPixelsSize
{
	if (static_cast<int>(font_size) != current_font_size)
		render();

	auto dst = SDL_Rect{static_cast<int>(clipping.pos.x), static_cast<int>(clipping.pos.y), static_cast<int>(size.w), static_cast<int>(size.h)};
	g_Renderer.CopyTexture(title_texture.get(), nullptr, &dst);

	return ActualPixelsSize{0, current_font_size};
}

void YouTube::UI::Text::render()
{
	title_texture = g_Renderer.RenderTextToNewTexture(text_str, g_FontManager.get_font("Roboto-Regular.ttf", font_size), {235, 235, 235});
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
		auto tabs_data = data.at(U("contents")).at(U("tvBrowseRenderer")).at(U("content")).at(U("tvSecondaryNavRenderer")).at(U("sections")).as_array()[0].at(U("tvSecondaryNavSectionRenderer")).at(U("tabs")).as_array();

		for (const auto &tab_data : tabs_data)
		{
			tabs.emplace_back(tab_data.as_object());
			break;
		}

		state = State::Loaded;
	});
	state = State::Loading;
}

YouTube::UI::HomeTab::HomeTab(const web::json::object &data)
{
	ASSERT(data.begin()->first == U("tabRenderer"), "Home tab data is not defined by tabRenderer object");
	title = utility::conversions::to_utf8string(data.at(U("tabRenderer")).at(U("title")).as_string());

	auto sections_data = data.at(U("tabRenderer")).at(U("content")).at(U("tvSurfaceContentRenderer")).at(U("content")).at(U("sectionListRenderer")).at(U("contents")).as_array();

	for (const auto &section_data : sections_data)
	{
		shelfs.emplace_back(section_data.as_object());
	}
}

auto YouTube::UI::HomeTab::display(ActualPixelsRectangle clipping) -> ActualPixelsSize
{
	g_KeyboardCallbacks.emplace_back(std::bind(&HomeTab::keyboard_callback, this, std::placeholders::_1));

	g_Renderer.DrawBox(clipping, {47, 47, 47});

	clipping.pos.y += display_top_navigation(clipping).h;

	for (int i = selected_shelf; i < shelfs.size(); ++i)
	{
		auto& shelf = shelfs[i];
		clipping.pos.y += shelf.display(clipping, i == selected_shelf).h;
	}

	return ActualPixelsSize();
}

auto YouTube::UI::HomeTab::display_top_navigation(ActualPixelsRectangle clipping) -> ActualPixelsSize
{
	clipping.size.h = 6.5_rem;
	g_Renderer.DrawBox(clipping, {57, 57, 57});

	return clipping.size;
}

bool YouTube::UI::HomeTab::keyboard_callback(SDL_KeyboardEvent event)
{
	if (event.keysym.sym == SDLK_UP && selected_shelf > 0)
	{
		--selected_shelf;
		return true;
	}
	if (event.keysym.sym == SDLK_DOWN && selected_shelf < shelfs.size() - 1)
	{
		++selected_shelf;
		return true;
	}
	return false;
}

YouTube::UI::MainMenu::MainMenu()
{
	main_content = std::make_unique<HomeView>();
}

YouTube::UI::Shelf::Shelf(const web::json::object &data)
{
	ASSERT(data.begin()->first == U("shelfRenderer"));
	title = Text{build_text(data.at(U("shelfRenderer")).at(U("title"))), 1.5_rem};

	auto items_data = data.at(U("shelfRenderer")).at(U("content")).at(U("horizontalListRenderer")).at(U("items")).as_array();
	for (auto item_data : items_data)
	{
		if (auto item = MediaItem::create(item_data.as_object()); item)
			items.push_back(std::move(item));
	}
}

auto YouTube::UI::Shelf::display(ActualPixelsRectangle clipping, bool selected) -> ActualPixelsSize
{
	if (selected)
		g_KeyboardCallbacks.emplace_back(std::bind(&Shelf::keyboard_callback, this, std::placeholders::_1));

	clipping.pos.x += 3_rem;
	clipping.pos.y += 0.125_rem /* half of line/font height diff */;

	title.display(clipping);

	clipping.pos.y += 1.5_rem /* font height */ + 0.125_rem /* half of line/font height diff */ + 1_rem /* margin-top of media item */;

	for (int i = std::max(selected_item - 1, 0); i < items.size(); ++i)
	{
		auto& item = items[i];
		item->display(clipping, selected && i == selected_item);
		clipping.pos.x += 22_rem;
	}

	return RemSize{0., 25.375};
}

bool YouTube::UI::Shelf::keyboard_callback(SDL_KeyboardEvent event)
{
	if (event.keysym.sym == SDLK_LEFT && selected_item > 0)
	{
		--selected_item;
		return true;
	}
	if (event.keysym.sym == SDLK_RIGHT && selected_item < items.size() - 1)
	{
		++selected_item;
		return true;
	}
	return false;
}

auto YouTube::UI::MainMenu::display(ActualPixelsRectangle clipping) -> ActualPixelsSize
{
	clipping.pos.x += 8.5_rem;

	main_content->display(clipping);

	return ActualPixelsSize();
}

YouTube::UI::MediaItem::MediaItem(const web::json::object &data)
	: thumbnail{data.at(U("thumbnail")).as_object()}, video_id{utility::conversions::to_utf8string(data.at(U("navigationEndpoint")).at(U("watchEndpoint")).at(U("videoId")).as_string())}
{
}

auto YouTube::UI::MediaItem::create(const web::json::object &data) -> std::unique_ptr<MediaItem>
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

auto YouTube::UI::MediaItem::display(ActualPixelsRectangle clipping, bool selected) -> ActualPixelsSize
{
	if (selected)
		g_KeyboardCallbacks.emplace_back(std::bind(&MediaItem::keyboard_callback, this, std::placeholders::_1));

	const auto thumbnailrect = [clipping](bool selected) {
		if (selected)
			return RemRectangle{ clipping.pos - RemSize{0.5, 0.5}, RemSize{22, 12.25} };
		else
			return RemRectangle{ clipping.pos, RemSize{21, 11.75} };
	}(selected);

	thumbnail.display(thumbnailrect);

	clipping.pos.y += 11.75_rem;

	if (selected)
		g_Renderer.DrawBox(RemRectangle{ clipping.pos - RemSize{0.5, 0}, RemSize{22, 8.15 } }, { 235, 235, 235 });

	title.display(clipping).y;
	clipping.pos.y += 1.5_rem; /* title font size */
	clipping.pos.y += 0.5_rem; /* title margin bottom */
	secondary.display(clipping);

	return ActualPixelsSize();
}

bool YouTube::UI::MediaItem::keyboard_callback(SDL_KeyboardEvent event)
{
	if (event.keysym.sym == SDLK_RETURN)
	{
		g_PlayingVideo = std::make_unique<YouTubeVideo>(video_id, g_Renderer);
		g_PlayingVideo->start();
		return true;
	}
	return false;
}

YouTube::UI::MusicVideo::MusicVideo(const web::json::object &data)
	: MediaItem(data)
{
	title = Text{build_text(data.at(U("primaryText"))), 1.5_rem};

	secondary = Text{build_text(data.at(U("secondaryText"))) + U('\n') + build_text(data.at(U("tertiaryText"))), 1_rem};
	length_text = Text{build_text(data.at(U("lengthText"))), 0.875_rem};
}

YouTube::UI::Video::Video(const web::json::object &data)
	: MediaItem(data)
{
	title = Text{build_text(data.at(U("title"))), 1.5_rem};

	secondary = Text{build_text(data.at(U("shortBylineText"))) + U('\n') + build_text(data.at(U("shortViewCountText"))) + U(" � ") + build_text(data.at(U("publishedTimeText"))), 1_rem};
	length_text = Text{build_text(data.at(U("lengthText"))), 0.875_rem};
}

YouTube::UI::Thumbnail::Thumbnail(const web::json::object &data)
{
	auto thumbnails = data.at(U("thumbnails")).as_array();
	auto it = std::lower_bound(thumbnails.begin(), thumbnails.end(), Vec2D<int>{475, 264} /* max size on FullHD */, [](const web::json::value &lhs, const Vec2D<int> &rhs) {
		return lhs.at(U("height")).as_integer() < rhs.h || lhs.at(U("width")).as_integer() < rhs.w;
	});

	if (it == thumbnails.end())
		--it; // get heightest even if not good enough

	size = ActualPixelsSize{it->at(U("width")).as_integer(), it->at(U("height")).as_integer()};
	g_ImageManager.get_image(it->at(U("url")).as_string())
		.then([&](ImageManager::img_ptr image) {
			thumbnail = image;
		});
}

auto YouTube::UI::Thumbnail::display(ActualPixelsRectangle clipping) -> ActualPixelsSize
{
	if (!thumbnail)
		return clipping.size;

	auto srcrect = calculate_projection_rect(size, clipping.size);
	g_Renderer.CopyTexture(thumbnail.get(), srcrect, clipping);

	return clipping.size;
}
