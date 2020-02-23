#include "pch.h"
#include "Renderer.h"

#include <SDL2/SDL_image.h>
#include <SDL2/SDL2_gfxPrimitives.h>

#include <utf8.h>

#include "YouTubeCore.h"

using namespace std;
using namespace Renderer::Dimensions;
using namespace YouTube;

#define GUARD() \
	ASSERT(renderer, "Renderer not initialized"); \
	std::unique_lock<std::mutex> lc{ renderer_mtx }

auto GuardedRenderer::CopyTexture(SDL_Texture* texture, const SDL_Rect* srcrect, const SDL_Rect* dstrect) -> int
{
	GUARD();
	return SDL_RenderCopy(renderer.get(), texture, srcrect, dstrect);
}

auto GuardedRenderer::CopyTexture(SDL_Texture* texture, const SDL_Rect srcrect, const SDL_Rect dstrect) -> int
{
	return CopyTexture(texture, &srcrect, &dstrect);
}

auto GuardedRenderer::CopyTexture(SDL_Texture* texture, const ActualPixelsRectangle srcrect, const ActualPixelsRectangle dstrect) -> int
{
	auto sdl_srcrect = SDL_Rect{ srcrect.pos.x, srcrect.pos.y, srcrect.size.w, srcrect.size.h };
	auto sdl_dstrect = SDL_Rect{ dstrect.pos.x, dstrect.pos.y, dstrect.size.w, dstrect.size.h };
	return CopyTexture(texture, &sdl_srcrect, &sdl_dstrect);
}

auto GuardedRenderer::DrawBox(ActualPixelsRectangle rect, Color color) -> int
{
	GUARD();
	return boxRGBA(renderer.get(), rect.pos.x, rect.pos.y, rect.pos.x + rect.size.w, rect.pos.y + rect.size.h, color.r, color.g, color.b, color.a);
}

auto GuardedRenderer::Present() -> void
{
	GUARD();
	SDL_RenderPresent(renderer.get());
}

auto GuardedRenderer::Clear(Color color) -> void
{
	GUARD();
	SDL_SetRenderDrawColor(renderer.get(), color.r, color.g, color.b, color.a);
	SDL_RenderClear(renderer.get());
}

auto GuardedRenderer::LoadTexture(SDL_RWops* src, bool freesrc) -> std::unique_ptr<SDL_Texture>
{
	GUARD();
	return std::unique_ptr<SDL_Texture>(IMG_LoadTexture_RW(renderer.get(), src, freesrc));
}

auto GuardedRenderer::RenderTextToNewTexture(const utf8string& _text, TTF_Font* const font, Color color) -> std::unique_ptr<SDL_Texture>
{
	GUARD();
	std::string text;
	utf8::utf16to8(_text.begin(), _text.end(), std::back_inserter(text));

	auto surface = std::unique_ptr<SDL_Surface>(TTF_RenderUTF8_Blended(font, text.c_str(), color));
	return std::unique_ptr<SDL_Texture>(SDL_CreateTextureFromSurface(renderer.get(), surface.get()));
}

Renderer::Dimensions::ActualPixelsPoint::ActualPixelsPoint(ScaledPercentagePoint other)
{
	auto dim = g_Renderer.GetSize();
	x = other.x * dim.scaled_width;
	y = other.y * dim.scaled_height;
}

Renderer::Dimensions::ActualPixelsPoint::ActualPixelsPoint(ActualPercentagePoint other)
{
	auto dim = g_Renderer.GetSize();
	x = other.x * dim.actual_width;
	y = other.y * dim.actual_height;
}

Renderer::Dimensions::ActualPixelsPoint::ActualPixelsPoint(RemPoint other)
	: Vec2D{ other.x, other.y }
{}

Renderer::Dimensions::ActualPixelsRectangle::ActualPixelsRectangle(ScaledPercentageRectangle other)
	: pos{ other.pos }, size{ other.size }
{}

Renderer::Dimensions::ActualPixelsRectangle::ActualPixelsRectangle(ActualPercentageRectangle other)
	: pos{ other.pos }, size{ other.size }
{}

Renderer::Dimensions::ActualPixelsRectangle::ActualPixelsRectangle(RemRectangle other)
	: pos{ other.pos }, size{ other.size }
{}

Renderer::Dimensions::ActualPercentageRectangle::ActualPercentageRectangle(ScaledPercentageRectangle other)
	: pos{ other.pos }, size{ other.size }
{}

Renderer::Dimensions::ActualPercentagePoint::ActualPercentagePoint(ScaledPercentagePoint other)
{
	auto dim = g_Renderer.GetSize();
	x = other.x * dim.scaled_width / dim.actual_width;
	y = other.y * dim.scaled_height / dim.actual_height;
}

Renderer::Dimensions::ScaledPercentageRectangle::ScaledPercentageRectangle(ActualPixelsRectangle other)
	: pos{ other.pos }, size{ other.size }
{}

Renderer::Dimensions::ScaledPercentagePoint::ScaledPercentagePoint(ActualPixelsPoint other)
{
	auto dim = g_Renderer.GetSize();
	x = other.x / dim.scaled_width;
	y = other.y / dim.scaled_height;
}

Renderer::Dimensions::Rem::operator double() const
{
	return value * 16 * g_Renderer.scaled_width / 1280;
}

Renderer::Dimensions::RemPoint::RemPoint(ActualPixelsPoint other)
{
	auto one_over_rem = 1. / static_cast<double>(1_rem);
	x = Rem{ other.x * one_over_rem };
	y = Rem{ other.y * one_over_rem };
}
