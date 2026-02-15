#include "bbox.h"

void UpdateBoundingBox(float3* BBmin, float3* BBmax, float3 vertex) {
    if (vertex.x < BBmin->x) BBmin->x = vertex.x;
    if (vertex.y < BBmin->y) BBmin->y = vertex.y;
    if (vertex.z < BBmin->z) BBmin->z = vertex.z;

    if (vertex.x > BBmax->x) BBmax->x = vertex.x;
    if (vertex.y > BBmax->y) BBmax->y = vertex.y;
    if (vertex.z > BBmax->z) BBmax->z = vertex.z;
}