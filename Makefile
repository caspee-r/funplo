
CFLAGS = `pkg-config --cflags sdl2`
LIBS = `pkg-config --libs sdl2`

main: main.c
	cc -o  main $(CFLAGS) $(LIBS) main.c
