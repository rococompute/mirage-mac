# MirageOS on Apple Silicon

This superproject pins the Solo5 HVF runtime, OCaml cross toolchain, Mirage,
crypto, GMP and opam-monorepo forks, opam overlay, and integration tests used to build MirageOS
unikernels on Apple Silicon Macs. The network test cases require macOS 26+.

The fork repositories currently require GitHub repository access. Authenticate
Git before cloning or letting opam fetch sources (for example, with
`gh auth login` followed by `gh auth setup-git`). Clone the pinned sources over HTTPS:

```sh
git clone --recurse-submodules https://github.com/rococompute/mirage-mac.git
cd mirage-mac
```

For an existing clone, initialize the exact committed revisions with:

```sh
git submodule sync --recursive
git submodule update --init --recursive
```

Build the cross-toolchain and Mirage CLI through the committed overlay:

```sh
brew install llvm lld aarch64-elf-gcc
opam switch create mirage-hvf ocaml-base-compiler.5.5.2
eval "$(opam env --switch=mirage-hvf)"
opam repo add hvf-mac ./opam-repository --this-switch --rank=1
opam install mirage ocaml-solo5 solo5
```

See [the overlay instructions](opam-repository/README.md) for regeneration and
[the matrix instructions](mirage-tests/README.md) for building and running test
unikernels. Start with `cd mirage-tests && ./run-matrix.sh --no-network` for the
cases that only need Hypervisor.framework. Network cases also need vmnet and
outbound internet access.

## Unikernels that run programs of their own

`solo5-hvt` maps guest memory W^X: only the image's code is executable. A
unikernel that loads programs into its memory and runs them (kykeon runs Linux
agent binaries this way) declares user memory instead, the one region that may
be written and executed, and can link itself higher to leave room below for
programs linked at fixed addresses. In `config.ml`:

```ocaml
let () =
  register "agent"
    ~user_memory:[ user_memory ~address:0x200000 ~size:0xbfd00000 "guest" ]
    ~text_start:0xc0000000
    [ main $ ... ]
```

The tender checks the region when it loads the image and refuses to run it,
saying why, if it does not fit (`--mem`, the image, alignment). C code gets it
with `solo5_user_memory_acquire("guest", &info)`. See USER_MEMORY in
[solo5/docs/architecture.md](solo5/docs/architecture.md).

When updating forks, commit and push each child repository first, regenerate and
commit the overlay, and then commit the corresponding gitlinks here. The overlay
uses HTTPS Git URLs pinned to commits; its package revisions must increase when
the installed fork contents change. `git submodule update --remote` is not part
of reproducible setup because it follows moving branches.
