#pragma once

#include <memory>
#include <optional>
#include <vector>
#include <functional>

#include <SDL2/SDL_events.h>

class GuardedRenderer;
class ImageManager;
class YouTubeAPI;
class FontManager;
class YouTubeVideo;
class TextRenderer;

namespace Renderer {
	class RenderQueue;
}

namespace YouTube
{
	using namespace std;

	void Initialize();
	void Shutdown();

	extern GuardedRenderer g_Renderer;
	extern ImageManager g_ImageManager;
	extern YouTubeAPI g_API;
	extern FontManager g_FontManager;
	extern TextRenderer g_TextRenderer;

	extern Renderer::RenderQueue g_RendererQueue;

	extern std::vector<std::function<bool(SDL_KeyboardEvent)>> g_KeyboardCallbacks;
	extern std::unique_ptr<YouTubeVideo> g_PlayingVideo;

	// RAII wrapper around core system initialization and destruction.
	// Could be created at the beginning of main, instaed of calling
	// Initialize() and Shutdown() before return.
	struct YouTubeCoreRAII
	{
		YouTubeCoreRAII() { Initialize(); };
		~YouTubeCoreRAII() { Shutdown(); }
	};
}