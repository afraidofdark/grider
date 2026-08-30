/*
 * Copyright (c) 2019-2026 OtSoftware
 * This code is licensed under the GNU Lesser General Public License v3.0 (LGPL-3.0).
 * For more information, including options for a more permissive commercial license,
 * please visit [otsoftware.tr] or contact us at [info@otsoftare.tr].
 */

#pragma once

#include <Entity.h>

#include <unordered_map>
#include <vector>

namespace ToolKit
{
  namespace Editor
  {

    // Axis direction a node connects to its neighbour. Mirrors the grid axis
    // convention used by the editor (Left=-X, Right=+X, Front=-Z, Back=+Z).
    enum class GridDir
    {
      Xm, // Left
      Xp, // Right
      Zm, // Front
      Zp  // Back
    };

    // A single grid cell: its lattice index, world position and size, its four
    // connection flags, and the tile entity it is bound to (null for data-only
    // usage). The connection flags read/write the tile's "X-"/"X+"/"Z-"/"Z+"
    // custom data through the graph; they are the edges a future pathfinder
    // would traverse.
    struct GridNode
    {
      int ix = 0, iz = 0; // Lattice indices (col along X, row along Z).
      Vec3 center;        // World position of the node (the tile top-center).
      Vec3 size;          // Tile AABB extent (max - min); neighbour spacing.
      bool xm = false, xp = false, zm = false, zp = false; // Connection flags.
      EntityPtr tile;     // Owning tile entity (null for data-only nodes).
    };

    // Independent, reusable data model of a tile grid. It holds the node set and
    // every node's connection state, and answers the queries traversal needs
    // (node-at-position, neighbour, connectivity). It is also the single place
    // the connection custom-data of the scene tiles is read from / written to,
    // so the editor's bridge rendering and future pathfinding share one source.
    //
    // The class is a plain value type: it owns no resources, binds to the scene
    // only when LoadFromScene is called, and can be built purely from data
    // (nodes filled by hand) for algorithmic use.
    class GridGraph
    {
     public:
      // Custom-data flag name written on the tiles for a direction.
      static const char* DirFlagName(GridDir d);

      // Removes all nodes.
      void Clear();

      // Builds the graph from a GridNode's tile children: resolves each tile to
      // its data entity (legacy prefab tiles carry the flags on an inner "Tile"
      // entity), reads its X-/X+/Z-/Z+ custom data, computes lattice indices and
      // world positions from the tile geometry, and rebuilds the spatial index.
      // Tiles lingering in the hierarchy after deletion are ignored.
      void LoadFromScene(EntityPtr gridNode);

      // Writes every node's connection flags back to its tile's custom data.
      // Tiles are resolved through their data entity, the same way they are read.
      void WriteToScene();

      // Nodes of the graph (mutable / read-only).
      std::vector<GridNode>& Nodes();
      const std::vector<GridNode>& Nodes() const;

      // Node whose world center matches the given position (spatial index), or
      // null when there is no node there.
      GridNode* NodeAt(const Vec3& center);

      // The node adjacent to n in direction d (offset by n.size), or null when
      // there is no node in that direction. Returns a mutable node so callers
      // can edit the neighbour's flags through it (see Mirror).
      GridNode* Neighbor(const GridNode& n, GridDir d);

      // True when both nodes open the side facing each other (the bridge rule:
      // a passage exists only when both sides agree).
      bool Connected(const GridNode& a, const GridNode& b) const;

      // Syncs the reciprocal flag on the neighbour across n's side d: the
      // neighbour's facing side is set to the same value n carries, so turning
      // one side on/off updates both tiles in one action. Returns the neighbour
      // node (which received the flag), or null when there is none.
      GridNode* Mirror(GridNode& n, GridDir d);

     private:
      // Node entity the connection flags live on: prefab tiles carry them on an
      // inner "Tile" entity, auto-generated tiles carry them on themselves.
      static EntityPtr GetDataEntity(EntityPtr tile);

      // Reads / writes a connection flag on the node's data entity custom data.
      // The write creates the entry if the tile does not have it yet.
      bool ReadFlag(const GridNode& n, const char* name) const;
      void WriteFlag(GridNode& n, const char* name, bool value);

      std::vector<GridNode> m_nodes;
      std::unordered_map<String, size_t> m_index; // RoundKey(center) -> node.
    };

  } // namespace Editor
} // namespace ToolKit
