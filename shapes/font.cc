/*
 *  Copyright (C) 2000-2022  The Exult Team
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA 02111-1307, USA.
 */

#ifdef HAVE_CONFIG_H
#	include <config.h>
#endif

#include "font.h"

#include "U7file.h"
#include "databuf.h"
#include "exceptions.h"
#include "ibuf8.h"
#include "ignore_unused_variable_warning.h"
#include "utils.h"
#include "vgafile.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <istream>
#include <vector>

using std::size_t;
using std::string;
using std::strncmp;
using std::toupper;

FontManager fontManager;

/*
 *  日本語（UTF-8 の多バイト文字）の対応。
 *
 *  正しい UTF-8 の並びは1字として読み、別に用意した日本語フォント
 *  （<PATCH>/jfont8.bin、<PATCH>/jfont12.bin）の絵で描く。
 *  正しくないバイト（Exult の内蔵フォントの特殊文字 0x81 など）は、
 *  今までどおり1バイト1字として描く。
 */

namespace {
	// UTF-8 を1字読む。読めたら字の番号を返し、len にバイト数を入れる。
	// 正しい並びでなければ -1 を返す。
	// end が nullptr なら 0 終端の文字列として読む。
	long Utf8_peek(const char* text, const char* end, int& len) {
		const auto*    s = reinterpret_cast<const unsigned char*>(text);
		const unsigned c = s[0];
		int            n;
		uint32_t       cp;
		if (c < 0xC2) {
			return -1;
		} else if (c < 0xE0) {
			n  = 2;
			cp = c & 0x1F;
		} else if (c < 0xF0) {
			n  = 3;
			cp = c & 0x0F;
		} else if (c < 0xF5) {
			n  = 4;
			cp = c & 0x07;
		} else {
			return -1;
		}
		if (end && text + n > end) {
			return -1;
		}
		for (int i = 1; i < n; i++) {
			// 0 終端なら s[i] が 0 のところでここに引っかかって止まる。
			if ((s[i] & 0xC0) != 0x80) {
				return -1;
			}
			cp = (cp << 6) | (s[i] & 0x3F);
		}
		len = n;
		return static_cast<long>(cp);
	}

	// 1字分のバイト数（正しくない並びは1バイト）。
	inline int Char_len(const char* text, const char* end = nullptr) {
		int len;
		return Utf8_peek(text, end, len) >= 0 ? len : 1;
	}

	// 日本語として1字ずつ折り返してよい字か（かな、漢字、全角の記号など）。
	inline bool Is_cjk(long cp) {
		return cp >= 0x2000;
	}

	inline bool Is_cjk_at(const char* text) {
		int len;
		return Is_cjk(Utf8_peek(text, nullptr, len));
	}

	bool In_list(const char32_t* list, long cp) {
		for (; *list; list++) {
			if (static_cast<long>(*list) == cp) {
				return true;
			}
		}
		return false;
	}

	// 行の頭に来てはいけない字（行頭禁則）。
	bool Is_no_start(long cp) {
		if (cp < 0x80) {
			return cp == '.' || cp == ',' || cp == '!' || cp == '?' || cp == '"' || cp == '\'' || cp == ')' || cp == ':'
				   || cp == ';';
		}
		return In_list(
				U"、。，．・：；？！ー」』）】〉》〕｝］〟”’ゝゞヽヾぁぃぅぇぉっゃゅょゎァィゥェォッャュョヮヵヶ…‥～", cp);
	}

	// 行の終わりに来てはいけない字（開きかっこ）。
	bool Is_no_end(long cp) {
		return In_list(U"「『（【〈《〔｛［〝“‘", cp);
	}

	// 文の切れ目になる日本語の記号で終わっているか（pbreak 用）。
	bool Ends_with_jpunct(const char* start, const char* end) {
		static const char* const marks[] = {"。", "！", "？", "」", "』", "、", "…"};
		for (const char* m : marks) {
			const size_t n = std::strlen(m);
			if (static_cast<size_t>(end - start) >= n && std::memcmp(end - n, m, n) == 0) {
				return true;
			}
		}
		return false;
	}

	// 日本語フォント（BDF から作った独自形式）。
	//   "U7JF" 版(2) 字数(2) 幅(1) 高さ(1) 1行のバイト数(1) 予備(1)
	//   字の番号(4)×字数（小さい順）、絵（高さ×1行のバイト数）×字数
	struct JFont {
		int                   w = 0, h = 0, bpr = 0;
		std::vector<uint32_t> cps;
		std::vector<uint8_t>  bits;

		const uint8_t* find(uint32_t cp) const {
			auto it = std::lower_bound(cps.begin(), cps.end(), cp);
			if (it == cps.end() || *it != cp) {
				return nullptr;
			}
			return bits.data() + (it - cps.begin()) * h * bpr;
		}
	};

	const JFont* Get_jfont(int size) {
		static std::array<std::unique_ptr<JFont>, 2> fonts;
		static std::array<bool, 2>                   tried{};
		const int                                    idx = size >= 12 ? 1 : 0;
		if (tried[idx]) {
			return fonts[idx].get();
		}
		if (!is_system_path_defined("<PATCH>")) {
			return nullptr;    // まだパスが決まっていないので、あとでもう一度試す。
		}
		tried[idx] = true;
		char name[64];
		std::snprintf(name, sizeof(name), "<PATCH>/jfont%d.bin", idx ? 12 : 8);
		if (!U7exists(name)) {
			return nullptr;
		}
		try {
			auto in = U7open_in(name, false);
			if (!in) {
				return nullptr;
			}
			unsigned char hdr[12];
			in->read(reinterpret_cast<char*>(hdr), sizeof(hdr));
			if (!in->good() || std::memcmp(hdr, "U7JF", 4) != 0) {
				std::cerr << name << ": not a Japanese font file" << std::endl;
				return nullptr;
			}
			auto      f   = std::make_unique<JFont>();
			const int cnt = hdr[6] | (hdr[7] << 8);
			f->w          = hdr[8];
			f->h          = hdr[9];
			f->bpr        = hdr[10];
			f->cps.resize(cnt);
			for (auto& cp : f->cps) {
				unsigned char b[4];
				in->read(reinterpret_cast<char*>(b), 4);
				cp = b[0] | (b[1] << 8) | (b[2] << 16) | (static_cast<uint32_t>(b[3]) << 24);
			}
			f->bits.resize(static_cast<size_t>(cnt) * f->h * f->bpr);
			in->read(reinterpret_cast<char*>(f->bits.data()), static_cast<std::streamsize>(f->bits.size()));
			if (!in->good()) {
				std::cerr << name << ": file is too short" << std::endl;
				return nullptr;
			}
			fonts[idx] = std::move(f);
		} catch (const std::exception& e) {
			std::cerr << name << ": " << e.what() << std::endl;
		}
		return fonts[idx].get();
	}

	// RLE の絵に使われている色を数える。
	void Count_colors(Shape_frame* f, std::array<int, 256>& cnt) {
		const unsigned char* p = f->get_data();
		if (!p) {
			return;
		}
		const unsigned char* const pend = p + f->get_size();
		while (p + 2 <= pend) {
			const int bl = p[0] | (p[1] << 8);
			p += 2;
			if (bl == 0 || p + 4 > pend) {
				break;
			}
			p += 4;    // x, y
			int n = bl >> 1;
			if ((bl & 1) == 0) {
				for (int i = 0; i < n && p < pend; i++) {
					cnt[*p++]++;
				}
			} else {
				while (n > 0 && p < pend) {
					const int b = *p++;
					const int c = b >> 1;
					if (b & 1) {
						cnt[*p++] += c;
					} else {
						for (int i = 0; i < c && p < pend; i++) {
							cnt[*p++]++;
						}
					}
					n -= c;
				}
			}
		}
	}
}    // namespace

/*
 *  日本語の字の色と大きさを、元のフォントの絵から決める。
 */

void Font::prepare_jglyphs() {
	jready = true;
	if (!font_shapes) {
		return;
	}
	std::array<int, 256> cnt{};
	for (int i = 0; i < font_shapes->get_num_frames(); i++) {
		Shape_frame* f = font_shapes->get_frame(i);
		if (f && f->is_rle()) {
			Count_colors(f, cnt);
		}
	}
	// いちばん多い色と、その次に多い色。
	int first = -1, second = -1;
	for (int c = 0; c < 256; c++) {
		if (!cnt[c]) {
			continue;
		}
		if (first < 0 || cnt[c] > cnt[first]) {
			second = first;
			first  = c;
		} else if (second < 0 || cnt[c] > cnt[second]) {
			second = c;
		}
	}
	jfill    = first < 0 ? 0 : first;
	joutline = -1;
	// 黒（0）がいちばん多く、ほかの色もそれなりにあるなら、黒は縁取り。
	if (first == 0 && second > 0 && cnt[second] * 5 > cnt[0]) {
		jfill    = second;
		joutline = 0;
	}
	const int room = highest + lowest + 1 - (joutline >= 0 ? 2 : 0);
	jsize          = (room >= 12 && Get_jfont(12)) ? 12 : 8;
}

Shape_frame* Font::get_jglyph(uint32_t cp) {
	if (!jready) {
		prepare_jglyphs();
	}
	auto it = jglyphs.find(cp);
	if (it != jglyphs.end()) {
		return it->second.get();
	}
	const JFont* jf = Get_jfont(jsize);
	if (!jf && jsize == 12) {
		jf = Get_jfont(8);
	}
	const uint8_t* bits = jf ? jf->find(cp) : nullptr;
	if (!bits) {
		jglyphs[cp] = nullptr;
		return nullptr;
	}
	const int pad = joutline >= 0 ? 1 : 0;
	const int w   = jf->w + 2 * pad;
	const int h   = jf->h + 2 * pad;
	std::vector<unsigned char> pix(static_cast<size_t>(w) * h, 255);
	auto on = [&](int x, int y) {
		return x >= 0 && y >= 0 && x < jf->w && y < jf->h && (bits[y * jf->bpr + x / 8] & (0x80 >> (x % 8)));
	};
	for (int y = 0; y < h; y++) {
		for (int x = 0; x < w; x++) {
			const int gx = x - pad;
			const int gy = y - pad;
			if (on(gx, gy)) {
				pix[y * w + x] = static_cast<unsigned char>(jfill);
			} else if (pad) {
				for (int dy = -1; dy <= 1; dy++) {
					for (int dx = -1; dx <= 1; dx++) {
						if (on(gx + dx, gy + dy)) {
							pix[y * w + x] = static_cast<unsigned char>(joutline);
						}
					}
				}
			}
		}
	}
	// 行の高さの中で上下の真ん中に置く。
	const int line  = highest + lowest + 1;
	const int top   = std::max(0, (line - h + 1) / 2);
	auto      frame = std::make_shared<Shape_frame>(pix.data(), w, h, 0, highest - top, true);
	jglyphs[cp]     = frame;
	return frame.get();
}

Shape_frame* Font::next_frame(const char*& text, const char* end) {
	int        len;
	const long cp = Utf8_peek(text, end, len);
	if (cp >= 0) {
		text += len;
		if (Shape_frame* f = get_jglyph(static_cast<uint32_t>(cp))) {
			return f;
		}
		// 日本語フォントに無い字は「?」で描く。
		return font_shapes->get_frame('?');
	}
	return font_shapes->get_frame(static_cast<unsigned char>(*text++));
}

//	Want a more restrictive test for space.
inline bool Is_space(char c) {
	return c == ' ' || c == '\n' || c == '\t';
}

/*
 *  Pass space.
 */

static const char* Pass_whitespace(const char* text) {
	while (Is_space(*text)) {
		text++;
	}
	return text;
}

// Just spaces and tabs:
static const char* Pass_space(const char* text) {
	while (*text == ' ' || *text == '\t') {
		text++;
	}
	return text;
}

/*
 *  Pass a word.
 */

static const char* Pass_word(const char* text) {
	int len;
	// 日本語は1字ずつ折り返す。ただし行頭禁則の字と、開きかっこの次の字はくっつける。
	if (Is_cjk(Utf8_peek(text, nullptr, len))) {
		for (;;) {
			const long cp = Utf8_peek(text, nullptr, len);
			text += cp >= 0 ? len : 1;
			if (!Is_no_end(cp) || !*text) {
				break;
			}
		}
		while (*text) {
			const long cp = Utf8_peek(text, nullptr, len);
			if (!Is_no_start(cp >= 0 ? cp : static_cast<unsigned char>(*text))) {
				break;
			}
			text += cp >= 0 ? len : 1;
		}
		return text;
	}
	while (*text && (*text != '^') && (!Is_space(*text) || (*text == '\f') || (*text == '\v'))) {
		if (Is_cjk_at(text)) {
			break;
		}
		text += Char_len(text);
	}
	return text;
}

/*
 *  Draw text within a rectangular area.
 *  Special characters handled are:
 *      \n  New line.
 *      space   Word break.
 *      tab Treated like a space for now.
 *      *   Page break.
 *      ^   Uppercase next letter.
 *
 *  Output: If out of room, -offset of end of text painted.
 *      Else height of text painted.
 */

int Font::paint_text_box(
		Image_buffer8* win,                // Buffer to paint in.
		const char* text, int x, int y,    // Top-left corner of box.
		int w, int h,                      // Dimensions.
		int            vert_lead,          // Extra spacing between lines.
		bool           pbreak,             // End at punctuation.
		bool           center,             // Center each line.
		Cursor_info*   cursor,             // We set x, y if not nullptr.
		unsigned char* trans) {
	const char* start    = text;    // Remember the start.
	auto        clipsave = win->SaveClip();
	auto        newclip  = clipsave.Rect().intersect(TileRect(x, y, w, h));
	win->set_clip(newclip.x, newclip.y, newclip.w, newclip.h);

	const int   endx           = x + w;    // Figure where to stop.
	int         curx           = x;
	int         cury           = y;
	const int   height         = get_text_height() + vert_lead + ver_lead;
	const int   space_width    = get_text_width(" ", 1);
	const int   max_lines      = h / height;    // # lines that can be shown.
	auto*       lines          = new string[max_lines + 1];
	int         cur_line       = 0;
	const char* last_punct_end = nullptr;    // ->last period, qmark, etc.
	// Last punct in 'lines':
	int last_punct_line   = -1;
	int last_punct_offset = -1;
	int coff              = -1;

	if (cursor) {
		coff      = cursor->offset;
		cursor->x = -1;
	}
	while (*text) {
		if (cursor && text - start == coff) {
			cursor->set_found(curx, cury, cur_line);
		}
		switch (*text) {    // Special cases.
		case '\n':          // Next line.
			curx = x;
			text++;
			cur_line++;
			cury += height;
			if (cur_line >= max_lines) {
				break;    // No more room.
			}
			continue;
		case '\r':    //??
			text++;
			continue;
		case ' ':    // Space.
		case '\t': {
			// Pass space.
			const char* wrd = Pass_space(text);
			if (wrd != text) {
				int w = get_text_width(text, static_cast<uint32>(wrd - text));
				if (w <= 0) {
					w = space_width;
				}
				const int nsp = w / space_width;
				lines[cur_line].append(nsp, ' ');
				if (cursor && coff > text - start && coff < wrd - start) {
					cursor->set_found(curx + static_cast<uint32>(coff - (text - start)) * space_width, cury, cur_line);
				}
				curx += nsp * space_width;
			}
			text = wrd;
			break;
		}
		}
		if (cur_line >= max_lines) {
			break;
		}

		if (*text == '*') {
			text++;
			if (cur_line) {
				break;
			}
		}
		bool ucase_next = *text == '^';
		if (ucase_next) {    // Skip it.
			text++;
			// 日本語の字の前の ^ は無視する。
			if (static_cast<unsigned char>(*text) >= 0x80) {
				ucase_next = false;
			}
		}
		// Pass word & get its width.
		const char* ewrd = Pass_word(text);
		int         width;
		if (ucase_next) {
			const char c = static_cast<char>(toupper(static_cast<unsigned char>(*text)));
			width        = get_text_width(&c, 1u) + get_text_width(text + 1, static_cast<uint32>(ewrd - text - 1));
		} else {
			width = get_text_width(text, static_cast<uint32>(ewrd - text));
		}
		if (curx + width - hor_lead > endx) {
			// Word-wrap.
			if (ucase_next) {
				text--;    // Put the '^' back.
			}
			curx = x;
			cur_line++;
			cury += height;
			if (cur_line >= max_lines) {
				break;    // No more room.
			}
		}
		if (cursor && coff >= text - start && coff < ewrd - start) {
			cursor->set_found(curx + get_text_width(text, static_cast<uint32>(coff - (text - start))), cury, cur_line);
		}
		// Store word.
		if (ucase_next) {
			lines[cur_line].push_back(static_cast<char>(toupper(static_cast<unsigned char>(*text))));
			++text;
		}
		lines[cur_line].append(text, ewrd - text);
		curx += width;
		text = ewrd;    // Continue past the word.
		// Keep loc. of punct. endings.
		if (text[-1] == '.' || text[-1] == '?' || text[-1] == '!' || text[-1] == ',' || text[-1] == '"'
			|| Ends_with_jpunct(start, text)) {
			last_punct_end    = text;
			last_punct_line   = cur_line;
			last_punct_offset = static_cast<int>(lines[cur_line].length());
		}
	}
	if (*text &&    // Out of room?
					// Break off at end of punct.
		pbreak && last_punct_end) {
		text = Pass_whitespace(last_punct_end);
	} else {
		last_punct_line = -1;
		if (cursor && text - start == coff &&    // Cursor at very end?
			cur_line < max_lines) {
			cursor->set_found(curx, cury, cur_line);
		}
	}
	if (cursor) {
		cursor->nlines = cur_line + (cur_line < max_lines);
	}
	cury = y;    // Render text.
	for (int i = 0; i <= cur_line; i++) {
		const char* str = lines[i].c_str();
		int         len = static_cast<int>(lines[i].length());
		if (i == last_punct_line) {
			len = last_punct_offset;
		}
		if (center) {
			center_text(win, x + w / 2, cury, str, trans);
		} else {
			paint_text(win, str, len, x, cury, trans);
		}
		cury += height;
		if (i == last_punct_line) {
			break;
		}
	}
	delete[] lines;
	if (*text) {                                   // Out of room?
		return -static_cast<int>(text - start);    // Return -offset of end.
	} else {                                       // Else return height.
		return cury - y;
	}
}

/*
 *  Draw text at a given location (which is the upper-left corner of the
 *  place to draw.
 *
 *  Output: Width in pixels of what was drawn.
 */

int Font::paint_text(
		Image_buffer8* win,     // Buffer to paint in.
		const char*    text,    // What to draw, 0-delimited.
		int xoff, int yoff,     // Upper-left corner of where to start.
		unsigned char* trans) {
	ignore_unused_variable_warning(win);
	int x = xoff;
	yoff += get_text_baseline();
	if (font_shapes) {
		while (*text) {
			const int    chr   = static_cast<unsigned char>(*text);
			Shape_frame* shape = next_frame(text, nullptr);
			if (!shape || !shape->is_rle()) {
				auto oldflags = std::cerr.flags();

				std::cerr << " unable to find rle frame for character '" << char(chr) << "' 0x" << std::hex << chr << " in font"
						  << std::endl;
				std::cerr.flags(oldflags);

				continue;
			}
			if (trans) {
				shape->paint_rle_remapped(x, yoff, trans);
			} else {
				shape->paint_rle(x, yoff);
			}
			x += shape->get_width() + hor_lead;
		}
	}
	return x - xoff;
}

/*
 *  Paint text using font from "fonts.vga".
 *
 *  Output: Width in pixels of what was painted.
 */

int Font::paint_text(
		Image_buffer8* win,        // Buffer to paint in.
		const char*    text,       // What to draw.
		int            textlen,    // Length of text.
		int xoff, int yoff,        // Upper-left corner of where to start.
		unsigned char* trans) {
	ignore_unused_variable_warning(win);
	int x = xoff;
	yoff += get_text_baseline();
	if (font_shapes) {
		const char* const end = text + textlen;
		while (text < end) {
			const int    chr   = static_cast<unsigned char>(*text);
			Shape_frame* shape = next_frame(text, end);
			if (!shape || !shape->is_rle()) {
				auto oldflags = std::cerr.flags();

				std::cerr << " unable to find rle frame for character '" << char(chr) << "' 0x" << std::hex << chr << " in font"
						  << std::endl;
				std::cerr.flags(oldflags);

				continue;
			}
			if (trans) {
				shape->paint_rle_remapped(x, yoff, trans);
			} else {
				shape->paint_rle(x, yoff);
			}
			x += shape->get_width() + hor_lead;
		}
	}
	return x - xoff;
}

/*
 *
 *  FIXED WIDTH RENDERING
 *
 */

/*
 *  Draw text within a rectangular area.
 *  Special characters handled are:
 *      \n  New line.
 *      space   Word break.
 *      tab Treated like a space for now.
 *      *   Page break.
 *      ^   Uppercase next letter.
 *
 *  Output: If out of room, -offset of end of text painted.
 *      Else height of text painted.
 */

int Font::paint_text_box_fixedwidth(
		Image_buffer8* win,                // Buffer to paint in.
		const char* text, int x, int y,    // Top-left corner of box.
		int w, int h,                      // Dimensions.
		int            char_width,         // Width of each character
		int            vert_lead,          // Extra spacing between lines.
		int            pbreak,             // End at punctuation.
		unsigned char* trans) {
	const char* start = text;    // Remember the start.
	win->set_clip(x, y, w, h);
	const int   endx           = x + w;    // Figure where to stop.
	int         curx           = x;
	int         cury           = y;
	const int   height         = get_text_height() + vert_lead + ver_lead;
	const int   max_lines      = h / height;    // # lines that can be shown.
	auto*       lines          = new string[max_lines + 1];
	int         cur_line       = 0;
	const char* last_punct_end = nullptr;    // ->last period, qmark, etc.
	// Last punct in 'lines':
	int last_punct_line   = -1;
	int last_punct_offset = -1;

	while (*text) {
		switch (*text) {    // Special cases.
		case '\n':          // Next line.
			curx = x;
			text++;
			cur_line++;
			if (cur_line >= max_lines) {
				break;    // No more room.
			}
			continue;
		case ' ':    // Space.
		case '\t': {
			// Pass space.
			const char* wrd = Pass_space(text);
			if (wrd != text) {
				const int w   = static_cast<int>(wrd - text) * char_width;
				const int nsp = w / char_width;
				lines[cur_line].append(nsp, ' ');
				curx += nsp * char_width;
			}
			text = wrd;
			break;
		}
		}

		if (cur_line >= max_lines) {
			break;
		}

		if (*text == '*') {
			text++;
			if (cur_line) {
				break;
			}
		}
		const bool ucase_next = *text == '^';
		if (ucase_next) {    // Skip it.
			text++;
		}
		// Pass word & get its width.
		const char* ewrd = Pass_word(text);
		int         nch  = 0;
		for (const char* q = text; q < ewrd; q += Char_len(q, ewrd)) {
			nch++;
		}
		const int width = nch * char_width;
		if (curx + width - hor_lead > endx) {
			// Word-wrap.
			if (ucase_next) {
				text--;    // Put the '^' back.
			}
			curx = x;
			cur_line++;
			if (cur_line >= max_lines) {
				break;    // No more room.
			}
		}

		// Store word.
		if (ucase_next) {
			lines[cur_line].push_back(static_cast<char>(toupper(static_cast<unsigned char>(*text))));
			++text;
		}
		lines[cur_line].append(text, ewrd - text);
		curx += width;
		text = ewrd;    // Continue past the word.
		// Keep loc. of punct. endings.
		if (text[-1] == '.' || text[-1] == '?' || text[-1] == '!' || text[-1] == ',' || text[-1] == '"'
			|| Ends_with_jpunct(start, text)) {
			last_punct_end    = text;
			last_punct_line   = cur_line;
			last_punct_offset = static_cast<int>(lines[cur_line].length());
		}
	}
	if (*text &&    // Out of room?
					// Break off at end of punct.
		pbreak && last_punct_end) {
		text = Pass_whitespace(last_punct_end);
	} else {
		last_punct_line = -1;
	}
	// Render text.
	for (int i = 0; i <= cur_line; i++) {
		const char* str = lines[i].data();
		int         len = static_cast<int>(lines[i].length());
		if (i == last_punct_line) {
			len = last_punct_offset;
		}
		paint_text_fixedwidth(win, str, len, x, cury, char_width, trans);
		cury += height;
		if (i == last_punct_line) {
			break;
		}
	}
	win->clear_clip();
	delete[] lines;
	if (*text) {                                   // Out of room?
		return -static_cast<int>(text - start);    // Return -offset of end.
	} else {                                       // Else return height.
		return cury - y;
	}
}

/*
 *  Draw text at a given location (which is the upper-left corner of the
 *  place to draw. Text will be drawn with the fixed width specified.
 *
 *  Output: Width in pixels of what was drawn.
 */

int Font::paint_text_fixedwidth(
		Image_buffer8* win,      // Buffer to paint in.
		const char*    text,     // What to draw, 0-delimited.
		int xoff, int yoff,      // Upper-left corner of where to start.
		int            width,    // Width of each character
		unsigned char* trans) {
	ignore_unused_variable_warning(win);
	int x = xoff;
	int w;
	int chr;
	yoff += get_text_baseline();
	while ((chr = static_cast<unsigned char>(*text)) != 0) {
		Shape_frame* shape = next_frame(text, nullptr);
		if (!shape || !shape->is_rle()) {
			auto oldflags = std::cerr.flags();

			std::cerr << " unable to find rle frame for character '" << char(chr) << "' 0x" << std::hex << chr << " in font"
					  << std::endl;
			std::cerr.flags(oldflags);

			continue;
		}
		x += w = (width - shape->get_width()) / 2;
		if (trans) {
			shape->paint_rle_remapped(x, yoff, trans);
		} else {
			shape->paint_rle(x, yoff);
		}
		x += width - w;
	}
	return x - xoff;
}

/*
 *  Draw text at a given location (which is the upper-left corner of the
 *  place to draw. Text will be drawn with the fixed width specified.
 *
 *  Output: Width in pixels of what was drawn.
 */

int Font::paint_text_fixedwidth(
		Image_buffer8* win,        // Buffer to paint in.
		const char*    text,       // What to draw.
		int            textlen,    // Length of text.
		int xoff, int yoff,        // Upper-left corner of where to start.
		int            width,      // Width of each character
		unsigned char* trans) {
	ignore_unused_variable_warning(win);
	int w;
	int x = xoff;
	yoff += get_text_baseline();
	const char* const end = text + textlen;
	while (text < end) {
		const int    chr   = static_cast<unsigned char>(*text);
		Shape_frame* shape = next_frame(text, end);
		if (!shape || !shape->is_rle()) {
			auto oldflags = std::cerr.flags();
			std::cerr << " unable to find rle frame for character '" << char(chr) << "' 0x" << std::hex << chr << " in font"
					  << std::endl;

			std::cerr.flags(oldflags);
			continue;
		}
		x += w = (width - shape->get_width()) / 2;
		if (trans) {
			shape->paint_rle_remapped(x, yoff, trans);
		} else {
			shape->paint_rle(x, yoff);
		}
		x += width - w;
	}
	return x - xoff;
}

/*
 *  Get the width in pixels of a 0-delimited string.
 */

int Font::get_text_width(const char* text) {
	int width = 0;
	if (font_shapes) {
		while (*text) {
			Shape_frame* shape = next_frame(text, nullptr);
			if (shape && shape->is_rle()) {
				width += shape->get_width() + hor_lead;
			}
		}
	}
	return width;
}

/*
 *  Get the width in pixels of a string given by length.
 */

int Font::get_text_width(
		const char* text,
		int         textlen    // Length of text.
) {
	int width = 0;
	if (font_shapes) {
		const char* const end = text + textlen;
		while (text < end) {
			Shape_frame* shape = next_frame(text, end);
			if (shape && shape->is_rle()) {
				width += shape->get_width() + hor_lead;
			}
		}
	}
	return width;
}

void Font::get_text_box_dims(const char* text, int& width, int& height, int vert_lead) {
	width         = 0;
	height        = 0;
	int cur_width = 0;

	int num_lines = 1;
	if (font_shapes) {
		while (*text) {
			if (*text == '\n') {
				num_lines++;
				width     = std::max(width, cur_width);
				cur_width = 0;
			}
			Shape_frame* shape = next_frame(text, nullptr);
			if (shape && shape->is_rle()) {
				cur_width += shape->get_width() + hor_lead;
			}
		}
		width  = std::max(width, cur_width);
		height = num_lines * (get_text_height() + vert_lead + ver_lead);
	}
}

/*
 *  Get font line-height.
 */

int Font::get_text_height() {
	// Note, I wont assume the fonts exist
	// Shape_frame *A = font_shapes->get_frame('A');
	// Shape_frame *y = font_shapes->get_frame('y');
	return highest + lowest + 1;
}

/*
 *  Get font baseline as the distance from the top.
 */

int Font::get_text_baseline() {
	// Shape_frame *A = font_shapes->get_frame('A');
	return highest;
}

/*
 *  Find cursor location within text, given (x,y) screen coords.
 *  Note:  This has to match paint_text_box().
 *
 *  Output: Offset in text if found.
 *      If not found, -offset of end of text there was room to paint.
 */

int Font::find_cursor(
		const char* text, int x, int y,    // Top-left corner of box.
		int w, int h,                      // Dimensions.
		int cx, int cy,                    // Mouse loc. to find.
		int vert_lead                      // Extra spacing between lines.
) {
	const char* start       = text;     // Remember the start.
	const int   endx        = x + w;    // Figure where to stop.
	int         curx        = x;
	int         cury        = y;
	const int   height      = get_text_height() + vert_lead + ver_lead;
	const int   space_width = get_text_width(" ", 1);
	const int   max_lines   = h / height;    // # lines that can be shown.
	int         cur_line    = 0;
	int         chr;

	while ((chr = *text) != 0) {
		switch (chr) {    // Special cases.
		case '\n':        // Next line.
			if (cy >= cury && cy < cury + height && cx >= curx && cx < x + w) {
				return static_cast<int>(text - start);
			}
			++text;
			curx = x;
			cur_line++;
			cury += height;
			if (cur_line >= max_lines) {
				break;    // No more room.
			}
			continue;
		case '\r':    //??
			++text;
			continue;
		case ' ':    // Space.
		case '\t':
			if (cy >= cury && cy < cury + height && cx >= curx && cx < curx + space_width) {
				return static_cast<int>(text - start);
			}
			++text;
			curx += space_width;
			continue;
		}
		if (cur_line >= max_lines) {
			break;
		}

		if (*text == '*') {
			text++;
			if (cur_line) {
				break;
			}
		}
		bool ucase_next = *text == '^';
		if (ucase_next) {    // Skip it.
			text++;
			// 日本語の字の前の ^ は無視する。
			if (static_cast<unsigned char>(*text) >= 0x80) {
				ucase_next = false;
			}
		}
		// Pass word & get its width.
		const char* ewrd = Pass_word(text);
		int         width;
		if (ucase_next) {
			const char c = static_cast<char>(toupper(static_cast<unsigned char>(*text)));
			width        = get_text_width(&c, 1u) + get_text_width(text + 1, static_cast<uint32>(ewrd - text - 1));
		} else {
			width = get_text_width(text, static_cast<uint32>(ewrd - text));
		}
		if (curx + width - hor_lead > endx) {
			// Word-wrap.
			// Past end of this line?
			if (cy >= cury && cy < cury + height && cx >= curx && cx < x + w) {
				return static_cast<int>(text - start - 1);
			}
			curx = x;
			cur_line++;
			cury += height;
			if (cur_line >= max_lines) {
				break;    // No more room.
			}
		}
		if (cy >= cury && cy < cury + height && cx >= curx && cx < curx + width) {
			const int woff = find_xcursor(text, static_cast<int>(ewrd - text), cx - curx);
			if (woff >= 0) {
				return static_cast<int>(text - start) + woff;
			}
		}
		curx += width;
		text = ewrd;    // Continue past the word.
	}
	if (cy >= cury && cy < cury + height &&    // End of last line?
		cx >= curx && cx < x + w) {
		return static_cast<int>(text - start);
	}
	return -static_cast<int>(text - start);    // Failed, so indicate where we are.
}

/*
 *  Find an x-coord. within a piece of text.
 *
 *  Output: Offset if found, else -1.
 */

int Font::find_xcursor(
		const char* text,
		int         textlen,    // Length of text.
		int         cx          // Loc. to find.
) {
	const char* start = text;
	int         curx  = 0;
	const char* const end = text + textlen;
	while (text < end) {
		const char*  here  = text;
		Shape_frame* shape = next_frame(text, end);
		if (shape && shape->is_rle()) {
			const int w = shape->get_width() + hor_lead;
			if (cx >= curx && cx < curx + w) {
				return static_cast<int>(here - start);
			}
			curx += w;
		}
	}
	return -1;
}

Font::Font() = default;

Font::Font(const File_spec& fname0, int index, int hlead, int vlead) {
	load(fname0, index, hlead, vlead);
}

Font::Font(const File_spec& fname0, const File_spec& fname1, int index, int hlead, int vlead) {
	load(fname0, fname1, index, hlead, vlead);
}

void Font::clean_up() {
	font_shapes.reset();
}

/**
 *  Loads a font from a multiobject.
 *  @param font_obj Where we are loading from.
 *  @param hleah    Horizontal lead of the font.
 *  @param vleah    Vertical lead of the font.
 */
int Font::load_internal(IDataSource& data, int hlead, int vlead) {
	if (!data.good()) {
		font_shapes.reset();
		hor_lead = 0;
		ver_lead = 0;
		return -1;
	} else {
		// Is it an IFF archive?
		char hdr[5] = {0};
		data.read(hdr, 4);
		data.seek(0);
		if (!strncmp(hdr, "font", 4)) {
			data.skip(8);    // Yes, skip first 8 bytes.
		}
		font_shapes = std::make_unique<Shape_file>(&data);
		hor_lead    = hlead;
		ver_lead    = vlead;
		calc_highlow();
	}
	return 0;
}

int Font::load(const File_spec& fname0, int index, int hlead, int vlead) {
	clean_up();
	IExultDataSource data(fname0, index);
	return load_internal(data, hlead, vlead);
}

int Font::load(const File_spec& fname0, const File_spec& fname1, int index, int hlead, int vlead) {
	clean_up();
	IExultDataSource data(fname0, fname1, index);
	return load_internal(data, hlead, vlead);
}

int Font::center_text(Image_buffer8* win, int x, int y, const char* s, unsigned char* trans) {
	return draw_text(win, x - get_text_width(s) / 2, y, s, trans);
}

void Font::calc_highlow() {
	bool unset = true;
	jglyphs.clear();
	jready = false;

	for (int i = 0; i < font_shapes->get_num_frames(); i++) {
		Shape_frame* f = font_shapes->get_frame(i);

		if (!f || !f->is_rle()) {
			continue;
		}

		if (unset) {
			unset   = false;
			highest = f->get_yabove();
			lowest  = f->get_ybelow();
			continue;
		}

		if (f->get_yabove() > highest) {
			highest = f->get_yabove();
		}
		if (f->get_ybelow() > lowest) {
			lowest = f->get_ybelow();
		}
	}
	// 日本語フォントがあるなら、その字が収まる高さにする。
	if (!unset && Get_jfont(8)) {
		prepare_jglyphs();
		const JFont* use = Get_jfont(jsize);
		if (!use) {
			use = Get_jfont(8);
		}
		const int need = use->h + (joutline >= 0 ? 2 : 0);
		const int line = highest + lowest + 1;
		if (need > line) {
			lowest += need - line;
		}
		jglyphs.clear();
		jready = false;
	}
}

FontManager::~FontManager() {
	reset();
}

/**
 *  Loads a font from a File_spec.
 *  @param name Name to give to this font.
 *  @param fname0   First file spec.
 *  @param index    Number of font to load.
 *  @param hleah    Horizontal lead of the font.
 *  @param vleah    Vertical lead of the font.
 */
void FontManager::add_font(const char* name, const File_spec& fname0, int index, int hlead, int vlead) {
	remove_font(name);

	auto font = std::make_shared<Font>(fname0, index, hlead, vlead);

	fonts[name] = font;
}

/**
 *  Loads a font from a File_spec.
 *  @param name Name to give to this font.
 *  @param fname0   First file spec.
 *  @param fname1   Second file spec.
 *  @param index    Number of font to load.
 *  @param hleah    Horizontal lead of the font.
 *  @param vleah    Vertical lead of the font.
 */
void FontManager::add_font(const char* name, const File_spec& fname0, const File_spec& fname1, int index, int hlead, int vlead) {
	remove_font(name);

	auto font = std::make_shared<Font>(fname0, fname1, index, hlead, vlead);

	fonts[name] = font;
}

void FontManager::remove_font(const char* name) {
	fonts.erase(name);
}

std::shared_ptr<Font> FontManager::get_font(const char* name) {
	return fonts[name];
}

void FontManager::reset() {
	fonts.clear();
}
