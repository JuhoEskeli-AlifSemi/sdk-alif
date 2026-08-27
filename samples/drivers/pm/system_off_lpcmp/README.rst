.. _alif-pm-lpcmp-wakeup-sample:

Alif PM LPCMP Wakeup Demo
##########################

Overview
********

This sample demonstrates waking an Alif RTSS-HE core out of deep sleep with
the **low-power comparator (LPCMP)** as the primary wakeup source, keeping the
**LPRTC** as the timeout/backup wakeup source.

It is a companion to :ref:`alif-pm-states-sample`, which stays the plain
LPRTC-only reference. Nothing in that sample is modified by this one; the
extra wakeup source is added by a separate snippet,
``pm-system-off-lpcmp-he``, that layers on top of ``pm-system-off-he``.

Which deep state is demonstrated follows the boot location, exactly as in
``system_off``:

**MRAM boot** (VTOR >= 0x80000000 - the default build)
   ``PM_STATE_SOFT_OFF``. There is no retention, so a wakeup resets the
   subsystem and execution restarts from ``main()``. Whether the LPCMP or the
   LPRTC woke it is visible from how long the board stayed dark: an LPRTC
   wakeup takes the full 26 s budget, an LPCMP wakeup happens as soon as the
   comparator input crosses the reference.

**TCM boot** (VTOR = 0x0)
   ``PM_STATE_SUSPEND_TO_RAM``, first the STANDBY substate (20 s budget) then
   the STOP substate (22 s budget). Context is retained, so the sample blocks
   on a semaphore given from the LPCMP interrupt handler and names the wake
   cause outright::

      woken by LPCMP after 4213 ms
      woken by LPRTC timeout after 20009 ms

How the wakeup source is configured
***********************************

Two independent things have to line up for a deep-sleep LPCMP wake:

#. The SE **off profile** must list the comparator in ``wakeup-events`` so the
   LP comparator stays powered while the RTSS subsystem is off, and its IRQ
   must be routed through the External Wakeup Interrupt Controller via
   ``ewic-cfg``. The snippet does this on both suspend-to-RAM profiles:

   .. code-block:: devicetree

      &off_profile_standby {
              wakeup-events = <(ALIF_WE_LPCMP | ALIF_WE_LPRTC)>;
              ewic-cfg = <(ALIF_EWIC_VBAT_LP_CMP_IRQ | ALIF_EWIC_RTC_A)>;
      };

      &off_profile_stop {
              wakeup-events = <(ALIF_WE_LPCMP | ALIF_WE_LPRTC)>;
              ewic-cfg = <(ALIF_EWIC_VBAT_LP_CMP_IRQ | ALIF_EWIC_RTC_A)>;
      };

   The ``ALIF_EWIC_VBAT_LP_CMP_IRQ`` bit position differs per SoC (bit 17 on
   E1C/B1, bit 19 on Ensemble); the value comes from whichever
   ``alif_aipm_*.h`` the SoC DTSI included, so the overlay stays generic.

   The SOFT_OFF profile is *not* in devicetree. ``pm-system-off-he`` pins the
   shared ``aipm-off`` ``vtor-address`` to ``0x0``, which is the TCM
   suspend-to-RAM resume entry point and the wrong place to restart an MRAM
   boot. So the application builds that profile at runtime from the live
   ``SCB->VTOR`` and pushes it with ``se_service_set_off_cfg()`` from a
   ``state_entry`` PM notifier, using the same two wakeup masks. The DTS
   handler skips the state because ``off_profile_soft_off`` stays disabled.

#. The comparator itself must be enabled. The snippet sets ``&lpcmp`` to
   ``okay``, and the application enables the block through the Zephyr
   comparator API. For the LP instance, ``comparator_set_trigger()`` is what
   writes the terminal selection and hysteresis into ``VBAT_ANA_REG2`` and
   sets the enable bit.

The SE side needs no application code. Because the ``lpcmp`` node is enabled,
``soc_lpcmp_init()`` in ``soc/alif/balletto/common/soc_common.c`` (and its
Ensemble counterpart) runs at ``COMPARATOR_INIT_PRIORITY``, turns the analog
peripheral supply on with ``se_service_power_settings_set()`` and pushes the
same devicetree terminal selection to the Secure Enclave with
``se_service_configure_lpcmp()``. Do not duplicate that call from the
application - the SoC uses ``lpcomp_clk_sel = 1`` and a second call with
different values would silently override it.

Hardware setup
**************

The comparator is wired up in the snippet as:

* **Positive terminal**: ``CMP_POS_IN0`` → **P2_4**
* **Negative terminal**: ``CMP_NEG_IN0`` → internal AON VREF, ~0.8 V
* **Hysteresis**: 42 mV

So the only external connection needed is a signal on **P2_4** that crosses
~0.8 V. A jumper from P2_4 to 1.8 V (or a push-button, or a bench supply
ramping across the threshold) is enough to wake the part. Check the DevKit
user guide for where P2_4 lands on the expansion headers.

To use a different pin, override the terminal selection in your own overlay:

.. code-block:: devicetree

   &lpcmp {
           /* CMP_POS_IN0..IN3 = P2_4, P2_5, P2_6, P2_7 */
           positive_input = "CMP_POS_IN1";
           /* CMP_NEG_IN0..IN3 = AON VREF, P2_0, P2_1, P2_2 */
           negative_input = "CMP_NEG_IN1";
   };

Requirements
************

* Alif Balletto or Ensemble development board, RTSS-HE core
* A way to drive the LPCMP positive terminal across the reference

Supported Boards
****************

* alif_b1_dk_rtss_he
* alif_e1c_dk_rtss_he
* alif_e7_dk_rtss_he / alif_e8_dk_rtss_he (see note below on P2_4)

.. note::
   On the E7/E8 AppKit pinctrl, P2_4..P2_7 are muxed to LPCAM rather than to
   the analog switches, so the LPCMP positive terminal is not brought out
   there. The build works, but you will need a board-specific pinctrl change
   to actually feed the comparator.

Building and Running
********************

MRAM boot (SOFT_OFF):

.. code-block:: console

   west build -p always -b alif_b1_dk/ab1c1f4m51820ph0/rtss_he \
       alif/samples/drivers/pm/system_off_lpcmp \
       -S pm-system-off-lpcmp-he

TCM boot (suspend-to-RAM with retention):

.. code-block:: console

   west build -p always -b alif_b1_dk/ab1c1f4m51820ph0/rtss_he \
       alif/samples/drivers/pm/system_off_lpcmp \
       -S pm-system-off-lpcmp-he \
       -DCONFIG_FLASH_BASE_ADDRESS=0x0 \
       -DCONFIG_FLASH_LOAD_OFFSET=0x0 \
       -DCONFIG_FLASH_SIZE=256

Flash the binary using SE Tools. See :ref:`programming_an_application` for
details.

Sample Output
*************

MRAM boot - SOFT_OFF, resets on wakeup
======================================

.. code-block:: console

   *** Booting Zephyr OS build v4.1.0 ***
   [00:00:00.005,000] <inf> pm_lpcmp: alif_b1_dk RTSS_HE (MRAM boot): LPCMP deep-sleep wakeup demo
   [00:00:00.016,000] <inf> pm_lpcmp: Deep state: PM_STATE_SOFT_OFF (resets on wakeup)
   [00:00:00.026,000] <inf> pm_lpcmp: Wakeup sources: LPCMP (primary) and LPRTC (timeout/backup)
   [00:00:00.037,000] <inf> pm_lpcmp: Drive the LPCMP positive terminal (P2_4) across ~0.8 V to wake
   [00:00:00.048,000] <inf> pm_lpcmp: LPCMP armed (pos 0, neg 0, hyst 7)
   [00:00:02.057,000] <inf> pm_lpcmp: --- round 1 ---
   [00:00:02.063,000] <inf> pm_lpcmp: Entering PM_STATE_SOFT_OFF (no retention - wakeup resets the subsystem), LPRTC backup in 26000 ms

   <-- board is dark; it restarts when P2_4 crosses ~0.8 V, or after 26 s -->

   *** Booting Zephyr OS build v4.1.0 ***
   [00:00:00.005,000] <inf> pm_lpcmp: alif_b1_dk RTSS_HE (MRAM boot): LPCMP deep-sleep wakeup demo
   [Cycle repeats...]

TCM boot - suspend-to-RAM, names the wake cause
===============================================

.. code-block:: console

   *** Booting Zephyr OS build v4.1.0 ***
   [00:00:00.005,000] <inf> pm_lpcmp: alif_b1_dk RTSS_HE (TCM boot): LPCMP deep-sleep wakeup demo
   [00:00:00.016,000] <inf> pm_lpcmp: Deep state: PM_STATE_SUSPEND_TO_RAM (STANDBY, then STOP)
   [00:00:00.027,000] <inf> pm_lpcmp: Wakeup sources: LPCMP (primary) and LPRTC (timeout/backup)
   [00:00:00.038,000] <inf> pm_lpcmp: Drive the LPCMP positive terminal (P2_4) across ~0.8 V to wake
   [00:00:00.049,000] <inf> pm_lpcmp: LPCMP armed (pos 0, neg 0, hyst 7)
   [00:00:02.058,000] <inf> pm_lpcmp: --- round 1 ---
   [00:00:02.064,000] <inf> pm_lpcmp: Entering PM_STATE_SUSPEND_TO_RAM (substate 0: STANDBY) for up to 20000 ms
   [00:00:22.081,000] <inf> pm_lpcmp: === Resumed from PM_STATE_SUSPEND_TO_RAM (substate 0: STANDBY): woken by LPRTC timeout after 20009 ms ===
   [00:00:22.095,000] <inf> pm_lpcmp: Entering PM_STATE_SUSPEND_TO_RAM (substate 1: STOP) for up to 22000 ms
   [00:00:26.310,000] <inf> pm_lpcmp: === Resumed from PM_STATE_SUSPEND_TO_RAM (substate 1: STOP): woken by LPCMP after 4213 ms ===
   [00:00:26.324,000] <inf> pm_lpcmp: --- round 2 ---

Configuration options
*********************

``CONFIG_APP_LPCMP_WAKEUP_ROUNDS`` (default ``0``)
   Number of deep-sleep rounds to run before parking in an idle loop. ``0``
   runs forever, which is usually what you want on the bench. On an MRAM boot
   each round ends in a reset, so the count only matters for a TCM boot.

Notes
*****

* **Debugger**: Disconnect the debugger before testing - it prevents the core
  from entering OFF states. The sample says so explicitly if a SOFT_OFF
  attempt times out without powering down.
* **Sleep budgets**: 20 s, 22 s and 26 s are chosen against the
  ``min-residency-us`` values ``pm-system-off-he`` installs (STANDBY 19 s,
  STOP 21 s, SOFT_OFF 25 s), so the PM policy picks the intended state. If you
  change one, keep the ordering.
* **IRQ handling - important.** The LP comparator drives its NVIC line
  straight from the comparator output and has no interrupt-status register to
  acknowledge, so the line stays asserted for as long as the input sits across
  the reference. ``comparator_alif_comp.c`` unmasks that line unconditionally
  at the end of its ``POST_KERNEL`` init, and its ISR does nothing at all for
  the LP instance except invoke ``data->callback``.

  Right after an LPCMP wakeup the comparator is still enabled (VBAT is
  retained) and the input is still across the reference, so the line is
  already asserted when the driver unmasks it. With no callback registered
  there is nothing to mask it: the ISR re-enters forever and the boot
  livelocks. That also starves the SE/MHU round trips ``soc_lpcmp_init()``
  makes a few init entries later at the same priority, so the board can go
  completely silent - not even the boot banner - until the input goes away.

  This sample therefore registers the comparator callback in a
  ``PRE_KERNEL_2`` ``SYS_INIT``, before the driver can unmask anything, so the
  very first ISR masks the line and boot proceeds normally. From then on the
  sample owns the enable: ``lpcmp_arm_for_sleep()`` unmasks before a sleep and
  the callback masks again on the way out. A second ``SYS_INIT`` at
  ``COMPARATOR_INIT_PRIORITY + 1`` covers the case where the input is *not*
  across the reference at boot, where no ISR fires to do the masking.

* **Arming probes before it sleeps.** Same root cause: unmasking while the
  input is already across the reference fires the ISR at once, so sleeping
  would just wake instantly. ``lpcmp_arm_for_sleep()`` unmasks, waits 100 ms,
  and only returns once the line has stayed quiet for that window; otherwise
  it reports the input as parked and retries every 2 s until it clears. This
  is the normal state of affairs immediately after an LPCMP wakeup, when the
  stimulus is still applied.

  All of that waiting runs with ``SUSPEND_TO_RAM`` and ``SOFT_OFF`` locked -
  the comparator is not usable as a wake source while it is still being
  probed, so entering a deep state there could park the board with nothing to
  bring it back. ``SUSPEND_TO_IDLE`` is locked for the lifetime of the sample
  for the same reason, leaving ``RUNTIME_IDLE`` (a plain WFI) as the only
  state these internal waits can reach.

  The snippet also drops the comparator's interrupt priority from the SoC-wide
  default of 0 to 3. A wakeup comparator has no business outranking the SE
  mailbox.

  Both of these are workarounds for driver behaviour. The fix belongs in
  ``comparator_alif_comp.c``: it should not unmask the LP instance until a
  trigger callback has been registered.

* **comparator_get_output()** is not usable on the LP instance: it reads
  ``CMP_STATUS`` at offset 0x18, past the end of the lpcmp node's 16-byte
  register block.
* **Power measurement**: For accurate measurements, make sure all unused
  peripherals are disabled. The LP comparator and its bandgap/LDO do draw
  current in the retained VBAT domain.
