#!/usr/bin/env python3
"""Extract POSIX TZ strings from IANA tzdata source files.

Compiles tzdata using zic, extracts the POSIX TZ string from each
compiled TZif binary, computes the UTC offset label, and prints
C array entries matching the format in main/ui_timezone.c.
"""

import os
import re
import subprocess
import sys
import tempfile

# (display_city, iana_zone_id) - order matches main/ui_timezone.c
ZONES = [
    ("Samoa", "Pacific/Pago_Pago"),
    ("Honolulu", "Pacific/Honolulu"),
    ("Anchorage", "America/Anchorage"),
    ("Los Angeles", "America/Los_Angeles"),
    ("Phoenix", "America/Phoenix"),
    ("Denver", "America/Denver"),
    ("Mexico City", "America/Mexico_City"),
    ("Chicago", "America/Chicago"),
    ("New York", "America/New_York"),
    ("Panama", "America/Panama"),
    ("Bogota", "America/Bogota"),
    ("Lima", "America/Lima"),
    ("Halifax", "America/Halifax"),
    ("Santiago", "America/Santiago"),
    ("St. John's", "America/St_Johns"),
    ("Sao Paulo", "America/Sao_Paulo"),
    ("Buenos Aires", "America/Argentina/Buenos_Aires"),
    ("UTC", "Etc/UTC"),
    ("Reykjavik", "Atlantic/Reykjavik"),
    ("London", "Europe/London"),
    ("Lisbon", "Europe/Lisbon"),
    ("Dublin", "Europe/Dublin"),
    ("Casablanca", "Africa/Casablanca"),
    ("Lagos", "Africa/Lagos"),
    ("Paris", "Europe/Paris"),
    ("Berlin", "Europe/Berlin"),
    ("Rome", "Europe/Rome"),
    ("Johannesburg", "Africa/Johannesburg"),
    ("Cairo", "Africa/Cairo"),
    ("Athens", "Europe/Athens"),
    ("Jerusalem", "Asia/Jerusalem"),
    ("Helsinki", "Europe/Helsinki"),
    ("Istanbul", "Europe/Istanbul"),
    ("Moscow", "Europe/Moscow"),
    ("Nairobi", "Africa/Nairobi"),
    ("Riyadh", "Asia/Riyadh"),
    ("Tehran", "Asia/Tehran"),
    ("Dubai", "Asia/Dubai"),
    ("Karachi", "Asia/Karachi"),
    ("Mumbai", "Asia/Kolkata"),
    ("Kolkata", "Asia/Kolkata"),
    ("Kathmandu", "Asia/Kathmandu"),
    ("Dhaka", "Asia/Dhaka"),
    ("Bangkok", "Asia/Bangkok"),
    ("Ho Chi Minh", "Asia/Ho_Chi_Minh"),
    ("Jakarta", "Asia/Jakarta"),
    ("Singapore", "Asia/Singapore"),
    ("Kuala Lumpur", "Asia/Kuala_Lumpur"),
    ("Hong Kong", "Asia/Hong_Kong"),
    ("Shanghai", "Asia/Shanghai"),
    ("Taipei", "Asia/Taipei"),
    ("Manila", "Asia/Manila"),
    ("Perth", "Australia/Perth"),
    ("Seoul", "Asia/Seoul"),
    ("Tokyo", "Asia/Tokyo"),
    ("Adelaide", "Australia/Adelaide"),
    ("Sydney", "Australia/Sydney"),
    ("Melbourne", "Australia/Melbourne"),
    ("Auckland", "Pacific/Auckland"),
    ("Fiji", "Pacific/Fiji"),
]

# tzdata source region files to compile
REGION_FILES = [
    "northamerica", "southamerica", "europe", "asia",
    "africa", "australasia", "etcetera", "backward",
]

ZIC = "/usr/sbin/zic"


def compile_tzdata(srcdir, destdir):
    """Compile tzdata source files into TZif binaries using zic."""
    for region in REGION_FILES:
        path = os.path.join(srcdir, region)
        if not os.path.exists(path):
            print(f"Warning: {path} not found, skipping", file=sys.stderr)
            continue
        subprocess.run(
            [ZIC, "-d", destdir, path],
            check=True,
            capture_output=True,
        )


def extract_posix_string(tzif_path):
    """Extract the POSIX TZ string from a TZif v2/v3 binary file.

    In TZif v2/v3, the file ends with \\n<posix_string>\\n.
    """
    with open(tzif_path, "rb") as f:
        data = f.read()
    last_nl = data.rindex(b"\n")
    second_last_nl = data.rindex(b"\n", 0, last_nl)
    return data[second_last_nl + 1 : last_nl].decode("ascii")


def parse_utc_offset(posix_tz):
    """Parse a POSIX TZ string and return the UTC offset as a display string.

    POSIX offsets have inverted sign from UTC convention:
      EST5       -> UTC-5
      CET-1      -> UTC+1
      IST-5:30   -> UTC+5:30
      <-04>4     -> UTC-4
    """
    s = posix_tz
    # Skip the standard time abbreviation
    if s.startswith("<"):
        s = s[s.index(">") + 1 :]
    else:
        i = 0
        while i < len(s) and s[i].isalpha():
            i += 1
        s = s[i:]

    # Parse sign (POSIX: positive = west of UTC)
    negate = False
    if s and s[0] == "-":
        negate = True
        s = s[1:]
    elif s and s[0] == "+":
        s = s[1:]

    # Parse hours[:minutes]
    match = re.match(r"(\d+)(?::(\d+))?", s)
    if not match:
        return "UTC+0"

    hours = int(match.group(1))
    minutes = int(match.group(2)) if match.group(2) else 0

    # Invert sign for display
    if negate:
        sign = "+"
    elif hours == 0 and minutes == 0:
        sign = "+"
    else:
        sign = "-"

    if minutes:
        return f"UTC{sign}{hours}:{minutes:02d}"
    return f"UTC{sign}{hours}"


def main():
    script_dir = os.path.dirname(os.path.abspath(__file__))

    with tempfile.TemporaryDirectory() as tmpdir:
        print("Compiling tzdata...", file=sys.stderr)
        compile_tzdata(script_dir, tmpdir)

        for city, iana_id in ZONES:
            tzif_path = os.path.join(tmpdir, iana_id)
            if not os.path.exists(tzif_path):
                print(f"ERROR: {iana_id} not found", file=sys.stderr)
                continue

            posix_tz = extract_posix_string(tzif_path)
            offset_label = parse_utc_offset(posix_tz)
            name_field = f'"{city} ({offset_label})"'
            tz_field = f'"{posix_tz}"'
            print(f"    {{{name_field + ',':<40s}{tz_field}}},")


if __name__ == "__main__":
    main()
