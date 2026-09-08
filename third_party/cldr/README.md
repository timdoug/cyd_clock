# CLDR Clock Locale Snapshot

This directory contains a compact, generated subset of Unicode CLDR data used
to evaluate clock/date display behavior for this firmware.

Source: `https://github.com/unicode-org/cldr-json/releases/download/48.2.0/cldr-48.2.0-json-full.zip`
Version: `48.2.0`
License: Unicode License v3 (`Unicode-3.0`), copied in `LICENSE`.

Regenerate with:

```sh
make update-cldr CLDR_VERSION=48.2.0
```

The full CLDR release zip is cached under `.cache/` and is intentionally not
committed.
