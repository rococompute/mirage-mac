# Plan: MirageOS unikernels as a sandbox on Apple Silicon Macs

## Goal

Build a reliable, production-grade way to build and run MirageOS unikernels on Apple Silicon Macs running macOS 26 or newer. These unikernels are the isolation layer for agent-written code. A later project will put a language and an agentic coding workflow on top of them. That layer is out of scope here, but this plan defines the platform contract it will depend on (see Platform contract).

The plan needs two personal forks:

- **Solo5 fork.** Adds a Hypervisor.framework (HVF) backend to the hvt tender. The tender talks to HVF directly, the way `hvt_kvm.c` talks to KVM. The guest ABI stays the existing aarch64 hvt ABI (`HVT_ABI_VERSION` 2), plus one appended, feature-gated hypercall for host random bytes (see `ENTROPY_PLAN.md`).
- **ocaml-solo5 fork.** Makes the OCaml cross-compiler that every Mirage `-t hvt` build uses build and install on macOS/arm64.

Getting either fork merged upstream is optional. It isn't part of the production bar, but keeping the changes upstreamable keeps rebases cheap.

This plan builds on `GROK_SUGGESTION.md`. Its claims were checked against upstream Solo5, the prototype branch, the macOS 26.6.1 SDK, `mirage/ocaml-solo5` and `mirage/mirage`. Where they differ, the plan follows what was verified.

## Status (2026-10-26)

MirageOS unikernels build and run on this Mac. `README.md` covers how to use it, and the fork histories record what changed.

| Phase | State | Notes |
| --- | --- | --- |
| 0. Spikes and rebase | Done | All four spikes passed (`solo5/docs/hvf-spikes.md`): the 4 KiB granule enforces exact W^X, a deny-all sandbox works, vmnet works without root, and density was measured (13.5 ms cold start; 32 concurrent unikernels at about 13.6 MiB each). The prototype rebased with two small conflicts. |
| 1. Host-agnostic fixes | Done | test_fpu, test_time, `.tdata` in `PT_TLS`, `@fd` errno and MTU, non-blocking kick pipe, plus two found along the way: hvt data and bss forced RW (LLD made test_wnox RWE), and an aarch64 buggyelf fixture. |
| 2. HVF backend | Done | Every Linux-aarch64 hvt test passes, plus `rnow`, `rnox`, `wnox` and `xnor` (stricter than Linux). Net tests run unprivileged over `tests/netpeer`. |
| 3. Containment | Done | Sandbox with `RLIMIT_NOFILE` clamp, `--exit-report`, `--cpu-limit`, 16 hostile-guest cases. The exit decoder is a pure function, checked in the suite (20M inputs) and fuzzed with libFuzzer, ASan and UBSan (201M runs, no findings). |
| 4. ocaml-solo5 and first unikernel | Done | ocaml-solo5 builds on macOS. Mirage hello runs. Two more forks were needed: `mirage` (solo5 0.13 bound) and `mirage-crypto` (24 MHz timer). |
| 5. Networking | Done | `solo5-vmnet` (vmnet, no root) with an egress allow-list. gvproxy was not needed. |
| 6. Mirage reliability | Matrix done, soak not run | `mirage-tests/run-matrix.sh`: 11 cases, all passing. A fifth fork was needed: `gmp` (macOS host and cross binutils). The 24-hour soak has not been run. |
| 7. Debugging | Not started | Upstream aarch64 KVM has no gdb or dumpcore either. |
| 8. CI and packaging | Packaging done, CI not started | The opam overlay is at `opam-repository/`. CI needs remote forks and a self-hosted runner. |

What changed from the plan as written, and why:

- **`RLIMIT_CPU` cannot be used.** With it set, `hv_vcpu_run()` livelocks in the kernel. `--cpu-limit` uses a watchdog thread instead.
- **`RLIMIT_FSIZE=0` was dropped.** XNU applies it to writes inside an existing file too, so it would break block devices.
- **Networking needed no third-party code.** On macOS 26, vmnet works unprivileged with the unrestricted `com.apple.security.virtualization` entitlement. `solo5-vmnet` replaces the gvproxy plan, and building gvproxy was declined in this session anyway.
- **The forks were finished, not just started.** Getting Mirage working took five forks (solo5, ocaml-solo5, mirage, mirage-crypto, gmp), fixing real macOS-host and LLVM-toolchain issues. Two of them are not Mac-specific: OCaml 5.5.2 patch offsets, and Clang placing `-Bstatic` late.
- **Guests get host entropy.** Apple silicon has no RNG instruction and a 24 MHz counter, so the fork adds `solo5_random()`, served from `arc4random_buf()`. mirage-crypto seeds and feeds Fortuna from it. This amends bar item 4 (`ENTROPY.md`, `ENTROPY_PLAN.md`).
- **Startup and density are good enough to need no pre-warmed pool.** A Mirage unikernel starts and exits in 13.5 ms. 32 run side by side at about 13.6 MiB each. A TCP/IP unikernel boots and answers 10,000 pings in under 50 ms.

Open items, in priority order:

1. **The 24-hour soak** (Phase 6).
2. **Remote forks, CI and a self-hosted runner** (Phase 8). These need the GitHub locations decided.
3. **gdb and dumpcore** (Phase 7).
4. **Report the `RLIMIT_CPU` livelock to Apple.** Upstream the host-agnostic fixes: the Solo5 Phase 1 commits, ocaml-solo5's 5.5.2 patches and `-pthread`, gmp, and mirage-crypto PR #283.

## Threat model

The guest is **untrusted**. It runs code an agent wrote, and the agent may be wrong, compromised or adversarial. The design has to hold even against a guest that is actively trying to escape.

**What must be protected:**
- the host filesystem and the credentials on it
- the host network and whatever it can reach
- other unikernels
- the supervisor process
- host CPU and memory, which the guest must not be able to exhaust

**The trust boundary has three layers:**

1. **HVF stage-2 isolation.** The guest sees only its own RAM and the hypercall MMIO page.
2. **The tender's hypercall handlers.** These are the only host code the guest can drive directly. This is the attack surface that matters most.
3. **The seatbelt sandbox around the tender process.** It limits the damage if a tender bug is exploited.

Consequences for this plan:

- **The sandbox is a hard gate.** A tender that runs agent code without a working seatbelt profile isn't production.
- **Guest W^X stays.** It doesn't protect the host. It makes memory-corruption bugs inside the guest, for example in C stubs, harder to turn into arbitrary code execution, and that holds even though most guest code is OCaml.
- **The tender never holds broad network access.** Network policy belongs to a host-side component the supervisor controls (see Network), not to vmnet inside the tender.
- **Resource limits are part of the bar.** Guest memory is capped by `--mem`, block devices by the image size, and wall-clock time, CPU time and console output by the supervisor.
- **The hypercall handlers get fuzzed** on the HVF backend, because the HVF exit decoding is new code.

## What was verified, and what changed from the suggestion

Checked on 2026-10-25 against:

- upstream `Solo5/solo5` `main` at `46b837a` (`v0.13.0`)
- the prototype branch `hvf-macos-aarch64` at `https://tangled.org/tsiry-sandratraina.com/solo5` (head `994556c`)
- the Xcode SDK for macOS 26.6.1 (25G76) on this Mac
- `mirage/ocaml-solo5` at `81c437d`
- `mirage/mirage` at `dccd7db`

| Claim or assumption | Status | Effect on the plan |
| --- | --- | --- |
| `hv_vm_config_set_ipa_granule(cfg, HV_IPA_GRANULE_4KB)` exists | **Confirmed.** Public API in `hv_vm_config.h`, `API_AVAILABLE(macos(26.0))`, along with `hv_vm_config_get_default_ipa_granule`. | macOS 26 is the minimum version. |
| The prototype exists, with the tender in `b1ca0516` | **Confirmed.** Commits: `bc5451c` (`.tdata` in `PT_TLS`), `1847515` (test_fpu and test_time fixes), `b1ca051` (HVF backend), `6ba23db`, `994556c` (opam). | Port it. Don't rewrite it. |
| Rebase conflicts are expected | **Confirmed, and they will be large.** The prototype branched at `20cee34` (#657), and upstream has 74 commits since, including the net ring work and #670 `--no-net-ring`. | Rebasing is a real Phase 0 task. |
| The prototype skips W^X on segments that aren't 16 KiB aligned, and links Mac-built unikernels at `0x4000` | **Confirmed.** See `hvt_hvf.c` and `configure.sh:471` on the branch. Upstream links aarch64 at `0x1000`. | Use the 4 KiB granule. Keep the 16 KiB link as a fallback the fork accepts (see Memory and W^X). |
| The prototype maps guest RAM RWX and also host-`mprotect`s each segment | **Confirmed.** | Drop the host `mprotect`. Enforce W^X at stage 2 only. |
| "`test_xnow` and `test_wnox` are the W^X bar" | **Partly wrong.** On Linux, `tests.bats` runs only `xnow hvt`. `wnox`, `rnow`, `rnox` and `xnor` are OpenBSD-only, because KVM can't enforce guest X/NX (`hvt_kvm.c:163`). | HVF controls X at stage 2, so target the stricter OpenBSD bar. |
| "`test_seccomp` is replaced by a macOS sandbox test" | **Wrong.** `test_seccomp` is spt-only. No hvt backend has a privilege-drop test. | The sandbox test is new and runs on the tender side. |
| The guest issues hypercalls with `str Wt, [Xn]` | **Confirmed.** `hvt_do_hypercall` and `hvt_ring_kick` both emit `str %w0, [%1]` with an `rZ` constraint, so Rt can be `wzr`. | Decode only this form. Drop the suggested `STR Xt` case. |
| "HVF has shipped builds where ISS.ISV is clear for valid stores" | **Unverified.** I found no source for it. | Treat ISV=0 as a fatal error with a full diagnostic. |
| The ring-kick pipe should be non-blocking | **Confirmed. It's a bug on every pipe backend** (`hvt_module_net.c:510` uses plain `pipe()`). | A host-agnostic Phase 1 commit. |
| The `@fd` path in `tap_attach` checks a stale `errno` | **Confirmed** (`tenders/common/tap_attach.c:72-81`). | A host-agnostic Phase 1 commit. |
| A `.tdata`/`PT_TLS` linker-script issue with LLD | **Plausible.** The prototype's fix (`bc5451c`) touches all six `bindings/*.lds` files. | Its own commit, checked with GNU ld and LLD. |
| gdb and dumpcore on aarch64 can be cloned from the KVM code | **Confirmed.** `hvt_gdb_kvm_aarch64.c`, `hvt_gdb_aarch64.h` and `hvt_dumpcore_kvm_aarch64.c` exist. | Same as the suggestion. |
| Hosted GitHub macOS runners can't run HVF | **Confirmed.** actions/runner-images#13505 was closed `NOT_PLANNED` on 2026-01-08. | A self-hosted bare-metal runner is required. |
| vmnet shared NAT works from an ad-hoc-signed, unprivileged tender | **Not established, and probably false.** The long-standing vmnet API needs root or the restricted `com.apple.vm.networking` entitlement. macOS 26 adds `vmnet_network_*`, but its privilege requirements are unknown. | This is moot for the sandbox design anyway: the tender only ever gets an `@fd` from the host (see Network). |
| `hvt_drop_privileges` runs after the devices and the I/O thread are set up | **Confirmed.** The order is `hvt_init` → `elf_load` → `hvt_vcpu_init` → `setup_modules` → `hvt_drop_privileges` → `hvt_vcpu_loop`. | Only `hv_vcpu_run` has to work inside the sandbox. |
| Mirage and `ocaml-solo5` need no changes for macOS | **Wrong.** I had claimed this in an earlier draft without checking. Both `ocaml-solo5.opam` and `ocaml-solo5-cross-aarch64.opam` limit `available` to Linux, FreeBSD, OpenBSD and DragonFly. `ocaml-solo5` also requires `ocaml >= 5.5 & < 5.6`, and this Mac's opam switch is on 5.3.0. | A separate `ocaml-solo5` fork phase. |
| The `mirage` tool blocks macOS hosts for `-t hvt` | **No host check found** in `lib/devices/target.ml`. The only macOS-specific code is the `macosx` *target* (running on Unix). A real build hasn't proven anything yet. | Check this in the `ocaml-solo5` phase with a real build. |

State of this machine: macOS 26.6.1, `kern.hv_support: 1`, `hw.pagesize: 16384`. Neither `ld.lld` nor `llvm-objcopy` is installed. opam is installed, with OCaml 5.3.0 in its `default` switch.

## Platform contract

The future language and agent layer should be able to rely on all of the following, without knowing anything about HVF:

1. **One command per sandbox:** `solo5-hvt [--mem=N] [--block:NAME=FILE] [--net:NAME=@FD] -- app.hvt [args]`. Each invocation is one process, one VM and one vCPU. Running several is safe and independent.
2. **Default deny.** The guest has no network unless it is given an `--net` fd, and it can't touch any file except the block images it was given. The tender process itself is sandboxed.
3. **Bounded resources.** Guest RAM is fixed by `--mem`. Wall-clock time and console bytes are bounded by the supervisor, which kills the tender. CPU time is capped with `RLIMIT_CPU`, set by the tender before it enters the sandbox (fork-only flag `--cpu-limit=SECONDS`).
4. **Exit reporting a machine can read.** Today, guest exit codes and tender faults (`errx(1, …)`) overlap. The fork adds an opt-in `--exit-report=FD` flag that writes one JSON line before exiting:
   - `{"kind":"guest_exit","status":N}`
   - `{"kind":"guest_abort"}`
   - `{"kind":"fault","reason":"translation_fault","pc":…}`
   - `{"kind":"tender_error","message":…}`

   This is a tender CLI addition, not an ABI change.
5. **Startup cost is measured.** The time from exec to the guest's first instruction, and the steady-state memory per sandbox, are measured and published (Phase 0 and Phase 6), so the agent layer can size its pools.
6. **A reproducible build.** `mirage configure -t hvt && make` works on the Mac against a pinned opam repository containing both forks. The build time of a small unikernel is measured (Phase 4), because the agent loop rebuilds often.
7. **Networking only through the host.** Any network access the guest gets goes through a host-side component that enforces the supervisor's policy: none, host-only, allow-listed egress, or full NAT.

The supervisor, the policy engine and the language live in the next project. This plan delivers the pieces above and one reference script, `scripts/run-sandboxed.sh`, that exercises them all.

## Production bar

macOS/arm64 counts as Production in the forks' docs when all of the following hold for 14 consecutive nightly runs on the self-hosted runner.

1. **Suite parity.** Every `hvt` test that Linux aarch64 runs in `tests/tests.bats` passes on Darwin, with no new `skip`. The `net*` tests run over an `@fd` socketpair harness instead of `tap100`.
2. **W^X stricter than on Linux.** `xnow`, `wnox`, `rnow` and `rnox` pass on Darwin and print `host/guest translation fault`. `xnor` passes too if the spike shows stage-2 execute-only works.
3. **Mac-built binaries are fully protected.** Unikernels built with the fork's toolchain run with every segment protected at stage 2, and the tender prints no "protections skipped" warning. If the 4 KiB granule spike passes, `.hvt` files built on Linux also run unmodified with full W^X. That's preferred, but not required.
4. **ABI stable.** `HVT_ABI_VERSION` stays 2. The layout of `struct hvt_boot_info`, the MMIO addresses and the existing hypercall numbers do not change. New hypercalls are only appended, and each is offered through a `host_features` bit that guests check before using it, so a guest built against the fork still runs on a stock tender. The only one so far is `HVT_HYPERCALL_RANDOM` (`HVT_FEATURE_RANDOM`, see `ENTROPY_PLAN.md`).
5. **Sandbox enforced.** `solo5-hvt` enters a seatbelt sandbox before the first `hv_vcpu_run`. The test proves that `open`, `execve`, `fork` and `socket` fail after the drop, while the fds opened beforehand keep working. `solo5-hvt-debug` alone keeps `HVT_DROP_PRIVILEGES=0`.
6. **Hostile guests contained.**
   - A fuzz campaign against the HVF exit and hypercall decoding finds no tender crash and no access outside the guest.
   - A set of hostile guests gets a clean, reported fault every time: hypercalls with garbage pointers, out-of-range MMIO, HVC and SMC, writes to the stage-2 read-only text segment, and spinning without ever calling a hypercall (stopped by the CPU limit).
7. **Mirage works.** The Mirage matrix in Phase 6 builds on the Mac with the forked toolchain and passes, including network, block and TLS workloads, and a 24-hour soak running 16 sandboxes concurrently.
8. **Network policy holds.** With no `--net`, the guest has no network. With host-only policy, the guest reaches the host-side service and nothing else. Allow-listed egress reaches only the listed destinations. A test checks each case.
9. **Packaging.** A pinned opam repository installs both forks on a clean macOS 26 arm64 user account: a signed `solo5-hvt`, the toolchain with its depexts, and `ocaml-solo5` on OCaml 5.5.
10. **CI.** Every PR compiles and signs the tender on hosted `macos-26`. The upstream Linux CI keeps running on the fork. A nightly job on bare-metal Apple Silicon runs the Solo5 suite, the hostile-guest set and the Mirage matrix, and fails if `kern.hv_support` isn't 1.
11. **The forks track upstream.** Each fork's changes are an ordered stack of commits on top of upstream `main`. A weekly job rebases the stack and runs CI. Each fork is rebased at least once per upstream release.

Until all of these hold, the backend is Experimental, behind the Darwin/arm64 host check in `configure.sh`.

## Architecture

### The Solo5 fork

HVF becomes another backend in the existing tender split, next to KVM, FreeBSD vmm, OpenBSD vmm and DragonFly nvmm. It implements the functions `hvt_main.c` already calls: `hvt_init`, `hvt_vcpu_init`, `hvt_guest_mprotect`, `hvt_drop_privileges` and `hvt_vcpu_loop`.

| File | Role |
| --- | --- |
| `tenders/hvt/hvt_hvf.c` | VM config (IPA granule), guest RAM, `hv_vm_map`, `hvt_guest_mprotect` through `hv_vm_protect`, `hvt_drop_privileges` |
| `tenders/hvt/hvt_hvf.h` | `struct hvt_b`: vCPU, `hv_vcpu_exit_t *`, kick pipe |
| `tenders/hvt/hvt_hvf_aarch64.c` | EL1 register setup, run loop, exit and ESR decoding |
| `tenders/hvt/hvt_sandbox_macos.c` + `sandbox.sb` | Seatbelt profile, compiled in |
| `tenders/hvt/hvt_gdb_hvf_aarch64.c` | gdb ops, in `hvt_gdb_aarch64.h` register order |
| `tenders/hvt/hvt_dumpcore_hvf_aarch64.c` | ELF core, with the same notes as the KVM aarch64 version |
| `tenders/hvt/solo5-hvt.entitlements` | `com.apple.security.hypervisor` |
| `tenders/common/elf_compat.h` | Minimal ELF64 definitions for hosts without `<elf.h>` |
| `tenders/hvt/hvt_main.c` (fork-only) | `--exit-report=FD`, `--cpu-limit=SECONDS` |

The guest side (`bindings/hvt/`, `bindings/hvt_cpu_aarch64.c`) doesn't change. The stage-1 page tables are already built on the host side by the shared `hvt_cpu_aarch64.c`.

The threading follows HVF's rules. `hv_vcpu_create` binds the vCPU to the thread that calls it, and only that thread may call `hv_vcpu_run`, so `hvt_vcpu_init` and `hvt_vcpu_loop` run on the main thread. The net I/O thread is created in `setup_modules`, before the sandbox is entered.

These are ruled out:
- **Virtualization.framework.** It expects a Linux-shaped guest with a DTB, a GIC and virtio, which would amount to a second ABI.
- **SPT on macOS.** It's a weaker isolation model, and it can't run `.hvt` binaries.
- **HVC, a GIC, virtio or SMP for the core path.**

### The ocaml-solo5 fork

The fork is a small stack on top of `mirage/ocaml-solo5`:

- Add `os = "macos" & arch = "arm64"` to `available` in `ocaml-solo5.opam` and `ocaml-solo5-cross-aarch64.opam`.
- Fix whatever breaks when building the OCaml 5.5 cross-compiler and `nolibc`/`openlibm` on a macOS host: BSD versus GNU `sed`/`install`, Mach-O host tools versus ELF target tools, and assumptions about the host `cc`. The build will show which of these actually bite. Fix each one so that it still works on Linux.
- It depends on the Solo5 fork's toolchain wrappers (`aarch64-solo5-none-static-{cc,ld,objcopy}`), which use `ld.lld` and `llvm-objcopy` on Darwin.

### Distribution

Use one opam repository (for example `opam repo add hvf-mac git+<repo-url>`) that carries:
- `solo5` as version `0.13.0+hvf.N`
- `ocaml-solo5` as `<upstream>+hvf.N`

The version suffixes mean these never collide with upstream releases. A lock file pins the Mirage dependency set the Phase 6 matrix tested against. Use `opam pin` only during development.

## Technical design

### Memory and W^X

Solo5 aarch64 guests use 4 KiB stage-1 pages and link at `max-page-size=0x1000`. Apple Silicon host pages are 16 KiB. With a 16 KiB IPA granule, `.text` and `.rodata` can share one stage-2 page.

**Primary design: a 4 KiB IPA granule.**

1. Call `hv_vm_config_create()`, then `hv_vm_config_set_ipa_granule(cfg, HV_IPA_GRANULE_4KB)`, then `hv_vm_create(cfg)`. Guard it with `__builtin_available(macOS 26, *)` and build with `-mmacosx-version-min=26.0`. On older macOS, exit with a message that names the minimum version.
2. Allocate guest RAM with `mmap(MAP_ANON|MAP_PRIVATE, PROT_READ|PROT_WRITE)`, sized as a multiple of 2 MiB, and `hv_vm_map` it at IPA 0 as RWX.
3. Enforce W^X at stage 2 only. Host mappings stay RW and never executable for the life of the process. Remove the prototype's host `mprotect`: it can't split 16 KiB pages, and HVF's stage-2 permissions don't depend on the host's.
4. `hvt_guest_mprotect` calls `hv_vm_protect` on the exact segment. If it fails, the tender exits. Nothing is skipped silently.
5. Remove the Darwin `max-page-size=0x4000` special case.
6. Every stage-2 permission fault prints `HVF: host/guest translation fault: pc=…, va=…, pa=…`.

**Fallback, if the granule spike fails: a 16 KiB link.** The forked toolchain links with `max-page-size=0x4000` on Darwin, as the prototype does. That gives full stage-2 W^X for every unikernel built on the Mac, with the default 16 KiB granule. `.hvt` files built on Linux at 4 KiB alignment are refused with a clear message ("rebuild with the macOS toolchain"). They don't run unprotected. This loses binary compatibility with Linux, which bar item 3 allows, but it never runs a guest without W^X.

**Granule spike (Phase 0).** A standalone HVF program, ad-hoc signed with the hypervisor entitlement, that:

- prints the default granule
- creates a VM with the 4 KiB granule and maps 64 KiB RWX at IPA 0
- protects `[0x1000, 0x2000)` as R and `[0x2000, 0x3000)` as RW
- runs a guest with the MMU off that checks:
  - (a) a store to `0x1000` is a stage-2 DABT
  - (b) a store to `0x2000` succeeds
  - (c) a branch into `0x2000` is an IABT
  - (d) whether an execute-only page can be executed but not read
- times 10⁶ stores under the 4 KiB granule against the 16 KiB one

If (a), (b) or (c) fails, switch to the fallback.

### vCPU setup and exits

EL1 setup mirrors `hvt_kvm_aarch64.c`, and the prototype already does it:

- `MAIR_EL1`
- `TCR_EL1`: 4 KiB granules, 40-bit VA
- `TTBR0_EL1` at `AARCH64_PGD_PGT_BASE`
- `SCTLR_EL1`: MMU and caches on, alignment checks off, keeping the RES1 bits from HVF's reset value
- initial PSTATE through `HV_REG_CPSR`

`cpu_cycle_freq` is the host's `cntfrq_el0`, which is 24 MHz, passed through unchanged.

| Exit | Handling |
| --- | --- |
| DABT_LOW, IPA < `AARCH64_MMIO_BASE` | Translation fault, exit 1, and report `translation_fault` |
| DABT_LOW, IPA is a hypercall address | Require ISV=1, WnR=1, SAS=word. Read `X[SRT]` (SRT 31 means zero), PC += 4, dispatch. Anything else is fatal, with ESR, PC, VA and IPA |
| DABT_LOW, IPA == `HVT_RING_KICK_MMIO_BASE` | Same decode, then a 1-byte write to the **non-blocking** kick pipe, ignoring `EAGAIN` |
| DABT_LOW, any other MMIO IPA | Fatal |
| IABT_LOW | Translation fault |
| WFI/WFE | PC += 4 |
| BRK | Handled by gdb in the debug tender, fatal otherwise |
| HVC, SMC, sysreg traps, anything else | Fatal, with EC, ESR and PC |
| `VTIMER_ACTIVATED` | Shouldn't happen, because the vtimer is masked and there's no GIC. Fatal |
| `CANCELED` | Check for exit or gdb requests, then resume |

Every guest-supplied pointer goes through the existing `HVT_CHECKED_GPA_P` bounds checks in the shared hypercall handlers. The HVF-specific attack surface is the ESR/IPA decoding above, and that's what the fuzzer targets (Phase 3).

### Privilege drop (seatbelt)

Use `sandbox_init(3)` with a custom SBPL profile. It's deprecated, but it's the only public way to confine an already-running process. App Sandbox doesn't fit: it's tied to entitlements and conflicts with the hypervisor entitlement and with passing fds in.

SBPL rules match **paths and operations, not fds**, so the design goes in steps:

1. **Spike.**
   - Trace what `hv_vcpu_run` needs under `(deny default) (debug deny)`, using `log stream --style compact --predicate 'sender == "Sandbox"'`.
   - Test whether I/O keeps working under `deny file-read* file-write*` on vnode fds opened before `sandbox_init`.
   - Test kqueue, poll and the kick pipe.
2. If pre-opened fds keep working, deny all of `file-*`, `process-exec*`, `process-fork`, `network-*` and `sysctl-write`, and allow only the Mach lookups the trace showed.
3. If they don't, generate the profile at runtime with escaped `(literal "<realpath>")` rules for exactly the block images. The profile stays compiled in.
4. If `hv_vcpu_run` needs a service that also grants broad file or process access, **stop**. The threat model rules out claiming production without a sandbox. Look at alternatives before going further, such as running the tender as a dedicated low-privilege user with an empty home directory, in addition to seatbelt.

Set `RLIMIT_CPU` (for `--cpu-limit`), `RLIMIT_NOFILE` (the current fd count) and `RLIMIT_FSIZE` (0) before `sandbox_init`.

**Test.** `tests/test_sandbox_macos/` contains a host program that links `hvt_sandbox_macos.c`, opens a block file and a socketpair, applies the profile, and asserts:
- `open`, `execve`, `fork` and `socket` fail
- the pre-opened fds still work

After that, the whole suite runs under the sandbox.

### Signing

- The link rule ad-hoc signs with `codesign -s - --entitlements tenders/hvt/solo5-hvt.entitlements -f`. `make install` and opam's post-install step sign again, because `strip` invalidates the signature.
- When `hv_vm_create` returns `HV_DENIED`, the error should say how to re-sign the binary.
- Developer ID signing and notarization are out of scope.

### Network

The tender never opens a network itself. Its only network attachment is `--net:NAME=@FD`, where the supervisor hands it one end of a `SOCK_DGRAM` socketpair carrying one Ethernet frame per datagram. The host-side end goes to a **network service** that enforces policy. That fits the threat model, it keeps the seatbelt profile free of any network access, and the tender stays unprivileged.

Network policies, from most restrictive to least:

| Policy | Mechanism |
| --- | --- |
| `none` | No `--net` argument. This is the default. |
| `host-only` | The supervisor's own userspace switch. The guest reaches one host-side endpoint, such as a package cache or an API shim. |
| `egress-allowlist` | Userspace NAT with destination filtering |
| `nat` | Userspace NAT with no filtering |

The userspace NAT is chosen in the Phase 0 network spike. Candidates, in order of preference:

1. **gvproxy (gvisor-tap-vsock).** Unprivileged. It already serves vfkit/podman on macOS over unixgram with raw frames, and it provides DHCP, DNS and port forwarding. The spike confirms that its framing matches Solo5's `@fd` (one frame per `read`), and finds out where egress filtering can hook in.
2. **A small switch written for this project,** built on the macOS 26 `vmnet_network_*` API. Only if the vmnet spike shows that API works unprivileged.
3. **A root launchd helper that owns vmnet** (in the style of `socket_vmnet`). Last resort, because it adds a privileged component.

In-tree tests use a plain socketpair peer that answers ARP and ICMP, so `net`, `net_2if`, `net_mtu`, `net_ring` and `net_ring memory` run without any network service.

### Toolchain and build

- `configure.sh` on Darwin/arm64 finds `ld.lld` and `llvm-objcopy` in `PATH`, under `/opt/homebrew/opt/{llvm,lld}/bin`, or in Nix, and accepts overrides. If they're missing, the error message names `brew install llvm lld`. On Darwin/x86_64, configure fails with a one-line message.
- `elf_compat.h` is used only when `<elf.h>` is missing.
- Mach-O has no `__start_modules`/`__stop_modules`, so use `section$start$__DATA$modules` and `section$end$__DATA$modules` behind `#ifdef __APPLE__`.
- The `.tdata` `:tbss` fix goes in all six `bindings/*.lds` files.
- `tests.bats` gets a `Darwin)` branch that skips, with a reason, if `kern.hv_support` isn't 1 or the tender doesn't have the entitlement.

## Work sequence

Each phase ends with a check an agent can re-run. Don't start a phase until the previous phase's check passes.

### Phase 0: Spikes and rebase

**Done when** the five spike results are recorded in `docs/hvf-spikes.md` and the rebased prototype builds on current upstream `main`.

1. Run `brew install llvm lld`, and create an opam switch on OCaml 5.5 (`opam switch create hvf 5.5.0`, or the latest 5.5.x).
2. **Granule spike.** Its result decides between the primary design and the 16 KiB fallback.
3. **Sandbox spike.** Its result decides between the static and the generated profile, or stops the project (see step 4 under Privilege drop).
4. **Network spike.**
   - gvproxy over unixgram to a Solo5 `@fd`: do the frame boundaries survive, and can egress be filtered?
   - The `vmnet_network_*` API as a normal user, as root, and with `com.apple.vm.networking`.
5. **Density spike.** Measure exec-to-first-instruction latency, and the RSS of an idle `test_hello` tender. Then start 64 tenders at once and record any HVF limit on VMs per system.
6. Rebase `994556c` onto upstream `main`. Expect conflicts in `hvt_core.c`, `hvt_module_net.c`, `hvt_boot_info.c`, `configure.sh` and `tests.bats`. Make the kick path match the current `kick_net_pipe` logic in `hvt_boot_info.c:103-105`.
7. Record the CI runner's hardware, who owns it, and its `kern.hv_support` value.

### Phase 1: Host-agnostic fixes (Solo5 fork)

**Done when** these sit as separate commits at the bottom of the stack, and Linux x86_64 and aarch64 CI are green. They're all good candidates for upstream.

1. The `test_fpu` Clang operand bug (from `1847515`).
2. The `test_time` bounded retry budget for a 24 MHz counter (from `1847515`).
3. `.tdata` in `PT_TLS` across `bindings/*.lds` (from `bc5451c`).
4. In the `tap_attach` `@fd` path, clear `errno` and set `*mtu`, and add a Linux socketpair test.
5. `O_NONBLOCK` on the write end of the kick pipe, ignoring `EAGAIN`.

### Phase 2: Experimental HVF backend

**Done when**
- `tests/run-tests.sh` passes on this Mac for every test in bar item 1
- the W^X tests pass
- `git diff upstream/main -- include/hvt_abi.h` is empty (Phase 2 only; the random hypercall was appended later under amended bar item 4)
- if the granule spike passed, a `test_hello.hvt` and a `test_xnow.hvt` built on Linux also pass

Work items:

- Land the HVF tender with the granule or fallback design, stage-2-only W^X, the non-blocking kick, fail-closed decoding and the translation-fault message.
- Add the `configure.sh` gate, toolchain discovery and ad-hoc signing.
- Add the Darwin branch to `tests.bats` and the socketpair net peer, and enable `xnow`, `wnox`, `rnow`, `rnox` (and `xnor` if the spike allows) on Darwin.
- Never skip these on Darwin: `hello`, `quiet`, `output`, `globals`, `exception`, `zeropage`, `xnow`, `wnox`, `rnow`, `rnox`, `notls`, `tls`, `ssp`, `fpu`, `time`, `blk` (all four variants), `net`, `net_2if`, `net_mtu`, `net_ring`, `net_ring memory`, `mft_maxdevices`, `buggyelf`.

### Phase 3: Containment

**Done when**
- `test_sandbox_macos` passes
- the full suite passes under the sandbox
- the hostile-guest set (bar item 6) reports the right fault for every case
- `--exit-report` and `--cpu-limit` work
- 24 hours of fuzzing produce no tender crash

Work items:

- Implement `hvt_sandbox_macos.c` from the Phase 0 trace, plus the rlimits.
- Add `--exit-report=FD` and `--cpu-limit=SECONDS` in `hvt_main.c`. They're fork-only commits, kept at the top of the stack.
- Add `tests/hostile/`, a set of small unikernels, each attempting one escape or abuse, each with the expected `exit-report` JSON line.
- Build a fuzzer for the exit decoding. Factor the ESR/IPA decode out into a pure function (`hvf_decode_exit(esr, ipa, regs) -> action`), and run it under libFuzzer with ASan on the host. No VM is needed. Also run a guest-side fuzzer unikernel that issues random hypercall numbers and arguments.

### Phase 4: ocaml-solo5 fork and the first Mirage unikernel

**Done when** `mirage configure -t hvt && make` on this Mac builds the `mirage-skeleton` `tutorial/hello` unikernel with the forked toolchain, `solo5-hvt` runs it under the sandbox, and its clean build time is recorded.

- Fork `mirage/ocaml-solo5`, add macOS/arm64 to `available`, and fix build failures on the macOS host one commit at a time. Re-check each fix on Linux.
- Check what `mirage` and `dune` generate for `-t hvt` on Darwin. Fix anything there in a fork of whichever package is affected (the tool, a library, or `ocaml-solo5`) and log it in `docs/hvf-spikes.md`.
- Set up the opam repository with both forks.

### Phase 5: Networking through the host

**Done when** a Mirage unikernel gets the right behaviour for each of `none`, `host-only`, `egress-allowlist` and `nat`, through the network service chosen in Phase 0, with the tender unprivileged and sandboxed.

- Integrate the chosen userspace NAT or switch, with egress filtering.
- Write `scripts/run-sandboxed.sh`. It creates the socketpair, starts or attaches to the network service, launches `solo5-hvt` with `--net:service=@N --exit-report=… --cpu-limit=…`, enforces a wall-clock timeout and a console byte limit, and prints the exit report.
- Write one test per policy: ICMP and TCP reach the destinations they should, and attempts to reach anything else fail.

### Phase 6: Mirage reliability

**Done when** the matrix below passes on every nightly, and the 24-hour soak finishes with no tender crash, no leaked process or fd, and steady memory.

The matrix, all built on the Mac and all running under `scripts/run-sandboxed.sh`:

| Workload | What it exercises |
| --- | --- |
| `mirage-skeleton` `tutorial/hello` and `noop` | Boot, console, exit codes |
| `device-usage/block` | Block reads and writes, `--block` |
| `device-usage/network` and `applications/http` | The mirage-tcpip stack, the net ring, the kick path |
| `applications/dns` and `device-usage/clock` | UDP, timers, the 24 MHz counter |
| `applications/crypto` and `applications/static_website_tls` | C stubs and aarch64 crypto code (`mirage-crypto`, `tls`) built with the forked toolchain, plus entropy |
| `device-usage/http-fetch` | Outbound TCP under the `egress-allowlist` policy |
| A deliberately crashing unikernel | Abort and fault reporting |

The soak runs 16 sandboxes in a loop for 24 hours, mixing these workloads under random policies. It records start latency, how exits are distributed, RSS, and how many host fds are open.

### Phase 7: Debugging

**Done when** a scripted gdb session (`target remote`, `break solo5_app_main`, `stepi`, `info registers pc`, `continue`) passes against `solo5-hvt-debug --gdb`, and `dumpcore hvt` produces a core that gdb reads `pc` from.

This comes after Mirage reliability because it's for developers and isn't on the sandbox's critical path. Coredumps still help debug agent workloads that crash, so do dumpcore first if time is short.

- Registers through `hv_vcpu_{get,set}_reg` and `hv_vcpu_{get,set}_sys_reg`, in the `hvt_gdb_aarch64.h` order.
- Software breakpoints: patch in `BRK #0` and call `hv_vcpu_set_trap_debug_exceptions(true)`. On EC 0x3c, restore the original instruction, single-step, and re-arm.
- Single-step with `MDSCR_EL1.SS` plus `PSTATE.SS`.
- Clone the KVM aarch64 dumpcore with the register access swapped for HVF calls.

### Phase 8: CI, packaging, promotion

**Done when** every item of the production bar holds for 14 consecutive nightly runs, and both forks' docs say **Production** for macOS/arm64.

- Hosted `macos-26` jobs on every PR to either fork: build, sign, `codesign --verify`, and build the `ocaml-solo5` cross-compiler.
- The upstream Linux CI stays enabled on the Solo5 fork.
- Self-hosted runner `solo5-macos-arm-hv`: a nightly run of the Solo5 suite, the hostile-guest set, the Mirage matrix and the soak (weekly). It fails if `kern.hv_support` isn't 1.
- A weekly job that rebases both forks onto upstream in scratch branches and runs CI. A failure opens an issue.
- Docs to write:
  - the macOS 26 floor
  - the entitlement and re-signing
  - the threat model and what the sandbox does and doesn't block
  - network policies
  - the platform contract
  - the 24 MHz counter
  - the measured start latency, RSS and build time

## Commit slices

**Solo5 fork,** bottom to top:

1. The Phase 1 fixes.
2. The HVF backend.
3. The seatbelt sandbox and its test.
4. The hostile-guest tests and the fuzz harness.
5. gdb and dumpcore.
6. CI and docs.
7. Fork-only commits: `--exit-report`, `--cpu-limit`, the opam repository metadata.

**ocaml-solo5 fork:**

1. Host-portability fixes, each one Linux-safe.
2. The `available` change.
3. Fork-only packaging.

Slices 1 to 6 of the Solo5 fork, and slice 1 of the `ocaml-solo5` fork, stay upstreamable. Keep the prototype author's authorship on commits that come from the prototype branch. Keep both upstream licenses (ISC for Solo5, the repository's own license for `ocaml-solo5`) and their copyright headers. If you send a slice upstream, follow Solo5's `AI.md`: disclose AI assistance and the tool used, have a person review every line, and keep PRs small.

## Out of scope

- The language, the agent orchestration, the supervisor's policy engine, and pooling and scheduling. This plan delivers only the platform contract they consume.
- Intel Macs, and macOS 14 and 15.
- Virtualization.framework, a GIC, virtio, SMP and nested virtualization.
- Bridged networking, Developer ID notarization and App Store distribution.
- Snapshotting or forking a running guest to cut startup time. Reconsider it only if the Phase 0 density numbers are too slow for the agent loop.

## Risks

| Risk | Signal | Response |
| --- | --- | --- |
| The 4 KiB granule doesn't enforce permissions below 16 KiB | Granule spike | Use the 16 KiB link fallback and refuse Linux-built binaries. |
| Seatbelt can't confine `hv_vcpu_run` tightly | Sandbox trace | Stop. Look at a dedicated low-privilege user on top of seatbelt before going further. |
| Pre-opened fds are blocked under the sandbox | Sandbox spike | Generate the profile at runtime with literal paths. |
| No unprivileged userspace NAT with framing that fits `@fd` | Network spike | Write a small frame-adapting shim, or fall back to a root helper. |
| `ocaml-solo5` needs deep changes to build on a macOS host | Phase 4 stalls | Build the cross-compiler in a Linux VM and ship it as a binary opam package for Darwin. That's a reasonable intermediate step, since the output is ELF either way. |
| A Mirage library with C stubs misbuilds with the forked toolchain | The Phase 6 TLS or crypto row fails | Fix it in a fork of that library, and track it in the opam repository. |
| HVF limits on concurrent VMs or startup cost are too high for the agent loop | Density spike | Pre-warm a pool of tenders. Consider snapshotting as a follow-up. |
| The forks drift from upstream | The weekly rebase job fails | Rebase promptly. Push the upstreamable slices upstream. |
| `sandbox_init` is removed in a future macOS | Release notes, the nightly on the newest macOS | Re-plan the containment layer. Pin the macOS version in the meantime. |
| The 24 MHz `cntvct_el0` surprises guests | Timing bugs | Already exposed through `cpu_cycle_freq`. Document it. |
| The data-abort syndrome changes in a future macOS | Fatal ISV/SAS message in the nightly | Decoding fails closed. Add a narrow decoder then. |
