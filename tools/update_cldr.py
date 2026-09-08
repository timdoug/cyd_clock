#!/usr/bin/env python3
"""Download CLDR JSON and derive compact clock-locale display data.

The firmware should not carry the whole CLDR corpus. This tool pins an
upstream cldr-json release, caches the full release zip under .cache/, and
vendors only the small subset needed to reason about clock/date display:

  * text direction
  * default numbering system and digits
  * default calendar preference by likely territory
  * weekday/month names and compact weekday+date patterns

The tool writes both a reviewable JSON snapshot and a compact generated C
include consumed by the firmware date renderer. The full CLDR corpus stays in
.cache/ and is not committed.
"""

from __future__ import annotations

import argparse
import datetime as _dt
import hashlib
import json
import re
import sys
import urllib.request
import zipfile
from pathlib import Path


ROOT = Path(__file__).resolve().parent.parent
DEFAULT_VERSION = "48.2.0"
SAMPLE_DATE = _dt.date(2026, 7, 6)
CLOCK_SKELETON = "yMMMEd"
LDML_FIELD_CHARS = set("ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz")
SHAPED_SCOPES = {
    "ar", "fa", "he", "hi", "bn", "th", "ur", "mr", "ta", "te", "pa", "gu",
    "kn", "ml", "ne", "si", "km", "lo", "my", "am", "kok", "as", "ti", "yo",
    "pcm", "pa_arab", "bho", "ps", "or", "sd", "mai",
}
TEXT_DIR_ENUM = {
    "left-to-right": "I18N_TEXT_LTR",
    "right-to-left": "I18N_TEXT_RTL",
}
CALENDAR_ENUM = {
    "gregorian": "I18N_CAL_GREGORIAN",
    "persian": "I18N_CAL_PERSIAN",
    "buddhist": "I18N_CAL_BUDDHIST",
}


def parse_lang_list(path: Path) -> list[str]:
    text = path.read_text(encoding="utf-8")
    return re.findall(r"I18N_(?:PLAIN|SHAPED)\((\w+),", text)


def cldr_tag(code: str) -> str:
    parts = code.replace("_", "-").split("-")
    out = []
    for i, part in enumerate(parts):
        if i == 0:
            out.append(part.lower())
        elif len(part) == 4:
            out.append(part.title())
        elif len(part) in (2, 3):
            out.append(part.upper())
        else:
            out.append(part)
    return "-".join(out)


def candidate_tags(code: str) -> list[str]:
    tag = cldr_tag(code)
    candidates = [tag]
    if "-" in tag:
        candidates.append(tag.split("-", 1)[0])
    # Historical/project aliases we may carry in lang_list.inc.
    aliases = {
        "no": ["nb"],
        "zh-Hant": ["zh-Hant", "zh"],
    }
    candidates.extend(aliases.get(tag, []))
    seen = set()
    return [c for c in candidates if not (c in seen or seen.add(c))]


def download(url: str, dest: Path) -> None:
    dest.parent.mkdir(parents=True, exist_ok=True)
    tmp = dest.with_suffix(dest.suffix + ".tmp")
    print(f"Downloading {url}", file=sys.stderr)
    with urllib.request.urlopen(url) as response, tmp.open("wb") as out:
        while True:
            chunk = response.read(1024 * 1024)
            if not chunk:
                break
            out.write(chunk)
    tmp.replace(dest)


def sha256(path: Path) -> str:
    h = hashlib.sha256()
    with path.open("rb") as f:
        for chunk in iter(lambda: f.read(1024 * 1024), b""):
            h.update(chunk)
    return h.hexdigest()


class CldrZip:
    def __init__(self, path: Path):
        self.path = path
        self.zf = zipfile.ZipFile(path)
        self.names = self.zf.namelist()
        self._suffix_map: dict[str, str] = {}
        for name in self.names:
            if name.endswith("/"):
                continue
            self._suffix_map[name] = name
            # Keep the shortest matching suffix winner; release zips have a
            # root directory prefix, and all data paths below cldr-json are
            # unique.
            for marker in ("/cldr-json/",):
                if marker in name:
                    suffix = name[name.index(marker) + 1 :]
                    self._suffix_map[suffix] = name
                    break
            else:
                self._suffix_map[name.split("/", 1)[-1]] = name

    def read_text_suffix(self, suffix: str) -> str | None:
        name = self._suffix_map.get(suffix)
        if name is None:
            matches = [n for n in self.names if n.endswith(suffix)]
            if not matches:
                return None
            name = sorted(matches, key=len)[0]
        return self.zf.read(name).decode("utf-8")

    def read_json_suffix(self, suffix: str) -> dict | None:
        text = self.read_text_suffix(suffix)
        return json.loads(text) if text is not None else None

    def read_locale(self, package: str, tag_candidates: list[str],
                    filename: str) -> tuple[str, dict] | tuple[None, None]:
        for tag in tag_candidates:
            suffix = f"{package}/main/{tag}/{filename}"
            data = self.read_json_suffix(suffix)
            if data is None:
                continue
            main = data.get("main", {})
            if tag in main:
                return tag, main[tag]
            if main:
                key = next(iter(main))
                return key, main[key]
        return None, None


def gregorian_to_jalali(gy: int, gm: int, gd: int) -> tuple[int, int, int]:
    gdm = [31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31]
    gy2 = gy - 1600
    g_day_no = 365 * gy2 + (gy2 + 3) // 4 - (gy2 + 99) // 100 + (gy2 + 399) // 400
    for i in range(gm - 1):
        g_day_no += gdm[i]
    if gm > 2 and ((gy % 4 == 0 and gy % 100 != 0) or gy % 400 == 0):
        g_day_no += 1
    g_day_no += gd - 1
    j_day_no = g_day_no - 79
    j_np = j_day_no // 12053
    j_day_no %= 12053
    jy = 979 + 33 * j_np + 4 * (j_day_no // 1461)
    j_day_no %= 1461
    if j_day_no >= 366:
        jy += (j_day_no - 366) // 365 + 1
        j_day_no = (j_day_no - 366) % 365
    jm = 1
    while jm <= 11 and j_day_no >= (31 if jm <= 6 else 30):
        j_day_no -= 31 if jm <= 6 else 30
        jm += 1
    return int(jy), int(jm), int(j_day_no + 1)


def likely_territory(tag: str, likely_subtags: dict) -> str:
    def territory_from_max(maximized: str | None) -> str | None:
        if not maximized:
            return None
        for part in maximized.split("-"):
            if len(part) in (2, 3) and part.upper() == part:
                return part
        return None

    keys = [tag]
    if "-" in tag:
        keys.append(tag.split("-", 1)[0])
    for key in keys:
        terr = territory_from_max(likely_subtags.get(key))
        if terr:
            return terr
    return "001"


def calendar_preference(territory: str, prefs: dict) -> list[str]:
    return prefs.get(territory) or prefs.get("001") or ["gregorian"]


def get_nested(obj: dict, keys: list[str], default=None):
    cur = obj
    for key in keys:
        if not isinstance(cur, dict) or key not in cur:
            return default
        cur = cur[key]
    return cur


def ordered_values(table: dict | None, count: int) -> list[str]:
    if not table:
        return []
    return [table.get(str(i), "") for i in range(1, count + 1)]


def day_values(table: dict | None) -> list[str]:
    if not table:
        return []
    keys = ["sun", "mon", "tue", "wed", "thu", "fri", "sat"]
    return [table.get(k, "") for k in keys]


def tokenize_ldml(pattern: str) -> list[tuple[str, str]]:
    tokens: list[tuple[str, str]] = []
    i = 0
    while i < len(pattern):
        ch = pattern[i]
        if ch == "'":
            i += 1
            lit = ""
            while i < len(pattern):
                if pattern[i] == "'":
                    if i + 1 < len(pattern) and pattern[i + 1] == "'":
                        lit += "'"
                        i += 2
                        continue
                    i += 1
                    break
                lit += pattern[i]
                i += 1
            if lit:
                tokens.append(("literal", lit))
            continue
        if ch in LDML_FIELD_CHARS:
            j = i + 1
            while j < len(pattern) and pattern[j] == ch:
                j += 1
            tokens.append(("field", pattern[i:j]))
            i = j
            continue
        j = i + 1
        while j < len(pattern) and pattern[j] != "'" and pattern[j] not in LDML_FIELD_CHARS:
            j += 1
        tokens.append(("literal", pattern[i:j]))
        i = j
    return tokens


def localized_digits(s: str, digits: str) -> str:
    if not digits or digits == "0123456789":
        return s
    return "".join(digits[ord(ch) - ord("0")] if "0" <= ch <= "9" else ch for ch in s)


def sample_date_parts(calendar: str, sample: _dt.date) -> tuple[int, int, int]:
    if calendar == "persian":
        return gregorian_to_jalali(sample.year, sample.month, sample.day)
    if calendar == "buddhist":
        return sample.year + 543, sample.month, sample.day
    return sample.year, sample.month, sample.day


def render_sample(pattern: str, calendar_data: dict, calendar: str,
                  digits: str, sample: _dt.date = SAMPLE_DATE) -> str:
    year, month, day = sample_date_parts(calendar, sample)
    weekday = (sample.weekday() + 1) % 7  # Python Mon=0, CLDR keys start Sun.
    months = calendar_data.get("months", {})
    days = calendar_data.get("days", {})
    eras = calendar_data.get("eras", {})

    def month_name(width: str) -> str:
        table = get_nested(months, ["format", width], {})
        return table.get(str(month), str(month))

    def day_name(width: str) -> str:
        keys = ["sun", "mon", "tue", "wed", "thu", "fri", "sat"]
        table = get_nested(days, ["format", width], {})
        return table.get(keys[weekday], keys[weekday])

    out = ""
    for kind, text in tokenize_ldml(pattern):
        if kind == "literal":
            out += text
            continue
        ch = text[0]
        n = len(text)
        if ch in "yYuU":
            out += str(year).zfill(n) if n == 2 else str(year)
        elif ch in "ML":
            if n == 5:
                out += month_name("narrow")
            elif n >= 4:
                out += month_name("wide")
            elif n == 3:
                out += month_name("abbreviated")
            else:
                out += str(month).zfill(n)
        elif ch == "d":
            out += str(day).zfill(n)
        elif ch in "Eec":
            if n == 5:
                out += day_name("narrow")
            elif n == 6:
                out += day_name("short")
            elif n >= 4:
                out += day_name("wide")
            else:
                out += day_name("abbreviated")
        elif ch == "G":
            era_table = eras.get("eraAbbr", {})
            era = era_table.get("1") or era_table.get("0") or ""
            out += era
        else:
            out += "{" + text + "}"
    return localized_digits(out, digits)


def calendar_summary(locale_data: dict, calendar: str, digits: str) -> dict:
    cal = get_nested(locale_data, ["dates", "calendars", calendar], {})
    if not cal:
        return {}
    formats = cal.get("dateFormats", {})
    available = get_nested(cal, ["dateTimeFormats", "availableFormats"], {})
    if available.get(CLOCK_SKELETON):
        pattern = available[CLOCK_SKELETON]
        pattern_source = f"availableFormats/{CLOCK_SKELETON}"
    else:
        pattern = formats.get("medium")
        pattern_source = "dateFormats/medium"
    months = get_nested(cal, ["months", "format"], {})
    days = get_nested(cal, ["days", "format"], {})
    eras = cal.get("eras", {})
    era_table = eras.get("eraAbbr", {})
    summary = {
        "clock_skeleton": CLOCK_SKELETON,
        "clock_pattern": pattern,
        "clock_pattern_source": pattern_source,
        "clock_sample": render_sample(pattern, cal, calendar, digits) if pattern else "",
        "era_abbr": era_table.get("1") or era_table.get("0") or "",
        "date_formats": {
            k: formats.get(k, "") for k in ("short", "medium", "long", "full")
        },
        "months": {
            "abbreviated": ordered_values(months.get("abbreviated"), 12),
            "wide": ordered_values(months.get("wide"), 12),
        },
        "weekdays": {
            "abbreviated": day_values(days.get("abbreviated")),
            "short": day_values(days.get("short")),
            "wide": day_values(days.get("wide")),
        },
    }
    return summary


def locale_summary(cldr: CldrZip, code: str, likely: dict, cal_prefs: dict,
                   numbering_systems: dict) -> dict:
    candidates = candidate_tags(code)
    layout_tag, layout = cldr.read_locale("cldr-misc-full", candidates, "layout.json")
    numbers_tag, numbers = cldr.read_locale("cldr-numbers-full", candidates, "numbers.json")
    greg_tag, greg = cldr.read_locale("cldr-dates-full", candidates, "ca-gregorian.json")

    chosen_tag = layout_tag or numbers_tag or greg_tag or candidates[0]
    territory = likely_territory(chosen_tag, likely)
    pref = calendar_preference(territory, cal_prefs)

    number_data = numbers.get("numbers", {}) if numbers else {}
    default_numbering = number_data.get("defaultNumberingSystem", "latn")
    ns_info = numbering_systems.get(default_numbering, {})
    digits = ns_info.get("_digits", "0123456789")

    orientation = get_nested(layout or {}, ["layout", "orientation"], {})
    character_order = orientation.get("characterOrder", "left-to-right")

    calendars = {}
    if greg:
        calendars["gregorian"] = calendar_summary(greg, "gregorian", digits)

    extra_calendar_packages = {
        "buddhist": ("cldr-cal-buddhist-full", "ca-buddhist.json"),
        "persian": ("cldr-cal-persian-full", "ca-persian.json"),
    }
    for calendar in pref:
        if calendar == "gregorian" or calendar not in extra_calendar_packages:
            continue
        package, filename = extra_calendar_packages[calendar]
        cal_tag, cal_data = cldr.read_locale(package, candidates, filename)
        if cal_data:
            calendars[calendar] = calendar_summary(cal_data, calendar, digits)

    return {
        "cldr_locale": chosen_tag,
        "source_locales": {
            "layout": layout_tag,
            "numbers": numbers_tag,
            "gregorian": greg_tag,
        },
        "likely_territory": territory,
        "character_order": character_order,
        "default_numbering_system": default_numbering,
        "digits": digits,
        "calendar_preference": pref,
        "clock_default_calendar": pref[0] if pref else "gregorian",
        "calendars": calendars,
    }


def c_string(value: str) -> str:
    escaped = value.replace("\\", "\\\\").replace('"', '\\"')
    return f'"{escaped}"'


def c_array(values: list[str]) -> str:
    return "{ " + ", ".join(c_string(v) for v in values) + " }"


def c_ident(code: str) -> str:
    return re.sub(r"[^a-zA-Z0-9_]", "_", code)


def has_non_ascii(value: str) -> bool:
    return any(ord(ch) > 0x7E for ch in value)


def pattern_field_widths(pattern: str, chars: str) -> list[int]:
    widths = []
    for kind, text in tokenize_ldml(pattern):
        if kind == "field" and text[0] in chars:
            widths.append(len(text))
    return widths


def select_months(calendar: dict, pattern: str) -> list[str]:
    width = max(pattern_field_widths(pattern, "ML") or [3])
    key = "wide" if width >= 4 else "abbreviated"
    months = calendar.get("months", {})
    values = months.get(key) or months.get("abbreviated") or months.get("wide") or []
    return values


def select_weekdays(calendar: dict, pattern: str) -> list[str]:
    width = max(pattern_field_widths(pattern, "Eec") or [3])
    if width == 5:
        preferred = "narrow"
    elif width == 6:
        preferred = "short"
    elif width >= 4:
        preferred = "wide"
    else:
        preferred = "abbreviated"
    weekdays = calendar.get("weekdays", {})
    values = weekdays.get(preferred) or weekdays.get("short") or weekdays.get("abbreviated") or weekdays.get("wide") or []
    return values


def locale_calendar(locale: dict) -> tuple[str, dict]:
    calendar = locale.get("clock_default_calendar") or "gregorian"
    calendars = locale.get("calendars", {})
    if calendar not in calendars:
        calendar = "gregorian"
    return calendar, calendars.get(calendar, {})


def token_initializer(token: tuple[str, str], scope: str, literal_names: dict[str, str]) -> str | None:
    kind, text = token
    if kind == "literal":
        if not text:
            return None
        literal = literal_names.get(text) or c_string(text)
        return f"    {{ I18N_DATE_LITERAL, 0, {literal} }},"

    ch = text[0]
    width = len(text)
    if ch in "yYuU":
        return f"    {{ I18N_DATE_YEAR, {width}, NULL }},"
    if ch in "ML":
        return f"    {{ I18N_DATE_MONTH, {width}, NULL }},"
    if ch == "d":
        return f"    {{ I18N_DATE_DAY, {width}, NULL }},"
    if ch in "Eec":
        return f"    {{ I18N_DATE_WEEKDAY, {width}, NULL }},"
    if ch == "G":
        return f"    {{ I18N_DATE_ERA, {width}, NULL }},"
    print(f"{scope}: unsupported LDML field {text!r}; preserving literally", file=sys.stderr)
    return f"    {{ I18N_DATE_LITERAL, 0, {c_string(text)} }},"


def write_c_locale_include(path: Path, snapshot: dict) -> None:
    locales: dict[str, dict] = snapshot["locales"]
    out: list[str] = [
        "/*",
        " * Generated by tools/update_cldr.py; do not edit by hand.",
        f" * Source: CLDR {snapshot['cldr_version']} ({snapshot['source_sha256'][:16]})",
        " * Logical upstream patterns remain in third_party/cldr/clock_locale_data.json.",
        " */",
        "",
    ]
    locale_rows: list[str] = []

    for code, locale in locales.items():
        ident = c_ident(code)
        calendar, calendar_data = locale_calendar(locale)
        pattern = calendar_data.get("clock_pattern") or "E d MMM y"
        text_dir = locale.get("character_order", "left-to-right")
        is_rtl = text_dir == "right-to-left"
        tokens = tokenize_ldml(pattern)
        token_order = list(reversed(tokens)) if is_rtl else tokens
        months = select_months(calendar_data, pattern)
        weekdays = select_weekdays(calendar_data, pattern)
        era = calendar_data.get("era_abbr", "")
        if len(months) != 12:
            print(f"{code}: CLDR month table incomplete; using empty strings", file=sys.stderr)
            months = (months + [""] * 12)[:12]
        if len(weekdays) != 7:
            print(f"{code}: CLDR weekday table incomplete; using empty strings", file=sys.stderr)
            weekdays = (weekdays + [""] * 7)[:7]

        digits_symbol = f"cldr_digits_{ident}"
        out.append(f"static const char {digits_symbol}[] = {c_string(locale.get('digits', '0123456789'))};")

        literal_names: dict[str, str] = {}
        shaped_literal_values: list[str] = []
        if code in SHAPED_SCOPES:
            out.extend([
                "#if 0",
                f"// >>> cldr-date-source {code}",
                f"static const char *const cldr_weekdays_{ident}[7] = {c_array(weekdays)};",
                f"static const char *const cldr_months_{ident}[12] = {c_array(months)};",
                f"static const char cldr_era_{ident}[] = {c_string(era)};",
            ])
            for kind, text in token_order:
                if kind == "literal" and text and has_non_ascii(text) and text not in literal_names:
                    sym = f"cldr_literal_{ident}_{len(shaped_literal_values)}"
                    literal_names[text] = f"{sym}_shaped"
                    shaped_literal_values.append(text)
                    out.append(f"static const char {sym}[] = {c_string(text)};")
            out.extend([
                f"// <<< cldr-date-source {code}",
                "#endif",
                f"extern const char *const cldr_weekdays_{ident}_shaped[7];",
                f"extern const char *const cldr_months_{ident}_shaped[12];",
                f"extern const char cldr_era_{ident}_shaped[];",
            ])
            for i in range(len(shaped_literal_values)):
                out.append(f"extern const char cldr_literal_{ident}_{i}_shaped[];")
            weekdays_ref = f"cldr_weekdays_{ident}_shaped"
            months_ref = f"cldr_months_{ident}_shaped"
            era_ref = f"cldr_era_{ident}_shaped"
        else:
            out.append(f"static const char *const cldr_weekdays_{ident}[7] = {c_array(weekdays)};")
            out.append(f"static const char *const cldr_months_{ident}[12] = {c_array(months)};")
            out.append(f"static const char cldr_era_{ident}[] = {c_string(era)};")
            weekdays_ref = f"cldr_weekdays_{ident}"
            months_ref = f"cldr_months_{ident}"
            era_ref = f"cldr_era_{ident}"

        out.extend([
            f"static const i18n_date_token_t cldr_pattern_{ident}[] = {{",
            f"    /* {calendar}; {calendar_data.get('clock_pattern_source', '')}; logical: {pattern} */",
        ])
        if is_rtl:
            out.append("    /* Stored in visual token order because the firmware has no runtime bidi engine. */")
        for token in token_order:
            initializer = token_initializer(token, code, literal_names)
            if initializer:
                out.append(initializer)
        out.extend([
            "    { I18N_DATE_END, 0, NULL },",
            "};",
            "",
        ])

        locale_rows.append(
            f"    [LANG_{code.upper()}] = {{ {TEXT_DIR_ENUM.get(text_dir, 'I18N_TEXT_LTR')}, "
            f"{CALENDAR_ENUM.get(calendar, 'I18N_CAL_GREGORIAN')}, {digits_symbol}, "
            f"cldr_pattern_{ident}, {weekdays_ref}, {months_ref}, {era_ref} }},"
        )

    out.extend([
        "static const i18n_locale_info_t locale_info[LANG_COUNT] = {",
        *locale_rows,
        "};",
        "",
    ])
    path.write_text("\n".join(out), encoding="utf-8")


def write_vendor_readme(outdir: Path, version: str, source_url: str) -> None:
    readme = f"""# CLDR Clock Locale Snapshot

This directory contains a compact, generated subset of Unicode CLDR data used
to evaluate clock/date display behavior for this firmware.

Source: `{source_url}`
Version: `{version}`
License: Unicode License v3 (`Unicode-3.0`), copied in `LICENSE`.

Regenerate with:

```sh
make update-cldr CLDR_VERSION={version}
```

The full CLDR release zip is cached under `.cache/` and is intentionally not
committed.
"""
    (outdir / "README.md").write_text(readme, encoding="utf-8")


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--version", default=DEFAULT_VERSION)
    ap.add_argument("--cache-dir", default=str(ROOT / ".cache" / "cldr"))
    ap.add_argument("--out-dir", default=str(ROOT / "third_party" / "cldr"))
    ap.add_argument("--c-out", default=str(ROOT / "main" / "i18n_locale.inc"))
    ap.add_argument("--locales", nargs="*", help="override project language list")
    ap.add_argument("--no-download", action="store_true")
    args = ap.parse_args()

    version = args.version
    source_url = (
        "https://github.com/unicode-org/cldr-json/releases/download/"
        f"{version}/cldr-{version}-json-full.zip"
    )
    cache_path = Path(args.cache_dir) / f"cldr-{version}-json-full.zip"
    if not cache_path.exists():
        if args.no_download:
            print(f"{cache_path} does not exist and --no-download was set", file=sys.stderr)
            return 1
        download(source_url, cache_path)

    print(f"Reading {cache_path}", file=sys.stderr)
    cldr = CldrZip(cache_path)
    likely_data = cldr.read_json_suffix("cldr-core/supplemental/likelySubtags.json")
    cal_pref_data = cldr.read_json_suffix("cldr-core/supplemental/calendarPreferenceData.json")
    ns_data = cldr.read_json_suffix("cldr-core/supplemental/numberingSystems.json")
    if not likely_data or not cal_pref_data or not ns_data:
        print("Missing required CLDR supplemental JSON files", file=sys.stderr)
        return 1

    likely = get_nested(likely_data, ["supplemental", "likelySubtags"], {})
    cal_prefs = get_nested(cal_pref_data, ["supplemental", "calendarPreferenceData"], {})
    numbering_systems = get_nested(ns_data, ["supplemental", "numberingSystems"], {})

    locales = args.locales or parse_lang_list(ROOT / "main" / "i18n" / "lang_list.inc")
    snapshot = {
        "cldr_version": version,
        "source_url": source_url,
        "source_sha256": sha256(cache_path),
        "license": "Unicode-3.0",
        "generated_by": "tools/update_cldr.py",
        "sample_gregorian_date": SAMPLE_DATE.isoformat(),
        "locales": {},
    }
    for code in locales:
        snapshot["locales"][code] = locale_summary(
            cldr, code, likely, cal_prefs, numbering_systems
        )

    outdir = Path(args.out_dir)
    outdir.mkdir(parents=True, exist_ok=True)
    license_text = cldr.read_text_suffix("LICENSE")
    if license_text:
        (outdir / "LICENSE").write_text(license_text, encoding="utf-8")
    write_vendor_readme(outdir, version, source_url)
    out_path = outdir / "clock_locale_data.json"
    out_path.write_text(json.dumps(snapshot, ensure_ascii=False, indent=2, sort_keys=True) + "\n",
                        encoding="utf-8")
    print(f"Wrote {out_path}", file=sys.stderr)
    c_out = Path(args.c_out)
    write_c_locale_include(c_out, snapshot)
    print(f"Wrote {c_out}", file=sys.stderr)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
