/*
 * Copyright (c) 2019-2026 OtSoftware
 * This code is licensed under the GNU Lesser General Public License v3.0 (LGPL-3.0).
 * For more information, including options for a more permissive commercial license,
 * please visit [otsoftware.tr] or contact us at [info@otsoftare.tr].
 */

#pragma once

#include "GridGraph.h"

#include <Plugin.h>
#include <ToolKit.h>

#include <Editor/UI/Window.h>

#include <unordered_map>

namespace ToolKit
{
  namespace Editor
  {

    // No TK_EDITOR_API here: the plugin consumes the editor headers with
    // TK_EDITOR_API == dllimport, but this class is *defined* in the plugin.
    // Marking it dllimport makes MSVC reject the static Class member definition
    // (C2491) and warn about inconsistent linkage on every method (C4273).
    class GridEditor : public Window
    {
     public:
      TKDeclareClass(GridEditor, Window);

      GridEditor();
      ~GridEditor();

      void Show() override;

      // Persists the current settings (TKParams) to the project config directory.
      void SaveSettings();

      // Restores settings from disk. Called by the plugin once on load.
      void LoadSettings();

     protected:
      // Defines the serializable parameters (called by the object factory on
      // construction). They get saved/loaded automatically via the Object base.
      void ParameterConstructor() override;

     public:
      // Serializable settings. TileSize (D) is the width and depth of a tile,
      // its height is fixed at g_tileHeight. GridCols (N) x GridRows (M) is the
      // tile repeat count on the horizontal / vertical axes.
      TKDeclareParam(float, TileSize);
      TKDeclareParam(int, GridCols);
      TKDeclareParam(int, GridRows);
      // Resource-relative path of the asset in the placement DropZone. Stored
      // relative (not absolute) so the setting survives workspace moves.
      TKDeclareParam(String, PlacementPath);
      // Selected compass direction of the placement tool (persisted).
      TKDeclareParam(int, PlacementDir);

     public:
      // Called every frame by the plugin. Rebuilds the connection bridges
      // (bridge quads under each GridNode's BridgeNode) whenever the tiles'
      // custom data state changes.
      void UpdateBridges();

     private:
      // Returns (creating and persisting on first use) an unlit color material
      // under the project resources with the given file name. Idempotent: a
      // material already saved with the project is reused instead of re-created.
      MaterialPtr GetOrCreateUnlitColorMaterial(const String& fileName, const Vec3& color);

      // Returns the checker material used by the grid. dark selects between the
      // light/dark gray checker pair.
      MaterialPtr GetOrCreateCheckerMaterial(bool dark);

      // Rebuilds the bridge quads of a GridNode from the node connections
      // modelled by GridGraph (X-/X+/Z-/Z+). Existing bridges are cleared.
      void RebuildBridges(EntityPtr gridNode);

      // Returns a stable signature of a GridNode's tile connection state. Used
      // to detect changes (connection toggles, tile add/remove).
      String ComputeGridSignature(EntityPtr gridNode) const;

      // ---- Placement tool ---------------------------------------------------

      // Direction the placed object's -Z (front) faces. Mirrors the grid's axis
      // convention (Left=-X, Right=+X, Front=-Z, Back=+Z).
      enum PlacementDir
      {
        PlacementDirXp = 0, // +X (right)
        PlacementDirXm,     // -X (left)
        PlacementDirZp,     // +Z (back)
        PlacementDirZm      // -Z (front)
      };

      // Yaw in degrees about +Y that makes an object's -Z face the given
      // direction. Used to orient a placement before it snaps onto a tile.
      float PlacementYaw(int dir) const;

      // Instantiates the dropped asset (mesh / skinMesh / scene prefab) into the
      // scene, ready to be positioned. Returns null when the asset can't be
      // loaded or isn't a supported drop type.
      EntityPtr InstantiatePlacement(const EditorScenePtr& scene, const String& fullPath, const String& ext);

      // Drops the instantiated asset onto the currently selected tile: centered
      // on it, resting on its top surface, facing the selected direction, and
      // parented under the tile so it follows the tile and can be re-aligned.
      void PlaceObjectOnSelectedTile();

      // Center of the tile's top surface in world space. Same math as
      // RebuildBridges (local AABB offset by the world translation).
      Vec3 GetTileTopCenter(const EntityPtr& tile) const;

      // True when the entity is a tile: a direct child of a GridNode (the
      // plugin's BridgeNode master is excluded).
      bool IsTile(const EntityPtr& e) const;

      // True when the entity is an object placed by the tool: parented directly
      // under a tile so it moves and re-aligns with the tile it was dropped on.
      bool IsPlacedObject(const EntityPtr& e) const;

      // Nearest compass direction the object's -Z (front) faces, derived from
      // its world orientation so the compass stays in sync even if the object
      // was rotated by hand in the viewport.
      int FacingDir(const EntityPtr& obj) const;

      // Rotates a placed object so its -Z faces the given direction and
      // re-anchors it on the tile it sits under (centered, base flush with the
      // tile top). Used to adjust a placement's direction after the fact.
      void ReorientPlacedObject(const EntityPtr& obj, int dir);

      // Full (absolute) path / extension / file name of the asset in the
      // placement DropZone. The absolute path is runtime-only; the persisted
      // form is the resource-relative PlacementPath param.
      String m_placementPath;
      String m_placementExt;
      String m_placementName;

      // Snapshot of a single tile's connection flags. Kept per GridNode so the
      // plugin can tell which flag a user edited and mirror the reciprocal flag
      // on the neighbour tile (one checkbox change updates both tiles).
      struct TileFlags
      {
        bool left = false, right = false, front = false, back = false;
      };

      // Cached signatures per GridNode; drives change detection in UpdateBridges.
      std::unordered_map<ObjectId, String> m_bridgeSignatures;

      // Cached per-tile connection flags per GridNode. Compared against the live
      // state in RebuildBridges to find the flag the user changed.
      std::unordered_map<ObjectId, std::map<String, TileFlags>> m_tileFlags;
    };

    typedef std::shared_ptr<GridEditor> GridEditorPtr;

  } // namespace Editor
} // namespace ToolKit
