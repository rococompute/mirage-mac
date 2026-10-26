# Plan: host entropy for unikernel CSPRNGs

**Status:** implemented 2026-10-26. Where the implementation differs from
this plan, see "As built" at the end.

Review of `ENTROPY.md`. The diagnosis is right and the proposed fix is the one to build. A guest CSPRNG cannot invent entropy the hardware timer does not have, Fortuna in `mirage-crypto-rng` is the generator to keep, and a host hypercall filled from the host CSPRNG is the seed source. The block device, the command-line seed and the wall clock stay rejected for the reasons in that note.

Several details in the proposal would ship the wrong failure mode or would kill the Linux tender. This plan is that proposal with those details fixed. The work is local to the forks in this workspace.

## What is wrong today

`Mirage_crypto_rng_mirage.initialize` builds Fortuna's seed from `Entropy.bootstrap` and one `whirlwind_bootstrap` (`rng/mirage/mirage_crypto_rng_mirage.ml`). On this machine `bootstrap` is whirlwind too: `entropy_cpu_stubs.c` only has RDRAND/RDSEED, and the M4 Pro has no `FEAT_RNG`. Whirlwind folds `cntvct_el0`. That counter ticks at 24 MHz, and a minimal guest reaches `solo5_app_main` in about 13 ms, so boot-time samples repeat across boots more than they do on x86. The per-call sequence number in the `mirage-crypto` fork only keeps the self-test from seeing two identical reads. It is not entropy.

After boot, the only top-up is `Entropy.timer_accumulator`, hooked from `Mirage_runtime.at_enter_iter`. `rdrand_task` does not start, because `Entropy.cpu_rng` is `` `Not_supported ``.

Mirage's default random device is already this Fortuna (`mirage/lib/devices/random.ml`). Fixing it fixes TLS keys, tokens and the Mirage `random` argument. OCaml's `Random` module is a separate, non-cryptographic generator. It is out of scope.

## Decisions

1. **One new hypercall, appended.** In `include/hvt_abi.h`, add `HVT_HYPERCALL_RANDOM` after `HVT_HYPERCALL_HALT` and before `HVT_HYPERCALL_MAX`. Existing numbers stay as they are. `HALT` is 8 today; `RANDOM` becomes 9. The ring kick sits at `HVT_HYPERCALL_MMIO_BASE + 0x100`, which leaves room for 16 hypercalls. This uses one of those slots.

2. **Advertise it with a feature bit, and do not bump `HVT_ABI_VERSION`.** `struct hvt_boot_info` already has `host_features`, zeroed in `tenders/hvt/hvt_boot_info.c`. Add `HVT_FEATURE_RANDOM (1U << 1)` next to `HVT_FEATURE_RING_IO`. The tender sets the bit. The guest issues the hypercall only when the bit is set. A new guest on a stock Solo5 tender sees the bit clear (stock tenders write zero) and keeps whirlwind. An old guest on this tender never issues the new number. Layout of `hvt_boot_info`, the MMIO base and the existing hypercall numbers do not change.

   This amends rule 4 in `PLAN.md`. That rule currently forbids any edit to `hvt_abi.h`. The amended rule: version stays 2, boot-info layout and MMIO addresses stay, hypercalls are only appended, and a new hypercall is off unless its `host_features` bit is set.

3. **The handler lives in the core tender, for every backend.** Register it from `setup()` in `tenders/hvt/hvt_core.c`, beside walltime and poll. HVF, KVM, FreeBSD, OpenBSD and DragonFly then share it. Fill the buffer with `arc4random_buf()` on macOS and the BSDs, and `getrandom()` on Linux (retry on `EINTR`). Do not open `/dev/urandom`.

4. **Linux seccomp gains `getrandom`.** `hvt_seccomp_linux.c` does not allow it. A call after `hvt_seccomp_apply()` would be `SIGSYS`, and the exit report would be a signal rather than a filled buffer. Add `SCMP_SYS(getrandom)` to that allow list. The macOS seatbelt stays `(deny default)`: `ENTROPY.md` already checked that `arc4random_buf()` works after `hvt_sandbox_macos_enter()`.

5. **`solo5_random` exists on every binding.** Declare it in `include/solo5.h`. The hvt binding issues the hypercall when the feature bit was seen at `start.c`, and returns `SOLO5_R_EUNSPEC` without a hypercall otherwise. The spt, virtio, xen and muen bindings return `SOLO5_R_EUNSPEC` immediately. That keeps those targets linkable. `mirage-crypto` treats `EUNSPEC` as "fall back to whirlwind". The header's general rule allows an implementation to abort instead of returning an error. This function is the exception: it must return.

6. **Bounds.** The hypercall argument is a guest pointer to a struct, same shape as block and net, not the buffer pointer itself. `hvt_do_hypercall` only carries a 32-bit pointer.

   ```c
   struct hvt_hc_random {
       HVT_GUEST_PTR(uint8_t *) data; /* IN, guest buffer */
       size_t len;                    /* IN */
       int ret;                       /* OUT, solo5_result_t */
   };
   ```

   Handler order:

   - `HVT_CHECKED_GPA_P` on the struct itself.
   - `len == 0`: `ret = SOLO5_R_OK`, return. An empty request is valid.
   - `len > 4096`: `ret = SOLO5_R_EINVAL`, return. The guest loops. Do not abort. Check this before the buffer's GPA check, so a huge length cannot overflow `gpa + len`.
   - Otherwise `HVT_CHECKED_GPA_P` on `(data, len)`. A pointer outside guest memory takes the existing path: `errx` with `Invalid guest access`, which `--exit-report` already classifies as `invalid_access`. Same check block and console use. The tender writes through the host mapping, as block read does.

   `ENTROPY.md` lists zero length and a huge length as hostile cases that should fault the guest. Zero length must succeed. A huge in-range length must come back as `EINVAL` so a guest that asks for too much can retry in chunks. The hostile case is a pointer that fails the GPA check.

7. **Seed from the Mirage backend, and keep whirlwind in the mix.** The hook is `Mirage_crypto_rng_mirage.initialize`, not `entropy.ml`'s CPU path. `entropy.ml` is also linked into Unix programs and has no Solo5 headers. A C stub in `rng/mirage/` calls `solo5_random`.

   Prepend one 32-byte host chunk to the seed that `bootstrap_functions` already builds (`bootstrap`, `bootstrap`, `whirlwind_bootstrap`, `bootstrap`). If `solo5_random` returns `EUNSPEC`, the seed is today's seed and boot still succeeds. Whirlwind bytes mixed in after real host bytes do not weaken Fortuna.

   Top up with the same shape as `rdrand_task`: an Lwt loop on the existing `sleep` argument (default one second) that `Entropy.feed_pools`s 32 bytes per pool. Fortuna has 32 pools (`rng/fortuna.ml`), so this is 32 hypercalls of 32 bytes a second, under the 4096 cap. Register the source as `solo5-host` before the first feed, so `Entropy.sources` prints it.

   Leave the cycle-counter sequence number in place. `entropy_test` still requires successive timer reads to differ.

8. **The boot log still says `no CPU RNG bootstrap`.** That line is `cpu_bootstrap_check` reporting that RDRAND is absent. It stays. The signal that host entropy is in use is the source name `solo5-host` in the line `using Fortuna, entropy sources:`.

## Files

| Change | Where |
| --- | --- |
| Hypercall number, struct, `HVT_FEATURE_RANDOM` | `solo5/include/hvt_abi.h` |
| Set the feature bit | `solo5/tenders/hvt/hvt_boot_info.c` |
| Handler and registration | `solo5/tenders/hvt/hvt_core.c` |
| `getrandom` allowed | `solo5/tenders/hvt/hvt_seccomp_linux.c` |
| Declaration | `solo5/include/solo5.h` |
| hvt binding, bit recorded from boot info | `solo5/bindings/hvt/random.c` (new), called from `bindings/hvt/start.c` next to `net_init` |
| Stub bindings | `bindings/{spt,virtio,xen,muen}/` one small function each |
| Guest test | `solo5/tests/test_random/` |
| Hostile pointer case | `solo5/tests/test_hostile/test_hostile.c`, expectation in `tests/tests.bats` |
| Sandbox regression | `solo5/tests/sandbox_macos/sandbox_macos.c` calls `arc4random_buf` after `hvt_sandbox_macos_enter` |
| Fallback switch | `solo5-hvt --no-random` clears the feature bit. Handled with the other core flags in `hvt_main.c` / `hvt_boot_info.c` |
| C stub and seed/top-up | `mirage-crypto/rng/mirage/` |
| Overlay | `opam-repository/` via `./generate.sh`, version bumps on the solo5 and mirage-crypto forks |
| Docs | `README.md` known gaps, `PLAN.md` rule 4 and the entropy bullet |

## Work sequence

Each step ends with a command that has to pass before the next step starts.

### 1. Tender hypercall

Done when `tests/test_random` prints two different non-zero 32-byte lines, a second boot prints a different first line, and `test_hostile random_bad_gpa` exits with `"reason":"invalid_access"`.

- Append the hypercall and the feature bit. Set the bit in `hvt_boot_info.c`.
- Implement the handler in `hvt_core.c` with the bounds order above.
- hvt binding: `random_init()` stores the bit; `solo5_random` returns `SOLO5_R_EUNSPEC` when it is clear and does not touch MMIO.
- Stub `solo5_random` on the other four bindings.
- `test_random` calls `solo5_random` twice into 32-byte buffers, hex-dumps them, and exits 0 only if both are non-zero and differ. `tests.bats` runs it twice and diffs the first line.
- `test_hostile` case `random_bad_gpa`: a `RANDOM` store whose buffer pointer is `0xfffffff0` and whose length is 32. Expect `invalid_access`, same as `hypercall_bad_gpa`.
- A guest-side check, called from `test_random`, that `solo5_random(buf, 0)` returns `SOLO5_R_OK` and `solo5_random(buf, 8192)` returns `SOLO5_R_EINVAL` and leaves the buffer unchanged.
- `--no-random`: `test_random` exits 0 by printing `fallback` and making no MMIO store. Easiest check is that the tender's hypercall counter stays unused; a guest that sees the bit clear returns a distinct exit message and never stores to the `RANDOM` page. Assert the exit report is `guest_exit`, not `invalid_access`.
- Add `arc4random_buf` of 16 bytes to `tests/sandbox_macos` after the sandbox is entered. Expect success. Also draw 16 bytes in the tender before `hvt_sandbox_macos_enter`, so the CSPRNG is initialised while opening files is still allowed.
- On the Linux seccomp path, add `getrandom` and run `test_random` under the sandboxed tender. A missing allow-list entry fails this, as `SIGSYS`.

`cd solo5 && ./configure.sh && make && tests/run-tests.sh`

### 2. Mirage seed

Done when a unikernel built with the fork prints `solo5-host` among its entropy sources, and the same unikernel built and run with `--no-random` boots and does not print `solo5-host`.

- C stub `mc_solo5_random` in `rng/mirage/`, filling `bytes` at an offset for a length. Return false when `solo5_random` is not `SOLO5_R_OK`.
- In `initialize`, if the stub works, register `solo5-host` and prepend 32 host bytes to the existing seed concatenation. Start the one-second `feed_pools` loop. If the stub fails, do what `initialize` does today.
- Do not change `Entropy.bootstrap` or the whirlwind self-test.
- Point the Mirage matrix crypto unikernel (the one in `mirage-crypto/mirage/unikernel.ml`, or the matrix entry that already logs sources) at this check. `run-matrix.sh` fails the crypto entry when `solo5-host` is absent from its log under a normal `solo5-hvt`, and fails it when `solo5-host` is present under `--no-random`.

`cd mirage-tests && ./run-matrix.sh`

The crypto unikernel generates an RSA key at boot. After this step that key is derived from host CSPRNG output. That is the case `ENTROPY.md` calls the worst one today.

### 3. Package and close the gap

Done when a clean opam install of the overlay picks up both commits, and the known-gap list no longer says entropy is unfixed.

- One commit on `solo5/` (`hvf-macos`), one on `mirage-crypto/`.
- Bump the fork versions, run `opam-repository/generate.sh`, and install into the `hvf` switch.
- `README.md`: remove the entropy known gap.
- `PLAN.md`: replace the entropy bullet with a pointer here, and replace rule 4 with the amended wording in decision 2.
- `solo5/docs/building.md`, macOS section: one paragraph that `solo5_random` is served from `arc4random_buf` inside the seatbelt, that `--no-random` turns it off, and that a guest built against this header still runs on a stock tender by staying on whirlwind.

## Acceptance

- With the default tender, the first Fortuna seed contains 32 bytes from `arc4random_buf` or `getrandom`, on top of the existing whirlwind material.
- While the guest runs, each Fortuna pool is fed 32 host bytes at least once per `sleep` interval (one second unless the unikernel passed another).
- A buffer outside guest memory is `invalid_access` in `--exit-report`. Length 0 is `SOLO5_R_OK`. Length above 4096 is `SOLO5_R_EINVAL` and the guest continues.
- `--no-random`, and a stock tender that leaves bit 1 clear, never sees a `RANDOM` hypercall. The guest boots on whirlwind.
- `tests/run-tests.sh`, the hostile cases, the exit-report checks and `mirage-tests/run-matrix.sh` pass.
- `README.md` does not list entropy as an open gap.

## Out of scope

- OCaml `Random.self_init`. Mirage code that needs secrets goes through `mirage-crypto-rng`. A nolibc `getentropy` can call `solo5_random` later if a real unikernel needs it.
- virtio-rng, Xen, and an spt `getrandom` seccomp hole. Those bindings return `EUNSPEC` and keep today's behaviour.
- Deleting whirlwind, or treating `no CPU RNG bootstrap` as a failure.
- Upstream Solo5. The ABI addition is confined to these forks. The feature bit is what makes a guest safe to run elsewhere.

## As built

Implemented as above, with these differences:

- **`--no-random` leaves the hypercall unregistered**, not only the feature
  bit clear. A guest that ignores the bit then stops with `invalid_access`,
  as it would on a stock tender. Hostile case `random_unoffered` checks both
  ways. The bit is derived from whether the handler is registered, so the
  two cannot disagree.
- **`solo5.h` defines `SOLO5_HAVE_RANDOM`.** The mirage-crypto stub only
  calls `solo5_random` under `__ocaml_solo5__` and that macro, so
  mirage-crypto still builds for Unix and against stock Solo5 headers.
- **The `stub` binding** (Solo5's link-check target) also gets the
  `SOLO5_R_EUNSPEC` stub. The plan listed four bindings, but there are five.
- **One hypercall per top-up, not 32.** Each second the guest fetches 32
  pools × 32 bytes in one 1024-byte request and splits it across the pools.
- **No pre-draw before the sandbox.** `tests/sandbox_macos` makes the
  process's first `arc4random_buf()` and `getentropy()` calls inside the
  sandbox, which is the tender's situation, and both work. A pre-draw would
  add nothing.
- **Extra hostile case `random_wrap`.** A buffer whose end wraps past 2^64.
- **The Mirage check uses its own unikernel.** Only mirage-crypto's
  `mirage/` test unikernel prints its entropy sources. The skeleton
  `applications/crypto` that the matrix's `crypto` case builds does not. The
  matrix gets `mirage-tests/unikernels/entropy` and two cases: `entropy`
  (`solo5-host` listed) and `entropy-fallback` (boots under `--no-random`).
- **Overlay revisions are per fork.** `generate.sh` now has a revision
  column: solo5 is `0.13.0+hvf.2`, mirage-crypto `2.4.1+hvf.3`.

Measured: over a 5 s run, the tender served 7 requests: a 32-byte boot seed
and six 1024-byte pool feeds. `--no-random` served none.

Not checked on this machine: the Linux tender (`getrandom`, the seccomp
entry) and the spt/virtio/xen/muen stubs, which only got a syntax check.


Final run (2026-10-26): `tests/run-tests.sh` 104 passed, 0 failed;
`mirage-tests/run-matrix.sh` 11 passed, 0 failed (including `entropy` and
`entropy-fallback`). The matrix also got two fixes found along the way:
build failures were silently skipped, and rebuilding an already-pulled
unikernel failed when Finder wrote a `.DS_Store` into `duniverse/`.
