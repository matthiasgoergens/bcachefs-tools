# Hermetic reproducer for the bcachefs per-snapshot accounting key aliasing.
#
# Builds two bcachefs-tools from source at pinned commits and runs a filesystem
# written by the OLD one through a read-write update by the NEW one.
#
#   OLD_REF  parent of the first offending commit -- the clean baseline
#   NEW_REF  any later commit; defaults to the master used when this was written
#
# Runs entirely unprivileged: no FUSE, no /dev/fuse, no loop devices, no kernel
# bcachefs module, no VM.  Everything happens on a plain image file.
#
#   docker build -t bcachefs-acct-repro .
#   docker run --rm bcachefs-acct-repro
#
# To bisect, override the refs (earlier layers stay cached):
#   docker build --build-arg NEW_REF=<sha> -t bcachefs-acct-repro .

FROM archlinux:base-devel AS deps
# rust-bindgen is required: the build shells out to the `bindgen` CLI, which is
# not pulled in by clang alone (fs/codegen.rs panics with ENOENT without it).
RUN pacman -Syu --noconfirm \
        git rust rust-bindgen clang pkgconf python-docutils \
        libaio libsodium liburcu zlib lz4 zstd xz keyutils \
        systemd-libs util-linux-libs libunwind \
    && pacman -Scc --noconfirm

# Clone once; both build stages reuse this layer.
FROM deps AS src
ARG REPO=https://github.com/koverstreet/bcachefs-tools.git
RUN git clone "$REPO" /src

# --- the two builds; each cached independently on its ref -------------------

FROM src AS build-old
# Parent of e80608bb6 ("accounting: per-snapshot key-count accounting").
ARG OLD_REF=2c7d916a53b1ebc47ce6314146de10ab96ebf869
RUN cd /src && git checkout --detach "$OLD_REF" && make --jobs="$(nproc)"
RUN cp /src/bcachefs /bcachefs-old

FROM src AS build-new
ARG NEW_REF=472b4aa4488e0d3aee6041d033e24dc3b6af2852
RUN cd /src && git checkout --detach "$NEW_REF" && make --jobs="$(nproc)"
RUN cp /src/bcachefs /bcachefs-new

# --- run --------------------------------------------------------------------

FROM deps AS run
COPY --from=build-old /bcachefs-old /usr/local/bin/bcachefs-old
COPY --from=build-new /bcachefs-new /usr/local/bin/bcachefs-new
COPY repro.sh /repro.sh
RUN chmod +x /repro.sh
ENTRYPOINT ["/repro.sh"]
