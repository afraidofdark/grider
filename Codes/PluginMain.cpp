/*
 * Copyright (c) 2019-2026 OtSoftware
 * This code is licensed under the GNU Lesser General Public License v3.0 (LGPL-3.0).
 * For more information, including options for a more permissive commercial license,
 * please visit [otsoftware.tr] or contact us at [info@otsoftare.tr].
 */

#include "PluginMain.h"

ToolKit::Editor::PluginMain Self;

extern "C" TK_PLUGIN_API ToolKit::Plugin* TK_STDCAL GetInstance() { return &Self; }

namespace ToolKit
{
  namespace Editor
  {

    void PluginMain::Init(Main* master)
    {
      Main::SetProxy(master);
      GetObjectFactory()->Register<GridEditor>();
    }

    void PluginMain::Destroy()
    {
      GetObjectFactory()->Unregister<GridEditor>();
    }

    GridEditorPtr g_gridEditor = nullptr;

    void PluginMain::Frame(float deltaTime)
    {
      // Keeps the bridge quads in sync with the tiles' connection custom data.
      if (g_gridEditor)
      {
        g_gridEditor->UpdateBridges();
      }
    }

    void PluginMain::OnLoad(XmlDocumentPtr state)
    {
      g_gridEditor = MakeNewPtr<GridEditor>();
      g_gridEditor->LoadSettings();
      g_gridEditor->AddToUI();
    }

    void PluginMain::OnUnload(XmlDocumentPtr state)
    {
      g_gridEditor->RemoveFromUI();
      g_gridEditor = nullptr;
    }

    void PluginMain::OnPlay() {}

    void PluginMain::OnPause() {}

    void PluginMain::OnResume() {}

    void PluginMain::OnStop() {}

  } // namespace Editor
} // namespace ToolKit
