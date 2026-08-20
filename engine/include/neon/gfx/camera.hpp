#pragma once
#include "neon/math/mat4.hpp"
#include "neon/math/math.hpp"
#include "neon/math/vec3.hpp"

namespace neon::gfx {

struct Camera {
    math::Vec3 position{0.0f, 3.0f, 10.0f};
    math::Vec3 target{0.0f, 1.0f, 0.0f};
    math::Vec3 up{0.0f, 1.0f, 0.0f};
    float fovY = 55.0f * math::kDegToRad;
    float nearPlane = 0.1f;
    float farPlane = 800.0f;

    math::Mat4 View() const {
        math::Vec3 f = (target - position).Normalized();
        math::Vec3 s = math::Cross(f, up).Normalized();
        math::Vec3 u = math::Cross(s, f);
        math::Mat4 m;
        m.m[0] = s.x;  m.m[1] = s.y;  m.m[2] = s.z;
        m.m[4] = u.x;  m.m[5] = u.y;  m.m[6] = u.z;
        m.m[8] = -f.x; m.m[9] = -f.y; m.m[10] = -f.z;
        m.m[3] = -math::Dot(s, position);
        m.m[7] = -math::Dot(u, position);
        m.m[11] = math::Dot(f, position);
        return m;
    }

    math::Mat4 Projection(float aspect) const {
        return math::Mat4::Perspective(fovY, aspect, nearPlane, farPlane);
    }

    math::Mat4 ViewProjection(float aspect) const { return Projection(aspect) * View(); }
};

} // namespace neon::gfx
