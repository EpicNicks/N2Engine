#pragma once

namespace N2Engine::Physics
{
    /**
     * Physics material properties
     * Controls friction and bounciness of colliders
     */
    struct PhysicsMaterial
    {
        float staticFriction = 0.5f;  // Friction when not moving (0 = ice, 1 = rubber)
        float dynamicFriction = 0.5f; // Friction when sliding (0 = ice, 1 = rubber)
        float restitution = 0.3f;     // Bounciness (0 = no bounce, 1 = perfect bounce)

        // Presets
        static PhysicsMaterial Default()
        {
            return {0.5f, 0.5f, 0.3f};
        }

        static PhysicsMaterial Ice()
        {
            return {0.05f, 0.05f, 0.1f};
        }

        static PhysicsMaterial Rubber()
        {
            return {0.9f, 0.8f, 0.8f};
        }

        static PhysicsMaterial Bouncy()
        {
            return {0.5f, 0.5f, 0.95f};
        }

        static PhysicsMaterial Metal()
        {
            return {0.4f, 0.3f, 0.5f};
        }
    };
}