#ifndef SHAPE_H
#define SHAPE_H

// Built-in primitives; shapeTable in loadObjects.h maps every entry to a
// Create* builder. NONE is not a shape, so it stays out of the SHAPE_COUNT
// range and shapeTable lookups.
typedef enum ShapeKind {
	NONE = -1,
	SHAPE_CUBE,
	SHAPE_SPHERE,
	SHAPE_SPHERE_HIGH,
	SHAPE_PLANE,
	SHAPE_CONE,
	SHAPE_CAPSULE,
	SHAPE_TORUS,
	SHAPE_DISK,
	SHAPE_PYRAMID,
	SHAPE_PRISM,
	SHAPE_HEMISPHERE,
	SHAPE_TUBE,
	SHAPE_QUAD,
	SHAPE_UV_SPHERE,
	SHAPE_ICOSPHERE,
	SHAPE_CYLINDER,
	SHAPE_COUNT
} ShapeKind;

#endif // SHAPE_H
