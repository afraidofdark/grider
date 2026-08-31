/*
 * Copyright (c) 2019-2026 OtSoftware
 * This code is licensed under the GNU Lesser General Public License v3.0 (LGPL-3.0).
 * For more information, including options for a more permissive commercial license,
 * please visit [otsoftware.tr] or contact us at [info@otsoftare.tr].
 */

#include "GridEditor.h"

#include <Editor/Source/App.h>
#include <Editor/Source/EditorScene.h>
#include <Editor/UI/EditorImGuiTextureCache.h>
#include <Editor/UI/View/View.h>

#include <Entity.h>
#include <Logger.h>
#include <Material.h>
#include <MaterialComponent.h>
#include <Mesh.h>
#include <MeshComponent.h>
#include <Prefab.h>
#include <Primative.h>
#include <SkeletonComponent.h>
#include <Util.h>

#include <filesystem>

#include <cctype>
#include <cmath>
#include <map>
#include <set>
#include <unordered_map>
#include <unordered_set>

namespace ToolKit
{
  namespace Editor
  {

    TKDefineClass(GridEditor, Window);

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

      // World-space AABB of an entity and its whole child hierarchy. The engine
      // bounding box only covers an entity's own MeshComponent, which is empty
      // for prefab roots (their geometry lives on children), so the true
      // footprint is merged from every descendant. Used to pivot placed objects
      // on their real center and drop them flush on the tile top.
      BoundingBox GetWorldBounds(EntityPtr root)
      {
        BoundingBox bounds = root->GetBoundingBox(true);

        for (Node* childNode : root->m_node->m_children)
        {
          if (EntityPtr child = childNode->OwnerEntity())
          {
            bounds.UpdateBoundary(GetWorldBounds(child));
          }
        }

        return bounds;
      }

      // Converts an absolute resource path to the workspace-relative form that
      // gets persisted in the plugin settings (e.g. "Meshes/Cube.mesh"). Returns
      // the input unchanged when the path isn't under the resource root.
      String ToResourceRelativePath(const String& absolute)
      {
        String root = NormalizePath(Main::GetInstance()->m_resourceRoot);
        if (root.empty())
        {
          return absolute;
        }

        String path = NormalizePath(absolute);
        if (path.size() > root.size() && path.compare(0, root.size(), root) == 0 &&
            path[root.size()] == GetPathSeparator())
        {
          return path.substr(root.size() + 1);
        }

        return absolute;
      }

      // Inverse of ToResourceRelativePath: re-attaches the workspace resource
      // root so a persisted relative path resolves back to a real file.
      String ToResourceAbsolutePath(const String& relative)
      {
        String root = NormalizePath(Main::GetInstance()->m_resourceRoot);
        if (root.empty())
        {
          return relative;
        }

        return ConcatPaths({root, relative});
      }

      // Byte length of the UTF-8 code point starting at text[offset] (1 for
      // ASCII / malformed sequences) so truncation never splits a multi-byte
      // character (accents, Turkish letters, emoji, ...).
      size_t Utf8CharLen(const String& text, size_t offset)
      {
        const unsigned char c = static_cast<unsigned char>(text[offset]);
        if ((c & 0x80) == 0) { return 1; }
        if ((c & 0xE0) == 0xC0) { return 2; }
        if ((c & 0xF0) == 0xE0) { return 3; }
        if ((c & 0xF8) == 0xF0) { return 4; }
        return 1;
      }

      // Short label for a grid cell: as-is when it fits within maxChars code
      // points, otherwise cut at maxChars and finished with "...".
      String TruncateLabel(const String& text, size_t maxChars)
      {
        size_t total = 0;
        for (size_t i = 0; i < text.size(); ++total)
        {
          i += Utf8CharLen(text, i);
        }
        if (total <= maxChars)
        {
          return text;
        }

        size_t offset = 0;
        for (size_t n = 0; n < maxChars; ++n)
        {
          offset += Utf8CharLen(text, offset);
        }
        return text.substr(0, offset) + "...";
      }

      // Next free "Area N" label: the largest existing numeric suffix + 1, so
      // removing areas never produces duplicate auto-generated names.
      String NextAreaName(const std::vector<GridEditor::PlacementSlot>& slots)
      {
        int maxNum = 0;
        for (const GridEditor::PlacementSlot& s : slots)
        {
          if (s.name.size() > 5 && s.name.compare(0, 5, "Area ") == 0)
          {
            int num = 0;
            bool ok = true;
            for (size_t k = 5; k < s.name.size(); ++k)
            {
              if (!isdigit(static_cast<unsigned char>(s.name[k])))
              {
                ok = false;
                break;
              }
              num = num * 10 + (s.name[k] - '0');
            }
            if (ok)
            {
              maxNum = glm::max(maxNum, num);
            }
          }
        }

        return "Area " + std::to_string(maxNum + 1);
      }
    }

    GridEditor::GridEditor()
    {
      m_name = "Grid Editor";
    }

    GridEditor::~GridEditor()
    {
    }

    void GridEditor::ParameterConstructor()
    {
      Super::ParameterConstructor();

      // D: width/depth of a tile (its height is fixed at g_tileHeight).
      TileSize_Define(5.0f, "Tile", 0, true, true);
      // N: horizontal repeat count, M: vertical repeat count.
      GridCols_Define(4, "Tile", 0, true, true);
      GridRows_Define(4, "Tile", 0, true, true);
      // Placement tool state. Hidden params: persisted with the window, but not
      // exposed as editable properties (they live in the window UI only).
      ActiveSlot_Define(0, "Placement", 0, false, false);
      PlacementDir_Define(PlacementDirZm, "Placement", 0, false, false);
      TileExtendDir_Define(PlacementDirZm, "Placement", 0, false, false);
    }

    float GridEditor::PlacementYaw(int dir) const
    {
      // Rotating about +Y by theta maps the local -Z (forward) to
      // (-sin(theta), 0, -cos(theta)). Solve theta for each target axis.
      switch (dir)
      {
        case PlacementDirXp: return -90.0f; // forward -> +X
        case PlacementDirXm: return 90.0f;  // forward -> -X
        case PlacementDirZp: return 180.0f; // forward -> +Z
        default: return 0.0f;               // PlacementDirZm: forward already faces -Z.
      }
    }

    int GridEditor::FacingDir(const EntityPtr& obj) const
    {
      // The object's -Z (front) in world space, projected onto the XZ plane.
      // Read from the world orientation so a manually rotated object still
      // snaps to the nearest compass direction.
      Vec3 f = obj->m_node->GetOrientation(TransformationSpace::TS_WORLD) * Vec3(0.0f, 0.0f, -1.0f);
      if (fabsf(f.x) >= fabsf(f.z))
      {
        return f.x >= 0.0f ? PlacementDirXp : PlacementDirXm;
      }

      return f.z >= 0.0f ? PlacementDirZp : PlacementDirZm;
    }

    bool GridEditor::IsTile(const EntityPtr& e) const
    {
      if (e == nullptr)
      {
        return false;
      }

      EntityPtr parent = e->Parent();
      return parent != nullptr && parent->GetNameVal() == "GridNode" && e->GetNameVal() != "BridgeNode";
    }

    bool GridEditor::IsPlacedObject(const EntityPtr& e) const
    {
      // A placed object is parented directly under a tile, so it is recognized
      // by its position in the grid hierarchy alone (no side table to keep in
      // sync with the scene).
      return e != nullptr && e->Parent() != nullptr && IsTile(e->Parent());
    }

    Vec3 GridEditor::GetTileTopCenter(const EntityPtr& tile) const
    {
      Vec3 pos       = tile->m_node->GetTranslation(TransformationSpace::TS_WORLD);
      BoundingBox bb = tile->GetBoundingBox();

      return Vec3(pos.x + (bb.min.x + bb.max.x) * 0.5f, pos.y + bb.max.y, pos.z + (bb.min.z + bb.max.z) * 0.5f);
    }

    EntityPtr GridEditor::InstantiatePlacement(const EditorScenePtr& scene,
                                               const String& fullPath,
                                               const String& ext,
                                               const String& name)
    {
      if (scene == nullptr || fullPath.empty())
      {
        return nullptr;
      }

      if (ext == SCENE)
      {
        // Prefabs are .scene files under the project's Prefabs folder.
        String path   = GetRelativeResourcePath(fullPath);
        String folder = fullPath.substr(0, fullPath.length() - path.length());
        if (folder != PrefabPath(""))
        {
          TK_ERR("GridEditor: Can't place a prefab outside of the Prefabs folder: %s", fullPath.c_str());
          return nullptr;
        }

        PrefabPtr prefab = MakeNewPtr<Prefab>();
        prefab->SetNameVal(name);
        prefab->SetPrefabPathVal(path);
        prefab->Load();
        prefab->Init(scene);
        scene->AddEntity(prefab); // AddEntity links the prefab roots into the scene.

        return prefab;
      }

      if (ext == MESH || ext == SKINMESH)
      {
        // Mirrors EditorViewport::LoadDragMesh: a plain entity carrying the
        // mesh through a MeshComponent.
        EntityPtr entity = MakeNewPtr<Entity>();
        entity->SetNameVal(name);
        entity->AddComponent<MeshComponent>();

        MeshPtr mesh;
        if (ext == SKINMESH)
        {
          mesh = GetMeshManager()->Create<SkinMesh>(fullPath);
        }
        else
        {
          mesh = GetMeshManager()->Create<Mesh>(fullPath);
        }
        entity->GetMeshComponent()->SetMeshVal(mesh);
        mesh->Init(false);

        if (mesh->IsSkinned())
        {
          SkeletonComponentPtr skelComp = entity->AddComponent<SkeletonComponent>();
          skelComp->SetSkeletonResourceVal(((SkinMesh*) mesh.get())->m_skeleton);
          skelComp->Init();
        }

        MaterialComponentPtr matComp = entity->AddComponent<MaterialComponent>();
        matComp->UpdateMaterialList();

        scene->AddEntity(entity);
        return entity;
      }

      TK_ERR("GridEditor: Unsupported placement drop type: %s", ext.c_str());
      return nullptr;
    }

    void GridEditor::PlaceObjectOnSelectedTile(const PlacementSlot& slot)
    {
      App* app = GetApp();
      if (app == nullptr)
      {
        return;
      }

      if (slot.absPath.empty())
      {
        app->SetStatusMsg(g_statusFailed);
        return;
      }

      EditorScenePtr scene = app->GetCurrentScene();
      if (scene == nullptr)
      {
        return;
      }

      // The placement target is a tile: a child of a GridNode (bridges are the
      // plugin's own entities and must not be a placement target).
      EntityPtr sel = scene->GetCurrentSelection();
      if (sel == nullptr || sel->GetNameVal() == "BridgeNode")
      {
        app->SetStatusMsg(g_statusFailed);
        return;
      }

      EntityPtr parent = sel->Parent();
      if (parent == nullptr || parent->GetNameVal() != "GridNode")
      {
        app->SetStatusMsg(g_statusFailed);
        return;
      }

      // Tile top-surface center (same math as RebuildBridges).
      Vec3 topCenter = GetTileTopCenter(sel);

      // Face the object's -Z (front) toward the selected direction. The object
      // is named after the dropped asset's file name (falling back to the area
      // name).
      const String objName = slot.fileName.empty() ? slot.name : slot.fileName;
      EntityPtr obj        = InstantiatePlacement(scene, slot.absPath, slot.ext, objName);
      if (obj == nullptr)
      {
        return;
      }

      float yaw = PlacementYaw(GetPlacementDirVal());
      obj->m_node->SetOrientation(glm::angleAxis(glm::radians(yaw), Y_AXIS), TransformationSpace::TS_WORLD);

      // Center it on the tile and drop its base onto the tile top. The world
      // bounding box is read after orienting so the rotation is accounted for.
      // GetWorldBounds includes child geometry, so prefab placements pivot on
      // their real center (a prefab root carries no mesh of its own).
      Vec3 objPos     = obj->m_node->GetTranslation(TransformationSpace::TS_WORLD);
      BoundingBox obb = GetWorldBounds(obj);
      Vec3 objCenter  = obb.GetCenter();
      Vec3 delta      = topCenter - objCenter;
      delta.y         = topCenter.y - obb.min.y;
      obj->m_node->SetTranslation(objPos + delta, TransformationSpace::TS_WORLD);

      // Parent the object under the tile, keeping its world transform. This is
      // what marks it as a placed object later: the tool recognizes it by its
      // parent, and it follows the tile if the grid moves. Node does not
      // inherit the parent's scale by default, so the object stays unscaled.
      sel->m_node->AddChild(obj->m_node, true);

      scene->AddToSelection(obj->GetIdVal(), false);
    }

    void GridEditor::ReorientPlacedObject(const EntityPtr& obj, int dir)
    {
      EntityPtr tile = obj->Parent();
      if (tile == nullptr)
      {
        return;
      }

      // The object's node origin rests on the tile center, so a world-space
      // Y-axis turn spins it in place without moving it.
      float yaw = PlacementYaw(dir);
      obj->m_node->SetOrientation(glm::angleAxis(glm::radians(yaw), Y_AXIS), TransformationSpace::TS_WORLD);
    }

    void GridEditor::DrawPlacementCompass(int& dir, bool canPlace, const std::function<void()>& onPlace, const char* id)
    {
      ImGui::PushID(id);

      const float availW  = ImGui::GetContentRegionAvail().x;
      const float spacing = ImGui::GetStyle().ItemSpacing.x;
      const float radioW  = ImGui::GetFrameHeight(); // Radio circle + padding footprint.

      // Z+
      const float zPlusW = ImGui::CalcTextSize("Z+").x + radioW;
      ImGui::SetCursorPosX((availW - zPlusW) * 0.5f);
      ImGui::RadioButton("Z+", &dir, PlacementDirZp);

      // Blank row after Z+.
      ImGui::Dummy(ImVec2(0, 8));

      // X- [Place] X+: all three next to each other, centered.
      const float placeW = 64.0f;
      const float groupW = (ImGui::CalcTextSize("X-").x + radioW) + spacing + placeW + spacing +
                           (ImGui::CalcTextSize("X+").x + radioW);
      ImGui::SetCursorPosX((availW - groupW) * 0.5f);
      ImGui::RadioButton("X-", &dir, PlacementDirXm);
      ImGui::SameLine();
      ImGui::BeginDisabled(!canPlace);
      if (ImGui::Button("Place", ImVec2(placeW, 0)))
      {
        if (onPlace)
        {
          onPlace();
        }
      }
      ImGui::EndDisabled();
      ImGui::SameLine();
      ImGui::RadioButton("X+", &dir, PlacementDirXp);

      // Blank row after the Place row.
      ImGui::Dummy(ImVec2(0, 8));

      // Z-
      const float zMinusW = ImGui::CalcTextSize("Z-").x + radioW;
      ImGui::SetCursorPosX((availW - zMinusW) * 0.5f);
      ImGui::RadioButton("Z-", &dir, PlacementDirZm);

      ImGui::PopID();
    }

    void GridEditor::ExtendTileFrom(const EntityPtr& tile, int dir)
    {
      App* app = GetApp();
      if (app == nullptr)
      {
        return;
      }

      EditorScenePtr scene = app->GetCurrentScene();
      if (scene == nullptr || tile == nullptr || !IsTile(tile))
      {
        app->SetStatusMsg(g_statusFailed);
        return;
      }

      EntityPtr gridNode = tile->Parent();
      if (gridNode == nullptr)
      {
        app->SetStatusMsg(g_statusFailed);
        return;
      }

      // Map the compass direction to the graph axis.
      GridDir gd;
      switch (dir)
      {
        case PlacementDirXp: gd = GridDir::Xp; break;
        case PlacementDirXm: gd = GridDir::Xm; break;
        case PlacementDirZp: gd = GridDir::Zp; break;
        default:             gd = GridDir::Zm; break;
      }

      GridGraph graph;
      graph.LoadFromScene(gridNode);

      GridNode* selNode = graph.NodeAtPoint(GetTileTopCenter(tile));
      if (selNode == nullptr)
      {
        app->SetStatusMsg(g_statusFailed);
        return;
      }

      // The neighbour cell must be free: there is no tile in that direction.
      if (graph.Neighbor(*selNode, gd) != nullptr)
      {
        app->SetStatusMsg(g_statusFailed);
        return;
      }

      // Neighbour centre on the lattice.
      int dx = 0, dz = 0;
      Vec3 offset(0.0f);
      switch (gd)
      {
        case GridDir::Xm: dx = -1; offset.x = -selNode->size.x; break;
        case GridDir::Xp: dx =  1; offset.x =  selNode->size.x; break;
        case GridDir::Zm: dz = -1; offset.z = -selNode->size.z; break;
        default:          dz =  1; offset.z =  selNode->size.z; break;
      }
      const Vec3 nbrCenter = selNode->center + offset;

      // Checker material: the opposite of the selected tile's.
      MaterialPtr darkMat  = GetOrCreateCheckerMaterial(true);
      MaterialPtr lightMat = GetOrCreateCheckerMaterial(false);
      MaterialPtr selMat   = tile->GetMaterialComponent()->GetFirstMaterial();
      MaterialPtr newMat   = (selMat == darkMat) ? lightMat : darkMat;

      CubePtr cube = MakeNewPtr<Cube>();
      cube->SetNameVal("Tile_" + std::to_string(selNode->ix + dx) + "x" + std::to_string(selNode->iz + dz));
      cube->SetCubeScaleVal(Vec3(selNode->size.x, g_tileHeight, selNode->size.z));
      cube->GetMeshComponent()->Init(false);
      cube->GetMaterialComponent()->SetFirstMaterial(newMat);

      Vec3 pos(nbrCenter.x, g_tileHeight * 0.5f, nbrCenter.z);
      cube->m_node->SetTranslation(pos, TransformationSpace::TS_WORLD);

      scene->AddEntity(cube);
      gridNode->m_node->AddChild(cube->m_node, true);

      // The freshly extended tile becomes the selection, so the grid can be
      // grown further tile by tile.
      scene->AddToSelection(cube->GetIdVal(), false);

      // Connect the new tile to the selected one: both facing sides open, so
      // the bridge between them rebuilds. Flags are set before Nodes() grows
      // (reallocation would invalidate selNode).
      GridNode n;
      n.ix     = selNode->ix + dx;
      n.iz     = selNode->iz + dz;
      n.center = nbrCenter;
      n.size   = selNode->size;
      n.tile   = cube;

      switch (gd)
      {
        case GridDir::Xp: selNode->xp = true; n.xm = true; break;
        case GridDir::Xm: selNode->xm = true; n.xp = true; break;
        case GridDir::Zp: selNode->zp = true; n.zm = true; break;
        default:          selNode->zm = true; n.zp = true; break;
      }

      graph.Nodes().push_back(n);
      graph.WriteToScene();

      app->SetStatusMsg("Tile extended.");
    }

    MaterialPtr GridEditor::GetOrCreateUnlitColorMaterial(const String& fileName, const Vec3& color)
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

    MaterialPtr GridEditor::GetOrCreateCheckerMaterial(bool dark)
    {
      if (dark)
      {
        return GetOrCreateUnlitColorMaterial("BricksCheckerDark.material", Vec3(g_checkerDark));
      }

      return GetOrCreateUnlitColorMaterial("BricksCheckerLight.material", Vec3(g_checkerLight));
    }

    String GridEditor::ComputeGridSignature(EntityPtr gridNode) const
    {
      // Model the grid from the tiles, then emit a per-cell entry. Ordered by
      // cell so the signature is stable regardless of the order the children
      // are iterated in.
      GridGraph graph;
      graph.LoadFromScene(gridNode);

      std::map<String, String> entries;
      for (const GridNode& n : graph.Nodes())
      {
        String key = RoundKey(n.center.x) + "," + RoundKey(n.center.z);

        String entry = key;
        entry += "|";
        entry += n.xm ? "L" : ".";
        entry += n.xp ? "R" : ".";
        entry += n.zm ? "F" : ".";
        entry += n.zp ? "B" : ".";

        entries[key] = entry;
      }

      String sig = std::to_string(entries.size());
      for (const auto& pair : entries)
      {
        sig += pair.second;
      }

      return sig;
    }

    void GridEditor::UpdateBridges()
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

    void GridEditor::RebuildBridges(EntityPtr gridNode)
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

      // Model the grid from the tiles' connection custom data. This is the
      // single source the reciprocal sync and bridge building below read from.
      GridGraph graph;
      graph.LoadFromScene(gridNode);

      auto cellKey = [](const Vec3& c) { return RoundKey(c.x) + "," + RoundKey(c.z); };

      // ---- Reciprocal flag sync ---------------------------------------------
      // The user edits a single checkbox (say a tile's X- side). Mirror that
      // change onto the facing checkbox of the neighbour tile so the custom
      // data stays symmetric: turning one side off (or on) updates both tiles
      // in one action.
      std::map<String, TileFlags> current;
      for (const GridNode& n : graph.Nodes())
      {
        TileFlags f;
        f.left  = n.xm;
        f.right = n.xp;
        f.front = n.zm;
        f.back  = n.zp;
        current[cellKey(n.center)] = f;
      }

      ObjectId gridId = gridNode->GetIdVal();
      auto storedIt   = m_tileFlags.find(gridId);

      // On the first sighting of a grid only record its state. Propagating then
      // would compare every flag against an empty snapshot and normalize away
      // any deliberate asymmetry loaded from disk.
      if (storedIt != m_tileFlags.end() && !storedIt->second.empty())
      {
        const std::map<String, TileFlags>& stored = storedIt->second;

        // A changed flag is mirrored onto the facing flag of the neighbour
        // node; GridGraph::Mirror also writes the neighbour's tile custom data,
        // so the scene stays in sync with the graph.
        for (GridNode& n : graph.Nodes())
        {
          String key     = cellKey(n.center);
          auto sit       = stored.find(key);
          TileFlags old  = sit != stored.end() ? sit->second : TileFlags();
          if (n.xm != old.left)  graph.Mirror(n, GridDir::Xm);
          if (n.xp != old.right) graph.Mirror(n, GridDir::Xp);
          if (n.zm != old.front) graph.Mirror(n, GridDir::Zm);
          if (n.zp != old.back)  graph.Mirror(n, GridDir::Zp);
        }
      }

      // Persist the synced flags back to the tiles, then store the post-sync
      // snapshot so the next frame's diff starts from reality (the propagation
      // above may have changed neighbour flags).
      graph.WriteToScene();

      std::map<String, TileFlags> postSync;
      for (const GridNode& n : graph.Nodes())
      {
        TileFlags f;
        f.left  = n.xm;
        f.right = n.xp;
        f.front = n.zm;
        f.back  = n.zp;
        postSync[cellKey(n.center)] = f;
      }
      m_tileFlags[gridId] = postSync;

      // A bridge is a quad laid flat on the tile tops (rotated -90 degrees
      // about X), centered on the midpoint of the two tile centers and scaled
      // to span exactly from one center to the other. Scaling a centered quad
      // extends both ways; placing it at the midpoint makes both halves land on
      // the tile centers, so it never pokes past the neighbour.
      std::set<String> placedMidpoints;

      auto createBridge = [&](const GridNode& a, const GridNode& b, bool alongX)
      {
        Vec3 mid     = (a.center + b.center) * 0.5f;
        mid.y        = (a.center.y > b.center.y ? a.center.y : b.center.y) + g_bridgeLift;

        String key = RoundKey(mid.x) + "," + RoundKey(mid.z);
        if (!placedMidpoints.insert(key).second)
        {
          return; // The same bridge was already created from the other node.
        }

        QuadPtr quad = MakeNewPtr<Quad>();
        quad->SetNameVal("Bridge");
        quad->GetMaterialComponent()->SetFirstMaterial(bridgeMat);

        quad->m_node->SetTranslation(mid, TransformationSpace::TS_WORLD);
        quad->m_node->SetOrientation(glm::angleAxis(glm::radians(-90.0f), X_AXIS), TransformationSpace::TS_WORLD);

        // The bridge spans node center to node center along one axis and is a
        // slim strip across the other: its width is 5% of the node's extent on
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

      // A bridge connects two nodes only when BOTH sides flag the facing
      // connection (AND). Otherwise toggling a node's flag off would not remove
      // the bridge at that location, because the neighbour's reciprocal flag
      // would still emit it. The midpoint dedup still collapses the two
      // symmetric attempts (this node and the neighbour) into a single quad.
      for (GridNode& n : graph.Nodes())
      {
        if (GridNode* nb = graph.Neighbor(n, GridDir::Xm))
        {
          if (graph.Connected(n, *nb)) // The neighbour must face back toward n.
          {
            createBridge(n, *nb, true);
          }
        }

        if (GridNode* nb = graph.Neighbor(n, GridDir::Xp))
        {
          if (graph.Connected(n, *nb))
          {
            createBridge(n, *nb, true);
          }
        }

        if (GridNode* nb = graph.Neighbor(n, GridDir::Zm)) // Front faces -Z.
        {
          if (graph.Connected(n, *nb))
          {
            createBridge(n, *nb, false);
          }
        }

        if (GridNode* nb = graph.Neighbor(n, GridDir::Zp)) // Back faces +Z.
        {
          if (graph.Connected(n, *nb))
          {
            createBridge(n, *nb, false);
          }
        }
      }
    }

    void GridEditor::SaveSettings()
    {
      App* app = GetApp();
      if (app == nullptr || app->m_workspace == nullptr)
      {
        return;
      }

      String cfgDir = app->m_workspace->GetConfigDirectory();
      std::filesystem::create_directories(cfgDir);

      String path = ConcatPaths({cfgDir, "GridEditor.settings"});

      std::ofstream file;
      file.open(path.c_str(), std::ios::out | std::ios::trunc);
      if (!file.is_open())
      {
        TK_ERR("GridEditor: Can't open settings file for writing: %s", path.c_str());
        return;
      }

      XmlDocumentPtr doc = MakeNewPtr<XmlDocument>();
      XmlNode* root      = CreateXmlNode(doc.get(), "GridEditor");

      // Serialize the whole window, TKParams included, using the same path the
      // editor uses for its own windows.
      Serialize(doc.get(), root);

      // Placement areas are a dynamic list, so they live outside the TKParams:
      // one <Slot> child per area, storing the name and the resource-relative
      // asset path. Only filled areas persist; the single trailing empty
      // "add a new area" placeholder is runtime-only.
      XmlNode* slotsNode = CreateXmlNode(doc.get(), "PlacementSlots", root);
      for (const PlacementSlot& s : m_slots)
      {
        if (s.absPath.empty())
        {
          continue;
        }

        XmlNode* slotNode = CreateXmlNode(doc.get(), "Slot", slotsNode);
        WriteAttr(slotNode, doc.get(), "name", s.name);
        WriteAttr(slotNode, doc.get(), "path", s.relPath);
      }

      std::string xml;
      rapidxml::print(std::back_inserter(xml), *doc, 0);
      file << xml;
      file.close();
      doc->clear();
    }

    void GridEditor::LoadSettings()
    {
      App* app = GetApp();
      if (app == nullptr || app->m_workspace == nullptr)
      {
        return;
      }

      String path = ConcatPaths({app->m_workspace->GetConfigDirectory(), "GridEditor.settings"});
      if (CheckFile(path))
      {
        XmlFilePtr file       = MakeNewPtr<XmlFile>(path.c_str());
        XmlDocumentPtr doc    = MakeNewPtr<XmlDocument>();
        doc->parse<0>(file->data());

        SerializationFileInfo info;
        info.File     = path;
        info.Document = doc.get();

        if (XmlNode* root = doc->first_node("GridEditor"))
        {
          const char* xmlRootObject = Object::StaticClass()->Name.c_str();
          if (XmlNode* objNode = root->first_node(xmlRootObject))
          {
            // Restores TileSize / GridCols / GridRows / ActiveSlot / PlacementDir.
            DeSerialize(info, objNode);
          }

          // Restore the placement areas: one <Slot> per area (name + relative
          // asset path), and re-derive the runtime fields from those paths.
          m_slots.clear();
          if (XmlNode* slotsNode = root->first_node("PlacementSlots"))
          {
            for (XmlNode* slotNode = slotsNode->first_node("Slot"); slotNode != nullptr;
                 slotNode = slotNode->next_sibling("Slot"))
            {
              PlacementSlot s;
              ReadAttr(slotNode, "name", s.name);
              ReadAttr(slotNode, "path", s.relPath);
              if (!s.relPath.empty())
              {
                s.absPath  = ToResourceAbsolutePath(s.relPath);
                DecomposePath(s.absPath, nullptr, &s.fileName, &s.ext);
              }
              m_slots.push_back(s);
            }
          }
        }
      }

      // Legacy files may carry nameless filled slots; give them auto names.
      for (PlacementSlot& s : m_slots)
      {
        if (!s.absPath.empty() && s.name.empty())
        {
          s.name = NextAreaName(m_slots);
        }
      }

      // A fresh editor starts with one empty area ready to drop into. The
      // empty placeholder has no name; the UI shows it as "<Empty>".
      if (m_slots.empty())
      {
        m_slots.push_back(PlacementSlot());
      }

      // Keep the persisted active-area index valid.
      if (GetActiveSlotVal() < 0 || GetActiveSlotVal() >= (int) m_slots.size())
      {
        SetActiveSlotVal((int) m_slots.size() - 1);
      }
    }

    void GridEditor::Show()
    {
      ImGui::SetNextWindowSize(ImVec2(360, 560), ImGuiCond_Once);
      if (ImGui::Begin(m_name.c_str(), &m_visible))
      {
        HandleStates();

        // Current selection state, used by both the Tile Extend and the
        // Placement sections. A tile is a placement / extension target; a
        // placed object (parented under a tile) can have its direction
        // re-aligned live. Re-derived every frame so the UI can't go stale.
        EditorScenePtr scene = GetApp() ? GetApp()->GetCurrentScene() : nullptr;
        EntityPtr sel        = scene ? scene->GetCurrentSelection() : nullptr;
        const bool onTile    = IsTile(sel);
        const bool onPlaced  = IsPlacedObject(sel);

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

        // ---- Place ----------------------------------------------------------
        ImGui::Spacing();
        const float placeGridW = 120.0f;
        ImGui::SetCursorPosX((ImGui::GetContentRegionAvail().x - placeGridW) * 0.5f);
        if (ImGui::Button("Place Grid", ImVec2(placeGridW, 0)))
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

              // Model the grid first, independently of the scene: an N x M set
              // of nodes on the lattice, all four sides connected. The tiles
              // are created from the nodes below and the connection state is
              // written to them through the graph.
              GridGraph graph;
              float originX = floorf(cursorPos.x / D) * D;
              float originZ = floorf(cursorPos.z / D) * D;

              for (int iz = 0; iz < M; ++iz)
              {
                for (int ix = 0; ix < N; ++ix)
                {
                  GridNode n;
                  n.ix     = ix;
                  n.iz     = iz;
                  n.center = Vec3(originX + ix * D, g_tileHeight, originZ + iz * D); // Tile top-center.
                  n.size   = Vec3(D, g_tileHeight, D);
                  n.xm = n.xp = n.zm = n.zp = true; // A fresh grid is fully bridged.
                  graph.Nodes().push_back(n);
                }
              }

              // Master group: an empty entity that parents every placed tile,
              // keeping the outliner organized.
              EntityPtr master = MakeNewPtr<Entity>();
              master->SetNameVal("GridNode");
              master->m_node->SetTranslation(cursorPos, TransformationSpace::TS_WORLD);
              scene->AddEntity(master);

              for (GridNode& n : graph.Nodes())
              {
                const bool darkTile = ((n.ix + n.iz) % 2) == 1;

                CubePtr cube = MakeNewPtr<Cube>();
                cube->SetNameVal("Tile_" + std::to_string(n.ix) + "x" + std::to_string(n.iz));
                // Re-generates the cube geometry at the tile size. The cube
                // rests on the ground plane when centered at half its height.
                cube->SetCubeScaleVal(Vec3(D, g_tileHeight, D));
                cube->GetMeshComponent()->Init(false);
                cube->GetMaterialComponent()->SetFirstMaterial(darkTile ? darkMat : lightMat);

                Vec3 pos(n.center.x, g_tileHeight * 0.5f, n.center.z);
                cube->m_node->SetTranslation(pos, TransformationSpace::TS_WORLD);

                scene->AddEntity(cube);
                master->m_node->AddChild(cube->m_node, true);
                n.tile = cube;
              }

              // Auto-set the connection custom data so each tile is self
              // contained (bridges work without a prefab author).
              graph.WriteToScene();
            }
          }
        }

        // ---- Tile Extend -----------------------------------------------------
        // Extends the grid one tile at a time: select a tile, pick a compass
        // direction and press Place. The neighbour cell is filled when free;
        // otherwise the operation fails with a status message.
        ImGui::Spacing();
        ImGui::SeparatorText("Tile Extend");

        if (onTile)
        {
          ImGui::Text("Selected tile: %s", sel->GetNameVal().c_str());
        }
        else
        {
          ImGui::TextDisabled("Select a tile to extend the grid.");
        }

        const int extendStartDir = GetTileExtendDirVal();
        int extendDir            = extendStartDir;
        DrawPlacementCompass(
            extendDir,
            onTile,
            [this, sel, extendDir]() -> void
            {
              ExtendTileFrom(sel, extendDir);
            },
            "TileExtendCompass");

        if (extendDir != extendStartDir)
        {
          SetTileExtendDirVal(extendDir);
          SaveSettings();
        }

        // Connections of the selected tile. Toggling a side mirrors the
        // reciprocal flag on the neighbour tile: a connection exists only when
        // both tiles face each other, so a bridge appears/disappears on both.
        if (onTile)
        {
          EntityPtr gridNode = sel->Parent();
          if (gridNode != nullptr)
          {
            GridGraph graph;
            graph.LoadFromScene(gridNode);

            if (GridNode* selNode = graph.NodeAtPoint(GetTileTopCenter(sel)))
            {
              bool xm = selNode->xm;
              bool xp = selNode->xp;
              bool zm = selNode->zm;
              bool zp = selNode->zp;

              // Scoped so the checkbox ids don't clash with the compass radios
              // above (they use the same X-/X+/Z-/Z+ labels).
              ImGui::PushID("TileConnections");
              ImGui::Text("connections:");
              ImGui::SameLine();
              if (ImGui::Checkbox("X-", &xm))
              {
                selNode->xm = xm;
                graph.Mirror(*selNode, GridDir::Xm);
                graph.WriteToScene();
              }
              ImGui::SameLine();
              if (ImGui::Checkbox("X+", &xp))
              {
                selNode->xp = xp;
                graph.Mirror(*selNode, GridDir::Xp);
                graph.WriteToScene();
              }
              ImGui::SameLine();
              if (ImGui::Checkbox("Z-", &zm))
              {
                selNode->zm = zm;
                graph.Mirror(*selNode, GridDir::Zm);
                graph.WriteToScene();
              }
              ImGui::SameLine();
              if (ImGui::Checkbox("Z+", &zp))
              {
                selNode->zp = zp;
                graph.Mirror(*selNode, GridDir::Zp);
                graph.WriteToScene();
              }
              ImGui::PopID();
            }
          }
        }

        // ---- Object List -----------------------------------------------------
        // The placement areas ("dropzones"), each holding one asset. Exactly
        // one empty area is kept at the end as the next area's slot: once it
        // receives a drop, a new empty one is appended. The empty placeholder
        // has no name; the UI shows it as "<Empty>".
        ImGui::Spacing();
        ImGui::SeparatorText("Object List");
        if (m_slots.empty() || !m_slots.back().absPath.empty())
        {
          m_slots.push_back(PlacementSlot());
          SaveSettings();
        }

        // Keep the persisted active-area index valid (removals shift the list).
        if (GetActiveSlotVal() < 0 || GetActiveSlotVal() >= (int) m_slots.size())
        {
          SetActiveSlotVal((int) m_slots.size() - 1);
        }

        // Grid of placement areas, flowing left to right directly in the window
        // (no child scroll region). Each cell is a launcher-style card: a frame
        // around the dropzone with the name centered under it. Click a card to
        // select the area Place acts on; Delete removes the selected area. Only
        // the parent window scrolls when the grid outgrows it.
        const float zoneW = 48.0f + ImGui::GetStyle().FramePadding.x * 2.0f; // DropZone image button footprint
        const float cardW = 84.0f;
        const float cardH = zoneW + ImGui::GetStyle().ItemSpacing.y + ImGui::GetTextLineHeight() + 12.0f;
        const int gridCols =
            glm::max(1, (int) ((ImGui::GetContentRegionAvail().x + ImGui::GetStyle().ItemSpacing.x) /
                               (cardW + ImGui::GetStyle().ItemSpacing.x)));

        int shown = 0;
        for (int i = 0; i < (int) m_slots.size(); ++i)
        {
          PlacementSlot& slot = m_slots[i];

          // Only filled areas are shown, plus the single trailing empty slot
          // that serves as the "add a new area" placeholder.
          if (slot.absPath.empty() && i != (int) m_slots.size() - 1)
          {
            continue;
          }

          if (shown % gridCols != 0)
          {
            ImGui::SameLine();
          }
          ++shown;

          ImGui::PushID(i);

          const bool selected = (i == GetActiveSlotVal());
          const bool filled   = !slot.absPath.empty();

          // A cell is a launcher-style card: a framed area with the dropzone
          // and the name centered under it. The child is borderless /
          // padding-less and sized to fully contain the card, so no per-cell
          // scrollbar ever appears; only the parent window scrolls.
          ImGui::BeginChild("##SlotCell",
                            ImVec2(cardW, cardH),
                            ImGuiChildFlags_None,
                            ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);

          const ImVec2 cellMin = ImGui::GetWindowPos();
          const ImVec2 cellMax = ImVec2(cellMin.x + cardW, cellMin.y + cardH);

          // Launcher-style card frame: base fill, brighter when hovered, accent
          // border when selected.
          const bool hovered = ImGui::IsWindowHovered();
          ImVec4 fill        = ImGui::GetStyle().Colors[ImGuiCol_FrameBg];
          if (hovered)
          {
            fill.x = glm::min(fill.x + 0.02f, 1.0f);
            fill.y = glm::min(fill.y + 0.02f, 1.0f);
            fill.z = glm::min(fill.z + 0.02f, 1.0f);
          }
          ImGui::GetWindowDrawList()->AddRectFilled(cellMin, cellMax, ImGui::GetColorU32(fill), 4.0f);
          if (selected)
          {
            ImGui::GetWindowDrawList()->AddRect(ImVec2(cellMin.x - 1.0f, cellMin.y - 1.0f),
                                                ImVec2(cellMax.x + 1.0f, cellMax.y + 1.0f),
                                                ImGui::GetColorU32(ImVec4(0.9f, 0.6f, 0.2f, 1.0f)),
                                                4.0f,
                                                0,
                                                2.0f);
          }

          // The dropzone accepts a mesh / skinMesh / scene prefab from the
          // asset browser. The area's own path is fed back as the file, so a
          // filled area shows that asset's thumbnail (empty path = plain drop
          // target with the fallback icon).
          ImGui::SetCursorPos(ImVec2((cardW - zoneW) * 0.5f, 6.0f));
          View::DropZone(EditorImGuiTextureCache::Acquire(UI::m_meshIcon),
                         slot.absPath,
                         [this, i](DirectoryEntry& entry) -> void
                         {
                           // Only assets the placement tool can instantiate
                           // are useful in an area.
                           if (entry.m_ext != MESH && entry.m_ext != SKINMESH && entry.m_ext != SCENE)
                           {
                             GetApp()->SetStatusMsg("GridEditor: drop a mesh, skinMesh or scene prefab into an area.");
                             return;
                           }

                           PlacementSlot& s = m_slots[i];
                           s.absPath  = entry.GetFullPath();
                           s.ext      = entry.m_ext;
                           s.fileName = entry.m_fileName;
                           // Persist as a workspace-relative path so the
                           // setting survives workspace moves; keep the
                           // absolute form for the runtime DropZone /
                           // instantiation.
                           s.relPath = ToResourceRelativePath(s.absPath);
                           // The empty placeholder is nameless; give it an auto
                           // name now that it holds an asset.
                           if (s.name.empty())
                           {
                             s.name = NextAreaName(m_slots);
                           }
                           // The area that just received a drop becomes the
                           // active one, so Place drops it right away.
                           SetActiveSlotVal(i);
                           SaveSettings();
                         },
                         "");

          // A click on the dropzone selects the area.
          if (ImGui::IsItemClicked(ImGuiMouseButton_Left))
          {
            SetActiveSlotVal(i);
          }

          // Name under the card, centered: the asset name when filled,
          // "<Empty>" for the empty placeholder. Only the asset name shows in
          // the tooltip.
          const String label = TruncateLabel(filled ? slot.fileName : "<Empty>", 10);
          const float labelW = ImGui::CalcTextSize(label.c_str()).x;
          ImGui::SetCursorPos(ImVec2(glm::max(0.0f, (cardW - labelW - 4.0f) * 0.5f),
                                     6.0f + zoneW + ImGui::GetStyle().ItemSpacing.y));
          if (ImGui::Selectable(label.c_str(), selected, ImGuiSelectableFlags_None, ImVec2(labelW + 4.0f, 0)))
          {
            SetActiveSlotVal(i);
          }
          if (filled && ImGui::IsItemHovered())
          {
            ImGui::BeginTooltip();
            ImGui::TextUnformatted((slot.fileName + slot.ext).c_str());
            ImGui::EndTooltip();
          }

          ImGui::EndChild();
          ImGui::PopID();
        }

        // Delete removes the selected area: press the Delete key or use the
        // Delete button below.
        const int delSlot    = GetActiveSlotVal();
        const bool canDelete = delSlot >= 0 && delSlot < (int) m_slots.size() && !m_slots[delSlot].absPath.empty();
        if (canDelete && ImGui::IsWindowFocused() && ImGui::IsKeyPressed(ImGuiKey_Delete, false))
        {
          m_slots.erase(m_slots.begin() + delSlot);
          if (GetActiveSlotVal() >= (int) m_slots.size())
          {
            SetActiveSlotVal((int) m_slots.size() - 1);
          }
          SaveSettings();
        }

        // Delete button: removes the selected area from the list.
        ImGui::Spacing();
        ImGui::BeginDisabled(!canDelete);
        const float delW = 80.0f;
        ImGui::SetCursorPosX((ImGui::GetContentRegionAvail().x - delW) * 0.5f);
        if (ImGui::Button("Delete", ImVec2(delW, 0)))
        {
          m_slots.erase(m_slots.begin() + delSlot);
          if (GetActiveSlotVal() >= (int) m_slots.size())
          {
            SetActiveSlotVal((int) m_slots.size() - 1);
          }
          SaveSettings();
        }
        ImGui::EndDisabled();

        // ---- Object Placement ------------------------------------------------
        // The placement component: pick an area (from the Object List) and a
        // compass direction, then press Place. When a placed object is
        // selected, the compass radios re-align it instead.
        ImGui::Spacing();
        ImGui::SeparatorText("Object Placement");

        ImGui::Spacing();
        if (onPlaced)
        {
          ImGui::Text("Selected: %s (placed)", sel->GetNameVal().c_str());
        }
        else if (onTile)
        {
          ImGui::Text("Selected tile: %s", sel->GetNameVal().c_str());
        }
        else
        {
          ImGui::TextDisabled("Select a tile to place, or a placed object to re-align.");
        }

        // The active area drives Place: whatever asset it holds is placed.
        const int activeSlot = GetActiveSlotVal();
        const bool hasAsset =
            activeSlot >= 0 && activeSlot < (int) m_slots.size() && !m_slots[activeSlot].absPath.empty();
        if (hasAsset)
        {
          const PlacementSlot& active = m_slots[activeSlot];
          ImGui::Text("Place: %s (%s)", active.name.c_str(), active.fileName.c_str());
        }
        else
        {
          ImGui::TextDisabled("Select an area that holds an asset to place.");
        }

        // Compass: when a placed object is selected it shows (and edits) that
        // object's facing; otherwise it holds the persisted direction for the
        // next Place.
        ImGui::Spacing();
        const int startDir = onPlaced ? FacingDir(sel) : GetPlacementDirVal();
        int dir            = startDir;
        DrawPlacementCompass(
            dir,
            !onPlaced && hasAsset,
            [this, dir]() -> void
            {
              SetPlacementDirVal(dir);
              PlaceObjectOnSelectedTile(m_slots[GetActiveSlotVal()]);
            },
            "GridPlacementCompass");

        // Apply a direction change: re-align the selected placed object, or
        // store the new default direction for the next Place.
        if (dir != startDir)
        {
          if (onPlaced)
          {
            ReorientPlacedObject(sel, dir);
          }

          SetPlacementDirVal(dir);
          SaveSettings();
        }
      }
      ImGui::End();
    }

  } // namespace Editor
} // namespace ToolKit
