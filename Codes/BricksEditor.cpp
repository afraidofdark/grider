/*
 * Copyright (c) 2019-2026 OtSoftware
 * This code is licensed under the GNU Lesser General Public License v3.0 (LGPL-3.0).
 * For more information, including options for a more permissive commercial license,
 * please visit [otsoftware.tr] or contact us at [info@otsoftare.tr].
 */

#include "BricksEditor.h"

#include <Editor/Source/App.h>
#include <Editor/Source/EditorScene.h>
#include <Editor/UI/View/View.h>

#include <Entity.h>
#include <Logger.h>
#include <Mesh.h>
#include <MeshComponent.h>
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
      // Thickness (depth axis) of the bridge quads.
      constexpr float g_bridgeThickness = 0.2f;

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

      GridSize_Define(1, "Bricks", 0, true, true);
      MeshFile_Define("", "Bricks", 0, true, true);
      PrefabFile_Define("", "Bricks", 0, true, true);
    }

    void BricksEditor::ParameterEventConstructor()
    {
      Super::ParameterEventConstructor();

      // Restore the dropped mesh after deserialization.
      SetBrickMesh(GetMeshFileVal());
    }

    void BricksEditor::SetBrickMesh(const String& path)
    {
      m_mesh = nullptr;
      if (path.empty())
      {
        return;
      }

      // MeshFile stores a workspace-relative path (e.g. "ciiip/foo.mesh").
      // Resolve it to a full path before loading, mirroring the engine's mesh
      // deserialization (ParameterVariant MeshPtr case).
      String fullPath = MeshPath(path);
      String ext;
      DecomposePath(fullPath, nullptr, nullptr, &ext);

      if (ext == SKINMESH)
      {
        m_mesh = GetMeshManager()->Create<SkinMesh>(fullPath);
      }
      else
      {
        m_mesh = GetMeshManager()->Create<Mesh>(fullPath);
      }

      if (m_mesh)
      {
        m_mesh->Init(false);
      }
    }

    BoundingBox BricksEditor::GetPrefabBoundary()
    {
      PrefabPtr probe = MakeNewPtr<Prefab>();
      probe->SetPrefabPathVal(GetPrefabFileVal());
      probe->Load();

      // Load() parses the prefab scene; its boundary (AABB) is then available
      // through the prefab entity's own bounding box.
      return probe->GetBoundingBox();
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

        if (Prefab* prefab = child->As<Prefab>())
        {
          if (EntityPtr tile = prefab->GetFirstByName("Tile"))
          {
            Vec3 pos = prefab->m_node->GetTranslation(TransformationSpace::TS_WORLD);
            String key = RoundKey(pos.x) + "," + RoundKey(pos.z);

            String entry = key;
            entry += "|";
            entry += ReadTileConnection(tile, "LeftCon") ? "L" : ".";
            entry += ReadTileConnection(tile, "RightCon") ? "R" : ".";
            entry += ReadTileConnection(tile, "FrontCon") ? "F" : ".";
            entry += ReadTileConnection(tile, "BackCon") ? "B" : ".";

            entries[key] = entry;
          }
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
        EntityPtr prefab; // Owning prefab entity (to write reciprocal custom data).
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

        if (Prefab* prefab = child->As<Prefab>())
        {
          if (EntityPtr tile = prefab->GetFirstByName("Tile"))
          {
            const BoundingBox& bb = prefab->GetBoundingBox();
            Vec3 pos             = prefab->m_node->GetTranslation(TransformationSpace::TS_WORLD);

            Tile t;
            t.center.x = pos.x + (bb.min.x + bb.max.x) * 0.5f;
            t.center.y = pos.y + bb.max.y;
            t.center.z = pos.z + (bb.min.z + bb.max.z) * 0.5f;
            t.size     = bb.max - bb.min;
            t.left     = ReadTileConnection(tile, "LeftCon");
            t.right    = ReadTileConnection(tile, "RightCon");
            t.front    = ReadTileConnection(tile, "FrontCon");
            t.back     = ReadTileConnection(tile, "BackCon");
            t.prefab   = child;
            tiles.push_back(t);
          }
        }
      }

      auto cellKey = [](const Vec3& c) { return RoundKey(c.x) + "," + RoundKey(c.z); };

      std::unordered_map<String, size_t> tileIndex;
      for (size_t i = 0; i < tiles.size(); ++i)
      {
        tileIndex[cellKey(tiles[i].center)] = i;
      }

      // ---- Reciprocal flag sync ---------------------------------------------
      // The user edits a single checkbox (say a tile's LeftCon). Mirror that
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
            case 'L': recipName = "RightCon"; recip = &nb.right; break;
            case 'R': recipName = "LeftCon";  recip = &nb.left;  break;
            case 'F': recipName = "BackCon";  recip = &nb.back;  break;
            default:  recipName = "FrontCon"; recip = &nb.front; break;
          }

          if (*recip == value)
          {
            return; // Already in sync.
          }

          // Write the reciprocal flag on the neighbour tile's custom data, the
          // same way the editor's property panel does. Saved with the scene on
          // the next save.
          if (EntityPtr nbTile = nb.prefab ? nb.prefab->As<Prefab>()->GetFirstByName("Tile") : nullptr)
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
        mid.y        = a.center.y > b.center.y ? a.center.y : b.center.y;

        String key = RoundKey(mid.x) + "," + RoundKey(mid.z);
        if (!placedMidpoints.insert(key).second)
        {
          return; // The same bridge was already created from the other tile.
        }

        QuadPtr quad = MakeNewPtr<Quad>();
        quad->SetNameVal("Bridge");

        quad->m_node->SetTranslation(mid, TransformationSpace::TS_WORLD);
        quad->m_node->SetOrientation(glm::angleAxis(glm::radians(-90.0f), X_AXIS), TransformationSpace::TS_WORLD);

        float length = alongX ? fabsf(b.center.x - a.center.x) : fabsf(b.center.z - a.center.z);
        quad->m_node->SetScale(Vec3(alongX ? length : g_bridgeThickness,
                                    alongX ? g_bridgeThickness : length,
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
          // Restores GridSize/MeshFile and, via ParameterEventConstructor,
          // reloads the brick mesh.
          DeSerialize(info, objNode);
        }
      }
    }

    void BricksEditor::Show()
    {
      ImGui::SetNextWindowSize(ImVec2(340, 260), ImGuiCond_Once);
      if (ImGui::Begin(m_name.c_str(), &m_visible))
      {
        HandleStates();

        // ---- Brick mesh ----------------------------------------------------
        ImGui::SeparatorText("Brick Mesh");

        // Drop a mesh (.mesh) or a prefab (.scene) to use it as the brick.
        // Placed bricks use the object's bounding box to size and space
        // themselves.
        // DropZone passes `file` to ImGui::ImageButton as its id, so it must
        // never be empty (ImGui asserts on an empty id at the window root).
        const String prefabFile = GetPrefabFileVal();
        const String meshFile   = GetMeshFileVal();
        const String current    = !prefabFile.empty() ? prefabFile : meshFile;
        const String dropFile   = current.empty() ? "##BrickMesh" : current;
        TexturePtr dropIcon     = !prefabFile.empty() ? UI::m_prefabIcn : UI::m_meshIcon;
        View::DropZone(EditorImGuiTextureCache::Acquire(dropIcon),
                       dropFile,
                       [this](const DirectoryEntry& entry)
                       {
                         if (entry.m_ext == MESH || entry.m_ext == SKINMESH)
                         {
                           // Store workspace-relative so the setting survives a
                           // project move (engine convention: resources are
                           // referenced by their relative path).
                           const String rel = GetRelativeResourcePath(entry.GetFullPath());
                           SetBrickMesh(rel);
                           SetMeshFileVal(rel);
                           SetPrefabFileVal("");

                           // Persist immediately so the setting survives an
                           // editor restart.
                           SaveSettings();

                           TK_LOG("Selected brick mesh: %s", rel.c_str());
                         }
                         else if (entry.m_ext == SCENE)
                         {
                           SetMeshFileVal("");
                           m_mesh = nullptr;
                           SetPrefabFileVal(GetRelativeResourcePath(entry.GetFullPath()));

                           SaveSettings();

                           TK_LOG("Selected brick prefab: %s", entry.GetFullPath().c_str());
                         }
                         else
                         {
                           GetApp()->SetStatusMsg(g_statusFailed);
                           TK_ERR("Only mesh or scene (prefab) files are accepted.");
                         }
                       },
                       "Brick (Mesh / Prefab)");

        // ---- Grid -----------------------------------------------------------
        ImGui::SeparatorText("Grid");

        int gridSize = GetGridSizeVal();
        if (ImGui::InputInt("Size (N x N)", &gridSize, 1, 10))
        {
          if (gridSize < 1)
            gridSize = 1;
          if (gridSize > 100)
            gridSize = 100;

          SetGridSizeVal(gridSize);
          SaveSettings();
        }

        // ---- Place ----------------------------------------------------------
        ImGui::Spacing();
        if (ImGui::Button("Place Bricks", ImVec2(-FLT_MIN, 0)))
        {
          App* app = GetApp();
          if (app && app->m_cursor)
          {
            Vec3 cursorPos = app->m_cursor->m_worldLocation;

            EditorScenePtr scene = app->GetCurrentScene();
            if (scene)
            {
              int N = GetGridSizeVal();

              // Determine the brick source and its bounding box. Prefab and
              // mesh both tile by their AABB; an empty source falls back to
              // unit cubes.
              const bool isPrefab = !GetPrefabFileVal().empty();
              const bool isMesh   = !isPrefab && m_mesh != nullptr;

              BoundingBox bb;
              Vec3 size(1.0f);
              bool valid = true;

              if (isPrefab)
              {
                bb    = GetPrefabBoundary();
                size  = bb.max - bb.min;
                valid = size.x >= 0.0001f && size.z >= 0.0001f;
                if (!valid)
                {
                  TK_ERR("Selected prefab has an empty bounding box.");
                }
              }
              else if (isMesh)
              {
                bb    = m_mesh->m_boundingBox;
                size  = bb.max - bb.min;
                valid = size.x >= 0.0001f && size.z >= 0.0001f;
                if (!valid)
                {
                  TK_ERR("Selected mesh has an empty bounding box.");
                }
              }
              else
              {
                // Unit cube centered at its origin.
                bb.min = Vec3(-0.5f);
                bb.max = Vec3(0.5f);
              }

              if (valid)
              {
                // Master group: an empty entity that parents every placed
                // brick, keeping the outliner organized.
                EntityPtr master = MakeNewPtr<Entity>();
                master->SetNameVal("GridNode");
                master->m_node->SetTranslation(cursorPos, TransformationSpace::TS_WORLD);
                scene->AddEntity(master);

                float originX = floorf(cursorPos.x / size.x) * size.x;
                float originZ = floorf(cursorPos.z / size.z) * size.z;

                for (int ix = 0; ix < N; ++ix)
                {
                  for (int iz = 0; iz < N; ++iz)
                  {
                    // Tiles by the brick's AABB: the min corner sits on the
                    // cell and the brick rests on the ground plane.
                    Vec3 pos(originX + ix * size.x - bb.min.x,
                             -bb.min.y,
                             originZ + iz * size.z - bb.min.z);

                    if (isPrefab)
                    {
                      // Instantiate the prefab at this cell. AddEntity links
                      // the prefab's contents into the scene.
                      PrefabPtr prefab = MakeNewPtr<Prefab>();
                      prefab->SetPrefabPathVal(GetPrefabFileVal());
                      prefab->Load();
                      prefab->Init(scene);
                      scene->AddEntity(prefab);

                      prefab->m_node->SetTranslation(pos, TransformationSpace::TS_WORLD);
                      master->m_node->AddChild(prefab->m_node, true);
                    }
                    else if (isMesh)
                    {
                      EntityPtr ntt = MakeNewPtr<Entity>();
                      MeshComponentPtr meshCom = ntt->AddComponent<MeshComponent>();
                      meshCom->SetMeshVal(m_mesh);
                      ntt->m_node->SetTranslation(pos, TransformationSpace::TS_WORLD);

                      scene->AddEntity(ntt);
                      master->m_node->AddChild(ntt->m_node, true);
                    }
                    else
                    {
                      CubePtr cube = MakeNewPtr<Cube>();
                      cube->GetMeshComponent()->Init(false);
                      cube->m_node->SetTranslation(pos, TransformationSpace::TS_WORLD);

                      scene->AddEntity(cube);
                      master->m_node->AddChild(cube->m_node, true);
                    }
                  }
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
