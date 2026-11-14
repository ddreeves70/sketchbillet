#pragma once

/// @file
/// The kernel abstraction layer.

#include <filesystem>

#include "sb/kernel/Types.h"

namespace sb::kernel {

/// The Kernel Abstraction Layer.
///
/// Every geometry operation in SketchBillet goes through this interface, so the kernel
/// can be tested, recorded and replaced without touching the rest of the code.
///
/// Rules for implementations:
///  - Never throw across this boundary; report problems in OpResult::issues.
///  - Never expose kernel-specific types in this header.
///  - Be deterministic: same inputs, same output.
///
/// Phase 0 covers primitives, Booleans, meshing and export; moving came next, then
/// faces from sketch shapes and extrude. Phase 2 added faces, edges and vertices
/// (elements()), shape history in every OpResult, fillet and chamfer.
class IKernel {
public:
    /// Destroys the kernel. Shapes it made stay valid.
    virtual ~IKernel() = default;

    /// Makes a rectangular block with edges along the x, y and z axes.
    /// @param corner The corner with the smallest x, y and z (mm).
    /// @param dx Size along x (mm). Must be greater than 0.
    /// @param dy Size along y (mm). Must be greater than 0.
    /// @param dz Size along z (mm). Must be greater than 0.
    /// @return The solid, or diagnostics if a size is not positive.
    virtual OpResult makeBox(const Point3& corner, double dx, double dy, double dz) = 0;

    /// Makes a solid cylinder.
    /// @param baseCenter Center of the bottom face (mm).
    /// @param axis Direction from the bottom face to the top face. Any length except zero.
    /// @param radius Radius (mm). Must be greater than 0.
    /// @param height Height along @p axis (mm). Must be greater than 0.
    /// @return The solid, or diagnostics if an input is not valid.
    virtual OpResult makeCylinder(const Point3& baseCenter, const Vec3& axis, double radius,
                                  double height) = 0;

    /// Combines two shapes.
    /// @param op Union, cut (@p a minus @p b) or intersect.
    /// @param a The first shape; for a cut, the one that keeps its material.
    /// @param b The second shape; for a cut, the tool that is removed.
    /// @return The combined shape, or diagnostics if the operation failed. Its
    ///         history gives each face's Face in @p a (input 0) or @p b (input 1).
    virtual OpResult boolean(BoolOp op, const ShapeHandle& a, const ShapeHandle& b) = 0;

    /// Measures the smallest axis-aligned box around a shape.
    /// @param shape The shape to measure.
    /// @return A tight (not padded) bounding box (mm).
    virtual Box3 bounds(const ShapeHandle& shape) const = 0;

    /// Measures volume, area and center of mass.
    /// @param shape The shape to measure.
    /// @return The mass properties, assuming uniform density.
    virtual MassProps massProps(const ShapeHandle& shape) const = 0;

    /// Counts the topological entities of a shape.
    /// @param shape The shape to count.
    /// @return Counts of solids, shells, faces, edges and vertices.
    virtual Topology topology(const ShapeHandle& shape) const = 0;

    /// Checks that the shape is valid and that every solid is closed (watertight).
    /// @param shape The shape to check.
    /// @return No diagnostics for a valid closed solid; otherwise what is wrong.
    virtual Diagnostics validate(const ShapeHandle& shape) const = 0;

    /// Turns exact geometry into triangles, for display and STL export.
    /// @param shape The shape to mesh.
    /// @param quality How closely the triangles follow the surface.
    /// @return The mesh; empty if the shape has no faces.
    virtual Mesh tessellate(const ShapeHandle& shape, const MeshQuality& quality) const = 0;

    /// Writes a shape to a file.
    /// @param shape The shape to write.
    /// @param format STL or STEP.
    /// @param path Where to write. An existing file is replaced.
    /// @return No diagnostics on success; otherwise why the file could not be written.
    virtual Diagnostics exportFile(const ShapeHandle& shape, ExportFormat format,
                                   const std::filesystem::path& path) const = 0;

    // Added after Phase 0. New operations go at the end, so plugins built
    // against an earlier IKernel keep working.

    /// Moves and turns a copy of a shape. The shape itself isn't changed.
    /// @param shape The shape to move.
    /// @param motion The turn (radians, about a line in mm), then the slide (mm).
    /// @return The moved shape, or diagnostics if the axis is 0, 0, 0 while turning.
    ///         Its history gives each face's Face in @p shape.
    virtual OpResult transform(const ShapeHandle& shape, const Motion& motion) = 0;

    /// Makes a flat face from an outline and its holes, on a plane (a sketch's
    /// closed shape, ready to extrude).
    ///
    /// Ends of neighboring edges may be a hair apart (up to about 1e-5 mm, as a
    /// solver leaves them); they're joined where they meet.
    /// @param profile The outline (counterclockwise) and holes (clockwise), in the
    ///        plane's x and y (mm).
    /// @param plane Where the face lies.
    /// @return The face, or diagnostics if the outline doesn't close, a piece has no
    ///         length or size, or the plane's directions aren't square to each other.
    ///         Its history gives, for each edge, the Curve it was made from.
    virtual OpResult makeFace(const Profile2& profile, const Plane& plane) = 0;

    /// Sweeps a flat face along a straight line, making a solid: a rectangle
    /// becomes a block, a circle a cylinder.
    /// @code
    /// auto face = kernel.makeFace(profile, plane);
    /// auto solid = kernel.extrude(face.shape, {0, 0, 10});  // 10 mm straight up
    /// @endcode
    /// @param face A flat face, like one from makeFace().
    /// @param vector Which way and how far to sweep it (mm). Not 0, 0, 0, and not
    ///        lying flat in the face.
    /// @return The solid, or diagnostics if the face or the vector won't do. Its
    ///         history: each side face from an Edge of the face (input 0), and the
    ///         two ends from Start and End.
    virtual OpResult extrude(const ShapeHandle& face, const Vec3& vector) = 0;

    // Added in Phase 2: faces, edges and vertices, for naming and picking.

    /// Lists a shape's faces, edges and vertices, what each looks like, and which
    /// faces each edge borders.
    /// @code
    /// auto box = kernel.makeBox({0, 0, 0}, 10, 20, 30);
    /// auto parts = kernel.elements(box.shape);  // 6 faces, 12 edges, 8 vertices
    /// @endcode
    /// @param shape The shape to list.
    /// @return The lists; empty if the shape isn't valid. The same shape always gives
    ///         the same lists in the same order.
    virtual Elements elements(const ShapeHandle& shape) const = 0;

    /// Rounds a solid's edges with a fillet: each edge becomes a round face of the
    /// given radius, blending the two faces it joined.
    /// @param shape The solid.
    /// @param edges Which edges, by number in elements() of @p shape. At least one.
    /// @param radius The round's radius (mm). More than 0, and small enough to fit
    ///        between the faces.
    /// @return The rounded solid, with history: kept faces from Face, round faces
    ///         from Edge (and corner blends from Vertex) of input 0. Diagnostics if an
    ///         edge number isn't in the shape or the round doesn't fit.
    virtual OpResult fillet(const ShapeHandle& shape, const std::vector<int>& edges, double radius) = 0;

    /// Bevels a solid's edges with a chamfer: each edge becomes a flat face cutting
    /// the corner off, the same distance back along both faces.
    /// @param shape The solid.
    /// @param edges Which edges, by number in elements() of @p shape. At least one.
    /// @param distance How far back from the edge the bevel starts on each face (mm). More than 0.
    /// @return The beveled solid, with history like fillet(). Diagnostics if an edge
    ///         number isn't in the shape or the bevel doesn't fit.
    virtual OpResult chamfer(const ShapeHandle& shape, const std::vector<int>& edges, double distance) = 0;

    /// Turns a flat face around a line in its plane, making a solid: a rectangle
    /// beside the line becomes a tube or a disc, a half circle on it a ball.
    /// @code
    /// auto face = kernel.makeFace(profile, plane);
    /// // A full turn about the line through 0, 0, 0 going up (z).
    /// auto solid = kernel.revolve(face.shape, {0, 0, 0}, {0, 0, 1}, 2 * std::numbers::pi);
    /// @endcode
    /// @param face A flat face, like one from makeFace(). It may touch the line but
    ///        not cross it.
    /// @param point A point on the line (mm).
    /// @param direction Which way the line goes. Not 0, 0, 0, and lying in the face's
    ///        plane, as @p point must.
    /// @param angle How far to turn (radians), the right-hand way about
    ///        @p direction: more than 0 and at most 2 pi. A full turn (2 pi) has no
    ///        ends.
    /// @return The solid, or diagnostics if the face, the line or the angle won't
    ///         do. Its history: each side face from an Edge of the face (input 0;
    ///         an edge lying on the line makes no face), and for less than a full
    ///         turn the two ends from Start and End.
    virtual OpResult revolve(const ShapeHandle& face, const Point3& point, const Vec3& direction,
                             double angle) = 0;

    /// Reflects a copy of a shape across a plane, as in a mirror: a left-hand
    /// part becomes a right-hand one. The shape itself isn't changed.
    /// @param shape The shape to reflect.
    /// @param point A point on the plane (mm).
    /// @param normal The plane's normal: square to it, any length but 0.
    /// @return The reflected shape, or diagnostics if the normal is 0, 0, 0. Its
    ///         history gives each face's Face in @p shape.
    virtual OpResult mirror(const ShapeHandle& shape, const Point3& point, const Vec3& normal) = 0;

    /// Hollows a solid out, leaving walls of a thickness and some faces open, like
    /// a box with its lid taken off. The outside stays the same size.
    /// @param shape The solid.
    /// @param faces Which faces to leave open, by number in elements() of
    ///        @p shape. At least one.
    /// @param thickness How thick the walls are (mm): more than 0, and thin enough
    ///        to fit inside the solid.
    /// @return The hollow solid. Its history: outside faces from their Face of
    ///         input 0, and each inside face from the outside Face it's a wall of,
    ///         with OriginKind::Inside. Diagnostics if a face number isn't in the
    ///         shape or the walls don't fit.
    virtual OpResult shell(const ShapeHandle& shape, const std::vector<int>& faces, double thickness) = 0;

    // Added in Phase 3: measuring.

    /// Finds the shortest distance between two faces, edges or vertices, of one
    /// shape or of two, and the points where it's shortest.
    /// @code
    /// auto box = kernel.makeBox({0, 0, 0}, 10, 20, 30).shape;
    /// auto gap = kernel.distance({box, TopoKind::Face, 0}, {box, TopoKind::Face, 1});
    /// @endcode
    /// @param a The first element.
    /// @param b The second element.
    /// @return The distance (mm) and the two nearest points, or diagnostics if an
    ///         element isn't in its shape. Where many points are nearest (two
    ///         parallel faces, say), it gives one of them, the same one every time.
    virtual Closest distance(const ElementOf& a, const ElementOf& b) const = 0;
};

}  // namespace sb::kernel
