/*
 * Copyright (c) 2019-2026 OtSoftware
 * This code is licensed under the GNU Lesser General Public License v3.0 (LGPL-3.0).
 * For more information, including options for a more permissive commercial license,
 * please visit [otsoftware.tr] or contact us at [info@otsoftare.tr].
 */

#pragma once

/**
 * @file GridFootprint.h
 * Boundary tracing of a set of grid cells.
 *
 * The tooling that wraps a selection (the diorama frame) needs the OUTER
 * CONTOUR of the selected cells, not the cells themselves: the contour is what
 * gets extruded, and its turning points are where the corner brackets go. This
 * header holds that geometry as pure data and pure functions: no engine types,
 * no scene, no rendering. It is deliberately header only so the tracing can be
 * reasoned about (and tested) on its own.
 *
 * Conventions:
 *  - A cell is addressed by its lattice indices (x, z), the same pair the grid
 *    graph assigns to a tile.
 *  - A corner of the lattice is stored in HALF cell units: the corners of the
 *    cell (x, z) are (2x +/- 1, 2z +/- 1). Everything stays integral, so loops
 *    chain by exact key lookup instead of by float comparison.
 *  - The 2D plane is (u, v) = (x, z). Loops are traced with the footprint on
 *    the LEFT of the walk, which makes the material side of a run the RIGHT
 *    side (see RunOutwardDir). Outer borders come out counter clockwise and
 *    holes clockwise; both are consumed the same way, because only the
 *    material side matters to the caller.
 */

#include <algorithm>
#include <map>
#include <set>
#include <utility>
#include <vector>

namespace ToolKit
{
  namespace Editor
  {
    namespace Diorama
    {
      // A grid cell on the lattice (col along X, row along Z).
      struct Cell
      {
        int x = 0;
        int z = 0;
      };

      // A lattice corner in half cell units.
      struct Corner
      {
        int x = 0;
        int z = 0;
      };

      // A signed lattice axis: one component is +/-1, the other is 0.
      struct Dir
      {
        int x = 0;
        int z = 0;
      };

      // A straight run of a boundary: the axis aligned segment from `from` to
      // `to`. Collinear boundary edges are merged, so a run is a full side of
      // the outline and the runs of a loop meet exactly at its turning points.
      struct Run
      {
        Corner from;
        Corner to;
      };

      // One closed loop of a footprint boundary: the outer border or a hole.
      struct Loop
      {
        std::vector<Run> runs;
      };

      // Direction a run is walked in.
      inline Dir RunDir(const Run& run)
      {
        Dir d;
        d.x = (run.to.x > run.from.x) - (run.to.x < run.from.x);
        d.z = (run.to.z > run.from.z) - (run.to.z < run.from.z);
        return d;
      }

      // Outward normal of a run: the side the material of a frame built around
      // the footprint goes to. Loops are traced with the footprint on the left,
      // so the material side is the right of the walk: rotating (dx, dz) by -90
      // degrees in the (x, z) plane gives (dz, -dx).
      inline Dir RunOutwardDir(const Run& run)
      {
        const Dir d = RunDir(run);
        Dir n;
        n.x = d.z;
        n.z = -d.x;
        return n;
      }

      // Traces the boundary of a cell set into closed loops. Every side of an
      // occupied cell whose neighbour is not occupied is a boundary edge; the
      // edges are chained into loops. Non convex footprints (L shapes, rings,
      // combs) and holes come out as they are: one loop per border.
      inline void TraceBoundary(const std::vector<Cell>& cells, std::vector<Loop>& loops)
      {
        loops.clear();

        std::set<std::pair<int, int>> occupied;
        for (const Cell& c : cells)
        {
          occupied.insert({c.x, c.z});
        }

        // Boundary edges, keyed by their starting corner. A corner is shared by
        // the edges of up to four cells; the ones that meet there but belong to
        // different borders (two cells touching only at a corner) are chained
        // arbitrarily, because the set of edges -- and therefore the geometry
        // built from them -- is the same whichever way they pair up.
        struct Edge
        {
          Corner to;
          bool used = false;
        };
        std::map<std::pair<int, int>, std::vector<Edge>> outgoing;

        auto isOccupied = [&occupied](int x, int z) -> bool
        {
          return occupied.find({x, z}) != occupied.end();
        };

        size_t edgeCount = 0;
        for (const Cell& c : cells)
        {
          const int x0 = 2 * c.x;
          const int z0 = 2 * c.z;

          // Side without a neighbour -> boundary edge, walked so the footprint
          // (the cell itself) stays on the left.
          if (!isOccupied(c.x, c.z - 1)) // Z- side: walk +X.
          {
            outgoing[{x0 - 1, z0 - 1}].push_back({{x0 + 1, z0 - 1}, false});
            ++edgeCount;
          }
          if (!isOccupied(c.x + 1, c.z)) // X+ side: walk +Z.
          {
            outgoing[{x0 + 1, z0 - 1}].push_back({{x0 + 1, z0 + 1}, false});
            ++edgeCount;
          }
          if (!isOccupied(c.x, c.z + 1)) // Z+ side: walk -X.
          {
            outgoing[{x0 + 1, z0 + 1}].push_back({{x0 - 1, z0 + 1}, false});
            ++edgeCount;
          }
          if (!isOccupied(c.x - 1, c.z)) // X- side: walk -Z.
          {
            outgoing[{x0 - 1, z0 + 1}].push_back({{x0 - 1, z0 - 1}, false});
            ++edgeCount;
          }
        }

        // Walk the edges into loops. Nothing is inserted into `outgoing` while
        // chaining, so the references taken here stay valid.
        for (auto& entry : outgoing)
        {
          for (Edge& edge : entry.second)
          {
            if (edge.used)
            {
              continue;
            }

            const Corner start {entry.first.first, entry.first.second};
            Corner current  = start;
            Corner previous = start;
            Loop loop;
            bool closed = false;

            for (size_t step = 0; step <= edgeCount; ++step)
            {
              auto found = outgoing.find({current.x, current.z});
              if (found == outgoing.end())
              {
                break; // Broken chain: the edges cannot form a loop.
              }

              // Prefer the straight continuation, so two borders touching at a
              // corner do not get braided into runs that turn for no reason.
              const Dir incoming {(current.x > previous.x) - (current.x < previous.x),
                                  (current.z > previous.z) - (current.z < previous.z)};
              Edge* pick = nullptr;
              for (Edge& candidate : found->second)
              {
                if (candidate.used)
                {
                  continue;
                }

                const Dir dir {(candidate.to.x > current.x) - (candidate.to.x < current.x),
                               (candidate.to.z > current.z) - (candidate.to.z < current.z)};
                if (pick == nullptr)
                {
                  pick = &candidate;
                }
                if (dir.x == incoming.x && dir.z == incoming.z && (incoming.x != 0 || incoming.z != 0))
                {
                  pick = &candidate;
                  break;
                }
              }

              if (pick == nullptr)
              {
                break; // Every edge leaving this corner is used.
              }

              pick->used        = true;
              const Corner next = pick->to;

              // Merge collinear edges: a run grows while the walk keeps its
              // direction and a new run starts at every turn.
              if (!loop.runs.empty() && loop.runs.back().to.x == current.x && loop.runs.back().to.z == current.z)
              {
                Run& last  = loop.runs.back();
                const Dir d = RunDir(last);
                const Dir n {(next.x > current.x) - (next.x < current.x),
                             (next.z > current.z) - (next.z < current.z)};
                if (d.x == n.x && d.z == n.z)
                {
                  last.to = next;
                }
                else
                {
                  loop.runs.push_back({current, next});
                }
              }
              else
              {
                loop.runs.push_back({current, next});
              }

              previous = current;
              current  = next;

              if (current.x == start.x && current.z == start.z)
              {
                closed = true;
                break;
              }
            }

            // A loop of a cell boundary cannot be shorter than four runs; a
            // shorter one means the chain broke and is dropped.
            if (closed && loop.runs.size() >= 4)
            {
              loops.push_back(loop);
            }
          }
        }
      }

      // Grows a footprint by `radius` cells and shrinks it back (a
      // morphological closing). Gaps and notches narrower than 2 * radius are
      // filled in, which is what keeps a selection whose pieces stand close
      // together one diorama instead of a wall around every piece, and what
      // takes the stair steps out of an outline that was rasterized from
      // bounding boxes. The outer boundary of the footprint does not move.
      //
      // It runs on a dense grid of the footprint's bounding box, so the cost is
      // linear in the cells (plus the kernel) rather than quadratic.
      inline void CloseFootprint(const std::vector<Cell>& cells, int radius, std::vector<Cell>& out)
      {
        out.clear();
        if (radius <= 0 || cells.empty())
        {
          out = cells;
          return;
        }

        int minX = cells[0].x, maxX = cells[0].x, minZ = cells[0].z, maxZ = cells[0].z;
        for (const Cell& c : cells)
        {
          minX = std::min(minX, c.x);
          maxX = std::max(maxX, c.x);
          minZ = std::min(minZ, c.z);
          maxZ = std::max(maxZ, c.z);
        }

        const int pad = radius + 1;
        const int sizeX = maxX - minX + 2 * pad + 1;
        const int sizeZ = maxZ - minZ + 2 * pad + 1;

        std::vector<char> source(size_t(sizeX) * sizeZ, 0);
        auto index = [&](int x, int z) -> size_t
        {
          return size_t(x - minX + pad) + size_t(z - minZ + pad) * size_t(sizeX);
        };

        for (const Cell& c : cells)
        {
          source[index(c.x, c.z)] = 1;
        }

        // Dilate: a cell belongs to the grown set when any cell of its
        // neighbourhood does.
        std::vector<char> grown(size_t(sizeX) * sizeZ, 0);
        for (int z = 0; z < sizeZ; ++z)
        {
          for (int x = 0; x < sizeX; ++x)
          {
            if (!source[size_t(x) + size_t(z) * size_t(sizeX)])
            {
              continue;
            }

            for (int dz = -radius; dz <= radius; ++dz)
            {
              for (int dx = -radius; dx <= radius; ++dx)
              {
                const int nx = x + dx, nz = z + dz;
                if (nx < 0 || nx >= sizeX || nz < 0 || nz >= sizeZ)
                {
                  continue;
                }
                grown[size_t(nx) + size_t(nz) * size_t(sizeX)] = 1;
              }
            }
          }
        }

        // Erode: a cell survives when its whole neighbourhood is grown.
        for (int z = 0; z < sizeZ; ++z)
        {
          for (int x = 0; x < sizeX; ++x)
          {
            if (!grown[size_t(x) + size_t(z) * size_t(sizeX)])
            {
              continue;
            }

            bool full = true;
            for (int dz = -radius; dz <= radius && full; ++dz)
            {
              for (int dx = -radius; dx <= radius; ++dx)
              {
                const int nx = x + dx, nz = z + dz;
                if (nx < 0 || nx >= sizeX || nz < 0 || nz >= sizeZ ||
                    !grown[size_t(nx) + size_t(nz) * size_t(sizeX)])
                {
                  full = false;
                  break;
                }
              }
            }

            if (full)
            {
              out.push_back({x + minX - pad, z + minZ - pad});
            }
          }
        }
      }

      // Every cell the base of a diorama has to fill: the cells of the
      // footprint plus the empty cells they enclose (holes). A hole left
      // unfilled would be a pit straight through the base, while filling it
      // gives the recessed floor the tiles around it imply.
      //
      // The enclosed cells are found by flooding the empty cells of the
      // bounding box from its border: whatever the flood cannot reach is
      // surrounded by the footprint.
      inline void FillRegion(const std::vector<Cell>& cells, std::vector<Cell>& out)
      {
        out = cells;

        if (cells.empty())
        {
          return;
        }

        int minX = cells[0].x, maxX = cells[0].x, minZ = cells[0].z, maxZ = cells[0].z;
        for (const Cell& c : cells)
        {
          minX = std::min(minX, c.x);
          maxX = std::max(maxX, c.x);
          minZ = std::min(minZ, c.z);
          maxZ = std::max(maxZ, c.z);
        }

        std::set<std::pair<int, int>> occupied;
        for (const Cell& c : cells)
        {
          occupied.insert({c.x, c.z});
        }

        // Flood the empty cells of the (padded) bounding box from outside.
        std::set<std::pair<int, int>> outside;
        std::vector<Cell> stack {{minX - 1, minZ - 1}};
        outside.insert({minX - 1, minZ - 1});

        while (!stack.empty())
        {
          const Cell c = stack.back();
          stack.pop_back();

          const Cell neighbors[4] {{c.x - 1, c.z}, {c.x + 1, c.z}, {c.x, c.z - 1}, {c.x, c.z + 1}};
          for (const Cell& n : neighbors)
          {
            if (n.x < minX - 1 || n.x > maxX + 1 || n.z < minZ - 1 || n.z > maxZ + 1)
            {
              continue;
            }
            if (occupied.find({n.x, n.z}) != occupied.end() || outside.find({n.x, n.z}) != outside.end())
            {
              continue;
            }

            outside.insert({n.x, n.z});
            stack.push_back(n);
          }
        }

        // Unreached empty cells are holes. Ordered by row then column so the
        // generated geometry does not depend on the selection order.
        for (int z = minZ; z <= maxZ; ++z)
        {
          for (int x = minX; x <= maxX; ++x)
          {
            if (occupied.find({x, z}) != occupied.end() || outside.find({x, z}) != outside.end())
            {
              continue;
            }

            out.push_back({x, z});
          }
        }
      }

    } // namespace Diorama
  }   // namespace Editor
} // namespace ToolKit
