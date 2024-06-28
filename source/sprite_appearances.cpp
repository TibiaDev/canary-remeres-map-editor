//////////////////////////////////////////////////////////////////////
// This file is part of Canary Map Editor
//////////////////////////////////////////////////////////////////////
// Canary Map Editor is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// Canary Map Editor is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program. If not, see <http://www.gnu.org/licenses/>.
//////////////////////////////////////////////////////////////////////

#include "main.h"

#include "sprite_appearances.h"
#include "settings.h"
#include "filehandle.h"
#include "gui.h"

#include <lzma.h>

// APPEARANCES
#define BYTES_IN_SPRITE_SHEET 384 * 384 * 4
#define LZMA_UNCOMPRESSED_SIZE BYTES_IN_SPRITE_SHEET + 122
#define LZMA_HEADER_SIZE LZMA_PROPS_SIZE + 8
#define SPRITE_SHEET_WIDTH_BYTES 384 * 4

namespace fs = std::filesystem;

SpriteAppearances g_spriteAppearances;

void SpriteAppearances::init() {
	// in tibia 12.81 there is currently 3482 sheets
	sheets.reserve(4000);
}

void SpriteAppearances::terminate() {
	unload();
}

bool SpriteAppearances::loadCatalogContent(const std::string &dir, bool loadData /* true*/) {
	using json = nlohmann::json;
	fs::path catalogPath = fs::path(dir) / fs::path("catalog-content.json");
	if (!fs::exists(catalogPath)) {
		spdlog::error("catalog-content.json is not present in given directory. {}", catalogPath.string().c_str());
		return false;
	}

	std::ifstream file(catalogPath, std::ios::in);
	if (!file.is_open()) {
		spdlog::error("Unable to open catalog-content.json.");
		return false;
	}

	json document = json::parse(file, nullptr, false);

	file.close();

	for (const auto &obj : document) {
		const auto &type = obj["type"];
		if (type == "appearances") {
			appearanceFile = obj["file"];
		} else if (type == "sprite") {
			int lastSpriteId = obj["lastspriteid"].get<int>();

			SpriteSheetPtr sheet = SpriteSheetPtr(new SpriteSheet(obj["firstspriteid"].get<int>(), lastSpriteId, static_cast<SpriteLayout>(obj["spritetype"].get<int>()), (fs::path(dir) / fs::path(obj["file"].get<std::string>())).string()));
			sheets.push_back(sheet);

			spritesCount = std::max<int>(spritesCount, lastSpriteId);

			if (loadData) {
				if (!loadSpriteSheet(sheet)) {
					spdlog::error("[SpriteAppearances::loadCatalogContent] - Unable to load sprite sheet");
					return false;
				}
			}
		}
	}
	return true;
}

bool SpriteAppearances::loadSpriteSheet(const SpriteSheetPtr &sheet) {
	if (sheet->loaded) {
		return false;
	}

	std::ifstream file(sheet->path, std::ios::binary | std::ios::in);
	if (!file.is_open()) {
		spdlog::error("[SpriteAppearances::loadSpriteSheet] - Unable to open given sheets files");
		return false;
	}

	std::vector<uint8_t> buffer((std::istreambuf_iterator<char>(file)), (std::istreambuf_iterator<char>()));

	int pos = 0;

	file.close();

	/*
	   CIP's header, always 32 (0x20) bytes.
	   Header format:
	   [0x00, X):		  A variable number of NULL (0x00) bytes. The amount of pad-bytes can vary depending on how many
						   bytes the "7-bit integer encoded LZMA file size" take.
	   [X, X + 0x05):	  The constant byte sequence [0x70 0x0A 0xFA 0x80 0x24]
	   [X + 0x05, 0x20]:   LZMA file size (Note: excluding the 32 bytes of this header) encoded as a 7-bit integer
   */

	while (buffer[pos++] == 0x00)
		;
	pos += 4;
	while ((buffer[pos++] & 0x80) == 0x80)
		;

	uint8_t lclppb = buffer[pos++];

	lzma_options_lzma options {};
	options.lc = lclppb % 9;

	int remainder = lclppb / 9;
	options.lp = remainder % 5;
	options.pb = remainder / 5;

	uint32_t dictionarySize = 0;
	for (uint8_t i = 0; i < 4; ++i) {
		dictionarySize += buffer[pos++] << (i * 8);
	}

	options.dict_size = dictionarySize;

	pos += 8; // cip compressed size

	lzma_stream stream = LZMA_STREAM_INIT;

	lzma_filter filters[2] = {
		lzma_filter { LZMA_FILTER_LZMA1, &options },
		lzma_filter { LZMA_VLI_UNKNOWN, NULL }
	};

	lzma_ret ret = lzma_raw_decoder(&stream, filters);
	if (ret != LZMA_OK) {
		spdlog::error("Failed to initialize lzma raw decoder result: {}", static_cast<int>(ret));
		return false;
	}

	std::unique_ptr<uint8_t[]> decompressed = std::make_unique<uint8_t[]>(LZMA_UNCOMPRESSED_SIZE); // uncompressed size, bmp file + 122 bytes header

	stream.next_in = &buffer[pos];
	stream.next_out = decompressed.get();
	stream.avail_in = buffer.size();
	stream.avail_out = LZMA_UNCOMPRESSED_SIZE;

	ret = lzma_code(&stream, LZMA_RUN);
	if (ret != LZMA_STREAM_END) {
		spdlog::error("Failed to decode lzma buffer result: {}", static_cast<int>(ret));
		return false;
	}

	lzma_end(&stream); // free memory

	// pixel data start (bmp header end offset)
	uint32_t data;
	std::memcpy(&data, decompressed.get() + 10, sizeof(uint32_t));

	uint8_t* bufferStart = decompressed.get() + data;

	// Flip vertically
	for (int y = 0; y < 192; ++y) {
		uint8_t* itr1 = &bufferStart[y * SPRITE_SHEET_WIDTH_BYTES];
		uint8_t* itr2 = &bufferStart[(384 - y - 1) * SPRITE_SHEET_WIDTH_BYTES];

		for (std::size_t x = 0; x < SPRITE_SHEET_WIDTH_BYTES; ++x) {
			std::swap(*(itr1 + x), *(itr2 + x));
		}
	}

	sheet->data = std::make_unique<uint8_t[]>(LZMA_UNCOMPRESSED_SIZE);
	std::memcpy(sheet->data.get(), bufferStart, BYTES_IN_SPRITE_SHEET);

	sheet->loaded = true;
	return true;
}

void SpriteAppearances::unload() {
	spritesCount = 0;
	sheets.clear();
}

SpriteSheetPtr SpriteAppearances::getSheetBySpriteId(int id, bool load /* = true */) {
	if (id == 0) {
		return nullptr;
	}

	// find sheet
	auto sheetIt = std::find_if(sheets.begin(), sheets.end(), [=](const SpriteSheetPtr &sheet) {
		return id >= sheet->firstId && id <= sheet->lastId;
	});

	if (sheetIt == sheets.end()) {
		return nullptr;
	}

	const SpriteSheetPtr &sheet = *sheetIt;
	if (load && !sheet->loaded) {
		loadSpriteSheet(sheet);
	}

	return sheet;
}

wxImage SpriteAppearances::getWxImageBySpriteId(int id, bool toSavePng /* = false*/) {
	const auto &sprite = getSprite(id);
	if (!sprite) {
		spdlog::error("[{}] - Unknown sprite id", __func__);
		return {};
	}

	const int bgshade = g_settings.getInteger(Config::ICON_BACKGROUND);
	const uint32_t magenta = 0xFF00FF;
	const uint32_t lightMagenta = 0xD000CF;

	const int width = sprite->size.width;
	const int height = sprite->size.height;
	auto pixels = sprite->pixels.data();
	wxImage image(width, height);
	for (int y = 0; y < height; ++y) {
		for (int x = 0; x < width; ++x) {
			const int index = (y * width + x) * 4;
			uint8_t r = pixels[index + 2];
			uint8_t g = pixels[index + 1];
			uint8_t b = pixels[index];

			// Combines the color channels into a single 32-bit value
			uint32_t color = (r << 16) | (g << 8) | b;

			// Replaces magenta with the background color
			if (color == magenta || color == lightMagenta) {
				r = g = b = bgshade; // Sets RGB to the background color
			}
			image.SetRGB(x, y, r, g, b);
		}
	}

	return image;
}

SpritePtr SpriteAppearances::getSprite(int spriteId) {
	// caching
	auto it = sprites.find(spriteId);
	if (it != sprites.end()) {
		return it->second;
	}

	const auto &sheet = getSheetBySpriteId(spriteId);
	if (!sheet || !sheet->loaded) {
		return nullptr;
	}

	auto spriteWidth = sheet->getSpriteSize().width;
	auto spriteHeight = sheet->getSpriteSize().height;

	SpritePtr sprite = SpritePtr(new Sprites(spriteWidth, spriteHeight));

	int spriteOffset = spriteId - sheet->firstId;
	int allColumns = spriteWidth == 32 ? 12 : 6; // 64 pixel width == 6 columns each 64x or 32 pixels, 12 columns
	int spriteRow = static_cast<int>(std::floor(static_cast<float>(spriteOffset) / static_cast<float>(allColumns)));
	int spriteColumn = spriteOffset % allColumns;

	int spriteWidthBytes = spriteWidth * 4;

	for (int height = spriteHeight * spriteRow, offset = 0; height < spriteHeight + (spriteRow * spriteHeight); height++, offset++) {
		auto bufferData = &sheet->data[(height * SPRITE_SHEET_WIDTH_BYTES) + (spriteColumn * spriteWidthBytes)];
		std::memcpy(&sprite->pixels[offset * spriteWidthBytes], bufferData, spriteWidthBytes);
	}

	// cache it for faster later access
	sprites[spriteId] = sprite;

	return sprite;
}