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

      // Rebuilds runtime state (the loaded mesh) after deserialization.
      void ParameterEventConstructor() override;

     public:
      // Serializable settings. GridSize is the NxN brick count; MeshFile is the
      // path of the dropped brick mesh; PrefabFile is the relative path of the
      // dropped brick prefab (.scene). Whichever of MeshFile/PrefabFile is set
      // wins; when both are empty we fall back to unit cubes.
      TKDeclareParam(int, GridSize);
      TKDeclareParam(String, MeshFile);
      TKDeclareParam(String, PrefabFile);

     public:
      // Called every frame by the plugin. Rebuilds the connection bridges
      // (bridge quads under each GridNode's BridgeNode) whenever the tiles'
      // custom data state changes.
      void UpdateBridges();

     private:
      // Loads the brick mesh resource from path. Empty path clears the mesh.
      void SetBrickMesh(const String& path);

      // Loads the prefab scene and returns its boundary (AABB), used to size and
      // space the grid just like a mesh bounding box.
      BoundingBox GetPrefabBoundary();

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

      // Runtime loaded mesh, derived from MeshFile. Not serialized directly.
      MeshPtr m_mesh = nullptr;
    };

    typedef std::shared_ptr<BricksEditor> BricksEditorPtr;

  } // namespace Editor
} // namespace ToolKit
