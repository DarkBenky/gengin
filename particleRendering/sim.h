#include "../object/format.h"

typedef struct ParticleContainer ParticleContainer;

// Per-particle callbacks: receive the container and index, return a computed value
typedef float3 (*ParticleColorFn)(const ParticleContainer *pc, int index);
typedef float (*ParticleOpacityFn)(const ParticleContainer *pc, int index);
typedef void (*ParticleUpdateFn)(ParticleContainer *pc, int index); // updates individual particle

typedef struct Cell {
    float3 BBoxMin;
    float3 BBoxMax;

    int particleCount;
    int particleCapacity; // does not support resizing when the container is full try to respawn particles

    int *particleIndices;
} Cell;

typedef struct ParticleSubVolumes {
    int xCellCount;
    int yCellCount;
    int zCellCount;

    Cell *cells;
} ParticleSubVolumes;

typedef struct ParticleContainer {
    int particleCount;

    // SOA - layout
    float *xPos;
    float *yPos;
    float *zPos;

    float *xVel;
    float *yVel;
    float *zVel;

    int3 *particleCellIndex;

    float ParticleLifeTime;
    float *lifetime;

    // Volume where particles can live when they move out of bounds they respawn
    float3 BBmin;
    float3 BBmax;
    float3 SpawnPoint;
    float3 DefaultVelocityVector;

    ParticleColorFn ColorFn;
    ParticleOpacityFn OpacityFn;
    ParticleUpdateFn UpdateFn;

    ParticleSubVolumes SubVolumes;
} ParticleContainer;

static void InitParticleContainer(
    ParticleContainer *particles,
    int particleCount,
    float3 spawnPosition,
    float3 BBmin,
    float3 BBmax,
    float3 ParticleLifeTime,
    ParticleColorFn colorFn,
    ParticleOpacityFn opacityFn,
    ParticleUpdateFn updateFn)
{
    particles->particleCount = particleCount;
    particles->BBmin = BBmin;
    particles->BBmax = BBmax;
    particles->SpawnPoint = spawnPosition;

    particles->ColorFn = colorFn;
    particles->OpacityFn = opacityFn;
    particles->UpdateFn = updateFn;

    // TODO: Finish
}

// TODO:
static void UpdateParticles(
    ParticleContainer *particles,
    float deltaTime
) {
    
}