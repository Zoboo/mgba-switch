/* Copyright (c) 2013-2018 Jeffrey Pfau
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#include <mgba-util/gui/font.h>
#include <mgba-util/gui/font-metrics.h>
#include <mgba-util/image/png-io.h>
#include <mgba-util/string.h>
#include <mgba-util/vfs.h>

#include <GLES3/gl3.h>
#include <ft2build.h>
#include FT_FREETYPE_H

#define GLYPH_HEIGHT 24
#define CELL_HEIGHT 32
#define CELL_WIDTH 32
#define MAX_GLYPHS 1024

static const GLfloat _offsets[] = {
	0.f, 0.f,
	1.f, 0.f,
	1.f, 1.f,
	0.f, 1.f,
};

static const GLchar* const _gles3Header =
	"#version 300 es\n"
	"precision mediump float;\n";

static const char* const _vertexShader =
	"in vec2 offset;\n"
	"in vec3 origin;\n"
	"in vec2 glyph;\n"
	"in vec2 dims;\n"
	"in mat2 transform;\n"
	"in vec4 color;\n"
	"out vec4 fragColor;\n"
	"out vec2 texCoord;\n"

	"void main() {\n"
	"	texCoord = (glyph + offset * dims) / 512.0;\n"
	"	vec2 scaledOffset = (transform * (offset * 2.0 - vec2(1.0)) + vec2(1.0)) / 2.0 * dims;\n"
	"	fragColor = color;\n"
	"	gl_Position = vec4((origin.x + scaledOffset.x) / 640.0 - 1.0, -(origin.y + scaledOffset.y) / 360.0 + 1.0, origin.z, 1.0);\n"
	"}";

static const char* const _fragmentShader =
	"in vec2 texCoord;\n"
	"in vec4 fragColor;\n"
	"out vec4 outColor;\n"
	"uniform sampler2D tex;\n"
	"uniform float cutoff;\n"
	"uniform vec3 colorModulus;\n"

	"void main() {\n"
	"	vec4 texColor = texture(tex, texCoord);\n"
	"	texColor.a = clamp((texColor.r - cutoff) / (1.0 - cutoff), 0.0, 1.0);\n"
	"	texColor.rgb = fragColor.rgb * colorModulus;\n"
	"	texColor.a *= fragColor.a;\n"
	"	outColor = texColor;\n"
	"}";

struct CachedGlyph {
	uint32_t code;
	unsigned size;
	int x, y, w, h, left, top, advance;
};

struct GUIFont {
	FT_Library library;
	FT_Face face;
	uint16_t (*widths)[128];
	unsigned size;
	GLuint textTexture;
	GLuint batchTexture;
	struct CachedGlyph cache[512];
	unsigned cacheCount;
	int atlasX, atlasY, atlasRow;
	GLuint font;
	int currentGlyph;
	GLuint program;
	GLuint vbo;
	GLuint vao;
	GLuint texLocation;
	GLuint cutoffLocation;
	GLuint colorModulusLocation;

	GLuint originLocation;
	GLuint glyphLocation;
	GLuint dimsLocation;
	GLuint transformLocation[2];
	GLuint colorLocation;

	GLuint originVbo;
	GLuint glyphVbo;
	GLuint dimsVbo;
	GLuint transformVbo[2];
	GLuint colorVbo;

	GLfloat originData[MAX_GLYPHS][3];
	GLfloat glyphData[MAX_GLYPHS][2];
	GLfloat dimsData[MAX_GLYPHS][2];
	GLfloat transformData[2][MAX_GLYPHS][2];
	GLfloat colorData[MAX_GLYPHS][4];
};

static bool _loadTexture(const char* path) {
	struct VFile* vf = VFileOpen(path, O_RDONLY);
	if (!vf) {
		return false;
	}
	png_structp png = PNGReadOpen(vf, 0);
	png_infop info = png_create_info_struct(png);
	png_infop end = png_create_info_struct(png);
	bool success = false;
	if (png && info && end) {
		success = PNGReadHeader(png, info);
	}
	void* pixels = NULL;
	if (success) {
		unsigned height = png_get_image_height(png, info);
		unsigned width = png_get_image_width(png, info);
		pixels = malloc(width * height);
		if (pixels) {
			success = PNGReadPixels8(png, info, pixels, width, height, width);
			success = success && PNGReadFooter(png, end);
		} else {
			success = false;
		}
		if (success) {
			glTexImage2D(GL_TEXTURE_2D, 0, GL_R8, width, height, 0, GL_RED, GL_UNSIGNED_BYTE, pixels);
		}
	}
	PNGReadClose(png, info, end);
	if (pixels) {
		free(pixels);
	}
	vf->close(vf);
	return success;
}

struct GUIFont* GUIFontCreate(void) {
	struct GUIFont* font = calloc(1, sizeof(struct GUIFont));
	if (!font) {
		return NULL;
	}
	glGenTextures(1, &font->font);
	glActiveTexture(GL_TEXTURE0);
	glBindTexture(GL_TEXTURE_2D, font->font);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
	if (!_loadTexture("romfs:/font-new.png")) {
		GUIFontDestroy(font);
		return NULL;
	}

	font->size = GLYPH_HEIGHT;
	font->widths = calloc(37, sizeof(*font->widths));
	font->batchTexture = font->font;
	if (!FT_Init_FreeType(&font->library)) {
		if (!FT_New_Face(font->library, "romfs:/SourceSans3-Regular.otf", 0, &font->face)) {
			FT_Set_Pixel_Sizes(font->face, 0, font->size);
			glGenTextures(1, &font->textTexture);
			glBindTexture(GL_TEXTURE_2D, font->textTexture);
			glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
			glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
			glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
			glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
			glTexImage2D(GL_TEXTURE_2D, 0, GL_R8, 512, 512, 0, GL_RED, GL_UNSIGNED_BYTE, NULL);
		}
	}
	font->currentGlyph = 0;
	font->program = glCreateProgram();
	GLuint vertexShader = glCreateShader(GL_VERTEX_SHADER);
	GLuint fragmentShader = glCreateShader(GL_FRAGMENT_SHADER);
	const GLchar* shaderBuffer[2];

	shaderBuffer[0] = _gles3Header;

	shaderBuffer[1] = _vertexShader;
	glShaderSource(vertexShader, 2, shaderBuffer, NULL);

	shaderBuffer[1] = _fragmentShader;
	glShaderSource(fragmentShader, 2, shaderBuffer, NULL);

	glAttachShader(font->program, vertexShader);
	glAttachShader(font->program, fragmentShader);

	glCompileShader(fragmentShader);

	GLint success;
	bool shadersValid = true;
	glGetShaderiv(fragmentShader, GL_COMPILE_STATUS, &success);
	if (!success) {
		GLchar msg[512];
		glGetShaderInfoLog(fragmentShader, sizeof(msg), NULL, msg);
		puts(msg);
		shadersValid = false;
	}

	glCompileShader(vertexShader);

	glGetShaderiv(vertexShader, GL_COMPILE_STATUS, &success);
	if (!success) {
		GLchar msg[512];
		glGetShaderInfoLog(vertexShader, sizeof(msg), NULL, msg);
		puts(msg);
		shadersValid = false;
	}

	glLinkProgram(font->program);

	glGetProgramiv(font->program, GL_LINK_STATUS, &success);
	if (!success) {
		GLchar msg[512];
		glGetProgramInfoLog(font->program, sizeof(msg), NULL, msg);
		puts(msg);
		shadersValid = false;
	}

	glDeleteShader(vertexShader);
	glDeleteShader(fragmentShader);
	if (!shadersValid) {
		GUIFontDestroy(font);
		return NULL;
	}

	font->texLocation = glGetUniformLocation(font->program, "tex");
	font->cutoffLocation = glGetUniformLocation(font->program, "cutoff");
	font->colorModulusLocation = glGetUniformLocation(font->program, "colorModulus");

	font->originLocation = glGetAttribLocation(font->program, "origin");
	font->glyphLocation = glGetAttribLocation(font->program, "glyph");
	font->dimsLocation = glGetAttribLocation(font->program, "dims");
	font->transformLocation[0] = glGetAttribLocation(font->program, "transform");
	font->transformLocation[1] = font->transformLocation[0] + 1;
	font->colorLocation = glGetAttribLocation(font->program, "color");

	GLuint offsetLocation = glGetAttribLocation(font->program, "offset");

	glGenVertexArrays(1, &font->vao);
	glBindVertexArray(font->vao);

	glGenBuffers(1, &font->vbo);
	glBindBuffer(GL_ARRAY_BUFFER, font->vbo);
	glBufferData(GL_ARRAY_BUFFER, sizeof(_offsets), _offsets, GL_STATIC_DRAW);
	glVertexAttribPointer(offsetLocation, 2, GL_FLOAT, GL_FALSE, 0, NULL);
	glVertexAttribDivisor(offsetLocation, 0);
	glEnableVertexAttribArray(offsetLocation);

	glGenBuffers(1, &font->originVbo);
	glBindBuffer(GL_ARRAY_BUFFER, font->originVbo);
	glBufferData(GL_ARRAY_BUFFER, sizeof(GLfloat) * 3 * MAX_GLYPHS, NULL, GL_STREAM_DRAW);
	glVertexAttribPointer(font->originLocation, 3, GL_FLOAT, GL_FALSE, 0, NULL);
	glVertexAttribDivisor(font->originLocation, 1);
	glEnableVertexAttribArray(font->originLocation);

	glGenBuffers(1, &font->glyphVbo);
	glBindBuffer(GL_ARRAY_BUFFER, font->glyphVbo);
	glBufferData(GL_ARRAY_BUFFER, sizeof(GLfloat) * 2 * MAX_GLYPHS, NULL, GL_STREAM_DRAW);
	glVertexAttribPointer(font->glyphLocation, 2, GL_FLOAT, GL_FALSE, 0, NULL);
	glVertexAttribDivisor(font->glyphLocation, 1);
	glEnableVertexAttribArray(font->glyphLocation);

	glGenBuffers(1, &font->dimsVbo);
	glBindBuffer(GL_ARRAY_BUFFER, font->dimsVbo);
	glBufferData(GL_ARRAY_BUFFER, sizeof(GLfloat) * 2 * MAX_GLYPHS, NULL, GL_STREAM_DRAW);
	glVertexAttribPointer(font->dimsLocation, 2, GL_FLOAT, GL_FALSE, 0, NULL);
	glVertexAttribDivisor(font->dimsLocation, 1);
	glEnableVertexAttribArray(font->dimsLocation);

	glGenBuffers(2, font->transformVbo);
	glBindBuffer(GL_ARRAY_BUFFER, font->transformVbo[0]);
	glBufferData(GL_ARRAY_BUFFER, sizeof(GLfloat) * 2 * MAX_GLYPHS, NULL, GL_STREAM_DRAW);
	glVertexAttribPointer(font->transformLocation[0], 2, GL_FLOAT, GL_FALSE, 0, NULL);
	glVertexAttribDivisor(font->transformLocation[0], 1);
	glEnableVertexAttribArray(font->transformLocation[0]);
	glBindBuffer(GL_ARRAY_BUFFER, font->transformVbo[1]);
	glBufferData(GL_ARRAY_BUFFER, sizeof(GLfloat) * 2 * MAX_GLYPHS, NULL, GL_STREAM_DRAW);
	glVertexAttribPointer(font->transformLocation[1], 2, GL_FLOAT, GL_FALSE, 0, NULL);
	glVertexAttribDivisor(font->transformLocation[1], 1);
	glEnableVertexAttribArray(font->transformLocation[1]);

	glGenBuffers(1, &font->colorVbo);
	glBindBuffer(GL_ARRAY_BUFFER, font->colorVbo);
	glBufferData(GL_ARRAY_BUFFER, sizeof(GLfloat) * 4 * MAX_GLYPHS, NULL, GL_STREAM_DRAW);
	glVertexAttribPointer(font->colorLocation, 4, GL_FLOAT, GL_FALSE, 0, NULL);
	glVertexAttribDivisor(font->colorLocation, 1);
	glEnableVertexAttribArray(font->colorLocation);

	glBindVertexArray(0);

	return font;
}

void GUIFontDestroy(struct GUIFont* font) {
	if (font->face) FT_Done_Face(font->face);
	if (font->library) FT_Done_FreeType(font->library);
	free(font->widths);
	glDeleteTextures(1, &font->textTexture);
	glDeleteBuffers(1, &font->vbo);
	glDeleteBuffers(1, &font->originVbo);
	glDeleteBuffers(1, &font->glyphVbo);
	glDeleteBuffers(1, &font->dimsVbo);
	glDeleteBuffers(2, font->transformVbo);
	glDeleteBuffers(1, &font->colorVbo);
	glDeleteProgram(font->program);
	glDeleteTextures(1, &font->font);
	glDeleteVertexArrays(1, &font->vao);
	free(font);
}

unsigned GUIFontHeight(const struct GUIFont* font) {
	return font->size;
}

unsigned GUIFontGlyphWidth(const struct GUIFont* font, uint32_t glyph) {
	uint16_t* width = font->widths && glyph < 128 ? &font->widths[font->size - 12][glyph] : NULL;
	if (width && *width) return *width - 1;
	if (font->face && !FT_Load_Char(font->face, glyph, FT_LOAD_DEFAULT)) {
		unsigned advance = font->face->glyph->advance.x >> 6;
		if (width) *width = advance + 1;
		return advance;
	}
	if (glyph > 0x7F) {
		glyph = '?';
	}
	return defaultFontMetrics[glyph].width * font->size / 12;
}

void GUIFontIconMetrics(const struct GUIFont* font, enum GUIIcon icon, unsigned* w, unsigned* h) {
	UNUSED(font);
	if (icon >= GUI_ICON_MAX) {
		if (w) {
			*w = 0;
		}
		if (h) {
			*h = 0;
		}
	} else {
		if (w) {
			*w = defaultIconMetrics[icon].width * 2;
		}
		if (h) {
			*h = defaultIconMetrics[icon].height * 2;
		}
	}
}

void SwitchUIFontSize(struct GUIFont* font, unsigned size) {
	if (size < 12) size = 12;
	if (size > 48) size = 48;
	if (font->size == size) return;
	font->size = size;
	if (font->face) FT_Set_Pixel_Sizes(font->face, 0, size);
}

unsigned SwitchUIFontSizeGet(const struct GUIFont* font) {
	return font->size;
}

static void _batchTexture(struct GUIFont* font, GLuint texture) {
	if (font->batchTexture != texture) GUIFontDrawSubmit(font);
	font->batchTexture = texture;
}

static struct CachedGlyph* _glyph(struct GUIFont* font, uint32_t code) {
	unsigned i;
	for (i = 0; i < font->cacheCount; ++i) {
		if (font->cache[i].code == code && font->cache[i].size == font->size) return &font->cache[i];
	}
	if (FT_Load_Char(font->face, code, FT_LOAD_RENDER)) return NULL;
	FT_GlyphSlot slot = font->face->glyph;
	int w = slot->bitmap.width, h = slot->bitmap.rows;
	if (font->atlasX + w + 2 > 512) {
		font->atlasX = 0;
		font->atlasY += font->atlasRow;
		font->atlasRow = 0;
	}
	if (font->atlasY + h + 2 > 512 || font->cacheCount == 512) {
		GUIFontDrawSubmit(font);
		font->cacheCount = 0;
		font->atlasX = font->atlasY = font->atlasRow = 0;
	}
	struct CachedGlyph* g = &font->cache[font->cacheCount++];
	*g = (struct CachedGlyph) {code, font->size, font->atlasX + 1, font->atlasY + 1, w, h,
	                         slot->bitmap_left, slot->bitmap_top, slot->advance.x >> 6};
	if (w && h) {
		GLint active, texture, unpack;
		glGetIntegerv(GL_ACTIVE_TEXTURE, &active);
		glActiveTexture(GL_TEXTURE0);
		glGetIntegerv(GL_TEXTURE_BINDING_2D, &texture);
		glGetIntegerv(GL_UNPACK_ALIGNMENT, &unpack);
		glBindTexture(GL_TEXTURE_2D, font->textTexture);
		glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
		unsigned char* pixels = calloc((w + 2) * (h + 2), 1);
		if (pixels) {
			for (int row = 0; row < h; ++row) memcpy(pixels + (row + 1) * (w + 2) + 1, slot->bitmap.buffer + row * slot->bitmap.pitch, w);
			glTexSubImage2D(GL_TEXTURE_2D, 0, g->x - 1, g->y - 1, w + 2, h + 2, GL_RED, GL_UNSIGNED_BYTE, pixels);
			free(pixels);
		}
		glPixelStorei(GL_UNPACK_ALIGNMENT, unpack);
		glBindTexture(GL_TEXTURE_2D, texture);
		glActiveTexture(active);
	}
	font->atlasX += w + 2;
	if (h + 2 > font->atlasRow) font->atlasRow = h + 2;
	return g;
}

void GUIFontDrawGlyph(struct GUIFont* font, int x, int y, uint32_t color, uint32_t glyph) {
	struct CachedGlyph* cached = NULL;
	if (font->face) cached = _glyph(font, glyph);
	_batchTexture(font, cached ? font->textTexture : font->font);
	if (glyph > 0x7F) {
		glyph = '?';
	}
	struct GUIFontGlyphMetric metric = defaultFontMetrics[glyph];

	if (font->currentGlyph >= MAX_GLYPHS) {
		GUIFontDrawSubmit(font);
	}

	int offset = font->currentGlyph;

	font->originData[offset][0] = x;
	font->originData[offset][1] = y - GLYPH_HEIGHT + metric.padding.top * 2;
	font->originData[offset][2] = 0;
	font->glyphData[offset][0] = (glyph & 15) * CELL_WIDTH + metric.padding.left * 2;
	font->glyphData[offset][1] = (glyph >> 4) * CELL_HEIGHT + metric.padding.top * 2;
	font->dimsData[offset][0] = CELL_WIDTH - (metric.padding.left + metric.padding.right) * 2;
	font->dimsData[offset][1] = CELL_HEIGHT - (metric.padding.top + metric.padding.bottom) * 2;
	if (cached) {
		font->originData[offset][0] = x + cached->left;
		font->originData[offset][1] = y - cached->top;
		font->glyphData[offset][0] = cached->x;
		font->glyphData[offset][1] = cached->y;
		font->dimsData[offset][0] = cached->w;
		font->dimsData[offset][1] = cached->h;
	} else {
		float scale = font->size / 24.f;
		font->originData[offset][1] = y - font->size + metric.padding.top * 2 * scale;
	}
	font->transformData[0][offset][0] = 1.0f;
	font->transformData[0][offset][1] = 0.0f;
	font->transformData[1][offset][0] = 0.0f;
	font->transformData[1][offset][1] = 1.0f;
	if (!cached) {
		float scale = font->size / 24.f;
		font->originData[offset][0] += (scale - 1.f) * font->dimsData[offset][0] * .5f;
		font->originData[offset][1] += (scale - 1.f) * font->dimsData[offset][1] * .5f;
		font->transformData[0][offset][0] = scale;
		font->transformData[1][offset][1] = scale;
	}
	font->colorData[offset][0] = (color & 0xFF) / 255.0f;
	font->colorData[offset][1] = ((color >> 8) & 0xFF) / 255.0f;
	font->colorData[offset][2] = ((color >> 16) & 0xFF) / 255.0f;
	font->colorData[offset][3] = ((color >> 24) & 0xFF) / 255.0f;

	++font->currentGlyph;
}

void GUIFontDrawIcon(struct GUIFont* font, int x, int y, enum GUIAlignment align, enum GUIOrientation orient, uint32_t color, enum GUIIcon icon) {
	if (icon >= GUI_ICON_MAX) {
		return;
	}
	_batchTexture(font, font->font);
	struct GUIIconMetric metric = defaultIconMetrics[icon];

	float hFlip = 1.0f;
	float vFlip = 1.0f;
	switch (align & GUI_ALIGN_HCENTER) {
	case GUI_ALIGN_HCENTER:
		x -= metric.width;
		break;
	case GUI_ALIGN_RIGHT:
		x -= metric.width * 2;
		break;
	}
	switch (align & GUI_ALIGN_VCENTER) {
	case GUI_ALIGN_VCENTER:
		y -= metric.height;
		break;
	case GUI_ALIGN_BOTTOM:
		y -= metric.height * 2;
		break;
	}

	switch (orient) {
	case GUI_ORIENT_HMIRROR:
		hFlip = -1.0;
		break;
	case GUI_ORIENT_VMIRROR:
		vFlip = -1.0;
		break;
	case GUI_ORIENT_0:
	default:
		// TODO: Rotate
		break;
	}
	if (font->currentGlyph >= MAX_GLYPHS) {
		GUIFontDrawSubmit(font);
	}

	int offset = font->currentGlyph;

	font->originData[offset][0] = x;
	font->originData[offset][1] = y;
	font->originData[offset][2] = 0;
	font->glyphData[offset][0] = metric.x * 2;
	font->glyphData[offset][1] = metric.y * 2 + 256;
	font->dimsData[offset][0] = metric.width * 2;
	font->dimsData[offset][1] = metric.height * 2;
	font->transformData[0][offset][0] = hFlip;
	font->transformData[0][offset][1] = 0.0f;
	font->transformData[1][offset][0] = 0.0f;
	font->transformData[1][offset][1] = vFlip;
	font->colorData[offset][0] = (color & 0xFF) / 255.0f;
	font->colorData[offset][1] = ((color >> 8) & 0xFF) / 255.0f;
	font->colorData[offset][2] = ((color >> 16) & 0xFF) / 255.0f;
	font->colorData[offset][3] = ((color >> 24) & 0xFF) / 255.0f;

	++font->currentGlyph;
}

void GUIFontDrawIconSize(struct GUIFont* font, int x, int y, int w, int h, uint32_t color, enum GUIIcon icon) {
	if (icon >= GUI_ICON_MAX) {
		return;
	}
	_batchTexture(font, font->font);
	struct GUIIconMetric metric = defaultIconMetrics[icon];

	if (!w) {
		w = metric.width * 2;
	}
	if (!h) {
		h = metric.height * 2;
	}

	if (font->currentGlyph >= MAX_GLYPHS) {
		GUIFontDrawSubmit(font);
	}

	int offset = font->currentGlyph;

	font->originData[offset][0] = x + w / 2 - metric.width;
	font->originData[offset][1] = y + h / 2 - metric.height;
	font->originData[offset][2] = 0;
	font->glyphData[offset][0] = metric.x * 2;
	font->glyphData[offset][1] = metric.y * 2 + 256;
	font->dimsData[offset][0] = metric.width * 2;
	font->dimsData[offset][1] = metric.height * 2;
	font->transformData[0][offset][0] = w * 0.5f / metric.width;
	font->transformData[0][offset][1] = 0.0f;
	font->transformData[1][offset][0] = 0.0f;
	font->transformData[1][offset][1] = h * 0.5f / metric.height;
	font->colorData[offset][0] = (color & 0xFF) / 255.0f;
	font->colorData[offset][1] = ((color >> 8) & 0xFF) / 255.0f;
	font->colorData[offset][2] = ((color >> 16) & 0xFF) / 255.0f;
	font->colorData[offset][3] = ((color >> 24) & 0xFF) / 255.0f;

	++font->currentGlyph;
}

void GUIFontDrawSubmit(struct GUIFont* font) {
	if (!font->currentGlyph) {
		return;
	}
	GLint active, texture;
	glGetIntegerv(GL_ACTIVE_TEXTURE, &active);
	glActiveTexture(GL_TEXTURE0);
	glGetIntegerv(GL_TEXTURE_BINDING_2D, &texture);
	glUseProgram(font->program);
	glBindVertexArray(font->vao);
	glActiveTexture(GL_TEXTURE0);
	glBindTexture(GL_TEXTURE_2D, font->batchTexture);

	glEnable(GL_BLEND);
	glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

	glUniform1i(font->texLocation, 0);

	glBindBuffer(GL_ARRAY_BUFFER, font->originVbo);
	glBufferData(GL_ARRAY_BUFFER, sizeof(GLfloat) * 3 * font->currentGlyph, font->originData, GL_STREAM_DRAW);

	glBindBuffer(GL_ARRAY_BUFFER, font->glyphVbo);
	glBufferData(GL_ARRAY_BUFFER, sizeof(GLfloat) * 2 * font->currentGlyph, font->glyphData, GL_STREAM_DRAW);

	glBindBuffer(GL_ARRAY_BUFFER, font->dimsVbo);
	glBufferData(GL_ARRAY_BUFFER, sizeof(GLfloat) * 2 * font->currentGlyph, font->dimsData, GL_STREAM_DRAW);

	glBindBuffer(GL_ARRAY_BUFFER, font->transformVbo[0]);
	glBufferData(GL_ARRAY_BUFFER, sizeof(GLfloat) * 2 * font->currentGlyph, font->transformData[0], GL_STREAM_DRAW);

	glBindBuffer(GL_ARRAY_BUFFER, font->transformVbo[1]);
	glBufferData(GL_ARRAY_BUFFER, sizeof(GLfloat) * 2 * font->currentGlyph, font->transformData[1], GL_STREAM_DRAW);

	glBindBuffer(GL_ARRAY_BUFFER, font->colorVbo);
	glBufferData(GL_ARRAY_BUFFER, sizeof(GLfloat) * 4 * font->currentGlyph, font->colorData, GL_STREAM_DRAW);

	glUniform1f(font->cutoffLocation, font->batchTexture == font->textTexture ? 0.f : 0.5f);
	glUniform3f(font->colorModulusLocation, 1.f, 1.f, 1.f);
	glDrawArraysInstanced(GL_TRIANGLE_FAN, 0, 4, font->currentGlyph);

	font->currentGlyph = 0;

	glBindTexture(GL_TEXTURE_2D, texture);
	glActiveTexture(active);
	glBindVertexArray(0);
	glUseProgram(0);
}
