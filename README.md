# Toy A: Fork/Respawn Supervisor

A parent process that keeps exactly N worker processes alive. If any worker dies, for any reason, the parent notices, reports how it died, cleans it up, and starts a replacement. The parent never blocks waiting on any one child.

This is the skeleton of every device in the cluster. In V0 the parent will also run the epoll intake loop, and the workers will run inference. This toy builds only the "keep workers alive" part, so you can test it in isolation.

---

## 1. Why this matters for the cluster

| Cluster need | What this toy teaches |
|---|---|
| Workers are processes, not threads, so a crash is isolated | `fork`, separate address spaces |
| The parent detects a dead worker and respawns it | `waitpid`, exit status decoding |
| Detection must not block the network intake loop | `signalfd` inside an epoll loop |
| Requeue the dead worker's request | A slot table mapping PID to worker slot (the ancestor of the in-progress marker) |
| A debug trigger that crashes a worker on command | Stretch goal S3 |

---

## 2. Concepts you need (and the terms used below)

**Process lifecycle.** `fork()` creates a child that is a copy of the parent. It returns twice: 0 in the child, and the child's PID in the parent. The child runs until it calls `exit`, returns from `main`, or is killed by a signal.

**Zombie.** When a child exits, the kernel frees its memory but keeps a small record (PID and exit status) so the parent can find out what happened. Until the parent collects that record, the child is a zombie and shows as `Z` / `<defunct>` in `ps`. Zombies use a PID slot each, and enough of them will stop the system from creating new processes.

**Reaping.** Collecting a zombie's exit status with `wait`/`waitpid`. After that, the record is gone.

**Orphan.** A child whose parent died first. It gets reparented to PID 1 (or to a "subreaper" process), which reaps it eventually. Orphans are not zombies.

**Signal.** An asynchronous notification the kernel delivers to a process, such as SIGCHLD ("one of your children changed state"), SIGKILL (from `kill -9`), or SIGSEGV (bad memory access). Each signal has a *disposition*: default action, ignore, or a handler function.

**Signal mask.** The set of signals a process has *blocked*. A blocked signal is not lost. It stays *pending* until it is unblocked or read some other way. You change the mask with `sigprocmask`. A forked child inherits the parent's mask.

**Standard signals don't queue.** If SIGCHLD is already pending and another child dies, you still have one pending SIGCHLD, not two. Keep this in mind when you design your reaping loop.

**signalfd.** A Linux-specific syscall that creates a file descriptor you `read()` signals from, instead of having them delivered to a handler. Since it is an fd, you can add it to epoll, and "a child died" becomes an ordinary event in your loop. This avoids signal handlers, which can interrupt your code at any instruction and may only call a short list of async-signal-safe functions.

**`waitpid(pid, &status, options)`.**
- `pid = -1` means any child.
- `WNOHANG` means return 0 right away if no child has exited, instead of blocking.
- The return value is the reaped PID, 0 (nothing to reap yet, with WNOHANG), or -1 with `errno == ECHILD` (no children at all).
- Decode `status` with the macros `WIFEXITED`/`WEXITSTATUS` (normal exit and its code) and `WIFSIGNALED`/`WTERMSIG` (killed by which signal). `WCOREDUMP` is a common extension.

**epoll recap.** You already used this in echo3. Here the only fd in the interest list is the signalfd, at least for now.

---

## 3. Build it in two steps

### Step 1: blocking supervisor (no epoll)

Spec:
1. The parent forks `N = 4` workers.
2. Each worker prints `worker <slot> pid=<pid> started`, then loops forever: sleep 1 second, print a heartbeat line.
3. The parent loops on a blocking `waitpid(-1, &status, 0)`. For each reaped child, it prints:
   `slot <k> pid=<pid> died: exited code=<c>` or `... died: killed by signal <s> (<name>)`.
   Then it forks a replacement into the **same slot**.
4. The parent keeps a slot table: an array of N entries, each holding the PID currently in that slot. When a PID is reaped, look up its slot. The replacement goes there.

Done when: `kill -9` on any worker produces a correct death message and a new worker in the same slot, and `ps` shows no zombies.

### Step 2: event-driven supervisor (signalfd + epoll)

**What changes, in one sentence.** From the outside the program looks almost like Step 1: it keeps N workers alive and respawns any that die, with the same output lines. Inside, the parent no longer sleeps in `waitpid`. It sleeps in `epoll_wait`, and "a child died" arrives as an ordinary fd event. One new behavior is visible: Ctrl-C (or SIGTERM) triggers a clean shutdown with a summary, instead of everyone dying at once.

**Why bother, if Step 1 works?** In Step 1 the parent spends its whole life blocked in `waitpid`. In V0 the same parent also runs the network intake loop with epoll, and a process can't sleep in `waitpid` and `epoll_wait` at the same time. The fix is to make child deaths something epoll can report. Then a single loop handles both, and adding the listening socket later is just one more `epoll_ctl`.

Spec:
1. Same workers and the same slot table. Add a **respawn counter per slot** next to the PID, for the shutdown summary.
2. **Setup, in this order:**
   - Block SIGCHLD, SIGINT and SIGTERM with `sigprocmask`. Blocked signals are not lost: they stay *pending* until something reads them. The order matters (section 6, Q1).
   - Create a signalfd for those three signals. Pending signals can now be `read` from this fd like data from a pipe.
   - Create an epoll instance and register the signalfd for `EPOLLIN`.
   - Only then fork the initial N workers.
3. **Main loop:** `epoll_wait` with no timeout (`-1`). When the signalfd is readable:
   - `read` one `struct signalfd_siginfo` to consume the notification. Check the return value: it should be exactly `sizeof(struct signalfd_siginfo)`. `ssi_signo` tells you which signal it was.
   - **SIGCHLD:** reap **every** child that is ready, not just one (see the "don't queue" note above). Loop on `waitpid(-1, &status, WNOHANG)`:
     - `> 0`: look up the slot, print the death line (same format as Step 1), respawn into the same slot, bump that slot's counter, and loop again.
     - `0`: every remaining child is still alive. Stop and go back to `epoll_wait`.
     - `-1`: `ECHILD` means no children at all. Anything else is a real error: print `strerror(errno)`.
     - Never pass `0` (blocking) here. The parent must never sleep anywhere except `epoll_wait`.
     - Don't rely on `ssi_pid`. It names one child, not all of the ones that died.
   - **SIGINT or SIGTERM:** begin graceful shutdown (item 5).
4. **Worker setup after fork.** The child inherits the parent's signal mask and every open fd. Before it starts its heartbeat loop, each worker must:
   - **Unblock** SIGTERM (simplest: restore an empty mask). Otherwise the shutdown SIGTERM stays pending forever, the worker never dies, and shutdown hangs.
   - **Close** the epoll fd and the signalfd. The worker has no use for them (section 6, Q3 asks what could go wrong if it keeps them).

   Put this in your `spawn_worker` function, so the initial spawn and every respawn share it.
5. **Graceful shutdown**, on SIGINT or SIGTERM:
   1. Stop respawning. From now on, a dying worker is expected.
   2. Send SIGTERM to every PID in the slot table.
   3. Reap all of them. A blocking `waitpid(-1, &status, 0)` is fine here, since shutdown is the only work left. Stop when it returns `-1` with `ECHILD`. Print each death line as usual.
   4. Print a summary of how many times each slot was respawned during the run.
   5. Close the epoll fd and signalfd, and exit 0.

Example of what Ctrl-C might look like (exact format is up to you):

```
^C
parent: got SIGINT, shutting down
slot 0 pid=4512 died: killed by signal 15 (SIGTERM)
slot 1 pid=4498 died: killed by signal 15 (SIGTERM)
slot 2 pid=4520 died: killed by signal 15 (SIGTERM)
slot 3 pid=4499 died: killed by signal 15 (SIGTERM)
summary: slot 0 respawned 2x, slot 1 0x, slot 2 1x, slot 3 0x
```

Note that Ctrl-C sends SIGINT to the whole foreground process group, so the workers get it too. Since they have unblocked their mask, its default action kills them before the parent's SIGTERM arrives. Their death lines will say SIGINT (2) instead of SIGTERM. That's fine. The `kill -TERM <parent_pid>` path is the one that exercises the parent sending SIGTERM to every worker.

**Shape of the event loop** (structure only, not code):

```
setup: block signals -> signalfd -> epoll_create1 -> epoll_ctl(ADD, signalfd) -> spawn N
loop:
    epoll_wait(..., -1)
    for each ready fd (only the signalfd, for now):
        read one signalfd_siginfo
        SIGCHLD         -> drain with waitpid(WNOHANG) until 0; report + respawn each
        SIGINT/SIGTERM  -> shutdown(); exit 0
```

**What carries over from Step 1 unchanged:** the worker body, `spawn_worker` (plus the post-fork cleanup above), the slot lookup, and death reporting (`WIFEXITED`/`WIFSIGNALED` decoding). Only the main loop and the setup are new.

Done when: every test in section 4 passes, and Ctrl-C leaves no workers running (`pgrep -f <your binary>` is empty).

Why the shutdown step matters: in V0 this is how you stop a device cleanly between benchmark runs, instead of leaving orphaned workers that skew the next run.

---

## 4. Test plan

Open two terminals. Run the supervisor in one. In the other:

| # | Test | Command idea | Expected |
|---|---|---|---|
| T1 | Kill one worker | `kill -9 <pid>` | One death line with signal 9 (SIGKILL), one respawn in the same slot |
| T2 | Kill two at once | `kill -9 <pid1> <pid2>` | **Two** death lines and two respawns, even if only one SIGCHLD was read |
| T3 | Kill all workers | `pkill -9 -P <parent_pid>` | N deaths, N respawns |
| T4 | Normal exit | Make worker 0 call `exit(3)` after 5 seconds | `exited code=3`, respawned |
| T5 | Crash | Make worker 1 dereference a null pointer after 7 seconds | `killed by signal 11 (SIGSEGV)` |
| T6 | No zombies | `ps -o pid,ppid,stat,cmd --ppid <parent_pid>` after T1-T5 | No `Z` in the STAT column |
| T7 | Graceful shutdown | Ctrl-C the parent | Summary printed, `pgrep` finds no workers |
| T8 | Parent killed hard | `kill -9 <parent_pid>` | Workers are orphaned and keep running. Note what their PPID becomes. (This is not a bug to fix here. See S4.) |

T2 is the important one. If it fails only sometimes, the bug is in your reaping loop.

---

## 5. Pitfalls (read these before debugging)

- **Duplicated output after fork.** `std::cout` (and C's `stdout`) is buffered in user space. If the parent has unflushed output when it forks, the child gets a copy of that buffer and prints it too. This is most visible when you redirect output to a file. Think about when to flush.
- **How a C++ worker should exit.** `exit()` (or returning from `main`) in a forked child runs static destructors and flushes inherited buffers that belong to the parent. Look up `_exit()` and decide which one a worker should use.
- **Explicitly ignoring SIGCHLD changes `wait`.** Setting SIGCHLD's disposition to `SIG_IGN` makes the kernel auto-reap children, and `waitpid` then fails with `ECHILD`. Blocking a signal and ignoring it are different things.
- **The child inherits more than memory.** It also inherits the signal mask and every open fd, including the epoll fd and the signalfd. Decide what each worker should do with these.
- **`ssi_pid` is not a full list.** The `signalfd_siginfo` you read names one child. It doesn't tell you how many died. Let `waitpid` tell you.
- **Check every return value.** `fork` can fail (`EAGAIN` when you hit the process limit). `read` on the signalfd can return a short count. Print `strerror(errno)` on failure. Silent failures were the main problem in echo2.

---

## 6. Questions to answer in NOTES.md before step 2

1. Why must SIGCHLD be blocked *before* the signalfd is created? What happens to a SIGCHLD that arrives while it is unblocked with its default disposition?
2. If two children die at nearly the same moment, how many SIGCHLD notifications can you receive? What does that mean for how many times you call `waitpid` per epoll event?
3. After `fork`, the worker holds copies of the epoll fd and signalfd. Should it close them? What could go wrong if it doesn't?
4. (CS537 tie-in) Right after `fork`, parent and child have identical memory contents. Are those physically the same pages? What happens the first time the child writes to a variable? (This sets up Toy B, where you deliberately make memory *shared*.)

---

## 7. Stretch goals (optional, each is small)

- **S1. Restart limit.** A worker that crashes right on startup causes an infinite respawn loop that burns a CPU core. If a slot dies more than K times in T seconds, stop respawning it and log the slot as failed. (Real supervisors such as systemd do this.)
- **S2. Uptime per slot.** Record the start time of each worker and print its lifetime when it dies.
- **S3. Crash on command.** The parent sends SIGUSR1 to a chosen slot, and the worker reacts by crashing. Pick the slot from stdin, or from a second fd in the epoll loop. This is the cluster's debug trigger in miniature.
- **S4. Die with the parent.** Look up `prctl(PR_SET_PDEATHSIG)` (Linux-only) and make T8 kill the workers too. Write down why a race exists if the parent dies before the child calls it, and how to close that race.

---

## 8. Suggested layout

```
toyA/
  step1_blocking/
    supervisor.cpp
  step2_signalfd/
    supervisor.cpp
  Makefile          # -Wall -Wextra -g ; one target per step
  NOTES.md          # answers to section 6, test results, surprises
```

In NOTES.md, record for each test: what you expected, what happened, and why, if they differed. These notes become your writeup material.

---

## 9. References (look up as needed)

- `man 2 fork`, `man 2 waitpid`, `man 2 signalfd`, `man 2 sigprocmask`, `man 7 signal`
- `man 7 signal-safety` (why handlers are dangerous, and therefore why signalfd)
- TLPI (Kerrisk): ch 20-22 signals (22.11 signalfd), ch 24-26 process creation, termination, and monitoring children
- OSTEP ch 5 (Process API), which you've already covered

---

## 10. After this toy

Toy B: `mmap(MAP_SHARED | MAP_ANONYMOUS)` before fork, a shared counter race between workers, a fix with a process-shared robust mutex, then `std::atomic`. After that, the slot table from this toy moves into shared memory and becomes the in-progress marker (worker PID plus request ID) that V0 uses to requeue a dead worker's request.
