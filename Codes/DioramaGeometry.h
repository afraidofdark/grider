/*
 * Copyright (c) 2019-2026 OtSoftware
 * This code is licensed under the GNU Lesser General Public License v3.0 (LGPL-3.0).
 * For more information, including options for a more permissive commercial license,
 * please visit [otsoftware.tr] or contact us at [info@otsoftare.tr].
 */

#pragma once

/**
 * @file DioramaGeometry.h
 * The shape of a diorama frame around a footprint, as axis aligned boxes.
 *
 * "Make walls that wrap this selection from the outside, as if it stood on a
 * board" is a solid, and a solid made of axis aligned boxes is what this header
 * produces: the plinth the selection stands on (with the table top the wall
 * stands on), the wall rising around the outer edge of that plinth, the corner
 * brackets at every turning point of the outline, and the fill that turns the
 * plinth into a solid block.
 *
 * The shape parameters are in TILE UNITS (see Params), so the same recipe reads
 * the same whatever the tile size of the grid is.
 *
 * It depends on GridFootprint.h only, so the boxes can be produced -- and
 * checked -- without a scene, a mesh or a renderer. The caller turns each box
 * into mesh geometry and gives them materials.
 */

#include "GridFootprint.h"

#include <algorithm>
#include <cmath>
#include <vector>

namespace ToolKit
{
  namespace Editor
  {
    namespace Diorama
    {
      // Where the lattice lives in the world: the center of the cell (0, 0),
      // the lattice step on both axes (the tile size), the height of the tile
      // tops (the floor the frame is built around) and the tile thickness (the
      // level the fill under the footprint starts at).
      struct Layout
      {
        float originX    = 0.0f;
        float originZ    = 0.0f;
        float tileX      = 1.0f;
        float tileZ      = 1.0f;
        float floorY     = 0.0f;
        float tileHeight = 0.0f;

        // World size of one TILE for the shape parameters (see Params). A grid
        // footprint leaves it 0: there the lattice step IS the tile. A
        // rasterized footprint uses a finer lattice than the tile grid (a cell
        // boundary is only as precise as the cell, and tile sized cells would
        // leave up to half a tile between the content and its wall), so it sets
        // this to the tile size of the tool and the walls keep their
        // proportions.
        float tileUnit = 0.0f;
      };

      // Shape of the frame, in TILE UNITS: every value is a multiple of the
      // tile size of the lattice the footprint lives on, so the same recipe
      // reads the same on a 1 unit test grid and on a 5 unit game grid. A wall
      // of 0.6 tiles is a wall on both. BuildParts converts them to world units
      // once, through UnitOfLayout.
      struct Params
      {
        // How far the plinth reaches outward from the selection edge: the table
        // top the walls stand on. The default is the wall thickness, which puts
        // the wall straight on the edge of the selection with no ring around
        // it; a larger value leaves a visible table top between the two.
        float plinthMargin = 0.16f;
        // How deep the plinth hangs below the floor.
        float plinthDepth = 0.5f;
        // The wall: the raised band that wraps the selection from outside.
        float wallHeight = 0.6f;
        float wallThickness = 0.16f;
        // Corner brackets, placed at every turning point of the outline.
        float cornerSize = 0.26f;     // Span of the bracket across the corner.
        float cornerRise = 0.14f;     // How far it rises above the wall.
        float cornerOverhang = 0.03f; // How far it sticks out of the plinth.
        // Fills the volume under the footprint (holes included) so the base is
        // a solid block instead of a hollow shell.
        bool solid = true;
      };

      // World size of one tile unit of a lattice. The shape parameters above are
      // multiples of it; a grid footprint contributes its own tile size, and a
      // lattice with unequal steps averages them (the wall is one band around
      // both axes).
      inline float UnitOfLayout(const Layout& layout)
      {
        if (layout.tileUnit > 0.0f)
        {
          return layout.tileUnit;
        }

        return std::max(0.5f * (layout.tileX + layout.tileZ), 0.0001f);
      }

      // An axis aligned box in world space.
      struct Box
      {
        float minX = 0.0f, minY = 0.0f, minZ = 0.0f;
        float maxX = 0.0f, maxY = 0.0f, maxZ = 0.0f;
      };

      // The box sets a diorama is made of. They are kept apart because each one
      // gets its own material: the plinth, the wall and its brackets.
      struct Parts
      {
        // Plinth boxes (see bandCount for where the fill starts). The fill goes
        // to the same material, so the two share one list.
        std::vector<Box> base;
        std::vector<Box> frame;
        std::vector<Box> brackets;

        // The first `bandCount` boxes of `base` are the band around the
        // selection; the rest is the fill under it. Tooling that wants to tell
        // the two apart (a check, a different material) does not have to
        // re-derive the split.
        size_t bandCount = 0;
      };

      // World position of a lattice corner.
      inline float CornerWorldX(const Layout& layout, const Corner& c)
      {
        return layout.originX + 0.5f * layout.tileX * static_cast<float>(c.x);
      }

      inline float CornerWorldZ(const Layout& layout, const Corner& c)
      {
        return layout.originZ + 0.5f * layout.tileZ * static_cast<float>(c.z);
      }

      // World space XZ rectangle of a corner block: the offsets along BOTH
      // outward normals of the two runs meeting at the corner, each in
      // [inner, outer]. It is the miter square of the frame, and the reason a
      // strip per run is not enough -- at a convex corner the two strips meet
      // only along a line and this block fills the notch between them.
      inline Box CornerBlock(const Corner& corner,
                             const Dir& first,
                             const Dir& second,
                             const Layout& layout,
                             float inner,
                             float outer)
      {
        const float cx = CornerWorldX(layout, corner);
        const float cz = CornerWorldZ(layout, corner);
        const float offsets[2] {inner, outer};

        Box box;
        bool started = false;

        for (int i = 0; i < 2; ++i)
        {
          for (int j = 0; j < 2; ++j)
          {
            const float x = cx + first.x * offsets[i] + second.x * offsets[j];
            const float z = cz + first.z * offsets[i] + second.z * offsets[j];

            if (!started)
            {
              box.minX = box.maxX = x;
              box.minZ = box.maxZ = z;
              started = true;
              continue;
            }

            box.minX = std::min(box.minX, x);
            box.maxX = std::max(box.maxX, x);
            box.minZ = std::min(box.minZ, z);
            box.maxZ = std::max(box.maxZ, z);
          }
        }

        return box;
      }

      // Splits a box around an XZ rectangle: the parts of the box outside the
      // rectangle, as up to four boxes (left, right and the two end slabs of
      // the middle column). The height of the box is untouched, so a piece
      // that is cut keeps its full vertical extent.
      inline void SubtractRectXZ(const Box& box,
                                 float minX,
                                 float maxX,
                                 float minZ,
                                 float maxZ,
                                 std::vector<Box>& out)
      {
        if (maxX <= box.minX || minX >= box.maxX || maxZ <= box.minZ || minZ >= box.maxZ)
        {
          out.push_back(box); // No overlap.
          return;
        }

        if (minX > box.minX)
        {
          Box left = box;
          left.maxX = minX;
          out.push_back(left);
        }
        if (maxX < box.maxX)
        {
          Box right = box;
          right.minX = maxX;
          out.push_back(right);
        }

        const float midMinX = std::max(box.minX, minX);
        const float midMaxX = std::min(box.maxX, maxX);

        if (minZ > box.minZ)
        {
          Box front = box;
          front.minX = midMinX;
          front.maxX = midMaxX;
          front.maxZ = minZ;
          out.push_back(front);
        }
        if (maxZ < box.maxZ)
        {
          Box back = box;
          back.minX = midMinX;
          back.maxX = midMaxX;
          back.minZ = maxZ;
          out.push_back(back);
        }
      }

      // A strip of material along a run: the segment itself, pushed outward
      // from the boundary line into the offset band [inner, outer]. The run's
      // own length is not extended; the ends are closed by the corner blocks.
      inline Box RunStrip(const Run& run, const Layout& layout, float inner, float outer)
      {
        const Dir outward = RunOutwardDir(run);

        const float fromX = CornerWorldX(layout, run.from);
        const float fromZ = CornerWorldZ(layout, run.from);
        const float toX   = CornerWorldX(layout, run.to);
        const float toZ   = CornerWorldZ(layout, run.to);

        Box box;
        box.minX = std::min(fromX, toX);
        box.maxX = std::max(fromX, toX);
        box.minZ = std::min(fromZ, toZ);
        box.maxZ = std::max(fromZ, toZ);

        if (outward.x > 0)
        {
          box.maxX += outer;
          box.minX += inner;
        }
        else if (outward.x < 0)
        {
          box.minX -= outer;
          box.maxX -= inner;
        }
        else if (outward.z > 0)
        {
          box.maxZ += outer;
          box.minZ += inner;
        }
        else
        {
          box.minZ -= outer;
          box.maxZ -= inner;
        }

        return box;
      }

      // The box a single cell of the fill occupies: the tile footprint, from
      // the bottom of the tiles down to the bottom of the base.
      inline Box CellFillBox(const Cell& cell, const Layout& layout, float top, float bottom)
      {
        const float cx = layout.originX + layout.tileX * static_cast<float>(cell.x);
        const float cz = layout.originZ + layout.tileZ * static_cast<float>(cell.z);

        Box box;
        box.minX = cx - 0.5f * layout.tileX;
        box.maxX = cx + 0.5f * layout.tileX;
        box.minZ = cz - 0.5f * layout.tileZ;
        box.maxZ = cz + 0.5f * layout.tileZ;
        box.minY = bottom;
        box.maxY = top;
        return box;
      }

      // The box a run of cells along X occupies: `count` neighbouring cells
      // starting at `first`, from the top of the fill down to its bottom.
      inline Box CellRunFillBox(const Cell& first, int count, const Layout& layout, float top, float bottom)
      {
        Box box = CellFillBox(first, layout, top, bottom);
        box.maxX += layout.tileX * static_cast<float>(count - 1);
        return box;
      }

      // Removes the footprint from a box set: no material of the frame may sit
      // over a tile. It matters where two cells meet only at a corner (a
      // diagonal pinch): the corner block of one cell reaches into the other,
      // and clipping it there keeps the frame outside the selection. The cut
      // is made on the XZ footprint, so a clipped box keeps its full height.
      inline void ClipToFootprint(std::vector<Box>& boxes, const std::vector<Cell>& cells, const Layout& layout)
      {
        if (boxes.empty() || cells.empty())
        {
          return;
        }

        std::vector<Box> clipped;
        for (const Box& box : boxes)
        {
          std::vector<Box> pieces {box};
          for (const Cell& cell : cells)
          {
            const Box tile = CellFillBox(cell, layout, 0.0f, 0.0f);

            std::vector<Box> next;
            for (const Box& piece : pieces)
            {
              SubtractRectXZ(piece, tile.minX, tile.maxX, tile.minZ, tile.maxZ, next);
            }
            pieces.swap(next);

            if (pieces.empty())
            {
              break;
            }
          }

          clipped.insert(clipped.end(), pieces.begin(), pieces.end());
        }

        boxes.swap(clipped);
      }

      // Marks the cells a world space box covers on a lattice. This is how a
      // selection that is not made of tiles (a building, a car, a whole block
      // dragged over with the mouse) gets a footprint: what the selected
      // geometry covers, seen from above.
      //
      // A cell is marked when the box covers its CENTER. Marking every cell the
      // box merely touches would push the outline a whole cell outward, which
      // reads as a gap between the content and the wall of the diorama built
      // around it; the center rule keeps that error at half a cell, inward or
      // outward. A box thinner than a lattice step still marks the cell it sits
      // on.
      inline void RasterizeBounds(float minX,
                                  float minZ,
                                  float maxX,
                                  float maxZ,
                                  const Layout& layout,
                                  std::vector<Cell>& out)
      {
        if (layout.tileX <= 0.0f || layout.tileZ <= 0.0f)
        {
          return;
        }

        const float eps = 0.0001f;
        int firstX      = (int) std::ceil((minX - layout.originX) / layout.tileX - eps);
        int lastX       = (int) std::floor((maxX - layout.originX) / layout.tileX + eps);
        int firstZ      = (int) std::ceil((minZ - layout.originZ) / layout.tileZ - eps);
        int lastZ       = (int) std::floor((maxZ - layout.originZ) / layout.tileZ + eps);

        if (lastX < firstX) // Thinner than a lattice step: the cell it sits on.
        {
          firstX = lastX = (int) std::lround(((minX + maxX) * 0.5f - layout.originX) / layout.tileX);
        }
        if (lastZ < firstZ)
        {
          firstZ = lastZ = (int) std::lround(((minZ + maxZ) * 0.5f - layout.originZ) / layout.tileZ);
        }

        for (int z = firstZ; z <= lastZ; ++z)
        {
          for (int x = firstX; x <= lastX; ++x)
          {
            out.push_back({x, z});
          }
        }
      }

      // Builds the whole frame of a footprint.
      //
      // The plinth is a band of `plinthMargin` around the selection, the wall is
      // its outermost `wallThickness` raised above the floor, and every turning
      // point of the outline gets a bracket spanning the full height of the
      // corner. Solid fill (holes included) is what makes it a block rather
      // than a shell.
      inline void BuildParts(const std::vector<Cell>& cells, const Layout& layout, const Params& params, Parts& out)
      {
        out.base.clear();
        out.frame.clear();
        out.brackets.clear();
        out.bandCount = 0;

        // The recipe is written in tile units; the geometry is built in world
        // units, so this is the one place the two meet.
        const float unit = UnitOfLayout(layout);

        const float margin = std::max(params.plinthMargin, 0.0f) * unit;
        if (margin <= 0.0f || cells.empty())
        {
          return;
        }

        // The wall can never reach inside the selection: it lives in the outer
        // part of the plinth band.
        const float wall       = std::min(std::max(params.wallThickness, 0.0f) * unit, margin);
        const float baseBottom = layout.floorY - std::max(params.plinthDepth, 0.0f) * unit;
        const float wallTop    = layout.floorY + std::max(params.wallHeight, 0.0f) * unit;

        std::vector<Loop> loops;
        TraceBoundary(cells, loops);

        for (const Loop& loop : loops)
        {
          for (const Run& run : loop.runs)
          {
            Box band = RunStrip(run, layout, 0.0f, margin);
            band.minY = baseBottom;
            band.maxY = layout.floorY;
            out.base.push_back(band);

            if (wall > 0.0f)
            {
              Box wallBox = RunStrip(run, layout, margin - wall, margin);
              wallBox.minY = layout.floorY;
              wallBox.maxY = wallTop;
              out.frame.push_back(wallBox);
            }
          }

          // Turning points: the corner block of the plinth, the corner block of
          // the wall, and the bracket that covers both. The bracket never
          // reaches inside the selection either: it stops at the boundary.
          const float bracketSize = std::min(std::max(params.cornerSize * unit, wall), margin);
          const float overhang    = std::max(params.cornerOverhang, 0.0f) * unit;
          const float rise        = std::max(params.cornerRise, 0.0f) * unit;

          for (size_t i = 0; i < loop.runs.size(); ++i)
          {
            const Run& previous = loop.runs[(i + loop.runs.size() - 1) % loop.runs.size()];
            const Run& next     = loop.runs[i];
            const Dir first     = RunOutwardDir(previous);
            const Dir second    = RunOutwardDir(next);

            Box corner = CornerBlock(next.from, first, second, layout, 0.0f, margin);
            corner.minY = baseBottom;
            corner.maxY = layout.floorY;
            out.base.push_back(corner);

            if (wall > 0.0f)
            {
              Box wallCorner = CornerBlock(next.from, first, second, layout, margin - wall, margin);
              wallCorner.minY = layout.floorY;
              wallCorner.maxY = wallTop;
              out.frame.push_back(wallCorner);
            }

            // A bracket with no size and no overhang would be a degenerate
            // sliver: the two corner brackets of a zero size parameter are
            // simply not built.
            if (bracketSize > 0.0f || overhang > 0.0f)
            {
              Box bracket = CornerBlock(next.from, first, second, layout, margin - bracketSize, margin + overhang);
              bracket.minY = baseBottom;
              bracket.maxY = wallTop + rise;
              out.brackets.push_back(bracket);
            }
          }
        }

        // The band, the lip and the brackets are outside the selection by
        // construction; clipping against the footprint keeps that true where
        // the outline doubles back on itself (see ClipToFootprint). The fill
        // is added afterwards, because it is the one part that DOES sit under
        // the tiles.
        ClipToFootprint(out.base, cells, layout);
        ClipToFootprint(out.frame, cells, layout);
        ClipToFootprint(out.brackets, cells, layout);
        out.bandCount = out.base.size();

        if (params.solid)
        {
          std::vector<Cell> fill;
          FillRegion(cells, fill);

          // Start the fill at the bottom of the tiles: the tiles themselves
          // occupy the space above it, so the block is solid without doubling
          // up on the tiles.
          const float fillTop = std::max(layout.floorY - std::max(layout.tileHeight, 0.0f), baseBottom);

          if (fillTop > baseBottom)
          {
            // Runs of neighbouring cells along X become one box: a solid
            // footprint is then a few boxes per row instead of one per cell,
            // which keeps the mesh small and takes the seams out of it (a fine
            // footprint can easily be thousands of cells).
            std::sort(fill.begin(),
                      fill.end(),
                      [](const Cell& a, const Cell& b)
                      {
                        return a.z != b.z ? a.z < b.z : a.x < b.x;
                      });

            for (size_t i = 0; i < fill.size();)
            {
              int run = 1;
              while (i + size_t(run) < fill.size() && fill[i + run].z == fill[i].z &&
                     fill[i + run].x == fill[i].x + run)
              {
                ++run;
              }

              out.base.push_back(CellRunFillBox(fill[i], run, layout, fillTop, baseBottom));
              i += size_t(run);
            }
          }
        }
      }

    } // namespace Diorama
  }   // namespace Editor
} // namespace ToolKit
