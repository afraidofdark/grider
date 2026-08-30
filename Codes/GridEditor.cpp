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
      PlacementPath_Define("", "Placement", 0, false, false);
      PlacementDir_Define(PlacementDirZm, "Placement", 0, false, false);
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

    EntityPtr GridEditor::InstantiatePlacement(const EditorScenePtr& scene, const String& fullPath, const String& ext)
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
        prefab->SetNameVal(m_placementName);
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
        entity->SetNameVal(m_placementName);
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

    void GridEditor::PlaceObjectOnSelectedTile()
    {
      App* app = GetApp();
      if (app == nullptr)
      {
        return;
      }

      if (m_placementPath.empty())
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

      EntityPtr obj = InstantiatePlacement(scene, m_placementPath, m_placementExt);
      if (obj == nullptr)
      {
        return;
      }

      // Face the object's -Z (front) toward the selected direction.
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

      if (XmlNode* root = doc->first_node("GridEditor"))
      {
        const char* xmlRootObject = Object::StaticClass()->Name.c_str();
        if (XmlNode* objNode = root->first_node(xmlRootObject))
        {
          // Restores TileSize / GridCols / GridRows / PlacementPath / PlacementDir.
          DeSerialize(info, objNode);
        }
      }

      // Restore the placement asset from its persisted resource-relative path
      // and re-derive the runtime fields from it.
      const String& rel = GetPlacementPathVal();
      if (!rel.empty())
      {
        m_placementPath = ToResourceAbsolutePath(rel);
        DecomposePath(m_placementPath, nullptr, &m_placementName, &m_placementExt);
      }
    }

    void GridEditor::Show()
    {
      ImGui::SetNextWindowSize(ImVec2(340, 440), ImGuiCond_Once);
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

        // ---- Placement -------------------------------------------------------
        ImGui::Spacing();
        ImGui::SeparatorText("Placement");

        // DropZone: accepts a mesh / skinMesh / scene prefab from the asset
        // browser. Stores the dropped file so Place can instantiate it. The
        // current m_placementPath is fed back as the file, so once something is
        // dropped the zone shows that asset's thumbnail (empty path = plain
        // drop target with the fallback icon).
        View::DropZone(EditorImGuiTextureCache::Acquire(UI::m_meshIcon),
                       m_placementPath,
                       [this](DirectoryEntry& entry) -> void
                       {
                         m_placementPath = entry.GetFullPath();
                         m_placementExt  = entry.m_ext;
                         m_placementName = entry.m_fileName;
                         // Persist as a workspace-relative path so the setting
                         // survives workspace moves; keep the absolute form for
                         // the runtime DropZone / instantiation.
                         SetPlacementPathVal(ToResourceRelativePath(m_placementPath));
                         SaveSettings();
                       },
                       "Prefab / Mesh");

        if (!m_placementName.empty())
        {
          ImGui::Text("Drop: %s", m_placementName.c_str());
        }
        else
        {
          ImGui::TextDisabled("Drop a mesh or prefab (.scene) above.");
        }

        // What the placement section acts on this frame. A tile is a placement
        // target (Place drops a new object onto it); a placed object (parented
        // under a tile) can have its direction re-aligned live. The mode is
        // re-derived from the selection every frame, so the UI can't go stale.
        EditorScenePtr scene = GetApp() ? GetApp()->GetCurrentScene() : nullptr;
        EntityPtr sel        = scene ? scene->GetCurrentSelection() : nullptr;
        const bool onTile    = IsTile(sel);
        const bool onPlaced  = IsPlacedObject(sel);

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

        // Compass: the four axis directions around a center Place button.
        // Matches the grid axis convention (Left=-X, Right=+X, Front=-Z).
        // When a placed object is selected the compass shows (and edits) that
        // object's facing; otherwise it holds the persisted direction for the
        // next Place.
        ImGui::Spacing();
        ImGui::PushID("GridPlacementCompass");
        const int startDir = onPlaced ? FacingDir(sel) : GetPlacementDirVal();
        int dir            = startDir;
        if (ImGui::BeginTable("##GridPlacementCompass", 3))
        {
          ImGui::TableNextRow();
          ImGui::TableNextColumn();
          ImGui::Dummy(ImVec2(24, 24));
          ImGui::TableNextColumn();
          ImGui::RadioButton("Z+", &dir, PlacementDirZp);
          ImGui::TableNextColumn();
          ImGui::Dummy(ImVec2(24, 24));

          ImGui::TableNextRow();
          ImGui::TableNextColumn();
          ImGui::RadioButton("X-", &dir, PlacementDirXm);
          ImGui::TableNextColumn();
          if (onPlaced)
          {
            // A placed object is re-oriented by its compass radios directly.
            ImGui::BeginDisabled();
            ImGui::Button("Place", ImVec2(64, 0));
            ImGui::EndDisabled();
          }
          else if (ImGui::Button("Place", ImVec2(64, 0)))
          {
            SetPlacementDirVal(dir);
            PlaceObjectOnSelectedTile();
          }
          ImGui::TableNextColumn();
          ImGui::RadioButton("X+", &dir, PlacementDirXp);

          ImGui::TableNextRow();
          ImGui::TableNextColumn();
          ImGui::Dummy(ImVec2(24, 24));
          ImGui::TableNextColumn();
          ImGui::RadioButton("Z-", &dir, PlacementDirZm);
          ImGui::TableNextColumn();
          ImGui::Dummy(ImVec2(24, 24));

          ImGui::EndTable();
        }

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
        ImGui::PopID();
      }
      ImGui::End();
    }

  } // namespace Editor
} // namespace ToolKit
