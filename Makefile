
CFLAGS = `pkg-config --cflags sdl2 freetype2`
LIBS = `pkg-config --libs sdl2 freetype2` -lGL -lm

main: main.c
	cc -ggdb -o main $(CFLAGS) $(LIBS) main.c microui.c
