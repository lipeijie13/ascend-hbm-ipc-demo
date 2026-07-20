# XDS dependency

This directory contains the minimal XDS sources needed by the SSD-to-HBM
validation path:

- `file_p2p/`: the userspace `read_file`/`drain_read` API;
- `p2p_dev.c` and UAPI headers: the experimental Linux kernel module.

The files were copied from the `yuanrong-datasystem` `xds` branch after its
post-`6576f2d9` correctness fixes. That branch is the source of truth for this
demo snapshot. The userspace library is built only with `ENABLE_XDS=ON`.

The `xds_kernel_module` target is opt-in and never runs as part of the normal
build. Loading the resulting module changes host-wide kernel state and must be
performed separately by an administrator after checking kernel, NVMe, and
Ascend driver compatibility.
