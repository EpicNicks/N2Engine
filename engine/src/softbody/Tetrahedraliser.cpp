#include "engine/softbody/Tetrahedraliser.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <format>
#include <limits>
#include <map>
#include <utility>

#include <math/DeterministicMath.hpp>

#include "engine/rendering/Mesh.hpp"

namespace N2Engine::SoftBodies
{
    namespace
    {
        using Vec3 = std::array<double, 3>;
        using Corners = std::array<std::uint32_t, 4>;

        // How close render vertices must be, as a share of the bounding box diagonal, to count as one position
        constexpr double kWeldTolerance = 1e-5;
        // A mesh thinner than this share of its longest side has no volume to voxelise
        constexpr double kFlatTolerance = 1e-5;
        // The parity rays are shifted by these shares of a voxel so they never run along an edge shared by two
        // triangles (which a grid-aligned mesh, e.g. a box with diagonal edges on its faces, would otherwise hit)
        constexpr double kRayOffsetU = 0.0137;
        constexpr double kRayOffsetV = 0.0291;
        // A weight this far below 0 still counts as inside a tetrahedron
        constexpr double kInsideTolerance = 1e-5;

        // The 5-tet split of a voxel. Corner bits: x = 1, y = 2, z = 4. Even voxels (i + j + k even) use the first
        // set, odd voxels the mirrored one, so the face diagonals of neighbouring voxels agree.
        constexpr int kSplitEven[5][4] = {{0, 1, 2, 4}, {3, 1, 2, 7}, {5, 1, 4, 7}, {6, 2, 4, 7}, {1, 2, 4, 7}};
        constexpr int kSplitOdd[5][4] = {{1, 0, 3, 5}, {2, 0, 3, 6}, {4, 0, 5, 6}, {7, 3, 5, 6}, {0, 3, 5, 6}};

        Vec3 Sub(const Vec3 &a, const Vec3 &b) { return {a[0] - b[0], a[1] - b[1], a[2] - b[2]}; }
        Vec3 Cross(const Vec3 &a, const Vec3 &b)
        {
            return {a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]};
        }
        double Dot(const Vec3 &a, const Vec3 &b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }

        /// Six times the signed volume of the tetrahedron a, b, c, d (positive for a right-handed corner order)
        double Volume6(const Vec3 &a, const Vec3 &b, const Vec3 &c, const Vec3 &d)
        {
            return Dot(Cross(Sub(b, a), Sub(c, a)), Sub(d, a));
        }

        struct Welded
        {
            std::vector<Vec3> positions;
            std::vector<std::uint32_t> remap; // original vertex -> welded position
        };

        Welded Weld(const std::span<const Renderer::Common::Vertex> vertices, const double tolerance)
        {
            // Positions closer than `tolerance` (on every axis) are one. A grid of cells four tolerances wide finds
            // them: a vertex looks in its own cell and, for each axis, in the neighbour on the side it is within a
            // tolerance of (a plain rounding of the position would split two equal positions that straddle a rounding
            // boundary).
            Welded welded;
            welded.remap.resize(vertices.size());
            const double cellSize = tolerance * 4.0;
            std::map<std::array<std::int64_t, 3>, std::vector<std::uint32_t>> cells;
            for (std::size_t i = 0; i < vertices.size(); ++i)
            {
                const Vec3 p{vertices[i].position[0], vertices[i].position[1], vertices[i].position[2]};
                std::array<std::int64_t, 3> cell{};
                std::array<std::array<std::int64_t, 2>, 3> look{};
                std::array<int, 3> lookCount{};
                for (int a = 0; a < 3; ++a)
                {
                    const double scaled = p[a] / cellSize;
                    const double base = std::floor(scaled);
                    cell[a] = static_cast<std::int64_t>(base);
                    const double fraction = scaled - base;
                    look[a][0] = cell[a];
                    lookCount[a] = 1;
                    if (fraction < 0.25)
                    {
                        look[a][lookCount[a]++] = cell[a] - 1;
                    }
                    else if (fraction > 0.75)
                    {
                        look[a][lookCount[a]++] = cell[a] + 1;
                    }
                }

                std::int64_t found = -1;
                for (int ix = 0; ix < lookCount[0] && found < 0; ++ix)
                {
                    for (int iy = 0; iy < lookCount[1] && found < 0; ++iy)
                    {
                        for (int iz = 0; iz < lookCount[2] && found < 0; ++iz)
                        {
                            const auto it = cells.find(std::array<std::int64_t, 3>{look[0][ix], look[1][iy], look[2][iz]});
                            if (it == cells.end())
                            {
                                continue;
                            }
                            for (const std::uint32_t candidate : it->second)
                            {
                                const Vec3 &q = welded.positions[candidate];
                                if (std::abs(q[0] - p[0]) <= tolerance && std::abs(q[1] - p[1]) <= tolerance &&
                                    std::abs(q[2] - p[2]) <= tolerance)
                                {
                                    found = candidate;
                                    break;
                                }
                            }
                        }
                    }
                }
                if (found < 0)
                {
                    found = static_cast<std::int64_t>(welded.positions.size());
                    welded.positions.push_back(p);
                    cells[cell].push_back(static_cast<std::uint32_t>(found));
                }
                welded.remap[i] = static_cast<std::uint32_t>(found);
            }
            return welded;
        }

        struct Grid
        {
            Vec3 origin{};
            double h = 1.0;
            int n[3] = {1, 1, 1};

            [[nodiscard]] std::size_t CellCount() const
            {
                return static_cast<std::size_t>(n[0]) * static_cast<std::size_t>(n[1]) * static_cast<std::size_t>(n[2]);
            }
            [[nodiscard]] std::size_t Cell(const int i, const int j, const int k) const
            {
                return static_cast<std::size_t>(i) +
                       static_cast<std::size_t>(n[0]) * (static_cast<std::size_t>(j) + static_cast<std::size_t>(n[1]) * k);
            }
            [[nodiscard]] std::size_t Node(const int i, const int j, const int k) const
            {
                return static_cast<std::size_t>(i) +
                       static_cast<std::size_t>(n[0] + 1) *
                           (static_cast<std::size_t>(j) + static_cast<std::size_t>(n[1] + 1) * k);
            }
            [[nodiscard]] int CellCoord(const double p, const int axis) const
            {
                return std::clamp(static_cast<int>(std::floor((p - origin[axis]) / h)), 0, n[axis] - 1);
            }
        };

        Grid MakeGrid(const Vec3 &boxMin, const Vec3 &extent, const double longest, const int resolution)
        {
            Grid grid;
            grid.h = longest / resolution;
            for (int a = 0; a < 3; ++a)
            {
                grid.n[a] = std::max(1, static_cast<int>(std::ceil(extent[a] / grid.h - 1e-6)));
                // Centred on the box, so a symmetric mesh gets a symmetric voxelisation
                grid.origin[a] = boxMin[a] - (grid.n[a] * grid.h - extent[a]) * 0.5;
            }
            return grid;
        }

        /// Which voxels are inside: two of the three axis-parity tests say so, or the voxel holds a welded vertex
        std::vector<std::uint8_t> Voxelise(const Grid &grid, const std::vector<Vec3> &positions,
                                           const std::vector<std::array<std::uint32_t, 3>> &triangles,
                                           const std::vector<std::uint32_t> &usedVertices)
        {
            std::vector<std::uint8_t> votes(grid.CellCount(), 0);

            for (int a = 0; a < 3; ++a)
            {
                const int u = (a + 1) % 3;
                const int v = (a + 2) % 3;
                const double du = kRayOffsetU * grid.h;
                const double dv = kRayOffsetV * grid.h;
                const int nu = grid.n[u];
                const int nv = grid.n[v];
                std::vector<std::vector<double>> columns(static_cast<std::size_t>(nu) * static_cast<std::size_t>(nv));

                for (const auto &triangle : triangles)
                {
                    const Vec3 &p0 = positions[triangle[0]];
                    const Vec3 &p1 = positions[triangle[1]];
                    const Vec3 &p2 = positions[triangle[2]];
                    const double det = (p1[u] - p0[u]) * (p2[v] - p0[v]) - (p2[u] - p0[u]) * (p1[v] - p0[v]);
                    if (std::abs(det) < 1e-18)
                    {
                        continue; // edge-on to the ray (or degenerate): it can't be crossed
                    }
                    const double minU = std::min({p0[u], p1[u], p2[u]});
                    const double maxU = std::max({p0[u], p1[u], p2[u]});
                    const double minV = std::min({p0[v], p1[v], p2[v]});
                    const double maxV = std::max({p0[v], p1[v], p2[v]});
                    const int iMin = std::max(0, static_cast<int>(std::floor((minU - grid.origin[u] - du) / grid.h - 0.5)));
                    const int iMax = std::min(nu - 1, static_cast<int>(std::ceil((maxU - grid.origin[u] - du) / grid.h - 0.5)));
                    const int jMin = std::max(0, static_cast<int>(std::floor((minV - grid.origin[v] - dv) / grid.h - 0.5)));
                    const int jMax = std::min(nv - 1, static_cast<int>(std::ceil((maxV - grid.origin[v] - dv) / grid.h - 0.5)));

                    for (int j = jMin; j <= jMax; ++j)
                    {
                        const double sv = grid.origin[v] + (j + 0.5) * grid.h + dv;
                        for (int i = iMin; i <= iMax; ++i)
                        {
                            const double su = grid.origin[u] + (i + 0.5) * grid.h + du;
                            const double l1 = ((su - p0[u]) * (p2[v] - p0[v]) - (p2[u] - p0[u]) * (sv - p0[v])) / det;
                            const double l2 = ((p1[u] - p0[u]) * (sv - p0[v]) - (su - p0[u]) * (p1[v] - p0[v])) / det;
                            const double l0 = 1.0 - l1 - l2;
                            if (l0 >= 0.0 && l1 >= 0.0 && l2 >= 0.0)
                            {
                                columns[static_cast<std::size_t>(i) + static_cast<std::size_t>(nu) * j].push_back(
                                    l0 * p0[a] + l1 * p1[a] + l2 * p2[a]);
                            }
                        }
                    }
                }

                for (int j = 0; j < nv; ++j)
                {
                    for (int i = 0; i < nu; ++i)
                    {
                        auto &crossings = columns[static_cast<std::size_t>(i) + static_cast<std::size_t>(nu) * j];
                        if (crossings.empty())
                        {
                            continue;
                        }
                        std::ranges::sort(crossings);
                        std::size_t passed = 0;
                        for (int k = 0; k < grid.n[a]; ++k)
                        {
                            const double centre = grid.origin[a] + (k + 0.5) * grid.h;
                            while (passed < crossings.size() && crossings[passed] < centre)
                            {
                                ++passed;
                            }
                            if ((passed & 1u) != 0)
                            {
                                int c[3];
                                c[a] = k;
                                c[u] = i;
                                c[v] = j;
                                ++votes[grid.Cell(c[0], c[1], c[2])];
                            }
                        }
                    }
                }
            }

            std::vector<std::uint8_t> occupied(grid.CellCount(), 0);
            for (std::size_t cell = 0; cell < occupied.size(); ++cell)
            {
                occupied[cell] = votes[cell] >= 2 ? 1 : 0;
            }
            for (const std::uint32_t vertex : usedVertices)
            {
                const Vec3 &p = positions[vertex];
                occupied[grid.Cell(grid.CellCoord(p[0], 0), grid.CellCoord(p[1], 1), grid.CellCoord(p[2], 2))] = 1;
            }
            return occupied;
        }

        std::size_t CountOccupied(const std::vector<std::uint8_t> &occupied)
        {
            return static_cast<std::size_t>(std::ranges::count(occupied, std::uint8_t{1}));
        }

        /// The barycentric weights of `p` in the tetrahedron; false for a degenerate one
        bool Barycentric(const Vec3 &p, const std::array<Vec3, 4> &t, double (&w)[4])
        {
            const double volume = Volume6(t[0], t[1], t[2], t[3]);
            if (!(std::abs(volume) > 1e-300))
            {
                return false;
            }
            w[0] = Volume6(p, t[1], t[2], t[3]) / volume;
            w[1] = Volume6(t[0], p, t[2], t[3]) / volume;
            w[2] = Volume6(t[0], t[1], p, t[3]) / volume;
            w[3] = 1.0 - w[0] - w[1] - w[2];
            return true;
        }
    }

    TetResult Tetrahedralise(const std::span<const Renderer::Common::Vertex> vertices,
                             const std::span<const std::uint32_t> indices, const TetSettings &settings)
    {
        TetResult result;
        if (vertices.empty() || indices.size() < 3 || indices.size() % 3 != 0)
        {
            result.error = "the mesh has no triangles";
            return result;
        }

        // The extent of everything, for the weld tolerance
        Vec3 allMin{std::numeric_limits<double>::max(), std::numeric_limits<double>::max(),
                    std::numeric_limits<double>::max()};
        Vec3 allMax{std::numeric_limits<double>::lowest(), std::numeric_limits<double>::lowest(),
                    std::numeric_limits<double>::lowest()};
        for (const auto &vertex : vertices)
        {
            for (int a = 0; a < 3; ++a)
            {
                if (!std::isfinite(vertex.position[a]))
                {
                    result.error = "the mesh has a vertex position that is not a finite number";
                    return result;
                }
                allMin[a] = std::min(allMin[a], static_cast<double>(vertex.position[a]));
                allMax[a] = std::max(allMax[a], static_cast<double>(vertex.position[a]));
            }
        }
        const Vec3 allExtent = Sub(allMax, allMin);
        const double diagonal = std::sqrt(Dot(allExtent, allExtent));
        if (!(diagonal > 0.0))
        {
            result.error = "the mesh is a single point";
            return result;
        }

        // 1. Weld
        const Welded welded = Weld(vertices, diagonal * kWeldTolerance);
        std::vector<std::array<std::uint32_t, 3>> triangles;
        triangles.reserve(indices.size() / 3);
        for (std::size_t i = 0; i + 2 < indices.size(); i += 3)
        {
            if (indices[i] >= vertices.size() || indices[i + 1] >= vertices.size() || indices[i + 2] >= vertices.size())
            {
                result.error = "the mesh has an index past its last vertex";
                return result;
            }
            const std::array<std::uint32_t, 3> t{welded.remap[indices[i]], welded.remap[indices[i + 1]],
                                                 welded.remap[indices[i + 2]]};
            if (t[0] != t[1] && t[1] != t[2] && t[0] != t[2])
            {
                triangles.push_back(t);
            }
        }
        if (triangles.empty())
        {
            result.error = "the mesh has no triangles (every one collapses to a line or a point)";
            return result;
        }

        std::vector<std::uint32_t> used;
        {
            std::vector<std::uint8_t> seen(welded.positions.size(), 0);
            for (const auto &t : triangles)
            {
                for (const std::uint32_t v : t)
                {
                    if (!seen[v])
                    {
                        seen[v] = 1;
                        used.push_back(v);
                    }
                }
            }
        }
        Vec3 boxMin{std::numeric_limits<double>::max(), std::numeric_limits<double>::max(),
                    std::numeric_limits<double>::max()};
        Vec3 boxMax{std::numeric_limits<double>::lowest(), std::numeric_limits<double>::lowest(),
                    std::numeric_limits<double>::lowest()};
        for (const std::uint32_t v : used)
        {
            for (int a = 0; a < 3; ++a)
            {
                boxMin[a] = std::min(boxMin[a], welded.positions[v][a]);
                boxMax[a] = std::max(boxMax[a], welded.positions[v][a]);
            }
        }
        const Vec3 extent = Sub(boxMax, boxMin);
        const double longest = std::max({extent[0], extent[1], extent[2]});
        const double shortest = std::min({extent[0], extent[1], extent[2]});
        if (!(shortest > longest * kFlatTolerance))
        {
            result.error = "the mesh is flat (no thickness along one axis), so it has no volume to simulate";
            return result;
        }

        // 2. Voxelise, lowering the resolution until the body fits
        int resolution = std::clamp(settings.resolution, Physics::kMinTetResolution, Physics::kMaxTetResolution);
        Grid grid;
        std::vector<std::uint8_t> occupied;
        std::size_t occupiedCount = 0;
        while (true)
        {
            grid = MakeGrid(boxMin, extent, longest, resolution);
            occupied = Voxelise(grid, welded.positions, triangles, used);
            occupiedCount = CountOccupied(occupied);
            if (occupiedCount == 0)
            {
                result.error = "nothing is inside the mesh (is it a closed surface?)";
                return result;
            }
            const std::size_t tetCount = occupiedCount * 5;
            if (tetCount <= settings.maxTets)
            {
                break;
            }
            if (resolution <= Physics::kMinTetResolution)
            {
                result.error = std::format("even at resolution {} the body has {} tetrahedra, above the limit of {}",
                                           resolution, tetCount, settings.maxTets);
                return result;
            }
            const double scale = Math::Exp(Math::Log(static_cast<double>(settings.maxTets) / static_cast<double>(tetCount)) / 3.0);
            resolution = std::clamp(std::min(resolution - 1, static_cast<int>(std::floor(resolution * scale))),
                                    Physics::kMinTetResolution, Physics::kMaxTetResolution);
        }

        // 3. Tetrahedra: nodes in voxel order, 5 tetrahedra per inside voxel
        auto mesh = std::make_shared<Physics::TetMeshData>();
        mesh->resolution = resolution;
        mesh->voxelSize = static_cast<float>(grid.h);
        mesh->weldedVertexCount = static_cast<std::uint32_t>(welded.positions.size());
        mesh->tets.reserve(occupiedCount * 5);

        std::vector<std::int32_t> nodeOf(static_cast<std::size_t>(grid.n[0] + 1) * (grid.n[1] + 1) * (grid.n[2] + 1), -1);
        std::vector<std::int32_t> firstTetOfCell(grid.CellCount(), -1);
        std::vector<std::array<double, 3>> nodePositions; // as stored (float), widened, for the embedding
        const auto nodeFor = [&](const int i, const int j, const int k) -> std::uint32_t
        {
            std::int32_t &id = nodeOf[grid.Node(i, j, k)];
            if (id < 0)
            {
                id = static_cast<std::int32_t>(mesh->positions.size());
                const float x = static_cast<float>(grid.origin[0] + i * grid.h);
                const float y = static_cast<float>(grid.origin[1] + j * grid.h);
                const float z = static_cast<float>(grid.origin[2] + k * grid.h);
                mesh->positions.emplace_back(x, y, z);
                nodePositions.push_back({x, y, z});
            }
            return static_cast<std::uint32_t>(id);
        };

        for (int k = 0; k < grid.n[2]; ++k)
        {
            for (int j = 0; j < grid.n[1]; ++j)
            {
                for (int i = 0; i < grid.n[0]; ++i)
                {
                    const std::size_t cell = grid.Cell(i, j, k);
                    if (!occupied[cell])
                    {
                        continue;
                    }
                    std::uint32_t corner[8];
                    for (int c = 0; c < 8; ++c)
                    {
                        corner[c] = nodeFor(i + (c & 1), j + ((c >> 1) & 1), k + ((c >> 2) & 1));
                    }
                    firstTetOfCell[cell] = static_cast<std::int32_t>(mesh->tets.size());
                    const auto &split = ((i + j + k) & 1) == 0 ? kSplitEven : kSplitOdd;
                    for (const auto &t : split)
                    {
                        Corners tet{corner[t[0]], corner[t[1]], corner[t[2]], corner[t[3]]};
                        if (Volume6(nodePositions[tet[0]], nodePositions[tet[1]], nodePositions[tet[2]],
                                    nodePositions[tet[3]]) < 0.0)
                        {
                            std::swap(tet[2], tet[3]); // a right-handed corner order: positive volume
                        }
                        mesh->tets.push_back(tet);
                    }
                }
            }
        }

        // 4. Embed every welded position (the best tetrahedron among its own voxel's and, if it sits outside, the
        // voxels around), then give each original vertex its position's embedding
        const auto tetPositions = [&](const std::uint32_t tet)
        {
            const Corners &c = mesh->tets[tet];
            return std::array<Vec3, 4>{nodePositions[c[0]], nodePositions[c[1]], nodePositions[c[2]],
                                       nodePositions[c[3]]};
        };
        std::vector<Physics::TetEmbedding> weldedEmbedding(welded.positions.size());
        for (std::size_t w = 0; w < welded.positions.size(); ++w)
        {
            const Vec3 &p = welded.positions[w];
            const int centre[3] = {grid.CellCoord(p[0], 0), grid.CellCoord(p[1], 1),
                                   grid.CellCoord(p[2], 2)};
            double bestScore = -std::numeric_limits<double>::max();
            std::uint32_t bestTet = 0;
            double bestWeights[4] = {1.0, 0.0, 0.0, 0.0};
            const auto consider = [&](const std::uint32_t tet)
            {
                double weights[4];
                if (!Barycentric(p, tetPositions(tet), weights))
                {
                    return;
                }
                const double score = std::min({weights[0], weights[1], weights[2], weights[3]});
                if (score > bestScore)
                {
                    bestScore = score;
                    bestTet = tet;
                    std::copy(std::begin(weights), std::end(weights), std::begin(bestWeights));
                }
            };
            for (int ring = 0; ring <= 2 && bestScore < -kInsideTolerance; ++ring)
            {
                for (int k = std::max(0, centre[2] - ring); k <= std::min(grid.n[2] - 1, centre[2] + ring); ++k)
                {
                    for (int j = std::max(0, centre[1] - ring); j <= std::min(grid.n[1] - 1, centre[1] + ring); ++j)
                    {
                        for (int i = std::max(0, centre[0] - ring); i <= std::min(grid.n[0] - 1, centre[0] + ring); ++i)
                        {
                            const bool onRing = std::max({std::abs(i - centre[0]), std::abs(j - centre[1]),
                                                          std::abs(k - centre[2])}) == ring;
                            const std::int32_t first = firstTetOfCell[grid.Cell(i, j, k)];
                            if (onRing && first >= 0)
                            {
                                for (std::uint32_t t = 0; t < 5; ++t)
                                {
                                    consider(static_cast<std::uint32_t>(first) + t);
                                }
                            }
                        }
                    }
                }
            }
            if (bestScore < -kInsideTolerance)
            {
                // Nothing within two voxels (a stray vertex far from the body): the nearest by a full scan
                for (std::uint32_t t = 0; t < mesh->tets.size(); ++t)
                {
                    consider(t);
                }
            }
            Physics::TetEmbedding &embedding = weldedEmbedding[w];
            embedding.tet = bestTet;
            for (int c = 0; c < 4; ++c)
            {
                embedding.weights[c] = static_cast<float>(bestWeights[c]);
            }
        }
        mesh->surfaceEmbedding.resize(vertices.size());
        for (std::size_t i = 0; i < vertices.size(); ++i)
        {
            mesh->surfaceEmbedding[i] = weldedEmbedding[welded.remap[i]];
        }

        result.mesh = std::move(mesh);
        return result;
    }

    TetResult Tetrahedralise(const Rendering::Mesh &mesh, const TetSettings &settings)
    {
        return Tetrahedralise(mesh.GetVertices(), mesh.GetIndices(), settings);
    }

    TetCache &TetCache::Instance()
    {
        static TetCache cache;
        return cache;
    }

    void TetCache::Clear()
    {
        _entries.clear();
    }

    TetResult TetCache::GetOrBuild(const std::shared_ptr<const Rendering::Mesh> &mesh, const int resolution)
    {
        TetResult result;
        if (!mesh)
        {
            result.error = "there is no mesh";
            return result;
        }
        const int requested = std::clamp(resolution, Physics::kMinTetResolution, Physics::kMaxTetResolution);
        const std::uint64_t version = mesh->GetVersion();

        // Entries whose mesh is gone can never match again (its address may be another mesh's now)
        std::erase_if(_entries, [](const Entry &entry) { return entry.mesh.expired(); });
        for (const Entry &entry : _entries)
        {
            if (entry.version == version && entry.resolution == requested && entry.mesh.lock() == mesh)
            {
                ++_hits;
                result.mesh = entry.data;
                result.cacheHit = true;
                return result;
            }
        }

        ++_misses;
        TetSettings settings;
        settings.resolution = requested;
        result = Tetrahedralise(*mesh, settings);
        if (result.Ok())
        {
            // An older version of this mesh is stale for good; other resolutions of this version stay cached
            std::erase_if(_entries, [&](const Entry &entry) { return entry.version != version && entry.mesh.lock() == mesh; });
            if (_entries.size() >= kMaxEntries)
            {
                _entries.erase(_entries.begin());
            }
            _entries.push_back(Entry{mesh, version, requested, result.mesh});
        }
        return result;
    }
}
