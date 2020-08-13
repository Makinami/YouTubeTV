#pragma once

#include <memory>

#include <cpprest/http_client.h>

#include "Renderer.h"

namespace YouTube::UI
{
	using namespace Renderer::Dimensions;

	struct Position
	{
		float x{ 0 }, y{ 0 };
	};

	struct Dimensions
	{
		float width{ 0 }, height{ 0 };
	};

	class BasicElement
	{
	protected:
		enum class State { Uninitialized, Loading, Loaded };
	public:
		virtual auto display(ActualPixelsRectangle clipping) -> ActualPixelsSize = 0;

		virtual ~BasicElement()
		{
			ctx.cancel();
			loading_task.wait();
		}

	protected:
		State state = State::Uninitialized;
		pplx::task<void> loading_task;
		pplx::cancellation_token_source ctx;
	};

	class MainMenu
	{
	public:
		MainMenu();
		virtual auto display(ActualPixelsRectangle clipping) -> ActualPixelsSize;

	private:
		std::unique_ptr<BasicElement> main_content;
	};
}