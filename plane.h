#ifndef PLANE_H
#define PLANE_H
#include <assert.h>

typedef struct {
	float ox,oy;
	float min_x, max_x;
	float min_y, max_y;
	unsigned int pixels_per_unit;
	size_t scale;
}CartesianPlane2d;

#define SCREEN2PLANE(sval, scale, pixels_per_scale) (((float)(sval) / (float)(pixels_per_scale)) * (scale))
#define PLANE2SCREEN(pval, scale, pixels_per_scale)  (((float)(pval) / (float)(scale)) * (pixels_per_scale))

void plane_change_scale(CartesianPlane2d* plane, size_t scale);

#ifdef PLANE_IMPLEMENTATION

void plane_change_scale(CartesianPlane2d* plane, size_t scale)
{
	plane->scale = scale;
}


#endif // PLANE_IMPLEMENTATION
#endif // PLANE_H
