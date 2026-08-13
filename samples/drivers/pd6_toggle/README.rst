.. _pd6_toggle_sample:

PD6 (SYSTOP) Toggle Energy Demo
###############################

Overview
********

This sample keeps the RTSS-HE core fully awake and does nothing but alternate
the **SYSTOP power domain (PD6)** between powered and gated, using Secure
Enclave (SE) run-profile requests:

* PD6 **ON** for ``PHASE_MS`` milliseconds, then
* PD6 **OFF** for ``PHASE_MS`` milliseconds, repeating forever.

The core never sleeps (it uses ``k_busy_wait()``), so the only thing changing in
the current trace is the PD6 state. With an energy analyzer on ``VDD_SOC`` the
two current levels are directly visible, and the difference between them is the
cost of keeping PD6 powered while awake.

There is intentionally **no UART/console, no logging and no GPIO** in this
build. Nothing may touch a SYSTOP peripheral, otherwise accessing it while PD6
is OFF would fault and would also add peripheral current that contaminates the
measurement.

How it works
************

At startup the app reads the board's current run profile with
``se_service_get_run_cfg()`` so the clocks, DCDC and memory settings always
match the running configuration. It then clears the ``PD_SYST_MASK`` bit to form
a base mask, and in the loop only sets or clears that one bit via
``se_service_set_run_cfg()``.

Because ``se_service_set_run_cfg()`` skips the SE call when the profile is
unchanged, alternating between the two masks guarantees a real request every
half period.

Adjusting the demo
******************

* ``PHASE_MS`` in :file:`src/main.c` sets how long PD6 stays in each state.
* ``last_err``, ``on_count`` and ``off_count`` are ``volatile`` and can be read
  with a debugger, since there is no console output.

Building and running
********************

.. code-block:: console

   west build -b alif_b1_dk/ab1c1f4m51820ph0/rtss_he alif/samples/drivers/pd6_toggle
   west flash

Then observe ``VDD_SOC`` current on an energy analyzer: it steps between the
PD6-ON and PD6-OFF levels every ``PHASE_MS``.
