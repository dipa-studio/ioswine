# Fixtures for device_log_replay_test.c

Trimmed verbatim excerpts from device logs.
The test embeds the same lines as string literals and asserts the
failure signatures on them; these files are the checkable source
of those literals.

- touch-019-selftest.txt: lines from device log
  (aidoku-touch-selftest, first two taps).
- touch-019-interactive.txt: lines from device log
  (aidoku-interactive, IPC trace on). NUL bytes stripped.

The wq_trampoline line (slot=0 wlh=0x50694c800 flags=0x464005
nevents=1) does not appear in the saved interactive log; it is
embedded in the test as a string literal.
