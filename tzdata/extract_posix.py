#!/usr/bin/env python3
"""Extract POSIX TZ strings from IANA tzdata source files.

Reads zone1970.tab for the canonical list of zones, compiles tzdata
using zic, extracts POSIX TZ strings from TZif binaries, and generates
C code for main/ui_timezone.c with a two-level region/city structure.

Deduplicates zones within each region by functional POSIX equivalence
(same offset and DST rules regardless of abbreviation name), keeping the
largest city by population as the representative.
"""

import argparse
import io
import os
import re
import shutil
import subprocess
import sys
import tarfile
import tempfile
import urllib.request

# tzdata source region files to compile
REGION_FILES = [
    "northamerica", "southamerica", "europe", "asia",
    "africa", "antarctica", "australasia", "etcetera", "backward",
]

DEFAULT_ZIC = "/usr/sbin/zic"
IANA_RELEASE_URL = "https://data.iana.org/time-zones/releases"

REQUIRED_SOURCE_FILES = ["zone1970.tab"] + REGION_FILES


def normalize_release_name(version):
    """Return a full tzdata release filename stem, e.g. tzdata2026b."""
    if version.startswith("tzdata"):
        return version.removesuffix(".tar.gz")
    return f"tzdata{version}"


def latest_tzdata_release():
    """Return the newest tzdata release listed by IANA."""
    with urllib.request.urlopen(f"{IANA_RELEASE_URL}/") as response:
        index = response.read().decode("utf-8", errors="replace")

    releases = re.findall(r"tzdata(\d{4}[a-z]{1,2})\.tar\.gz", index)
    if not releases:
        raise RuntimeError("Could not find any tzdata releases in IANA index")

    return normalize_release_name(max(releases, key=lambda r: (int(r[:4]), r[4:])))


def fetch_tzdata(version):
    """Download and extract an IANA tzdata release into a temporary directory."""
    release = latest_tzdata_release() if version == "latest" else normalize_release_name(version)
    url = f"{IANA_RELEASE_URL}/{release}.tar.gz"
    print(f"Downloading {url}", file=sys.stderr)

    with urllib.request.urlopen(url) as response:
        archive = response.read()

    tempdir = tempfile.TemporaryDirectory()
    destdir = tempdir.name
    with tarfile.open(fileobj=io.BytesIO(archive), mode="r:gz") as tar:
        # The "data" filter rejects path traversal, absolute paths, links,
        # and device nodes - strictly stronger than the manual prefix check
        # this replaces (which missed symlink-then-write chains).
        tar.extractall(destdir, filter="data")

    print(f"Extracted {release} into temporary directory", file=sys.stderr)
    return tempdir


def check_tzdata_sources(srcdir):
    """Return required source filenames that are missing from srcdir."""
    return [
        name for name in REQUIRED_SOURCE_FILES
        if not os.path.exists(os.path.join(srcdir, name))
    ]


def run_zic(zic, destdir, path):
    """Run zic on one source file, surfacing its diagnostics on failure."""
    try:
        subprocess.run(
            [zic, "-d", destdir, path],
            check=True,
            capture_output=True,
            text=True,
        )
    except subprocess.CalledProcessError as e:
        print(e.stderr, file=sys.stderr, end="")
        raise


def compile_tzdata(srcdir, destdir, zic):
    """Compile tzdata source files into TZif binaries using zic.

    Every REGION_FILES entry is required (main() aborts earlier via
    check_tzdata_sources), so a missing region here is a hard error.
    """
    for region in REGION_FILES:
        run_zic(zic, destdir, os.path.join(srcdir, region))


def extract_posix_string(tzif_path):
    """Extract the POSIX TZ string from a TZif v2/v3 binary file.

    In TZif v2/v3, the file ends with \\n<posix_string>\\n. Returns "" for a
    legal-but-empty footer (a zone with no POSIX-representable rules), which
    the caller must skip rather than emit as a bogus UTC entry.
    """
    with open(tzif_path, "rb") as f:
        data = f.read()
    try:
        last_nl = data.rindex(b"\n")
        second_last_nl = data.rindex(b"\n", 0, last_nl)
    except ValueError:
        raise RuntimeError(
            f"{tzif_path}: no TZif footer (v1 file? zic too old?)"
        ) from None
    return data[second_last_nl + 1 : last_nl].decode("ascii")


def _parse_posix_offset(s):
    """Parse a POSIX offset string and return (minutes_from_utc, chars_consumed).

    POSIX offsets have inverted sign from UTC convention:
      5       -> -300 (UTC-5)
      -1      -> +60  (UTC+1)
      -5:30   -> +330 (UTC+5:30)
    """
    i = 0
    negate = False
    if i < len(s) and s[i] == "-":
        negate = True
        i += 1
    elif i < len(s) and s[i] == "+":
        i += 1

    match = re.match(r"(\d+)(?::(\d+))?", s[i:])
    if not match:
        return 0, i

    hours = int(match.group(1))
    minutes = int(match.group(2)) if match.group(2) else 0
    total = hours * 60 + minutes

    # Invert sign: POSIX positive = west of UTC = negative UTC offset
    # (a leading "-" means east of UTC, which stays positive)
    if not negate:
        total = -total

    return total, i + match.end()


def _skip_abbrev(s):
    """Skip a POSIX timezone abbreviation, return remaining string."""
    if s.startswith("<"):
        try:
            end = s.index(">")
        except ValueError:
            raise ValueError(
                f"malformed POSIX abbreviation, no closing '>': {s!r}"
            ) from None
        return s[end + 1:]
    i = 0
    while i < len(s) and s[i].isalpha():
        i += 1
    return s[i:]


def _format_offset(minutes):
    """Format a UTC offset in minutes as a display string."""
    if minutes >= 0:
        sign = "+"
    else:
        sign = "-"
        minutes = -minutes
    hours = minutes // 60
    mins = minutes % 60
    if mins:
        return f"UTC{sign}{hours}:{mins:02d}"
    return f"UTC{sign}{hours}"


def parse_utc_offset(posix_tz):
    """Parse a POSIX TZ string and return the standard (non-DST) UTC offset.

    Handles the Dublin/Ireland inversion where IST-1GMT0 defines summer as
    "standard" - detects this by checking if DST offset is behind standard
    (springs back instead of forward) and uses the real winter offset.
    """
    s = _skip_abbrev(posix_tz)
    std_minutes, consumed = _parse_posix_offset(s)
    s = s[consumed:]

    # Check for DST abbreviation + offset
    if s and (s[0].isalpha() or s[0] == "<"):
        s = _skip_abbrev(s)
        if s and (s[0].isdigit() or s[0] in "+-"):
            dst_minutes, _ = _parse_posix_offset(s)
        else:
            # Default DST offset is std + 60
            dst_minutes = std_minutes + 60

        # If DST is behind standard, the string is inverted (e.g., Dublin).
        # The "DST" offset is actually the real winter/standard time.
        if dst_minutes < std_minutes:
            return _format_offset(dst_minutes)

    return _format_offset(std_minutes)


def offset_sort_key(posix_tz):
    """Return numeric offset in minutes for sorting."""
    offset = parse_utc_offset(posix_tz)
    m = re.match(r"UTC([+-])(\d+)(?::(\d+))?", offset)
    if not m:
        return 0
    val = int(m.group(2)) * 60 + (int(m.group(3)) if m.group(3) else 0)
    return val if m.group(1) == "+" else -val


def read_zone1970_tab(path):
    """Read zone1970.tab and return the list of zone ID strings."""
    zones = []
    with open(path, encoding="utf-8") as f:
        for line in f:
            if line.startswith("#") or not line.strip():
                continue
            parts = line.split("\t")
            zone_id = parts[2].strip()
            zones.append(zone_id)
    return zones


def zone_display_name(zone_id):
    """Convert IANA zone ID to display city name.

    America/New_York -> "New York"
    America/Argentina/Buenos_Aires -> "Argentina/Buenos Aires"
    """
    parts = zone_id.split("/", 1)
    if len(parts) < 2:
        return zone_id
    return parts[1].replace("_", " ")


# Approximate city/metro populations (millions) for dedup representative selection.
# Only entries that appear in zone1970.tab are needed. Cities not listed default to 0.
CITY_POPULATIONS = {
    "Shanghai": 29, "Tokyo": 14, "Istanbul": 16, "Lagos": 16, "Kolkata": 15,
    "Karachi": 15, "Dhaka": 23, "Cairo": 22, "New_York": 18, "Sao_Paulo": 22,
    "Mexico_City": 22, "Mumbai": 21, "Beijing": 22, "Jakarta": 11, "Lima": 11,
    "London": 9.5, "Bangkok": 11, "Ho_Chi_Minh": 9, "Bogota": 8, "Tehran": 9,
    "Hong_Kong": 7.5, "Baghdad": 8, "Santiago": 7, "Riyadh": 7.5, "Singapore": 6,
    "Ankara": 5.7, "Nairobi": 5, "Johannesburg": 6, "Casablanca": 4, "Dubai": 3.5,
    "Kuala_Lumpur": 8, "Taipei": 7, "Toronto": 6.5, "Chicago": 9.5, "Los_Angeles": 13,
    "Paris": 11, "Madrid": 6.5, "Rome": 4.3, "Berlin": 3.7, "Kyiv": 3,
    "Bucharest": 2, "Warsaw": 1.8, "Athens": 3.2, "Amsterdam": 1.1,
    "Denver": 2.9, "Phoenix": 4.9, "Havana": 2.1, "Anchorage": 0.3,
    "Honolulu": 1, "Auckland": 1.6, "Sydney": 5.3, "Melbourne": 5,
    "Brisbane": 2.6, "Adelaide": 1.4, "Darwin": 0.15, "Perth": 2.1,
    "Halifax": 0.4, "Winnipeg": 0.8, "Edmonton": 1.4, "Vancouver": 2.6,
    "Buenos_Aires": 15, "Montevideo": 1.8, "Asuncion": 2.4,
    "Santo_Domingo": 3.3, "Caracas": 3, "La_Paz": 2.4, "Guayaquil": 2.7,
    "Guatemala": 3, "Tegucigalpa": 1.2, "Managua": 1.1, "Panama": 1.5,
    "San_Juan": 2.3, "Port-au-Prince": 2.8,
    "Beirut": 2.4, "Jerusalem": 1, "Gaza": 0.7, "Damascus": 2.5,
    "Amman": 4.3, "Kuwait": 3, "Doha": 2, "Muscat": 1.5, "Baku": 2.3,
    "Tbilisi": 1.2, "Yerevan": 1.1, "Kabul": 4.4, "Tashkent": 2.5,
    "Almaty": 2, "Kathmandu": 1.5, "Colombo": 0.75, "Yangon": 5.3,
    "Novosibirsk": 1.6, "Krasnoyarsk": 1.1, "Vladivostok": 0.6,
    "Magadan": 0.09, "Kamchatka": 0.18, "Sakhalin": 0.17, "Srednekolymsk": 0.003,
    "Irkutsk": 0.62, "Yakutsk": 0.31, "Omsk": 1.2, "Yekaterinburg": 1.5,
    "Samara": 1.2, "Volgograd": 1,
    "Abidjan": 5, "Khartoum": 6, "Addis_Ababa": 5, "Maputo": 1.1,
    "Juba": 0.5, "Windhoek": 0.43, "Tunis": 2.4, "Tripoli": 1.1,
    "El_Aaiun": 0.2, "Sao_Tome": 0.08, "Ndjamena": 1.3,
    "Pago_Pago": 0.004, "Adak": 0.3, "Marquesas": 0.01,
    "Gambier": 0.001, "Pitcairn": 0.00005, "Easter": 0.008,
    "Galapagos": 0.03, "Noumea": 0.18, "Norfolk": 0.002,
    "Chatham": 0.0006, "Tongatapu": 0.08, "Kiritimati": 0.006,
    "Fiji": 0.3, "Apia": 0.04, "Azores": 0.07, "Cape_Verde": 0.18,
    "Nuuk": 0.06, "Miquelon": 0.006, "Noronha": 0.003,
    "St_Johns": 0.11, "Eucla": 0.002, "Lord_Howe": 0.0004,
    "Troll": 0.00004, "Casey": 0.00003, "Davis": 0.00003,
    "Mawson": 0.00003, "Rothera": 0.00003, "Vostok": 0.00003,
    "Dublin": 1.4, "Lisbon": 2.9,
}


def city_population(zone_id):
    """Get approximate population for a zone ID's city."""
    # Try last component (e.g., America/New_York -> New_York)
    city = zone_id.rsplit("/", 1)[-1]
    return CITY_POPULATIONS.get(city, 0)


def normalize_posix_tz(posix_tz):
    """Normalize a POSIX TZ string to a canonical form for functional equivalence.

    Strips abbreviation names and replaces with a placeholder, keeping only
    the numeric offset and DST transition rules.

    Examples:
        PKT-5           -> <X>-5
        <+05>-5         -> <X>-5
        EST5EDT,...     -> <X>5<X>,...
        AEST-10AEDT,... -> <X>-10<X>,...
    """
    s = posix_tz
    result = []
    i = 0
    while i < len(s):
        if s[i] == "<":
            # Skip <abbrev>
            try:
                end = s.index(">", i)
            except ValueError:
                raise ValueError(
                    f"malformed POSIX abbreviation, no closing '>': {s!r}"
                ) from None
            result.append("<X>")
            i = end + 1
        elif s[i].isalpha():
            # Skip alpha abbreviation
            while i < len(s) and s[i].isalpha():
                i += 1
            result.append("<X>")
        elif s[i] == ",":
            # Rest is DST rules, append verbatim
            result.append(s[i:])
            break
        else:
            result.append(s[i])
            i += 1
    return "".join(result)


def generate_timezone_block(srcdir, zic):
    """Generate the C timezone arrays from extracted IANA tzdata sources."""
    zone1970_path = os.path.join(srcdir, "zone1970.tab")

    zone_ids = read_zone1970_tab(zone1970_path)

    with tempfile.TemporaryDirectory() as tmpdir:
        print("Compiling tzdata...", file=sys.stderr)
        compile_tzdata(srcdir, tmpdir, zic)

        # Etc/UTC comes from the "etcetera" region compiled above; it is not
        # listed in zone1970.tab, so it is looked up separately below.

        # Extract POSIX strings for all zones
        entries = []
        for zone_id in zone_ids:
            tzif_path = os.path.join(tmpdir, zone_id)
            if not os.path.exists(tzif_path):
                print(f"WARNING: {zone_id} not found", file=sys.stderr)
                continue
            posix_tz = extract_posix_string(tzif_path)
            if not posix_tz:
                print(f"WARNING: {zone_id} has no POSIX-representable rules; "
                      "skipped", file=sys.stderr)
                continue
            region = zone_id.split("/")[0]
            city = zone_display_name(zone_id)
            entries.append((region, city, posix_tz, zone_id))

        # Add UTC
        utc_path = os.path.join(tmpdir, "Etc/UTC")
        if os.path.exists(utc_path):
            entries.append(("UTC", "UTC", extract_posix_string(utc_path),
                            "Etc/UTC"))

        print(f"Total zones from zone1970.tab: {len(entries)}",
              file=sys.stderr)

        # --- Deduplicate by functional equivalence within region ---
        # Two POSIX strings that differ only in abbreviation names
        # (e.g., PKT-5 vs <+05>-5) compute identical times.
        # Only dedup within the same region so users can find entries
        # in their geographic area.
        by_region_normalized = {}
        for entry in entries:
            region, city, posix_tz, zone_id = entry
            norm = normalize_posix_tz(posix_tz)
            key = (region, norm)
            pop = city_population(zone_id)
            if key not in by_region_normalized or pop > by_region_normalized[key][1]:
                by_region_normalized[key] = (entry, pop)
        entries = [v[0] for v in by_region_normalized.values()]
        print(f"After functional dedup (within region): {len(entries)}",
              file=sys.stderr)

        # (UTC always survives dedup: the pass keys on the region, and the
        # UTC entry is the only member of its "UTC" region.)

        # Build city display labels with UTC offset and DST status
        labeled = []
        for region, city, posix_tz, zone_id in entries:
            offset = parse_utc_offset(posix_tz)
            has_dst = "," in posix_tz  # DST rules have comma-separated transitions
            if city == "UTC":
                label = f"UTC ({offset})"
            else:
                dst_str = "DST" if has_dst else "no DST"
                label = f"{city} ({offset}, {dst_str})"
            labeled.append((region, label, posix_tz))

        # Sort by region name, then by UTC offset, then by city label
        labeled.sort(key=lambda e: (e[0], offset_sort_key(e[2]), e[1]))

        # Collect unique regions in order
        seen_regions = []
        for region, _, _ in labeled:
            if region not in seen_regions:
                seen_regions.append(region)

        out = []
        out.append("// Generated from zone1970.tab - deduplicated within each region by")
        out.append("// functional POSIX equivalence, largest city by population as representative.")
        out.append("")

        out.append("static const char *regions[] = {")
        for r in seen_regions:
            out.append(f'    "{r}",')
        out.append("};")
        out.append(f"#define NUM_REGIONS {len(seen_regions)}")
        out.append("")

        out.append("static const timezone_entry_t timezones[] = {")
        for region, city, posix_tz in labeled:
            r_field = f'"{region}"'
            c_field = f'"{city}"'
            t_field = f'"{posix_tz}"'
            out.append(f"    {{{r_field + ',':<16s}{c_field + ',':<40s}{t_field}}},")
        out.append("};")
        out.append("#define NUM_TIMEZONES ((int)(sizeof(timezones) / sizeof(timezones[0])))")

        print(f"\n// {len(labeled)} zones, {len(seen_regions)} regions",
              file=sys.stderr)

        return "\n".join(out) + "\n\n"


def update_ui_timezone(path, generated_block):
    """Replace the generated timezone block in main/ui_timezone.c."""
    with open(path, encoding="utf-8") as f:
        source = f.read()

    pattern = (
        r"// Generated from zone1970\.tab.*?"
        r"#define NUM_TIMEZONES [^\n]*\n(?:\n)?"
    )
    updated, count = re.subn(pattern, generated_block, source, count=1, flags=re.S)
    if count != 1:
        raise RuntimeError(f"Could not find generated timezone block in {path}")

    with open(path, "w", encoding="utf-8") as f:
        f.write(updated)

    print(f"Updated {path}", file=sys.stderr)


def parse_args():
    parser = argparse.ArgumentParser(
        description="Generate POSIX TZ tables from IANA tzdata sources.",
    )
    parser.add_argument(
        "--download",
        nargs="?",
        const="latest",
        metavar="VERSION",
        help=(
            "download and extract an IANA tzdata release first "
            "(default: latest)"
        ),
    )
    parser.add_argument(
        "--srcdir",
        default=os.path.dirname(os.path.abspath(__file__)),
        help="directory containing extracted IANA tzdata source files",
    )
    parser.add_argument(
        "--update-ui",
        metavar="PATH",
        help="replace the generated timezone block in main/ui_timezone.c",
    )
    parser.add_argument(
        "--output",
        metavar="PATH",
        help="write generated C block to PATH instead of stdout",
    )
    parser.add_argument(
        "--zic",
        default=DEFAULT_ZIC,
        help=f"path to zic compiler (default: {DEFAULT_ZIC})",
    )
    return parser.parse_args()


def main():
    args = parse_args()
    srcdir = os.path.abspath(args.srcdir)
    source_tempdir = None

    if args.download:
        source_tempdir = fetch_tzdata(args.download)
        srcdir = source_tempdir.name

    try:
        missing = check_tzdata_sources(srcdir)
        if missing:
            print("Missing tzdata source files:", file=sys.stderr)
            for name in missing:
                print(f"  {name}", file=sys.stderr)
            print(
                f"\nRun: {sys.argv[0]} --download",
                file=sys.stderr,
            )
            return 1

        if not shutil.which(args.zic) and not os.path.exists(args.zic):
            print(f"zic not found: {args.zic}", file=sys.stderr)
            return 1

        generated_block = generate_timezone_block(srcdir, args.zic)

        if args.output:
            with open(args.output, "w", encoding="utf-8") as f:
                f.write(generated_block)
            print(f"Wrote {args.output}", file=sys.stderr)
        elif not args.update_ui:
            print(generated_block, end="")

        if args.update_ui:
            update_ui_timezone(args.update_ui, generated_block)

        return 0
    finally:
        if source_tempdir:
            source_tempdir.cleanup()


if __name__ == "__main__":
    raise SystemExit(main())
