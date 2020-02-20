#pragma once

#include <string>
#include <unordered_map>
#include <memory>
#include <mutex>

#include <SDL2/SDL_ttf.h>

#include "Deleters.h"

class FontManager
{
public:
	auto get_font(const std::string& name, int size) -> TTF_Font*
	{
		auto key = name + std::to_string(size);
		if (auto it = fonts.find(key); it != fonts.end())
			return it->second.get();
		else
			return load_font(name, size);
	}

	void clear()
	{
		std::scoped_lock<std::mutex> lc{ write_mtx };
		fonts.clear();
	}

private:
	auto load_font(const std::string& name, int size) -> TTF_Font*
	{
		std::scoped_lock<std::mutex> lc{ write_mtx };

		auto key = name + std::to_string(size);
		return fonts.insert_or_assign(key, std::unique_ptr<TTF_Font>(TTF_OpenFont(name.c_str(), size))).first->second.get();
	}

private:
	std::mutex write_mtx;
	std::unordered_map<std::string, std::unique_ptr<TTF_Font>> fonts;
};

