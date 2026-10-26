# MirageOS unikernels on Apple Silicon

This workspace makes MirageOS unikernels build and run natively on an Apple
Silicon Mac (macOS 26+). The unikernels run in Hypervisor.framework VMs
through Solo5's `hvt` tender, with no root, no Linux VM and no
Virtualization.framework. See `PLAN.md` for the design, the production bar
and its status.

## What is here

| Directory | What it is |
| --- | --- |
| `solo5/` | Solo5 fork, branch `hvf-macos`: HVF backend, sandbox, `solo5-vmnet`, tests |
| `ocaml-solo5/` | ocaml-solo5 fork: builds the OCaml cross-compiler on macOS |
| `mirage/` | mirage tool fork: accepts solo5 0.13 |
| `mirage-crypto/` | mirage-crypto fork: entropy self-test on 24 MHz ARM timers |
| `ocaml-gmp/` | gmp fork: builds with LLVM binutils and as a macOS host library |
| `opam-repository/` | opam overlay carrying the five forks (`./generate.sh` rebuilds it) |
| `mirage-tests/` | `run-matrix.sh`: builds and runs a set of mirage-skeleton unikernels |
| `spikes/` | the throwaway programs behind `solo5/docs/hvf-spikes.md` |

Each fork is a stack of commits on top of its upstream; `git log
<upstream tag>..hvf-macos` in each shows exactly what changed.

## Setting up a machine

```sh
brew install llvm lld aarch64-elf-gcc
opam switch create mirage-hvf ocaml-base-compiler.5.5.2
eval $(opam env --switch mirage-hvf)
opam repo add hvf-mac /Users/yev/Development/solo5-mac/opam-repository \
    --this-switch --rank=1
opam install mirage ocaml-solo5 solo5 opam-monorepo
```

This machine already has a switch set up this way, called `hvf`.

## Building and running a unikernel

```sh
eval $(opam env --switch hvf)
cd my-unikernel
mirage configure -t hvt
make depends    # keep the hvf-mac repository selected in the switch
make build
solo5-hvt --mem=64 -- dist/my-unikernel.hvt
```

With a network (NAT to the internet, DHCP and DNS from macOS, no root):

```sh
solo5-vmnet -- solo5-hvt --mem=128 -- dist/my-unikernel.hvt
```

With a network restricted to some destinations, a CPU time limit and a
machine-readable exit report:

```sh
solo5-vmnet --allow=203.0.113.10/32:443 -- \
    solo5-hvt --mem=128 --cpu-limit=60 --exit-report=3 -- \
    dist/my-unikernel.hvt 3>report.json
```

With a block device: `--block:NAME=disk.img`. More detail is in
`solo5/docs/building.md`, section "_hvt_: Running on macOS".

## What the sandbox guarantees

- The guest only sees its own memory. Code is execute-only, read-only data
  is read-only, and nothing writable is executable (stage 2, 4 KiB granule).
- Before the guest runs, the tender enters a deny-all seatbelt sandbox: no
  opening or creating files, no exec or fork, no network, no new file
  descriptors. Only the block and network devices it was handed keep
  working.
- The tender never has network access itself. All guest traffic goes
  through `solo5-vmnet`, which can enforce an egress allow-list.
- A misbehaving guest (bad hypercalls or MMIO, illegal instructions, W^X
  violations, endless loops under `--cpu-limit`) is stopped and the reason
  reported. `solo5/tests/test_hostile` exercises each case.

## Checking it works

```sh
cd solo5 && ./configure.sh && make && tests/run-tests.sh   # Solo5 suite
cd mirage-tests && ./run-matrix.sh                          # MirageOS matrix
```

## Known gaps

- **Entropy.** Apple Silicon has no hardware RNG instruction and Solo5 has no
  entropy hypercall, so guests seed their RNG from the 24 MHz timer. That is
  weak for long-lived TLS keys. See `ENTROPY.md` for the issue and the proposed fix.
- **Debugging.** `solo5-hvt-debug` (gdb stub, core dumps) is not built on
  macOS yet. Upstream does not implement these for aarch64 on Linux either.
- **CI.** Nothing runs automatically yet. GitHub's hosted macOS runners cannot
  use Hypervisor.framework, so this needs a self-hosted Apple Silicon runner.
- Host-to-guest connections over vmnet could not be tested from the agent's
  sandbox. Guest-to-internet and guest-to-guest traffic are tested.
