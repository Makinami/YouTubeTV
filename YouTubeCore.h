#pragma once

#include <memory>

class GuardedRenderer;
class ImageManager;
class YouTubeAPI;
class FontManager;
class TextRenderer;

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

	// RAII wrapper around core system initialization and destruction.
	// Could be created at the beginning of main, instaed of calling
	// Initialize() and Shutdown() before return.
	struct YouTubeCoreRAII
	{
		YouTubeCoreRAII() { Initialize(); };
		~YouTubeCoreRAII() { Shutdown(); }
	};
}