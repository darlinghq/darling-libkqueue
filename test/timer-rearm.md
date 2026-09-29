# Combined registration updates

Build `timer-rearm.c` against Darling's Darwin headers and run it inside
Darling with the candidate libkqueue linked into libSystem:

```sh
cc -UNDEBUG timer-rearm.c -o timer-rearm
./timer-rearm
```

The fixture uses `kevent64`, so it is not a native Linux test program. Keep
assertions enabled. The final marker is `PASS: timer update with enable and
disable`; require both this marker and exit status zero.

It verifies:

- Rearming an existing 30-second timer for 20 milliseconds using
  `EV_ADD | EV_ENABLE` updates both its deadline and user data.
- Updating a disabled timer keeps it disabled until explicitly enabled.
- Updating enabled or disabled socket read/write registrations changes user
  data without attempting invalid epoll add/modify operations.
- Repeated disabled updates remain disabled, and `EV_ADD` without
  `EV_DISABLE` enables an existing disabled socket registration.

On upstream `7ef17bacc2eb35583b0782c3263cb12f3d14eee8`, the first timer receive
times out after one second instead of receiving the shortened timer.

Validation used fresh builds of all 22 staged libSystem/libkqueue objects,
substituting current upstream libkqueue for the baseline and this branch for
the candidate. Other libSystem sources, XNU headers, generated dependencies,
and the rest of the ARM64 guest remained staged. This is not a clean build of
the entire current Darling tree or an x86_64 runtime result.

Additional isolated guest probes passed with the candidate: a short dispatch
timer queued after a long periodic timer; CoreFoundation deadlines before
and after NSUserDefaults initialization; AppKit default/tracking run-loop
deadlines; and an empty event-queue deadline. The event-preservation test also
used the separate AppKit queue-preservation correction. Those integration
probes do not establish correctness of every kqueue filter or event flag.
