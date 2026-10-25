// The store's three screens (xsdk's docs/scope/63-homebrew-store.md, step 4), the "Stage" mockup: the categories in a
// list on the left with the selected one's apps beside it; a category opened slides its apps into the list, the
// selected app's screenshot beside them; an app opened narrows the list to a strip of names beside the app's page.
// Drawn by hand on Dear ImGui's draw lists, as SwanStation's picker is; the controller comes through ImGui's keys.

#include "ui.h"

#include <math.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>

#include <algorithm>
#include <string>
#include <vector>

#define IMGUI_DEFINE_MATH_OPERATORS
#include "imgui.h"
#include "imgui_impl_xsdk.h"
#include "worker.h"
#include "xsdk/gpu.h"
#include "xsdk/log.h"
#include "xsdk/present.h"
#include "xsdk/texture.h"

// Archivo regular, medium and bold, OFL 1.1 (fonts/OFL.txt), built in by fonts.S.
extern "C" const unsigned char font_regular[], font_regular_end[], font_medium[], font_medium_end[], font_bold[],
	font_bold_end[];

namespace
{

enum : uint32_t {
	// The Guide's first opening held a frame up 2.7 s (xsdk's docs/HARDWARE.md, "The Guide, milestone 37").
	BEGIN_TIMEOUT_MS = 5000,
	BACKGROUND_ARGB = 0xFF0D0D0E,
	// A slide or a narrowing from one screen to the next.
	TRANSITION_FRAMES = 12,
	// Frames the selection rests on an app before its screenshot is asked for: scrolling past asks for none.
	SHOT_REST_FRAMES = 8,
	// Screenshots held as textures, the least recently shown going first.
	SHOT_TEXTURES = 6,
	// shots: the frames each screen shows before it's written, and at most how long one waits for its screenshot.
	SHOT_FRAMES = 4,
	SHOT_WAIT_FRAMES = 600,
};

enum Level { CATEGORIES, APPS, PAGE };

// The mockup's colours: near-black, warm white, two greys, hairline rules, a lighter row for the selection.
const ImU32 BACKGROUND = IM_COL32(0x0D, 0x0D, 0x0E, 0xFF);
const ImU32 TEXT = IM_COL32(0xEE, 0xEE, 0xEA, 0xFF);
const ImU32 GREY = IM_COL32(0x8A, 0x8A, 0x86, 0xFF);
const ImU32 DIM = IM_COL32(0x6F, 0x6F, 0x6B, 0xFF);
const ImU32 RULE = IM_COL32(0x2A, 0x2A, 0x2C, 0xFF);
const ImU32 SELECTED = IM_COL32(0x1B, 0x1B, 0x1D, 0xFF);
const ImU32 WHITE = IM_COL32(0xFF, 0xFF, 0xFF, 0xFF);
const ImU32 PLACEHOLDER = IM_COL32(0x16, 0x16, 0x18, 0xFF);
const ImU32 FILL = IM_COL32(0x3A, 0x3A, 0x3E, 0xFF);
const ImU32 VEIL = IM_COL32(0x00, 0x00, 0x00, 0xB8);
const ImU32 PANEL = IM_COL32(0x14, 0x14, 0x16, 0xFF);

// The layout in pixels of a 720-line display; every size scales with the display's height. The title-safe area is
// the middle 90%.
const float SAFE_X = 64.0f;
const float SAFE_Y = 36.0f;
const float TOP = 84.0f;
const float BOTTOM = 684.0f;
const float COLUMN_GAP = 32.0f;
const float LIST_WIDTH = 380.0f;
const float STRIP_WIDTH = 230.0f;
const float ROW = 56.0f;
const float ROW_SIZE = 27.0f;
const float STRIP_ROW = 42.0f;
const float STRIP_SIZE = 21.0f;
const float STATUS_SIZE = 17.0f;
const float ROW_PAD = 16.0f;
const float SELECTION_RULE = 3.0f;
const float HEADER_SIZE = 16.0f;
const float HINT_SIZE = 18.0f;
const float SLIDE = 120.0f;
// Letter spacing, against the size, for the capitals of the breadcrumb and labels.
const float CAPS_TRACKING = 0.14f;
// The categories' panel.
const float CATEGORY_NAME_SIZE = 60.0f;
const float SENTENCE_SIZE = 24.0f;
const float SENTENCE_LINE = 31.0f;
const float PREVIEW_ROW = 40.0f;
const float PREVIEW_SIZE = 22.0f;
// The apps' panel.
const float APP_NAME_SIZE = 40.0f;
const float APP_SENTENCE_SIZE = 22.0f;
const float AUTHOR_SIZE = 18.0f;
// The app's page.
const float LABEL_SIZE = 14.0f;
const float PAGE_NAME_SIZE = 52.0f;
const float FACT_SIZE = 20.0f;
const float FACT_GAP = 40.0f;
const float PLATE_WIDTH = 180.0f;
const float PLATE_HEIGHT = 48.0f;
const float PLATE_SIZE = 20.0f;
const float PAGE_SHOT_TOP = 306.0f;
const float MESSAGE_SIZE = 26.0f;
const float MESSAGE_LINE_SIZE = 16.0f;
// The overlay's panel.
const float OVERLAY_WIDTH = 760.0f;
const float OVERLAY_PAD = 40.0f;
const float OVERLAY_TITLE_SIZE = 30.0f;
const float OVERLAY_BODY_SIZE = 21.0f;
const float OVERLAY_LINE = 29.0f;

const char ELLIPSIS[] = "\xE2\x80\xA6";

ImFont *regular;
ImFont *medium;
ImFont *bold;
// Where apps install, ui_run's.
const char *apps_folder = "";

struct Shot {
	bool used;
	size_t app;
	// Not had: no screenshot came, so none is asked for again.
	bool failed;
	XsdkTexture texture;
	uint32_t shown;
};

// What's over the screens, waiting for A or B: the word before an app not by xenosworks first installs, a removal to
// confirm, and the offer to delete what an app made once it's removed.
enum Overlay { NO_OVERLAY, WARNING, REMOVAL, LEFTOVERS };

struct State {
	StoreCatalogue *catalogue = nullptr;
	StoreSource source = STORE_NONE;
	const char *why = "";
	// Each category's apps, by index into the catalogue.
	std::vector<std::vector<size_t>> apps;
	Level level = CATEGORIES;
	Level from = CATEGORIES;
	uint32_t transition = TRANSITION_FRAMES;
	size_t category = 0;
	size_t row[2] = { 0, 0 };
	size_t scroll[3] = { 0, 0, 0 };
	uint32_t rest = 0;
	bool asked = false;
	size_t asked_app = 0;
	Shot shots[SHOT_TEXTURES] = {};
	uint32_t frame = 0;
	Overlay overlay = NO_OVERLAY;
	size_t overlay_app = 0;
	size_t leftovers = 0;
	// The job under way, and what the last one said, on its app's page.
	bool busy = false;
	size_t busy_app = 0;
	std::string message;
	size_t message_app = 0;
};

State state;

float text_width(ImFont *font, float size, const std::string &text, float tracking = 0.0f)
{
	if (tracking == 0.0f)
		return font->CalcTextSizeA(size, FLT_MAX, 0.0f, text.c_str()).x;
	float width = 0.0f;
	for (const char *c = text.c_str(); *c;) {
		int bytes = 1;
		while ((c[bytes] & 0xC0) == 0x80)
			bytes++;
		width += font->CalcTextSizeA(size, FLT_MAX, 0.0f, c, c + bytes).x + (c[bytes] ? tracking * size : 0.0f);
		c += bytes;
	}
	return width;
}

void draw_text(
	ImDrawList *draw, ImFont *font, float size, ImVec2 at, ImU32 colour, const std::string &text, float tracking = 0.0f)
{
	if (tracking == 0.0f) {
		draw->AddText(font, size, at, colour, text.c_str());
		return;
	}
	for (const char *c = text.c_str(); *c;) {
		int bytes = 1;
		while ((c[bytes] & 0xC0) == 0x80)
			bytes++;
		draw->AddText(font, size, at, colour, c, c + bytes);
		at.x += font->CalcTextSizeA(size, FLT_MAX, 0.0f, c, c + bytes).x + tracking * size;
		c += bytes;
	}
}

// Text cut short with an ellipsis to fit max_width.
std::string fitted(ImFont *font, float size, std::string text, float max_width, float tracking = 0.0f)
{
	if (text_width(font, size, text, tracking) <= max_width)
		return text;
	while (!text.empty()) {
		size_t end = text.size() - 1;
		while (end > 0 && (text[end] & 0xC0) == 0x80)
			end--;
		text.erase(end);
		while (!text.empty() && text.back() == ' ')
			text.pop_back();
		if (text_width(font, size, text + ELLIPSIS, tracking) <= max_width)
			break;
	}
	return text + ELLIPSIS;
}

// Text wrapped to width, its lines past max_lines left out, the last one shown cut short with an ellipsis.
void draw_wrapped(ImDrawList *draw, ImFont *font, float size, float line, ImVec2 at, float width, int max_lines,
	ImU32 colour, const char *text)
{
	const char *start = text;
	const char *const text_end = text + strlen(text);
	for (int index = 0; index < max_lines && *start; index++) {
		const char *end = font->CalcWordWrapPosition(size, start, text_end, width);
		if (end == start)
			end = start + 1;
		std::string shown(start, end);
		while (*end == ' ' || *end == '\n')
			end++;
		if (index == max_lines - 1 && *end)
			shown = fitted(font, size, shown + " " + end, width);
		draw_text(draw, font, size, ImVec2(at.x, at.y + index * line), colour, shown);
		start = end;
	}
}

std::string capitals(const char *text)
{
	std::string upper(text);
	for (char &c : upper)
		c = (char)toupper((unsigned char)c);
	return upper;
}

std::string bytes_text(uint64_t bytes)
{
	char text[32];
	if (bytes >= 1000ull * 1000 * 1000)
		snprintf(text, sizeof text, "%.1f GB", (double)bytes / 1e9);
	else if (bytes >= 1000 * 1000)
		snprintf(text, sizeof text, "%.1f MB", (double)bytes / 1e6);
	else
		snprintf(text, sizeof text, "%u KB", (unsigned)((bytes + 999) / 1000));
	return text;
}

// What the list and the panels say of an app's state, and what A does on its page.
bool busy_on(const StoreApp &app)
{
	return state.busy && &app == &state.catalogue->apps[state.busy_app];
}

std::string state_text(const StoreApp &app)
{
	if (busy_on(app))
		return store_worker_progress()->step == STORE_STEP_REMOVING ? "Removing" : "Installing";
	switch (app.installed) {
	case STORE_INSTALLED_CURRENT:
		return "Installed";
	case STORE_INSTALLED_OLDER:
		return "Update ready";
	case STORE_INSTALLED_NEWER:
		return "Installed, newer";
	default:
		return bytes_text(app.installed_bytes);
	}
}

const char *action_text(const StoreApp &app)
{
	return app.installed == STORE_INSTALLED_NONE ? "Install"
		: app.installed == STORE_INSTALLED_OLDER ? "Update"
												 : "Launch";
}

// "By <author>", and that it isn't by xenosworks when it isn't (xsdk's docs/scope/63-homebrew-store.md, "Other
// people's homebrew").
std::string author_text(const StoreApp &app)
{
	return std::string("By ") + app.author + (strcasecmp(app.author, "xenosworks") ? ", not xenosworks" : "");
}

float ease(float t)
{
	return 1.0f - (1.0f - t) * (1.0f - t) * (1.0f - t);
}

ImU32 faded(ImU32 colour, float alpha)
{
	return (colour & ~IM_COL32_A_MASK) |
		(ImU32)((float)((colour >> IM_COL32_A_SHIFT) & 0xFF) * alpha) << IM_COL32_A_SHIFT;
}

const StoreApp &app_at(size_t category, size_t row)
{
	return state.catalogue->apps[state.apps[category][row]];
}

size_t selected_app()
{
	return state.apps[state.category][state.row[APPS]];
}

// The list on the left: its entries, the selected one on a lighter row with a white rule, the list kept scrolled to
// it, at x and alpha for a slide.
struct ListLook {
	float width;
	float row;
	float size;
	bool status;
};

void draw_list(ImDrawList *draw, float scale, float x, float alpha, const ListLook &look, Level level)
{
	const bool categories = level == CATEGORIES;
	const size_t count = categories ? state.catalogue->category_count : state.apps[state.category].size();
	const size_t selected = categories ? state.category : state.row[APPS];
	const float row = look.row * scale;
	const float top = TOP * scale;
	const size_t fit = std::max<size_t>(1, (size_t)((BOTTOM - TOP - HINT_SIZE * 2.0f) * scale / row));
	size_t &scroll = state.scroll[level];
	if (selected < scroll)
		scroll = selected;
	if (selected >= scroll + fit)
		scroll = selected - fit + 1;
	const float width = look.width * scale;
	const float pad = ROW_PAD * scale;
	for (size_t i = scroll; i < count && i < scroll + fit; i++) {
		const float y = top + (float)(i - scroll) * row;
		const bool chosen = i == selected;
		if (chosen) {
			draw->AddRectFilled(ImVec2(x, y), ImVec2(x + width, y + row), faded(SELECTED, alpha));
			draw->AddRectFilled(ImVec2(x, y), ImVec2(x + SELECTION_RULE * scale, y + row), faded(WHITE, alpha));
		}
		const std::string name = categories ? state.catalogue->categories[i].name : app_at(state.category, i).name;
		const std::string status = !look.status ? ""
			: categories                        ? std::to_string(state.apps[i].size())
												: state_text(app_at(state.category, i));
		const float status_width = status.empty() ? 0.0f : text_width(regular, STATUS_SIZE * scale, status) + pad;
		const float size = look.size * scale;
		draw_text(draw, chosen ? medium : regular, size, ImVec2(x + pad, y + (row - size) / 2.0f),
			faded(chosen ? TEXT : GREY, alpha), fitted(regular, size, name, width - pad * 2.0f - status_width));
		if (!status.empty()) {
			draw_text(draw, regular, STATUS_SIZE * scale,
				ImVec2(x + width - pad - text_width(regular, STATUS_SIZE * scale, status),
					y + (row - STATUS_SIZE * scale) / 2.0f),
				faded(DIM, alpha), status);
		}
	}
}

Shot *shot_of(size_t app)
{
	for (Shot &shot : state.shots) {
		if (shot.used && shot.app == app)
			return &shot;
	}
	return nullptr;
}

// The screenshot, or a quiet box while it comes or when it can't.
void draw_shot(ImDrawList *draw, ImVec2 from, ImVec2 to, size_t app, float alpha)
{
	Shot *const shot = shot_of(app);
	if (shot && !shot->failed) {
		shot->shown = state.frame;
		draw->AddRectFilled(from, to, faded(PLACEHOLDER, alpha));
		draw->AddImage(
			(ImTextureID)(intptr_t)&shot->texture, from, to, ImVec2(0, 0), ImVec2(1, 1), faded(WHITE, alpha));
		return;
	}
	draw->AddRectFilled(from, to, faded(PLACEHOLDER, alpha));
	if (shot) {
		const float size = (to.y - from.y) * 0.05f;
		const char *const text = "No screenshot";
		draw_text(draw, regular, size,
			ImVec2((from.x + to.x - text_width(regular, size, text)) / 2.0f, (from.y + to.y - size) / 2.0f),
			faded(DIM, alpha), text);
	}
}

void draw_categories_panel(ImDrawList *draw, float scale, float x, float right, float alpha)
{
	const StoreCategory &category = state.catalogue->categories[state.category];
	const float width = right - x;
	float y = TOP * scale;
	draw_text(draw, bold, CATEGORY_NAME_SIZE * scale, ImVec2(x, y), faded(TEXT, alpha),
		fitted(bold, CATEGORY_NAME_SIZE * scale, category.name, width));
	y += CATEGORY_NAME_SIZE * scale * 1.25f;
	draw_wrapped(draw, regular, SENTENCE_SIZE * scale, SENTENCE_LINE * scale, ImVec2(x, y), width, 2,
		faded(GREY, alpha), category.sentence);
	y += SENTENCE_LINE * scale * 2.0f + 24.0f * scale;
	draw->AddLine(ImVec2(x, y), ImVec2(right, y), faded(RULE, alpha), 1.0f);
	y += 8.0f * scale;
	const std::vector<size_t> &apps = state.apps[state.category];
	const float row = PREVIEW_ROW * scale;
	const size_t fit = (size_t)((BOTTOM * scale - y) / row);
	for (size_t i = 0; i < apps.size() && i < fit; i++) {
		const StoreApp &app = state.catalogue->apps[apps[i]];
		const float text_y = y + (row - PREVIEW_SIZE * scale) / 2.0f;
		const bool more = i == fit - 1 && apps.size() > fit;
		const std::string name = more ? "and " + std::to_string(apps.size() - i) + " more" : app.name;
		const std::string status = more ? "" : state_text(app);
		const float status_width = text_width(regular, STATUS_SIZE * scale, status);
		draw_text(draw, regular, PREVIEW_SIZE * scale, ImVec2(x, text_y), faded(more ? GREY : TEXT, alpha),
			fitted(regular, PREVIEW_SIZE * scale, name, width - status_width - ROW_PAD * scale));
		draw_text(draw, regular, STATUS_SIZE * scale,
			ImVec2(right - status_width, y + (row - STATUS_SIZE * scale) / 2.0f), faded(DIM, alpha), status);
		y += row;
	}
	if (apps.empty())
		draw_text(
			draw, regular, PREVIEW_SIZE * scale, ImVec2(x, y + 8.0f * scale), faded(GREY, alpha), "Nothing here yet");
}

void draw_apps_panel(ImDrawList *draw, float scale, float x, float right, float alpha)
{
	if (state.apps[state.category].empty())
		return;
	const StoreApp &app = state.catalogue->apps[selected_app()];
	const float width = right - x;
	const float shot_height = std::min(width * 9.0f / 16.0f, 398.0f * scale);
	float y = TOP * scale;
	draw_shot(draw, ImVec2(x, y), ImVec2(x + shot_height * 16.0f / 9.0f, y + shot_height), selected_app(), alpha);
	y += shot_height + 24.0f * scale;
	draw_text(draw, bold, APP_NAME_SIZE * scale, ImVec2(x, y), faded(TEXT, alpha),
		fitted(bold, APP_NAME_SIZE * scale, app.name, width));
	y += APP_NAME_SIZE * scale * 1.25f;
	draw_text(draw, regular, APP_SENTENCE_SIZE * scale, ImVec2(x, y), faded(GREY, alpha),
		fitted(regular, APP_SENTENCE_SIZE * scale, app.sentence, width));
	y += APP_SENTENCE_SIZE * scale * 1.6f;
	draw_text(draw, regular, AUTHOR_SIZE * scale, ImVec2(x, y), faded(DIM, alpha),
		fitted(regular, AUTHOR_SIZE * scale, author_text(app), width));
}

void draw_page(ImDrawList *draw, float scale, float x, float right, float alpha)
{
	const StoreApp &app = state.catalogue->apps[selected_app()];
	const float width = right - x;
	float y = TOP * scale;
	draw_text(draw, medium, LABEL_SIZE * scale, ImVec2(x, y), faded(GREY, alpha),
		capitals(state.catalogue->categories[app.category].name), CAPS_TRACKING);
	y += 20.0f * scale;
	draw_text(draw, bold, PAGE_NAME_SIZE * scale, ImVec2(x, y), faded(TEXT, alpha),
		fitted(bold, PAGE_NAME_SIZE * scale, app.name, width));
	y += 64.0f * scale;
	draw_text(draw, regular, APP_SENTENCE_SIZE * scale, ImVec2(x, y), faded(GREY, alpha),
		fitted(regular, APP_SENTENCE_SIZE * scale, app.sentence, width));
	y += 32.0f * scale;
	draw_text(draw, regular, AUTHOR_SIZE * scale, ImVec2(x, y), faded(DIM, alpha),
		fitted(regular, AUTHOR_SIZE * scale, author_text(app), width));
	y += 40.0f * scale;

	// The facts, then the one thing A does, on a white plate at the right.
	const float plate_x = right - PLATE_WIDTH * scale;
	const char *const labels[] = { "VERSION", "SIZE", "NEEDS" };
	const std::string values[] = { app.version, bytes_text(app.installed_bytes), app.needs[0] ? app.needs : "Nothing" };
	float fact_x = x;
	for (int i = 0; i < 3; i++) {
		const float value_width = text_width(regular, FACT_SIZE * scale, values[i]);
		const float room = i == 2 ? plate_x - FACT_GAP * scale - fact_x : value_width;
		draw_text(draw, medium, LABEL_SIZE * scale, ImVec2(fact_x, y), faded(GREY, alpha), labels[i], CAPS_TRACKING);
		draw_text(draw, regular, FACT_SIZE * scale, ImVec2(fact_x, y + 20.0f * scale), faded(TEXT, alpha),
			fitted(regular, FACT_SIZE * scale, values[i], std::max(room, 0.0f)));
		fact_x +=
			std::max(value_width, text_width(medium, LABEL_SIZE * scale, labels[i], CAPS_TRACKING)) + FACT_GAP * scale;
	}
	const float plate_y = y - 4.0f * scale;
	const ImVec2 plate_end(right, plate_y + PLATE_HEIGHT * scale);
	std::string action = std::string("A  ") + action_text(app);
	ImU32 action_colour = BACKGROUND;
	if (busy_on(app)) {
		// Under way: the plate dark, filling as it goes.
		const StoreProgress *const progress = store_worker_progress();
		const float done = progress->total_bytes ? (float)progress->done_bytes / (float)progress->total_bytes : 0.0f;
		draw->AddRectFilled(ImVec2(plate_x, plate_y), plate_end, faded(SELECTED, alpha));
		draw->AddRectFilled(ImVec2(plate_x, plate_y),
			ImVec2(plate_x + PLATE_WIDTH * scale * std::min(done, 1.0f), plate_end.y), faded(FILL, alpha));
		draw->AddRect(ImVec2(plate_x, plate_y), plate_end, faded(RULE, alpha));
		char text[32];
		const char *const step = progress->step == STORE_STEP_DOWNLOADING ? "Downloading"
			: progress->step == STORE_STEP_INSTALLING                     ? "Installing"
																		  : "Removing";
		snprintf(text, sizeof text, progress->total_bytes ? "%s %d%%" : "%s", step, (int)(done * 100.0f));
		action = text;
		action_colour = TEXT;
	} else {
		draw->AddRectFilled(ImVec2(plate_x, plate_y), plate_end, faded(WHITE, alpha));
	}
	draw_text(draw, bold, PLATE_SIZE * scale,
		ImVec2(plate_x + (PLATE_WIDTH * scale - text_width(bold, PLATE_SIZE * scale, action)) / 2.0f,
			plate_y + (PLATE_HEIGHT * scale - PLATE_SIZE * scale) / 2.0f),
		faded(action_colour, alpha), action);
	if (!state.message.empty() && state.message_app == selected_app()) {
		draw_text(draw, regular, MESSAGE_LINE_SIZE * scale, ImVec2(x, y + 48.0f * scale), faded(GREY, alpha),
			fitted(regular, MESSAGE_LINE_SIZE * scale, state.message, width));
	}

	const float shot_top = PAGE_SHOT_TOP * scale;
	const float shot_height = std::min(BOTTOM * scale - shot_top, width * 9.0f / 16.0f);
	draw_shot(draw, ImVec2(x, shot_top), ImVec2(x + shot_height * 16.0f / 9.0f, shot_top + shot_height), selected_app(),
		alpha);
}

ListLook list_look(Level level)
{
	return level == PAGE ? ListLook{ STRIP_WIDTH, STRIP_ROW, STRIP_SIZE, false }
						 : ListLook{ LIST_WIDTH, ROW, ROW_SIZE, true };
}

void draw_panel(ImDrawList *draw, float scale, Level level, float list_width, float right, float alpha)
{
	const float x = (SAFE_X + list_width + COLUMN_GAP * 2.0f) * scale;
	if (level == CATEGORIES)
		draw_categories_panel(draw, scale, x, right, alpha);
	else if (level == APPS)
		draw_apps_panel(draw, scale, x, right, alpha);
	else
		draw_page(draw, scale, x, right, alpha);
}

void draw_message(ImDrawList *draw, ImVec2 size, float scale, const std::string &message, const std::string &detail)
{
	const float width = size.x - SAFE_X * 2.0f * scale;
	const float y = size.y * 0.42f;
	draw_wrapped(draw, medium, MESSAGE_SIZE * scale, MESSAGE_SIZE * scale * 1.3f, ImVec2(SAFE_X * scale, y), width, 2,
		TEXT, message.c_str());
	if (!detail.empty()) {
		draw_wrapped(draw, regular, HINT_SIZE * scale, HINT_SIZE * scale * 1.35f,
			ImVec2(SAFE_X * scale, y + MESSAGE_SIZE * scale * 3.0f), width, 3, GREY, detail.c_str());
	}
}

// The screens veiled, and a panel saying what's asked and what A and B do.
void draw_overlay(ImDrawList *draw, ImVec2 size, float scale)
{
	const StoreApp &app = state.catalogue->apps[state.overlay_app];
	const std::string name = app.name;
	const std::string folder = std::string(apps_folder) + app.id;
	std::string title;
	std::string body;
	const char *hints = "";
	if (state.overlay == WARNING) {
		// xsdk's docs/scope/63-homebrew-store.md, "Other people's homebrew".
		title = name + " isn't by xenosworks";
		body = name + " is made by " + app.author +
			", not xenosworks. xenosworks lists it but hasn't checked what it " +
			"does, and isn't responsible for any damage to your console or your files.";
		hints = "A  install     B  cancel";
	} else if (state.overlay == REMOVAL) {
		title = "Remove " + name + "?";
		body = "Its files go from " + folder + ". What it made itself, its saves and settings, stays unless you " +
			"choose to delete that too.";
		hints = "A  remove     B  cancel";
	} else {
		title = "Delete what " + name + " made?";
		body = std::to_string(state.leftovers) + (state.leftovers == 1 ? " file it made is" : " files it made are") +
			" still in " + folder + ": its saves, settings and the like. Deleted, they're gone for good.";
		hints = "A  delete     B  keep";
	}
	draw->AddRectFilled(ImVec2(0.0f, 0.0f), size, VEIL);
	const float width = OVERLAY_WIDTH * scale;
	const float pad = OVERLAY_PAD * scale;
	const float height = pad * 2.0f + (OVERLAY_TITLE_SIZE + 22.0f + OVERLAY_LINE * 4.0f + 18.0f + HINT_SIZE) * scale;
	const ImVec2 from((size.x - width) / 2.0f, (size.y - height) / 2.0f);
	draw->AddRectFilled(from, ImVec2(from.x + width, from.y + height), PANEL);
	draw->AddRect(from, ImVec2(from.x + width, from.y + height), RULE);
	float y = from.y + pad;
	draw_text(draw, bold, OVERLAY_TITLE_SIZE * scale, ImVec2(from.x + pad, y), TEXT,
		fitted(bold, OVERLAY_TITLE_SIZE * scale, title, width - pad * 2.0f));
	y += (OVERLAY_TITLE_SIZE + 22.0f) * scale;
	draw_wrapped(draw, regular, OVERLAY_BODY_SIZE * scale, OVERLAY_LINE * scale, ImVec2(from.x + pad, y),
		width - pad * 2.0f, 4, GREY, body.c_str());
	y += (OVERLAY_LINE * 4.0f + 18.0f) * scale;
	draw_text(draw, regular, HINT_SIZE * scale, ImVec2(from.x + pad, y), DIM, hints);
}

void draw_store(ImDrawList *draw, ImVec2 size)
{
	const float scale = size.y / 720.0f;
	const float right = size.x - SAFE_X * scale;
	std::string crumb = "HOMEBREW";
	if (state.catalogue && state.level != CATEGORIES)
		crumb += "  /  " + capitals(state.catalogue->categories[state.category].name);
	draw_text(draw, medium, HEADER_SIZE * scale, ImVec2(SAFE_X * scale, SAFE_Y * scale), GREY, crumb, CAPS_TRACKING);
	if (!state.catalogue) {
		draw_message(draw, size, scale, "Fetching the catalogue\xE2\x80\xA6", "");
		return;
	}
	if (state.source == STORE_NONE) {
		draw_message(draw, size, scale, "The catalogue didn't come, and none is kept on this console.",
			std::string(state.why) + ".  A  try again     B  quit");
		return;
	}
	const char *const notice = state.source == STORE_KEPT_OFFLINE ? "Offline: the catalogue kept on this console"
		: state.source == STORE_KEPT_REFUSED ? "The catalogue that came was refused; the one kept is shown"
											 : "";
	draw_text(draw, regular, HEADER_SIZE * scale,
		ImVec2(right - text_width(regular, HEADER_SIZE * scale, notice), SAFE_Y * scale), DIM, notice);

	const float t = ease((float)state.transition / (float)TRANSITION_FRAMES);
	const bool moving = state.transition < TRANSITION_FRAMES;
	const ListLook to = list_look(state.level);
	float list_width = to.width;
	if (moving && (state.from == CATEGORIES) != (state.level == CATEGORIES)) {
		// Into a category or out of one: the lists slide, the deeper one from the right.
		const float direction = state.level > state.from ? 1.0f : -1.0f;
		draw_list(draw, scale, (SAFE_X - direction * SLIDE * t) * scale, 1.0f - t, list_look(state.from), state.from);
		draw_list(draw, scale, (SAFE_X + direction * SLIDE * (1.0f - t)) * scale, t, to, state.level);
	} else if (moving) {
		// Onto an app's page or off it: the list narrows or widens.
		const ListLook from = list_look(state.from);
		const ListLook between = { from.width + (to.width - from.width) * t, from.row + (to.row - from.row) * t,
			from.size + (to.size - from.size) * t, to.status };
		list_width = between.width;
		draw_list(draw, scale, SAFE_X * scale, 1.0f, between, state.level);
	} else {
		draw_list(draw, scale, SAFE_X * scale, 1.0f, to, state.level);
	}
	const float rule_x = (SAFE_X + list_width + COLUMN_GAP) * scale;
	draw->AddLine(ImVec2(rule_x, TOP * scale), ImVec2(rule_x, BOTTOM * scale), RULE, 1.0f);
	if (moving)
		draw_panel(draw, scale, state.from, list_look(state.from).width, right, 1.0f - t);
	draw_panel(draw, scale, state.level, list_width, right, moving ? t : 1.0f);

	std::string hints = state.level == CATEGORIES ? "A  open     B  quit" : "A  open     B  back";
	if (state.level == PAGE) {
		const StoreApp &app = state.catalogue->apps[selected_app()];
		hints = app.receipt ? "X  remove     B  back" : "B  back";
	}
	draw_text(draw, regular, HINT_SIZE * scale, ImVec2(SAFE_X * scale, (BOTTOM - HINT_SIZE) * scale), DIM, hints);
	if (state.overlay != NO_OVERLAY)
		draw_overlay(draw, size, scale);
}

void go(Level level)
{
	state.from = state.level;
	state.level = level;
	state.transition = 0;
	state.rest = 0;
}

bool pressed(ImGuiKey key, bool repeat = false)
{
	return ImGui::IsKeyPressed(key, repeat);
}

int vertical()
{
	if (pressed(ImGuiKey_GamepadDpadDown, true) || pressed(ImGuiKey_GamepadLStickDown, true))
		return 1;
	if (pressed(ImGuiKey_GamepadDpadUp, true) || pressed(ImGuiKey_GamepadLStickUp, true))
		return -1;
	return 0;
}

size_t moved(size_t at, int by, size_t count)
{
	if (by < 0)
		return at ? at - 1 : 0;
	return by > 0 && at + 1 < count ? at + 1 : at;
}

// The screenshot worker's results made textures, and the one wanted asked for once the selection rests.
void tend_shots()
{
	size_t app;
	uint32_t *pixels;
	if (store_worker_take_shot(&app, &pixels)) {
		state.asked = false;
		Shot *slot = nullptr;
		for (Shot &shot : state.shots) {
			if (!shot.used) {
				slot = &shot;
				break;
			}
			if (!slot || shot.shown < slot->shown)
				slot = &shot;
		}
		if (slot->used && !slot->failed) {
			// The GPU may still read it for the last frame.
			(void)xsdk_gpu_wait(xsdk_gpu_fence(), BEGIN_TIMEOUT_MS);
			xsdk_texture_free(&slot->texture);
		}
		*slot = Shot{ true, app, true, {}, state.frame };
		if (pixels &&
			NT_SUCCESS(xsdk_texture_create(
				&slot->texture, XSDK_FORMAT_A8R8G8B8, STORE_SHOT_WIDTH, STORE_SHOT_HEIGHT, 1, true))) {
			static const uint32_t channels[4] = { XSDK_CHANNEL_BLUE, XSDK_CHANNEL_GREEN, XSDK_CHANNEL_RED,
				XSDK_CHANNEL_ALPHA };
			xsdk_texture_swizzle(&slot->texture, channels);
			xsdk_texture_upload(&slot->texture, 0, pixels, STORE_SHOT_WIDTH * 4);
			xsdk_gpu_invalidate(slot->texture.address, slot->texture.size_bytes);
			slot->failed = false;
		}
		free(pixels);
	}
	if (state.level == CATEGORIES || state.apps[state.category].empty() || state.asked)
		return;
	const size_t wanted = selected_app();
	if (!shot_of(wanted) && (state.level == PAGE || state.rest >= SHOT_REST_FRAMES)) {
		store_worker_want_shot(wanted);
		state.asked = true;
		state.asked_app = wanted;
	}
}

void free_shots()
{
	(void)xsdk_gpu_wait(xsdk_gpu_fence(), BEGIN_TIMEOUT_MS);
	for (Shot &shot : state.shots) {
		if (shot.used && !shot.failed)
			xsdk_texture_free(&shot.texture);
		shot = Shot{};
	}
}

// Writes the frame resolved into the front buffer at address as a binary PPM beside the .xex. The front buffer is a
// tiled A8R8G8B8 texture whose words hold B, G, R then A (xsdk's docs/GUIDE.md, "Textures, render targets and
// resolves").
void write_shot(uint32_t address, uint32_t width, uint32_t height, const char *name)
{
	const NTSTATUS waited = xsdk_gpu_wait(xsdk_gpu_fence(), BEGIN_TIMEOUT_MS);
	XsdkTexture frame;
	const NTSTATUS status = xsdk_texture_init(&frame, XSDK_FORMAT_A8R8G8B8, width, height, 1, true);
	FILE *const file = NT_SUCCESS(waited) && NT_SUCCESS(status) ? fopen(name, "wb") : nullptr;
	if (!file) {
		xsdk_log_write("store: can't write %s: 0x%08X, 0x%08X", name, (unsigned)waited, (unsigned)status);
		return;
	}
	xsdk_texture_attach(&frame, xsdk_present_pixels(address));
	fprintf(file, "P6\n%u %u\n255\n", (unsigned)width, (unsigned)height);
	std::vector<uint32_t> row(width);
	std::vector<uint8_t> rgb(width * 3);
	for (uint32_t y = 0; y < height; y++) {
		xsdk_texture_download_rows(&frame, 0, y, 1, row.data(), width * 4);
		for (uint32_t x = 0; x < width; x++) {
			const uint32_t pixel = __builtin_bswap32(row[x]);
			rgb[x * 3] = (uint8_t)(pixel >> 16);
			rgb[x * 3 + 1] = (uint8_t)(pixel >> 8);
			rgb[x * 3 + 2] = (uint8_t)pixel;
		}
		fwrite(rgb.data(), 1, rgb.size(), file);
	}
	fclose(file);
	xsdk_log_write("store: wrote %s", name);
}

bool present(uint32_t width, uint32_t height, const char *shot)
{
	uint32_t address;
	const NTSTATUS status = xsdk_present_begin(&address, BEGIN_TIMEOUT_MS);
	if (!NT_SUCCESS(status)) {
		xsdk_log_write("store: xsdk_present_begin failed: 0x%08X", (unsigned)status);
		return false;
	}
	xsdk_gpu_set_target(width, height);
	ImGui_ImplXsdk_RenderDrawData(ImGui::GetDrawData());
	xsdk_gpu_resolve(address, width, height, BACKGROUND_ARGB);
	if (shot)
		write_shot(address, width, height, shot);
	xsdk_present_end();
	return true;
}

void take_catalogue()
{
	StoreCatalogue *catalogue;
	if (state.catalogue || !store_worker_catalogue(&catalogue, &state.source, &state.why))
		return;
	state.catalogue = catalogue;
	state.apps.assign(catalogue->category_count, {});
	for (size_t i = 0; i < catalogue->app_count; i++)
		state.apps[catalogue->apps[i].category].push_back(i);
}

// A job's result put into its app's entry, and what it said kept for the app's page.
void tend_job()
{
	state.busy = store_worker_busy(&state.busy_app);
	StoreJobResult result;
	if (!store_worker_take_job(&result))
		return;
	StoreApp &app = state.catalogue->apps[result.app];
	app.installed = result.installed;
	app.installed_version = result.installed_version;
	app.receipt = result.receipt;
	app.receipt_author_same = result.receipt_author_same;
	state.message_app = result.app;
	if (!result.done)
		state.message =
			std::string(result.kind == STORE_JOB_INSTALL ? "Not installed: " : "Not removed: ") + result.why;
	else if (result.kind == STORE_JOB_INSTALL)
		state.message = std::string("Installed in ") + apps_folder + app.id;
	else
		state.message = "Removed";
	if (result.done && result.kind == STORE_JOB_REMOVE && result.other_files) {
		state.overlay = LEFTOVERS;
		state.overlay_app = result.app;
		state.leftovers = result.other_files;
	}
}

// A on an app's page: its install, after the word if it isn't by xenosworks and the store hasn't installed it from
// that author before. Opening an installed app comes with step 6.
void act(size_t index)
{
	const StoreApp &app = state.catalogue->apps[index];
	if (state.busy || (app.installed != STORE_INSTALLED_NONE && app.installed != STORE_INSTALLED_OLDER))
		return;
	if (strcasecmp(app.author, "xenosworks") != 0 && !(app.receipt && app.receipt_author_same)) {
		state.overlay = WARNING;
		state.overlay_app = index;
		return;
	}
	store_worker_job(STORE_JOB_INSTALL, index);
}

// What A does on the overlay; B closes it.
void answer(bool yes)
{
	const Overlay overlay = state.overlay;
	state.overlay = NO_OVERLAY;
	if (!yes)
		return;
	if (overlay == WARNING)
		store_worker_job(STORE_JOB_INSTALL, state.overlay_app);
	else
		store_worker_job(overlay == REMOVAL ? STORE_JOB_REMOVE : STORE_JOB_REMOVE_ALL, state.overlay_app);
}

// shots: presses and screens written, each once the screens have settled.
enum Act { SHOOT, PRESS_A, PRESS_B, PRESS_DOWN };
struct Step {
	Act act;
	const char *name;
};
const Step SHOT_STEPS[] = { { SHOOT, "shot_categories.ppm" }, { PRESS_A, nullptr }, { SHOOT, "shot_apps.ppm" },
	{ PRESS_A, nullptr }, { SHOOT, "shot_app.ppm" }, { PRESS_A, nullptr }, { SHOOT, "shot_installed.ppm" },
	{ PRESS_B, nullptr }, { PRESS_B, nullptr }, { PRESS_DOWN, nullptr }, { PRESS_A, nullptr }, { PRESS_A, nullptr },
	{ PRESS_A, nullptr }, { SHOOT, "shot_warning.ppm" } };

// Settled: no slide, no job, and the screenshot in, or come to nothing, if the screen shows one.
bool settled()
{
	return state.transition == TRANSITION_FRAMES && !state.busy &&
		(state.level == CATEGORIES || state.apps[state.category].empty() || shot_of(selected_app()));
}

} // namespace

bool ui_open(void)
{
	IMGUI_CHECKVERSION();
	ImGui::CreateContext();
	ImGuiIO &io = ImGui::GetIO();
	io.IniFilename = nullptr;
	io.LogFilename = nullptr;
	ImFontConfig config;
	config.FontDataOwnedByAtlas = false;
	regular =
		io.Fonts->AddFontFromMemoryTTF((void *)font_regular, (int)(font_regular_end - font_regular), ROW_SIZE, &config);
	medium =
		io.Fonts->AddFontFromMemoryTTF((void *)font_medium, (int)(font_medium_end - font_medium), ROW_SIZE, &config);
	bold = io.Fonts->AddFontFromMemoryTTF((void *)font_bold, (int)(font_bold_end - font_bold), ROW_SIZE, &config);
	if (!regular || !medium || !bold || !ImGui_ImplXsdk_Init()) {
		xsdk_log_write("store: setting up Dear ImGui failed");
		ImGui::DestroyContext();
		return false;
	}
	return true;
}

void ui_close(void)
{
	free_shots();
	ImGui_ImplXsdk_Shutdown();
	ImGui::DestroyContext();
}

void ui_run(uint32_t width, uint32_t height, const char *apps, bool shots)
{
	apps_folder = apps;
	size_t step = 0;
	uint32_t settled_frames = 0;
	for (;; state.frame++) {
		ImGui_ImplXsdk_NewFrame(width, height);
		ImGui::NewFrame();
		take_catalogue();
		const bool usable = state.catalogue && state.source != STORE_NONE && state.catalogue->category_count;
		if (usable)
			tend_job();
		bool open = pressed(ImGuiKey_GamepadFaceDown);
		bool back = pressed(ImGuiKey_GamepadFaceRight);
		int by = vertical();
		const char *shot = nullptr;
		if (shots && step < IM_ARRAYSIZE(SHOT_STEPS) && usable) {
			settled_frames = settled() || settled_frames > SHOT_WAIT_FRAMES ? settled_frames + 1 : 0;
			if (settled_frames > SHOT_FRAMES) {
				const Step &next = SHOT_STEPS[step++];
				shot = next.act == SHOOT ? next.name : nullptr;
				open = next.act == PRESS_A;
				back = next.act == PRESS_B;
				by = next.act == PRESS_DOWN ? 1 : by;
				settled_frames = 0;
			}
		}
		if (state.overlay != NO_OVERLAY) {
			if (open || back)
				answer(open);
		} else if (state.catalogue && state.source == STORE_NONE) {
			if (open) {
				store_worker_retry();
				state.catalogue = nullptr;
			}
			if (back) {
				ImGui::EndFrame();
				return;
			}
		} else if (usable && state.transition == TRANSITION_FRAMES) {
			if (state.level == CATEGORIES) {
				state.category = moved(state.category, by, state.catalogue->category_count);
				if (by) {
					state.row[APPS] = 0;
					state.scroll[APPS] = state.scroll[PAGE] = 0;
				}
				if (open && !state.apps[state.category].empty())
					go(APPS);
			} else {
				const size_t before = state.row[APPS];
				state.row[APPS] = moved(state.row[APPS], by, state.apps[state.category].size());
				if (state.row[APPS] != before)
					state.rest = 0;
				if (open && state.level == PAGE)
					act(selected_app());
				else if (open)
					go(PAGE);
				if (state.level == PAGE && pressed(ImGuiKey_GamepadFaceLeft) && !state.busy &&
					state.catalogue->apps[selected_app()].receipt) {
					state.overlay = REMOVAL;
					state.overlay_app = selected_app();
				}
			}
			if (back) {
				if (state.level == CATEGORIES) {
					ImGui::EndFrame();
					return;
				}
				go(state.level == PAGE ? APPS : CATEGORIES);
			}
		} else if (!state.catalogue && back) {
			ImGui::EndFrame();
			return;
		}
		if (state.transition < TRANSITION_FRAMES)
			state.transition++;
		state.rest++;
		if (usable)
			tend_shots();
		draw_store(ImGui::GetBackgroundDrawList(), ImVec2((float)width, (float)height));
		ImGui::Render();
		if (!present(width, height, shot) || (shots && step == IM_ARRAYSIZE(SHOT_STEPS) && shot))
			return;
	}
}
