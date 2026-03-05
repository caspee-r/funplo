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
#define NUMBERS_COLOR 0x182dfc

#define GET_COMPONENT_B(rgb) (u8)((rgb) & 0xff)
#define GET_COMPONENT_G(rgb) (u8)(((rgb) >> 8) & 0xff)
#define GET_COMPONENT_R(rgb) (u8)(((rgb) >> 16) & 0xff)

#define ARR_LEN(arr) (sizeof((arr)) / sizeof((arr[0])))
typedef struct {
	float x;
	float y;
}Vec2;

typedef struct {
void render_axis(SDL_Renderer * renderer, int width, int height, Axis axis){
	int x1 = 0, x2 = 0;
	int y1 = 0, y2 = 0;
	switch(axis.orient){
	case HORIZONTAL:{
		y1 = height /2;
		y2 = y1;
		x2 = width;
		for (int x = 0; x <= width; x += axis.step){
			if (x != width/2)
				SDL_RenderDrawLine(renderer, x, height/2+1, x, height/2-1);
		}

	}break;
	case VERTICAL:{
		x1 = width / 2;
		x2 = x1;
		y2 = height;
		for (int y = 0; y <= height; y += axis.step){
			if (y  != height/2)
				SDL_RenderDrawLine(renderer, width/2+1, y, width/2-1, y);
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
		int alignment_size = r * FONT_SIZE;
		int start_x = x - alignment_size/2 ;//+ FONT_SPRITE_W/2;
		for (int j = 0; j < r; j++){
			int i = temp_buf[j] - 32; // get the index in the sprite
			int rowpx = (i / (FONT_SPRITE_COLS)) * FONT_SPRITE_H;
			int colpx = (i % (FONT_SPRITE_COLS)) * FONT_SPRITE_W;
			SDL_Rect sRect = {.x = colpx+FONT_SPRITE_W/4, .y = rowpx, .w = FONT_SPRITE_W/2, .h = FONT_SPRITE_H};
			SDL_Rect dRect = {.x = start_x, .y = y+2, .w = FONT_SIZE, .h = FONT_SIZE};
			start_x += FONT_SIZE; // spacing
			SDL_RenderCopy(renderer, font_texture, &sRect, &dRect);
		}
	}
}
}

void plot_function(SDL_Renderer * renderer, int width, int height)
{
	SDL_Point* points = malloc(sizeof(SDL_Point)*2048);

	int p_x = -(width)/2;
	int x_pos = 0;
	int count = 0;
	for (int x = p_x; x <= width/2; ++x){
		int f_x = x *-x + 20;
		f_x = (height/2) - f_x;
		SDL_Point p = {.x = x_pos, .y = f_x};
		x_pos++;
		points[count++] = p;
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
	bool quit = false;
	Axis x_axis = {.label = "x", .step = 5,.orient = HORIZONTAL};
	Axis y_axis = {.label = "f(x)", .step = 5 ,.orient = VERTICAL};
	int width = WIDTH;
	int height = HEIGHT;
	while (!quit){
		SDL_Event event = {0};
		while (SDL_PollEvent(&event)){
			switch (event.type){
			case SDL_QUIT:
				quit = true;
				break;
			}
		}
		SDL_SetRenderDrawColor(renderer, GET_COMPONENT_R(BG), GET_COMPONENT_G(BG), GET_COMPONENT_B(BG), 255);
		SDL_RenderClear(renderer);
		SDL_SetRenderDrawColor(renderer, 0xff, 0xcc, 0x8, 255);
		render_axis(renderer, width, height,x_axis);
		render_axis(renderer, width, height,y_axis);
		SDL_SetRenderDrawColor(renderer, 0xff, 0x00,0x00, 255);
		plot_function(renderer, width, height);
		SDL_RenderPresent(renderer);
		SDL_GetWindowSize(win, &width, &height);
	}

	return 0;
}
