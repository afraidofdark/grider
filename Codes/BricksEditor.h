/*
 * Copyright (c) 2019-2026 OtSoftware
 * This code is licensed under the GNU Lesser General Public License v3.0 (LGPL-3.0).
 * For more information, including options for a more permissive commercial license,
 * please visit [otsoftware.tr] or contact us at [info@otsoftare.tr].
 */

#pragma once

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
    class BricksEditor : public Window
    {
     public:
      TKDeclareClass(BricksEditor, Window);

      BricksEditor();
      ~BricksEditor();

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

      // Ensures the tile carries the given connection custom data, writing the
      // value. Creates the entry if the tile does not have it yet.
      void SetTileConnection(const EntityPtr& tile, const char* name, bool value);

      // Returns the entity that holds the connection custom data for a grid
      // child. Legacy prefab tiles carry it on an inner "Tile" entity; the
      // auto-generated tiles carry it on themselves.
      EntityPtr GetTileDataEntity(EntityPtr child) const;

      // Rebuilds the bridge quads of a GridNode from its tiles' custom data
      // (LeftCon/RightCon/FrontCon/BackCon). Existing bridges are cleared.
      void RebuildBridges(EntityPtr gridNode);

      // Returns a stable signature of a GridNode's tile connection state. Used
      // to detect changes (connection toggles, tile add/remove).
      String ComputeGridSignature(EntityPtr gridNode) const;

      // Reads a boolean connection flag from the tile's custom data.
      bool ReadTileConnection(const EntityPtr& tile, const char* name) const;

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

    typedef std::shared_ptr<BricksEditor> BricksEditorPtr;

  } // namespace Editor
} // namespace ToolKit
