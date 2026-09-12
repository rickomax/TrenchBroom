/*
 Copyright (C) 2026 Kristian Duske

 This file is part of TrenchBroom.

 TrenchBroom is free software: you can redistribute it and/or modify
 it under the terms of the GNU General Public License as published by
 the Free Software Foundation, either version 3 of the License, or
 (at your option) any later version.

 TrenchBroom is distributed in the hope that it will be useful,
 but WITHOUT ANY WARRANTY; without even the implied warranty of
 MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 GNU General Public License for more details.

 You should have received a copy of the GNU General Public License
 along with TrenchBroom. If not, see <http://www.gnu.org/licenses/>.
 */

#include "mdl/CustomTextures.h"
#include "mdl/Palette.h"

#include "kd/result.h"

#include <vector>

#include <catch2/catch_test_macros.hpp>

namespace tb::mdl
{
namespace
{

/**
 * A palette of 256 greys, except for the transparent index, which is the bright pink
 * the standard palette puts there: a pixel quantized onto it by mistake is then
 * unmistakable.
 */
Palette makeTestPalette()
{
  auto data = std::vector<unsigned char>(256 * 3);
  for (size_t i = 0; i < 256; ++i)
  {
    data[i * 3 + 0] = uint8_t(i);
    data[i * 3 + 1] = uint8_t(i);
    data[i * 3 + 2] = uint8_t(i);
  }
  data[255 * 3 + 0] = 159;
  data[255 * 3 + 1] = 91;
  data[255 * 3 + 2] = 83;

  return makePalette(data, PaletteColorFormat::Rgb) | kdl::value();
}

/**
 * A palette whose first two colours are pure red and pure blue and whose rest is black,
 * for telling apart two channel orders that a grey palette would quantize identically.
 */
Palette makeColorPalette()
{
  auto data = std::vector<unsigned char>(256 * 3, 0);
  data[0] = 255; // index 0 is red
  data[5] = 255; // index 1 is blue
  return makePalette(data, PaletteColorFormat::Rgb) | kdl::value();
}

/** An RGBA image of one colour, with the given alpha. */
std::vector<uint8_t> makeImage(
  const size_t width, const size_t height, const uint8_t grey, const uint8_t alpha = 255)
{
  auto image = std::vector<uint8_t>(width * height * 4);
  for (size_t i = 0; i < width * height; ++i)
  {
    image[i * 4 + 0] = grey;
    image[i * 4 + 1] = grey;
    image[i * 4 + 2] = grey;
    image[i * 4 + 3] = alpha;
  }
  return image;
}

} // namespace

TEST_CASE("makeCustomTextureName")
{
  SECTION("a name is lowercased and stripped of what a wad cannot hold")
  {
    CHECK(makeCustomTextureName("Brick", false) == "brick");
    CHECK(makeCustomTextureName("My Texture", false) == "my_texture");
    CHECK(makeCustomTextureName("wall.01", false) == "wall_01");
    // The characters a compiler reads meaning from are kept.
    CHECK(makeCustomTextureName("*water1", false) == "*water1");
    CHECK(makeCustomTextureName("+0button", false) == "+0button");
  }

  SECTION("the underscores an odd name leaves at either end are trimmed")
  {
    CHECK(makeCustomTextureName("(brick)", false) == "brick");
    CHECK(makeCustomTextureName("  spaced  ", false) == "spaced");
  }

  SECTION("a name that survives nothing still names something")
  {
    CHECK(makeCustomTextureName("()", false) == "texture");
    CHECK(makeCustomTextureName("", false) == "texture");
  }

  SECTION("a masked texture gets the prefix an engine looks for")
  {
    CHECK(makeCustomTextureName("grate", true) == "{grate");
    // A prefix in the source name is not doubled.
    CHECK(makeCustomTextureName("{grate", true) == "{grate");
    CHECK(makeCustomTextureName("{grate", false) == "grate");
  }

  SECTION("a name is cut to what a wad stores, leaving room for the prefix")
  {
    CHECK(makeCustomTextureName("123456789012345678", false) == "123456789012345");
    CHECK(makeCustomTextureName("123456789012345678", true) == "{12345678901234");
    CHECK(makeCustomTextureName("123456789012345678", false).size() == 15u);
    CHECK(makeCustomTextureName("123456789012345678", true).size() == 15u);
  }
}

TEST_CASE("quantizeCustomTexture")
{
  const auto palette = makeTestPalette();

  SECTION("a texture must be a multiple of sixteen on both sides")
  {
    CHECK(
      quantizeCustomTexture("t", 20, 16, makeImage(20, 16, 128).data(), false, palette)
        .is_error());
    CHECK(
      quantizeCustomTexture("t", 16, 20, makeImage(16, 20, 128).data(), false, palette)
        .is_error());
    CHECK(quantizeCustomTexture("t", 0, 0, makeImage(16, 16, 128).data(), false, palette)
            .is_error());
    CHECK(
      quantizeCustomTexture("t", 16, 32, makeImage(16, 32, 128).data(), false, palette)
        .is_success());
  }

  SECTION("an opaque image quantizes onto the palette and is not masked")
  {
    const auto image = makeImage(16, 16, 128);
    const auto result =
      quantizeCustomTexture("brick", 16, 16, image.data(), false, palette) | kdl::value();

    CHECK(result.name == "brick");
    CHECK(result.width == 16u);
    CHECK(result.height == 16u);
    CHECK_FALSE(result.masked);
    REQUIRE(result.mipLevels.size() == CustomTextureMipLevels);
    CHECK(result.mipLevels[0].front() == 128);
  }

  SECTION("the mip levels halve twice")
  {
    const auto image = makeImage(64, 32, 200);
    const auto result =
      quantizeCustomTexture("t", 64, 32, image.data(), false, palette) | kdl::value();

    REQUIRE(result.mipLevels.size() == 4u);
    CHECK(result.mipLevels[0].size() == 64u * 32u);
    CHECK(result.mipLevels[1].size() == 32u * 16u);
    CHECK(result.mipLevels[2].size() == 16u * 8u);
    CHECK(result.mipLevels[3].size() == 8u * 4u);

    // One flat colour stays that colour all the way down.
    for (const auto& level : result.mipLevels)
    {
      for (const auto index : level)
      {
        CHECK(index == 200);
      }
    }
  }

  SECTION(
    "a fully transparent pixel becomes the transparent index and masks the "
    "texture")
  {
    auto image = makeImage(16, 16, 128);
    image[3] = 0; // the first pixel's alpha

    const auto result =
      quantizeCustomTexture("grate", 16, 16, image.data(), false, palette) | kdl::value();

    CHECK(result.masked);
    CHECK(result.name == "{grate");
    CHECK(result.mipLevels[0][0] == PaletteTransparentIndex);
    CHECK(result.mipLevels[0][1] == 128);
  }

  SECTION("an opaque pixel is never quantized onto the transparent index")
  {
    // The exact colour the test palette puts at the transparent index: the nearest
    // other colour has to be chosen instead, or an opaque pixel would become a hole.
    auto image = std::vector<uint8_t>(16 * 16 * 4);
    for (size_t i = 0; i < 16 * 16; ++i)
    {
      image[i * 4 + 0] = 159;
      image[i * 4 + 1] = 91;
      image[i * 4 + 2] = 83;
      image[i * 4 + 3] = 255;
    }

    const auto result =
      quantizeCustomTexture("t", 16, 16, image.data(), false, palette) | kdl::value();

    CHECK_FALSE(result.masked);
    for (const auto index : result.mipLevels[0])
    {
      CHECK(index != PaletteTransparentIndex);
    }
  }

  SECTION("a mip level averages only the opaque pixels above it")
  {
    // Half the image transparent, half a flat grey: the level below must be that grey
    // rather than something part way towards the transparent index's colour.
    auto image = makeImage(16, 16, 100);
    for (size_t y = 0; y < 16; ++y)
    {
      for (size_t x = 0; x < 8; ++x)
      {
        image[(y * 16 + x) * 4 + 3] = 0;
      }
    }

    const auto result =
      quantizeCustomTexture("t", 16, 16, image.data(), false, palette) | kdl::value();

    REQUIRE(result.mipLevels[1].size() == 8u * 8u);
    for (size_t y = 0; y < 8; ++y)
    {
      // The left four columns came from four transparent pixels each.
      for (size_t x = 0; x < 4; ++x)
      {
        CHECK(result.mipLevels[1][y * 8 + x] == PaletteTransparentIndex);
      }
      for (size_t x = 4; x < 8; ++x)
      {
        CHECK(result.mipLevels[1][y * 8 + x] == 100);
      }
    }
  }

  SECTION("a BGRA source is read in its own order")
  {
    const auto colorPalette = makeColorPalette();

    // Mostly red in the first channel and mostly blue in the third.
    auto image = std::vector<uint8_t>(16 * 16 * 4);
    for (size_t i = 0; i < 16 * 16; ++i)
    {
      image[i * 4 + 0] = 200;
      image[i * 4 + 1] = 10;
      image[i * 4 + 2] = 10;
      image[i * 4 + 3] = 255;
    }

    const auto asRgba =
      quantizeCustomTexture("t", 16, 16, image.data(), false, colorPalette)
      | kdl::value();
    const auto asBgra =
      quantizeCustomTexture("t", 16, 16, image.data(), true, colorPalette) | kdl::value();

    // Read as RGBA the pixel is red; read as BGRA the same bytes are blue.
    CHECK(asRgba.mipLevels[0][0] == 0);
    CHECK(asBgra.mipLevels[0][0] == 1);
  }
}

TEST_CASE("makeCustomTextureLump")
{
  const auto palette = makeTestPalette();
  const auto image = makeImage(32, 16, 77);
  const auto customTexture =
    quantizeCustomTexture("brick", 32, 16, image.data(), false, palette) | kdl::value();

  const auto lump = makeCustomTextureLump(customTexture);

  CHECK(lump.name == "brick");
  CHECK(lump.type == WadMipTextureType);

  const auto readUint32 = [&](const size_t offset) {
    return uint32_t(lump.data[offset]) | uint32_t(lump.data[offset + 1]) << 8
           | uint32_t(lump.data[offset + 2]) << 16
           | uint32_t(lump.data[offset + 3]) << 24;
  };

  // The lump starts with the sixteen byte name, then the two dimensions and the four
  // level offsets, all relative to the start of the lump.
  CHECK(std::string{reinterpret_cast<const char*>(lump.data.data())} == "brick");
  CHECK(readUint32(16) == 32u);
  CHECK(readUint32(20) == 16u);

  const auto headerSize = size_t(16 + 4 * 6);
  CHECK(readUint32(24) == headerSize);
  CHECK(readUint32(28) == headerSize + 32u * 16u);
  CHECK(readUint32(32) == headerSize + 32u * 16u + 16u * 8u);
  CHECK(readUint32(36) == headerSize + 32u * 16u + 16u * 8u + 8u * 4u);

  CHECK(lump.data.size() == headerSize + 32 * 16 + 16 * 8 + 8 * 4 + 4 * 2);
  // The pixels follow the header, starting with the full size level.
  CHECK(lump.data[headerSize] == 77);
}

} // namespace tb::mdl
