# Upstream report: rounding-term sign in sys_macosx.c

Found while porting chrony's legacy macOS clock driver to ESP-IDF
(`components/chrony/port/sys_esp32.c`, which carries the fix). Not sent yet.

Files:

- `sys_macosx-rounding.patch`: one-line fix to `sys_macosx.c`. Applies
  cleanly to chrony 4.9 and to main as vendored (4.9-10-gc97d808), and the
  patched 4.9 file compiles without warnings in a macOS `./configure` build.
- `drvsim.py`: a model of the driver's bookkeeping, used for the numbers
  below. `python3 drvsim.py [hours [read_interval_s]]`.

To send: apply the patch to a checkout of chrony's git repository, commit it
with the message below, and send it with `git format-patch`/`git send-email`
to the chrony-dev mailing list, where patches are reviewed.

## Draft

Subject: [PATCH] sys_macosx: fix sign of rounding error in start_adjust()

In the legacy (adjtime-only) driver, start_adjust() ends with

    offset_register = rounding_error - old_adjust_remaining - predicted_error;

offset_register is the amount by which the clock is fast. Once adjtime()
applies adjustment_requested, the part that was not requested,
rounding_error = adjust_required - adjustment_requested, still has to be
corrected, so the clock is fast by -rounding_error. That is also the
convention of the other two terms: a cancelled remainder enters as
-old_adjust_remaining and the pre-applied drift as -predicted_error.

The rounding is to a microsecond (UTI_DoubleToTimeval()), but its sign is
the same on most calls when the same drift accrues between calls, so the
error builds up with the number of start_adjust() calls, i.e. with how
often the cooked time is read. A model of the driver (clock fast by
15.76 ppm and known exactly, drift removal every 4 s, BSD adjtime() at
500 ppm) gives this cooked-time error after one hour:

    cooked time read every    current       patched
    16 s                      +34.6 us      -0.9 us
     4 s                       -1.9 us      -0.9 us
     1 s                     -829.1 us      -0.9 us
     0.25 s                  -861.3 us      -0.9 us

(The remaining -0.9 us/h comes from the model's other approximations and
does not depend on the read rate.) In chronyd's closed loop such a drift
looks like a frequency error, which the frequency estimate absorbs, but it
changes with the wakeup rate. The legacy path only runs where
ntp_adjtime() is not available, so the practical impact is small.

I found this while porting the driver to ESP-IDF, where adjtime() rejects
corrections over about 2146 s and the remainder beyond that limit goes
through the same term, so there the sign matters much more.

    --- a/sys_macosx.c
    +++ b/sys_macosx.c
    @@ -178,7 +178,7 @@

       old_adjust_remaining = UTI_TimevalToDouble(&oldadj);

    -  offset_register = rounding_error - old_adjust_remaining - predicted_error;
    +  offset_register = -rounding_error - old_adjust_remaining - predicted_error;

       T0 = T1;
     }

## Not included

On a system that starts with a clock decades off and a nonzero frequency
(from a driftfile), the same driver computed a drift-removal interval of
about 1e10 s after the first update, because the interval follows the
estimated error, and its predicted-error term then exceeded what ESP-IDF's
adjtime() accepts. macOS's adjtime() was not tested with such values, so this
is left out of the report; the port caps the interval at 64 s.
