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

#include "mdl/MapSidecar.h"

#include "fs/DiskIO.h"
#include "fs/PathInfo.h"
#include "mdl/BrushNode.h"
#include "mdl/Entity.h"
#include "mdl/EntityNode.h"
#include "mdl/GroupNode.h"
#include "mdl/LayerNode.h"
#include "mdl/PatchNode.h"
#include "mdl/SplineEntity.h"
#include "mdl/TerrainEntity.h"
#include "mdl/WorldNode.h"

#include "kd/overload.h"
#include "kd/reflection_impl.h"
#include "kd/result.h"
#include "kd/string_compare.h"
#include "kd/string_format.h"
#include "kd/string_utils.h"

#include <fmt/format.h>

#include <algorithm>
#include <cctype>
#include <map>
#include <ostream>
#include <random>
#include <set>
#include <sstream>

namespace tb::mdl
{
namespace
{

constexpr auto SidecarHeader = "// TrenchBroom tool data";

} // namespace

std::string quoteSidecarString(const std::string& str)
{
  auto result = std::string{"\""};
  for (const auto c : str)
  {
    if (c == '"' || c == '\\')
    {
      result += '\\';
    }
    result += c;
  }
  result += '"';
  return result;
}

std::optional<std::string> unquoteSidecarString(const std::string& str, size_t& position)
{
  if (position >= str.size() || str[position] != '"')
  {
    return std::nullopt;
  }

  auto result = std::string{};
  for (++position; position < str.size(); ++position)
  {
    const auto c = str[position];
    if (c == '\\' && position + 1 < str.size())
    {
      result += str[++position];
    }
    else if (c == '"')
    {
      ++position;
      return result;
    }
    else
    {
      result += c;
    }
  }
  return std::nullopt;
}

kdl_reflect_impl(SidecarRecord);

const std::vector<std::string>& sidecarPropertyPrefixes()
{
  static const auto prefixes = std::vector<std::string>{
    TerrainPropertyKeys::HeightsPrefix,
    TerrainPropertyKeys::MaterialsPrefix,
    TerrainPropertyKeys::Origin,
    TerrainPropertyKeys::CellSize,
    TerrainPropertyKeys::Columns,
    TerrainPropertyKeys::Rows,
    TerrainPropertyKeys::TexScale,
    TerrainPropertyKeys::DefaultMaterial,
    SplinePropertyKeys::PointPrefix,
    SplinePropertyKeys::TemplateBrushPrefix,
    SplinePropertyKeys::TemplateEntityPrefix,
    SplinePropertyKeys::Subdivisions,
    SplinePropertyKeys::Closed,
    SplinePropertyKeys::LockUVs,
    SplinePropertyKeys::TemplateGroupId,
  };
  return prefixes;
}

bool isSidecarPropertyKey(const std::string& key)
{
  const auto& prefixes = sidecarPropertyPrefixes();
  return std::ranges::any_of(
    prefixes, [&](const auto& prefix) { return kdl::cs::str_is_prefix(key, prefix); });
}

std::filesystem::path sidecarPathForMap(const std::filesystem::path& mapPath)
{
  // Appended rather than substituted for the extension, so that "level.map" and
  // "level.bsp" in the same folder cannot share a sidecar.
  auto path = mapPath;
  path += ".tbtools";
  return path;
}

std::string generateSidecarId()
{
  static auto engine = std::mt19937_64{std::random_device{}()};
  return fmt::format("{:016x}", engine());
}

bool assignSidecarIds(WorldNode& worldNode)
{
  auto seen = std::set<std::string>{};
  auto changed = false;

  worldNode.accept(kdl::overload(
    [](auto&& thisLambda, WorldNode& node) { node.visitChildren(thisLambda); },
    [](auto&& thisLambda, LayerNode& node) { node.visitChildren(thisLambda); },
    [](auto&& thisLambda, GroupNode& node) { node.visitChildren(thisLambda); },
    [&](EntityNode& entityNode) {
      const auto& entity = entityNode.entity();
      if (std::ranges::none_of(entity.properties(), [](const auto& property) {
            return isSidecarPropertyKey(property.key());
          }))
      {
        return;
      }

      const auto* id = entity.property(SidecarPropertyKeys::DataId);
      if (id && !id->empty() && seen.insert(*id).second)
      {
        return;
      }

      auto newId = generateSidecarId();
      while (!seen.insert(newId).second)
      {
        newId = generateSidecarId();
      }

      auto newEntity = entity;
      newEntity.addOrUpdateProperty(SidecarPropertyKeys::DataId, std::move(newId));
      entityNode.setEntity(std::move(newEntity));
      changed = true;
    },
    [](BrushNode&) {},
    [](PatchNode&) {}));

  return changed;
}

std::vector<SidecarRecord> collectSidecarRecords(const WorldNode& worldNode)
{
  auto records = std::vector<SidecarRecord>{};

  worldNode.accept(kdl::overload(
    [](auto&& thisLambda, const WorldNode& node) { node.visitChildren(thisLambda); },
    [](auto&& thisLambda, const LayerNode& node) { node.visitChildren(thisLambda); },
    [](auto&& thisLambda, const GroupNode& node) { node.visitChildren(thisLambda); },
    [&](const EntityNode& entityNode) {
      const auto& entity = entityNode.entity();
      const auto* id = entity.property(SidecarPropertyKeys::DataId);
      if (!id || id->empty())
      {
        return;
      }

      auto properties = std::vector<EntityProperty>{};
      for (const auto& property : entity.properties())
      {
        if (isSidecarPropertyKey(property.key()))
        {
          properties.push_back(property);
        }
      }

      if (!properties.empty())
      {
        records.push_back(SidecarRecord{*id, std::move(properties)});
      }
    },
    [](const BrushNode&) {},
    [](const PatchNode&) {}));

  return records;
}

void applySidecarRecords(WorldNode& worldNode, const std::vector<SidecarRecord>& records)
{
  auto byId = std::map<std::string, const SidecarRecord*>{};
  for (const auto& record : records)
  {
    byId.emplace(record.id, &record);
  }

  worldNode.accept(kdl::overload(
    [](auto&& thisLambda, WorldNode& node) { node.visitChildren(thisLambda); },
    [](auto&& thisLambda, LayerNode& node) { node.visitChildren(thisLambda); },
    [](auto&& thisLambda, GroupNode& node) { node.visitChildren(thisLambda); },
    [&](EntityNode& entityNode) {
      const auto* id = entityNode.entity().property(SidecarPropertyKeys::DataId);
      if (!id)
      {
        return;
      }

      const auto it = byId.find(*id);
      if (it == byId.end())
      {
        // Without a record the entity keeps only its generated brushes, which stay in
        // the map as ordinary geometry.
        return;
      }

      auto entity = entityNode.entity();
      for (const auto& property : it->second->properties)
      {
        entity.addOrUpdateProperty(property.key(), property.value());
      }
      entityNode.setEntity(std::move(entity));
    },
    [](BrushNode&) {},
    [](PatchNode&) {}));
}

std::string serializeSidecar(const std::vector<SidecarRecord>& records)
{
  auto stream = std::ostringstream{};
  stream << SidecarHeader << "\n";
  stream << "// Written by TrenchBroom next to the map file. Deleting it leaves the\n";
  stream << "// map's geometry intact but makes its terrains and splines uneditable.\n";

  for (const auto& record : records)
  {
    stream << "{\n";
    stream << quoteSidecarString(SidecarPropertyKeys::DataId) << " "
           << quoteSidecarString(record.id) << "\n";
    for (const auto& property : record.properties)
    {
      stream << quoteSidecarString(property.key()) << " "
             << quoteSidecarString(property.value()) << "\n";
    }
    stream << "}\n";
  }

  return stream.str();
}

Result<std::vector<SidecarRecord>> parseSidecar(const std::string& str)
{
  auto records = std::vector<SidecarRecord>{};
  auto record = std::optional<SidecarRecord>{};

  for (const auto& rawLine : kdl::str_split(str, "\n"))
  {
    const auto line = kdl::str_trim(rawLine);
    if (line.empty() || kdl::cs::str_is_prefix(line, "//"))
    {
      continue;
    }

    if (line == "{")
    {
      if (record)
      {
        return Error{"Unexpected '{' inside a tool data record"};
      }
      record = SidecarRecord{};
      continue;
    }

    if (line == "}")
    {
      if (!record)
      {
        return Error{"Unexpected '}' outside of a tool data record"};
      }
      if (record->id.empty())
      {
        return Error{"Tool data record without an id"};
      }
      records.push_back(std::move(*record));
      record = std::nullopt;
      continue;
    }

    if (!record)
    {
      return Error{"Tool data property outside of a record"};
    }

    auto position = size_t(0);
    const auto key = unquoteSidecarString(line, position);
    if (!key)
    {
      return Error{"Malformed tool data property key"};
    }

    while (position < line.size()
           && std::isspace(static_cast<unsigned char>(line[position])))
    {
      ++position;
    }

    const auto value = unquoteSidecarString(line, position);
    if (!value)
    {
      return Error{"Malformed tool data property value"};
    }

    if (*key == SidecarPropertyKeys::DataId)
    {
      record->id = *value;
    }
    else
    {
      record->properties.emplace_back(*key, *value);
    }
  }

  if (record)
  {
    return Error{"Unterminated tool data record"};
  }

  return records;
}

Result<void> writeSidecarFile(
  const std::filesystem::path& path, const std::vector<SidecarRecord>& records)
{
  if (records.empty())
  {
    // A map that no longer holds any tool data must not leave a stale sidecar behind,
    // which would be applied to whatever ids happen to match later.
    return fs::Disk::pathInfo(path) == fs::PathInfo::File
             ? fs::Disk::deleteFile(path) | kdl::transform([](auto) {})
             : Result<void>{};
  }

  const auto contents = serializeSidecar(records);
  return fs::Disk::withOutputStream(path, [&](auto& stream) { stream << contents; });
}

Result<std::vector<SidecarRecord>> readSidecarFile(const std::filesystem::path& path)
{
  if (fs::Disk::pathInfo(path) != fs::PathInfo::File)
  {
    return std::vector<SidecarRecord>{};
  }

  return fs::Disk::withInputStream(
           path,
           [](auto& stream) {
             return std::string{
               std::istreambuf_iterator<char>{stream}, std::istreambuf_iterator<char>{}};
           })
         | kdl::and_then([](const auto& contents) { return parseSidecar(contents); });
}

} // namespace tb::mdl
