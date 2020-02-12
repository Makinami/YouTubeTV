#include <SDL2/SDL.h>

#include "YouTubeVideo.h"

using namespace std::chrono_literals;
using namespace std::string_literals;

#undef main

int main(int argc, char* argv[])
{
	if (argc < 2) {
		fprintf(stderr, "Usage: test <file>\n");
		return -1;
	}

	if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_TIMER))
	{
		fprintf(stderr, "Could not initialize SDL- %s\n", SDL_GetError());
		return -1;
	}

	auto window = std::unique_ptr<SDL_Window>(SDL_CreateWindow("YouTubeTV", SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED, 800, 400, SDL_WINDOW_SHOWN));
	if (!window)
	{
		fprintf(stderr, "SDL: could not create window - exiting\n");
		return -1;
	}	

	GuardedRenderer renderer(window.get());

	YouTubeVideo media("B084zv59WwA", renderer);

	bool paused = false;
	media.start();

	SDL_Event event;
	while (true)
	{
		{
			// won't deadlock 'cause this is the only thread that needs both at the same time
			auto [flc, frame_ptr] = media.get_video_frame();
			auto [rlc, renderer_ptr] = renderer.get_renderer();
			SDL_RenderCopy(renderer_ptr, frame_ptr, nullptr, nullptr);
			flc.unlock();

			SDL_RenderPresent(renderer_ptr);
		}

		if (SDL_PollEvent(&event) == 0) continue;
		switch (event.type)
		{
			case SDL_KEYDOWN:
				switch (event.key.keysym.sym) {
					case SDLK_SPACE:
						if (paused)
						{
							media.unpause();
							paused = false;
						}
						else
						{
							media.pause();
							paused = true;
						}
						break;
					case SDLK_RIGHT:
					{
						auto new_time = media.get_time() + std::chrono::duration<double>{5};
						media.seek(new_time);
						break;
					}
					case SDLK_LEFT:
					{
						auto new_time = media.get_time() - std::chrono::duration<double>{5};
						media.seek(new_time);
						break;
					}
				}
				break;
			case SDL_QUIT:
				SDL_Quit();
				return 0;
				break;
			default:
				break;
		}
	}

	return 0;
}