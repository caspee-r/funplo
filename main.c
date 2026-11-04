#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <string.h>
#include <unistd.h>
#include <SDL2/SDL.h>
#include <SDL2/SDL_opengl.h>
#include <math.h>
#include <ft2build.h>
#include FT_FREETYPE_H

#include "microui.h"
#include "desc_font.h"
#define PLANE_IMPLEMENTATION
#include "plane.h"
#define EXPR_IMPLEMENTATION
#include "expr.h"


typedef unsigned char u8;
typedef unsigned int u32;

#define WIDTH 800
#define HEIGHT 600
#define X_AXIS_NUMBER_YOFFSET 2
#define X_AXIS_NUMBER_XOFFSET 2
#define Y_AXIS_NUMBER_XOFFSET 4
#define Y_AXIS_NUMBER_YOFFSET 4
#define DEFAULT_PIXELS_PER_UNIT 100
#define DEFAULT_SCALE 2
#define ZOOM_FACTOR 1.25

// sample the function twice per screen pixel so the curve stays smooth
// at any zoom level
#define CURVE_SUBSAMPLE 2
#define MAX_WINDOW_PIXELS 4096

#define FONT_PIXEL 18      // freetype pixel size, cells stay monospaced
#define FONT_GLYPHS 96     // ascii 32..127

#define BG 0xffffff
#define AXIS_COLOR 0x000000
#define GRID_COLOR 0xd3cecd
#define SUBGRID_COLOR 0xe8e4e4
#define NUMBERS_COLOR 0x182dfc

#define PANEL_WIDTH 250
#define MAX_FUNCS 10
#define CURVE_STRIP_QUOTA 64 // poles and asymptotes cut the strip

#define GET_COMPONENT_B(rgb) (u8)((rgb) & 0xff)
#define GET_COMPONENT_G(rgb) (u8)(((rgb) >> 8) & 0xff)
#define GET_COMPONENT_R(rgb) (u8)(((rgb) >> 16) & 0xff)

#define ARR_LEN(arr) (sizeof((arr)) / sizeof((arr[0])))

static const char* program_name = "funplot";
static char temp_buf[1024] = {0};
static GLuint font_texture = 0;

typedef struct {
	char text[EXPR_TEXT_MAX];
	Expr compiled;
	ExprResult err;
	bool valid;
} FuncEntry;

// each function gets its own color from this palette
static const u32 FUNC_COLORS[] = {
	0xd81b60, // pink red
	0x1e88e5, // blue
	0x43a047, // green
	0xf4511e, // orange
	0x8e24aa, // purple
	0x00acc1, // cyan
	0x5d4037, // brown
	0x9e9d24, // olive
};

static FuncEntry funcs[MAX_FUNCS] = {0};
static int func_count = 0;

static mu_Context mu_ctx;
static CartesianPlane2d plane = {0};
static bool plane_pan = false;
static int mouse_x = 0, mouse_y = 0;

// reusable curve vertex buffers, rebuilt every frame
typedef struct { float x, y; } CurvePoint;
static CurvePoint* curve_points = NULL;
static int curve_strip_starts[CURVE_STRIP_QUOTA];
static int curve_strip_counts[CURVE_STRIP_QUOTA];
static int curve_strip_count = 0;

// ------------------------------------------------------------------
// helpers
// ------------------------------------------------------------------

static u32 color_from(mu_Color c)
{
	return ((u32) c.r << 16) | ((u32) c.g << 8) | (u32) c.b;
}

static void gl_set_color_u32(u32 rgb)
{
	glColor3ub(GET_COMPONENT_R(rgb), GET_COMPONENT_G(rgb), GET_COMPONENT_B(rgb));
}

static void gl_fill_rect(float x, float y, float w, float h, u32 rgb)
{
	gl_set_color_u32(rgb);
	glRectf(x, y, x + w, y + h);
}

// ------------------------------------------------------------------
// font, rasterized with freetype at startup into a single white RGBA
// atlas; advances are uniform because every candidate is monospace,
// glyphs keep their own bearings for crisp positioning
// ------------------------------------------------------------------
typedef struct {
	int x, y;  // top-left corner in the atlas
	int w, h;  // bitmap size
	int bx, fy; // bearing offset from pen x, offset from string top
} FontGlyph;

static FontGlyph font_glyphs[FONT_GLYPHS];
static int font_atlas_w, font_atlas_h;
static int font_advance = 12;
static int font_ascent = 12;
static int font_descent = 0;
static int font_height = 12;

// bake one freetype font into the atlas globals, false if it fails
static bool font_bake_ft(FT_Library ft, const char* path)
{
	FT_Face face = NULL;
	if (FT_New_Face(ft, path, 0, &face)) return false;
	if (FT_Set_Pixel_Sizes(face, 0, FONT_PIXEL)){ FT_Done_Face(face); return false; }

	FT_Size_Metrics m = face->size->metrics;
	font_advance = (int) ceil(m.max_advance / 64.0);
	font_ascent = (int) ceil(m.ascender / 64.0);
	font_descent = (int) ceil(-m.descender / 64.0);
	font_height = font_ascent + font_descent;

	// 1. rasterize every glyph and lay tiles out left to right
	u8* bits[FONT_GLYPHS] = {0};
	int pen = 0, atlash = font_height;
	for (int i = 0; i < FONT_GLYPHS; i++){
		FT_UInt gi = FT_Get_Char_Index(face, 32 + i);
		if (gi == 0 || FT_Load_Glyph(face, gi, FT_LOAD_DEFAULT) || FT_Render_Glyph(face->glyph, FT_RENDER_MODE_NORMAL)){
			// missing/free glyph, blank tile
			font_glyphs[i].w = font_glyphs[i].h = 0;
			continue;
		}
		FT_GlyphSlot g = face->glyph;
		FT_Bitmap* b = &g->bitmap;
		int gw = (int) b->width, gh = (int) b->rows;
		bits[i] = malloc(sizeof(u8) * gw * gh);
		for (int y0 = 0; y0 < gh; y0++) memcpy(&bits[i][y0 * gw], &b->buffer[y0 * b->pitch], gw);
		font_glyphs[i].x = pen;
		font_glyphs[i].y = 0;
		font_glyphs[i].w = gw;
		font_glyphs[i].h = gh;
		font_glyphs[i].bx = g->bitmap_left;
		// freetype positions glyphs around the baseline, strings are
		// addressed by their top edge like the old bitmap font was
		font_glyphs[i].fy = font_ascent - g->bitmap_top;
		if (font_glyphs[i].fy + gh > atlash) atlash = font_glyphs[i].fy + gh;
		pen += gw + 1; // 1px padding between tiles so linear filtering doesn't bleed
	}
	FT_Done_Face(face);

	// 2. expand the alpha map into a white RGBA atlas
	font_atlas_w = pen;
	font_atlas_h = atlash;
	u32* pixels = malloc(sizeof(u32) * font_atlas_w * font_atlas_h);
	memset(pixels, 0, sizeof(u32) * font_atlas_w * font_atlas_h);
	for (int i = 0; i < FONT_GLYPHS; i++){
		if (font_glyphs[i].w == 0){ free(bits[i]); continue; }
		for (int y0 = 0; y0 < font_glyphs[i].h; y0++){
			u32* dst = &pixels[(font_glyphs[i].y + y0) * font_atlas_w + font_glyphs[i].x];
			for (int x0 = 0; x0 < font_glyphs[i].w; x0++){
				u8 a = bits[i][y0 * font_glyphs[i].w + x0];
				// white texel, alpha carries the coverage (RGBA memory order)
				dst[x0] = 0x00ffffffu | ((u32) a << 24);
			}
		}
		free(bits[i]);
	}

	GLuint font;
	glGenTextures(1, &font);
	font_texture = font;
	glBindTexture(GL_TEXTURE_2D, font_texture);
	glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, font_atlas_w, font_atlas_h, 0, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
	free(pixels);
	if (getenv("FUNPLO_DEBUG")) fprintf(stderr, "[debug]: font '%s' advance=%d ascent=%d descent=%d height=%d atlas=%dx%d\n",
									   path, font_advance, font_ascent, font_descent, font_height, font_atlas_w, font_atlas_h);
	return true;
}

// last resort: build glyph tiles from the embedded bitmap font
static bool font_bake_bitmap(void)
{
	int sw = desc_font_width / 10, sh = desc_font_height / 10;
	font_advance = 12;
	font_ascent = 12;
	font_descent = 0;
	font_height = 12;
	for (int i = 0; i < FONT_GLYPHS; i++){
		int rowpx = (i / 10) * sh, colpx = (i % 10) * sw;
		font_glyphs[i].x = colpx + sw / 4; // same crop as the old renderer
		font_glyphs[i].y = rowpx;
		font_glyphs[i].w = sw / 2;         // 30px wide crop displayed at 12px
		font_glyphs[i].h = sh;
		font_glyphs[i].bx = 0;
		font_glyphs[i].fy = 0;
	}
	font_atlas_w = desc_font_width;
	font_atlas_h = desc_font_height;
	GLuint font;
	glGenTextures(1, &font);
	font_texture = font;
	glBindTexture(GL_TEXTURE_2D, font_texture);
	glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, font_atlas_w, font_atlas_h, 0, GL_RGBA, GL_UNSIGNED_BYTE, desc_font_data);
	return true;
}

static bool font_bake_any(void)
{
	FT_Library ft = NULL;
	if (FT_Init_FreeType(&ft)) return false;
	// prefer the user's monospace fonts; anything with a uniform advance
	// works, the layout only ever assumes fixed cells
	static const char* candidates[] = {
		"/usr/share/fonts/TTF/JetBrainsMonoNerdFontMono-Regular.ttf",
		"/usr/share/fonts/truetype/jetbrains/JetBrainsMono-Regular.ttf",
		"/usr/share/fonts/noto/NotoSansMono-Regular.ttf",
		"/usr/share/fonts/truetype/dejavu/DejaVuSansMono.ttf",
		"/usr/share/fonts/TTF/DejaVuSansMono.ttf",
		"/usr/share/fonts/TTF/liberation/LiberationMono-Regular.ttf",
		"/usr/share/fonts/TTF/liberation-mono/LiberationMono-Regular.ttf",
	};
	bool ok = false;
	for (size_t i = 0; !ok && i < ARR_LEN(candidates); i++){
		if (access(candidates[i], R_OK) != 0) continue;
		ok = font_bake_ft(ft, candidates[i]);
	}
	// ask the system what it thinks monospace is, then give up to the
	// embedded bitmap font
	if (!ok && getenv("FUNPLO_FONT") && getenv("FUNPLO_FONT")[0]) ok = font_bake_ft(ft, getenv("FUNPLO_FONT"));
	if (!ok){
		FILE* fc = popen("fc-match -f '%{file}' 'JetBrains Mono'", "r");
		if (fc){
			char path[512] = {0};
			if (fgets(path, sizeof(path), fc)){
				path[strcspn(path, "\n")] = 0;
				ok = font_bake_ft(ft, path);
			}
			pclose(fc);
		}
	}
	FT_Done_FreeType(ft);
	if (!ok) ok = font_bake_bitmap();
	return ok;
}

// render a string with the baked font, strings are top-addressed
static void gl_draw_text(int x, int y, const char* str, int len, mu_Color color)
{
	if (len < 0) len = (int) strlen(str);
	glEnable(GL_TEXTURE_2D);
	glBindTexture(GL_TEXTURE_2D, font_texture);
	glColor4ub(color.r, color.g, color.b, color.a);
	glBegin(GL_QUADS);
	for (int j = 0; j < len; j++){
		unsigned char ch = (unsigned char) str[j];
		if (ch >= 32 && ch < 128){
			const FontGlyph* fg = &font_glyphs[ch - 32];
			if (fg->w > 0){
				float u0 = (float) fg->x / (float) font_atlas_w;
				float u1 = (float)(fg->x + fg->w) / (float) font_atlas_w;
				float v0 = (float) fg->y / (float) font_atlas_h;
				float v1 = (float)(fg->y + fg->h) / (float) font_atlas_h;
				int px = x + fg->bx;
				int py = y + fg->fy;
				glTexCoord2f(u0, v0); glVertex2i(px, py);
				glTexCoord2f(u1, v0); glVertex2i(px + fg->w, py);
				glTexCoord2f(u1, v1); glVertex2i(px + fg->w, py + fg->h);
				glTexCoord2f(u0, v1); glVertex2i(px, py + fg->h);
			}
		}
		x += font_advance;
	}
	glEnd();
	glDisable(GL_TEXTURE_2D);
}

static void gl_draw_text_u32(int x, int y, const char* str, int len, u32 rgb)
{
	gl_draw_text(x, y, str, len, mu_color(GET_COMPONENT_R(rgb), GET_COMPONENT_G(rgb), GET_COMPONENT_B(rgb), 255));
}

static void render_number(int num, int x, int y)
{
	int r = snprintf(temp_buf, ARR_LEN(temp_buf), "%d", num);
	if (r > 0) gl_draw_text_u32(x, y, temp_buf, r, NUMBERS_COLOR);
}

//NOTE(caspeer): zooming moves the grid step over many orders of
// magnitude, plain digits would balloon ("0.00390625", "262144") and
// clutter the axes; switch to scientific notation outside the range
// where plain digits stay short
static void fmt_number(double v, char* buf, size_t n)
{
	if (v == 0){ snprintf(buf, n, "0"); return; }
	if (!isfinite(v)){ snprintf(buf, n, "-"); return; }
	double mag = fabs(v);
	if (mag >= 1e-2 && mag < 1e6){
		snprintf(buf, n, "%.6g", v); // %g drops trailing zeros
		return;
	}
	// scientific without exponent padding ("1.02e-5" instead of
	// "1.0200000000000000e-05"), keeps labels the same visual size
	int exp10 = (int) floor(log10(mag));
	double mant = v / pow(10, exp10);
	snprintf(buf, n, "%.3ge%d", mant, exp10);
}

// ------------------------------------------------------------------
// micro ui
// ------------------------------------------------------------------

//NOTE(caspeer): the baked font is monospaced, one advance per glyph,
// so the layout math keeps treating text as a grid of fixed cells
static int mu_font_text_width(mu_Font font, const char* str, int len)
{
	(void) font;
	if (len < 0) len = strlen(str);
	return len * font_advance;
}

static int mu_font_text_height(mu_Font font)
{
	(void) font;
	return font_height;
}

static void mu_frame_callback(mu_Context* ctx, mu_Rect rect, int colorid)
{
	mu_draw_rect(ctx, rect, ctx->style->colors[colorid]);
}

static void mu_setup(void)
{
	mu_init(&mu_ctx);
	mu_ctx.text_width = mu_font_text_width;
	mu_ctx.text_height = mu_font_text_height;
	mu_ctx.draw_frame = mu_frame_callback;
	mu_ctx.style->size = mu_vec2(font_advance, font_height + 1);
	mu_ctx.style->title_height = 26;
	mu_ctx.style->scrollbar_size = 10;
	mu_ctx.style->thumb_size = 8;
	// light theme to match the white plotting background
	mu_ctx.style->colors[MU_COLOR_TEXT]        = mu_color(48, 52, 56, 255);
	mu_ctx.style->colors[MU_COLOR_BORDER]      = mu_color(180, 190, 195, 255);
	mu_ctx.style->colors[MU_COLOR_WINDOWBG]    = mu_color(243, 246, 247, 255);
	mu_ctx.style->colors[MU_COLOR_TITLEBG]     = mu_color(222, 229, 232, 255);
	mu_ctx.style->colors[MU_COLOR_TITLETEXT]   = mu_color(48, 52, 56, 255);
	mu_ctx.style->colors[MU_COLOR_PANELBG]     = mu_color(255, 255, 255, 255);
	mu_ctx.style->colors[MU_COLOR_BUTTON]      = mu_color(223, 230, 233, 255);
	mu_ctx.style->colors[MU_COLOR_BUTTONHOVER] = mu_color(208, 218, 223, 255);
	mu_ctx.style->colors[MU_COLOR_BUTTONFOCUS] = mu_color(190, 203, 210, 255);
	mu_ctx.style->colors[MU_COLOR_BASE]        = mu_color(255, 255, 255, 255);
	mu_ctx.style->colors[MU_COLOR_BASEHOVER]   = mu_color(235, 242, 245, 255);
	mu_ctx.style->colors[MU_COLOR_BASEFOCUS]   = mu_color(215, 227, 234, 255);
	mu_ctx.style->colors[MU_COLOR_SCROLLBASE]  = mu_color(208, 216, 220, 255);
	mu_ctx.style->colors[MU_COLOR_SCROLLTHUMB] = mu_color(160, 172, 180, 255);
}

static bool ui_mouse_over_panel(int x, int y)
{
	mu_Container* c = mu_get_container(&mu_ctx, "funplot");
	if (!c) return false;
	return x >= c->rect.x && x < c->rect.x + c->rect.w
		&& y >= c->rect.y && y < c->rect.y + c->rect.h;
}

// ------------------------------------------------------------------
// microui command rendering
// ------------------------------------------------------------------

static void render_ui_display(int width, int height)
{
	(void) width;
	mu_Command* cmd = NULL;
	glEnable(GL_SCISSOR_TEST);
	while (mu_next_command(&mu_ctx, &cmd)){
		switch (cmd->type){
		case MU_COMMAND_RECT:{
			gl_fill_rect(cmd->rect.rect.x, cmd->rect.rect.y,
						 cmd->rect.rect.w, cmd->rect.rect.h,
						 color_from(cmd->rect.color));
		} break;
		case MU_COMMAND_TEXT:{
			gl_draw_text(cmd->text.pos.x, cmd->text.pos.y, cmd->text.str, -1, cmd->text.color);
		} break;
		case MU_COMMAND_CLIP:{
			glScissor(cmd->clip.rect.x, height - (cmd->clip.rect.y + cmd->clip.rect.h),
					  cmd->clip.rect.w, cmd->clip.rect.h);
		} break;
		case MU_COMMAND_ICON:
			//NOTE(caspeer): the panel uses no icons (no close button, no
			// treenodes, no checkboxes) so drawing nothing is fine
			break;
		}
	}
	glDisable(GL_SCISSOR_TEST);
}

// ------------------------------------------------------------------
// plotting
// ------------------------------------------------------------------

// can a grid label of this text fit next to a tick with this stride?
static bool axis_label_fits(const char* str, float stride)
{
	return (float)(font_advance * (int) strlen(str)) + 2.f * X_AXIS_NUMBER_XOFFSET <= stride;
}

void render_cartesianplane2d(int width, int height)
{
	float oxf = plane.ox * (float) width;
	float oyf = plane.oy * (float) height;
	float unit = (float) plane.pixels_per_unit;
	float step = plane.scale;

	// 1. sub grid, one faint line at every half unit
	gl_set_color_u32(SUBGRID_COLOR);
	glBegin(GL_LINES);
	for (int m = 1;; m += 2){
		float px = oxf + (float) m * unit * 0.5f;
		if (px > width) break;
		glVertex2f(px, 0); glVertex2f(px, height);
	}
	for (int m = -1;; m -= 2){
		float px = oxf + (float) m * unit * 0.5f;
		if (px < 0) break;
		glVertex2f(px, 0); glVertex2f(px, height);
	}
	for (int m = 1;; m += 2){
		float py = oyf - (float) m * unit * 0.5f;
		if (py < 0) break;
		glVertex2f(0, py); glVertex2f(width, py);
	}
	for (int m = -1;; m -= 2){
		float py = oyf - (float) m * unit * 0.5f;
		if (py > height) break;
		glVertex2f(0, py); glVertex2f(width, py);
	}
	glEnd();

	// 2. main grid, one line per unit
	gl_set_color_u32(GRID_COLOR);
	glBegin(GL_LINES);
	for (int k = 1;; k++){
		float px = oxf + (float) k * unit;
		if (px > width) break;
		glVertex2f(px, 0); glVertex2f(px, height);
	}
	for (int k = -1;; k--){
		float px = oxf + (float) k * unit;
		if (px < 0) break;
		glVertex2f(px, 0); glVertex2f(px, height);
	}
	for (int k = 1;; k++){
		float py = oyf - (float) k * unit;
		if (py < 0) break;
		glVertex2f(0, py); glVertex2f(width, py);
	}
	for (int k = -1;; k--){
		float py = oyf - (float) k * unit;
		if (py > height) break;
		glVertex2f(0, py); glVertex2f(width, py);
	}
	glEnd();

	// 3. the axes themselves
	gl_set_color_u32(AXIS_COLOR);
	glBegin(GL_LINES);
	glVertex2f(0, oyf); glVertex2f(width, oyf);
	glVertex2f(oxf, 0); glVertex2f(oxf, height);
	glEnd();

	// 4. grid numbers, fixed size and fixed offsets so they never
	// "shrink" while zooming, values adapt instead
	// long scientific labels can outgrow the step stride, in that case
	// only alternate ticks are labeled so neighbors never collide
	// the lone origin "0" is skipped whenever the first label to its
	// left would bleed over the axis and collide with it
	bool draw_zero = true;
	fmt_number(-step, temp_buf, ARR_LEN(temp_buf));
	if (oxf - unit + X_AXIS_NUMBER_XOFFSET + (float) strlen(temp_buf) * font_advance > oxf + font_advance){
		draw_zero = false;
	}
	if (draw_zero) render_number(0, (int) oxf + font_advance, (int) oyf + X_AXIS_NUMBER_YOFFSET);
	for (int k = 1;; k++){
		float px = oxf + (float) k * unit;
		if (px > width) break;
		fmt_number((double) k * step, temp_buf, ARR_LEN(temp_buf));
		if (axis_label_fits(temp_buf, unit) || (axis_label_fits(temp_buf, unit * 2.f) && (k & 1))){
			gl_draw_text_u32((int) px + X_AXIS_NUMBER_XOFFSET, (int) oyf + X_AXIS_NUMBER_YOFFSET, temp_buf, -1, NUMBERS_COLOR);
		}
	}
	for (int k = -1;; k--){
		float px = oxf + (float) k * unit;
		if (px < 0) break;
		fmt_number((double) k * step, temp_buf, ARR_LEN(temp_buf));
		if (axis_label_fits(temp_buf, unit) || (axis_label_fits(temp_buf, unit * 2.f) && (k & 1))){
			gl_draw_text_u32((int) px + X_AXIS_NUMBER_XOFFSET, (int) oyf + X_AXIS_NUMBER_YOFFSET, temp_buf, -1, NUMBERS_COLOR);
		}
	}
	for (int k = 1;; k++){
		float py = oyf - (float) k * unit;
		if (py < 0) break;
		fmt_number((double) k * step, temp_buf, ARR_LEN(temp_buf));
		gl_draw_text_u32((int) oxf + Y_AXIS_NUMBER_XOFFSET, (int) py + Y_AXIS_NUMBER_YOFFSET, temp_buf, -1, NUMBERS_COLOR);
	}
	for (int k = -1;; k--){
		float py = oyf - (float) k * unit;
		if (py > height) break;
		fmt_number((double) k * step, temp_buf, ARR_LEN(temp_buf));
		gl_draw_text_u32((int) oxf + Y_AXIS_NUMBER_XOFFSET, (int) py + Y_AXIS_NUMBER_YOFFSET, temp_buf, -1, NUMBERS_COLOR);
	}
}

//NOTE(caspeer): curves are broken into strips wherever the function is
// undefined (NaN/inf) or way off screen (poles, asymptotes), which keeps
// tan(x) & co. from being connected with vertical lines
static void render_function_curve(FuncEntry* f, int width, int height, int func_index)
{
	const Expr* e = &f->compiled;
	float oxf = plane.ox * (float) width;
	float oyf = plane.oy * (float) height;
	float y_margin = 3.5f * (float) height;
	int max_n = mu_min(width * CURVE_SUBSAMPLE, MAX_WINDOW_PIXELS * CURVE_SUBSAMPLE) + 2;
	int vcount = 0;
	curve_strip_count = 0;
	bool in_strip = false;
	int strip_start = 0;
	float prev_py = 0.f;

	for (int i = 0; i < max_n; i++){
		float px = (float) i / (float) CURVE_SUBSAMPLE;
		double vx = ((px - oxf) / plane.pixels_per_unit) * plane.scale;
		double vy = expr_eval(e, vx);
		if (!isfinite(vy)){
			if (in_strip){
				if (curve_strip_count < CURVE_STRIP_QUOTA){
					curve_strip_starts[curve_strip_count] = strip_start;
					curve_strip_counts[curve_strip_count] = vcount - strip_start;
					curve_strip_count++;
				}
				in_strip = false;
			}
			continue;
		}
		float py = oyf - (float)(vy / plane.scale * plane.pixels_per_unit);
		if (py < -y_margin || py > height + y_margin){
			if (in_strip){
				if (curve_strip_count < CURVE_STRIP_QUOTA){
					curve_strip_starts[curve_strip_count] = strip_start;
					curve_strip_counts[curve_strip_count] = vcount - strip_start;
					curve_strip_count++;
				}
				in_strip = false;
			}
			continue;
		}
		if (in_strip && fabsf(py - prev_py) > 2.f * y_margin){
			if (curve_strip_count < CURVE_STRIP_QUOTA){
				curve_strip_starts[curve_strip_count] = strip_start;
				curve_strip_counts[curve_strip_count] = vcount - strip_start;
				curve_strip_count++;
			}
			in_strip = false;
		}
		if (!in_strip){
			strip_start = vcount;
			in_strip = true;
		}
		curve_points[vcount].x = px;
		curve_points[vcount].y = py;
		vcount++;
		prev_py = py;
	}
	if (in_strip){
		if (curve_strip_count < CURVE_STRIP_QUOTA){
			curve_strip_starts[curve_strip_count] = strip_start;
			curve_strip_counts[curve_strip_count] = vcount - strip_start;
			curve_strip_count++;
		}
	}

	gl_set_color_u32(FUNC_COLORS[func_index % ARR_LEN(FUNC_COLORS)]);
	glLineWidth(2.25f);
	// MSAA is enabled on the context, so GL_LINE_STRIP gives us
	// antialiased curves straight from the GPU
	glBegin(GL_LINE_STRIP);
	for (int s = 0; s < curve_strip_count; s++){
		if (s > 0) glEnd(), glBegin(GL_LINE_STRIP);
		for (int i = 0; i < curve_strip_counts[s]; i++){
			const CurvePoint* p = &curve_points[curve_strip_starts[s] + i];
			glVertex2f(p->x, p->y);
		}
	}
	glEnd();
}

// ------------------------------------------------------------------
// the side panel
// ------------------------------------------------------------------

static void compile_func(FuncEntry* f)
{
	f->err = expr_compile(f->text, &f->compiled);
	f->valid = f->err == EXPR_OK;
}

static void add_func(const char* text)
{
	if (func_count >= MAX_FUNCS) return;
	FuncEntry* f = &funcs[func_count++];
	snprintf(f->text, ARR_LEN(f->text), "%s", text);
	compile_func(f);
}

static void remove_func(int index)
{
	for (int i = index + 1; i < func_count; i++) funcs[i - 1] = funcs[i];
	func_count--;
	if (getenv("FUNPLO_DEBUG")) fprintf(stderr, "[debug]: remove_func(%d) -> count %d\n", index, func_count);
}

static void render_ui_panel(int width, int height)
{
	char buf[64];
	//NOTE(caspeer): microui keeps the widths pointer in its layout, so these
	// must not be throwaway compound literals, give them static storage
	static const int w_fill[] = { -1 };
	//NOTE(caspeer): a -1 width expands all the way to the body edge, ignoring
	// any items after it in the row; give the textbox "-(button + spacing + 1)"
	// so the trailing "x" button stays inside the panel without scrollbars
	static const int w_row4[] = { 12, 36, -(22 + 4 + 1), 22 };
	static const int w_stats[] = { 110, -1 };
	// vertical room reserved under the functions list for the stats section
	static const int STATS_RESERVE = 245;

	mu_Rect winr = mu_rect(mu_max(10, width - PANEL_WIDTH - 10), 10, PANEL_WIDTH, mu_max(80, height - 20));
	mu_begin_window_ex(&mu_ctx, "funplot", winr, MU_OPT_NORESIZE | MU_OPT_NOCLOSE);

	// functions list, scrollable in case there are a lot of them, keep
	// vertical room at the bottom for the stats section
	mu_layout_row(&mu_ctx, 1, w_fill, -STATS_RESERVE);
	mu_begin_panel(&mu_ctx, "functions");
	{
		// at most one removal per frame: removing AND reindexing inside
		// the loop would recycle the removed row's id while the mouse
		// press is still latched, firing the same button twice
		int pending_remove = -1;
		for (int i = 0; i < func_count; i++){
			FuncEntry* f = &funcs[i];
			// rows rehash per index: without this, every row's "x" label id
			// collides and one click removes all functions
			mu_push_id(&mu_ctx, &i, sizeof(i));
			mu_layout_row(&mu_ctx, 4, w_row4, 0);
			// color swatch, doubles as the color legend for the curve
			mu_Rect swr = mu_layout_next(&mu_ctx);
			mu_Rect cb = mu_rect(swr.x + (swr.w - 12) / 2, swr.y + (swr.h - 12) / 2, 12, 12);
			mu_draw_rect(&mu_ctx, cb, mu_color(GET_COMPONENT_R(FUNC_COLORS[i % ARR_LEN(FUNC_COLORS)]),
											   GET_COMPONENT_G(FUNC_COLORS[i % ARR_LEN(FUNC_COLORS)]),
											   GET_COMPONENT_B(FUNC_COLORS[i % ARR_LEN(FUNC_COLORS)]), 255));
			mu_label(&mu_ctx, "y =");
			if (mu_textbox(&mu_ctx, f->text, EXPR_TEXT_MAX) & MU_RES_CHANGE){
				compile_func(f);
				if (getenv("FUNPLO_DEBUG")) fprintf(stderr, "[debug]: textbox %d changed to '%s'\n", i, f->text);
			}
			if (mu_button_ex(&mu_ctx, "x", 0, MU_OPT_ALIGNCENTER)){
				pending_remove = i;
			}
			mu_pop_id(&mu_ctx);
			if (f->text[0] != 0 && !f->valid){
				// expression does not compile, show the error in red
				mu_layout_row(&mu_ctx, 1, w_fill, 0);
				mu_Color prev = mu_ctx.style->colors[MU_COLOR_TEXT];
				mu_ctx.style->colors[MU_COLOR_TEXT] = mu_color(214, 48, 49, 255);
				mu_label(&mu_ctx, expr_error_string(f->err));
				mu_ctx.style->colors[MU_COLOR_TEXT] = prev;
			}
		}
		mu_layout_row(&mu_ctx, 1, w_fill, 0);
		if (func_count < MAX_FUNCS && mu_button(&mu_ctx, "+ add function")){
			add_func("");
		}
		mu_end_panel(&mu_ctx);
		if (pending_remove >= 0) remove_func(pending_remove);
	}

	// stats
	mu_layout_row(&mu_ctx, 2, w_stats, 0);
	mu_label(&mu_ctx, "grid step");
	fmt_number(plane.scale, buf, ARR_LEN(buf));
	mu_label(&mu_ctx, buf);
	mu_label(&mu_ctx, "px/step");
	fmt_number(plane.pixels_per_unit, buf, ARR_LEN(buf));
	mu_label(&mu_ctx, buf);
	double wx = ((mouse_x - plane.ox * width) / plane.pixels_per_unit) * plane.scale;
	double wy = ((plane.oy * height - mouse_y) / plane.pixels_per_unit) * plane.scale;
	mu_label(&mu_ctx, "cursor x");
	fmt_number(wx, buf, ARR_LEN(buf));
	mu_label(&mu_ctx, buf);
	mu_label(&mu_ctx, "cursor y");
	fmt_number(wy, buf, ARR_LEN(buf));
	mu_label(&mu_ctx, buf);

	// reset view
	mu_layout_row(&mu_ctx, 1, w_fill, 0);
	if (mu_button(&mu_ctx, "reset view")){
		plane.ox = plane.oy = 0.5;
		plane.pixels_per_unit = DEFAULT_PIXELS_PER_UNIT;
		plane.scale = DEFAULT_SCALE;
	}
	mu_label(&mu_ctx, "drag=pan wheel=zoom");

	mu_end_window(&mu_ctx);
}

// ------------------------------------------------------------------
// input
// ------------------------------------------------------------------

static void forward_key_event(const SDL_KeyboardEvent* key)
{
	int mods = 0;
	if (key->keysym.mod & KMOD_SHIFT) mods |= MU_KEY_SHIFT;
	if (key->keysym.mod & KMOD_CTRL)  mods |= MU_KEY_CTRL;
	if (key->keysym.mod & KMOD_ALT)   mods |= MU_KEY_ALT;
	if (key->keysym.sym == SDLK_BACKSPACE) mods |= MU_KEY_BACKSPACE;
	if (key->keysym.sym == SDLK_RETURN) mods |= MU_KEY_RETURN;
	if (key->type == SDL_KEYDOWN) mu_input_keydown(&mu_ctx, mods);
	else mu_input_keyup(&mu_ctx, mods);
}

// ------------------------------------------------------------------
// headless test hook (dev only, not used in normal runs): feed a
// script file of synthetic SDL events through the regular loop so
// interactions can be verified under a virtual display.
//
//   m <x> <y>        move the mouse (absolute)
//   d <x> <y>        press and hold the left button
//   c <x> <y>        click the left button
//   u                release the left button
//   w <n>            wheel the mouse n ticks (negative = zoom in)
//   t <text>         type text into the current widget
//   S <name>         save FUNPLO_SHOT_DIR/<name>.bmp at this point
//   q                quit
// ------------------------------------------------------------------
static char shot_full_path[512] = {0};
static bool request_shot = false;
static int script_mouse_x = 400, script_mouse_y = 300;
// the script pushes at most one tagged event per frame and the main loop
// consumes the queue before rendering, one static tag buffer is enough
static char user_tag[128] = {0};
static char shot_dir[384] = "/tmp/opencode";
// the script file is consumed line by line, one line per frame, so event
// timings line up with frames of the actual ui
static FILE* script_file = NULL;

static void save_screenshot(const char* path, int width, int height)
{
	size_t size = (size_t) width * height * 3;
	u8* px = malloc(size);
	u8* flipped = malloc(size);
	glReadPixels(0, 0, width, height, GL_RGB, GL_UNSIGNED_BYTE, px);
	for (int y = 0; y < height; y++){
		memcpy(flipped + (size_t) y * width * 3, px + (size_t)(height - 1 - y) * width * 3, width * 3);
	}
	SDL_Surface* s = SDL_CreateRGBSurfaceFrom(flipped, width, height, 24, width * 3, 0xff, 0xff00, 0xff0000, 0);
	SDL_SaveBMP(s, path);
	SDL_FreeSurface(s);
	free(px);
	free(flipped);
	fprintf(stderr, "[script]: saved %s\n", path);
}

static void push_motion(int x, int y)
{
	SDL_Event e = {0};
	e.type = SDL_MOUSEMOTION;
	e.motion.x = x;
	e.motion.y = y;
	e.motion.xrel = x - script_mouse_x;
	e.motion.yrel = y - script_mouse_y;
	SDL_PushEvent(&e);
	script_mouse_x = x;
	script_mouse_y = y;
}

static void push_button(bool down, int x, int y)
{
	SDL_Event e = {0};
	e.type = down ? SDL_MOUSEBUTTONDOWN : SDL_MOUSEBUTTONUP;
	e.button.button = SDL_BUTTON_LEFT;
	e.button.x = x;
	e.button.y = y;
	SDL_PushEvent(&e);
}

// clicks span two frames in microui (down sets focus, up releases), so
// this only moves + presses; the script must send "u" to release
static void push_click(int x, int y)
{
	push_motion(x, y);
	push_button(true, x, y);
}

static void push_script_event(char* line)
{
	// strip the trailing newline
	line[strcspn(line, "\n")] = 0;
	if (getenv("FUNPLO_DEBUG")) fprintf(stderr, "[debug]: script: '%s'\n", line);
	int x, y, n;
	char text[128];
	char tag[128];
	SDL_Event e = {0};
	if (sscanf(line, "m %d %d", &x, &y) == 2){
		push_motion(x, y);
	}else if (line[0] == 'n' && line[1] == 0){
		// wait one frame, lets microui latch hover after a motion
	}else if (sscanf(line, "d %d %d", &x, &y) == 2){
		push_motion(x, y);
		push_button(true, x, y);
	}else if (sscanf(line, "c %d %d", &x, &y) == 2){
		push_click(x, y);
	}else if (line[0] == 'p'){
		// press at the current position without moving, so microui can
		// latch hover on the previous frame first
		push_button(true, script_mouse_x, script_mouse_y);
	}else if (line[0] == 'u'){
		push_button(false, script_mouse_x, script_mouse_y);
	}else if (sscanf(line, "w %d", &n) == 1){
		//NOTE(caspeer): pushing wheel events through the SDL2/SDL3 compat
		// shim garbles the event union, apply the zoom directly instead
		SDL_Event z = {0};
		z.type = SDL_USEREVENT;
		z.user.code = 3;
		z.user.data1 = (void*) (intptr_t) n;
		SDL_PushEvent(&z);
	}else if (sscanf(line, "t %127[^\n]", text) == 1){
		//NOTE(caspeer): pushing synthetic SDL_TEXTINPUT events crashes in
		// SDL, route the text through our own user event instead
		snprintf(user_tag, ARR_LEN(user_tag), "%s", text);
		SDL_Event tx = {0};
		tx.type = SDL_USEREVENT;
		tx.user.code = 2;
		tx.user.data1 = user_tag;
		SDL_PushEvent(&tx);
	}else if (sscanf(line, "S %127s", tag) == 1){
		snprintf(user_tag, ARR_LEN(user_tag), "%s", tag);
		e.type = SDL_USEREVENT;
		e.user.code = 1;
		e.user.data1 = user_tag;
		SDL_PushEvent(&e);
	}else if (line[0] == 'q'){
		e.type = SDL_QUIT;
		SDL_PushEvent(&e);
	}
}

static void queue_script_events(const char* path)
{
	FILE* f = fopen(path, "r");
	if (!f){
		fprintf(stderr, "[script]: can't open %s\n", path);
		return;
	}
	script_file = f;
}

int main()
{
	if (SDL_Init(SDL_INIT_VIDEO) < 0){
		fprintf(stderr, "[Fatal]: Can't Initialize the program\ndue to: %s", SDL_GetError());
		exit(1);
	}
	SDL_GL_SetAttribute(SDL_GL_MULTISAMPLEBUFFERS, 1);
	SDL_GL_SetAttribute(SDL_GL_MULTISAMPLESAMPLES, 4);
	SDL_Window* win = SDL_CreateWindow(program_name, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, WIDTH, HEIGHT,
									   SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE);
	SDL_GLContext gl = SDL_GL_CreateContext(win);
	if (win == NULL || gl == NULL){
		fprintf(stderr, "[Fatal]: Can't Open the window\ndue to: %s", SDL_GetError());
		exit(1);
	}
	SDL_GL_SetSwapInterval(getenv("FUNPLO_NOVSYNC") ? 0 : 1);
	glEnable(GL_MULTISAMPLE);
	glEnable(GL_BLEND);
	glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

	// bake the text font into an atlas before any text is drawn
	if (!font_bake_any()){
		fprintf(stderr, "[Warning]: couldn't load any font, no text will be shown");
	}

	// reusable GPU upload buffers
	curve_points = malloc(sizeof(CurvePoint) * (MAX_WINDOW_PIXELS * CURVE_SUBSAMPLE + 8));

	plane.ox = 0.5;
	plane.oy = 0.5;
	plane.pixels_per_unit = DEFAULT_PIXELS_PER_UNIT;
	plane.scale = DEFAULT_SCALE;

	add_func("x^2 - 2");
	add_func("sin(x)");

	mu_setup();
	SDL_StartTextInput();

	const char* screenshot_path = getenv("FUNPLO_SCREENSHOT");
	bool debug_frames = getenv("FUNPLO_DEBUG") != NULL;
	const char* script_path = getenv("FUNPLO_SCRIPT");
	if (getenv("FUNPLO_SHOT_DIR")) snprintf(shot_dir, sizeof(shot_dir), "%s", getenv("FUNPLO_SHOT_DIR"));
	if (script_path) queue_script_events(script_path);
	int frames = 0;
	bool quit = false;
	int width = WIDTH, height = HEIGHT;
	SDL_GetWindowSize(win, &width, &height);

	// Event Loop
	while (!quit){
		if (debug_frames) fprintf(stderr, "[debug]: frame %d: start\n", frames);
		// feed the test script at one line per frame so every queued
		// event lands on a frame where the ui actually exists
		if (script_file){
			char line[256];
			if (fgets(line, sizeof(line), script_file)) push_script_event(line);
			else { fclose(script_file); script_file = NULL; }
		}
		SDL_Event event = {0};
		while (SDL_PollEvent(&event)){
			switch (event.type){
			case SDL_QUIT:{
				quit = true;
			} break;
			case SDL_WINDOWEVENT:{
				if (event.window.event == SDL_WINDOWEVENT_CLOSE) quit = true;
			} break;
			case SDL_MOUSEMOTION:{
				if (debug_frames) fprintf(stderr, "[debug]: motion at (%d,%d)\n", event.motion.x, event.motion.y);
				mu_input_mousemove(&mu_ctx, event.motion.x, event.motion.y);
				if (plane_pan){
					plane.ox += event.motion.xrel / (float) width;
					plane.oy += event.motion.yrel / (float) height;
				}
			} break;
			case SDL_MOUSEBUTTONDOWN:{
				if (debug_frames) fprintf(stderr, "[debug]: btn down b=%d at (%d,%d)\n", event.button.button, event.button.x, event.button.y);
				mu_input_mousedown(&mu_ctx, event.button.x, event.button.y, 1 << (event.button.button - 1));
				if (event.button.button == SDL_BUTTON_LEFT && !ui_mouse_over_panel(event.button.x, event.button.y)){
					plane_pan = true;
				}
			} break;
			case SDL_MOUSEBUTTONUP:{
				mu_input_mouseup(&mu_ctx, event.button.x, event.button.y, 1 << (event.button.button - 1));
				if (event.button.button == SDL_BUTTON_LEFT) plane_pan = false;
			} break;
			case SDL_MOUSEWHEEL:{
				// let the panel have the wheel when it is under the cursor
				if (!ui_mouse_over_panel(event.wheel.mouseX, event.wheel.mouseY)){
					plane_zoom(&plane, pow(ZOOM_FACTOR, event.wheel.y), event.wheel.mouseX, event.wheel.mouseY, width, height);
				}
				mu_input_scroll(&mu_ctx, event.wheel.x * -30, event.wheel.y * -30);
			} break;
			case SDL_TEXTINPUT:{
				mu_input_text(&mu_ctx, event.text.text);
			} break;
			case SDL_KEYDOWN:
			case SDL_KEYUP:{
				forward_key_event(&event.key);
			} break;
			case SDL_USEREVENT:{
				if (event.user.code == 1){
					// screenshot request queued by the test script
					snprintf(shot_full_path, ARR_LEN(shot_full_path), "%s/%s.bmp", shot_dir, (char*) event.user.data1);
					request_shot = true;
				}else if (event.user.code == 2){
					mu_input_text(&mu_ctx, (char*) event.user.data1);
				}else if (event.user.code == 3){
					// zoom ticks from the test script
					int n = (int)(intptr_t) event.user.data1;
					int sign = n > 0 ? 1 : -1;
					for (int i = 0; i < (n < 0 ? -n : n); i++){
						plane_zoom(&plane, pow(ZOOM_FACTOR, sign), script_mouse_x, script_mouse_y, width, height);
					}
				}
				//NOTE(caspeer): do not free data1, it either points at the
				// static tag or is a plain integer
			} break;
			}
		}

		SDL_GetMouseState(&mouse_x, &mouse_y);
		SDL_GetWindowSize(win, &width, &height);

		glEnable(GL_MULTISAMPLE);
		glViewport(0, 0, width, height);
		glMatrixMode(GL_PROJECTION);
		glLoadIdentity();
		glOrtho(0, width, height, 0, -1, 1); // y-down, like SDL
		glMatrixMode(GL_MODELVIEW);
		glLoadIdentity();
		glClearColor(GET_COMPONENT_R(BG) / 255.f, GET_COMPONENT_G(BG) / 255.f, GET_COMPONENT_B(BG) / 255.f, 1.f);
		glClear(GL_COLOR_BUFFER_BIT);

		render_cartesianplane2d(width, height);
		if (debug_frames) fprintf(stderr, "[debug]: frame %d: plane done\n", frames);
		for (int i = 0; i < func_count; i++){
			if (funcs[i].valid && funcs[i].text[0] != 0){
				render_function_curve(&funcs[i], width, height, i);
			}
		}

		mu_begin(&mu_ctx);
		render_ui_panel(width, height);
		mu_end(&mu_ctx);
		if (debug_frames) fprintf(stderr, "[debug]: frame %d: ui built\n", frames);
		render_ui_display(width, height);
		if (debug_frames) fprintf(stderr, "[debug]: frame %d: ui drawn\n", frames);
		if (debug_frames) fprintf(stderr, "[debug]: frame %d: focus=%llu hover=%llu input='%c' mouse_down=%d\n", frames, (unsigned long long) mu_ctx.focus, (unsigned long long) mu_ctx.hover, mu_ctx.input_text[0] ? mu_ctx.input_text[0] : '-', mu_ctx.mouse_down);

		// testing hook: save a screenshot after N frames and exit
		if (screenshot_path && frames == 40 && !script_path){
			save_screenshot(screenshot_path, width, height);
			quit = true;
		}
		if (request_shot && shot_full_path[0] != 0){
			save_screenshot(shot_full_path, width, height);
			shot_full_path[0] = 0;
			request_shot = false;
		}
		frames++;

		SDL_GL_SwapWindow(win);
	}

	free(curve_points);
	SDL_GL_DeleteContext(gl);
	SDL_DestroyWindow(win);
	SDL_Quit();
	return 0;
}
