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

namespace ToolKit
{
  namespace Editor
  {

    class TK_EDITOR_API BricksEditor : public Window
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

     private:
      // Loads the brick mesh resource from path. Empty path clears the mesh.
      void SetBrickMesh(const String& path);

      // Loads the prefab scene and returns its boundary (AABB), used to size and
      // space the grid just like a mesh bounding box.
      BoundingBox GetPrefabBoundary();

      // Runtime loaded mesh, derived from MeshFile. Not serialized directly.
      MeshPtr m_mesh = nullptr;
    };

    typedef std::shared_ptr<BricksEditor> BricksEditorPtr;

  } // namespace Editor
} // namespace ToolKit
