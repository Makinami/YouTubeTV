#pragma once

#include <memory>
#include <mutex>
#include <algorithm>

#include "Deleters.h"

class GuardedRenderer
{
public:
	struct Dimensions
	{
		float scaled_width, scaled_height;
		int actual_width, actual_height;
	};

	struct Color
	{
		uint8_t r{ 0 }, g{ 0 }, b{ 0 }, a{ 1 };
		Color() {};
		Color(float _r, float _g, float _b, float _a = 1.f)
			: r(std::clamp(static_cast<int>(_r * 255), 0, 255)),
			  g(std::clamp(static_cast<int>(_g * 255), 0, 255)),
			  b(std::clamp(static_cast<int>(_b * 255), 0, 255)),
			  a(std::clamp(static_cast<int>(_a * 255), 0, 255))
		{}
		Color(int _r, int _g, int _b, int _a = 255)
			: r(std::clamp(_r, 0, 255)),
			  g(std::clamp(_g, 0, 255)),
			  b(std::clamp(_b, 0, 255)),
			  a(std::clamp(_a, 0, 255))
		{}
	};

	struct Rectangle
	{
		float x, y, w, h;
		Rectangle(float _x, float _y, float _w, float _h)
			: x(_x), y(_y), w(_w), h(_h)
		{}
		Rectangle(SDL_Rect rect)
			: x(rect.x), y(rect.y), w(rect.w), h(rect.h)
		{}
		Rectangle(SDL_FRect rect)
			: x(rect.x), y(rect.y), w(rect.w), h(rect.h)
		{}
	};

public:
	GuardedRenderer() {}
	GuardedRenderer(SDL_Window* window)
	{
		Initialize(window);
	}

	void Initialize(SDL_Window* window)
	{
		WARNING(renderer == nullptr, "Renderer already initialized");
		SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "linear");
		renderer = std::unique_ptr<SDL_Renderer>(SDL_CreateRenderer(window, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC));

		UpdateSize();
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

	void UpdateSize()
	{
		SDL_GetRendererOutputSize(renderer.get(), &width, &height);
		scaled_width = height * ratio;
		if (scaled_width > width)
		{
			scaled_width = width;
			scaled_height = scaled_width / ratio;
		}
		else
		{
			scaled_height = height;
		}
	}
	auto GetSize() const -> Dimensions
	{
		return { scaled_width, scaled_height, width, height };
	}

	auto Copy(SDL_Texture* texture, const SDL_Rect* srcrect, const SDL_Rect* dstrect) -> int;
	
	auto DrawBox(Rectangle rect, Color color) -> int;

	auto Present() -> void;

	auto LoadTexture(SDL_RWops* src, bool freesrc = true) -> std::unique_ptr<SDL_Texture>;

private:
	mutable std::mutex renderer_mtx;
	std::unique_ptr<SDL_Renderer> renderer;

	int width{ 0 }, height{ 0 };
	float scaled_width{ 0.f }, scaled_height{ 0.f };

	float ratio = 16.f / 9.f;
};