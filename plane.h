#ifndef PLANE_H
#define PLANE_H

//NOTE(caspeer): the plane keeps its origin as a fraction of the viewport so
// window resizing keeps the view settled. one "unit" is `scale` in world
// value and `pixels_per_unit` pixels on screen.
typedef struct {
	double ox, oy;           // origin as a fraction of the viewport
	double pixels_per_unit;  // pixels per one grid unit (one step)
	double scale;            // the world value of one grid unit
} CartesianPlane2d;

// sval is a viewport pixel offset from the origin (already signed),
// pval is a world value. for the y axis the caller negates the offset
// since up on the plane is down on the screen.
#define SCREEN2PLANE(sval, scale, pixels_per_scale) (((double)(sval) / (double)(pixels_per_scale)) * (scale))
#define PLANE2SCREEN(pval, scale, pixels_per_scale)  (((double)(pval) / (double)(scale)) * (pixels_per_scale))

// renormalization keeps the grid step visually in [PLANE_MIN_PPU, PLANE_MAX_PPU)
// by halving/doubling both `scale` and `pixels_per_unit`, which does not move
// anything on screen. it makes the labels stay a fixed size while zooming.
#define PLANE_MIN_PPU 50.0
#define PLANE_MAX_PPU 200.0

void plane_renormalize(CartesianPlane2d* plane);
// anchor the zoom at the cursor position (viewport pixels)
void plane_zoom(CartesianPlane2d* plane, double factor, double mouse_x, double mouse_y, int width, int height);

#ifdef PLANE_IMPLEMENTATION

void plane_renormalize(CartesianPlane2d* plane)
{
	while (plane->pixels_per_unit >= PLANE_MAX_PPU){
		plane->pixels_per_unit *= 0.5;
		plane->scale *= 0.5;
	}
	while (plane->pixels_per_unit < PLANE_MIN_PPU){
		plane->pixels_per_unit *= 2.0;
		plane->scale *= 2.0;
	}
}

void plane_zoom(CartesianPlane2d* plane, double factor, double mouse_x, double mouse_y, int width, int height)
{
	if (!(factor > 0.0)) return;
	double ox_px = plane->ox * width;
	double oy_px = plane->oy * height;
	// world value under the cursor, before the zoom
	double vx = SCREEN2PLANE(mouse_x - ox_px, plane->scale, plane->pixels_per_unit);
	double vy = SCREEN2PLANE(oy_px - mouse_y, plane->scale, plane->pixels_per_unit);

	plane->pixels_per_unit *= factor;
	plane_renormalize(plane);

	// keep the cursor anchored to the same world value
	plane->ox = (mouse_x - PLANE2SCREEN(vx, plane->scale, plane->pixels_per_unit)) / (double) width;
	plane->oy = (mouse_y + PLANE2SCREEN(vy, plane->scale, plane->pixels_per_unit)) / (double) height;
}

#endif // PLANE_IMPLEMENTATION
#endif // PLANE_H
