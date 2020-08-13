#include "pch.h"

#include "FontManager.h"

#include <algorithm>
#include <numeric>
#include <utf8.h>

using namespace std::string_literals;

void FontManager::Initialize()
{
	for (std::filesystem::path directory : { "./fonts", "C:\\Windows\\Fonts" })
	{
		if (!exists(directory))
		{
			spdlog::debug(L"FontManager: "s + directory.c_str() + L" does not exist. Skipping.");
			continue;
		}

		for (auto& file : std::filesystem::directory_iterator(directory))
		{
			auto font = std::unique_ptr<TTF_Font>(TTF_OpenFont(utility::conversions::to_utf8string(file.path()).c_str(), 12));
			if (font == nullptr)
				continue;

			auto family = std::string{ TTF_FontFaceFamilyName(font.get()) };
			auto style = TTF_FontFaceStyleName(font.get());
			font_files.insert({ family + ' ' + style, file.path() });
		}
	}

	spdlog::debug("FontManager: {} fonts found: {}", font_files.size(), std::accumulate(font_files.begin(), font_files.end(), ""s, [](std::string list, const auto& font) {
		if (list.size())
			return std::move(list) + ", " + font.first;
		else
			return font.first;
	}));
}
