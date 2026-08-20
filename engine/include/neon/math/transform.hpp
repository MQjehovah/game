#pragma once
#include "neon/math/mat4.hpp"
#include "neon/math/quat.hpp"
#include "neon/math/vec3.hpp"

namespace neon::math {

struct Transform {
    Vec3 position{};
    Quat rotation{};
    Vec3 scale{1.0f, 1.0f, 1.0f};

    Mat4 ToMat4() const {
        Mat4 r = rotation.ToMat4();
        Mat4 s = Mat4::Scale(scale);
        Mat4 m = r * s;
        m.m[3] = position.x;
        m.m[7] = position.y;
        m.m[11] = position.z;
        return m;
    }

    Vec3 Forward() const { return rotation.Rotate(Vec3::Forward()); }
    Vec3 Right() const { return rotation.Rotate(Vec3::Right()); }
    Vec3 Up() const { return rotation.Rotate(Vec3::Up()); }
};

} // namespace neon::math
