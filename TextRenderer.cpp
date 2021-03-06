#include "pch.h"
#include "TextRenderer.h"

#include "YouTubeCore.h"
#include "FontManager.h"

#include "cpprest/json.h"

#include <SDL2/SDL2_gfxPrimitives.h>
#include <numeric>

using namespace YouTube;

using namespace std::string_literals;

void apply_kerning(std::vector<Glyph>& glyphs)
{
    std::adjacent_difference(glyphs.rbegin(), glyphs.rend(), glyphs.rbegin(), [](const Glyph A, const Glyph B) -> Glyph {
        auto ret = A;
        if (A.font == B.font)
        {
            ret.metrics.advance = ret.rect.w + TTF_GetFontKerningSizeGlyphs(A.font, static_cast<uint16_t>(A.code_point), static_cast<uint16_t>(B.code_point));
        }
        else
        {
            ret.metrics.advance = ret.rect.w;
        }
        return ret;
    });
}

inline std::vector<Glyph> apply_kerning(std::vector<Glyph>&& glyphs)
{
    apply_kerning(glyphs);

    return std::move(glyphs);
}

std::vector<Word> group_into_words(const std::vector<Glyph>& glyphs)
{
    std::vector<Word> words;
    words.emplace_back();
    for (auto glyph : glyphs) {
        if (glyph.code_point == ' ')
        {
            // add advance and glyph without adding width and start next word
            words.back().advance += glyph.metrics.advance;
            words.back().characters.emplace_back(std::move(glyph));
            words.emplace_back();
        }
        else if (glyph.code_point < 256)
        {
            // add advance, width and glyph
            words.back().width += glyph.metrics.advance;
            words.back().advance += glyph.metrics.advance;
            words.back().characters.emplace_back(std::move(glyph));
        }
        else
        {
            // finish word if non-zero length
            if (words.back().characters.size())
                words.emplace_back();

            // add advance, width and glyph and start next word
            words.back().width += glyph.metrics.advance;
            words.back().advance += glyph.metrics.advance;
            words.back().characters.emplace_back(std::move(glyph));
            words.emplace_back();
        }
    }
    if (words.size() && words.back().characters.size() == 0)
        words.pop_back();

    return words;
}

std::unique_ptr<SDL_Texture> test;

std::u32string to_u32string(std::u8string internal)
{
    auto& f = std::use_facet<std::codecvt<char32_t, char8_t, std::mbstate_t>>(std::locale());

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

PreprocessedText TextRenderer::PreprocessText(utf8string text, TextStyle style)
{
    std::vector<TTF_Font*> fonts;
    std::transform(style.fonts.begin(), style.fonts.end(), std::back_inserter(fonts), [size = style.size](const std::string& filename) {
        return g_FontManager.get_font(filename, size);
    });

    return {
        .words = group_into_words(
            apply_kerning(transform_to_glyphs(to_u32string(text), fonts))
        ),
        .line_height = TTF_FontLineSkip(*fonts.begin())
    };
}

void TextRenderer::Render(utf8string text, Renderer::Dimensions::ActualPixelsRectangle rect, TextStyle style)
{
    Render(PreprocessText(text, style), rect, style.color);
}

void TextRenderer::Render(const PreprocessedText& text, Renderer::Dimensions::ActualPixelsRectangle rect, Renderer::Color color)
{
    g_Renderer.DrawBox(rect, { 0.5f, 0.0f, 0.5f, 0.5f });

    auto max_lines = floor(static_cast<float>(rect.size.h) / text.line_height);

    if (text.words.size() == 0)
        return;

    auto glyph_position = rect.pos;
    glyph_position.y += TTF_FontAscent(text.words[0].characters[0].font);

    auto remaining_width = rect.size.w;    
    auto line_count = 1;
    for (auto it = text.words.begin(); it != text.words.end() && line_count <= max_lines; ++it)
    {
        if (it->width > remaining_width)
        {
            glyph_position.x = rect.pos.x;
            glyph_position.y += text.line_height;
            remaining_width = rect.size.w;
            ++line_count;
            continue;
        }

        for (auto glyph : it->characters)
        {
            g_Renderer.CopyTexture(glyph.texture, glyph.rect, { {glyph_position.x, glyph_position.y - glyph.metrics.ascent}, {glyph.rect.w, glyph.rect.h} }, color);
            glyph_position.x += glyph.metrics.advance;
        }

        remaining_width -= it->advance;
    }
}

void TextRenderer::ClearAll()
{
    auto lc = std::scoped_lock(glyph_generation);

    glyphs.clear();
    atlases.clear();
}

std::vector<Glyph> TextRenderer::transform_to_glyphs(std::u32string_view text, const std::vector<TTF_Font*>& fonts)
{
    std::vector<Glyph> glyphs;
    std::transform(text.begin(), text.end(), std::back_inserter(glyphs), [&, this](char32_t code_point) {
        if (code_point > std::numeric_limits<char16_t>::max())
            code_point = 0xFFFD;

        auto it = std::find_if(fonts.begin(), fonts.end(), [code_point](auto font) {
            return TTF_GlyphIsProvided(font, static_cast<uint16_t>(code_point));
        });
        if (it == fonts.end())
            --it; // just use the last font even if it's missing the glyph

        auto font = *it;
        return get_glyph(font, static_cast<uint16_t>(code_point));
    });

    return glyphs;
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
    g_RendererQueue.push([&](GuardedRenderer* g_Renderer) {
        g_Renderer->CopySurfaceToTexture(surface.get(), atlas.texture.get(), nullptr, &glyph_position);
    }).wait();

    atlas.used += surface->w;

    auto metrics = Glyph::Metrics{ .height = TTF_FontHeight(font), .ascent = TTF_FontAscent(font), .descent = TTF_FontDescent(font), .line_skip = TTF_FontLineSkip(font) };
    if (TTF_GlyphMetrics(font, code_point, &metrics.minx, &metrics.maxx, &metrics.miny, &metrics.maxy, &metrics.advance))
    {
        spdlog::info("Font {} {} doesn't have codepoint {}", TTF_FontFaceFamilyName(font), TTF_FontFaceStyleName(font), static_cast<uint16_t>(code_point));
    }

    return glyphs.emplace(std::make_pair(font, code_point), Glyph{ atlas.texture.get(), glyph_position, metrics, font, code_point }).first->second;
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
