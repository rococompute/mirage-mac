# Making Apple Silicon a production Solo5 hvt target

Decision: the production host is an Apple Silicon Mac running macOS 26 or newer, and the guest is the existing aarch64 Solo5 hvt ABI (`HVT_ABI_VERSION` 2). Intel Macs are out of scope. The tender talks to Hypervisor.framework directly, the same way `hvt_kvm.c` talks to KVM. One binary built for Linux/KVM aarch64 must run unmodified on the Mac tender.

Upstream Solo5 (v0.13.0, `docs/building.md`) lists three production hvt hosts: Linux/KVM x86_64, FreeBSD vmm x86_64, and Linux/KVM aarch64. macOS is absent. A working prototype already exists and should be the starting point, not a rewrite:

- Branch: `hvf-macos-aarch64` at <https://tangled.org/tsiry-sandratraina.com/solo5>
- Tender commit: `b1ca0516` (`tenders/hvt/hvt_hvf.c`, `hvt_hvf.h`, `hvt_hvf_aarch64.c`)
- It boots the in-tree suite, applies real guest W^X through `hv_vm_protect`, and drives the net ring over a socketpair

That prototype is a port, not a production target. It warns that it drops no privileges, it skips page protections for any unikernel whose ELF segments are not 16 KiB aligned, it has no `solo5-hvt-debug`, and it has no path onto a real network. Those four gaps are the work.

## What "production" means here

Match the bar Solo5 already uses for Linux/KVM aarch64, plus the things a Mac host has to get right because the kernel will not do them.

A release can move macOS from experimental to production in `docs/building.md` when all of these hold for two weeks of CI:

1. `tests/run-tests.sh` is green on Apple Silicon for every hvt test that Linux aarch64 runs. `test_seccomp` is replaced by a macOS sandbox test with the same intent.
2. An aarch64 `.hvt` linked on Linux with the stock 4 KiB `p_align` runs on the Mac tender, including `test_xnow` and `test_wnox` (guest W^X). The tender does not warn that protections were skipped.
3. `HVT_ABI_VERSION` stays 2. No new hypercall, no change to `struct hvt_boot_info`, no change to the MMIO addresses in `include/hvt_abi.h`.
4. `solo5-hvt` applies a seatbelt profile before `hv_vcpu_run`. `solo5-hvt-debug` is built with `-DHVT_DROP_PRIVILEGES=0`, same split as `e739feec` on the KVM and FreeBSD objects.
5. `solo5-hvt-debug` speaks the existing gdb stub (`hvt_module_gdb.c`) and writes an aarch64 ELF core that `docs/debugging.md` can already load.
6. Block works on a regular file. Net works over the existing `@<fd>` attachment and over vmnet shared NAT.
7. `opam install solo5` on macOS/arm64 installs a signed `solo5-hvt` and an ELF toolchain. A Mirage `unikernel.hvt` built with that toolchain answers an ICMP echo through vmnet.
8. GitHub Actions compiles the tender on every PR. A bare-metal Apple Silicon runner executes the suite at least nightly.

Until those are true, the backend lands as experimental, behind the host check in `configure.sh`, and `docs/building.md` says so.

## Shape of the change

Stay inside the existing tender split. `hvt_main.c` already calls `hvt_init`, `hvt_vcpu_init`, `hvt_guest_mprotect`, `hvt_drop_privileges`, `hvt_vcpu_loop`. Each backend fills those in. HVF is another backend next to KVM, FreeBSD vmm, OpenBSD vmm, and DragonFly nvmm.

| File | Role |
| --- | --- |
| `tenders/hvt/hvt_hvf.c` | `hv_vm_create`, guest RAM, `hv_vm_map`, `hvt_guest_mprotect`, privilege drop |
| `tenders/hvt/hvt_hvf.h` | `struct hvt_b`: vcpu, `hv_vcpu_exit_t *`, net-kick pipe |
| `tenders/hvt/hvt_hvf_aarch64.c` | EL1 register setup, run loop, MMIO syndrome decode |
| `tenders/hvt/hvt_gdb_hvf_aarch64.c` | gdb register and breakpoint ops from `hvt.h` |
| `tenders/hvt/hvt_dumpcore_hvf_aarch64.c` | ELF core, same notes as `hvt_dumpcore_kvm_aarch64.c` |
| `tenders/hvt/hvt_sandbox_macos.c` | seatbelt profile applied from `hvt_drop_privileges` |
| `tenders/hvt/hvt_vmnet_macos.c` | vmnet shared device behind the existing net module |

Guest bindings stay `bindings/hvt/` plus `bindings/hvt_cpu_aarch64.c`. The guest already does a 4 KiB stage-1 map, hypercalls with `str Wt, [Xn]` at `HVT_HYPERCALL_MMIO_BASE` (`0x100000000`), and kicks the net ring at `HVT_RING_KICK_MMIO_BASE`. The tender leaves that MMIO unmapped at stage 2 so the access becomes a data abort. That is the whole hypercall ABI. Do not introduce HVC, a GIC, or a virtio bus for the core calls.

Virtualization.framework is the wrong layer. It wants a Linux-shaped guest (DTB, GIC, virtio). Using it would be a second ABI. SPT-on-macOS would be a different target that cannot run an existing `.hvt` ELF. Both are out of scope.

`hv_vcpu_create` binds the vCPU to the calling thread, and only that thread may call `hv_vcpu_run`. `hvt_vcpu_init` and `hvt_vcpu_loop` stay on the main thread. The I/O thread is created before the sandbox, as it already is for the kqueue path in `hvt_core.c`.

## Hard problems

### 4 KiB guests on a 16 KiB host

Apple Silicon userland pages are 16 KiB. Solo5 aarch64 guests and the ELF they ship are 4 KiB (`AARCH64_PAGE_SIZE`, `TCR_EL1` `TG0_4K` in `hvt_kvm_aarch64.c`). The prototype papers over this by linking macOS-built unikernels with `-z max-page-size=0x4000` and skipping `hv_vm_protect` when a segment is not 16 KiB aligned. A Linux-built unikernel then runs with guest W^X off. That fails the production bar.

macOS 26 added `hv_vm_config_set_ipa_granule(config, HV_IPA_GRANULE_4KB)`. QEMU uses it so `hv_vm_map` accepts 4 KiB-aligned ranges on a 16 KiB host. The tender should do the same:

1. `hv_vm_config_create`, set `HV_IPA_GRANULE_4KB`, then `hv_vm_create(config)`. Refuse to start on macOS older than 26 with a message that names the minimum version. Do not call the private `_hv_vm_config_set_ipa_granule`.
2. `mmap` guest RAM with `MAP_ANON`. Size is already rounded to `AARCH64_GUEST_BLOCK_SIZE` (2 MiB), so the host mapping is 16 KiB aligned.
3. `hv_vm_map` the whole region at IPA 0 with read and write. Do not map it executable in the host. Host `mprotect` cannot split a 16 KiB page, and the tender must be able to write guest memory for the ELF loader and hypercall arguments. Host mappings of guest RAM stay `PROT_READ | PROT_WRITE` for the process lifetime, so the tender never executes guest code.
4. Guest W^X is stage 2 only. `hvt_guest_mprotect` calls `hv_vm_protect(gpa, len, flags)` on the exact ELF segment. With a 4 KiB granule those ranges are legal. `test_xnow` and `test_wnox` are the check.
5. Delete the macOS `-z max-page-size=0x4000` special case. The toolchain emits the same ELF it emits on Linux.

This depends on one spike, and the spike is the first thing to do. `hv_vm_protect` of a 4 KiB interior of a 16 KiB host page is documented as the point of the API, and it still needs a 20-line program on this machine that maps 64 KiB, protects bytes `[0x1000, 0x2000)` as read-only, and checks that a guest store there aborts while a store at `0x2000` succeeds. If that fails, stop and report it. The fallback (relinking every unikernel at 16 KiB) forks the binary from Linux/KVM and is not the production design.

### MMIO exits

KVM decodes the faulting store and advances PC. HVF returns `HV_EXIT_REASON_EXCEPTION` with `ESR_EL2` and the IPA in `exception.physical_address`. The prototype handles this for the one instruction the ABI emits (`str Wt, [Xn]`, ISS.ISV set, SAS = 4, WnR = 1): read `X[SRT]` (SRT 31 is the zero register), add 4 to PC, then dispatch.

Apple has shipped HVF builds where ISS.ISV is clear for valid stores. Production code needs a fallback decoder used only when the IPA is in the hypercall page or equals `HVT_RING_KICK_MMIO_BASE`, and only for the A64 store forms the ABI can emit (`STR Wt`, `STR Xt`, unsigned immediate). Anything else is a fatal exit with PC, ESR, VA, and IPA in the message. Cover both paths with a guest that does the normal `str` and, if a compiler ever emits a pair, fail the test loudly rather than skipping the instruction.

The ring kick has no `ioeventfd` equivalent. The prototype writes one byte to a pipe, which is what the BSD backends do. Make the write end `O_NONBLOCK` and ignore `EAGAIN`, so a slow I/O thread cannot stall the vCPU inside `write`. A kick that finds the pipe already full is still a kick.

`HV_EXIT_REASON_VTIMER_ACTIVATED` stays masked (`hv_vcpu_set_vtimer_mask`). The guest reads `cntvct_el0` and waits by calling `HVT_HYPERCALL_POLL`. It has no GIC. WFI/WFE (`ESR_EC` 0x01) step the PC by 4. SMC and HVC are fatal.

EL1 setup copies `hvt_kvm_aarch64.c`: `MAIR_EL1`, `TCR_EL1` with 4 KiB granules and 40-bit VA, `TTBR0_EL1` at `AARCH64_PGD_PGT_BASE`, `SCTLR_EL1` MMU and caches on, alignment checking off. HVF gives the architectural reset value of `SCTLR_EL1` rather than zero, so preserve RES1 bits. The prototype already does this. Initial PSTATE is `HV_REG_CPSR`, not `SPSR_EL1`, until the vCPU has run. `cpu_cycle_freq` is `cntfrq_el0` read on the host. On Apple Silicon that is 24 MHz. Pass it through `hvt_boot_info` unchanged. Do not invent a nanosecond counter.

### Privilege drop

Linux uses seccomp (`hvt_seccomp_linux.c`), FreeBSD uses capsicum, OpenBSD uses pledge. macOS has seatbelt. `sandbox_init` is marked deprecated and is still the mechanism that can express "this process may only touch these fds". App Sandbox (the entitlement) fights `com.apple.security.hypervisor` and vmnet. Use a custom SBPL profile, applied after devices are open and the I/O thread exists, before `hv_vcpu_run`.

Profile, once traced, should be:

- deny by default
- allow the Mach services `hv_vcpu_run` actually looks up (discovered by running once under `(deny default)(debug deny)` and reading the sandbox violation log)
- allow `file-read*` and `file-write*` and `file-ioctl` only on the block-device fd and the net fd already open
- allow read/write on the kick pipe and the poll fds
- allow stdio
- deny `process-exec`, `process-fork`, `sysctl-write`, and file creation

`test_sandbox` is the acceptance test: after `hvt_drop_privileges`, a check in the tender (compiled into a test-only hypercall or run in the parent before the loop) confirms `open`, `exec`, and `fork` fail. Ship the profile as `tenders/hvt/sandbox.sb` and compile it in, so a user does not need a sidecar file.

The hypervisor entitlement is `com.apple.security.hypervisor`. It is only honoured on a signed binary. The link rule ad-hoc signs with `codesign -s - --entitlements tenders/hvt/hvf.entitlements -f solo5-hvt`. `make install` signs again, because `strip` invalidates the signature. `hv_vm_create` failure `HV_DENIED` prints that sentence, not just the hex code.

### Network

There is no `/dev/net/tun` on macOS. Two attachments:

- `--net:NAME=@FD` stays the test and advanced path. The prototype fixed a real bug: the `@fd` path checked `errno` without clearing it and never set `*mtu`. Keep that fix, and add a regression that does not need a Mac (the bug is in shared `tap_attach`).
- `--net:NAME=vmnet` opens a `vmnet_shared` interface (`com.apple.vm.networking`), reads the host-assigned guest MAC and the NAT subnet, and feeds the existing `hvt_module_net.c` packet path. The guest still speaks `NET_BASIC` or the ring. Bridged vmnet needs a privileged helper. Do not block production on bridged mode. Document shared NAT as the supported attachment.

In-tree net test for CI is a socketpair peer, the same one the prototype used for ARP plus ICMP. Check it in under `tests/` so a laptop without vmnet entitlements can still run the suite. vmnet is a separate, optional test.

### Debug

`hvt_gdb_supported` returns 0 on the debug binary. Implement the ops in `hvt.h`:

- registers via `hv_vcpu_get_reg` / `hv_vcpu_set_reg` and the sysreg calls, in the order `hvt_gdb_aarch64.h` already defines
- software breakpoints by patching `BRK #0` into guest memory and catching `ESR_EC` 0x3c, then stepping over and restoring
- single step via `MDSCR_EL1.SS` and `hv_vcpu_set_trap_debug_exceptions`
- `hvt_dumpcore_hvf_aarch64.c` cloned from `hvt_dumpcore_kvm_aarch64.c`, with register reads swapped to HVF

`tests/test_dumpcore` and a short gdb script (`target remote`, break, step, continue) are the checks.

### Toolchain and packaging

macOS clang emits Mach-O and there is no `<elf.h>`. The prototype's approach is the right one:

- `configure.sh` on Darwin/arm64 looks for `ld.lld` and `llvm-objcopy` (Homebrew `llvm`, Nix, or a prefix passed in). If they are missing, the error names the package and the variables to set.
- Darwin/x86_64 fails at configure with one sentence: this tender targets Apple Silicon.
- A private ELF64 header subset covers `tenders/common/elf.c`. Keep it minimal and only compiled on hosts without `<elf.h>`.
- Mach-O has no `__start_modules` / `__stop_modules`. Use `section$start$__DATA$modules` and `section$end$__DATA$modules`, which the prototype already does. Guard it so the ELF linkers are untouched.
- LLD honours the linker script's `PT_TLS` assignment literally. GNU ld stuffed `.tdata` into `PT_TLS` anyway. The script must name `:tbss` on `.tdata` so both linkers cover `.tdata` and `.tbss`. That fix is correct on Linux too. Land it on its own.
- opam: `available` for `os = "macos" & arch = "arm64"`, depexts on `llvm` and `llvm-objcopy`. The post-install step re-signs `solo5-hvt`.

`test_fpu` has a Clang-only bug (the second operand is loaded into `v0` twice, then the code multiplies by an uninitialised `v1`). `test_time` allows too few retries for a 24 MHz `cntvct_el0`: six calls can land inside one tick. Both fixes belong on upstream `main` before any Mac-specific commit, and both are already written on the prototype (`1847515b`).

## Work sequence

Each phase ends with something an agent can re-run. Do not start a phase until the previous check is green. PRs are sliced so Solo5 review stays small. Maintainers to expect review from are the people already on `hvt` (dinosaure, hannesm, reynir).

### Phase 0. Spike and rebase

Done when: a throwaway C program on this Mac prints the 4 KiB protect result, and the prototype branch builds against current `main`.

1. Clone `https://github.com/Solo5/solo5` and the prototype branch. Diff `b1ca0516` against the upstream tree it was cut from, and rebase onto current `main`. Expect conflicts in `hvt_core.c`, `hvt_module_net.c`, and `configure.sh` because of the net ring and `--no-net-ring` (`#670`).
2. Write `/tmp/hvf_granule.c` that creates a VM with `HV_IPA_GRANULE_4KB`, maps one `mmap` of 64 KiB at IPA 0, `hv_vm_protect`s `[0x1000, 0x2000)` read-only, runs a tiny guest that stores to `0x1000` and to `0x2000`, and exits 0 only if the first store data-aborts and the second does not. Sign it ad-hoc with the hypervisor entitlement.
3. Record the result in the PR description. A failure stops the project at this phase.
4. `sysctl kern.hv_support` on the machine that will run CI. Hosted GitHub `macos-26` runners still report it as unavailable (actions/runner-images#13505, and the hosted-runner docs still say nested virtualization is off). Pick a bare-metal box and note its owner.

### Phase 1. Upstream the host-agnostic fixes

Done when: Linux aarch64 and x86_64 CI are green and the diffs contain no HVF code.

- `test_fpu` Clang operand fix.
- `test_time` retry budget. Keep the loop bounded. Assert the counter moved, not that it moved within six calls.
- `.tdata` in `PT_TLS` in the hvt linker script.
- `@fd` path in the shared tap helper: clear `errno`, set `*mtu`.

### Phase 2. Experimental tender, 4 KiB granule

Done when: `tests/run-tests.sh` passes on this Mac, a Linux-built `test_hello.hvt` and `test_xnow.hvt` pass without a protection warning, and `HVT_ABI_VERSION` is unchanged.

- Land `hvt_hvf.c`, `hvt_hvf.h`, `hvt_hvf_aarch64.c` from the rebase, with these edits on top:
  - 4 KiB IPA granule, macOS 26 floor
  - host mapping never executable
  - `hv_vm_protect` at segment granularity, no 16 KiB skip
  - non-blocking ring kick
  - ISV=0 fallback decoder for the hypercall page
  - `configure.sh` enables the backend only for Darwin/arm64, requires `ld.lld` and `llvm-objcopy`, and says why
- Wire `hvt_main.c` the way the other backends are wired. Reuse the kqueue poll path.
- Ad-hoc sign in the link rule.
- Run the suite. Tests that need a tap device use `@fd` or get skipped with a named reason. Do not skip `test_xnow`, `test_wnox`, `test_exception`, `test_zeropage`, `test_fpu`, `test_time`, `test_tls`, `test_ssp`, `test_blk`, `test_hello`.

### Phase 3. Seatbelt

Done when: `test_sandbox` shows `open`/`exec`/`fork` failing after drop, and the rest of the suite still passes with the profile applied. `solo5-hvt-debug` is linked with `-DHVT_DROP_PRIVILEGES=0`.

Trace the profile before writing it. One run:

```sh
log stream --style compact --predicate 'senderImagePath CONTAINS "sandbox" OR eventMessage CONTAINS[c] "Sandbox"'
```

under a deny-default profile, then allow only the Mach services that appear. Check the profile in. If `hv_vcpu_run` needs a service that also grants arbitrary filesystem access, stop and write that down. A sandbox that cannot be tightened is a reason to stay experimental, and it should be an explicit decision rather than a warning print.

### Phase 4. gdb and coredump

Done when: `solo5-hvt-debug` hits a breakpoint in `test_hello`, single-steps one instruction, and `test_dumpcore` produces a core gdb can print `pc` from.

Follow `hvt_gdb_kvm_aarch64.c` for the register order so the stub in `hvt_module_gdb.c` stays shared. Software breakpoints first. Hardware breakpoints only if the debug-exception trap is straightforward.

### Phase 5. vmnet and opam

Done when: `opam install solo5` on a clean macOS 26 arm64 prefix yields a tender that runs a Mirage hello, and ICMP to that unikernel through `--net:service=vmnet` gets a reply.

- `hvt_vmnet_macos.c` implements shared mode only. Surface the vmnet MAC and MTU through the existing manifest setup.
- opam availability line, depexts, and a postinstall `codesign`.
- One Mirage test, checked in somewhere the Solo5 CI can skip when `mirage` is absent. Locally it is part of the production gate.

### Phase 6. CI and the doc promotion

Done when: a PR cannot merge with a broken Mac build, a nightly on bare metal is green for two weeks, and `docs/building.md` lists the target under Production with the macOS 26 floor and the vmnet invocation.

- `.github/workflows/macos-arm.yml`: `runs-on: macos-26`, `brew install llvm`, `configure`, `make`. This job does not run guests. It exists to catch toolchain and signature bitrot.
- A self-hosted runner labelled `solo5-macos-arm-hv` with `kern.hv_support == 1`. The job runs `tests/setup-tests.sh` and `tests/run-tests.sh`. The workflow fails if the sysctl is 0. Do not `continue-on-error` a missing hypervisor.
- `docs/building.md` and `docs/debugging.md` get a macOS section: entitlement, signing, `@fd`, vmnet, the 24 MHz counter, and the fact that the vCPU thread is the main thread.
- Only then change the support table from Experimental to Production.

## Review slices

Ship in this order, each slice buildable on its own:

1. Test and linker fixes from phase 1.
2. HVF backend, experimental, no sandbox and no gdb. Mark it experimental in the same PR.
3. 4 KiB granule and W^X, if it was not safe to include in slice 2.
4. Seatbelt and `test_sandbox`.
5. `solo5-hvt-debug`, gdb, coredump.
6. vmnet, opam, CI, doc promotion.

## Out of scope

- Intel Macs and the x86_64 HVF/VMX API. No current production fleet needs a second hypercall style (PIO at `HVT_HYPERCALL_PIO_BASE`).
- macOS 14 and 15. They cannot set a public 4 KiB IPA granule, so they cannot meet the W^X bar for Linux-built binaries.
- Virtualization.framework, a GIC, virtio, and SMP. Solo5 is intentionally unicore. Scale by running more tenders.
- Nested virtualization (`hv_vm_config_set_el2_enabled`). The guest is EL1.
- Matching KVM `ioeventfd` kick latency. The BSD pipe path is already accepted on a production host. Measure it, and record the number, once the suite is green.
- Bridged networking, notarized Developer ID distribution, and an App Store build. Ad-hoc signing is enough for `opam install` and local use. Developer ID is a packaging follow-up once the entitlement set is stable.

## Risks worth tracking

- The 4 KiB `hv_vm_protect` spike fails. The project stays experimental and the binary-compatibility goal needs a new design. Do not silently flip the linker to 16 KiB.
- Seatbelt cannot see a safe allow-list for `hv_vcpu_run`. Stay experimental and keep the warning. Do not claim production with an open tender process.
- Hosted GitHub runners grow `kern.hv_support`. Revisit the self-hosted runner when that happens. Until then the nightly is the test signal.
- `cntvct_el0` at 24 MHz surprises unikernels that busy-spin expecting a nanosecond step. The ABI already publishes `cpu_cycle_freq`. Mention it in the docs. Do not change the guest clock source.
- A future macOS changes the syndrome for `str Wt`. The ISV=0 decoder and a test that the hypercall still fires are the mitigation.
