#pragma once

#include <string>
#include <unordered_map>
#include <memory>
#include <mutex>
#include <filesystem>
#include <array>

#include <SDL2/SDL_ttf.h>
#include <cpprest/json.h>

#include "Deleters.h"

class FontManager
{
public:
	void Initialize();

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
		if (auto file = font_files.find(name); file != font_files.end())
		{
			std::scoped_lock<std::mutex> lc{ write_mtx };

			auto key = name + std::to_string(size);
			return fonts.insert_or_assign(key, std::unique_ptr<TTF_Font>(TTF_OpenFont(utility::conversions::to_utf8string(file->second).c_str(), size))).first->second.get();
		}
		else
		{
			std::cerr << "Could not find font " << name << '\n';
			return nullptr;
		}

	}

private:
	std::mutex write_mtx;
	std::unordered_map<std::string, std::unique_ptr<TTF_Font>> fonts;

	std::unordered_map<std::string, std::filesystem::path> font_files;
	constexpr static std::array supported_extensions{ ".ttf", ".ttc", ".fon" };
};

