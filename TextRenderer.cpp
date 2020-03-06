#include "pch.h"
#include "TextRenderer.h"

#include "YouTubeCore.h"
#include "FontManager.h"

#include "cpprest/json.h"

#include <SDL2/SDL2_gfxPrimitives.h>

using namespace YouTube;

using namespace std::string_literals;

std::unique_ptr<SDL_Texture> test;

std::u32string to_u32string(std::u8string internal)
{
    auto& f = std::use_facet<std::codecvt<char32_t, char8_t, std::mbstate_t>>(std::locale());
    //std::basic_string<char8_t> internal = u8"z\u00df\u6c34\U0001f34c"; // L"zß水🍌"

    // note that the following can be done with wstring_convert
    std::mbstate_t mb{}; // initial shift state
    std::basic_string<char32_t> external(internal.size() * f.max_length(), '\0');
    const char8_t* from_next;
    char32_t* to_next;
    f.in(mb, &internal[0], &internal[internal.size()], from_next,
        &external[0], &external[external.size()], to_next);
    // error checking skipped for brevity
    external.resize(to_next - &external[0]);

    return external;
}

std::u32string to_u32string(std::string text)
{
    std::u8string internal;
    std::copy(text.begin(), text.end(), std::back_inserter(internal));
    return to_u32string(internal);
}

void TextRenderer::Render(utf8string text, Renderer::Dimensions::ActualPixelsRectangle rect, TextStyle style)
{
	std::vector<TTF_Font*> fonts;
	std::transform(style.fonts.begin(), style.fonts.end(), std::back_inserter(fonts), [size = style.size](const std::string& filename) {
		return g_FontManager.get_font(filename, size);
	});

    std::basic_string<char8_t> internal = u8"z\u00df\u6c34\U0001f34c"; // L"zß水🍌"
    
    auto external = to_u32string(text);

    std::vector<Glyph> glyphs;
    std::transform(external.begin(), external.end(), std::back_inserter(glyphs), [&, this](char32_t code_point) {
        if (code_point > std::numeric_limits<char16_t>::max())
            code_point = 0xFFFD;

        auto it = std::find_if(fonts.begin(), fonts.end(), [code_point](auto font) {
            return TTF_GlyphIsProvided(font, code_point);
        });
        if (it == fonts.end())
            --it; // just use the last font even if it's missing the glyph

        auto font = *it;

        return get_glyph(font, code_point);
    });

    for (auto glyph : glyphs)
    {
        g_Renderer.CopyTexture(glyph.texture, glyph.rect, { {rect.pos.x, rect.pos.y}, {glyph.rect.w, glyph.rect.h} });
        rect.pos.x += glyph.rect.w;
    }

    SDL_Rect rect2 = { 0, 0, 2000, 29 };
    g_Renderer.DrawBox(rect2, { 0.5f, 0.5f, 0.5f, 1.0f });
    rect2.x = 0;
    rect2.y = 0;
    g_Renderer.CopyTexture(atlases[29][0].texture.get(), nullptr, &rect2);
}

void TextRenderer::ClearAll()
{
    auto lc = std::scoped_lock(glyph_generation);

    glyphs.clear();
    atlases.clear();
}

Glyph TextRenderer::get_glyph(TTF_Font* font, char16_t code_point)
{
    if (auto it = glyphs.find(std::make_pair(font, code_point)); it != glyphs.end())
        return it->second;

    return generate_glyph(font, code_point);
}

Glyph TextRenderer::generate_glyph(TTF_Font* font, char16_t code_point)
{
    auto lc = std::scoped_lock(glyph_generation);

    auto surface = std::unique_ptr<SDL_Surface>(TTF_RenderGlyph_Blended(font, code_point, { 255, 255, 255, 255 }));
    auto& atlas = get_atlas(surface->h, surface->w);

    auto glyph_position = SDL_Rect{ atlas.used, 0, surface->w, surface->h };
    g_Renderer.CopySurfaceToTexture(surface.get(), atlas.texture.get(), nullptr, &glyph_position);

    atlas.used += surface->w;

    return glyphs.emplace(std::make_pair(font, code_point), Glyph{ atlas.texture.get(), glyph_position }).first->second;
}

TextRenderer::Atlas& TextRenderer::get_atlas(int height, int width)
{
    auto& size_group = atlases.try_emplace(height, 0).first->second;

    if (size_group.size() == 0 || size_group.back().available() < width)
    {
        auto texture = g_Renderer.CreateTexture(SDL_PIXELFORMAT_RGBA8888, SDL_TEXTUREACCESS_STATIC | SDL_TEXTUREACCESS_TARGET, 2000, height, { 0, 0, 0, 0 });
        size_group.emplace_back(std::move(texture), 2000);
    }

    return size_group.back();
}
