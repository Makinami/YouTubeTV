#pragma once

#include <memory>

#include <cpprest/http_client.h>

namespace YouTube::UI
{
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
		virtual auto display(Position origin) -> Dimensions = 0;

	protected:
		State state = State::Uninitialized;
		pplx::task<void> loading_task;
	};

	class MainMenu : public BasicElement
	{
	public:
		MainMenu();
		virtual auto display(Position origin) -> Dimensions;

	private:
		std::unique_ptr<BasicElement> main_content;
	};
}