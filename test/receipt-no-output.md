# Successful receipts without output space

Compile receipt-no-output.c as a Mach-O against the Darling SDK and libSystem,
then run in a private prefix using the rebuilt kernel library. The target test
uses the real public kevent API and pipe filters, without depending on .NET.

Before the fix, the zero-capacity registration returns -1 and the seeded EAGAIN.
Afterward, test output capacities 0, 1 and 2 for a two-change batch. Both pipe
registrations must actually deliver read events, successful receipts must have
EV_ERROR/data=0 when space exists, and invalid filters must still return EINVAL
without an output buffer. This guards against merely hiding registration errors
or returning early before the rest of a batch is processed.

Provenance: Apple's published XNU bsd/kern/kern_event.c, kevent change-processing
loop (vendored in darling-xnu). It emits a receipt only when noutputs < nevents;
without output space, only an actual EV_ERROR sets error. This is rung 1.
