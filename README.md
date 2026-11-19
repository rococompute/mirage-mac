# MirageOS on Apple Silicon

This superproject pins the Solo5 HVF runtime, OCaml cross toolchain, Mirage,
crypto and GMP forks, opam overlay, and integration tests used to build MirageOS
unikernels on Apple Silicon Macs. The network test cases require macOS 26+.

Clone all pinned sources using public HTTPS URLs:

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

When updating forks, commit and push each child repository first, regenerate and
commit the overlay, and then commit the corresponding gitlinks here. The overlay
uses public Git URLs pinned to commits; its package revisions must increase when
the installed fork contents change. `git submodule update --remote` is not part
of reproducible setup because it follows moving branches.
