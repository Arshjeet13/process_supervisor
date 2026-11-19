# Process Supervisor

A small Linux process supervisor in C++. A parent process keeps exactly N worker processes alive. When a worker dies for any reason, the parent reports how it died, reaps it, and starts a replacement in the same slot. On Ctrl-C or SIGTERM it shuts the workers down cleanly instead of leaving orphans behind.

It is built in two versions:

| Version | Directory | How the parent waits |
|---|---|---|
| Step 1 | `supervisor1/` | Blocks in `waitpid` |
| Step 2 | `supervisor2/` | Sleeps in `epoll_wait`; child deaths and shutdown requests arrive through a `signalfd` |

This is the process-management core of a larger distributed project. There, the same parent will also run a network intake loop on epoll, and the workers will run inference. This repo builds and tests only the "keep workers alive" part, in isolation. The original assignment-style spec is in [SPEC.md](SPEC.md).

---

## Build and run

Requires Linux (`signalfd` and `epoll` are Linux-specific) and g++ with C++17.

```sh
cd supervisor2
make            # builds ./supervisor (rebuilds only if the .cpp changed)
./supervisor 4  # or: make run N=4
```

`num_workers` must be a whole number from 1 to 1024.

Sample session (Step 2), killing a worker from a second terminal and then sending SIGTERM to the parent:

```
worker 0 pid=919724 started
worker 1 pid=919725 started
worker 2 pid=919726 started
...
slot 0 pid=919724 died: killed by signal 9 (KILL)
worker 0 pid=919731 started
...
parent: got signal 15, shutting down
```

---

## How it works

### Step 1: blocking supervisor

1. Fork N workers and record each PID in a slot table (`std::vector<pid_t>`, index = slot).
2. Loop on `waitpid(-1, &status, 0)`. Each return is one dead child.
3. Look up its slot, decode `status`, print the death line, and fork a replacement into the same slot.

### Step 2: event-driven supervisor

```
setup:  block SIGCHLD/SIGTERM/SIGINT -> signalfd -> epoll_create1 -> epoll_ctl(ADD, signalfd) -> fork N workers
loop:   epoll_wait(-1)
          read one signalfd_siginfo
          SIGTERM / SIGINT -> shutdown: SIGTERM every worker, reap each, exit
          SIGCHLD          -> waitpid(-1, WNOHANG) until it returns 0; report + respawn each one
```

Workers print one `started` line, then a heartbeat every second.

---

## Design choices

### Workers are processes, not threads

A segfault in a thread takes down the whole process, supervisor included. A separate process with its own address space can crash without touching the parent, so the parent survives to report the crash and replace the worker.

### Step 1 depends on child state, not on signals

Blocking `waitpid` never looks at SIGCHLD. It checks the state of the children: a dead child stays a zombie, holding its exit status, until someone reaps it. So no death can be missed, even though SIGCHLD's default action is to discard the signal.

One caveat: *explicitly* setting SIGCHLD to `SIG_IGN` is different from the default. It tells the kernel to auto-reap children, and then `waitpid` fails with `ECHILD`. The code never does that.

### Why Step 2 exists: the parent has to wait on more than children

Blocking in `waitpid` makes the parent unable to notice anything except a child dying. That's fine on its own, but in the larger project the parent must also accept and read network requests. The alternatives:

- **Polling with `waitpid(WNOHANG)` in a loop.** Measured: about 15 million `waitpid` calls and a full CPU core used while a single child lived for 2 seconds. Adding a sleep trades that for detection latency.
- **`epoll_wait` + `signalfd`** (chosen). One sleep that wakes immediately for *any* registered fd, and uses no CPU while idle. Adding the listening socket later is one more `epoll_ctl`.

### signalfd instead of a signal handler

A handler runs asynchronously, possibly halfway through updating the slot table, and may only call async-signal-safe functions, which rules out `std::cout`, `std::vector` and `malloc`. With a signalfd, a signal is data that the main loop `read`s at a point of its choosing, so ordinary code is safe.

### Signals are blocked first, before the signalfd exists and before any fork

A signalfd doesn't capture signals. It only reports signals that are **pending**, and a signal is pending only while it is **blocked**. An unblocked SIGCHLD goes straight to its default action (discard), and the signalfd never becomes readable: verified with three children dying while SIGCHLD was unblocked.

So the three signals stay blocked for the parent's whole lifetime. Blocking happens before the first `fork`, so even a worker that dies immediately can't be missed.

### Reap in a loop with `WNOHANG`; don't trust `ssi_pid`

Standard signals don't queue: while one SIGCHLD is pending, more arrivals merge into it. Verified: three children died, and one signal was readable. One wake-up can therefore mean several deaths, and `ssi_pid` names only one of them.

Each wake-up drains with `waitpid(-1, &status, WNOHANG)` until it returns `0` ("children exist, none ready"). `WNOHANG` keeps the drain from blocking the event loop. `waitpid` is required anyway: reading the signalfd consumes the notification but doesn't reap anything.

### Workers undo what they inherit from `fork`

Right after `fork`, the child:

- **Restores the original signal mask.** It inherits the parent's blocked SIGTERM. Without this, the shutdown SIGTERM stays pending forever and the parent's `waitpid` on that worker hangs. Observed: the parent was stuck in `do_wait` with every worker still alive.
- **Closes the signalfd and epoll fd.** Only the parent uses them.

All of this is in `spawn_worker`, so the initial spawn and every respawn go through the same path.

### Output is flushed per line, and children exit with `_exit`

When stdout is a file or pipe, `std::cout` is fully buffered. Unflushed parent output gets copied into each child by `fork` and printed twice. Observed: a death line appeared twice when output was redirected. A process killed by a signal also never flushes, so its last lines are lost.

So:
- Every line ends with a flush.
- Workers end with `_exit()`, which doesn't flush buffers or run destructors that belong to the parent.
- The code doesn't use `std::unitbuf`. It flushes after every `<<` (5 `write` calls per line instead of 1), which lets lines from different workers interleave.

### Fatal errors shut down instead of orphaning workers

Every fatal error after the workers exist calls `shutdown()` instead of `exit()`: a failed respawn `fork`, `epoll_wait`, a short `read` from the signalfd, `waitpid`, or an unknown PID. That way a failing supervisor still SIGTERMs and reaps its workers.

`shutdown()` skips empty slots (PID 0). `kill(0, SIGTERM)` would signal the entire process group, the parent included.

### Ctrl-C vs `kill -TERM <parent>`

Ctrl-C sends SIGINT to the whole **foreground process group**, and `fork` puts workers in the parent's group. So on Ctrl-C the workers usually die from SIGINT themselves, and the parent reaps them during shutdown. `kill -TERM <parent>` exercises the path where the parent terminates each worker.

### Input validation

`std::stoi` throws on non-numbers and stops at the first non-digit (`"12abc"` → 12). The argument must be fully consumed and between 1 and `MAX_WORKERS` (1024).

---

## Testing

Run the supervisor in one terminal, and use another for commands:

| Test | Command | Expected | Result |
|---|---|---|---|
| Kill one worker | `kill -9 <pid>` | One death line, respawn in the same slot | Pass |
| Kill two at once | `kill -9 <pid1> <pid2>` | Two death lines even if one SIGCHLD was read | Pass |
| Kill all | `pkill -9 -P <parent>` | N deaths, N respawns | Pass |
| No zombies | `ps -o pid,stat --ppid <parent>` | No `Z` | Pass |
| SIGTERM to parent | `kill -TERM <parent>` | Clean exit, `pgrep` finds no workers | Pass |
| Ctrl-C | Ctrl-C, or `kill -INT -<pgid>` | Clean exit, no workers left | Pass |
| Fork failure | Lower `ulimit -u` until a fork fails | Error names the slot; started workers are reaped | Pass |
| Bad input | `./supervisor abc`, `0`, `5000` | Error message, exit 1 | Pass |
| Normal exit | Temporarily make a worker `_exit(3)` | `exited code=3`, respawned | Not yet run |
| Crash | Temporarily dereference null in a worker | `killed by signal 11 (SEGV)` | Not yet run |
| Parent killed hard | `kill -9 <parent>` | Workers orphaned; note their new PPID | Expected behavior, see below |

Handy commands: `ps -o pid,ppid,pgid,stat,wchan,cmd`, and `/proc/<pid>/status` (`SigBlk`, `SigIgn` and `SigPnd` are hex bitmasks; bit *n−1* stands for signal *n*).

---

## Known limitations

- **`kill -9` on the parent leaves orphans.** SIGKILL can't be blocked or handled, so no cleanup can run. The workers are re-parented to PID 1 or a subreaper. A fix is `prctl(PR_SET_PDEATHSIG)` in each worker.
- **Ctrl-Z then `fg` shuts the supervisor down.** After a stop/continue, Linux returns `EINTR` from `epoll_wait` even with no handlers installed, and the loop currently treats every `epoll_wait` error as fatal.
- **Shutdown does not yet print death lines or a per-slot respawn summary.**
- **No restart limit.** A worker that crashes on startup is respawned in a tight loop.
- **Signal names print without the `SIG` prefix** (`KILL`, not `SIGKILL`).

## Roadmap

- Restart limit per slot (stop respawning a slot that dies K times in T seconds)
- Per-slot uptime, and crash-on-command via SIGUSR1 for debugging
- `PR_SET_PDEATHSIG` so workers die with the parent
- Next: move the slot table into shared memory (`mmap(MAP_SHARED | MAP_ANONYMOUS)`) so workers and parent share it, the basis for requeueing a dead worker's in-flight request

## References

- `man 2 fork`, `man 2 waitpid`, `man 2 signalfd`, `man 2 sigprocmask`, `man 2 epoll_wait`, `man 7 signal`, `man 7 signal-safety`
- Kerrisk, *The Linux Programming Interface*: ch. 20–22 (signals), 24–26 (process creation and monitoring children)
