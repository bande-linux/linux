# unbound-linux kernel

Silicon support that is not merged yet. Kernel process and style: [Documentation/process/](Documentation/process/),
the source of truth. Open work goes to [NOTES/backlog.md](NOTES/backlog.md).

The rules below override your defaults. Follow them unless I say otherwise in the session.

## 0. Proper implementations only

No shortcuts and no band-aids. Every change is the long-term implementation:

- Fix the cause, not the symptom. Never widen a check, raise a limit, skip a step or special-case
  a value to make something work without knowing why that is correct.
- Support a new firmware or hardware version by understanding it: get its real layout and
  behaviour (a trace, the ADT, the macOS driver, vendor headers), implement it explicitly, and
  keep refusing versions that are not understood.
- Port to the current upstream code and APIs instead of carrying old versions or compat shims.
- Write it so it could go to mainline as it is.

When the proper implementation is not possible yet, say so and leave the gap visible (a refused
version, a missing feature, a backlog item) instead of covering it up.

## 1. No comments

Write none, in C, Rust, device-tree sources, Kconfig or Makefiles. The only exceptions are facts
this repository cannot tell the reader:

- behaviour of hardware or firmware we do not own (Apple SoC blocks, RTKit coprocessors, iBoot,
  firmware interfaces), with how it was established: a trace, the ADT, a macOS driver
- a magic value copied from outside, with its source
- kernel-doc on an exported symbol, where the subsystem's maintainers require it

One line. Never write what the next line does, a note to the reviewer ("now", "replaces",
"fixed"), a defence of a design, a banner, or a doc comment that restates a name or signature.

Upstream comments stay as they are. Removing them is its own commit, never part of a change.

## 2. No noise in the kernel log

Every line in dmesg is read, by people and by agents parsing boot logs. A driver logs state and
errors, nothing that explains them:

- an error in one short line, with the value that failed
- no `dev_info` on a successful probe, no progress chatter
- keep a warning only for hardware risk or data loss

## 3. No tests without a motivation

Do not add a test because you changed code. A new test needs one of these motivations, stated in
its name or first line:

- it reproduces a bug that actually happened and would come back unseen
- it pins behaviour of hardware or firmware we do not own; the expected value comes from a
  hardware trace, the ADT or macOS, never from reading our code
- it guards a safety property: a power, voltage or thermal limit is enforced, an unsupported
  firmware version is refused, a userspace-reachable path cannot corrupt memory

Even then, propose it in one line and wait for a yes. Prefer a test that boots the kernel on the
Mac and checks behaviour from userspace over a KUnit test of a helper.

Never write tests of constants, getters or formatting, a return value restated, mocks asserting
they were called, one test per branch, or edge-case sweeps.

When your change breaks an existing test: fix it if it guards one of the motivations above;
delete it if it only restated the old code, and list every deletion in your reply.

## 4. Before you finish

Re-read your diff. Delete every comment that fails §1, every log line that fails §2 and every test
you added that fails §3. Expect that to be most of them. Run `scripts/checkpatch.pl` on it.

## 5. Scope and consent

- Do what I asked, then stop. Name adjacent problems in one line at the end of your reply; don't
  fix them.
- No unrequested docs, renames or formatting passes. No CI or packaging work unless asked.
- Keep the fork rebasable on upstream: change upstream files only as far as the task needs, and
  stay out of code that is not Apple Silicon support.
- Reuse before you add: search the existing Apple drivers and the kernel's helpers first.
  Duplicate logic is a bug; factor it out the second time, inside your scope. No speculative
  optimisation.
- Ask first, in two or three sentences with the trade-off, before you:
  - touch more than a handful of files, or any file outside Apple Silicon support
  - move, rename or split a driver, a Kconfig symbol or a module
  - change a userspace interface: anything under `include/uapi/`, a sysfs or debugfs path, the
    GPU UAPI that Mesa uses
  - change a device-tree binding or compatible string, or what is read from `/chosen`
  - change which firmware versions a driver supports
  - add a dependency (a Rust crate, a firmware file the driver loads)
  - drive power management, PMU, SMC, voltage, fan or thermal hardware in a new way
  - act on the real Mac: boot a kernel on it, install anything, change its boot policy
- Never add a `Signed-off-by` line, and never send a patch or mail upstream. Those certify a
  person's authorship and need a person.
- I commit. Never run git commit, push or stash, and never sync with upstream.

## 6. Architecture

- The bootloader loads this kernel, builds its device tree from Apple's ADT and fills `/chosen`.
  The M2 Mac mini is `arch/arm64/boot/dts/apple/t8112-j473.dts`.
- Coprocessor drivers (GPU, display, SMC and others) talk to Apple firmware whose interfaces change
  between versions. A driver supports a fixed set of firmware versions and refuses others.
- The GPU driver (`drivers/gpu/drm/asahi`, Rust) and Mesa's userspace driver share
  `include/uapi/drm/asahi_drm.h`; a change on one side breaks the desktop on the other.
- A kernel is tested by booting it on the Mac through the bootloader's USB proxy, never by
  installing it.
- Anything submitted to mainline follows `Documentation/process/generated-content.rst`: tool use
  is disclosed and a person can defend every line.
