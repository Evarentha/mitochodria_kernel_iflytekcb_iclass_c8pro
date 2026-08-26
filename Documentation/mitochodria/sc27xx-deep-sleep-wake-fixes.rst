.. SPDX-License-Identifier: GPL-2.0
.. include:: <isonum.txt>

=====================================================
SC27xx RTC: fixing spurious wakes from AP deep sleep
=====================================================

Background
----------

On Unisoc SC2730-based boards the auxiliary ("boottime") alarm of the
RTC block is used by alarmtimer to deliver CLOCK_BOOTTIME_ALARM wakeups
across suspend. On iFLYTEK iClass C8Pro (UD710) tablets, entering AP
deep sleep was followed by an uncontrolled wake within a second, and
repeated suspend attempts kept being aborted. The device would also
power back on by itself within a minute after shutdown.

Root causes
-----------

Three independent problems had to be fixed together:

1. Corrupted RTC time base.

   A full power cycle clears the SC2730 time-valid flag while the
   counter restarts near zero. hctosys then fails, the system clock
   starts at 1970, and alarmtimer keeps programming alarms whose
   absolute time is already past or lands seconds away from suspend
   entry.

   Fixed by seeding the counter with a modern epoch at probe time when
   the power flag reports an invalid time
   (``CONFIG_MITOCHODRIA_RTC_FIX_BASELINE``), so both hctosys and alarm
   arithmetic stay sane until user space sets the real clock.

2. The auxiliary alarm comparator matches the seconds field alone.

   Register-level tracing against the OEM bootloader/firmware state and
   an in-driver latch self-test showed that all four alarm fields are
   written correctly, yet the comparator fires whenever the counter's
   *seconds* value equals the programmed one - day, hour and minute are
   ignored. Any armed auxiliary alarm therefore wakes the SoC within at
   most 60 seconds, whatever expiry was requested.

   Fixed by never arming the auxiliary hardware for boottime wake
   events (``CONFIG_MITOCHODRIA_RTC_NO_AUX_HW_WAKE``). In addition,
   ``alarm_irq_enable()`` no longer sets ``AUXALM_EN`` alongside the
   normal-alarm enable bit, and the resume path strips that bit from
   any restored enable mask; previously every arming re-enabled the
   broken comparator through this vendor helper.

3. Asynchronous register-write handshakes.

   Programming an alarm completes its register handshake
   asynchronously (~0.1-2 s depending on power state) and raises
   status bits even though nothing expired. With the fixes above the
   normal alarm path polls the handshake inline before enabling the
   interrupt, and ``sprd_rtc_set_aux_alarm()`` waits out and clears the
   completion status, so a mere write can no longer be reported as an
   expiry.

Configuration
-------------

=============================== =======================================
Option                          Purpose
=============================== =======================================
MITOCHODRIA_RTC_FIX_BASELINE    seed invalid counters at probe
MITOCHODRIA_RTC_SKIP_PAST_ALARM skip already-past alarm requests
MITOCHODRIA_RTC_WAKE_MIN_LEAD_S drop wake alarms due inside this horizon
MITOCHODRIA_RTC_NO_AUX_HW_WAKE  keep the aux alarm hardware disabled
MITOCHODRIA_RTC_MAIN_WAKE       deliver wake events via the polled main alarm
=============================== =======================================

The main alarm honours the full time tuple, is programmed with per-field
polling, and powers the system from power-down - which makes it suitable
for real alarms such as the user-visible alarm clock.

Behaviour and trade-offs
------------------------

* Deep sleep is stable: no sub-minute spurious wakes.
* User alarms ring on time via the main alarm.
* Wake alarms due within ``WAKE_MIN_LEAD_S`` (30 s by default) do not
  wake the device; they are delivered late, on the next natural wake.
* A runtime-armed main alarm may still power the system up from
  power-down; that is the intended alarm-clock power-on feature, and
  the driver locks the alarm at shutdown unless a genuine poweron
  alarm was requested.

Verification
------------

Validated on iFLYTEK iClass C8Pro with instrumented pre-ack traces of
the RTC-block banks: the spurious wake signature (AUXALM status raised
~0.2-0.9 s after programming with far-future alarm values) is gone, and
a deliberate main-alarm expiry now wakes the system with status
``ALARM_EN`` only. Standby current and repeated display-off cycles were
verified on hardware.
