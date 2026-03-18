#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <SDL2/SDL.h>
#include <math.h>
#include "desc_font.h"
#include <errno.h>


typedef unsigned char u8;

#define WIDTH 800
#define HEIGHT 600
#define MARKER_WIDTH 2
#define DEFAULT_PIXELS_PER_UNIT 100
#define DEFAULT_STEP 2

#define FONT_SIZE 12
#define FONT_SPACING 0
#define FONT_SPRITE_COLS 10
#define FONT_SPRITE_ROWS 10
#define FONT_SPRITE_W desc_font_width / FONT_SPRITE_COLS
#define FONT_SPRITE_H desc_font_height / FONT_SPRITE_ROWS

#define BG 0xffffff
#define AXIS_COLOR 0x000000
#define GRID_COLOR 0xd3cecd
#define NUMBERS_COLOR 0x182dfc

#define GET_COMPONENT_B(rgb) (u8)((rgb) & 0xff)
#define GET_COMPONENT_G(rgb) (u8)(((rgb) >> 8) & 0xff)
#define GET_COMPONENT_R(rgb) (u8)(((rgb) >> 16) & 0xff)

#define ARR_LEN(arr) (sizeof((arr)) / sizeof((arr[0])))

//FIX(caspeer): numbers rendering position

typedef struct {
	float x;
	float y;
}Vec2;

typedef struct {
	Vec2 origin;
	unsigned int pixels_per_unit;
	int step;
}Plane;

static char* program_name = "funplot";
static char temp_buf[1024] = {0};
static SDL_Texture* font_texture;

SDL_Surface *load_png_file_as_surface(uint32_t *data, size_t width, size_t height)
{
	SDL_Surface* image_surface = SDL_CreateRGBSurfaceFrom(
									  data,
									  (int) width,
									  (int) height,
									  32,
									  (int) width * 4,
									  0x000000FF,
									  0x0000FF00,
									  0x00FF0000,
									  0xFF000000);
	return image_surface;
}

void render_number(SDL_Renderer* renderer, int num, int x, int y)
{
	int r =  snprintf(temp_buf,ARR_LEN(temp_buf),"%d",num);
	if (r < 0){
		fprintf(stderr, "[Warning]: %s", strerror(errno));
	}else {
		int start_x = x;//+ FONT_SPRITE_W/2;
		for (int j = 0; j < r; j++){
			int i = temp_buf[j] - 32; // get the index in the sprite
			int rowpx = (i / (FONT_SPRITE_COLS)) * FONT_SPRITE_H;
			int colpx = (i % (FONT_SPRITE_COLS)) * FONT_SPRITE_W;
			SDL_Rect sRect = {.x = colpx+FONT_SPRITE_W/4, .y = rowpx, .w = FONT_SPRITE_W/2, .h = FONT_SPRITE_H};
			SDL_Rect dRect = {.x = start_x, .y = y, .w = FONT_SIZE, .h = FONT_SIZE};
			start_x += FONT_SIZE; // spacing
			SDL_RenderCopy(renderer, font_texture, &sRect, &dRect);
		}
	}
}

void render_cartesianplane2d(SDL_Renderer * renderer, CartesianPlane2d* plane, int width, int height)
{
	//TODO(caspeer): draw another grid lines that denotes subunits;
	int x = plane->ox * (float)width;
	int y = plane->oy * (float)height;
	// origin label
	render_number(renderer, 0, x+FONT_SIZE, y+X_AXIS_NUMBER_YOFFSET);
	int unit = plane->pixels_per_unit;
	SDL_SetRenderDrawColor(renderer, GET_COMPONENT_R(AXIS_COLOR), GET_COMPONENT_G(AXIS_COLOR), GET_COMPONENT_B(AXIS_COLOR), 255);
	SDL_RenderDrawLine(renderer, 0,y, width,y);
	SDL_RenderDrawLine(renderer, x, 0, x, height);
	SDL_SetRenderDrawColor(renderer, GET_COMPONENT_R(GRID_COLOR), GET_COMPONENT_G(GRID_COLOR), GET_COMPONENT_B(GRID_COLOR), 127);
	// x-axis
	int acc = plane->scale;
	for (int i = x+unit; i <= width; i += unit){
		render_number(renderer, acc, i+X_AXIS_NUMBER_XOFFSET, y+X_AXIS_NUMBER_YOFFSET);
		SDL_RenderDrawLine(renderer, i,0, i, height);
		acc += plane->scale;
	}
	acc = -plane->scale;
	for (int i = x-unit; i >= 0; i -= unit){
		render_number(renderer, acc, i+X_AXIS_NUMBER_XOFFSET, y+X_AXIS_NUMBER_YOFFSET);
		acc -= plane->scale;
		SDL_RenderDrawLine(renderer, i,0, i, height);
	}
	// y-axis
	acc = -plane->scale;
	for (int i = y+unit; i <= height; i += unit){
		render_number(renderer, acc, x+Y_AXIS_NUMBER_XOFFSET, i + Y_AXIS_NUMBER_YOFFSET);
		SDL_RenderDrawLine(renderer, 0, i, width, i);
		acc -= plane->scale;
	}
	acc = plane->scale;
	for (int i = y-unit; i >= 0; i -= unit){
		render_number(renderer, acc, x+Y_AXIS_NUMBER_XOFFSET, i + Y_AXIS_NUMBER_YOFFSET);
		SDL_RenderDrawLine(renderer, 0, i, width, i);
		acc += plane->scale;
	}
}

void plot_function(SDL_Renderer * renderer, CartesianPlane2d* plane,  int width, int height)
{
	//NOTE(caspeer): we need to interpolate the function range with a high percision
	SDL_Point* points = malloc(sizeof(SDL_Point)*2048*2048);
	int count = 0;
	float x = plane->min_x;
	//NOTE(caspeer): is it okay for this fuction or any function to depends on constant
	float sampling_interval = (float)(plane->max_x - plane->min_x)/(float)FUNC_SAMPLES;

	//TODO(caspeer): make the rendering looks smoother
	//TODO(caspeer): anti-alaising and MSAA ??
	//TODO(caspeer): is there a better way other then lerping ??
	//TODO(caspeer): make the plotting depends on the 2d plane.

	int ox = plane->ox*width;
	int oy = plane->oy*height;

	while (x <= plane->max_x){
		float f_x =  x*x;
		// get the screen coordinates from the plane coordinate
		int y_pos = oy - PLANE2SCREEN(f_x, plane->scale, plane->pixels_per_unit);
		int x_pos = ox + PLANE2SCREEN(x, plane->scale, plane->pixels_per_unit);
		SDL_Point p = {.x = x_pos, .y = y_pos};
		points[count++] = p;
		x += sampling_interval;
	}
	SDL_RenderDrawLines(renderer, points, count);
	free(points);
}

int main()
{

	if (SDL_Init(SDL_INIT_VIDEO) < 0){
		fprintf(stderr, "[Fatal]: Can't Initialize the program\ndue to: %s",
				SDL_GetError());
		exit(1);
	}

	SDL_Window* win = SDL_CreateWindow(program_name, 0, 0, WIDTH, HEIGHT,SDL_WINDOW_RESIZABLE );
	SDL_Renderer* renderer = SDL_CreateRenderer(win, -1,SDL_RENDERER_ACCELERATED);

	if (win == NULL || renderer == NULL){
		fprintf(stderr, "[Fatal]: Can't Open the window\ndue to: %s", SDL_GetError());
		exit(1);
	}
	SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "1"); // linear
	SDL_Surface* font_surface = load_png_file_as_surface(desc_font_data, desc_font_width, desc_font_height);
	font_texture = SDL_CreateTextureFromSurface(renderer, font_surface);
	if (font_surface == NULL || font_texture == NULL){
		fprintf(stderr, "[Warning]: couldn't load the font: %s",SDL_GetError());
	}
	SDL_SetTextureColorMod(font_texture, GET_COMPONENT_R(NUMBERS_COLOR),GET_COMPONENT_R(NUMBERS_COLOR),GET_COMPONENT_R(NUMBERS_COLOR));
	SDL_FreeSurface(font_surface);
	bool quit = false;
	int width = WIDTH;
	int height = HEIGHT;
	Vec2 origin = {.x = 0.5f, .y = 0.5f};
	Plane plane = {.origin = origin, .pixels_per_unit = DEFAULT_PIXELS_PER_UNIT, .step = DEFAULT_STEP};
	bool drag = false;
	// Event Loop
	while (!quit){
		SDL_Event event = {0};
		while (SDL_PollEvent(&event)){
			switch (event.type){
			case SDL_QUIT:{
				quit = true;
			} break;
			case SDL_MOUSEMOTION:{
				if ((event.motion.state & 1) == 1){
					plane.origin.x += event.motion.xrel / (float)width;
					plane.origin.y += event.motion.yrel / (float)height;
				}
			}break;
			}
		}
		SDL_GetWindowSize(win, &width, &height);
		SDL_SetRenderDrawColor(renderer, GET_COMPONENT_R(BG), GET_COMPONENT_G(BG), GET_COMPONENT_B(BG), 255);
		SDL_RenderClear(renderer);
		render_plane(renderer, plane, width, height);
		SDL_SetRenderDrawColor(renderer, 0xff, 0x00,0x00, 255);
		plot_function(renderer, width, height);
		SDL_RenderPresent(renderer);
	}
	return 0;
}
