#include "pch.h"
#include "YouTubeUI.h"

#include <variant>
#include <utility>
#include <algorithm>
#include <numeric>
#include <mutex>
#include <cpprest/json.h>

#include "YouTubeCore.h"
#include "Renderer.h"
#include "YouTubeAPI.h"
#include "FontManager.h"
#include "ImageManager.h"
#include "YouTubeVideo.h"
#include "TextRenderer.h"

using namespace Renderer::Dimensions;
using namespace std::string_literals;

namespace YouTube::UI
{
class HomeTab;
class Shelf;
class MediaItem;

class Text
{
public:
	Text() = default;
	Text(utility::string_t text, TextStyle _font_style);
	Text(std::string text, TextStyle _font_style);
	Text(const Text &) = delete;
	Text &operator=(const Text &) = delete;
	Text(Text &&other) noexcept = default;
	Text &operator=(Text &&other) noexcept = default;

	auto display(ActualPixelsRectangle clipping, Renderer::Color colour = default_colour) -> ActualPixelsSize;
	const auto& str() const { return text_str; }

private:
	void render();

public:
	static constexpr Renderer::Color default_colour = { 235, 235, 235 };
	static constexpr Renderer::Color selected_colour = { 0x2f, 0x2f, 0x2f };

private:
	utf8string text_str;
	TextStyle font_style;

	std::shared_ptr<SDL_Texture> title_texture;
	PreprocessedText preprocessed_text;
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

	Thumbnail(const nlohmann::json& data);

	~Thumbnail()
	{
		ctx.cancel();
		loading_task.wait();
	}

	auto display(ActualPixelsRectangle clipping) -> ActualPixelsSize;

private:
	ImageManager::img_ptr thumbnail;
	ActualPixelsSize size;

	pplx::task<void> loading_task;
	pplx::cancellation_token_source ctx;
};

class HomeView : public BasicElement
{
public:
	virtual auto display(ActualPixelsRectangle clipping) -> ActualPixelsSize;

	virtual ~HomeView()
	{
		ctx.cancel();
		loading_task.wait();
	}

private:
	void Initialize();

	std::vector<HomeTab> tabs;
	mutable std::mutex tabs_list_mtx;
};

class HomeTab : public BasicElement
{
public:
	HomeTab(const nlohmann::json& data);

	HomeTab() = default;
	HomeTab(const HomeTab& other) = delete;
	HomeTab& operator=(const HomeTab& other) = delete;
	HomeTab(HomeTab&& other) noexcept
		: HomeTab()
	{
		swap(*this, other);
	}
	HomeTab& operator=(HomeTab&& other) noexcept
	{
		swap(*this, other);
		return *this;
	}

	virtual auto display(ActualPixelsRectangle clipping) -> ActualPixelsSize;
	auto display_top_navigation(ActualPixelsRectangle clipping) -> ActualPixelsSize;

	bool keyboard_callback(SDL_KeyboardEvent event);

	friend void swap(HomeTab& first, HomeTab& second)
	{
		std::scoped_lock lock{ first.shelfs_list_mtx, second.shelfs_list_mtx };
		swap(first.ctx, second.ctx);
		swap(first.loading_task, second.loading_task);
		swap(first.title, second.title);
		swap(first.shelfs, second.shelfs);
		swap(first.selected_shelf, second.selected_shelf);
		swap(first.continuation_payload, second.continuation_payload);
	}

private:
	void load_more_shelfs();

	utf8string title;
	std::vector<Shelf> shelfs;
	mutable std::mutex shelfs_list_mtx;
	std::string continuation_payload;

	int selected_shelf = 0;
};

class Shelf
{
public:
	Shelf(const nlohmann::json& data);

	Shelf() = default;
	Shelf(const Shelf &) = delete;
	Shelf &operator=(const Shelf &) = delete;
	Shelf(Shelf&& other)
		: Shelf()
	{
		swap(*this, other);
	}
	Shelf& operator=(Shelf&& other)
	{
		swap(*this, other);
		return *this;
	}

	virtual auto display(ActualPixelsRectangle clipping, bool selected) -> ActualPixelsSize;

	bool keyboard_callback(SDL_KeyboardEvent event);

	friend void swap(Shelf& first, Shelf& second)
	{
		std::scoped_lock lock{ first.items_list_mtx, second.items_list_mtx };
		swap(first.title, second.title);
		swap(first.items, second.items);
		swap(first.selected_item, second.selected_item);
	}

	auto size() const { return items.size(); }

private:
	Text title;
	std::vector<std::unique_ptr<MediaItem>> items;
	mutable std::mutex items_list_mtx;

	int selected_item = 0;
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
	static const std::unordered_map<std::string, Type> type_mapping;

protected:
	MediaItem(const nlohmann::json& data);

public:
	static auto create(const nlohmann::json& data)->std::unique_ptr<MediaItem>;

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

const std::unordered_map<std::string, MediaItem::Type> MediaItem::type_mapping{
	{"tvMusicVideoRenderer", Type::MusicVideo},
	{"gridVideoRenderer", Type::Video}};

class MusicVideo : public MediaItem
{
public:
	MusicVideo(const nlohmann::json& data);

	//virtual auto display(ActualPixelsRectangle clipping)->ActualPixelsSize;

private:
	Text length_text;
};

class Video : public MediaItem
{
public:
	Video(const nlohmann::json& data);

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

auto build_text(const nlohmann::json& data) -> std::string
{
	if (data.is_string()) return data.get<std::string>();

	return std::accumulate(data["runs"].begin(), data["runs"].end(), ""s, [](std::string prev, const nlohmann::json& part) {
		ASSERT(part["text"].is_string());
		return std::move(prev) + part["text"].get<std::string>();
	});
}

YouTube::UI::Text::Text(utility::string_t text, TextStyle _font_style)
	: font_style{_font_style}
{
	text_str = utility::conversions::to_utf8string(std::move(text));
	render();
}

YouTube::UI::Text::Text(std::string text, TextStyle _font_style)
	: font_style{ _font_style }, text_str{ std::move(text) }
{
	render();
}

auto YouTube::UI::Text::display(ActualPixelsRectangle clipping, Renderer::Color colour) -> ActualPixelsSize
{
	if (static_cast<int>(font_style.size) != current_font_size)
		render();

	g_TextRenderer.Render(preprocessed_text, clipping, colour);

	return clipping.size;
}

void YouTube::UI::Text::render()
{
	preprocessed_text = g_TextRenderer.PreprocessText(text_str, font_style);
	current_font_size = static_cast<int>(font_style.size);
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
		std::lock_guard lock{ tabs_list_mtx };
		for (auto& tab : tabs)
			tab.display(clipping);
		break;
	}

	return ActualPixelsSize();
}

void YouTube::UI::HomeView::Initialize()
{
	loading_task = g_API.get(U("default"), ctx.get_token()).then([this](nlohmann::json data) {
		spdlog::trace("Home view data:\n{}", data.dump(2));
		try {
			const auto secondary_nav_renderer = data["contents"]["tvBrowseRenderer"]["content"]["tvSecondaryNavRenderer"];
			const auto title = build_text(secondary_nav_renderer["title"]);
			spdlog::info("Processing {} view", title);
			const auto tabs_data = secondary_nav_renderer["sections"][0]["tvSecondaryNavSectionRenderer"]["tabs"];

			for (const auto &tab_data : tabs_data)
			{
				auto tab = HomeTab{ tab_data };
				std::lock_guard lock{ tabs_list_mtx };
				tabs.emplace_back(std::move(tab));
				break;
			}

			state = State::Loaded;
			spdlog::info("{} view loaded", title);
		}
		catch (const web::json::json_exception& error)
		{
			spdlog::critical(error.what());
		}
		catch (...) {
			spdlog::critical("Unknowns exception");
		}

	});
	state = State::Loaded;
}

YouTube::UI::HomeTab::HomeTab(const nlohmann::json& data)
{
	ASSERT(data.contains("tabRenderer"), "Home tab data is not defined by tabRenderer object");
	const auto tab_renderer = data["tabRenderer"];
	title = build_text(tab_renderer["title"]);

	spdlog::info("Processing {} tab", title);
	for (const auto& section_data : tab_renderer["content"]["tvSurfaceContentRenderer"]["content"]["sectionListRenderer"]["contents"])
	{
		auto shelf = Shelf{ section_data };
		if (shelf.size())
			shelfs.emplace_back(std::move(shelf));
	}
	continuation_payload = tab_renderer["content"]["tvSurfaceContentRenderer"]["content"]["sectionListRenderer"]["continuations"][0]["nextContinuationData"]["continuation"];
	spdlog::info("{} tab loaded", title);
}

auto YouTube::UI::HomeTab::display(ActualPixelsRectangle clipping) -> ActualPixelsSize
{
	g_KeyboardCallbacks.emplace_back(std::bind(&HomeTab::keyboard_callback, this, std::placeholders::_1));

	g_Renderer.DrawBox(clipping, {47, 47, 47});

	clipping.pos.y += display_top_navigation(clipping).h;

	std::lock_guard lock{ shelfs_list_mtx };
	for (int i = selected_shelf; i < shelfs.size(); ++i)
	{
		auto& shelf = shelfs[i];
		clipping.pos.y += shelf.display(clipping, i == selected_shelf).h;
	}

	if (selected_shelf >= shelfs.size() - 2 && (loading_task == decltype(loading_task){} || loading_task.is_done()))
	{
		load_more_shelfs();
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

void YouTube::UI::HomeTab::load_more_shelfs()
{
	if (continuation_payload == "") return;

	ctx = {};
	loading_task = g_API.get_continuation(utility::conversions::to_string_t(continuation_payload), ctx.get_token()).then([this](nlohmann::json data) {
		spdlog::trace("Continuation data:\n{}", data.dump(2));
		for (const auto& section_data : data["continuationContents"]["sectionListContinuation"]["contents"])
		{
			auto shelf = Shelf{ section_data };
			if (shelf.size())
			{
				std::lock_guard lock{ shelfs_list_mtx };
				shelfs.emplace_back(std::move(shelf));
			}
		}
		//TODO: Better JSON handling
		if (data["continuationContents"].contains("sectionListContinuation") && !data["continuationContents"]["sectionListContinuation"]["continuations"].empty())
			continuation_payload = data["continuationContents"]["sectionListContinuation"]["continuations"][0]["nextContinuationData"]["continuation"];
		else
			continuation_payload = "";
	});
}

YouTube::UI::MainMenu::MainMenu()
{
	main_content = std::make_unique<HomeView>();
}

YouTube::UI::Shelf::Shelf(const nlohmann::json& data)
{
	ASSERT(data.contains("shelfRenderer"));
	const auto& shelf_renderer = data["shelfRenderer"];
	title = Text{ build_text(shelf_renderer["headerRenderer"]["shelfHeaderRenderer"]["title"]), {
		.fonts = { /*"Roboto Regular",*/ "Arial Regular", "Meiryo Regular" },
		.size = static_cast<int>(1.5_rem)
	} };

	spdlog::info("Processing {} shelf", title.str());
	for (const auto& item_data : shelf_renderer["content"]["horizontalListRenderer"]["items"])
	{
		if (auto item = MediaItem::create(item_data); item)
		{
			std::lock_guard lock{ items_list_mtx };
			items.push_back(std::move(item));
		}
	}
	spdlog::info("{} shelf loaded", title.str());
}

auto YouTube::UI::Shelf::display(ActualPixelsRectangle clipping, bool selected) -> ActualPixelsSize
{
	if (selected)
		g_KeyboardCallbacks.emplace_back(std::bind(&Shelf::keyboard_callback, this, std::placeholders::_1));

	clipping.pos.x += 3_rem;
	clipping.pos.y += 0.125_rem /* half of line/font height diff */;

	title.display({ clipping.pos, {clipping.size.w, static_cast<int>(1.75_rem)} });

	clipping.pos.y += 1.5_rem /* font height */ + 0.125_rem /* half of line/font height diff */ + 1_rem /* margin-top of media item */;

	std::lock_guard lock{ items_list_mtx };
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

YouTube::UI::MediaItem::MediaItem(const nlohmann::json& data)
	: thumbnail{ data["thumbnail"] }, video_id{ data["navigationEndpoint"]["watchEndpoint"]["videoId"].get<std::string>() }
{
}

auto YouTube::UI::MediaItem::create(const nlohmann::json& data) -> std::unique_ptr<MediaItem>
{
	auto it = type_mapping.find(data.begin().key());
	if (it == type_mapping.end())
	{
		spdlog::warn("Unsupported media item type: {}", data.begin().key());
		return nullptr;
	}

	try
	{
		switch (it->second)
		{
			case Type::MusicVideo:
				return std::make_unique<MusicVideo>(data["tvMusicVideoRenderer"]);
			case Type::Video:
				return std::make_unique<Video>(data["gridVideoRenderer"]);
		}
	}
	catch (const std::exception& exception)
	{
		spdlog::error("Failed to create the media item:\nError: {}\n{}", exception.what(), data.dump(2));
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

	clipping.pos.y += title.display({ clipping.pos, RemSize{21, 3.5} }, selected ? title.selected_colour : title.default_colour).y;
	clipping.pos.y += 0.5_rem; /* title margin bottom */
	secondary.display({ clipping.pos, RemSize{21, 1.25} });

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

YouTube::UI::MusicVideo::MusicVideo(const nlohmann::json& data)
	: MediaItem(data)
{
	title = Text{ build_text(data.at("primaryText")), {
		.fonts = { "Roboto Bold", "Arial Bold", "Meiryo Bold", "Roboto Regular", "Arial Regular", "Meiryo Regular" },
		.size = static_cast<int>(1.5_rem)
	} };

	secondary = Text{ build_text(data.at("secondaryText")) + " ??? " + build_text(data.at("tertiaryText")), {
		.fonts = { "Roboto Regular", "Arial Regular", "Meiryo Regular" },
		.size = static_cast<int>(1_rem)
	} };
	length_text = Text{ build_text(data.at("lengthText")), {
		.fonts = { "Roboto Bold", "Arial Bold", "Meiryo Bold", "Roboto Regular", "Arial Regular", "Meiryo Regular" },
		.size = static_cast<int>(0.875_rem)
	} };
}

YouTube::UI::Video::Video(const nlohmann::json& data)
	: MediaItem(data)
{
	title = Text{ build_text(data.at("title")), {
		.fonts = { "Roboto Bold", "Arial Bold", "Meiryo Bold", "Roboto Regular", "Arial Regular", "Meiryo Regular" },
		.size = static_cast<int>(1.5_rem)
	} };

	secondary = Text{ build_text(data.at("shortBylineText")) + " ??? " + build_text(data.at("shortViewCountText")) /*+ " ??? " + build_text(data.at("publishedTimeText"))*/, {
		.fonts = { "Roboto Regular", "Arial Regular", "Meiryo Regular" },
		.size = static_cast<int>(1_rem)
	} };
	length_text = Text{ build_text(data.at("lengthText")), {
		.fonts = { "Roboto Bold", "Arial Bold", "Meiryo Bold", "Roboto Regular", "Arial Regular", "Meiryo Regular" },
		.size = static_cast<int>(0.875_rem)
	} };
}

YouTube::UI::Thumbnail::Thumbnail(const nlohmann::json& data)
{
	auto thumbnails = data["thumbnails"];
	auto it = std::lower_bound(thumbnails.begin(), thumbnails.end(), Vec2D<int>{475, 264} /* max size on FullHD */, [](const nlohmann::json& lhs, const Vec2D<int>& rhs) {
		return lhs["height"].get<int>() < rhs.h || lhs["width"].get<int>() < rhs.w;
	});

	if (it == thumbnails.end())
		--it; // get heightest even if not good enough

	size = ActualPixelsSize{ it.value()["width"].get<int>(), it.value()["height"].get<int>() };
	auto url = it.value()["url"].get<std::string>();
	spdlog::info("Loading thumbnail: {}", url);
	loading_task = g_ImageManager.get_image(utility::conversions::to_string_t(url), ctx.get_token())
		.then([&, url](ImageManager::img_ptr image) {
			//TODO: Check if image was loaded correctly and display a placeholder if not
			thumbnail = image;
			spdlog::info("Thumbnail {} loaded", url);
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
