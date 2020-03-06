#pragma once

#include <memory>
#include <mutex>
#include <algorithm>
#include <limits>

#include <cpprest/details/basic_types.h>

#include "Deleters.h"

class GuardedRenderer;

namespace Renderer
{
	namespace Dimensions
	{
		template<typename T>
		struct Vec2D
		{
			Vec2D() : x{ T{0} }, y{ T{ 0 } } {}
			Vec2D(T _x, T _y) : x{ _x }, y{ _y } {}
			virtual ~Vec2D() {};
			union { T x; T w; };
			union { T y; T h; };
		};

		struct Rem
		{
			operator double() const;
			Rem& operator +=(const Rem& other)
			{
				value += other.value;
				return *this;
			}
			Rem& operator +=(const double& other)
			{
				value += other;
				return *this;
			}

			Rem& operator -=(const Rem& other)
			{
				value -= other.value;
				return *this;
			}
			Rem& operator -=(const double& other)
			{
				value -= other;
				return *this;
			}

			double value;
		};
		inline Rem operator+ (Rem x, const Rem& y) { x += y; return x; }
		inline Rem operator- (Rem x, const Rem& y) { x += y; return x; }


		inline Rem operator "" _rem(long double value)
		{
			return Rem{ static_cast<double>(value) };
		}

		inline Rem operator "" _rem(unsigned long long int value)
		{
			return Rem{ static_cast<double>(value) };
		}

		struct ActualPixelsPoint;
		struct ScaledPixelsPoint;
		struct ActualPercentagePoint;
		struct ScaledPercentagePoint;
		struct RemPoint;

		struct ActualPixelsPoint : public Vec2D<int>
		{
			ActualPixelsPoint(int x = 0, int y = 0) : Vec2D{ x, y } {};
			ActualPixelsPoint(ScaledPercentagePoint other);
			ActualPixelsPoint(ActualPercentagePoint other);
			ActualPixelsPoint(RemPoint other);

			ActualPixelsPoint& operator -=(const RemPoint& other);
		};

		inline ActualPixelsPoint operator- (ActualPixelsPoint x, const RemPoint& y) { x -= y; return x; }

		struct ScaledPixelsPoint : public Vec2D<int>
		{
			ScaledPixelsPoint(int x, int y) : Vec2D{ x, y } {};
		};
		struct ActualPercentagePoint : public Vec2D<float>
		{
			ActualPercentagePoint(float x = 0.f, float y = 0.f) : Vec2D{ x, y } {};
			ActualPercentagePoint(ScaledPercentagePoint other);
		};
		struct ScaledPercentagePoint : public Vec2D<float>
		{
			ScaledPercentagePoint(float x, float y) : Vec2D{ x, y } {};
			ScaledPercentagePoint(ActualPixelsPoint other);
		};

		struct RemPoint : public Vec2D<Rem>
		{
			RemPoint(double x, double y) : Vec2D<Rem>{ Rem{x}, Rem{y} } {};
			RemPoint(ActualPixelsPoint other);
		};

		using ActualPixelsSize = ActualPixelsPoint;
		using ScaledPixelsSize = ScaledPixelsPoint;
		using ActualPercentageSize = ActualPercentagePoint;
		using ScaledPercentageSize = ScaledPercentagePoint;
		using RemSize = RemPoint;

		struct ActualPixelsRectangle;
		struct ScaledPixelsRectangle;
		struct ActualPercentageRectangle;
		struct ScaledPercentageRectangle;
		struct RemRectangle;

		struct ActualPixelsRectangle
		{
			ActualPixelsRectangle(ActualPixelsPoint _pos, ActualPixelsSize _size) : pos{ _pos }, size{ _size } {}
			ActualPixelsRectangle(ScaledPercentageRectangle other);
			ActualPixelsRectangle(ActualPercentageRectangle other);
			ActualPixelsRectangle(RemRectangle other);
			ActualPixelsRectangle(SDL_Rect other) : pos{ other.x, other.y }, size{ other.w, other.h } {};

			ActualPixelsPoint pos;
			ActualPixelsSize size = { std::numeric_limits<int>::max(), std::numeric_limits<int>::max() };
		};

		struct ScaledPixelsRectangle
		{
			ScaledPixelsPoint pos;
			ScaledPixelsSize size = { std::numeric_limits<int>::max(), std::numeric_limits<int>::max() };
		};

		struct ActualPercentageRectangle
		{
			ActualPercentageRectangle(const ScaledPercentageRectangle other);

			ActualPercentagePoint pos;
			ActualPercentageSize size = { 1., 1. };
		};

		struct ScaledPercentageRectangle
		{
			ScaledPercentageRectangle(const ActualPixelsRectangle other);
			ScaledPercentagePoint pos;
			ScaledPercentageSize size = { 1., 1. };
		};

		struct RemRectangle
		{
			RemRectangle(RemPoint _pos, RemSize _size) : pos{ _pos }, size{ _size } {};
			RemPoint pos;
			RemSize size;
		};
	}

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
		operator SDL_Color() const { return { r, g, b, a }; }
	};
}

class GuardedRenderer
{
public:
	struct Dimensions
	{
		float scaled_width, scaled_height;
		int actual_width, actual_height;
	};

	friend Renderer::Dimensions::Rem;

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
		renderer = std::unique_ptr<SDL_Renderer>(SDL_CreateRenderer(window, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_TARGETTEXTURE));

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

	auto CopyTexture(SDL_Texture* texture, const SDL_Rect* srcrect, const SDL_Rect* dstrect) -> int;
	auto CopyTexture(SDL_Texture* texture, const SDL_Rect srcrect, const SDL_Rect dstrect) -> int;
	auto CopyTexture(SDL_Texture* texture, const Renderer::Dimensions::ActualPixelsRectangle srcrect, const Renderer::Dimensions::ActualPixelsRectangle dstrect) -> int;
	
	auto DrawBox(Renderer::Dimensions::ActualPixelsRectangle rect, Renderer::Color color) -> int;

	auto Present() -> void;
	auto Clear(Renderer::Color color = {0, 0, 0, 0}) -> void;

	auto LoadTexture(SDL_RWops* src, bool freesrc = true) -> std::unique_ptr<SDL_Texture>;

	auto RenderTextToNewTexture(const utf8string& text, TTF_Font* const font, Renderer::Color color) -> std::unique_ptr<SDL_Texture>;

	auto CreateTexture(const int format, int access, int w, int h, Renderer::Color color = Renderer::Color{ 0, 0, 0, 255 }, SDL_BlendMode blend = SDL_BLENDMODE_BLEND)->std::unique_ptr<SDL_Texture>;
	auto CreateTexture(SDL_Surface* surface)->std::unique_ptr<SDL_Texture>;

	auto CopyTextureToTexture(SDL_Texture* src, SDL_Texture* dest, const SDL_Rect* srcrect, const SDL_Rect* dstrect) -> int;
	auto CopySurfaceToTexture(SDL_Surface* src, SDL_Texture* dest, const SDL_Rect* srcrect, const SDL_Rect* dstrect) -> int;

private:
	mutable std::mutex renderer_mtx;
	std::unique_ptr<SDL_Renderer> renderer;

	int width{ 0 }, height{ 0 };
	float scaled_width{ 0.f }, scaled_height{ 0.f };

	float ratio = 16.f / 9.f;
};

inline SDL_Rect calculate_projection_rect(int dst_width, int dst_height,
	int src_width, int src_height)
{
	auto aspect_ratio = av_make_q(src_width, src_height);

	/* XXX: we suppose the screen has a 1.0 pixel ratio */
	auto height = dst_height;
	auto width = int{ av_rescale(height, aspect_ratio.num, aspect_ratio.den) & ~1 };
	if (width > dst_width)
	{
		width = dst_width;
		height = av_rescale(width, aspect_ratio.den, aspect_ratio.num) & ~1;
	}
	auto x = (dst_width - width) / 2;
	auto y = (dst_height - height) / 2;
	return { x, y, std::max(width, 1), std::max(height, 1) };
}

inline SDL_Rect calculate_projection_rect(Renderer::Dimensions::ActualPixelsSize dst, Renderer::Dimensions::ActualPixelsSize src)
{
	return calculate_projection_rect(dst.w, dst.h, src.w, src.h);
}