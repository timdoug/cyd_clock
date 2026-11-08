#!/usr/bin/env python3
"""Model of the legacy (adjtime-only) driver in chrony's sys_macosx.c.

Checks the sign of the rounding term in start_adjust():

    offset_register = rounding_error - old_adjust_remaining - predicted_error;

A clock runs fast by FREQ, which the driver knows exactly, and chronyd
reads the cooked time (raw + get_offset_correction()) every second, as its
scheduler does on each event. adjtime() follows BSD semantics: it slews the
pending delta at a fixed rate and returns the unapplied remainder, which the
next call replaces. With an exact frequency, the cooked time should stay at
true time; anything else is error introduced by the driver's bookkeeping.

The functions mirror sys_macosx.c (clock_initialise, start_adjust,
stop_adjust, get_offset_correction, drift_removal_timeout) with
UTI_DoubleToTimeval()'s microsecond rounding.

Usage: drvsim.py [hours [read_interval_s]]
"""
import math
import sys

FREQ = 15.76e-6          # clock fast by 15.76 ppm (any small value shows it)
SLEW_RATE = 500e-6       # adjtime() slew rate; BSD's traditional 500 ppm
DT = 0.01                # simulation step, seconds
DRIFT_REMOVAL_INTERVAL = 4.0
READ_INTERVAL = 1.0      # chronyd reading the cooked time (default)


class Clock:
    """System clock with BSD adjtime() semantics."""

    def __init__(self):
        self.true = 0.0
        self.raw = 0.0
        self.pending = 0.0

    def advance(self, h):
        self.true += h
        self.raw += h * (1.0 + FREQ)
        step = min(abs(self.pending), h * SLEW_RATE)
        step = math.copysign(step, self.pending)
        self.raw += step
        self.pending -= step

    def adjtime(self, delta):
        old, self.pending = self.pending, delta
        return old


def to_timeval(a):
    """UTI_DoubleToTimeval() followed by UTI_TimevalToDouble()."""
    sec = int(a)                      # C conversion truncates toward zero
    usec = round(1.0e6 * (a - sec))
    return sec + usec / 1.0e6


class Driver:
    def __init__(self, clock, rounding_sign):
        self.clock = clock
        self.sign = rounding_sign    # +1: as in sys_macosx.c, -1: patched
        self.offset_register = 0.0
        self.adjustment_requested = 0.0
        self.current_freq = FREQ
        self.interval = DRIFT_REMOVAL_INTERVAL
        self.T0 = clock.raw
        self.Tdrift = clock.raw
        clock.adjtime(0.0)

    def start_adjust(self):
        T1 = self.clock.raw
        elapsed = T1 - self.T0
        accrued_error = elapsed * self.current_freq
        dr_elapsed = min(max(0.0, T1 - self.Tdrift), self.interval)
        predicted_error = (self.interval - dr_elapsed) / 2.0 * self.current_freq
        adjust_required = -(accrued_error + self.offset_register + predicted_error)
        self.adjustment_requested = to_timeval(adjust_required)
        rounding_error = adjust_required - self.adjustment_requested
        old_adjust_remaining = self.clock.adjtime(self.adjustment_requested)
        self.offset_register = (self.sign * rounding_error - old_adjust_remaining
                                - predicted_error)
        self.T0 = T1

    def stop_adjust(self):
        adjustment_remaining = self.clock.adjtime(0.0)
        T1 = self.clock.raw
        elapsed = T1 - self.T0
        achieved = self.adjustment_requested - adjustment_remaining
        self.offset_register += (self.current_freq * (elapsed - achieved)
                                 - adjustment_remaining)
        self.adjustment_requested = 0.0
        self.T0 = T1

    def get_offset_correction(self):
        self.stop_adjust()
        corr = -self.offset_register
        self.start_adjust()
        return corr

    def drift_removal_timeout(self):
        self.stop_adjust()
        self.Tdrift = self.clock.raw
        self.start_adjust()


def run(rounding_sign, hours, read_interval):
    clock = Clock()
    drv = Driver(clock, rounding_sign)
    drv.start_adjust()
    next_drift = DRIFT_REMOVAL_INTERVAL
    next_read = read_interval
    worst = 0.0
    for _ in range(int(hours * 3600 / DT)):
        clock.advance(DT)
        if clock.true >= next_drift:
            drv.drift_removal_timeout()
            next_drift += DRIFT_REMOVAL_INTERVAL
        if clock.true >= next_read:
            cooked = clock.raw + drv.get_offset_correction()
            worst = max(worst, abs(cooked - clock.true))
            next_read += read_interval
    cooked = clock.raw + drv.get_offset_correction()
    return cooked - clock.true, worst


def main():
    hours = float(sys.argv[1]) if len(sys.argv) > 1 else 1.0
    read_interval = float(sys.argv[2]) if len(sys.argv) > 2 else READ_INTERVAL
    print(f"cooked time read every {read_interval:g} s, drift removal every"
          f" {DRIFT_REMOVAL_INTERVAL:g} s, clock fast by {FREQ * 1e6:g} ppm")
    for sign, name in ((+1, "sys_macosx.c (+rounding_error)"),
                       (-1, "patched      (-rounding_error)")):
        final, worst = run(sign, hours, read_interval)
        print(f"{name}: cooked - true after {hours:g} h = {final * 1e6:+9.3f} us,"
              f" worst {worst * 1e6:8.3f} us")


if __name__ == "__main__":
    main()
