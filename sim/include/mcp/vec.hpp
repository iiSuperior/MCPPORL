// Ports of net.minecraft.world.phys.{Vec3, Vec2, AABB}. Every operation keeps
// vanilla's evaluation order and float/double widths.
#pragma once

#include <cmath>

#include "mcp/jmath.hpp"

namespace mcp {

enum class Axis { X, Y, Z };

struct Vec3 {
    double x = 0.0, y = 0.0, z = 0.0;

    MCP_HD Vec3() = default;
    MCP_HD Vec3(double x_, double y_, double z_) : x(x_), y(y_), z(z_) {}

    MCP_HD Vec3 add(double ax, double ay, double az) const { return {x + ax, y + ay, z + az}; }
    MCP_HD Vec3 add(const Vec3& v) const { return add(v.x, v.y, v.z); }
    MCP_HD Vec3 subtract(double ax, double ay, double az) const { return add(-ax, -ay, -az); }
    MCP_HD Vec3 multiply(double sx, double sy, double sz) const { return {x * sx, y * sy, z * sz}; }
    MCP_HD Vec3 scale(double s) const { return multiply(s, s, s); }
    MCP_HD double lengthSqr() const { return x * x + y * y + z * z; }
    MCP_HD double length() const { return ::sqrt(x * x + y * y + z * z); }
    MCP_HD double horizontalDistanceSqr() const { return x * x + z * z; }

    MCP_HD Vec3 normalize() const {
        double dist = ::sqrt(x * x + y * y + z * z);
        return dist < static_cast<double>(1.0E-5F) ? Vec3{} : Vec3{x / dist, y / dist, z / dist};
    }

    MCP_HD double get(Axis a) const { return a == Axis::X ? x : (a == Axis::Y ? y : z); }
    MCP_HD Vec3 with(Axis a, double v) const {
        return {a == Axis::X ? v : x, a == Axis::Y ? v : y, a == Axis::Z ? v : z};
    }
    MCP_HD bool isFinite() const { return std::isfinite(x) && std::isfinite(y) && std::isfinite(z); }
};

struct Vec2 {
    float x = 0.0F, y = 0.0F;

    MCP_HD Vec2() = default;
    MCP_HD Vec2(float x_, float y_) : x(x_), y(y_) {}

    MCP_HD Vec2 scale(float s) const { return {x * s, y * s}; }
    MCP_HD float lengthSquared() const { return x * x + y * y; }
    // TODO(verify vs Vec2 source): length/normalized.
    MCP_HD float length() const { return static_cast<float>(::sqrt(static_cast<double>(x * x + y * y))); }
    MCP_HD Vec2 normalized() const {
        float len = static_cast<float>(::sqrt(static_cast<double>(x * x + y * y)));
        return len < 1.0E-4F ? Vec2{} : Vec2{x / len, y / len};
    }
};

struct AABB {
    double minX, minY, minZ, maxX, maxY, maxZ;

    MCP_HD double min(Axis a) const { return a == Axis::X ? minX : (a == Axis::Y ? minY : minZ); }
    MCP_HD double max(Axis a) const { return a == Axis::X ? maxX : (a == Axis::Y ? maxY : maxZ); }

    MCP_HD AABB move(double xa, double ya, double za) const {
        return {minX + xa, minY + ya, minZ + za, maxX + xa, maxY + ya, maxZ + za};
    }
    MCP_HD AABB move(const Vec3& v) const { return move(v.x, v.y, v.z); }

    MCP_HD AABB expandTowards(double xa, double ya, double za) const {
        AABB r = *this;
        if (xa < 0.0) r.minX += xa; else if (xa > 0.0) r.maxX += xa;
        if (ya < 0.0) r.minY += ya; else if (ya > 0.0) r.maxY += ya;
        if (za < 0.0) r.minZ += za; else if (za > 0.0) r.maxZ += za;
        return r;
    }
    MCP_HD AABB expandTowards(const Vec3& v) const { return expandTowards(v.x, v.y, v.z); }

    MCP_HD bool intersects(double x0, double y0, double z0, double x1, double y1, double z1) const {
        return minX < x1 && maxX > x0 && minY < y1 && maxY > y0 && minZ < z1 && maxZ > z0;
    }
};

}  // namespace mcp
