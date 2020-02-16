#pragma once

#include <memory>

class GuardedRenderer;
class ImageManager;
class YouTubeAPI;

namespace YouTube
{
	using namespace std;

	void Initialize();
	void Shutdown();

	extern GuardedRenderer g_Renderer;
	extern ImageManager g_ImageManager;
	extern YouTubeAPI g_API;

	// RAII wrapper around core system initialization and destruction.
	// Could be created at the beginning of main, instaed of calling
	// Initialize() and Shutdown() before return.
	struct YouTubeCoreRAII
	{
		YouTubeCoreRAII() { Initialize(); };
		~YouTubeCoreRAII() { Shutdown(); }
	};
}