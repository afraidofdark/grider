/*
 * Copyright (c) 2019-2026 OtSoftware
 * This code is licensed under the GNU Lesser General Public License v3.0 (LGPL-3.0).
 * For more information, including options for a more permissive commercial license,
 * please visit [otsoftware.tr] or contact us at [info@otsoftare.tr].
 */

#pragma once

/**
 * @file DioramaBuilder.h
 * Turns a footprint into a diorama in the scene.
 *
 * The shape itself is pure data (GridFootprint.h / DioramaGeometry.h); this is
 * the scene side of it: it generates the meshes, creates one entity per
 * material (plinth, frame, brackets) under a single master entity, and stores
 * the whole recipe on that master as custom data.
 *
 * WHY THE RECIPE IS STORED: a generated mesh is not a resource file, so a
 * scene that is saved and loaded again comes back with the master entity and
 * empty meshes. The recipe makes the diorama rebuildable from the scene alone,
 * which is how the tool re-creates it after a load (see GridEditor).
 */

#include "DioramaGeometry.h"

#include <Editor/Source/EditorScene.h>

#include <Material.h>
#include <Types.h>

#include <vector>

namespace ToolKit
{
  namespace Editor
  {
    // Everything a diorama is made of: where the lattice is, which cells are
    // wrapped, the shape of the frame and the colors of its parts.
    struct DioramaSpec
    {
      Diorama::Layout layout;
      std::vector<Diorama::Cell> cells;
      Diorama::Params shape;
      Vec3 baseColor {0.20f, 0.15f, 0.12f};
      Vec3 frameColor {0.55f, 0.40f, 0.24f};
      Vec3 bracketColor {0.72f, 0.58f, 0.32f};
    };

    // Name of the master entities the tool creates in the scene.
    const char* DioramaNodeName();

    // True when the entity is a diorama master created by the tool.
    bool IsDioramaNode(EntityPtr entity);

    // Reads / writes the recipe as custom data on the master entity. Reading
    // fails when the entity carries no cells, which is what tells a diorama
    // that still has to be built from one that is complete.
    void WriteDioramaSpec(EntityPtr node, const DioramaSpec& spec);
    bool ReadDioramaSpec(EntityPtr node, DioramaSpec& spec);

    // A stable string of everything the generated geometry depends on. Used to
    // detect that a diorama's recipe (or its parameters) changed.
    String DioramaSignature(const DioramaSpec& spec);

    // Creates or updates the diorama of a footprint and returns its master
    // entity. A diorama that already wraps the same cells (same lattice, same
    // cell set) is reused and rebuilt instead of duplicated, so pressing the
    // build button again refreshes the frame the selection already has.
    // `parent` is the entity the master is hung under (the grid it belongs to);
    // a null parent puts it in the scene root. Returns null when the footprint
    // produces no geometry.
    EntityPtr BuildDiorama(EditorScenePtr scene, EntityPtr parent, const DioramaSpec& spec);

    // Rebuilds the meshes of an existing diorama from its stored recipe.
    // Returns false when the entity carries no readable recipe.
    bool RebuildDiorama(EditorScenePtr scene, EntityPtr node);

    // Removes a diorama and its meshes from the scene.
    void RemoveDiorama(EditorScenePtr scene, EntityPtr node);

    // Returns the project material with the given file name, creating it as an
    // unlit color material on first use. `applyColor` writes the wanted color
    // back into an already existing material, which is what lets a color
    // parameter of the tool retint a diorama that was built before.
    MaterialPtr GetOrCreateColorMaterial(const String& fileName, const Vec3& color, bool applyColor);

  } // namespace Editor
} // namespace ToolKit
