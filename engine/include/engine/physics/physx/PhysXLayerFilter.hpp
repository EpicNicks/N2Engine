#pragma once

#ifdef N2ENGINE_PHYSX_ENABLED

#include <cstdint>

#include <PxFiltering.h>

#include "engine/Layers.hpp"

namespace N2Engine::Physics
{
    // The layer filtering of the PhysX scene, shared by PhysXBackend (rigid shapes), PhysXDeformableDriver (soft
    // bodies) and the tests. Header-only so a CPU-only test can call the scene's filter shader directly.

    /// A shape's layer as query filter data: word0 is its layer bit, which PhysX's fixed-function query filter ANDs
    /// with a query's layer mask
    inline physx::PxFilterData LayerQueryData(const int layer)
    {
        return physx::PxFilterData(Layers::MaskOf(layer), 0, 0, 0);
    }

    /// A shape's layer as simulation filter data: word0 is the layer bit and word1 the layers it collides with (its
    /// row of the Layers collision matrix)
    inline physx::PxFilterData LayerSimulationData(const int layer)
    {
        return physx::PxFilterData(Layers::MaskOf(layer), Layers::CollisionMaskFor(layer), 0, 0);
    }

    /// Simulation filter data for an actor the engine does not put on a layer (a soft body's deformable volume): it
    /// is on the Default layer and accepts every layer, so whether it collides with a collider is decided by that
    /// collider's row of the matrix alone (its Default bit). Nothing has to refresh it when the matrix changes. A
    /// shape PhysX creates is all-zero filter data, which LayersCollide rejects against every layer: a soft body
    /// left at that fell through every floor.
    inline physx::PxFilterData UnlayeredSimulationData()
    {
        return physx::PxFilterData(Layers::MaskOf(Layers::Default), 0xFFFFFFFFu, 0, 0);
    }

    /// Each side has to accept the other's layer
    inline bool LayersCollide(const physx::PxFilterData &a, const physx::PxFilterData &b)
    {
        return (a.word0 & b.word1) != 0 && (b.word0 & a.word1) != 0;
    }

    /// The scene's filter shader. PxDefaultSimulationFilterShader only asks for touch notifications on trigger
    /// pairs, so onContact never ran for solid contacts and OnCollisionEnter/Stay/Exit never fired. Reads nothing
    /// but its arguments: PhysX may call it from worker threads. It also runs for a soft body against a collider
    /// (PxFilterObjectType::eDEFORMABLE_VOLUME).
    inline physx::PxFilterFlags CollisionEventFilterShader(
        const physx::PxFilterObjectAttributes attributes0, const physx::PxFilterData filterData0,
        const physx::PxFilterObjectAttributes attributes1, const physx::PxFilterData filterData1,
        physx::PxPairFlags &pairFlags, const void *, physx::PxU32)
    {
        // Before the trigger branch, so the matrix applies to triggers too (as in Unity). Suppressed rather than
        // killed: a change of filter data or resetFiltering asks again.
        if (!LayersCollide(filterData0, filterData1))
        {
            return physx::PxFilterFlag::eSUPPRESS;
        }

        if (physx::PxFilterObjectIsTrigger(attributes0) || physx::PxFilterObjectIsTrigger(attributes1))
        {
            pairFlags = physx::PxPairFlag::eTRIGGER_DEFAULT;
            return physx::PxFilterFlag::eDEFAULT;
        }

        pairFlags = physx::PxPairFlag::eCONTACT_DEFAULT
                    | physx::PxPairFlag::eNOTIFY_TOUCH_FOUND
                    | physx::PxPairFlag::eNOTIFY_TOUCH_LOST
                    | physx::PxPairFlag::eNOTIFY_CONTACT_POINTS;
        return physx::PxFilterFlag::eDEFAULT;
    }
}

#endif
