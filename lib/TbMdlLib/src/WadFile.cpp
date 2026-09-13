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

#include "mdl/WadFile.h"

#include "fs/DiskIO.h"
#include "fs/File.h"
#include "fs/Reader.h"
#include "fs/ReaderException.h"

#include "kd/reflection_impl.h"
#include "kd/result.h"
#include "kd/string_format.h"
#include "kd/string_utils.h"

#include <fmt/format.h>

#include <algorithm>
#include <array>
#include <ostream>

namespace tb::mdl
{
namespace
{

/** The wad2 layout, which wad3 shares: a twelve byte header and a directory of thirty
 * two byte entries, with the lump data in between. */
constexpr auto HeaderSize = size_t(12);
constexpr auto DirectoryEntrySize = size_t(32);
/** The name field is sixteen bytes and the name has to be terminated, so fifteen
 * characters of it survive. */
constexpr auto NameSize = size_t(16);
constexpr auto MaxNameLength = NameSize - 1;

void writeUint32(std::ostream& stream, const uint32_t value)
{
  // Little endian, which is what the format stores regardless of the host.
  const auto bytes = std::array<char, 4>{
    char(value & 0xFF),
    char((value >> 8) & 0xFF),
    char((value >> 16) & 0xFF),
    char((value >> 24) & 0xFF)};
  stream.write(bytes.data(), std::streamsize(bytes.size()));
}

void writeName(std::ostream& stream, const std::string& name)
{
  auto bytes = std::array<char, NameSize>{};
  const auto length = std::min(name.size(), MaxNameLength);
  std::copy_n(name.begin(), length, bytes.begin());
  stream.write(bytes.data(), std::streamsize(bytes.size()));
}

} // namespace

kdl_reflect_impl(WadLump);

namespace
{

Result<std::vector<WadLump>> readLumps(
  fs::Reader& reader, const std::filesystem::path& path)
{
  try
  {
    if (reader.size() < HeaderSize)
    {
      return Error{fmt::format("'{}' is too small to be a wad file", path)};
    }

    reader.seekFromBegin(0);
    const auto magic = reader.readString(4);
    if (kdl::str_to_lower(magic) != "wad2" && kdl::str_to_lower(magic) != "wad3")
    {
      return Error{fmt::format("'{}' is not a wad file", path)};
    }

    const auto entryCount = reader.readSize<int32_t>();
    const auto directoryOffset = reader.readSize<int32_t>();

    if (reader.size() < directoryOffset + entryCount * DirectoryEntrySize)
    {
      return Error{fmt::format("'{}' has a truncated directory", path)};
    }

    auto lumps = std::vector<WadLump>{};
    lumps.reserve(entryCount);

    for (size_t i = 0; i < entryCount; ++i)
    {
      reader.seekFromBegin(directoryOffset + i * DirectoryEntrySize);

      const auto lumpOffset = reader.readSize<int32_t>();
      // The on disk size comes first and the unpacked size second; they differ only for
      // compressed lumps, which nothing writes.
      const auto lumpSize = reader.readSize<int32_t>();
      reader.seekForward(4);
      const auto type = reader.readString(1);
      reader.seekForward(3);
      auto name = reader.readString(NameSize);

      if (reader.size() < lumpOffset + lumpSize)
      {
        return Error{fmt::format("'{}' has an entry that runs past its end", path)};
      }

      auto data = std::vector<uint8_t>(lumpSize);
      if (lumpSize > 0)
      {
        reader.seekFromBegin(lumpOffset);
        reader.read(data.data(), data.size());
      }

      lumps.push_back(
        WadLump{std::move(name), type.empty() ? char(0) : type[0], std::move(data)});
    }

    return lumps;
  }
  catch (const fs::ReaderException& e)
  {
    return Error{fmt::format("Could not read '{}': {}", path, e.what())};
  }
}

} // namespace

Result<std::vector<WadLump>> readWadLumps(const std::filesystem::path& path)
{
  return fs::Disk::openFile(path)
         | kdl::and_then([&](const std::shared_ptr<fs::CFile>& file) {
             auto reader = file->reader();
             return readLumps(reader, path);
           });
}

Result<void> writeWad(
  const std::filesystem::path& path, const std::vector<WadLump>& lumps)
{
  // A wad addresses its lumps by name, so two lumps of one name would make the file
  // ambiguous. The later one wins, which is what makes writing an updated texture over
  // an older one work.
  const auto storedName = [](const WadLump& lump) {
    return kdl::str_to_lower(
      lump.name.substr(0, std::min(lump.name.size(), MaxNameLength)));
  };

  auto uniqueLumps = std::vector<const WadLump*>{};
  for (const auto& lump : lumps)
  {
    const auto name = storedName(lump);
    const auto existing = std::ranges::find_if(
      uniqueLumps,
      [&](const WadLump* candidate) { return storedName(*candidate) == name; });

    if (existing != uniqueLumps.end())
    {
      *existing = &lump;
    }
    else
    {
      uniqueLumps.push_back(&lump);
    }
  }

  return fs::Disk::withOutputStream(
           path,
           std::ios::out | std::ios::binary,
           [&](auto& stream) {
             auto directoryOffset = HeaderSize;
             for (const auto* lump : uniqueLumps)
             {
               directoryOffset += lump->data.size();
             }

             stream.write("WAD2", 4);
             writeUint32(stream, uint32_t(uniqueLumps.size()));
             writeUint32(stream, uint32_t(directoryOffset));

             auto offsets = std::vector<size_t>{};
             offsets.reserve(uniqueLumps.size());

             auto offset = HeaderSize;
             for (const auto* lump : uniqueLumps)
             {
               offsets.push_back(offset);
               if (!lump->data.empty())
               {
                 stream.write(
                   reinterpret_cast<const char*>(lump->data.data()),
                   std::streamsize(lump->data.size()));
               }
               offset += lump->data.size();
             }

             for (size_t i = 0; i < uniqueLumps.size(); ++i)
             {
               const auto* lump = uniqueLumps[i];
               writeUint32(stream, uint32_t(offsets[i]));
               // Uncompressed, so the stored size and the unpacked size are the same.
               writeUint32(stream, uint32_t(lump->data.size()));
               writeUint32(stream, uint32_t(lump->data.size()));
               stream.put(lump->type);
               // Compression, and two bytes of padding.
               stream.put(char(0));
               stream.put(char(0));
               stream.put(char(0));
               writeName(stream, lump->name);
             }
           })
         | kdl::transform_error([&](const auto& e) {
             return Error{fmt::format("Could not write '{}': {}", path, e.msg)};
           });
}

} // namespace tb::mdl
