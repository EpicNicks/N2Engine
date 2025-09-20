#include "math/MathRegistrar.hpp"

#include "math/Vector3.hpp"
#include "math/Quaternion.hpp"
#include "math/Matrix.hpp"

void N2Engine::Math::InitializeSIMD()
{
    Vector3::InitializeSIMD();
    Quaternion::InitializeSIMD();
    Matrix<float, 4, 4>::InitializeSIMD();
    Matrix<float, 3, 3>::InitializeSIMD();
}