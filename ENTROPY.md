# Issue: weak RNG seeding for unikernels on Apple Silicon

**Status:** open; fix proposed and not started
**Severity:** high for any unikernel that generates long-lived secrets (TLS
keys, tokens), low for everything else
**Scope:** this workspace's forks, purely local use

## Summary

MirageOS unikernels running on this Mac seed their CSPRNG almost entirely
from timing jitter measured with a 24 MHz counter. On this CPU the guest has
no hardware random source, and Solo5 has no way for the host to pass one in.
The generator itself (Fortuna, from mirage-crypto-rng) is sound, but the seed
it starts from may be much less unpredictable than it looks. A guest that
generates a key soon after boot is the worst case.

## Why there is no library fix

The standard library for this already exists and is in use:
`mirage-crypto-rng` is the OCaml/MirageOS CSPRNG, and every networked
unikernel links it through `mirage-crypto-rng-mirage`. A CSPRNG only
stretches a seed into a stream of output, though. It cannot create
unpredictability that was never in its inputs. The problem is where the seed
comes from, and a unikernel has no operating system to ask:

- **OCaml's `Random`** is not cryptographic. Even so, its `self_init` would
  use `getentropy()` or `/dev/urandom` on a normal OS. ocaml-solo5's nolibc
  provides neither, so in a unikernel it falls back to non-random values such
  as the time.
- **mirage-crypto-rng-mirage** seeds Fortuna at boot from, in order of
  preference:
  1. the CPU's RNG instruction: RDRAND/RDSEED on x86. mirage-crypto has no
     arm64 `RNDR` support.
  2. "whirlwind", which measures timing jitter with the CPU cycle counter.

  After boot, it mixes the cycle counter into the pools on every event-loop
  iteration.

On Linux/x86 servers step 1 provides good entropy. Here it does not apply.

## Evidence (this machine)

| Fact | How it was checked |
| --- | --- |
| The CPU (Apple M4 Pro) has no RNG instruction (FEAT_RNG / `RNDR`) | `sysctl hw.optional.arm` lists no `FEAT_RNG` |
| mirage-crypto has no CPU RNG on arm64 | `entropy_cpu_stubs.c`: RDRAND/RDSEED only. The guest prints `no CPU RNG bootstrap` at every boot |
| The guest's cycle counter is `cntvct_el0` at 24 MHz | `cntfrq_el0` on the host; `solo5/docs/hvf-spikes.md` |
| At 24 MHz, back-to-back reads often return the same value | mirage-crypto's boot self-test failed with `same data from timer` (upstream issue #216) |
| The workaround in the `mirage-crypto` fork adds a per-call counter, which adds no entropy | fork commit "entropy: make successive cycle counter reads differ on ARM", adapted from upstream draft PR #283, whose author says the values "are still poor" |
| Solo5's hvt ABI has no hypercall that returns host randomness | `include/hvt_abi.h`: walltime, puts, poll, block, net and halt only |
| The host CSPRNG works inside the tender's sandbox | `arc4random_buf()` and `getentropy()` both succeed after `hvt_sandbox_macos_enter()` (deny-all profile) |

## Impact

- **Boot-time secrets are the main risk.** Fortuna's state at first use is
  derived from whirlwind and early timer samples at 24 MHz resolution. Boot
  is fast and deterministic (a minimal unikernel starts in about 13 ms), so
  those samples vary less from boot to boot than on x86. Keys, nonces or
  session secrets generated early may be guessable with less work than their
  size suggests.
- **Long-running unikernels improve over time.** They keep folding timer
  samples into Fortuna's pools, but every sample is low-resolution.
- **Some unikernels are not affected at all**: those that never use
  randomness for secrets (hello, block I/O, plain HTTP, DNS lookups apart from
  query IDs).
- **For the agent sandbox this matters.** Agent-written code that runs a TLS
  server, generates credentials, or signs anything inherits the weakness
  without any visible sign of it.

The weakness is not specific to macOS. Any aarch64 host without `RNDR` and
with a slow generic timer (some ARM servers run theirs at 25 MHz) has it.
Linux/KVM x86 hosts do not.

## Proposed fix: a host entropy hypercall

Give the guest a way to ask the tender for random bytes, the way a Linux VM
gets entropy from virtio-rng.

1. **Solo5 fork**
   - Add hypercall `HVT_HYPERCALL_RANDOM` with a guest buffer pointer and
     length, and a `solo5_random(uint8_t *buf, size_t len)` binding in
     `solo5.h`.
   - The tender checks the buffer with `HVT_CHECKED_GPA_P` like every other
     hypercall, caps the length per call (for example 256 bytes), and fills
     it with `arc4random_buf()` on macOS (`getrandom()` on Linux, for
     completeness).
   - This needs no new sandbox rules; see the evidence above.
2. **mirage-crypto fork**
   - Add a C stub that calls `solo5_random` when built for Solo5.
   - Register it as an entropy source ("solo5-host"), preferred over
     whirlwind for the boot seed via the existing `Entropy.feed_pools`. Also
     feed 32 host bytes into the pools periodically, alongside the existing
     timer samples, so a long-running guest keeps getting real entropy.
   - The cycle-counter workaround can stay. It is harmless once real entropy
     arrives first.
3. **Tests**
   - A `test_random` Solo5 test: output is non-constant and differs between
     calls and between boots.
   - Hostile cases in `test_hostile`: a buffer beyond guest memory, a huge
     length, zero length.
   - A check in the Mirage matrix that the boot log no longer depends on
     whirlwind alone.

No `config.ml` changes are needed: every unikernel gets this when rebuilt
against the forks.

### Cost

The hvt guest ABI gains a hypercall, which breaks the plan's "ABI frozen"
rule. A unikernel built with this calls a hypercall that a stock Solo5 tender
does not know, and stock tenders stop such guests with "Invalid guest
hypercall". For purely local use, where every unikernel is built and run with
the forks, that does not matter. If running elsewhere ever matters, the
tender can advertise the capability through a flag in `hvt_boot_info`. The
guest then falls back to whirlwind when the flag is absent, at the cost of
one more ABI detail.

## Alternatives considered

| Option | ABI change | Unikernel change | Problems |
| --- | --- | --- | --- |
| **Entropy hypercall** (proposed) | yes, one hypercall | none | stock tenders reject such guests |
| Read-only "entropy" block device, filled from the host CSPRNG | none | each `config.ml` must declare and attach it | easy to forget, and then silently weak again |
| Seed passed on the command line | none | mirage-crypto must parse a boot argument | visible in `ps`; one-shot only, with nothing added later |
| Use the host's wall clock (`solo5_clock_wall`) as a jitter source | none | mirage-crypto only | still timing jitter, just finer-grained; not a real fix |
| Accept the weakness and document it | none | none | unsuitable for the agent-sandbox goal |

## Acceptance criteria

- A unikernel's first Fortuna seed comes from host CSPRNG bytes, whatever
  the hardware.
- The pools get host entropy at least every few seconds while the guest runs.
- Invalid buffers are reported as `invalid_access` by `--exit-report`, like
  other bad hypercalls.
- The Solo5 suite, the hostile cases, the exit-decoder checks and
  `mirage-tests/run-matrix.sh` all still pass.
- `README.md` and `PLAN.md` no longer list entropy as a known gap.
