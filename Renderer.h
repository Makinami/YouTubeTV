#pragma once

#include <memory>
#include <mutex>

#include "Deleters.h"

class GuardedRenderer
{
public:
	GuardedRenderer(SDL_Window* window)
	{
		SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "linear");
		renderer = std::unique_ptr<SDL_Renderer>(SDL_CreateRenderer(window, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC));
	}
	auto get_renderer() const
	{
		std::unique_lock<std::mutex> lc{ renderer_mtx };
		return std::tuple<std::unique_lock<std::mutex>, SDL_Renderer*>(std::move(lc), renderer.get());
	}
	mutable std::mutex renderer_mtx;
private:
	std::unique_ptr<SDL_Renderer> renderer;
};