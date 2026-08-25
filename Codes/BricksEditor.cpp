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

namespace ToolKit
{
  namespace Editor
  {

    TKDefineClass(BricksEditor, Window);

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

                if (isPrefab)
                {
                  TK_LOG("Placed %dx%d prefab bricks under GridNode", N, N);
                }
                else if (isMesh)
                {
                  TK_LOG("Placed %dx%d mesh bricks under GridNode", N, N);
                }
                else
                {
                  TK_LOG("Placed %dx%d cubes under GridNode at (%.0f, %.0f)", N, N, originX, originZ);
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
