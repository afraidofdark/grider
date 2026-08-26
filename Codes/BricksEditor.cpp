/*
 * Copyright (c) 2019-2026 OtSoftware
 * This code is licensed under the GNU Lesser General Public License v3.0 (LGPL-3.0).
 * For more information, including options for a more permissive commercial license,
 * please visit [otsoftware.tr] or contact us at [info@otsoftare.tr].
 */

#include "BricksEditor.h"

#include <Editor/Source/App.h>
#include <Editor/Source/EditorScene.h>

#include <Entity.h>
#include <Logger.h>
#include <Material.h>
#include <MaterialComponent.h>
#include <Mesh.h>
#include <Prefab.h>
#include <Primative.h>
#include <Util.h>

#include <filesystem>

#include <cmath>
#include <map>
#include <set>
#include <unordered_map>
#include <unordered_set>

namespace ToolKit
{
  namespace Editor
  {

    TKDefineClass(BricksEditor, Window);

    namespace
    {
      // Fixed tile height. Tiles are always flat and short.
      constexpr float g_tileHeight = 0.2f;

      // Checker pair in gray tones. Deliberately not black/white: that reads as
      // a harsh contrast, the checker only needs to separate adjacent tiles.
      constexpr float g_checkerLight = 0.7f;
      constexpr float g_checkerDark  = 0.3f;

      // Bridge color: near black unlit so bridges read as structure against the
      // gray tiles.
      constexpr float g_bridgeColor = 0.02f;

      // Bridge width relative to the tile's cross-axis extent (5% of D).
      constexpr float g_bridgeThicknessRatio = 0.05f;

      // Lifts the bridge quads above the tile tops so they don't Z-fight with
      // the tile surface they sit on.
      constexpr float g_bridgeLift = 0.01f;

      // Rounds a float to millimetre precision for stable keys.
      String RoundKey(float v)
      {
        return std::to_string(static_cast<long long>(std::round(v * 1000.0f)));
      }
    }

    BricksEditor::BricksEditor()
    {
      m_name = "Bricks Editor";
    }

    BricksEditor::~BricksEditor()
    {
    }

    void BricksEditor::ParameterConstructor()
    {
      Super::ParameterConstructor();

      // D: width/depth of a tile (its height is fixed at g_tileHeight).
      TileSize_Define(5.0f, "Tile", 0, true, true);
      // N: horizontal repeat count, M: vertical repeat count.
      GridCols_Define(4, "Tile", 0, true, true);
      GridRows_Define(4, "Tile", 0, true, true);
    }

    EntityPtr BricksEditor::GetTileDataEntity(EntityPtr child) const
    {
      // Legacy prefab tiles carry the connection custom data on an inner "Tile"
      // entity. Auto-generated tiles carry it on themselves.
      if (Prefab* prefab = child->As<Prefab>())
      {
        if (EntityPtr tile = prefab->GetFirstByName("Tile"))
        {
          return tile;
        }
      }

      return child;
    }

    void BricksEditor::SetTileConnection(const EntityPtr& tile, const char* name, bool value)
    {
      ParameterVariant* var = nullptr;
      if (!tile->m_localData.LookUp(CustomDataCategory.Name, name, &var))
      {
        // The tile does not carry the flag yet: create it so the connection
        // state is fully self contained on the tile.
        ParameterVariant newVar(value);
        newVar.m_name     = name;
        newVar.m_category = CustomDataCategory;
        tile->m_localData.Add(newVar);
      }
      else
      {
        *var = value;
      }
    }

    bool BricksEditor::ReadTileConnection(const EntityPtr& tile, const char* name) const
    {
      ParameterVariant* var = nullptr;
      if (tile->m_localData.LookUp(CustomDataCategory.Name, name, &var))
      {
        return var->GetCVar<bool>();
      }

      return false;
    }

    MaterialPtr BricksEditor::GetOrCreateUnlitColorMaterial(const String& fileName, const Vec3& color)
    {
      const String path = MaterialPath(fileName);

      // Idempotent: the material is saved with the project, so an existing
      // material file is loaded (and cached) instead of re-created.
      if (CheckFile(path))
      {
        return GetMaterialManager()->Create<Material>(path);
      }

      // First use: build an unlit color material (no diffuse texture, so the
      // unlit shader uses the material color) and persist it under the project
      // resources. It then survives restarts and is shared by every grid.
      std::filesystem::create_directories(std::filesystem::path(path).parent_path());

      MaterialPtr mat = GetMaterialManager()->GetCopyOfUnlitColorMaterial(false);
      mat->SetFile(path);
      mat->SetColorVal(color);
      mat->Init(false);
      mat->Save(false);
      GetMaterialManager()->Manage(mat);

      return mat;
    }

    MaterialPtr BricksEditor::GetOrCreateCheckerMaterial(bool dark)
    {
      if (dark)
      {
        return GetOrCreateUnlitColorMaterial("BricksCheckerDark.material", Vec3(g_checkerDark));
      }

      return GetOrCreateUnlitColorMaterial("BricksCheckerLight.material", Vec3(g_checkerLight));
    }

    String BricksEditor::ComputeGridSignature(EntityPtr gridNode) const
    {
      // Ordered by cell so the signature is stable regardless of the order the
      // children are iterated in.
      std::map<String, String> entries;

      for (Node* childNode : gridNode->m_node->m_children)
      {
        EntityPtr child = childNode->OwnerEntity();
        if (child == nullptr || child->GetNameVal() == "BridgeNode")
        {
          continue;
        }

        // Some editor delete paths remove the entity from the scene without
        // orphaning its node, and the undo stack keeps it alive. Treat it as
        // gone so the signature changes and the bridges get rebuilt.
        if (child->m_scene.expired())
        {
          continue;
        }

        EntityPtr tile = GetTileDataEntity(child);
        if (tile != nullptr)
        {
          Vec3 pos = child->m_node->GetTranslation(TransformationSpace::TS_WORLD);
          String key = RoundKey(pos.x) + "," + RoundKey(pos.z);

          String entry = key;
          entry += "|";
          entry += ReadTileConnection(tile, "X-") ? "L" : ".";
          entry += ReadTileConnection(tile, "X+") ? "R" : ".";
          entry += ReadTileConnection(tile, "Z-") ? "F" : ".";
          entry += ReadTileConnection(tile, "Z+") ? "B" : ".";

          entries[key] = entry;
        }
      }

      String sig = std::to_string(entries.size());
      for (const auto& pair : entries)
      {
        sig += pair.second;
      }

      return sig;
    }

    void BricksEditor::UpdateBridges()
    {
      App* app = GetApp();
      if (app == nullptr || app->m_gameMod != GameMod::Stop)
      {
        return;
      }

      EditorScenePtr scene = app->GetCurrentScene();
      if (scene == nullptr)
      {
        return;
      }

      // Collect the GridNodes first: RebuildBridges adds/removes entities and
      // must not mutate GetEntities() while it's being iterated.
      EntityPtrArray grids;
      for (EntityPtr ntt : scene->GetEntities())
      {
        if (ntt->GetNameVal() == "GridNode")
        {
          grids.push_back(ntt);
        }
      }

      std::unordered_set<ObjectId> liveGrids;
      for (EntityPtr grid : grids)
      {
        liveGrids.insert(grid->GetIdVal());

        String sig = ComputeGridSignature(grid);
        auto found = m_bridgeSignatures.find(grid->GetIdVal());
        if (found == m_bridgeSignatures.end() || found->second != sig)
        {
          RebuildBridges(grid);

          // RebuildBridges may have mirrored the user's flag edit onto the
          // neighbour tile, so re-read the signature from the synced state.
          m_bridgeSignatures[grid->GetIdVal()] = ComputeGridSignature(grid);
        }
      }

      // Drop signatures and flag snapshots of GridNodes that no longer exist.
      for (auto it = m_bridgeSignatures.begin(); it != m_bridgeSignatures.end();)
      {
        if (liveGrids.find(it->first) == liveGrids.end())
        {
          it = m_bridgeSignatures.erase(it);
        }
        else
        {
          ++it;
        }
      }
      for (auto it = m_tileFlags.begin(); it != m_tileFlags.end();)
      {
        if (liveGrids.find(it->first) == liveGrids.end())
        {
          it = m_tileFlags.erase(it);
        }
        else
        {
          ++it;
        }
      }
    }

    void BricksEditor::RebuildBridges(EntityPtr gridNode)
    {
      EditorScenePtr scene = GetApp()->GetCurrentScene();
      if (scene == nullptr)
      {
        return;
      }

      // Near-black unlit material for the bridge quads (created lazily,
      // idempotent). Shared by every bridge of every grid.
      MaterialPtr bridgeMat = GetOrCreateUnlitColorMaterial("BricksBridge.material", Vec3(g_bridgeColor));

      // Find (or create) the BridgeNode child that parents all bridges.
      EntityPtr bridgeNode = nullptr;
      for (Node* childNode : gridNode->m_node->m_children)
      {
        if (EntityPtr child = childNode->OwnerEntity())
        {
          if (child->GetNameVal() == "BridgeNode")
          {
            bridgeNode = child;
            break;
          }
        }
      }

      if (bridgeNode == nullptr)
      {
        bridgeNode = MakeNewPtr<Entity>();
        bridgeNode->SetNameVal("BridgeNode");
        scene->AddEntity(bridgeNode);
        gridNode->m_node->AddChild(bridgeNode->m_node, true);
      }

      // Clear the bridges of the previous pass. Copy the child list first:
      // OrphanSelf erases each child from m_children, which would invalidate
      // the range-for iterators and leave stale bridges behind (they visually
      // survive even though their tiles are gone).
      EntityPtrArray oldBridges;
      NodeRawPtrArray bridgeChildren = bridgeNode->m_node->m_children;
      for (Node* childNode : bridgeChildren)
      {
        if (EntityPtr child = childNode->OwnerEntity())
        {
          child->m_node->OrphanSelf(false);
          oldBridges.push_back(child);
        }
      }
      scene->RemoveEntity(oldBridges, true);

      // Collect the connection state of every tile.
      struct Tile
      {
        Vec3 center;    // World position of the tile center at its top surface.
        Vec3 size;      // Tile extent (AABB max-min).
        bool left, right, front, back;
        EntityPtr entity; // Owning entity (to write reciprocal custom data).
      };

      std::vector<Tile> tiles;
      for (Node* childNode : gridNode->m_node->m_children)
      {
        EntityPtr child = childNode->OwnerEntity();
        if (child == nullptr || child->GetNameVal() == "BridgeNode")
        {
          continue;
        }

        // Same guard as ComputeGridSignature: ignore lingering removed entities.
        if (child->m_scene.expired())
        {
          continue;
        }

        EntityPtr tile = GetTileDataEntity(child);
        if (tile != nullptr)
        {
          const BoundingBox& bb = child->GetBoundingBox();
          Vec3 pos              = child->m_node->GetTranslation(TransformationSpace::TS_WORLD);

          Tile t;
          t.center.x = pos.x + (bb.min.x + bb.max.x) * 0.5f;
          t.center.y = pos.y + bb.max.y;
          t.center.z = pos.z + (bb.min.z + bb.max.z) * 0.5f;
          t.size     = bb.max - bb.min;
          t.left     = ReadTileConnection(tile, "X-");
          t.right    = ReadTileConnection(tile, "X+");
          t.front    = ReadTileConnection(tile, "Z-");
          t.back     = ReadTileConnection(tile, "Z+");
          t.entity   = child;
          tiles.push_back(t);
        }
      }

      auto cellKey = [](const Vec3& c) { return RoundKey(c.x) + "," + RoundKey(c.z); };

      std::unordered_map<String, size_t> tileIndex;
      for (size_t i = 0; i < tiles.size(); ++i)
      {
        tileIndex[cellKey(tiles[i].center)] = i;
      }

      // ---- Reciprocal flag sync ---------------------------------------------
      // The user edits a single checkbox (say a tile's X- side). Mirror that
      // change onto the facing checkbox of the neighbour tile so the custom
      // data stays symmetric: turning one side off (or on) updates both tiles
      // in one action.
      std::map<String, TileFlags> current;
      for (size_t i = 0; i < tiles.size(); ++i)
      {
        TileFlags f;
        f.left   = tiles[i].left;
        f.right  = tiles[i].right;
        f.front  = tiles[i].front;
        f.back   = tiles[i].back;
        current[cellKey(tiles[i].center)] = f;
      }

      ObjectId gridId = gridNode->GetIdVal();
      auto storedIt   = m_tileFlags.find(gridId);

      // On the first sighting of a grid only record its state. Propagating then
      // would compare every flag against an empty snapshot and normalize away
      // any deliberate asymmetry loaded from disk.
      if (storedIt != m_tileFlags.end() && !storedIt->second.empty())
      {
        const std::map<String, TileFlags>& stored = storedIt->second;

        // Mirrors a changed flag onto the facing flag of the neighbour tile.
        auto syncNeighbour = [&](const Tile& t, char dir, bool value)
        {
          Vec3 offset;
          switch (dir)
          {
            case 'L': offset = Vec3(-t.size.x, 0.0f, 0.0f); break;
            case 'R': offset = Vec3(t.size.x, 0.0f, 0.0f); break;
            case 'F': offset = Vec3(0.0f, 0.0f, -t.size.z); break;
            case 'B': offset = Vec3(0.0f, 0.0f, t.size.z); break;
          }

          String nbKey = cellKey(t.center + offset);
          auto it      = tileIndex.find(nbKey);
          if (it == tileIndex.end())
          {
            return; // No tile in that direction.
          }

          Tile& nb    = tiles[it->second];
          String recipName;
          bool* recip = nullptr;
          switch (dir)
          {
            case 'L': recipName = "X+";  recip = &nb.right; break;
            case 'R': recipName = "X-";  recip = &nb.left;  break;
            case 'F': recipName = "Z+";  recip = &nb.back;  break;
            default:  recipName = "Z-";  recip = &nb.front; break;
          }

          if (*recip == value)
          {
            return; // Already in sync.
          }

          // Write the reciprocal flag on the neighbour tile's custom data, the
          // same way the editor's property panel does. Saved with the scene on
          // the next save.
          EntityPtr nbTile = GetTileDataEntity(nb.entity);
          if (nbTile != nullptr)
          {
            ParameterVariant* var = nullptr;
            if (nbTile->m_localData.LookUp(CustomDataCategory.Name, recipName, &var))
            {
              *var = value;
            }
          }

          *recip = value; // Bridge building below uses the synced state.
        };

        for (const auto& [key, cur] : current)
        {
          auto sit       = stored.find(key);
          TileFlags old  = sit != stored.end() ? sit->second : TileFlags();
          if (cur.left   != old.left)  syncNeighbour(tiles[tileIndex[key]], 'L', cur.left);
          if (cur.right  != old.right) syncNeighbour(tiles[tileIndex[key]], 'R', cur.right);
          if (cur.front  != old.front) syncNeighbour(tiles[tileIndex[key]], 'F', cur.front);
          if (cur.back   != old.back)  syncNeighbour(tiles[tileIndex[key]], 'B', cur.back);
        }
      }

      // Store the post-sync snapshot so the next frame's diff starts from
      // reality (the propagation above may have changed neighbour flags).
      for (size_t i = 0; i < tiles.size(); ++i)
      {
        TileFlags f;
        f.left   = tiles[i].left;
        f.right  = tiles[i].right;
        f.front  = tiles[i].front;
        f.back   = tiles[i].back;
        current[cellKey(tiles[i].center)] = f;
      }
      m_tileFlags[gridId] = current;

      // A bridge is a quad laid flat on the tile tops (rotated -90 degrees
      // about X), centered on the midpoint of the two tile centers and scaled
      // to span exactly from one center to the other. Scaling a centered quad
      // extends both ways; placing it at the midpoint makes both halves land on
      // the tile centers, so it never pokes past the neighbour.
      std::set<String> placedMidpoints;

      auto createBridge = [&](const Tile& a, const Tile& b, bool alongX)
      {
        Vec3 mid     = (a.center + b.center) * 0.5f;
        mid.y        = (a.center.y > b.center.y ? a.center.y : b.center.y) + g_bridgeLift;

        String key = RoundKey(mid.x) + "," + RoundKey(mid.z);
        if (!placedMidpoints.insert(key).second)
        {
          return; // The same bridge was already created from the other tile.
        }

        QuadPtr quad = MakeNewPtr<Quad>();
        quad->SetNameVal("Bridge");
        quad->GetMaterialComponent()->SetFirstMaterial(bridgeMat);

        quad->m_node->SetTranslation(mid, TransformationSpace::TS_WORLD);
        quad->m_node->SetOrientation(glm::angleAxis(glm::radians(-90.0f), X_AXIS), TransformationSpace::TS_WORLD);

        // The bridge spans tile center to tile center along one axis and is a
        // slim strip across the other: its width is 5% of the tile's extent on
        // that cross axis.
        float cross     = alongX ? a.size.z : a.size.x;
        float thickness = cross * g_bridgeThicknessRatio;
        float length    = alongX ? fabsf(b.center.x - a.center.x) : fabsf(b.center.z - a.center.z);
        quad->m_node->SetScale(Vec3(alongX ? length : thickness,
                                    alongX ? thickness : length,
                                    1.0f));

        scene->AddEntity(quad);
        bridgeNode->m_node->AddChild(quad->m_node, true);
      };

      // A bridge connects two tiles only when BOTH sides flag the facing
      // connection (AND). Otherwise toggling a tile's flag off would not remove
      // the bridge at that location, because the neighbour's reciprocal flag
      // would still emit it. The midpoint dedup still collapses the two
      // symmetric attempts (this tile and the neighbour) into a single quad.
      for (const Tile& t : tiles)
      {
        if (t.left)
        {
          Vec3 n = t.center + Vec3(-t.size.x, 0.0f, 0.0f);
          if (auto it = tileIndex.find(cellKey(n)); it != tileIndex.end())
          {
            const Tile& nb = tiles[it->second];
            if (nb.right) // The neighbour must face back toward this tile.
            {
              createBridge(t, nb, true);
            }
          }
        }

        if (t.right)
        {
          Vec3 n = t.center + Vec3(t.size.x, 0.0f, 0.0f);
          if (auto it = tileIndex.find(cellKey(n)); it != tileIndex.end())
          {
            const Tile& nb = tiles[it->second];
            if (nb.left)
            {
              createBridge(t, nb, true);
            }
          }
        }

        if (t.front) // Front faces -Z.
        {
          Vec3 n = t.center + Vec3(0.0f, 0.0f, -t.size.z);
          if (auto it = tileIndex.find(cellKey(n)); it != tileIndex.end())
          {
            const Tile& nb = tiles[it->second];
            if (nb.back)
            {
              createBridge(t, nb, false);
            }
          }
        }

        if (t.back) // Back faces +Z.
        {
          Vec3 n = t.center + Vec3(0.0f, 0.0f, t.size.z);
          if (auto it = tileIndex.find(cellKey(n)); it != tileIndex.end())
          {
            const Tile& nb = tiles[it->second];
            if (nb.front)
            {
              createBridge(t, nb, false);
            }
          }
        }
      }
    }

    void BricksEditor::SaveSettings()
    {
      App* app = GetApp();
      if (app == nullptr || app->m_workspace == nullptr)
      {
        return;
      }

      String cfgDir = app->m_workspace->GetConfigDirectory();
      std::filesystem::create_directories(cfgDir);

      String path = ConcatPaths({cfgDir, "BricksEditor.settings"});

      std::ofstream file;
      file.open(path.c_str(), std::ios::out | std::ios::trunc);
      if (!file.is_open())
      {
        TK_ERR("BricksEditor: Can't open settings file for writing: %s", path.c_str());
        return;
      }

      XmlDocumentPtr doc = MakeNewPtr<XmlDocument>();
      XmlNode* root      = CreateXmlNode(doc.get(), "BricksEditor");

      // Serialize the whole window, TKParams included, using the same path the
      // editor uses for its own windows.
      Serialize(doc.get(), root);

      std::string xml;
      rapidxml::print(std::back_inserter(xml), *doc, 0);
      file << xml;
      file.close();
      doc->clear();
    }

    void BricksEditor::LoadSettings()
    {
      App* app = GetApp();
      if (app == nullptr || app->m_workspace == nullptr)
      {
        return;
      }

      String path = ConcatPaths({app->m_workspace->GetConfigDirectory(), "BricksEditor.settings"});
      if (!CheckFile(path))
      {
        return;
      }

      XmlFilePtr file       = MakeNewPtr<XmlFile>(path.c_str());
      XmlDocumentPtr doc    = MakeNewPtr<XmlDocument>();
      doc->parse<0>(file->data());

      SerializationFileInfo info;
      info.File     = path;
      info.Document = doc.get();

      if (XmlNode* root = doc->first_node("BricksEditor"))
      {
        const char* xmlRootObject = Object::StaticClass()->Name.c_str();
        if (XmlNode* objNode = root->first_node(xmlRootObject))
        {
          // Restores TileSize / GridCols / GridRows.
          DeSerialize(info, objNode);
        }
      }
    }

    void BricksEditor::Show()
    {
      ImGui::SetNextWindowSize(ImVec2(340, 220), ImGuiCond_Once);
      if (ImGui::Begin(m_name.c_str(), &m_visible))
      {
        HandleStates();

        // ---- Tile geometry -------------------------------------------------
        ImGui::SeparatorText("Tile");

        float size = GetTileSizeVal();
        if (ImGui::InputFloat("Size (D)", &size, 0.1f, 1.0f, "%.2f"))
        {
          size = glm::clamp(size, 0.1f, 100.0f);
          SetTileSizeVal(size);
          SaveSettings();
        }

        int cols = GetGridColsVal();
        if (ImGui::InputInt("Columns (N)", &cols, 1, 10))
        {
          cols = glm::clamp(cols, 1, 100);
          SetGridColsVal(cols);
          SaveSettings();
        }

        int rows = GetGridRowsVal();
        if (ImGui::InputInt("Rows (M)", &rows, 1, 10))
        {
          rows = glm::clamp(rows, 1, 100);
          SetGridRowsVal(rows);
          SaveSettings();
        }

        ImGui::Text("Height: %.2f (fixed)", g_tileHeight);

        // ---- Place ----------------------------------------------------------
        ImGui::Spacing();
        if (ImGui::Button("Place Grid", ImVec2(-FLT_MIN, 0)))
        {
          App* app = GetApp();
          if (app && app->m_cursor)
          {
            Vec3 cursorPos = app->m_cursor->m_worldLocation;

            EditorScenePtr scene = app->GetCurrentScene();
            if (scene)
            {
              const float D   = GetTileSizeVal();
              const int N     = GetGridColsVal();
              const int M     = GetGridRowsVal();

              // The checker pair is created lazily and reused (idempotent), so
              // the tool is self sufficient: no user supplied asset required.
              MaterialPtr lightMat = GetOrCreateCheckerMaterial(false);
              MaterialPtr darkMat  = GetOrCreateCheckerMaterial(true);

              // Master group: an empty entity that parents every placed tile,
              // keeping the outliner organized.
              EntityPtr master = MakeNewPtr<Entity>();
              master->SetNameVal("GridNode");
              master->m_node->SetTranslation(cursorPos, TransformationSpace::TS_WORLD);
              scene->AddEntity(master);

              float originX = floorf(cursorPos.x / D) * D;
              float originZ = floorf(cursorPos.z / D) * D;

              for (int iz = 0; iz < M; ++iz)
              {
                for (int ix = 0; ix < N; ++ix)
                {
                  const bool darkTile = ((ix + iz) % 2) == 1;

                  CubePtr cube = MakeNewPtr<Cube>();
                  cube->SetNameVal("Tile_" + std::to_string(ix) + "x" + std::to_string(iz));
                  // Re-generates the cube geometry at the tile size. The cube
                  // rests on the ground plane when centered at half its height.
                  cube->SetCubeScaleVal(Vec3(D, g_tileHeight, D));
                  cube->GetMeshComponent()->Init(false);
                  cube->GetMaterialComponent()->SetFirstMaterial(darkTile ? darkMat : lightMat);

                  Vec3 pos(originX + ix * D, g_tileHeight * 0.5f, originZ + iz * D);
                  cube->m_node->SetTranslation(pos, TransformationSpace::TS_WORLD);

                  scene->AddEntity(cube);
                  master->m_node->AddChild(cube->m_node, true);

                  // Auto-set the connection custom data so the tile is self
                  // contained (bridges work without a prefab author). Default
                  // to all sides connected: a fresh grid is fully bridged.
                  SetTileConnection(cube, "X-", true);
                  SetTileConnection(cube, "X+", true);
                  SetTileConnection(cube, "Z-", true);
                  SetTileConnection(cube, "Z+", true);
                }
              }
            }
          }
        }
      }
      ImGui::End();
    }

  } // namespace Editor
} // namespace ToolKit
