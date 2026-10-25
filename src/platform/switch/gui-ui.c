/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#include "gui-ui.h"
#include "artwork.h"
#include "gui-images.h"
#include "game-settings.h"
#include "plaza.h"
#include "feature/gui/gui-runner.h"
#include <mgba-util/gui/font.h>
#include <mgba-util/gui/file-select.h>
#include <mgba-util/image.h>
#include <mgba-util/string.h>
#include <mgba-util/vfs.h>

#include <GLES3/gl3.h>
#include <switch.h>
#include <sys/stat.h>
#include <strings.h>
#include <unistd.h>

#define UI_TEXT 0xFFF5F3EF
#define UI_MUTED 0xFFB8B2AA
#define UI_FOCUS 0xFFFFA3BD
#define UI_BASE 0xFF1F1417
#define UI_SURFACE 0xFF332024

static struct mGUIRunner* _runner;
static PadState _pad;
static GLuint _program, _vao, _vbo, _white;
static GLint _tint, _bounds, _corner, _border, _shade, _crop;
static bool _files, _covers, _banners;
static unsigned _filter = 7, _frames;
static bool _filterFocused;
static unsigned _filterIndex;
static bool _inLibrary;
static bool _inArtwork;
static bool _importingArtwork;
static char _focusPath[PATH_MAX];
static char _bannerRom[PATH_MAX];
static float _bannerOpacity;
static struct GUIMenu _artworkMenu, _steamArtworkMenu, _localArtworkMenu;
static struct GUIMenu _interfaceMenu, _fileSystemMenu, _settingsMenu;
static unsigned _fileSystemIndex;
static bool _romDirectoryChanged;
static struct GUIMenu _romLibraryMenu;
static bool _romLibraryDirty;
static unsigned _layout;
static struct {
	const struct GUIMenu* menu;
	const void* entries;
	char directory[PATH_MAX];
	size_t count;
	size_t* games;
	size_t* files;
	size_t nGames, nFiles;
	unsigned filter;
} _library;
static float _scroll;
static struct {
	bool down, tracking, dragging, suppressTap;
	unsigned startX, lastX, finger;
	float velocity;
	u64 sampled;
} _swipe;
static size_t _lastSelection = SIZE_MAX;
static struct {
	char path[PATH_MAX];
	GLuint texture;
	unsigned width, height, used, checked;
	bool banner, failed, ready;
} _textures[8];
static unsigned _artworkRevision;
static unsigned _pickerPhase;
static size_t _pickerIndex;
static char _pickerQuery[513];
static int _pickerType = -1;
static unsigned _resetWatch;
static bool _resetBanner, _resetSuccess;
static char _resetGame[513], _resetMessage[192], _resetNotice[600];
static u64 _resetNoticeUntil;
static bool _gameSettingsOpen;
static unsigned _plazaInstalled;
static unsigned _gameSettingsIndex;
static bool _gameSettingsLocalNotice;
static char _gameSettingsRom[PATH_MAX];
static struct SwitchGameSettings _gameSettings;

static GLuint _shader(GLenum type, const char* source) {
	GLuint shader = glCreateShader(type);
	glShaderSource(shader, 1, &source, NULL);
	glCompileShader(shader);
	GLint compiled = 0;
	glGetShaderiv(shader, GL_COMPILE_STATUS, &compiled);
	if (!compiled) {
		glDeleteShader(shader);
		return 0;
	}
	return shader;
}

static void _quadShape(GLuint texture, int x, int y, int w, int h, uint32_t color, float u, float v, int radius,
                       int border) {
	GUIFontDrawSubmit(_runner->params.font);
	glUseProgram(_program);
	glBindVertexArray(_vao);
	glActiveTexture(GL_TEXTURE0);
	glBindTexture(GL_TEXTURE_2D, texture);
	glUniform4f(_tint, (color & 255) / 255.f, ((color >> 8) & 255) / 255.f, ((color >> 16) & 255) / 255.f,
	            (color >> 24) / 255.f);
	glUniform4f(_bounds, x, y, w, h);
	glUniform2f(_crop, u, v);
	glUniform1f(_corner, radius);
	glUniform1f(_border, border);
	glUniform1f(_shade, texture != _white && !radius ? 1.f : 0.f);
	glEnable(GL_BLEND);
	glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
	glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
}

static void _quad(GLuint texture, int x, int y, int w, int h, uint32_t color, float u, float v) {
	_quadShape(texture, x, y, w, h, color, u, v, 0, 0);
}

void SwitchUIDrawTexture(unsigned texture, int x, int y, int width, int height) {
	_quadShape(texture, x, y, width, height, 0xFFFFFFFF, 1, 1, 6, 0);
}

void SwitchUIRoundRect(int x, int y, int width, int height, int radius, uint32_t color) {
	_quadShape(_white, x, y, width, height, color, 1, 1, radius, 0);
}

void SwitchUIRoundFrame(int x, int y, int width, int height, int radius, uint32_t color) {
	_quadShape(_white, x, y, width, height, color, 1, 1, radius, 2);
}

void SwitchUIHint(struct GUIFont* font, int x, int y, const char* button, const char* label) {
	SwitchUIFontSize(font, 18);
	int width = GUIFontSpanWidth(font, button) + 18;
	SwitchUIRoundRect(x, y - 24, width, 30, 15, 0xFFEDE9E3);
	SwitchUIPrint(font, x + 9, y - 3, width - 12, UI_BASE, button);
	SwitchUIPrint(font, x + width + 10, y, 230, UI_TEXT, label);
	SwitchUIFontSize(font, 24);
}

void SwitchUIDrawRect(int x, int y, int width, int height, uint32_t color) {
	_quad(_white, x, y, width, height, color, 1, 1);
}

void SwitchUIDrawFrame(int x, int y, int width, int height, uint32_t color) {
	SwitchUIDrawRect(x, y, width, 2, color);
	SwitchUIDrawRect(x, y + height - 2, width, 2, color);
	SwitchUIDrawRect(x, y, 2, height, color);
	SwitchUIDrawRect(x + width - 2, y, 2, height, color);
}

void SwitchUIPrint(struct GUIFont* font, int x, int y, int maxWidth, uint32_t color, const char* text) {
	if (!text || maxWidth <= 0) {
		return;
	}
	char clipped[512];
	strlcpy(clipped, text, sizeof(clipped));
	size_t length = strlen(clipped);
	if (GUIFontSpanWidth(font, clipped) > (unsigned) maxWidth) {
		while (length &&
		       GUIFontSpanCountWidth(font, clipped, length) + GUIFontSpanWidth(font, "...") > (unsigned) maxWidth) {
			--length;
			while (length && ((unsigned char) clipped[length] & 0xC0) == 0x80)
				--length;
		}
		strlcpy(clipped + length, "...", sizeof(clipped) - length);
	}
	GUIFontPrint(font, x, y, GUI_ALIGN_LEFT, color, clipped);
}

static void _pollImages(void) {
	char path[PATH_MAX];
	bool banner;
	struct mImage* image;
	while (SwitchImagesPoll(path, sizeof(path), &banner, &image)) {
		for (unsigned i = 0; i < 8; ++i) {
			if (_textures[i].banner != banner || strcmp(_textures[i].path, path)) continue;
			_textures[i].ready = true;
			_textures[i].checked = _frames;
			_textures[i].failed = !image;
			if (!image) break;
			GUIFontDrawSubmit(_runner->params.font);
			if (!_textures[i].texture) glGenTextures(1, &_textures[i].texture);
			glActiveTexture(GL_TEXTURE0);
			glBindTexture(GL_TEXTURE_2D, _textures[i].texture);
			glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
			glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
			glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
			glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
			glPixelStorei(GL_UNPACK_ROW_LENGTH, image->stride);
			glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, image->width, image->height, 0, GL_RGBA, GL_UNSIGNED_BYTE, image->data);
			glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
			_textures[i].width = image->width;
			_textures[i].height = image->height;
			break;
		}
		if (image) mImageDestroy(image);
	}
}

static int _texture(const char* path, bool banner, bool preview) {
	_pollImages();
	unsigned oldest = 0;
	for (unsigned i = 0; i < 8; ++i) {
		if (_textures[i].banner == banner && !strcmp(_textures[i].path, path)) {
			_textures[i].used = _frames;
			if (_textures[i].failed && _frames - _textures[i].checked >= 300) _textures[i].ready = false;
			if (_textures[i].ready) return _textures[i].failed ? -1 : (int) i;
			if (preview) SwitchImagesRequestPath(path);
			else SwitchImagesRequest(path, banner);
			return -1;
		}
		if (_textures[i].used <= _textures[oldest].used) oldest = i;
	}
	if (_pickerPhase && !preview) return -1;
	if (_textures[oldest].texture) glDeleteTextures(1, &_textures[oldest].texture);
	memset(&_textures[oldest], 0, sizeof(_textures[oldest]));
	strlcpy(_textures[oldest].path, path, sizeof(_textures[oldest].path));
	_textures[oldest].banner = banner;
	_textures[oldest].used = _frames;
	if (preview) SwitchImagesRequestPath(path);
	else SwitchImagesRequest(path, banner);
	return -1;
}

static bool _image(const char* rom, bool banner, int x, int y, int w, int h) {
	int i = _texture(rom, banner, false);
	if (i < 0 && banner) i = _texture(rom, false, false);
	if (i < 0) {
		return false;
	}
	/* Fit covers without distortion; banners fill their wide background. */
	if (!banner) {
		float scale = fminf((float) w / _textures[i].width, (float) h / _textures[i].height);
		int iw = _textures[i].width * scale, ih = _textures[i].height * scale;
		x += (w - iw) / 2;
		y += (h - ih) / 2;
		w = iw;
		h = ih;
	}
	float u = 1, v = 1;
	if (banner) {
		float imageRatio = (float) _textures[i].width / _textures[i].height;
		float targetRatio = (float) w / h;
		if (imageRatio > targetRatio)
			u = targetRatio / imageRatio;
		else
			v = imageRatio / targetRatio;
	}
	uint32_t color = 0xFFFFFFFF;
	if (banner) {
		if (strcmp(_bannerRom, rom)) {
			strlcpy(_bannerRom, rom, sizeof(_bannerRom));
			_bannerOpacity = 0;
		}
		_bannerOpacity = fminf(1.f, _bannerOpacity + .12f);
		color = 0xFFFFFF | (uint32_t) (_bannerOpacity * 255) << 24;
	}
	_quadShape(_textures[i].texture, x, y, w, h, color, u, v, banner ? 0 : 8, 0);
	return true;
}

static void _path(struct GUIParams* params, const char* name, char* path, size_t size) {
	if (name[0] == '/' || !strncmp(name, "sdmc:/", 6)) {
		strlcpy(path, name, size);
		return;
	}
	size_t len = strlen(params->currentPath);
	snprintf(path, size, "%s%s%s", params->currentPath, len && params->currentPath[len - 1] != '/' ? "/" : "", name);
}

bool SwitchUIArtworkDraw(const char* rom, bool banner, int x, int y, int w, int h) {
	return _image(rom, banner, x, y, w, h);
}

void SwitchUIRomPath(struct GUIParams* params, const char* name, char* path, size_t size) {
	_path(params, name, path, size);
}

static const char* _platform(const char* name) {
	const char* dot = strrchr(name, '.');
	if (dot && (!strcasecmp(dot, ".gba") || !strcasecmp(dot, ".agb") || !strcasecmp(dot, ".mb")))
		return "GBA";
	if (dot && !strcasecmp(dot, ".gbc"))
		return "GBC";
	if (dot && (!strcasecmp(dot, ".gb") || !strcasecmp(dot, ".dmg") || !strcasecmp(dot, ".sgb")))
		return "GB";
	return "";
}

static bool _archive(const char* name) {
	const char* ext = strrchr(name, '.');
	return ext && (!strcasecmp(ext, ".zip") || !strcasecmp(ext, ".7z"));
}

static bool _game(const struct GUIMenuItem* item) {
	if (!GUIVariantCompareUInt(item->data, VFS_FILE))
		return false;
	if (*_platform(item->title) || _archive(item->title))
		return true;
	const char* ext = strrchr(item->title, '.');
	return ext &&
	    (!strcasecmp(ext, ".agb") || !strcasecmp(ext, ".bin") || !strcasecmp(ext, ".rom") || !strcasecmp(ext, ".mb") ||
		 !strcasecmp(ext, ".dmg") || !strcasecmp(ext, ".sgb") || !strcasecmp(ext, ".gbx"));
}

static bool _matches(const struct GUIMenuItem* item) {
	const char* platform = _platform(item->title);
	unsigned mask = !strcmp(platform, "GBA") ? 1 : !strcmp(platform, "GBC") ? 2 : !strcmp(platform, "GB") ? 4 : 7;
	/* Unknown platform files remain available when every console is enabled. */
	return mask == 7 ? _filter == 7 : (_filter & mask) != 0;
}

void SwitchUIBeginMenu(const struct GUIMenu* menu) {
	if (menu->title && !strcmp(menu->title, "Select file")) {
		_library.menu = NULL;
		_romLibraryDirty = true;
	}
}

static bool _indexLibrary(struct GUIParams* params, const struct GUIMenu* menu) {
	size_t count = GUIMenuItemListSize(&menu->items);
	if (_library.menu == menu && _library.entries == menu->items.vector && _library.count == count &&
	    _library.filter == _filter && !strcmp(_library.directory, params->currentPath))
		return true;
	free(_library.games);
	free(_library.files);
	_library.games = count ? malloc(count * sizeof(size_t)) : NULL;
	_library.files = count ? malloc(count * sizeof(size_t)) : NULL;
	_library.nGames = _library.nFiles = 0;
	_library.menu = NULL;
	if (count && (!_library.games || !_library.files))
		return false;
	for (size_t i = 0; i < count; ++i) {
		const struct GUIMenuItem* item = GUIMenuItemListGetConstPointer(&menu->items, i);
		if (_game(item) && _matches(item))
			_library.games[_library.nGames++] = i;
		if (!GUIVariantCompareUInt(item->data, VFS_FILE) || _matches(item))
			_library.files[_library.nFiles++] = i;
	}
	_library.menu = menu;
	_library.entries = menu->items.vector;
	_library.count = count;
	_library.filter = _filter;
	strlcpy(_library.directory, params->currentPath, sizeof(_library.directory));
	_lastSelection = SIZE_MAX;
	return true;
}

static size_t _position(size_t index, const size_t* entries, size_t count) {
	for (size_t i = 0; i < count; ++i)
		if (entries[i] == index)
			return i;
	return 0;
}

static void _title(const struct GUIMenuItem* item, char* out, size_t size) {
	const char* name = GUIVariantCompareUInt(item->data, VFS_FILE) ? strrchr(item->title, '/') : NULL;
	strlcpy(out, name ? name + 1 : item->title, size);
	if (!strcmp(out, "(Up)")) {
		strlcpy(out, "Parent directory", size);
		return;
	}
	size_t len = strlen(out);
	if (len && out[len - 1] == '/')
		out[len - 1] = 0;
	if (GUIVariantCompareUInt(item->data, VFS_FILE)) {
		char* ext = strrchr(out, '.');
		if (ext)
			*ext = 0;
	}
}

static void _folder(int x, int y, uint32_t color) {
	SwitchUIRoundRect(x, y, 18, 8, 3, color);
	SwitchUIRoundRect(x, y + 5, 34, 23, 4, color);
}

static void _cartridge(int x, int y, int width, uint32_t color) {
	SwitchUIRoundRect(x, y, width, width * 3 / 4, 6, color);
	SwitchUIRoundRect(x + width / 5, y + 8, width * 3 / 5, width / 4, 3, UI_SURFACE);
	for (int i = 0; i < 5; ++i)
		SwitchUIDrawRect(x + width / 4 + i * width / 10, y + width * 3 / 4 - 8, 4, 6, UI_BASE);
}

static void _wrapTitle(struct GUIFont* font, int x, int y, int width, const char* title, unsigned lines,
                       uint32_t color) {
	const char* cursor = title;
	for (unsigned line = 0; line < lines && *cursor; ++line) {
		while (*cursor == ' ')
			++cursor;
		if (line + 1 == lines) {
			SwitchUIPrint(font, x, y, width, color, cursor);
			break;
		}
		size_t bytes = 0, lastSpace = 0, available = strlen(cursor);
		while (bytes < available) {
			size_t next = bytes + 1;
			while (next < available && ((unsigned char) cursor[next] & 0xC0) == 0x80)
				++next;
			if (GUIFontSpanCountWidth(font, cursor, next) > (unsigned) width)
				break;
			if (cursor[bytes] == ' ')
				lastSpace = bytes;
			bytes = next;
		}
		if (!bytes)
			break;
		if (bytes < available && lastSpace)
			bytes = lastSpace;
		char text[512];
		size_t copied = bytes < sizeof(text) - 1 ? bytes : sizeof(text) - 1;
		memcpy(text, cursor, copied);
		text[copied] = 0;
		SwitchUIPrint(font, x, y, width, color, text);
		cursor += bytes;
		y += GUIFontHeight(font) + 5;
	}
}

static void _clearTextures(void) {
	for (int i = 0; i < 8; ++i) {
		glDeleteTextures(1, &_textures[i].texture);
		memset(&_textures[i], 0, sizeof(_textures[i]));
	}
}

static void _saveArtwork(void) {
	mCoreConfigSetIntValue(&_runner->config, "switch.artwork.covers", _covers);
	mCoreConfigSetIntValue(&_runner->config, "switch.artwork.banners", _banners);
	mCoreConfigSave(&_runner->config);
	SwitchArtworkSetDownloads(_covers, _banners);
}

static void _editKey(struct GUIParams* params) {
	SwkbdConfig keyboard;
	if (R_FAILED(swkbdCreate(&keyboard, 0)))
		return;
	swkbdConfigMakePresetPassword(&keyboard);
	swkbdConfigSetHeaderText(&keyboard, "SteamGridDB API key");
	swkbdConfigSetStringLenMax(&keyboard, 128);
	char key[129] = { 0 };
	if (R_SUCCEEDED(swkbdShow(&keyboard, key, sizeof(key))) && !SwitchArtworkSetKey(key)) {
		GUIShowMessageBox(params, GUI_MESSAGE_BOX_OK, 0, "Could not save API key. Use letters and numbers only.");
	}
	memset(key, 0, sizeof(key));
	swkbdClose(&keyboard);
	GUIInvalidateKeys(params);
}

static void _button(struct GUIFont* font, int x, int y, int width, const char* label, bool focused) {
	SwitchUIRoundRect(x, y, width, 44, 8, focused ? 0xFF383B31 : UI_SURFACE);
	if (focused)
		SwitchUIRoundFrame(x, y, width, 44, 8, UI_FOCUS);
	SwitchUIFontSize(font, 20);
	if (!strncmp(label, "A  ", 3)) {
		int contentWidth = GUIFontSpanWidth(font, label + 3) + 42;
		SwitchUIHint(font, x + (width - contentWidth) / 2, y + 29, "A", label + 3);
	} else {
		GUIFontPrint(font, x + width / 2, y + 29, GUI_ALIGN_HCENTER, UI_TEXT, label);
	}
}

static bool _inside(unsigned x, unsigned y, int left, int top, int width, int height) {
	return x >= (unsigned) left && y >= (unsigned) top && x < (unsigned) (left + width) &&
	    y < (unsigned) (top + height);
}

static bool _tap(struct GUIParams* params, unsigned* x, unsigned* y) {
	*x = params->cx;
	*y = params->cy;
	enum GUICursorState cursor = GUIPollCursor(params, x, y);
	bool tapped = cursor == GUI_CURSOR_CLICKED;
	/* The native cursor callback does not supply coordinates on release. */
	if (tapped) {
		*x = params->cx;
		*y = params->cy;
	}
	return tapped;
}

static bool _carouselTouch(size_t count, size_t* position, bool enabled, bool* tapped) {
	HidTouchScreenState touch = {0};
	hidGetTouchScreenStates(&touch, 1);
	u64 now = armTicksToNs(armGetSystemTick());
	float elapsed = _swipe.sampled ? (now - _swipe.sampled) / 1000000000.f : 0.f;
	_swipe.sampled = now;
	if (elapsed > .1f) {
		elapsed = 0.f;
		_swipe.velocity = 0.f;
		if (_swipe.dragging) _swipe.suppressTap = true;
		_swipe.tracking = _swipe.dragging = false;
	}
	bool down = touch.count == 1;
	if (!touch.count && _swipe.suppressTap) {
		*tapped = false;
		_swipe.suppressTap = false;
	}
	if (!enabled || !count) {
		if (_swipe.dragging && touch.count) _swipe.suppressTap = true;
		_swipe.tracking = _swipe.dragging = false;
		_swipe.velocity = 0.f;
		_swipe.down = touch.count != 0;
		return false;
	}
	if (down && !_swipe.down) {
		_swipe.velocity = 0.f;
		_swipe.startX = _swipe.lastX = touch.touches[0].x;
		_swipe.finger = touch.touches[0].finger_id;
		_swipe.tracking = _inside(touch.touches[0].x, touch.touches[0].y, 88, 230, 1144, 375);
		_swipe.dragging = false;
	}
	bool moving = false;
	if (down && _swipe.tracking && touch.touches[0].finger_id == _swipe.finger) {
		unsigned x = touch.touches[0].x;
		if (abs((int) x - (int) _swipe.startX) > 10) _swipe.dragging = true;
		if (_swipe.dragging) {
			float delta = ((int) _swipe.lastX - (int) x) / (_layout ? 254.f : 224.f);
			_scroll += delta;
			if (elapsed > 0.f) {
				float speed = fmaxf(-24.f, fminf(delta / elapsed, 24.f));
				float weight = 1.f - expf(-elapsed * 25.f);
				_swipe.velocity += (speed - _swipe.velocity) * weight;
			}
			moving = true;
		}
		_swipe.lastX = x;
	} else {
		if (_swipe.dragging) *tapped = false;
		if (_swipe.dragging && touch.count) _swipe.suppressTap = true;
		if (touch.count) _swipe.velocity = 0.f;
		_swipe.tracking = _swipe.dragging = false;
		if (fabsf(_swipe.velocity) > .08f) {
			_scroll += _swipe.velocity * elapsed;
			_swipe.velocity *= expf(-5.f * elapsed);
			moving = true;
		} else _swipe.velocity = 0.f;
	}
	_swipe.down = touch.count != 0;
	if (moving) {
		float limit = (float) (count - 1);
		if (_scroll <= 0.f || _scroll >= limit) _swipe.velocity = 0.f;
		_scroll = fmaxf(0.f, fminf(_scroll, limit));
		*position = (size_t) floorf(_scroll + .5f);
		*tapped = false;
	}
	return moving;
}

static void _artworkProgress(struct GUIParams* params, const struct SwitchArtworkState* status) {
	if (!status->busy && !status->searching) return;
	SwitchUIRoundRect(700, 622, 532, 38, 6, UI_BASE);
	SwitchUIFontSize(params->font, 15);
	SwitchUIPrint(params->font, 712, 641, 508, UI_TEXT, status->message);
	SwitchUIRoundRect(712, 650, 508, 4, 2, UI_SURFACE);
	if (status->percent) {
		SwitchUIRoundRect(712, 650, 508 * status->percent / 100, 4, 2, UI_FOCUS);
	} else {
		SwitchUIRoundRect(712 + (_frames * 4) % 408, 650, 100, 4, 2, UI_FOCUS);
	}
	SwitchUIFontSize(params->font, 24);
}

static void _pickerEditSearch(struct GUIParams* params) {
	SwkbdConfig keyboard;
	if (R_FAILED(swkbdCreate(&keyboard, 0))) return;
	swkbdConfigMakePresetDefault(&keyboard);
	swkbdConfigSetHeaderText(&keyboard, SwitchArtworkGetSource() == SWITCH_ARTWORK_ROMHACKPLAZA ?
	    "Search Romhack Plaza" : SwitchArtworkGetSource() == SWITCH_ARTWORK_STEAMGRIDDB ?
	    "Search SteamGridDB" : "Search artwork databases");
	swkbdConfigSetInitialText(&keyboard, _pickerQuery);
	swkbdConfigSetStringLenMax(&keyboard, 128);
	char title[sizeof(_pickerQuery)] = { 0 };
	bool accepted = R_SUCCEEDED(swkbdShow(&keyboard, title, sizeof(title)));
	swkbdClose(&keyboard);
	if (accepted && title[strspn(title, " \t\r\n")]) {
		strlcpy(_pickerQuery, title, sizeof(_pickerQuery));
		if (_pickerPhase == 2) {
			_pickerIndex = 0;
			SwitchArtworkSearch(_pickerQuery);
			SwitchImagesInvalidate();
		}
	}
	GUIInvalidateKeys(params);
}

static void _pickerOpen(struct GUIParams* params, const char* rom, bool banner) {
	if (!_pickerPhase) SwitchArtworkSetSource(SWITCH_ARTWORK_AUTO);
	if (!SwitchArtworkOpen(rom, banner)) {
		GUIShowMessageBox(params, GUI_MESSAGE_BOX_OK, 0, "Artwork service unavailable.");
		GUIInvalidateKeys(params);
		return;
	}
	if (strcmp(_focusPath, rom)) strlcpy(_focusPath, rom, sizeof(_focusPath));
	_pickerPhase = 2;
	_pickerIndex = 0;
	_pickerType = banner ? 1 : 0;
	SwitchArtworkSearch(_pickerQuery);
	SwitchImagesInvalidate();
	GUIInvalidateKeys(params);
}

static bool _gameSettingsInput(struct GUIParams* params, uint32_t input, bool tapped, unsigned x, unsigned y) {
	if (!_gameSettingsOpen || _pickerPhase) return false;
	if (input & ((1U << GUI_INPUT_BACK) | (1U << GUI_INPUT_CANCEL))) {
		_gameSettingsOpen = false;
		GUIInvalidateKeys(params);
		return true;
	}
	if (input & (1U << GUI_INPUT_UP)) _gameSettingsIndex = (_gameSettingsIndex + 5) % 6;
	if (input & (1U << GUI_INPUT_DOWN)) _gameSettingsIndex = (_gameSettingsIndex + 1) % 6;
	bool activate = input & (1U << GUI_INPUT_SELECT);
	if (tapped && _inside(x, y, 216, 220, 848, 324)) {
		_gameSettingsIndex = (y - 220) / 54;
		activate = true;
	}
	if (activate) {
		if (_gameSettingsIndex < 2) {
			const char* title = strrchr(_gameSettingsRom, '/');
			strlcpy(_pickerQuery, title ? title + 1 : _gameSettingsRom, sizeof(_pickerQuery));
			char* extension = strrchr(_pickerQuery, '.');
			if (extension) *extension = 0;
			_pickerOpen(params, _gameSettingsRom, _gameSettingsIndex == 1);
		} else if (_gameSettingsIndex == 2) {
			if (GUIShowMessageBox(params, GUI_MESSAGE_BOX_OK | GUI_MESSAGE_BOX_CANCEL, 0,
			    "Assign matching local covers and banners to this game? Existing matched artwork will be replaced.") == GUI_MENU_EXIT_ACCEPT) {
				_gameSettingsLocalNotice = SwitchArtworkApplyLocalGame(_gameSettingsRom, false);
				if (!_gameSettingsLocalNotice) GUIShowMessageBox(params, GUI_MESSAGE_BOX_OK, 0, "Another artwork task is active. Wait or cancel it in Artwork settings.");
			}
		} else if (_gameSettingsIndex == 3) {
			SwkbdConfig keyboard;
			if (R_SUCCEEDED(swkbdCreate(&keyboard, 0))) {
				swkbdConfigMakePresetDefault(&keyboard);
				swkbdConfigSetHeaderText(&keyboard, "Save folder (existing sdmc:/ folder; blank for default)");
				swkbdConfigSetInitialText(&keyboard, _gameSettings.directory);
				swkbdConfigSetStringLenMax(&keyboard, PATH_MAX - 1);
				struct SwitchGameSettings next = _gameSettings;
				bool accepted = R_SUCCEEDED(swkbdShow(&keyboard, next.directory, sizeof(next.directory)));
				swkbdClose(&keyboard);
				if (accepted) {
					if (SwitchGameSettingsSave(_gameSettingsRom, &next)) _gameSettings = next;
					else GUIShowMessageBox(params, GUI_MESSAGE_BOX_OK, 0, "Could not save settings. Choose an existing writable sdmc:/ folder.");
				}
			}
		} else if (_gameSettingsIndex == 4) {
			const char* extensions[] = { "", ".sav", ".srm", ".dat" };
			unsigned index = 0;
			while (index < 3 && strcmp(_gameSettings.extension, extensions[index])) ++index;
			struct SwitchGameSettings next = _gameSettings;
			strlcpy(next.extension, extensions[(index + 1) % 4], sizeof(next.extension));
			if (SwitchGameSettingsSave(_gameSettingsRom, &next)) _gameSettings = next;
			else GUIShowMessageBox(params, GUI_MESSAGE_BOX_OK, 0, "Could not save game settings.");
		} else if (_gameSettingsIndex == 5) {
			struct SwitchGameSettings next = { 0 };
			if (SwitchGameSettingsSave(_gameSettingsRom, &next)) _gameSettings = next;
			else GUIShowMessageBox(params, GUI_MESSAGE_BOX_OK, 0, "Could not save game settings.");
		}
		GUIInvalidateKeys(params);
	}
	return true;
}

static void _gameSettingsDraw(struct GUIParams* params) {
	if (!_gameSettingsOpen) return;
	SwitchUIDrawRect(0, 0, 1280, 720, 0xD9000000);
	SwitchUIRoundRect(184, 100, 912, 528, 16, UI_BASE);
	SwitchUIFontSize(params->font, 28);
	SwitchUIPrint(params->font, 216, 148, 848, UI_TEXT, "Game settings");
	const char* name = strrchr(_gameSettingsRom, '/');
	SwitchUIFontSize(params->font, 20);
	SwitchUIPrint(params->font, 216, 184, 848, UI_FOCUS, name ? name + 1 : _gameSettingsRom);
	const char* labels[] = { "Change cover", "Change banner", "Assign local artwork", "Save folder", "Save file extension (raw saves only)", "Restore default save settings" };
	for (unsigned i = 0; i < 6; ++i) {
		int top = 220 + 54 * i;
		if (_gameSettingsIndex == i) SwitchUIRoundFrame(212, top, 856, 48, 8, UI_FOCUS);
		SwitchUIFontSize(params->font, 20);
		SwitchUIPrint(params->font, 232, top + 25, 808, UI_TEXT, labels[i]);
		const char* value = i == 3 ? (_gameSettings.directory[0] ? _gameSettings.directory : "Default save folder") :
		                    i == 4 ? (_gameSettings.extension[0] ? _gameSettings.extension : "Default (.sav or configured player slot)") : NULL;
		if (value) { SwitchUIFontSize(params->font, 16); SwitchUIPrint(params->font, 232, top + 48, 808, UI_MUTED, value); }
	}
	if (_gameSettingsLocalNotice) {
		struct SwitchArtworkState local;
		SwitchArtworkGetState(&local);
		SwitchUIFontSize(params->font, 16);
		SwitchUIPrint(params->font, 216, 562, 848, UI_FOCUS, local.message);
	}
	SwitchUIFontSize(params->font, 18);
	SwitchUIPrint(params->font, 216, 580, 848, UI_MUTED, "Save changes apply next launch. Existing save files are not moved or converted.");
	SwitchUIHint(params->font, 216, 612, "B", "Back");
	SwitchUIHint(params->font, 880, 612, "A", "Change");
	SwitchUIFontSize(params->font, 24);
}

static bool _artworkImageFile(const char* name) {
	const char* extension = strrchr(name, '.');
	return extension && (!strcasecmp(extension, ".png") || !strcasecmp(extension, ".jpg") || !strcasecmp(extension, ".jpeg"));
}

static void _importArtwork(struct GUIParams* params, const char* rom) {
	char currentPath[PATH_MAX], selected[PATH_MAX], game[PATH_MAX];
	strlcpy(currentPath, params->currentPath, sizeof(currentPath));
	strlcpy(game, rom, sizeof(game));
	const char* basePath = params->basePath;
	size_t fileIndex = params->fileIndex;
	params->basePath = "sdmc:/";
	strlcpy(params->currentPath, "sdmc:/", sizeof(params->currentPath));
	params->fileIndex = 0;
	_importingArtwork = true;
	GUIInvalidateKeys(params);
	bool accepted = GUISelectFile(params, selected, sizeof(selected), _artworkImageFile, NULL, NULL);
	_importingArtwork = false;
	params->basePath = basePath;
	params->fileIndex = fileIndex;
	strlcpy(params->currentPath, currentPath, sizeof(params->currentPath));
	if (accepted && !SwitchArtworkImport(game, selected, _pickerType == 1))
		GUIShowMessageBox(params, GUI_MESSAGE_BOX_OK, 0, "Could not start image import. Please try again.");
	GUIInvalidateKeys(params);
}

static bool _pickerInput(struct GUIParams* params, const char* rom, uint32_t input, u64 buttons, bool tapped, unsigned x, unsigned y) {
	struct SwitchArtworkState status;
	SwitchArtworkGetState(&status);
	if (!_pickerPhase) return false;
	if (_pickerPhase == 2 && status.applying) {
		if (input & ((1U << GUI_INPUT_BACK) | (1U << GUI_INPUT_CANCEL))) {
			SwitchArtworkCancelChoice();
			_pickerPhase = 0;
			GUIInvalidateKeys(params);
		}
		return true;
	}
	if (_pickerPhase >= 3) {
		bool back = (input & ((1U << GUI_INPUT_BACK) | (1U << GUI_INPUT_CANCEL))) ||
		    (tapped && _inside(x, y, 840, 550, 320, 64));
		bool accept = (input & (1U << GUI_INPUT_SELECT)) ||
		    (tapped && _inside(x, y, 120, 550, 650, 64));
		if (_pickerPhase == 3) {
			if (back) _pickerPhase = 2;
			else if (accept) {
				if (SwitchArtworkReset(rom, _resetBanner)) {
					SwitchArtworkGetState(&status);
					_resetWatch = status.resetId;
					_pickerPhase = 4;
				} else {
					_resetSuccess = false;
					strlcpy(_resetMessage, "Could not start the reset. Please try again.", sizeof(_resetMessage));
					_pickerPhase = 5;
				}
			}
		} else if (back || (_pickerPhase == 5 && accept)) _pickerPhase = 0;
		if (back || accept) GUIInvalidateKeys(params);
		return true;
	}
	if ((buttons & HidNpadButton_StickL) || (tapped && _inside(x, y, 120, 158, 460, 44))) {
		SwitchArtworkSetSource((SwitchArtworkGetSource() + 1) % 3);
		_pickerIndex = 0;
		if (_pickerPhase == 2) SwitchArtworkSearch(_pickerQuery);
		SwitchImagesInvalidate();
		GUIInvalidateKeys(params);
		return true;
	}
	if (_pickerPhase == 2 &&
	    ((buttons & (HidNpadButton_L | HidNpadButton_R)) ||
	     (tapped && (_inside(x, y, 120, 550, 280, 44) || _inside(x, y, 420, 550, 300, 44))))) {
		if (_resetWatch) {
			GUIShowMessageBox(params, GUI_MESSAGE_BOX_OK, 0, "An artwork reset is already in progress.");
			GUIInvalidateKeys(params);
			return true;
		}
		_resetBanner = (buttons & HidNpadButton_R) || (tapped && x >= 420);
		const char* name = strrchr(rom, '/');
		strlcpy(_resetGame, name ? name + 1 : rom, sizeof(_resetGame));
		char* extension = strrchr(_resetGame, '.');
		if (extension) *extension = 0;
		_pickerPhase = 3;
		GUIInvalidateKeys(params);
		return true;
	}
	if ((buttons & HidNpadButton_Y) || (tapped && _inside(x, y, 600, 104, 560, 48))) {
		_pickerEditSearch(params);
		return true;
	}
	if (_pickerPhase == 2 && ((buttons & HidNpadButton_Minus) || (tapped && _inside(x, y, 600, 158, 220, 44)))) {
		if (!SwitchArtworkHasKey() && !SwitchPlazaHasKey()) {
			GUIShowMessageBox(params, GUI_MESSAGE_BOX_OK, 0, "Add a SteamGridDB key in Artwork settings or a Romhack Plaza key in Settings > Romhack Plaza to download artwork.");
		} else SwitchArtworkDownloadBestFromSource(rom, _pickerQuery, SwitchArtworkGetSource());
		GUIInvalidateKeys(params);
		return true;
	}
	if (_pickerPhase == 2 && ((buttons & HidNpadButton_X) || (tapped && _inside(x, y, 740, 550, 420, 44)))) {
		_importArtwork(params, rom);
		return true;
	}
	if (_pickerPhase == 2) {
		if ((buttons & HidNpadButton_ZL) || (tapped && _inside(x, y, 830, 158, 158, 44))) {
			_pickerOpen(params, rom, false);
			return true;
		}
		if ((buttons & HidNpadButton_ZR) || (tapped && _inside(x, y, 1002, 158, 158, 44))) {
			_pickerOpen(params, rom, true);
			return true;
		}
	}
	if ((input & ((1U << GUI_INPUT_BACK) | (1U << GUI_INPUT_CANCEL))) ||
	    (tapped && _inside(x, y, 840, 570, 320, 64))) {
		SwitchArtworkCancelChoice();
		_pickerPhase = 0;
		GUIInvalidateKeys(params);
		return true;
	}
	if (_pickerPhase == 1) {
		if ((input & (1U << GUI_INPUT_SELECT)) || (tapped && _inside(x, y, 120, 570, 650, 64))) {
			SwitchArtworkSearch(_pickerQuery);
			_pickerPhase = 2;
		}
		return true;
	}
	if (status.count) {
		if (input & ((1U << GUI_INPUT_LEFT) | (1U << GUI_INPUT_UP)))
			_pickerIndex = (_pickerIndex + status.count - 1) % status.count;
		if (input & ((1U << GUI_INPUT_RIGHT) | (1U << GUI_INPUT_DOWN)))
			_pickerIndex = (_pickerIndex + 1) % status.count;
		if (_pickerIndex >= status.count) _pickerIndex = status.count - 1;
		bool choose = input & (1U << GUI_INPUT_SELECT);
		if (tapped && _inside(x, y, 120, 240, 1040, 304)) {
			size_t item = (_pickerIndex / 3) * 3 + (x - 120) / 350;
			if (item < status.count) { choose = _pickerIndex == item; _pickerIndex = item; }
		}
		if (tapped && _inside(x, y, 120, 600, 650, 34)) choose = true;
		if (choose) {
			SwitchArtworkChoose(_pickerIndex);
			GUIInvalidateKeys(params);
		}
	}
	return true;
}

static void _resetNoticeDraw(struct GUIParams* params) {
	if (armTicksToNs(armGetSystemTick()) >= _resetNoticeUntil) return;
	SwitchUIRoundRect(480, 8, 768, 48, 8, UI_SURFACE);
	SwitchUIRoundFrame(480, 8, 768, 48, 8, UI_FOCUS);
	SwitchUIFontSize(params->font, 18);
	SwitchUIPrint(params->font, 496, 39, 736, UI_TEXT, _resetNotice);
	SwitchUIFontSize(params->font, 24);
}

static void _resetDraw(struct GUIParams* params) {
	SwitchUIDrawRect(0, 0, 1280, 720, 0xC8000000);
	SwitchUIRoundRect(96, 92, 1088, 552, 16, UI_BASE);
	SwitchUIRoundFrame(96, 92, 1088, 552, 16, UI_FOCUS);
	SwitchUIFontSize(params->font, 30);
	SwitchUIPrint(params->font, 144, 162, 976, UI_TEXT,
	              _pickerPhase == 3 ? "Reset artwork?" : _pickerPhase == 4 ? "Resetting artwork..." :
	              _resetSuccess ? "Artwork reset succeeded" : "Artwork reset failed");
	SwitchUIFontSize(params->font, 28);
	_wrapTitle(params->font, 144, 246, 976, _resetGame, 2, UI_TEXT);
	SwitchUIFontSize(params->font, 22);
	SwitchUIPrint(params->font, 144, 344, 976, UI_FOCUS, _resetBanner ? "Artwork type: Banner" : "Artwork type: Cover");
	SwitchUIFontSize(params->font, 18);
	SwitchUIPrint(params->font, 144, 400, 976, UI_TEXT,
	              _pickerPhase == 3 ? "Remove this replacement and restore the original artwork or fallback?" :
	              _pickerPhase == 4 ? "Working in the background. You can return to the library." : _resetMessage);
	if (_pickerPhase == 3) {
		SwitchUIPrint(params->font, 144, 442, 976, UI_MUTED, "Local originals and automatically downloaded artwork will be kept.");
		SwitchUIHint(params->font, 144, 588, "A", "Confirm reset");
		SwitchUIHint(params->font, 900, 588, "B", "Cancel");
	} else if (_pickerPhase == 4) {
		SwitchUIRoundRect(144, 448, 976, 6, 3, UI_SURFACE);
		unsigned offset = (armTicksToNs(armGetSystemTick()) / 10000000ULL) % 816;
		SwitchUIRoundRect(144 + offset, 448, 160, 6, 3, UI_FOCUS);
		SwitchUIHint(params->font, 900, 588, "B", "Library");
	} else {
		SwitchUIHint(params->font, 144, 588, "A", "OK");
		SwitchUIHint(params->font, 900, 588, "B", "Library");
	}
	SwitchUIFontSize(params->font, 24);
}

static void _pickerDraw(struct GUIParams* params) {
	struct SwitchArtworkState status;
	SwitchArtworkGetState(&status);
	if (_resetWatch && status.resetDone == _resetWatch) {
		_resetWatch = 0;
		_resetSuccess = status.resetSucceeded;
		strlcpy(_resetMessage, _resetSuccess ? "Original artwork or fallback restored." :
		        "Could not remove the replacement. Check SD card access and try again.", sizeof(_resetMessage));
		snprintf(_resetNotice, sizeof(_resetNotice), "%s reset %s: %s", _resetBanner ? "Banner" : "Cover",
		         _resetSuccess ? "succeeded" : "failed", _resetGame);
		_resetNoticeUntil = armTicksToNs(armGetSystemTick()) + 4000000000ULL;
		if (_pickerPhase == 4) _pickerPhase = 5;
	}
	if (status.revision != _artworkRevision) {
		_artworkRevision = status.revision;
		SwitchImagesInvalidate();
		_clearTextures();
	}
	if (!_pickerPhase) { _artworkProgress(params, &status); _resetNoticeDraw(params); return; }
	if (_pickerPhase >= 3) { _resetDraw(params); return; }
	SwitchUIDrawRect(0, 0, 1280, 720, 0xC8000000);
	SwitchUIRoundRect(96, 92, 1088, 552, 16, UI_BASE);
	SwitchUIRoundFrame(96, 92, 1088, 552, 16, UI_FOCUS);
	SwitchUIFontSize(params->font, 28);
	SwitchUIPrint(params->font, 120, 138, 460, UI_TEXT, _pickerPhase == 1 ? "Find alternative artwork?" : "Choose artwork");
	SwitchUIFontSize(params->font, 18);
	SwitchUIRoundRect(600, 104, 560, 48, 8, UI_SURFACE);
	SwitchUIRoundFrame(600, 104, 560, 48, 8, UI_FOCUS);
	char search[560];
	snprintf(search, sizeof(search), "Y  Search: %s", _pickerQuery);
	SwitchUIPrint(params->font, 616, 135, 528, UI_TEXT, search);
	const char* databases[] = { "Auto", "SteamGridDB", "Romhack Plaza" };
	char database[80];
	snprintf(database, sizeof(database), "L3  Database: %s", databases[SwitchArtworkGetSource()]);
	_button(params->font, 120, 158, 460, database, false);
	if (_pickerPhase == 1) {
		SwitchUIPrint(params->font, 120, 256, 1016, UI_TEXT, "No automatic artwork match was found for this game.");
		SwitchUIPrint(params->font, 120, 296, 1016, UI_MUTED, "Choose a database, edit the title, and preview alternatives before applying.");
		SwitchUIHint(params->font, 136, 612, "A", "Search database");
	} else {
		SwitchUIFontSize(params->font, 16);
		SwitchUIPrint(params->font, 120, 226, 1040, UI_MUTED, status.message);
		_button(params->font, 600, 158, 220, "-  Download best", false);
		_button(params->font, 830, 158, 158, "ZL  Cover", _pickerType == 0);
		_button(params->font, 1002, 158, 158, "ZR  Banner", _pickerType == 1);
		size_t first = (_pickerIndex / 3) * 3;
		for (size_t i = first; i < status.count && i < first + 3; ++i) {
			struct SwitchArtworkOption option;
			if (!SwitchArtworkGetOption(i, &option)) continue;
			int x = 120 + (i - first) * 350;
			SwitchUIRoundRect(x, 240, 330, 304, 9, UI_SURFACE);
			if (i == _pickerIndex) SwitchUIRoundFrame(x, 240, 330, 304, 9, UI_FOCUS);
			int texture = option.preview[0] ? _texture(option.preview, false, true) : -1;
			if (texture >= 0) {
				float scale = fminf(302.f / _textures[texture].width, 212.f / _textures[texture].height);
				int w = _textures[texture].width * scale, h = _textures[texture].height * scale;
				_quadShape(_textures[texture].texture, x + (330 - w) / 2, 250 + (212 - h) / 2, w, h, 0xFFFFFFFF, 1, 1, 6, 0);
			} else {
				bool failed = false;
				for (unsigned j = 0; j < 8; ++j)
					if (!strcmp(_textures[j].path, option.preview) && _textures[j].ready && _textures[j].failed) failed = true;
				SwitchUIFontSize(params->font, 18);
				SwitchUIPrint(params->font, x + 18, 344, 294, UI_MUTED, failed ? "Preview unavailable" : "Loading preview...");
			}
			SwitchUIFontSize(params->font, 18);
			SwitchUIPrint(params->font, x + 14, 495, 302, UI_TEXT, option.title);
			SwitchUIPrint(params->font, x + 14, 525, 302, UI_MUTED, option.banner ? "Use as banner" : "Use as cover");
		}
		if (!status.count) {
			SwitchUIFontSize(params->font, 20);
			SwitchUIPrint(params->font, 120, 338, 1016, UI_TEXT, status.finished ? "No suggestions available. Press Y to edit the search title, or B to cancel." : "Searching... Results will appear here as they arrive.");
		}
		if (status.count) {
			SwitchUIHint(params->font, 136, 624, "A", "Use selected artwork");
			SwitchUIHint(params->font, 500, 624, "< >", "Browse");
		}
		_button(params->font, 120, 550, 280, "L  Reset cover", false);
		_button(params->font, 420, 550, 300, "R  Reset banner", false);
		_button(params->font, 740, 550, 420, "X  Import image from SD", false);
	}
	SwitchUIHint(params->font, 868, 612, "B", "Cancel");
	SwitchUIFontSize(params->font, 24);
	_resetNoticeDraw(params);
}

static bool _artworkPages(struct GUIParams* params, struct GUIMenu* menu, struct GUIMenuState* state,
                          enum GUIMenuExitReason* result) {
	bool local = menu == &_localArtworkMenu;
	struct SwitchArtworkState progress;
	SwitchArtworkGetState(&progress);
	unsigned rows = local && progress.local ? 3 : 2;
	if (menu->index >= rows) menu->index = rows - 1;
	uint32_t input = 0;
	GUIPollInput(params, &input, NULL);
	unsigned x, y;
	bool tap = _tap(params, &x, &y);
	if ((input & ((1U << GUI_INPUT_BACK) | (1U << GUI_INPUT_CANCEL))) ||
	    (tap && _inside(x, y, 48, 664, 200, 56))) {
		if (_runner->core) SwitchArtworkSuspend();
		*result = GUI_MENU_EXIT_BACK;
		return true;
	}
	if (input & (1U << GUI_INPUT_UP)) menu->index = (menu->index + rows - 1) % rows;
	if (input & (1U << GUI_INPUT_DOWN)) menu->index = (menu->index + 1) % rows;
	bool activate = input & (1U << GUI_INPUT_SELECT);
	if (tap && _inside(x, y, 408, 226, 784, rows * 72)) {
		unsigned row = (y - 226) / 72;
		if (y < 226 + row * 72 + 44) { menu->index = row; activate = true; }
	}
	if (activate) {
		if (!local) {
			state->resultItem = GUIMenuItemListGetPointer(&menu->items, menu->index);
			*result = GUI_MENU_ENTER;
			return true;
		}
		if (menu->index == 2) SwitchArtworkCancelLocal();
		else if (!progress.busy && !progress.bulk && !progress.applying && !progress.searching) {
			bool accepted = menu->index || GUIShowMessageBox(params, GUI_MESSAGE_BOX_OK | GUI_MESSAGE_BOX_CANCEL, 0,
			    "Assign local covers and banners to all games? Existing artwork is replaced only when a local image matches.") == GUI_MENU_EXIT_ACCEPT;
			if (accepted) {
				const char* roots[16];
				size_t count = SwitchUIRomDirectoriesGet(&_runner->config, roots, 16);
				if (!count) { roots[0] = params->currentPath; count = 1; }
				if (!SwitchArtworkApplyLocal(roots, count, menu->index == 1))
					GUIShowMessageBox(params, GUI_MESSAGE_BOX_OK, 0, "Could not start local artwork assignment. Wait for the current artwork task to finish.");
			}
		} else GUIShowMessageBox(params, GUI_MESSAGE_BOX_OK, 0, "An artwork task is active. Wait for it to finish.");
		GUIInvalidateKeys(params);
	}
	SwitchArtworkGetState(&progress);
	params->drawStart(); params->guiPrepare();
	SwitchUIDrawRect(0, 0, 1280, 720, UI_BASE);
	SwitchUIFontSize(params->font, 30);
	SwitchUIPrint(params->font, 48, 66, 300, UI_TEXT, "Settings");
	SwitchUIRoundRect(40, 140, 280, 48, 8, UI_SURFACE);
	SwitchUIFontSize(params->font, 22);
	SwitchUIPrint(params->font, 62, 172, 238, UI_FOCUS, "Artwork");
	SwitchUIDrawRect(360, 112, 1, 512, 0xFF38332D);
	SwitchUIFontSize(params->font, 28);
	SwitchUIPrint(params->font, 408, 142, 784, UI_TEXT, local ? "Local artwork" : "Artwork");
	SwitchUIFontSize(params->font, 18);
	SwitchUIPrint(params->font, 408, 184, 784, UI_MUTED,
	              local ? "Shared image directory: /mGBA/artwork" : "Choose an artwork source.");
	_button(params->font, 408, 226, 784, local ? "Apply to all" : "SteamGridDB", menu->index == 0);
	_button(params->font, 408, 298, 784, local ? "Apply to all missing artwork" : "Local", menu->index == 1);
	if (local) {
		if (progress.local) _button(params->font, 408, 370, 784, "Cancel local assignment", menu->index == 2);
		SwitchUIFontSize(params->font, 18);
		SwitchUIPrint(params->font, 408, 450, 784, UI_MUTED, "Match image names to ROM titles. Ambiguous matches are skipped.");
		SwitchUIPrint(params->font, 408, 484, 784, UI_MUTED, "Use Game.cover.png or Game.banner.jpg; PNG, JPG and JPEG work.");
		SwitchUIPrint(params->font, 408, 518, 784, UI_MUTED, "Scans every configured ROM folder. Per-game choices are in Game Settings.");
		SwitchUIPrint(params->font, 408, 572, 784, UI_FOCUS, progress.message);
		if (progress.local) {
			char scope[96];
			snprintf(scope, sizeof(scope), "%u / %u games processed", progress.bulkDone, progress.bulkTotal);
			SwitchUIPrint(params->font, 408, 610, 784, UI_MUTED, scope);
			unsigned percent = progress.bulkTotal ? 100 * progress.bulkDone / progress.bulkTotal : progress.percent;
			if (percent > 100) percent = 100;
			SwitchUIRoundRect(408, 590, 784, 4, 2, UI_SURFACE);
			if (percent) SwitchUIRoundRect(408, 590, 784 * percent / 100, 4, 2, UI_FOCUS);
		}
	}
	SwitchUIDrawRect(48, 658, 1184, 1, 0xFF38332D);
	SwitchUIHint(params->font, 48, 696, "B", "Back");
	SwitchUIHint(params->font, 1040, 696, "A", "Select");
	SwitchUIFontSize(params->font, 24);
	params->guiFinish(); params->drawEnd();
	*result = GUI_MENU_CONTINUE;
	return true;
}

static bool _artwork(struct GUIParams* params, struct GUIMenu* menu, enum GUIMenuExitReason* result) {
	struct SwitchArtworkState progress;
	SwitchArtworkGetState(&progress);
	if (!_inArtwork) {
		SwitchArtworkSetDownloads(_covers, _banners);
		_inArtwork = true;
	}
	uint32_t input = 0;
	GUIPollInput(params, &input, NULL);
	if (input & ((1U << GUI_INPUT_BACK) | (1U << GUI_INPUT_CANCEL))) {
		if (_runner->core)
			SwitchArtworkSuspend();
		_inArtwork = false;
		*result = GUI_MENU_EXIT_BACK;
		return true;
	}
	if (input & (1U << GUI_INPUT_UP))
		menu->index = (menu->index + 5) % 6;
	if (input & (1U << GUI_INPUT_DOWN))
		menu->index = (menu->index + 1) % 6;
	bool activate = input & (1U << GUI_INPUT_SELECT);
	if (input & ((1U << GUI_INPUT_LEFT) | (1U << GUI_INPUT_RIGHT))) {
		if (menu->index == 1 || menu->index == 2)
			menu->index = menu->index == 1 ? 2 : 1;
		else if (menu->index == 3 || menu->index == 4)
			activate = true;
	}
	unsigned x, y;
	if (_tap(params, &x, &y)) {
		if (_inside(x, y, 1028, 218, 164, 44)) {
			menu->index = 0;
			activate = true;
		}
		if (_inside(x, y, 408, 286, 224, 44)) {
			menu->index = 1;
			activate = true;
		}
		if (_inside(x, y, 652, 286, 184, 44)) {
			menu->index = 2;
			activate = true;
		}
		if (_inside(x, y, 400, 350, 808, 70)) {
			menu->index = 3;
			activate = true;
		}
		if (_inside(x, y, 400, 436, 808, 70)) {
			menu->index = 4;
			activate = true;
		}
		if (_inside(x, y, 408, 532, 784, 44)) { menu->index = 5; activate = true; }
		if (_inside(x, y, 48, 664, 200, 56))
			input |= 1U << GUI_INPUT_BACK;
	}
	if (input & (1U << GUI_INPUT_BACK)) {
		if (_runner->core)
			SwitchArtworkSuspend();
		_inArtwork = false;
		*result = GUI_MENU_EXIT_BACK;
		return true;
	}
	if (activate && progress.local) {
		GUIShowMessageBox(params, GUI_MESSAGE_BOX_OK, 0, "Local artwork assignment is active. Wait for it to finish, or cancel it on the Local page.");
		GUIInvalidateKeys(params);
		activate = false;
	}
	if (activate) {
		switch (menu->index) {
		case 0:
			_editKey(params);
			break;
		case 1:
			SwitchArtworkTest();
			break;
		case 2:
			if (SwitchArtworkHasKey() &&
			    GUIShowMessageBox(params, GUI_MESSAGE_BOX_OK | GUI_MESSAGE_BOX_CANCEL, 0,
			                      "Remove the saved SteamGridDB API key?") == GUI_MENU_EXIT_ACCEPT &&
			    !SwitchArtworkSetKey(""))
				GUIShowMessageBox(params, GUI_MESSAGE_BOX_OK, 0, "Could not remove the saved key.");
			break;
		case 3:
			_covers = !_covers;
			_saveArtwork();
			break;
		case 4:
			_banners = !_banners;
			_saveArtwork();
			break;
		case 5:
			if (progress.bulk) SwitchArtworkCancelBulk();
			else if (!SwitchArtworkHasKey() && !SwitchPlazaHasKey()) {
				GUIShowMessageBox(params, GUI_MESSAGE_BOX_OK, 0, "Add a SteamGridDB or Romhack Plaza API key before downloading library artwork.");
				GUIInvalidateKeys(params);
			} else SwitchArtworkDownloadMissing(params->currentPath);
			break;
		}
	}
	SwitchArtworkGetState(&progress);
	params->drawStart();
	params->guiPrepare();
	SwitchUIDrawRect(0, 0, 1280, 720, UI_BASE);
	SwitchUIFontSize(params->font, 30);
	SwitchUIPrint(params->font, 48, 66, 300, UI_TEXT, "Settings");
	SwitchUIFontSize(params->font, 22);
	SwitchUIRoundRect(40, 140, 280, 48, 8, UI_SURFACE);
	SwitchUIPrint(params->font, 62, 172, 238, UI_FOCUS, "Artwork");
	SwitchUIDrawRect(360, 112, 1, 512, 0xFF38332D);
	SwitchUIFontSize(params->font, 28);
	SwitchUIPrint(params->font, 408, 142, 784, UI_TEXT, "SteamGridDB");
	SwitchUIFontSize(params->font, 18);
	SwitchUIPrint(params->font, 408, 184, 784, UI_MUTED, "SteamGridDB  /  optional artwork downloads");
	SwitchUIPrint(params->font, 408, 212, 600, UI_MUTED, "API key");
	SwitchUIRoundRect(408, 226, 592, 40, 6, UI_SURFACE);
	SwitchUIFontSize(params->font, 22);
	SwitchUIPrint(params->font, 424, 254, 556, UI_TEXT,
	              SwitchArtworkHasKey() ? "************************" : "No API key saved");
	_button(params->font, 1028, 224, 164, "Edit key", menu->index == 0);
	_button(params->font, 408, 286, 224, "Test connection", menu->index == 1);
	_button(params->font, 652, 286, 184, "Remove key", menu->index == 2);
	const char* labels[] = { "Download game artwork", "Download game banners" };
	const char* descriptions[] = { "Portrait covers for the game library.",
	                               "Background artwork for the focused game." };
	for (unsigned i = 0; i < 2; ++i) {
		int top = 350 + i * 86;
		if (menu->index == i + 3)
			SwitchUIRoundRect(396, top - 8, 816, 80, 8, UI_SURFACE);
		SwitchUIFontSize(params->font, 22);
		SwitchUIPrint(params->font, 408, top + 24, 660, UI_TEXT, labels[i]);
		SwitchUIFontSize(params->font, 18);
		SwitchUIPrint(params->font, 408, top + 58, 680, UI_MUTED, descriptions[i]);
		bool enabled = i ? _banners : _covers;
		SwitchUIRoundRect(1120, top + 6, 64, 32, 16, enabled ? UI_FOCUS : 0xFF645E56);
		SwitchUIRoundRect(enabled ? 1154 : 1124, top + 10, 24, 24, 12, 0xFFF5F3EF);
		if (menu->index == i + 3)
			SwitchUIRoundFrame(1114, top, 76, 44, 20, UI_FOCUS);
	}
	_button(params->font, 408, 532, 784, progress.local ? "Local assignment active (manage in Local)" : progress.bulk ? "Cancel library artwork downloads" :
	        "Download all missing covers and banners", menu->index == 5);
	char status[160];
	SwitchArtworkStatus(status, sizeof(status));
	SwitchUIFontSize(params->font, 18);
	SwitchUIPrint(params->font, 408, 598, 784, UI_FOCUS, status);
	char scope[160];
	if (progress.local) snprintf(scope, sizeof(scope), "Local assignment is active; open Local to view progress or cancel.");
	else if (progress.bulk) snprintf(scope, sizeof(scope), "%u / %u games processed — downloads continue while browsing", progress.bulkDone, progress.bulkTotal);
	else snprintf(scope, sizeof(scope), "Scans the current library folder and subfolders. Existing images are kept.");
	SwitchUIPrint(params->font, 408, 630, 784, UI_MUTED, scope);
	SwitchUIDrawRect(48, 658, 1184, 1, 0xFF38332D);
	SwitchUIHint(params->font, 48, 696, "B", "Back");
	SwitchUIHint(params->font, 1040, 696, "A", "Select");
	SwitchUIFontSize(params->font, 24);
	SwitchArtworkGetState(&progress);
	if (progress.busy || progress.searching || progress.bulk) {
		unsigned percent = progress.bulk && progress.bulkTotal ? 100 * progress.bulkDone / progress.bulkTotal : progress.percent;
		if (percent > 100) percent = 100;
		SwitchUIRoundRect(408, 610, 784, 4, 2, UI_SURFACE);
		if (percent) SwitchUIRoundRect(408, 610, 784 * percent / 100, 4, 2, UI_FOCUS);
	}
	params->guiFinish();
	params->drawEnd();
	*result = GUI_MENU_CONTINUE;
	return true;
}

static void _fallback(struct GUIFont* font, const struct GUIMenuItem* entry, int x, int y, int w, int h) {
	const char* platform = _platform(entry->title);
	uint32_t accent = !strcmp(platform, "GBA") ? 0xFFB9A98D : !strcmp(platform, "GBC") ? 0xFFB3B88C : 0xFFB8AAA0;
	SwitchUIRoundRect(x, y, w, h, 8, UI_SURFACE);
	SwitchUIDrawRect(x + 16, y + 18, 32, 3, accent);
	SwitchUIFontSize(font, 18);
	SwitchUIPrint(font, x + 16, y + 52, w - 32, accent,
	              *platform                  ? platform :
	                  _archive(entry->title) ? "ARCHIVE" :
	                                           "GAME");
	_cartridge(x + (w - 58) / 2, y + h / 3, 58, 0xFF5D5750);
	char title[512];
	_title(entry, title, sizeof(title));
	SwitchUIFontSize(font, 22);
	_wrapTitle(font, x + 18, y + h - 90, w - 36, title, 3, UI_TEXT);
}

static void _libraryHeader(struct GUIParams* params, bool files) {
	(void) files;
	for (unsigned i = 0; i < 3; ++i) {
		int x = 514 + i * 88;
		bool enabled = (_filter & (1U << i)) != 0;
		uint32_t ink = enabled ? UI_TEXT : UI_MUTED;
		SwitchUIRoundRect(x, 22, 76, 60, 12, enabled ? 0xFF9B5990 : UI_SURFACE);
		if (_filterFocused && _filterIndex == i)
			SwitchUIRoundFrame(x - 3, 19, 82, 66, 14, UI_TEXT);
		if (!i) {
			SwitchUIRoundRect(x + 13, 39, 50, 28, 10, ink);
			SwitchUIRoundRect(x + 27, 44, 24, 18, 2, UI_BASE);
			SwitchUIDrawRect(x + 17, 51, 8, 3, UI_BASE);
			SwitchUIDrawRect(x + 20, 48, 3, 9, UI_BASE);
			SwitchUIRoundRect(x + 54, 49, 4, 4, 2, UI_BASE);
		} else {
			SwitchUIRoundRect(x + 24, 31, 28, 43, i == 1 ? 6 : 3, ink);
			SwitchUIRoundRect(x + 28, 35, 20, 18, 2, UI_BASE);
			SwitchUIDrawRect(x + 28, 61, 9, 3, UI_BASE);
			SwitchUIDrawRect(x + 31, 58, 3, 9, UI_BASE);
			SwitchUIRoundRect(x + 42, 59, 4, 4, 2, UI_BASE);
			if (i == 1) SwitchUIRoundRect(x + 46, 56, 3, 3, 1, UI_BASE);
		}
	}
	SwitchUIFontSize(params->font, 24);
}

static void _libraryFooter(struct GUIParams* params) {
	SwitchUIDrawRect(72, 668, 1208, 52, 0xD4211E1B);
	SwitchUIHint(params->font, 1032, 701, "X", "Game Settings");
}

static struct GUIMenu* _activeLibrary(struct GUIParams* params, struct GUIMenu* files) {
	if (_romDirectoryChanged) {
		_romDirectoryChanged = false;
		_romLibraryDirty = true;
	}
	const char* roots[16];
	size_t count = SwitchUIRomDirectoriesGet(&_runner->config, roots, 16);
	if (_files || !count) return files;
	if (_romLibraryDirty) {
		_romLibraryDirty = false;
		char selected[PATH_MAX] = {0};
		const char* previous = mCoreConfigGetValue(&_runner->config, "lastGame");
		if (_romLibraryMenu.index < GUIMenuItemListSize(&_romLibraryMenu.items)) {
			const struct GUIMenuItem* item = GUIMenuItemListGetConstPointer(&_romLibraryMenu.items, _romLibraryMenu.index);
			if (GUIVariantCompareUInt(item->data, VFS_FILE)) previous = item->title;
		}
		if (previous) strlcpy(selected, previous, sizeof(selected));
		struct GUIParams scan = *params;
		scan.fileIndex = _romLibraryMenu.index;
		size_t skipped = 0;
		if (!SwitchUIFilesRefreshRoots(&scan, &_romLibraryMenu, roots, count, &skipped)) {
			GUIShowMessageBox(params, GUI_MESSAGE_BOX_OK, 0, "ROM library refresh stopped. Previous results were kept. Open ROM directories to retry.");
		} else if (skipped) {
			GUIShowMessageBox(params, GUI_MESSAGE_BOX_OK, 0, "%zu ROM directories could not be read and were skipped. Check them in File System settings.", skipped);
		}
		for (size_t i = 1; *selected && i < GUIMenuItemListSize(&_romLibraryMenu.items); ++i) {
			const char* path = GUIMenuItemListGetConstPointer(&_romLibraryMenu.items, i)->title;
			const char* match = !strncmp(selected, "sdmc:/", 6) ? selected + 5 : selected;
			if (!strncmp(path, "sdmc:/", 6)) path += 5;
			if (!strcmp(path, match)) { _romLibraryMenu.index = i; break; }
		}
		_library.menu = NULL;
		_lastSelection = SIZE_MAX;
	}
	return &_romLibraryMenu;
}
static bool _fileSystem(struct GUIParams* params, enum GUIMenuExitReason* result) {
	static const char* keys[] = { "switch.romDirectory", "savegamePath" };
	static const char* labels[] = { "ROM directories", "Saves location" };
	uint32_t input = 0;
	GUIPollInput(params, &input, NULL);
	unsigned x, y;
	bool tapped = _tap(params, &x, &y);
	if (tapped && _inside(x, y, 48, 664, 180, 56)) input |= 1U << GUI_INPUT_BACK;
	if (input & ((1U << GUI_INPUT_BACK) | (1U << GUI_INPUT_CANCEL))) {
		*result = GUI_MENU_EXIT_BACK;
		return true;
	}
	if (input & ((1U << GUI_INPUT_UP) | (1U << GUI_INPUT_DOWN))) _fileSystemIndex ^= 1;
	bool activate = input & (1U << GUI_INPUT_SELECT);
	if (tapped && _inside(x, y, 400, 160, 808, 192)) {
		_fileSystemIndex = (y - 160) / 96;
		activate = true;
	}
	if (tapped && _inside(x, y, 850, 664, 380, 56)) activate = true;
	if (activate && !_fileSystemIndex) {
		if (SwitchUIRomDirectoriesEdit(params, &_runner->config)) _romDirectoryChanged = true;
		GUIInvalidateKeys(params);
	} else if (activate) {
		const char* key = keys[_fileSystemIndex];
		const char* configured = mCoreConfigGetValue(&_runner->config, key);
		char old[PATH_MAX], path[PATH_MAX];
		strlcpy(old, configured ? configured : "", sizeof(old));
		strlcpy(path, configured ? configured : (_fileSystemIndex ? "" : params->currentPath), sizeof(path));
		if (SwitchUISelectDirectory(params, *path ? path : params->currentPath, path, sizeof(path))) {
			bool valid = !strncmp(path, "sdmc:/", 6) && (!_fileSystemIndex || !access(path, W_OK));
			if (!valid) {
				GUIShowMessageBox(params, GUI_MESSAGE_BOX_OK, 0, "The selected saves folder must be writable.");
			} else {
				/* Keep absolute SD paths compatible with upstream directory options. */
				mCoreConfigSetValue(&_runner->config, key, path + 5);
				if (!mCoreConfigSave(&_runner->config)) {
					mCoreConfigSetValue(&_runner->config, key, *old ? old : NULL);
					GUIShowMessageBox(params, GUI_MESSAGE_BOX_OK, 0, "Could not save the folder setting. The previous location is unchanged.");
				} else {
					if (!_fileSystemIndex) _romDirectoryChanged = true;
					GUIShowMessageBox(params, GUI_MESSAGE_BOX_OK, 0, _fileSystemIndex ?
					    "Save location updated for the next game launch. Existing saves were not moved. Per-game save folders still take priority." :
					    "ROM location updated. The library will refresh when you return.");
				}
			}
		}
		GUIInvalidateKeys(params);
	}
	params->drawStart();
	params->guiPrepare();
	SwitchUIDrawRect(0, 0, 1280, 720, UI_BASE);
	SwitchUIFontSize(params->font, 30);
	SwitchUIPrint(params->font, 48, 66, 300, UI_TEXT, "Settings");
	SwitchUIFontSize(params->font, 22);
	SwitchUIRoundRect(40, 140, 280, 48, 8, UI_SURFACE);
	SwitchUIPrint(params->font, 62, 172, 238, UI_FOCUS, "File System");
	for (unsigned i = 0; i < 2; ++i) {
		int top = 160 + i * 96;
		SwitchUIRoundRect(400, top, 808, 84, 8, UI_SURFACE);
		if (i == _fileSystemIndex) SwitchUIRoundFrame(400, top, 808, 84, 8, UI_FOCUS);
		SwitchUIFontSize(params->font, 22);
		SwitchUIPrint(params->font, 420, top + 30, 760, UI_TEXT, labels[i]);
		const char* value = mCoreConfigGetValue(&_runner->config, keys[i]);
		char count[48];
		if (!i) {
			const char* roots[16];
			size_t total = SwitchUIRomDirectoriesGet(&_runner->config, roots, 16);
			snprintf(count, sizeof(count), "%zu configured %s", total, total == 1 ? "folder" : "folders");
			value = total ? count : "Default: upstream file browser";
		}
		if (!value || !*value) value = i ? "Default: beside each ROM" : params->currentPath;
		SwitchUIFontSize(params->font, 18);
		SwitchUIPrint(params->font, 420, top + 62, 760, UI_MUTED, value);
	}
	SwitchUIPrint(params->font, 420, 398, 760, UI_MUTED, "Browse SD folders, then choose Use This Directory.");
	SwitchUIPrint(params->font, 420, 434, 760, UI_MUTED, "Existing files stay in place. Per-game save folders take priority.");
	SwitchUIHint(params->font, 48, 699, "B", "Back");
	SwitchUIHint(params->font, 860, 699, "A", "Change folder");
	SwitchUIFontSize(params->font, 24);
	params->guiFinish();
	params->drawEnd();
	*result = GUI_MENU_CONTINUE;
	return true;
}

bool SwitchUILibraryMenu(struct GUIParams* params, struct GUIMenu* menu, struct GUIMenuState* state,
                         enum GUIMenuExitReason* result) {
	if (_importingArtwork) return false;
	if (menu == SwitchUIPlazaMenu()) { SwitchUIPlazaOpen(params, &_runner->config); *result = GUI_MENU_EXIT_BACK; return true; }
	if (menu == &_fileSystemMenu) return _fileSystem(params, result);
	if (menu == SwitchUIAchievementsMenu()) return SwitchUIAchievementsRun(params, result);
	if (menu == SwitchUIUpdatesMenu()) return SwitchUIUpdatesRun(params, result);
	if (menu == &_interfaceMenu) {
		uint32_t input = 0;
		GUIPollInput(params, &input, NULL);
		unsigned x, y;
		bool tap = _tap(params, &x, &y);
		if (tap && _inside(x, y, 48, 664, 180, 56))
			input |= 1U << GUI_INPUT_BACK;
		if (tap && _inside(x, y, 850, 664, 380, 56))
			input |= 1U << GUI_INPUT_SELECT;
		if (input & ((1U << GUI_INPUT_BACK) | (1U << GUI_INPUT_CANCEL))) {
			*result = GUI_MENU_EXIT_BACK;
			return true;
		}
		if ((input & ((1U << GUI_INPUT_SELECT) | (1U << GUI_INPUT_LEFT) | (1U << GUI_INPUT_RIGHT))) ||
		    (tap && _inside(x, y, 400, 160, 808, 64))) {
			_layout = !_layout;
			mCoreConfigSetUIntValue(&_runner->config, "switch.interface.layout", _layout);
			mCoreConfigSave(&_runner->config);
		}
		params->drawStart();
		params->guiPrepare();
		SwitchUIDrawRect(0, 0, 1280, 720, UI_BASE);
		SwitchUIFontSize(params->font, 30);
		SwitchUIPrint(params->font, 48, 66, 300, UI_TEXT, "Settings");
		SwitchUIFontSize(params->font, 22);
		SwitchUIRoundRect(40, 140, 280, 48, 8, UI_SURFACE);
		SwitchUIPrint(params->font, 62, 172, 238, UI_FOCUS, "Interface");
		SwitchUIRoundFrame(400, 160, 808, 64, 8, UI_FOCUS);
		SwitchUIPrint(params->font, 420, 202, 480, UI_TEXT, "Library layout");
		GUIFontPrint(params->font, 1184, 202, GUI_ALIGN_RIGHT, UI_FOCUS, _layout ? "Console" : "Focus Shelf");
		SwitchUIFontSize(params->font, 18);
		SwitchUIPrint(params->font, 420, 268, 760, UI_MUTED, "Console uses the left-anchored game artwork layout.");
		SwitchUIHint(params->font, 48, 699, "B", "Back");
		SwitchUIHint(params->font, 860, 699, "A", "Change layout");
		SwitchUIFontSize(params->font, 24);
		params->guiFinish();
		params->drawEnd();
		*result = GUI_MENU_CONTINUE;
		return true;
	}
	if (menu == &_steamArtworkMenu)
		return _artwork(params, menu, result);
	if (menu == &_artworkMenu || menu == &_localArtworkMenu)
		return _artworkPages(params, menu, state, result);
	if (!_runner || _runner->core || !menu->title || strcmp(menu->title, "Select file"))
		return false;
	struct GUIMenu* fileMenu = menu;
	if (_romDirectoryChanged) {
		_romDirectoryChanged = false;
		_romLibraryDirty = true;
		_library.menu = NULL;
		_lastSelection = SIZE_MAX;
	}

	struct SwitchPlazaState plaza;
	SwitchPlazaGetState(&plaza);
	if (!_pickerPhase && !_gameSettingsOpen && _plazaInstalled != plaza.installedRevision) {
		_romLibraryDirty = true;
		_plazaInstalled = plaza.installedRevision;
		if (SwitchUIFilesRefreshLibrary(params, menu)) {
			_library.menu = NULL;
		}
	}
	SwitchUIUpdatesPoll(params, true);
	if (SwitchUIUpdatesShouldExit()) { *result = GUI_MENU_EXIT_CANCEL; return true; }
	if (!_inLibrary) {
		SwitchArtworkSetDownloads(_covers, _banners);
		_inLibrary = true;
	}
	menu = _activeLibrary(params, fileMenu);
	if (!_indexLibrary(params, menu))
		return false;
	padUpdate(&_pad);
	u64 buttons = padGetButtonsDown(&_pad);
	uint32_t input = 0;
	GUIPollInput(params, &input, NULL);
	unsigned cx, cy;
	bool tapped = _tap(params, &cx, &cy);
	if (_swipe.dragging || _swipe.suppressTap) tapped = false;
	bool carouselBlocked = _pickerPhase || _gameSettingsOpen;
	bool carouselTap = false;
	if (_pickerInput(params, _focusPath, input, buttons, tapped, cx, cy) ||
	    _gameSettingsInput(params, input, tapped, cx, cy)) {
		input = 0;
		buttons = 0;
		tapped = false;
	}
	bool gameSettingsRequested = !_pickerPhase && !_gameSettingsOpen &&
	    ((buttons & (HidNpadButton_X | HidNpadButton_ZR)) || (tapped && _inside(cx, cy, 1024, 668, 256, 52)));
	if (tapped && cx < 220) _filterFocused = false;
	if (!_pickerPhase && !_gameSettingsOpen && !_filterFocused) {
		const size_t* navEntries = _files ? _library.files : _library.games;
		size_t navCount = _files ? _library.nFiles : _library.nGames;
		bool enter = _files || !navCount || !_position(menu->index, navEntries, navCount);
		bool shortcut = enter && (buttons & HidNpadButton_StickL);
		if (shortcut) input |= 1U << GUI_INPUT_LEFT;
		int navigation = SwitchUINavigationInput(params, _files ? 1 : 0, enter, input, tapped, cx, cy);
		if (navigation != -1) {
			carouselTap = navigation == -3 && !_files;
			bool headerTap = navigation == -3 && cy >= 22 && cy < 82;
			carouselBlocked = !headerTap;
			input = 0; buttons = 0; tapped = headerTap;
			if (navigation == 0 || navigation == 1) { _files = navigation == 1; _lastSelection = SIZE_MAX; }
			if (navigation == 2) SwitchUIOpenSettings(params);
		}
	}
	bool changedFilter = false;
	if (!_pickerPhase && !_gameSettingsOpen && !carouselBlocked) {
		if (padGetButtons(&_pad) & HidNpadButton_X)
			input &= ~(1U << GUI_INPUT_CANCEL);
		bool wasFocused = _filterFocused;
		if (!_filterFocused && (!_files || !_position(menu->index, _library.files, _library.nFiles)) && (input & (1U << GUI_INPUT_UP))) {
			_filterFocused = true;
			input = 0;
		}
		if (_filterFocused) {
			if (input & (1U << GUI_INPUT_LEFT)) _filterIndex = (_filterIndex + 2) % 3;
			if (input & (1U << GUI_INPUT_RIGHT)) _filterIndex = (_filterIndex + 1) % 3;
			if (input & (1U << GUI_INPUT_SELECT)) {
				_filter ^= 1U << _filterIndex;
				changedFilter = true;
			}
			if (wasFocused && (input & ((1U << GUI_INPUT_DOWN) | (1U << GUI_INPUT_BACK) | (1U << GUI_INPUT_CANCEL))))
				_filterFocused = false;
			input = 0;
			buttons &= ~(HidNpadButton_L | HidNpadButton_R);
		}
		if (tapped) {
			for (unsigned i = 0; i < 3; ++i) {
				if (_inside(cx, cy, 514 + i * 88, 22, 76, 60)) {
					_filter ^= 1U << i;
					_filterIndex = i;
					changedFilter = true;
					tapped = false;
					break;
				}
			}
			if (tapped && cy > 100) _filterFocused = false;
		}
	}
	if (changedFilter) {
		_swipe.velocity = 0;
		_swipe.dragging = false;
		_scroll = 0;
		_lastSelection = SIZE_MAX;
	}
	if (buttons & HidNpadButton_Y) {
		_files = !_files;
		_lastSelection = SIZE_MAX;
	}
	menu = _activeLibrary(params, fileMenu);
	if (!_indexLibrary(params, menu))
		return false;
	if (buttons & HidNpadButton_Plus) {
		struct GUIMenuItem* ignored = NULL;
		GUIShowMenu(params, &_settingsMenu, &ignored);
		GUIInvalidateKeys(params);
		input = 0;
		tapped = false;
	}
	if (gameSettingsRequested) input = 0;
	if (input & (1U << GUI_INPUT_CANCEL)) {
		*result = GUI_MENU_EXIT_CANCEL;
		return true;
	}
	if (_files && (input & (1U << GUI_INPUT_BACK))) {
		*result = GUI_MENU_EXIT_BACK;
		return true;
	}
	const size_t* entries = _files ? _library.files : _library.games;
	size_t count = _files ? _library.nFiles : _library.nGames;
	size_t position = _position(menu->index, entries, count);
	if (count)
		menu->index = entries[position];
	if (position && (input & ((1U << GUI_INPUT_LEFT) | (1U << GUI_INPUT_UP))))
		--position;
	if (position + 1 < count && (input & ((1U << GUI_INPUT_RIGHT) | (1U << GUI_INPUT_DOWN))))
		++position;
	if (count && !changedFilter) {
		if (buttons & HidNpadButton_L) position = 0;
		if (buttons & HidNpadButton_R) position = count - 1;
	}
	if (_lastSelection == SIZE_MAX || (fabsf(_scroll - (float) position) > 3.f &&
	    (input || buttons || (!_swipe.dragging && !_swipe.velocity))))
		_scroll = position;
	bool swiping = _carouselTouch(count, &position, !_files && !carouselBlocked && !_filterFocused && !input && !buttons && !changedFilter &&
	    _lastSelection != SIZE_MAX, &tapped);
	if (!swiping) {
		_scroll += ((float) position - _scroll) * 0.22f;
		if (fabsf(_scroll - position) < 0.002f)
			_scroll = position;
	}
	if (_files) {
		if (position < state->start)
			state->start = position;
		if (position >= state->start + 10)
			state->start = position - 9;
		if (tapped && _inside(cx, cy, 88, 132, 1144, 500)) {
			size_t row = state->start + (cy - 132) / 50;
			if (row < count) {
				position = row;
				input |= 1U << GUI_INPUT_SELECT;
			}
		}
	} else if (tapped || carouselTap) {
		if (_layout && count) {
			int first = (int) floorf(_scroll) - 1;
			for (int ordinal = first; ordinal < first + 7; ++ordinal) {
				if (ordinal < 0 || (size_t) ordinal >= count) continue;
				bool selected = (size_t) ordinal == position;
				int left = 104 + (ordinal - _scroll) * 254;
				int right = left + (selected ? 248 : 224);
				if (left < 72) left = 72;
				if (right > left && _inside(cx, cy, left, selected ? 250 : 268, right - left, selected ? 368 : 336)) {
					if (selected && !carouselTap)
						input |= 1U << GUI_INPUT_SELECT;
					position = ordinal;
					break;
				}
			}
			if (tapped && _inside(cx, cy, 104, 638, 240, 24))
				input |= 1U << GUI_INPUT_SELECT;
		} else {
			float center = _scroll;
			int first = (int) floorf(_scroll + .5f) - 2;
			unsigned shown = 5;
			for (unsigned slot = 0; slot < shown; ++slot) {
				int ordinal = first + slot;
				if (ordinal < 0 || (size_t) ordinal >= count) continue;
				size_t index = ordinal;
				float distance = fminf(fabsf(ordinal - _scroll), 1.f);
				int w = 224 - distance * 20, h = w * 3 / 2;
				int x = 640 + (ordinal - center) * 224 - w / 2, y = 248 + (336 - h) / 2;
				if (x < 72) { w -= 72 - x; x = 72; }
				if (w <= 0) continue;
				if (_inside(cx, cy, x, y, w, h)) {
					if (index == position && !carouselTap)
						input |= 1U << GUI_INPUT_SELECT;
					position = index;
				}
			}
			if (tapped && count && _inside(cx, cy, 514, 610, 252, 44))
				input |= 1U << GUI_INPUT_SELECT;
			if (tapped && !count && _inside(cx, cy, 514, 430, 252, 44))
				_files = true;
		}
	}
	if (count)
		menu->index = entries[position];
	const struct GUIMenuItem* selected = count ? GUIMenuItemListGetConstPointer(&menu->items, menu->index) : NULL;
	if (!_pickerPhase && !_gameSettingsOpen && selected && _game(selected) &&
	    gameSettingsRequested) {
		_path(params, selected->title, _gameSettingsRom, sizeof(_gameSettingsRom));
		if (SwitchGameSettingsLoad(_gameSettingsRom, &_gameSettings)) {
			_gameSettingsOpen = true;
			_gameSettingsIndex = 0;
			_gameSettingsLocalNotice = false;
		} else GUIShowMessageBox(params, GUI_MESSAGE_BOX_OK, 0, "Could not read game settings. Check SD card access.");
		GUIInvalidateKeys(params);
		input = 0;
	}
	if (!_pickerPhase && !_gameSettingsOpen && selected && _game(selected) &&
	    (buttons & (HidNpadButton_ZL | HidNpadButton_Minus))) {
		char rom[PATH_MAX];
		_path(params, selected->title, rom, sizeof(rom));
		const char* filename = strrchr(selected->title, '/');
		strlcpy(_pickerQuery, filename ? filename + 1 : selected->title, sizeof(_pickerQuery));
		char* extension = strrchr(_pickerQuery, '.');
		if (extension) *extension = 0;
		_pickerOpen(params, rom, false);
		input = 0;
	}
	if (count && (input & (1U << GUI_INPUT_SELECT))) {
		state->resultItem = GUIMenuItemListGetPointer(&menu->items, menu->index);
		*result = GUI_MENU_EXIT_ACCEPT;
		if (GUIVariantCompareUInt(selected->data, VFS_FILE)) {
			char rom[PATH_MAX];
			_path(params, selected->title, rom, sizeof(rom));
			SwitchGameSettingsSetRom(rom);
			SwitchArtworkSuspend();
			SwitchImagesSuspend();
			_clearTextures();
			_inLibrary = false;
		}
		return true;
	}
	char path[PATH_MAX] = { 0 };
	if (selected)
		_path(params, selected->title, path, sizeof(path));
	if (strcmp(_focusPath, path)) {
		SwitchArtworkRequest("");
		strlcpy(_focusPath, path, sizeof(_focusPath));
	}
	params->drawStart();
	params->guiPrepare();
	++_frames;
	if (_layout && !_files) {
		SwitchUIDrawRect(0, 0, 1280, 720, UI_BASE);
		if (selected)
			_image(path, true, 0, 0, 1280, 668);
		SwitchUIConsoleDraw(params, menu, entries, count, position, _scroll);
		_libraryHeader(params, false);
		_libraryFooter(params);
		if (!count && (input & (1U << GUI_INPUT_SELECT)))
			_files = true;
		SwitchUIFontSize(params->font, 24);
		SwitchUINavigationDraw(params, 0);
		_gameSettingsDraw(params);
		_pickerDraw(params);
		params->guiFinish();
		params->drawEnd();
		_lastSelection = position;
		*result = GUI_MENU_CONTINUE;
		return true;
	}
	SwitchUIDrawRect(0, 0, 1280, 720, UI_BASE);
	if (!_files && selected)
		_image(path, true, 0, 0, 1280, 662);
	_libraryHeader(params, _files);
	if (_files) {
		for (size_t row = 0; row < 10 && state->start + row < count; ++row) {
			size_t ordinal = state->start + row;
			const struct GUIMenuItem* entry = GUIMenuItemListGetConstPointer(&menu->items, entries[ordinal]);
			int y = 132 + row * 50;
			bool focused = ordinal == position;
			if (focused) {
				SwitchUIRoundRect(88, y, 1152, 46, 8, UI_SURFACE);
				SwitchUIDrawRect(88, y + 10, 3, 26, UI_FOCUS);
			} else
				SwitchUIDrawRect(144, y + 46, 1088, 1, 0xFF302B25);
			bool folder = !GUIVariantCompareUInt(entry->data, VFS_FILE);
			if (folder)
				_folder(104, y + 10, UI_MUTED);
			else
				_cartridge(108, y + 10, 28, UI_MUTED);
			SwitchUIFontSize(params->font, 22);
			SwitchUIPrint(params->font, 158, y + 31, 886, UI_TEXT,
			              entry->title[0] == '(' ? "Parent directory" : entry->title);
			SwitchUIFontSize(params->font, 18);
			GUIFontPrint(params->font, 1216, y + 30, GUI_ALIGN_RIGHT, UI_MUTED,
			             folder                       ? "Folder" :
			                 *_platform(entry->title) ? _platform(entry->title) :
			                 _archive(entry->title)   ? "Archive" :
			                                            "File");
		}
		if (count > 10) {
			int thumb = 5000 / count;
			if (thumb < 20)
				thumb = 20;
			SwitchUIRoundRect(1254, 132, 3, 500, 1, UI_SURFACE);
			SwitchUIRoundRect(1254, 132 + position * (500 - thumb) / (count - 1), 3, thumb, 1, UI_FOCUS);
		}
	} else if (count) {
		char title[512];
		_title(selected, title, sizeof(title));
		SwitchUIFontSize(params->font, 32);
		int titleWidth = GUIFontSpanWidth(params->font, title);
		SwitchUIPrint(params->font, titleWidth < 1080 ? (1280 - titleWidth) / 2 : 100, 182, 1080, UI_TEXT, title);
		SwitchUIFontSize(params->font, 18);
		GUIFontPrint(params->font, 640, 218, GUI_ALIGN_HCENTER, UI_MUTED,
		             *_platform(selected->title)   ? _platform(selected->title) :
		                 _archive(selected->title) ? "Archive" :
		                                             "Game");
		float center = _scroll;
		int first = (int) floorf(_scroll + .5f) - 2;
		unsigned shown = 5;
		for (unsigned slot = 0; slot < shown; ++slot) {
			int ordinal = first + slot;
			if (ordinal < 0 || (size_t) ordinal >= count) continue;
			size_t index = ordinal;
			const struct GUIMenuItem* entry = GUIMenuItemListGetConstPointer(&menu->items, entries[index]);
			float distance = fminf(fabsf(ordinal - _scroll), 1.f);
			int w = 224 - distance * 20, h = w * 3 / 2;
			int x = 640 + (ordinal - center) * 224 - w / 2, y = 248 + (336 - h) / 2;
			if (x + w <= 0 || x >= 1280)
				continue;
			if (index == position)
				SwitchUIRoundRect(x - 6, y - 6, w + 12, h + 12, 13, UI_BASE);
			_path(params, entry->title, path, sizeof(path));
			bool artwork = _image(path, false, x, y, w, h);
			if (!artwork)
				_fallback(params->font, entry, x, y, w, h);
			if (index == position)
				SwitchUIRoundFrame(x - 4, y - 4, w + 8, h + 8, 11, UI_FOCUS);
			else if (artwork) {
				char label[512];
				_title(entry, label, sizeof(label));
				SwitchUIFontSize(params->font, 18);
				SwitchUIPrint(params->font, x, y + h + 28, w, UI_MUTED, label);
			}
		}
		_button(params->font, 514, 610, 252, _archive(selected->title) ? "A  Open archive" : "A  Play", true);
	} else {
		_cartridge(608, 218, 64, 0xFF736A61);
		SwitchUIFontSize(params->font, 30);
		GUIFontPrint(params->font, 640, 338, GUI_ALIGN_HCENTER, UI_TEXT,
		             _filter != 7 ? "No games match these consoles" : "No games in this folder");
		SwitchUIFontSize(params->font, 20);
		GUIFontPrint(params->font, 640, 382, GUI_ALIGN_HCENTER, UI_MUTED,
		             _filter != 7 ? "Enable console icons above, or browse another folder." :
		                       "Open Files to choose a folder containing your games.");
		_button(params->font, 514, 430, 252, "Browse files", true);
		if (input & (1U << GUI_INPUT_SELECT))
			_files = true;
	}
	_libraryFooter(params);
	SwitchUIFontSize(params->font, 24);
	SwitchUINavigationDraw(params, _files ? 1 : 0);
	_gameSettingsDraw(params);
	_pickerDraw(params);
	params->guiFinish();
	params->drawEnd();
	_lastSelection = position;
	*result = GUI_MENU_CONTINUE;
	return true;
}

void SwitchUIOpenSettings(struct GUIParams* params) {
	struct GUIMenuItem* ignored = NULL;
	GUIShowMenu(params, &_settingsMenu, &ignored);
	GUIInvalidateKeys(params);
}

struct GUIMenu* SwitchUIArtworkMenu(void) {
	return &_artworkMenu;
}

struct GUIMenu* SwitchUIInterfaceMenu(void) {
	return &_interfaceMenu;
}

void SwitchUIFallback(struct GUIFont* font, const struct GUIMenuItem* entry, int x, int y, int w, int h) {
	_fallback(font, entry, x, y, w, h);
}

void SwitchUIRailIcon(int kind, uint32_t color) {
	if (!kind) {
		SwitchUIRoundRect(24, 85, 24, 18, 3, color);
		SwitchUIRoundRect(28, 88, 16, 7, 2, UI_SURFACE);
		for (int i = 0; i < 5; ++i)
			SwitchUIDrawRect(29 + i * 3, 99, 2, 3, UI_BASE);
	} else {
		SwitchUIRoundRect(24, 155, 11, 6, 2, color);
		SwitchUIRoundRect(24, 159, 24, 14, 2, color);
	}
}

void SwitchUIInit(struct mGUIRunner* runner) {
	_runner = runner;
	padInitializeDefault(&_pad);
	const char* vertex =
	    "#version 300 es\nlayout(location=0) in vec2 pos; layout(location=1) in vec2 uv; out vec2 texcoord; out vec2 "
	    "point; uniform vec4 bounds; uniform vec2 crop; void main(){point=bounds.xy+pos*bounds.zw;"
	    "gl_Position=vec4(point.x/640.0-1.0,1.0-point.y/360.0,0,1);texcoord=vec2(0.5)+(uv-vec2(0.5))*crop;}";
	const char* fragment =
	    "#version 300 es\nprecision highp float; in vec2 texcoord; in vec2 point; uniform sampler2D image; uniform "
	    "vec4 tint; uniform vec4 bounds; uniform float corner; uniform float border; uniform float shade; out vec4 "
	    "pixel; void main(){vec2 q=abs(point-bounds.xy-bounds.zw*0.5)-(bounds.zw*0.5-corner);float "
	    "d=length(max(q,0.0))+min(max(q.x,q.y),0.0)-corner;float "
	    "a=1.0-smoothstep(-0.65,0.65,d);if(border>0.0)a*=smoothstep(-border-0.65,-border+0.65,d);pixel=texture(image,"
	    "texcoord)*tint;float "
	    "dim=0.44+0.39*(1.0-smoothstep(225.0,290.0,point.y))+0.43*smoothstep(500.0,590.0,point.y);pixel.rgb=mix(pixel."
	    "rgb,"
	    "vec3(0.09,0.078,0.122),shade*0.85*min(dim,0.93));pixel.a*=a;}";
	GLuint vs = _shader(GL_VERTEX_SHADER, vertex), fs = _shader(GL_FRAGMENT_SHADER, fragment);
	_program = glCreateProgram();
	glAttachShader(_program, vs);
	glAttachShader(_program, fs);
	glLinkProgram(_program);
	glDeleteShader(vs);
	glDeleteShader(fs);
	_tint = glGetUniformLocation(_program, "tint");
	_bounds = glGetUniformLocation(_program, "bounds");
	_corner = glGetUniformLocation(_program, "corner");
	_border = glGetUniformLocation(_program, "border");
	_shade = glGetUniformLocation(_program, "shade");
	_crop = glGetUniformLocation(_program, "crop");
	glUseProgram(_program);
	glUniform1i(glGetUniformLocation(_program, "image"), 0);
	glGenVertexArrays(1, &_vao);
	glBindVertexArray(_vao);
	glGenBuffers(1, &_vbo);
	glBindBuffer(GL_ARRAY_BUFFER, _vbo);
	const GLfloat vertices[] = { 0, 0, 0, 0, 1, 0, 1, 0, 0, 1, 0, 1, 1, 1, 1, 1 };
	glBufferData(GL_ARRAY_BUFFER, sizeof(vertices), vertices, GL_STATIC_DRAW);
	glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(GLfloat), NULL);
	glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(GLfloat), (void*) (2 * sizeof(GLfloat)));
	glEnableVertexAttribArray(0);
	glEnableVertexAttribArray(1);
	glGenTextures(1, &_white);
	glBindTexture(GL_TEXTURE_2D, _white);
	const uint32_t white = 0xFFFFFFFF;
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 1, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, &white);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
	char directory[PATH_MAX];
	mCoreConfigDirectory(directory, sizeof(directory));
	SwitchPlazaInit(directory);
	SwitchUIAchievementsInit(runner, directory);
	SwitchUIUpdatesInit(runner);
	strlcat(directory, "/artwork", sizeof(directory));
	SwitchArtworkInit(directory);
	SwitchImagesInit();
	int value = 0;
	mCoreConfigGetIntValue(&runner->config, "switch.artwork.covers", &value);
	_covers = value != 0;
	value = 0;
	mCoreConfigGetIntValue(&runner->config, "switch.artwork.banners", &value);
	_banners = value != 0;
	_artworkMenu.title = "Artwork";
	GUIMenuItemListInit(&_artworkMenu.items, 2);
	_steamArtworkMenu.title = "SteamGridDB";
	GUIMenuItemListInit(&_steamArtworkMenu.items, 0);
	_localArtworkMenu.title = "Local";
	GUIMenuItemListInit(&_localArtworkMenu.items, 0);
	*GUIMenuItemListAppend(&_artworkMenu.items) = (struct GUIMenuItem) { .title = "SteamGridDB", .submenu = &_steamArtworkMenu };
	*GUIMenuItemListAppend(&_artworkMenu.items) = (struct GUIMenuItem) { .title = "Local", .submenu = &_localArtworkMenu };
	_layout = 0;
	mCoreConfigGetUIntValue(&runner->config, "switch.interface.layout", &_layout);
	_layout = _layout == 1;
	_interfaceMenu.title = "Interface";
	GUIMenuItemListInit(&_interfaceMenu.items, 0);
	_fileSystemMenu.title = "File System";
	GUIMenuItemListInit(&_fileSystemMenu.items, 0);
	_fileSystemIndex = 0;
	_romDirectoryChanged = false;
	_romLibraryDirty = true;
	_romLibraryMenu.title = "ROM library";
	GUIMenuItemListInit(&_romLibraryMenu.items, 0);
	const char* romDirectory = mCoreConfigGetValue(&runner->config, "switch.romDirectory");
	if (romDirectory && (romDirectory[0] == '/' || !strncmp(romDirectory, "sdmc:/", 6)) &&
	    strlen(romDirectory) < sizeof(runner->params.currentPath)) {
		struct VDir* directory = VDirOpen(romDirectory);
		if (directory) {
			directory->close(directory);
			strlcpy(runner->params.currentPath, romDirectory, sizeof(runner->params.currentPath));
		}
	}
	_settingsMenu.title = "Settings";
	GUIMenuItemListInit(&_settingsMenu.items, 7);
	*GUIMenuItemListAppend(&_settingsMenu.items) =
	    (struct GUIMenuItem) { .title = "Interface", .submenu = &_interfaceMenu };
	*GUIMenuItemListAppend(&_settingsMenu.items) =
	    (struct GUIMenuItem) { .title = "File System", .submenu = &_fileSystemMenu };
	*GUIMenuItemListAppend(&_settingsMenu.items) =
	    (struct GUIMenuItem) { .title = "Artwork", .submenu = &_artworkMenu };
	*GUIMenuItemListAppend(&_settingsMenu.items) =
	    (struct GUIMenuItem) { .title = "RetroAchievements", .submenu = SwitchUIAchievementsMenu() };
	*GUIMenuItemListAppend(&_settingsMenu.items) = (struct GUIMenuItem) { .title = "Romhack Plaza", .submenu = SwitchUIPlazaMenu() };
	*GUIMenuItemListAppend(&_settingsMenu.items) = (struct GUIMenuItem) { .title = "Updates", .submenu = SwitchUIUpdatesMenu() };
	*GUIMenuItemListAppend(&_settingsMenu.items) = (struct GUIMenuItem) { .title = "Back" };
}

void SwitchUIDeinit(void) {
	SwitchUIUpdatesDeinit();
	SwitchUIAchievementsDeinit();
	SwitchUIFilesDeinit();
	SwitchImagesDeinit();
	SwitchArtworkDeinit();
	SwitchPlazaDeinit();
	GUIMenuItemListDeinit(&_artworkMenu.items);
	GUIMenuItemListDeinit(&_steamArtworkMenu.items);
	GUIMenuItemListDeinit(&_localArtworkMenu.items);
	GUIMenuItemListDeinit(&_interfaceMenu.items);
	GUIMenuItemListDeinit(&_fileSystemMenu.items);
	for (size_t i = 1; i < GUIMenuItemListSize(&_romLibraryMenu.items); ++i)
		free((char*) GUIMenuItemListGetPointer(&_romLibraryMenu.items, i)->title);
	GUIMenuItemListDeinit(&_romLibraryMenu.items);
	GUIMenuItemListDeinit(&_settingsMenu.items);
	_clearTextures();
	free(_library.games);
	free(_library.files);
	memset(&_library, 0, sizeof(_library));
	glDeleteTextures(1, &_white);
	glDeleteBuffers(1, &_vbo);
	glDeleteVertexArrays(1, &_vao);
	glDeleteProgram(_program);
	_runner = NULL;
}
