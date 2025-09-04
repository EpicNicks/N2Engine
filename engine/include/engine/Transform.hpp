#pragma once
#include <math/Matrix.hpp>

class Transform
{
private:
    N2Engine::Math::Matrix<float, 4, 4> _transform;
};