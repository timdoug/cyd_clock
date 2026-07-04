// Translation data: hand-edited, plain UTF-8 (NFC). This file is the one
// deliberate exception to the ASCII-only source rule; everything else in
// the repo stays ASCII. Included by i18n.c.
//
// Editing translations needs no special tooling - this is portable C.
// If an edit introduces a character that has no glyph yet, run
// python3 tools/check_i18n.py to find out (it renders as '?' on the
// device until then), and regenerate glyph bitmaps with
// `swift tools/gen_fonts.swift` on macOS. The generator scans this file:
// characters in the lang_ja/zh/zh_hant/ko tables (and their weekday,
// month, name, and date-suffix tables) go to that language's font;
// everything else goes into the base font's supplementary table.
//
// A missing string id is NULL and falls back to English via tr();
// English is the complete reference set.
//
// Each language lives in main/i18n/<id>.inc; the registration
// tables expand from main/i18n/lang_list.inc, which also generates
// the lang_t enum in i18n.h.

#include "i18n/en.inc"
#include "i18n/es.inc"
#include "i18n/fr.inc"
#include "i18n/de.inc"
#include "i18n/pt.inc"
#include "i18n/it.inc"
#include "i18n/nl.inc"
#include "i18n/sv.inc"
#include "i18n/da.inc"
#include "i18n/no.inc"
#include "i18n/fi.inc"
#include "i18n/pl.inc"
#include "i18n/cs.inc"
#include "i18n/tr.inc"
#include "i18n/ru.inc"
#include "i18n/id.inc"
#include "i18n/hr.inc"
#include "i18n/hu.inc"
#include "i18n/ro.inc"
#include "i18n/bg.inc"
#include "i18n/uk.inc"
#include "i18n/ja.inc"
#include "i18n/zh.inc"
#include "i18n/zh_hant.inc"
#include "i18n/ko.inc"
#include "i18n/el.inc"
#include "i18n/sk.inc"
#include "i18n/sl.inc"
#include "i18n/lv.inc"
#include "i18n/lt.inc"
#include "i18n/et.inc"
#include "i18n/vi.inc"
#include "i18n/ms.inc"
#include "i18n/ca.inc"
#include "i18n/is.inc"
#include "i18n/sr.inc"
#include "i18n/az.inc"
#include "i18n/be.inc"
#include "i18n/cy.inc"
#include "i18n/eo.inc"
#include "i18n/eu.inc"
#include "i18n/fil.inc"
#include "i18n/fo.inc"
#include "i18n/ga.inc"
#include "i18n/gl.inc"
#include "i18n/kk.inc"
#include "i18n/mk.inc"
#include "i18n/sq.inc"
#include "i18n/sw.inc"
#include "i18n/ka.inc"
#include "i18n/hy.inc"
#include "i18n/ar.inc"
#include "i18n/fa.inc"
#include "i18n/he.inc"
#include "i18n/hi.inc"
#include "i18n/bn.inc"
#include "i18n/th.inc"
#include "i18n/af.inc"
#include "i18n/uz.inc"
#include "i18n/jv.inc"
#include "i18n/ky.inc"
#include "i18n/mn.inc"
#include "i18n/ur.inc"
#include "i18n/mr.inc"
#include "i18n/ta.inc"
#include "i18n/te.inc"
#include "i18n/pa.inc"
#include "i18n/gu.inc"
#include "i18n/kn.inc"
#include "i18n/ml.inc"
#include "i18n/ne.inc"
#include "i18n/si.inc"
#include "i18n/km.inc"
#include "i18n/lo.inc"
#include "i18n/my.inc"
#include "i18n/am.inc"
#include "i18n/bs.inc"
#include "i18n/nn.inc"
#include "i18n/gd.inc"
#include "i18n/so.inc"
#include "i18n/zu.inc"
#include "i18n/tk.inc"
#include "i18n/ht.inc"
#include "i18n/qu.inc"
#include "i18n/rm.inc"
#include "i18n/dsb.inc"
#include "i18n/hsb.inc"
#include "i18n/ha.inc"
#include "i18n/ig.inc"
#include "i18n/ak.inc"
#include "i18n/ba.inc"
#include "i18n/cv.inc"
#include "i18n/kok.inc"
#include "i18n/as.inc"
#include "i18n/ti.inc"
#include "i18n/yo.inc"
#include "i18n/pcm.inc"
#include "i18n/yue.inc"
#include "i18n/pa_arab.inc"
#include "i18n/bho.inc"
#include "i18n/ps.inc"
#include "i18n/or.inc"
#include "i18n/sd.inc"
#include "i18n/om.inc"
#include "i18n/su.inc"
#include "i18n/ceb.inc"
#include "i18n/mg.inc"
#include "i18n/mai.inc"

static const char *const *const lang_tables[LANG_COUNT] = {
#define I18N_PLAIN(id, up)  [LANG_##up] = lang_##id,
#define I18N_SHAPED(id, up) [LANG_##up] = lang_##id##_shaped,
#include "i18n/lang_list.inc"
#undef I18N_PLAIN
#undef I18N_SHAPED
};

static const char *const *const weekdays_by_lang[LANG_COUNT] = {
#define I18N_PLAIN(id, up)  [LANG_##up] = weekdays_##id,
#define I18N_SHAPED(id, up) [LANG_##up] = weekdays_##id##_shaped,
#include "i18n/lang_list.inc"
#undef I18N_PLAIN
#undef I18N_SHAPED
};

static const char *const *const months_by_lang[LANG_COUNT] = {
#define I18N_PLAIN(id, up)  [LANG_##up] = months_##id,
#define I18N_SHAPED(id, up) [LANG_##up] = months_##id##_shaped,
#include "i18n/lang_list.inc"
#undef I18N_PLAIN
#undef I18N_SHAPED
};

static const char *const lang_names[LANG_COUNT] = {
#define I18N_PLAIN(id, up)  [LANG_##up] = lang_name_##id,
#define I18N_SHAPED(id, up) [LANG_##up] = lang_name_##id##_shaped,
#include "i18n/lang_list.inc"
#undef I18N_PLAIN
#undef I18N_SHAPED
};
