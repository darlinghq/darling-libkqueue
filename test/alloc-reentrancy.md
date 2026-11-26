# Allocator re-entrancy in libkqueue

Build `alloc-reentrancy.c` and `alloc-reentrancy-hook.c` against Darling's
Darwin headers and run inside Darling:

```sh
guest-cc.sh -o alloc-reentrancy-hook.dylib alloc-reentrancy-hook.c -dynamiclib -install_name @rpath/alloc-reentrancy-hook.dylib
guest-cc.sh -o alloc-reentrancy alloc-reentrancy.c
DYLD_INSERT_LIBRARIES=$PWD/alloc-reentrancy-hook.dylib ./alloc-reentrancy
```

The hook interposes `malloc`, `calloc`, `realloc`, and `free` and calls
`dup()` + `close()` inside each allocation call after `libSystem` startup,
reproducing the `libmimalloc` pattern where allocator initialization or pool
shrinking calls `close(2)` -> `sys_close_nocancel` -> `kqueue_closed_fd()`.

It verifies:
- `kevent64` registering/deleting 300 timers and waiting does not call the
  process allocator or trigger TLV lazy allocation (`__thread` ->
  `tlv_allocate_and_initialize_for_key` -> `malloc`) while holding
  `kqueue_lock(kq)`.
- `fork()` resets private allocator freelists cleanly (`__darling_kqueue_alloc_atfork`)
  and the child's kqueue/timer operations succeed.

Baseline (`libsystem_kernel.dylib` without this fix): hangs in `kqueue_closed_fd()`
waiting on `kqueue_lock(kq)` (killed with exit 137 after timeout).
Candidate: prints `PASS: kqueue allocation avoids malloc/close reentrancy deadlock`
and exits 0.
