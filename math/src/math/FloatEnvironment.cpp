#include "math/FloatEnvironment.hpp"

#if defined(N2_FP_CONTROL_X86)
#include <xmmintrin.h>
#endif

namespace N2Engine::Math
{
    FloatEnvironment::Word FloatEnvironment::Read()
    {
#if defined(N2_FP_CONTROL_X86)
        return static_cast<Word>(_mm_getcsr());
#elif defined(N2_FP_CONTROL_ARM64)
        std::uint64_t value = 0;
        __asm__ __volatile__("mrs %0, fpcr" : "=r"(value));
        return static_cast<Word>(value);
#else
        return 0;
#endif
    }

    void FloatEnvironment::Write(const Word value)
    {
#if defined(N2_FP_CONTROL_X86)
        _mm_setcsr(static_cast<unsigned int>(value));
#elif defined(N2_FP_CONTROL_ARM64)
        const std::uint64_t raw = value;
        __asm__ __volatile__("msr fpcr, %0" : : "r"(raw));
#else
        (void)value;
#endif
    }

    bool FloatEnvironment::IsDeterministic()
    {
        if constexpr (!kHasControlState)
        {
            return true;
        }
        return (Read() & kGovernedMask) == kPolicy;
    }

    FloatEnvironmentGuard::FloatEnvironmentGuard()
        : _previous(FloatEnvironment::Read()), _verified(true)
    {
        if constexpr (FloatEnvironment::kHasControlState)
        {
            FloatEnvironment::Write(FloatEnvironment::WithPolicy(_previous));
            _verified = FloatEnvironment::IsDeterministic();
        }
    }

    FloatEnvironmentGuard::~FloatEnvironmentGuard()
    {
        if constexpr (FloatEnvironment::kHasControlState)
        {
            // The governed bits back as they were; the status flags stay as the guarded code left them
            const FloatEnvironment::Word current = FloatEnvironment::Read();
            FloatEnvironment::Write((current & ~FloatEnvironment::kGovernedMask) |
                                    (_previous & FloatEnvironment::kGovernedMask));
        }
    }
}
