/*
 * Copyright (c) 2019-2026 OtSoftware
 * This code is licensed under the GNU Lesser General Public License v3.0 (LGPL-3.0).
 * For more information, including options for a more permissive commercial license,
 * please visit [otsoftware.tr] or contact us at [info@otsoftare.tr].
 */

#include "GridGraph.h"

#include <ParameterBlock.h>
#include <Prefab.h>

#include <cmath>
#include <cfloat>

namespace ToolKit
{
  namespace Editor
  {

    namespace
    {
      // Rounds a float to millimetre precision for stable spatial keys.
      String RoundKey(float v)
      {
        return std::to_string(static_cast<long long>(std::round(v * 1000.0f)));
      }

      // Spatial key of a node center: the same millimetre-stable key used by the
      // editor to match a tile with the cells around it.
      String CellKey(const Vec3& center)
      {
        return RoundKey(center.x) + "," + RoundKey(center.z);
      }
    }

    const char* GridGraph::DirFlagName(GridDir d)
    {
      switch (d)
      {
        case GridDir::Xm: return "X-";
        case GridDir::Xp: return "X+";
        case GridDir::Zm: return "Z-";
        default: return "Z+";
      }
    }

    void GridGraph::Clear()
    {
      m_nodes.clear();
      m_index.clear();
    }

    EntityPtr GridGraph::GetDataEntity(EntityPtr tile)
    {
      // Legacy prefab tiles carry the connection custom data on an inner "Tile"
      // entity; auto-generated tiles carry it on themselves.
      if (Prefab* prefab = tile->As<Prefab>())
      {
        if (EntityPtr tileEntity = prefab->GetFirstByName("Tile"))
        {
          return tileEntity;
        }
      }

      return tile;
    }

    bool GridGraph::ReadFlag(const GridNode& n, const char* name) const
    {
      if (n.tile == nullptr)
      {
        return false;
      }

      EntityPtr data = GetDataEntity(n.tile);
      ParameterVariant* var = nullptr;
      if (data->m_localData.LookUp(CustomDataCategory.Name, name, &var))
      {
        return var->GetCVar<bool>();
      }

      return false;
    }

    void GridGraph::WriteFlag(GridNode& n, const char* name, bool value)
    {
      if (n.tile == nullptr)
      {
        return;
      }

      EntityPtr data = GetDataEntity(n.tile);
      ParameterVariant* var = nullptr;
      if (!data->m_localData.LookUp(CustomDataCategory.Name, name, &var))
      {
        // The tile does not carry the flag yet: create it so the connection
        // state is fully self contained on the tile.
        ParameterVariant newVar(value);
        newVar.m_name     = name;
        newVar.m_category = CustomDataCategory;
        data->m_localData.Add(newVar);
      }
      else
      {
        *var = value;
      }
    }

    void GridGraph::LoadFromScene(EntityPtr gridNode)
    {
      Clear();

      if (gridNode == nullptr)
      {
        return;
      }

      // Collect the tiles: direct children of the grid root, excluding the
      // plugin's BridgeNode master. Same guards as the editor's grid signature.
      for (Node* childNode : gridNode->m_node->m_children)
      {
        EntityPtr child = childNode->OwnerEntity();
        if (child == nullptr || child->GetNameVal() == "BridgeNode")
        {
          continue;
        }

        // Some editor delete paths remove the entity from the scene without
        // orphaning its node, and the undo stack keeps it alive. Treat it as
        // gone.
        if (child->m_scene.expired())
        {
          continue;
        }

        // Node position = the tile top-surface center (same math the editor
        // uses for bridge placement); size = the tile AABB extent.
        Vec3 pos       = child->m_node->GetTranslation(TransformationSpace::TS_WORLD);
        BoundingBox bb = child->GetBoundingBox();

        GridNode n;
        n.tile    = child;
        n.center  = Vec3(pos.x + (bb.min.x + bb.max.x) * 0.5f, pos.y + bb.max.y, pos.z + (bb.min.z + bb.max.z) * 0.5f);
        n.size    = bb.max - bb.min;
        n.xm      = ReadFlag(n, "X-");
        n.xp      = ReadFlag(n, "X+");
        n.zm      = ReadFlag(n, "Z-");
        n.zp      = ReadFlag(n, "Z+");
        m_nodes.push_back(n);
      }

      // Best-effort lattice indices from the grid's minimum corner, so nodes can
      // be addressed as (col, row) even though the model is position-based.
      float minX = FLT_MAX, minZ = FLT_MAX;
      for (const GridNode& n : m_nodes)
      {
        minX = glm::min(minX, n.center.x);
        minZ = glm::min(minZ, n.center.z);
      }

      float D = m_nodes.empty() ? 1.0f : m_nodes[0].size.x;
      if (D <= 0.0f)
      {
        D = 1.0f;
      }

      for (GridNode& n : m_nodes)
      {
        n.ix = static_cast<int>(std::round((n.center.x - minX) / D));
        n.iz = static_cast<int>(std::round((n.center.z - minZ) / D));
      }

      // Rebuild the spatial index.
      m_index.clear();
      for (size_t i = 0; i < m_nodes.size(); ++i)
      {
        m_index[CellKey(m_nodes[i].center)] = i;
      }
    }

    void GridGraph::WriteToScene()
    {
      for (GridNode& n : m_nodes)
      {
        WriteFlag(n, "X-", n.xm);
        WriteFlag(n, "X+", n.xp);
        WriteFlag(n, "Z-", n.zm);
        WriteFlag(n, "Z+", n.zp);
      }
    }

    std::vector<GridNode>& GridGraph::Nodes() { return m_nodes; }

    const std::vector<GridNode>& GridGraph::Nodes() const { return m_nodes; }

    GridNode* GridGraph::NodeAt(const Vec3& center)
    {
      auto it = m_index.find(CellKey(center));
      if (it == m_index.end())
      {
        return nullptr;
      }

      return &m_nodes[it->second];
    }

    GridNode* GridGraph::Neighbor(const GridNode& n, GridDir d)
    {
      Vec3 offset;
      switch (d)
      {
        case GridDir::Xm: offset = Vec3(-n.size.x, 0.0f, 0.0f); break;
        case GridDir::Xp: offset = Vec3(n.size.x, 0.0f, 0.0f); break;
        case GridDir::Zm: offset = Vec3(0.0f, 0.0f, -n.size.z); break;
        default: offset = Vec3(0.0f, 0.0f, n.size.z); break;
      }

      auto it = m_index.find(CellKey(n.center + offset));
      if (it == m_index.end())
      {
        return nullptr;
      }

      return &m_nodes[it->second];
    }

    bool GridGraph::Connected(const GridNode& a, const GridNode& b) const
    {
      // The bridge rule: a passage exists only when both sides flag the facing
      // connection (AND). The direction is read from the spatial delta.
      Vec3 delta = b.center - a.center;
      if (delta.x > 0.0f)
      {
        return a.xp && b.xm;
      }
      if (delta.x < 0.0f)
      {
        return a.xm && b.xp;
      }
      if (delta.z > 0.0f)
      {
        return a.zp && b.zm;
      }

      return a.zm && b.zp;
    }

    GridNode* GridGraph::Mirror(GridNode& n, GridDir d)
    {
      GridNode* nb = Neighbor(n, d);
      if (nb == nullptr)
      {
        return nullptr;
      }

      // The reciprocal flag on the neighbour faces back toward n.
      GridDir recip;
      bool* target = nullptr;
      bool value;
      switch (d)
      {
        case GridDir::Xm: recip = GridDir::Xp; target = &nb->xp; value = n.xm; break;
        case GridDir::Xp: recip = GridDir::Xm; target = &nb->xm; value = n.xp; break;
        case GridDir::Zm: recip = GridDir::Zp; target = &nb->zp; value = n.zm; break;
        default: recip = GridDir::Zm; target = &nb->zm; value = n.zp; break;
      }

      if (*target == value)
      {
        return nb; // Already in sync.
      }

      *target = value;
      WriteFlag(*nb, DirFlagName(recip), value);
      return nb;
    }

  } // namespace Editor
} // namespace ToolKit
