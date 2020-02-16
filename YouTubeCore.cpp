#include "pch.h"

#include "YouTubeCore.h"

#include <SDL2/SDL.h>

#if defined(_WIN32) || defined(_WIN64)
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#endif

#include "Renderer.h"
#include "ImageManager.h"
#include "YouTubeAPI.h"

using namespace std;
using namespace std::literals;

namespace
{
	unique_ptr<SDL_Window> window;
}

namespace YouTube
{
	GuardedRenderer g_Renderer;
	ImageManager g_ImageManager;
	YouTubeAPI g_API;
}

void YouTube::Initialize()
{
	ASSERT(window == nullptr, "Core systems are already initialized");

#if defined(_WIN32) || defined(_WIN64)
	SetConsoleOutputCP(CP_UTF8);
#endif

	if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_TIMER))
		throw runtime_error("Could not initialize SDL: "s + SDL_GetError());

	window.reset(SDL_CreateWindow("YouTubeTV", SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED, 800, 400, SDL_WINDOW_SHOWN | SDL_WINDOW_RESIZABLE));
	if (window == nullptr)
		throw runtime_error("Could not create windows: "s + SDL_GetError());

	g_Renderer.Initialize(window.get());
}

void YouTube::Shutdown()
{
	window.release();

	SDL_Quit();
}
