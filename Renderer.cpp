#include "pch.h"
#include "Renderer.h"

#include <SDL2/SDL_image.h>
#include <SDL2/SDL2_gfxPrimitives.h>

#include <utf8.h>

using namespace std;

#define GUARD() \
	ASSERT(renderer, "Renderer not initialized"); \
	std::unique_lock<std::mutex> lc{ renderer_mtx }

auto GuardedRenderer::Copy(SDL_Texture* texture, const SDL_Rect* srcrect, const SDL_Rect* dstrect) -> int
{
	GUARD();
	return SDL_RenderCopy(renderer.get(), texture, srcrect, dstrect);
}

auto GuardedRenderer::DrawBox(Rectangle rect, Color color) -> int
{
	GUARD();
	return boxRGBA(renderer.get(), rect.x, rect.y, rect.x + rect.w, rect.y + rect.h, color.r, color.g, color.b, color.a);
}

auto GuardedRenderer::Present() -> void
{
	GUARD();
	SDL_RenderPresent(renderer.get());
}

auto GuardedRenderer::LoadTexture(SDL_RWops* src, bool freesrc) -> std::unique_ptr<SDL_Texture>
{
	GUARD();
	return std::unique_ptr<SDL_Texture>(IMG_LoadTexture_RW(renderer.get(), src, freesrc));
}

auto GuardedRenderer::RenderTextToNewTexture(const utility::string_t& _text, TTF_Font* const font, Color color) -> std::unique_ptr<SDL_Texture>
{
	GUARD();
	std::string text;
	utf8::utf16to8(_text.begin(), _text.end(), std::back_inserter(text));

	auto surface = std::unique_ptr<SDL_Surface>(TTF_RenderUTF8_Blended(font, text.c_str(), color));
	return std::unique_ptr<SDL_Texture>(SDL_CreateTextureFromSurface(renderer.get(), surface.get()));
}
