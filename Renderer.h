#pragma once

#include <memory>
#include <mutex>

#include "Deleters.h"

class GuardedRenderer
{
public:
	GuardedRenderer() {}
	GuardedRenderer(SDL_Window* window)
	{
		SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "linear");
		renderer = std::unique_ptr<SDL_Renderer>(SDL_CreateRenderer(window, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC));
	}

	void Initialize(SDL_Window* window)
	{
		WARNING(renderer == nullptr, "Renderer already initialized");
		SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "linear");
		renderer = std::unique_ptr<SDL_Renderer>(SDL_CreateRenderer(window, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC));
	}
	void Shutdown()
	{
		renderer = nullptr;
	}

	auto get_renderer() const
	{
		ASSERT(renderer, "Renderer not initialized");
		std::unique_lock<std::mutex> lc{ renderer_mtx };
		return std::tuple<std::unique_lock<std::mutex>, SDL_Renderer*>(std::move(lc), renderer.get());
	}

	auto Copy(SDL_Texture* texture, const SDL_Rect* srcrect, const SDL_Rect* dstrect) -> int;
	auto Present() -> void;

	auto LoadTexture(SDL_RWops* src, bool freesrc = true) -> std::unique_ptr<SDL_Texture>;

private:
	mutable std::mutex renderer_mtx;
	std::unique_ptr<SDL_Renderer> renderer;
};