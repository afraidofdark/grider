/*
 * Copyright (c) 2019-2026 OtSoftware
 * This code is licensed under the GNU Lesser General Public License v3.0 (LGPL-3.0).
 * For more information, including options for a more permissive commercial license,
 * please visit [otsoftware.tr] or contact us at [info@otsoftare.tr].
 */

#include "DioramaBuilder.h"

#include <Entity.h>
#include <Logger.h>
#include <MaterialComponent.h>
#include <Mesh.h>
#include <MeshComponent.h>
#include <ParameterBlock.h>
#include <Primative.h>
#include <Util.h>

#include <algorithm>
#include <filesystem>
#include <utility>

namespace ToolKit
{
  namespace Editor
  {

    namespace
    {
      const char* g_dioramaNodeName = "DioramaNode";

      // Mesh entities of a diorama, one per material.
      const char* g_baseMeshName    = "DioramaBase";
      const char* g_frameMeshName   = "DioramaFrame";
      const char* g_bracketMeshName = "DioramaBrackets";

      // Project materials the tool creates and keeps in sync with the color
      // parameters of the window.
      const char* g_baseMaterialFile    = "DioramaBase.material";
      const char* g_frameMaterialFile   = "DioramaFrame.material";
      const char* g_bracketMaterialFile = "DioramaBracket.material";

      // Custom data names of the recipe stored on the master entity. The shape
      // values are multiples of the tile size (see Diorama::Params); the key
      // names carry the wall vocabulary so a recipe written by an older build
      // (which stored world units under the old names) falls back to the
      // current defaults instead of being read as tile units.
      const char* g_cellsVar        = "Cells";
      const char* g_originXVar      = "OriginX";
      const char* g_originZVar      = "OriginZ";
      const char* g_tileXVar        = "TileX";
      const char* g_tileZVar        = "TileZ";
      const char* g_floorYVar       = "FloorY";
      const char* g_tileHeightVar   = "TileHeight";
      const char* g_marginVar       = "PlinthMargin";
      const char* g_baseDepthVar    = "PlinthDepth";
      const char* g_wallHeightVar   = "WallHeight";
      const char* g_wallThickVar    = "WallThickness";
      const char* g_cornerSizeVar   = "CornerSize";
      const char* g_cornerRiseVar   = "CornerRise";
      const char* g_overhangVar     = "CornerOverhang";
      const char* g_solidVar        = "Solid";
      const char* g_baseColorVar    = "BaseColor";
      const char* g_frameColorVar   = "WallColor";
      const char* g_bracketColorVar = "BracketColor";

      // Writes (or updates) a custom data entry of the entity, the same way the
      // grid stores its connection flags on a tile.
      template <typename T>
      void WriteCustom(EntityPtr entity, const String& name, const T& value)
      {
        ParameterVariant* found = nullptr;
        if (entity->m_localData.LookUp(CustomDataCategory.Name, name, &found))
        {
          *found = value;
          return;
        }

        ParameterVariant var(value);
        var.m_name     = name;
        var.m_category = CustomDataCategory;
        entity->m_localData.Add(var);
      }

      // Reads a custom data entry, falling back to a default when the entity
      // does not carry it (an older diorama, or one from a future version).
      template <typename T>
      T ReadCustom(EntityPtr entity, const String& name, const T& fallback)
      {
        ParameterVariant* found = nullptr;
        if (entity->m_localData.LookUp(CustomDataCategory.Name, name, &found))
        {
          return found->GetCVar<T>();
        }

        return fallback;
      }

      // "x,z;x,z;..." -- the footprint of the diorama, sorted so the same cell
      // set always produces the same text (the signature compares it).
      String EncodeCells(const std::vector<Diorama::Cell>& cells)
      {
        std::vector<Diorama::Cell> sorted = cells;
        std::sort(sorted.begin(),
                  sorted.end(),
                  [](const Diorama::Cell& a, const Diorama::Cell& b)
                  {
                    return a.z != b.z ? a.z < b.z : a.x < b.x;
                  });

        String text;
        for (const Diorama::Cell& cell : sorted)
        {
          if (!text.empty())
          {
            text += ";";
          }
          text += std::to_string(cell.x) + "," + std::to_string(cell.z);
        }

        return text;
      }

      void DecodeCells(const String& text, std::vector<Diorama::Cell>& cells)
      {
        cells.clear();

        size_t start = 0;
        while (start < text.size())
        {
          const size_t end = text.find(';', start);
          const String entry = text.substr(start, end == String::npos ? String::npos : end - start);

          const size_t comma = entry.find(',');
          if (comma != String::npos)
          {
            try
            {
              Diorama::Cell cell;
              cell.x = std::stoi(entry.substr(0, comma));
              cell.z = std::stoi(entry.substr(comma + 1));
              cells.push_back(cell);
            }
            catch (const std::exception&)
            {
              TK_ERR("Diorama: can't read the cell '%s' of a stored footprint.", entry.c_str());
            }
          }

          if (end == String::npos)
          {
            break;
          }
          start = end + 1;
        }
      }

      // Appends the boxes to a mesh as cube geometry. The engine's own cube
      // generator is reused per box, so the winding, normals and UVs of the
      // frame are exactly the ones the editor's primitives use.
      void AppendBoxes(MeshPtr mesh, const std::vector<Diorama::Box>& boxes)
      {
        VertexArray& vertices = mesh->m_clientSideVertices;
        UIntArray& indices    = mesh->m_clientSideIndices;

        mesh->UnInit(); // Drop the GPU buffers of a previous build.
        vertices.clear();
        indices.clear();

        if (boxes.empty())
        {
          return;
        }

        MeshPtr unit = MakeNewPtr<Mesh>();
        for (const Diorama::Box& box : boxes)
        {
          // A hair of size keeps a flat box (a zero thickness parameter)
          // renderable instead of degenerate.
          const Vec3 size(glm::max(box.maxX - box.minX, 0.0001f),
                          glm::max(box.maxY - box.minY, 0.0001f),
                          glm::max(box.maxZ - box.minZ, 0.0001f));
          const Vec3 center(0.5f * (box.minX + box.maxX), 0.5f * (box.minY + box.maxY), 0.5f * (box.minZ + box.maxZ));

          MeshGenerator::GenerateCube(unit, size);

          const uint offset = static_cast<uint>(vertices.size());
          for (const Vertex& vertex : unit->m_clientSideVertices)
          {
            Vertex moved = vertex;
            moved.pos += center;
            vertices.push_back(moved);
          }
          for (uint index : unit->m_clientSideIndices)
          {
            indices.push_back(offset + index);
          }
        }

        mesh->m_vertexCount = static_cast<uint>(vertices.size());
        mesh->m_indexCount  = static_cast<uint>(indices.size());
        mesh->CalculateAABB();
        mesh->ConstructFaces();
      }

      // Creates the entity of one part of the diorama. An empty part (no
      // brackets on a shape without turns, say) creates nothing at all.
      EntityPtr MakeMeshEntity(EditorScenePtr scene,
                               const String& name,
                               const std::vector<Diorama::Box>& boxes,
                               MaterialPtr material)
      {
        if (boxes.empty() || material == nullptr)
        {
          return nullptr;
        }

        EntityPtr entity = MakeNewPtr<Entity>();
        entity->SetNameVal(name);
        entity->AddComponent<MeshComponent>();

        MeshPtr mesh = MakeNewPtr<Mesh>();
        AppendBoxes(mesh, boxes);
        mesh->m_material = material;

        entity->GetMeshComponent()->SetMeshVal(mesh);
        entity->GetMeshComponent()->Init(false);

        MaterialComponentPtr matComp = entity->AddComponent<MaterialComponent>();
        matComp->UpdateMaterialList(); // Picks the material up from the mesh.
        if (matComp->GetMaterialList().empty())
        {
          matComp->SetFirstMaterial(material);
        }

        scene->AddEntity(entity);
        return entity;
      }

      // Removes every mesh entity hanging under the master, the way the bridge
      // rebuild clears its own children.
      void ClearMeshChildren(EditorScenePtr scene, EntityPtr node)
      {
        EntityPtrArray removed;
        NodeRawPtrArray children = node->m_node->m_children;
        for (Node* child : children)
        {
          if (EntityPtr entity = child->OwnerEntity())
          {
            entity->m_node->OrphanSelf(false);
            removed.push_back(entity);
          }
        }

        if (!removed.empty())
        {
          scene->RemoveEntity(removed, true);
        }
      }

      // True when two dioramas wrap the same cells of the same lattice.
      bool SameFootprint(const DioramaSpec& a, const DioramaSpec& b)
      {
        if (a.cells.size() != b.cells.size())
        {
          return false;
        }

        const Diorama::Layout& la = a.layout;
        const Diorama::Layout& lb = b.layout;
        if (fabsf(la.originX - lb.originX) > 0.001f || fabsf(la.originZ - lb.originZ) > 0.001f ||
            fabsf(la.tileX - lb.tileX) > 0.001f || fabsf(la.tileZ - lb.tileZ) > 0.001f)
        {
          return false;
        }

        return EncodeCells(a.cells) == EncodeCells(b.cells);
      }

      EntityPtr FindDioramaNode(EditorScenePtr scene, const DioramaSpec& spec)
      {
        for (EntityPtr entity : scene->GetEntities())
        {
          if (!IsDioramaNode(entity))
          {
            continue;
          }

          DioramaSpec stored;
          if (ReadDioramaSpec(entity, stored) && SameFootprint(stored, spec))
          {
            return entity;
          }
        }

        return nullptr;
      }

      // Builds the three meshes of a diorama under its master. Split out so a
      // rebuild from a stored recipe and a fresh build share one path.
      void BuildMeshes(EditorScenePtr scene, EntityPtr node, const DioramaSpec& spec, const Diorama::Parts& parts)
      {
        ClearMeshChildren(scene, node);

        EntityPtr base = MakeMeshEntity(scene,
                                        g_baseMeshName,
                                        parts.base,
                                        GetOrCreateColorMaterial(g_baseMaterialFile, spec.baseColor, true));
        EntityPtr frame = MakeMeshEntity(scene,
                                         g_frameMeshName,
                                         parts.frame,
                                         GetOrCreateColorMaterial(g_frameMaterialFile, spec.frameColor, true));
        EntityPtr brackets = MakeMeshEntity(scene,
                                            g_bracketMeshName,
                                            parts.brackets,
                                            GetOrCreateColorMaterial(g_bracketMaterialFile, spec.bracketColor, true));

        const EntityPtr meshes[3] {base, frame, brackets};
        for (EntityPtr mesh : meshes)
        {
          if (mesh != nullptr)
          {
            node->m_node->AddChild(mesh->m_node, true);
          }
        }
      }
    } // namespace

    const char* DioramaNodeName() { return g_dioramaNodeName; }

    bool IsDioramaNode(EntityPtr entity)
    {
      return entity != nullptr && entity->GetNameVal() == g_dioramaNodeName;
    }

    void WriteDioramaSpec(EntityPtr node, const DioramaSpec& spec)
    {
      if (node == nullptr)
      {
        return;
      }

      WriteCustom(node, g_cellsVar, EncodeCells(spec.cells));
      WriteCustom(node, g_originXVar, spec.layout.originX);
      WriteCustom(node, g_originZVar, spec.layout.originZ);
      WriteCustom(node, g_tileXVar, spec.layout.tileX);
      WriteCustom(node, g_tileZVar, spec.layout.tileZ);
      WriteCustom(node, g_floorYVar, spec.layout.floorY);
      WriteCustom(node, g_tileHeightVar, spec.layout.tileHeight);
      WriteCustom(node, g_marginVar, spec.shape.plinthMargin);
      WriteCustom(node, g_baseDepthVar, spec.shape.plinthDepth);
      WriteCustom(node, g_wallHeightVar, spec.shape.wallHeight);
      WriteCustom(node, g_wallThickVar, spec.shape.wallThickness);
      WriteCustom(node, g_cornerSizeVar, spec.shape.cornerSize);
      WriteCustom(node, g_cornerRiseVar, spec.shape.cornerRise);
      WriteCustom(node, g_overhangVar, spec.shape.cornerOverhang);
      WriteCustom(node, g_solidVar, spec.shape.solid);
      WriteCustom(node, g_baseColorVar, spec.baseColor);
      WriteCustom(node, g_frameColorVar, spec.frameColor);
      WriteCustom(node, g_bracketColorVar, spec.bracketColor);
    }

    bool ReadDioramaSpec(EntityPtr node, DioramaSpec& spec)
    {
      if (node == nullptr)
      {
        return false;
      }

      const String cells = ReadCustom<String>(node, g_cellsVar, String());
      if (cells.empty())
      {
        return false;
      }

      DecodeCells(cells, spec.cells);
      if (spec.cells.empty())
      {
        return false;
      }

      // The default shape is the one the window starts with, so a diorama from
      // an older file still rebuilds into something sensible.
      spec.layout.originX    = ReadCustom<float>(node, g_originXVar, 0.0f);
      spec.layout.originZ    = ReadCustom<float>(node, g_originZVar, 0.0f);
      spec.layout.tileX      = ReadCustom<float>(node, g_tileXVar, 1.0f);
      spec.layout.tileZ      = ReadCustom<float>(node, g_tileZVar, 1.0f);
      spec.layout.floorY     = ReadCustom<float>(node, g_floorYVar, 0.0f);
      spec.layout.tileHeight = ReadCustom<float>(node, g_tileHeightVar, 0.0f);
      spec.shape.plinthMargin = ReadCustom<float>(node, g_marginVar, spec.shape.plinthMargin);
      spec.shape.plinthDepth  = ReadCustom<float>(node, g_baseDepthVar, spec.shape.plinthDepth);
      spec.shape.wallHeight   = ReadCustom<float>(node, g_wallHeightVar, spec.shape.wallHeight);
      spec.shape.wallThickness = ReadCustom<float>(node, g_wallThickVar, spec.shape.wallThickness);
      spec.shape.cornerSize   = ReadCustom<float>(node, g_cornerSizeVar, spec.shape.cornerSize);
      spec.shape.cornerRise   = ReadCustom<float>(node, g_cornerRiseVar, spec.shape.cornerRise);
      spec.shape.cornerOverhang = ReadCustom<float>(node, g_overhangVar, spec.shape.cornerOverhang);
      spec.shape.solid          = ReadCustom<bool>(node, g_solidVar, spec.shape.solid);
      spec.baseColor            = ReadCustom<Vec3>(node, g_baseColorVar, spec.baseColor);
      spec.frameColor           = ReadCustom<Vec3>(node, g_frameColorVar, spec.frameColor);
      spec.bracketColor         = ReadCustom<Vec3>(node, g_bracketColorVar, spec.bracketColor);

      return true;
    }

    String DioramaSignature(const DioramaSpec& spec)
    {
      String signature = EncodeCells(spec.cells);

      const float numbers[] {spec.layout.originX,
                             spec.layout.originZ,
                             spec.layout.tileX,
                             spec.layout.tileZ,
                             spec.layout.floorY,
                             spec.layout.tileHeight,
                             spec.shape.plinthMargin,
                             spec.shape.plinthDepth,
                             spec.shape.wallHeight,
                             spec.shape.wallThickness,
                             spec.shape.cornerSize,
                             spec.shape.cornerRise,
                             spec.shape.cornerOverhang,
                             spec.shape.solid ? 1.0f : 0.0f,
                             spec.baseColor.x,
                             spec.baseColor.y,
                             spec.baseColor.z,
                             spec.frameColor.x,
                             spec.frameColor.y,
                             spec.frameColor.z,
                             spec.bracketColor.x,
                             spec.bracketColor.y,
                             spec.bracketColor.z};

      for (float number : numbers)
      {
        signature += "|" + std::to_string(number);
      }

      return signature;
    }

    EntityPtr BuildDiorama(EditorScenePtr scene, EntityPtr parent, const DioramaSpec& spec)
    {
      if (scene == nullptr || spec.cells.empty())
      {
        return nullptr;
      }

      // Build the shape first: a footprint that produces nothing (a margin of
      // zero, say) must not leave an empty master behind in the scene.
      Diorama::Parts parts;
      Diorama::BuildParts(spec.cells, spec.layout, spec.shape, parts);
      if (parts.base.empty() && parts.frame.empty() && parts.brackets.empty())
      {
        return nullptr;
      }

      // A diorama that already wraps this footprint is refreshed instead of
      // duplicated, so building twice does not stack two frames on each other.
      EntityPtr node = FindDioramaNode(scene, spec);
      if (node == nullptr)
      {
        node = MakeNewPtr<Entity>();
        node->SetNameVal(g_dioramaNodeName);

        // The mesh is generated in world space, so the master has to sit at the
        // identity: it is placed at the origin and then parented to the grid
        // keeping its world transform, which is what makes the frame follow the
        // grid the footprint belongs to.
        node->m_node->SetTranslation(Vec3(0.0f), TransformationSpace::TS_WORLD);
        scene->AddEntity(node);

        if (parent != nullptr)
        {
          parent->m_node->AddChild(node->m_node, true);
        }
      }
      else if (parent != nullptr && node->Parent() != parent)
      {
        parent->m_node->AddChild(node->m_node, true);
      }

      WriteDioramaSpec(node, spec);
      BuildMeshes(scene, node, spec, parts);

      return node;
    }

    bool RebuildDiorama(EditorScenePtr scene, EntityPtr node)
    {
      if (scene == nullptr || node == nullptr)
      {
        return false;
      }

      DioramaSpec spec;
      if (!ReadDioramaSpec(node, spec))
      {
        return false;
      }

      Diorama::Parts parts;
      Diorama::BuildParts(spec.cells, spec.layout, spec.shape, parts);
      BuildMeshes(scene, node, spec, parts);
      return true;
    }

    void RemoveDiorama(EditorScenePtr scene, EntityPtr node)
    {
      if (scene == nullptr || node == nullptr)
      {
        return;
      }

      ClearMeshChildren(scene, node);
      node->m_node->OrphanSelf(false);
      scene->RemoveEntity(EntityPtrArray {node}, false);
    }

    MaterialPtr GetOrCreateColorMaterial(const String& fileName, const Vec3& color, bool applyColor)
    {
      const String path = MaterialPath(fileName);

      // Idempotent: the material is saved with the project, so an existing
      // material file is loaded (and cached) instead of re-created.
      if (CheckFile(path))
      {
        MaterialPtr material = GetMaterialManager()->Create<Material>(path);

        // The color parameter of the window owns the tool's own materials: an
        // existing one is retinted instead of being left at its old color.
        if (applyColor && glm::distance(material->GetColorVal(), color) > 0.001f)
        {
          material->SetColorVal(color);
          material->Save(false);
        }

        return material;
      }

      std::filesystem::create_directories(std::filesystem::path(path).parent_path());

      MaterialPtr material = GetMaterialManager()->GetCopyOfUnlitColorMaterial(false);
      material->SetFile(path);
      material->SetColorVal(color);
      material->Init(false);
      material->Save(false);
      GetMaterialManager()->Manage(material);

      return material;
    }

  } // namespace Editor
} // namespace ToolKit
