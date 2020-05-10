#pragma once

#include <vector>
#include <memory>
#include <string>
#include <unordered_map>
#include <mutex>

#include <cpprest/details/basic_types.h>
#include <SDL2/SDL_ttf.h>

#include "Renderer.h"

struct TextStyle
{
	std::vector<std::string> fonts;
	Renderer::Color color;
	int size;
};

struct Glyph
{
	SDL_Texture* texture;
	SDL_Rect rect;
	struct Metrics {
		int minx = 0, maxx = 0, miny = 0, maxy = 0, advance = 0;
		int height = 0, ascent = 0, descent = 0, line_skip = 0;
	} metrics;
	TTF_Font* font;
	char32_t code_point;
};

struct Word
{
	std::vector<Glyph> characters;
	int width;
	int advance;
};

struct Text
{
	std::vector<Word> words;
};

namespace std
{
	template<> struct hash<std::pair<const TTF_Font*, char16_t>>
	{
		std::size_t operator()(std::pair<const TTF_Font*, char16_t> const& s) const noexcept
		{
			auto ptr = reinterpret_cast<intptr_t>(s.first);
			return ptr ^ (s.second << 48 | s.second << 32 | s.second << 16 | s.second);
		}
	};
}

class TextRenderer
{
private:
	struct Atlas
	{
		std::unique_ptr<SDL_Texture> texture;
		const int capacity = 0;
		int used = 0;

		Atlas(std::unique_ptr<SDL_Texture> _texture, int _capacity) : texture{ std::move(_texture) }, capacity{ _capacity } {};
		int available() const { return capacity - used; }

		Atlas() = default;
		Atlas(const Atlas&) = delete;
		Atlas& operator=(const Atlas&) = delete;
		Atlas(Atlas&& other) noexcept = default;
		Atlas& operator=(Atlas&& other) noexcept = default;
	};

public:
	void Render(utf8string text, Renderer::Dimensions::ActualPixelsRectangle rect, TextStyle style);
	void Render(const Text& text, Renderer::Dimensions::ActualPixelsRectangle rect, TextStyle style);

	void ClearAll();

private:
	Glyph get_glyph(TTF_Font* font, char16_t code_point);
	Glyph generate_glyph(TTF_Font* font, char16_t code_point);
	Atlas& get_atlas(int height, int width);

private:
	std::mutex glyph_generation;
	std::unordered_map<std::pair<const TTF_Font*, char16_t>, Glyph> glyphs;
	std::unordered_map<int, std::vector<Atlas>> atlases;
};

