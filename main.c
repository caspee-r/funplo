#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <SDL2/SDL.h>


typedef unsigned char u8;

#define WIDTH 800
#define HEIGHT 600

#define BG 0x181818

#define GET_COMPONENT_B(rgb) (u8)((rgb) & 0xff)
#define GET_COMPONENT_G(rgb) (u8)(((rgb) >> 8) & 0xff)
#define GET_COMPONENT_R(rgb) (u8)(((rgb) >> 16) & 0xff)

typedef enum {
	HORIZONTAL = 1,
	VERTICAL,
} ORIENT;

typedef struct {
	char* label;
	unsigned int step;
	ORIENT orient;
}Axis;

char* program_name = "funplot";


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
		}
	}break;
	}
	SDL_RenderDrawLine(renderer, x1, y1, x2, y2);
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
