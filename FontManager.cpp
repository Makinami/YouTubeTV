#include "pch.h"

#include "FontManager.h"

#include <algorithm>
#include <utf8.h>

void FontManager::Initialize()
{
	for (std::filesystem::path directory : { "./fonts", "C:\\Windows\\Fonts" })
	{
		if (!exists(directory))
		{
			std::cout << directory << " does not exist. Skipping.\n";
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

	std::cout << "Found fonts:\n";
	for (auto& font : font_files)
		std::cout << font.first << '\n';
}
