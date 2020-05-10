#include "pch.h"

#include "YouTubeCore.h"

#include <SDL2/SDL.h>
#include <SDL2/SDL_ttf.h>

#if defined(_WIN32) || defined(_WIN64)
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#endif

#include "Renderer.h"
#include "ImageManager.h"
#include "YouTubeAPI.h"
#include "FontManager.h"
#include "TextRenderer.h"

#include "YouTubeVideo.h"

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
	FontManager g_FontManager;
	TextRenderer g_TextRenderer;

	std::vector<std::function<bool(SDL_KeyboardEvent)>> g_KeyboardCallbacks;
	std::unique_ptr<YouTubeVideo> g_PlayingVideo;
}

void YouTube::Initialize()
{
	ASSERT(window == nullptr, "Core systems are already initialized");

#if defined(_WIN32) || defined(_WIN64)
	SetConsoleOutputCP(CP_UTF8);
#endif

	if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_TIMER))
		throw runtime_error("Could not initialize SDL: "s + SDL_GetError());

	if (TTF_Init() == -1)
		throw runtime_error("Could not initialize TTF: "s + TTF_GetError());

	window.reset(SDL_CreateWindow("YouTubeTV", SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED, 1280, 720, SDL_WINDOW_SHOWN | SDL_WINDOW_RESIZABLE | SDL_WINDOW_OPENGL));
	if (window == nullptr)
		throw runtime_error("Could not create windows: "s + SDL_GetError());

	g_Renderer.Initialize(window.get());

	g_FontManager.Initialize();
}

void YouTube::Shutdown()
{
	g_FontManager.clear();

	window.reset();

	TTF_Quit();
	SDL_Quit();
}
