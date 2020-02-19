#include "pch.h"
#include "Renderer.h"

#include <SDL2/SDL_image.h>

using namespace std;

#define GUARD() \
	ASSERT(renderer, "Renderer not initialized"); \
	std::unique_lock<std::mutex> lc{ renderer_mtx }

auto GuardedRenderer::Copy(SDL_Texture* texture, const SDL_Rect* srcrect, const SDL_Rect* dstrect) -> int
{
	GUARD();
	return SDL_RenderCopy(renderer.get(), texture, srcrect, dstrect);
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
