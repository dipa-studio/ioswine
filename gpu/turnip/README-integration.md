# turnip vkshim integration

Connects vkshim to real Vulkan entry points. The Vulkan
implementation lives in a separate helper process; the lsk-side
pointers vkshim needs are marshaling stubs that serialize each
call to the helper over a Unix socket (venus-like). vkshim.h and
vkshim.c are NOT changed: vkshim_load(turnip_get_proc_addr, &shim)
fills the 38-entry table with the stubs, and the stubs do the rest.

## Files

| File | What it is |
| --- | --- |
| `turnip_vktypes.h` | Minimal Vulkan types, `tw_` prefix, layout-identical to the official Vulkan 1.3.275 structs (field order and types match; verified by `_Static_assert` against `/usr/include/vulkan/vulkan_core.h` in the helper and in the host test). |
| `turnip_wire.h` | Shared wire protocol (framing, op enum, per-op request/response structs, caps). Used by both the stubs and the helper. |
| `turnip.h` / `turnip.c` | Freestanding lsk-side client: `turnip_init(path)`, `turnip_get_proc_addr(name)`, `turnip_shutdown()`, `turnip_connected()`, `turnip_last_error()`, `turnip_physical_device_name()`, plus the 38 stub functions. No libc, no function-pointer tables in static data (if/else name chain). |
| `gpu-helper.c` | Helper process. dlopens the Vulkan implementation, resolves entry points, serves the wire protocol on a Unix socket, executes the real Vulkan calls. |
| `tests/host/turnip_test.c` | Host test (see below). |
| `build-integration.sh` | Builds everything and runs the host test. |

## Why stubs and a helper process

The bionic-free iOS process can never dlopen Turnip (no dynamic
loader for the ICD, no bionic, no access to the KGSL driver from
that address space). So the Vulkan implementation runs in a
separate helper process that owns the driver, and the lsk side
only holds marshaling stubs. The stubs have the exact Vulkan
signatures vkshim expects, so vkshim_load() cannot tell them
from real entry points.

Handles on the wire are u64 ids assigned by the helper. The
client stores the id as the value of its opaque handle pointer
and never dereferences it. 0 is VK_NULL_HANDLE. The helper keeps
a handle table (id to real handle plus kind plus parent) and
dedupes by (handle, kind), so the same physical device or queue
always maps to the same id.

## Wire protocol (turnip_wire.h)

Framing magic `0x4E525554` ("TURN"), version 1, max payload
65536, little-endian, same architecture both ends. Request
header {magic, op, seq, payload_len}; response header {magic,
op, seq, result (VkResult), payload_len}. One op per Vulkan
entry, named after the entry (op 1-38 are the vkshim entries in
vkshim.h order), plus op 0 PING and op 39
GET_PHYSICAL_DEVICE_NAME (a host-test helper; vkshim has no
vkGetPhysicalDeviceProperties).

Per-op payloads are a fixed struct plus variable blobs in
documented order. Create-info structs are carried as raw bytes
of the layout-identical `tw_` struct with pointer fields zeroed;
the helper rebuilds them from the blobs (names, sub-structs,
arrays) and points the arrays into the payload or a scratch
buffer. The request and the response are synchronous: one
request, one response, one client at a time.

Alignment rule: both headers and every response struct with a
trailing variable array are a multiple of 8 bytes, so the
payload and the arrays that follow a struct are 8-byte aligned
(the payload structs carry u64 fields; a misaligned payload is
UB on arm64 and is flagged by UBSan on the host). Enforced by
`_Static_assert` in turnip_wire.h.

## Limits (enforced by the stub, documented in turnip_wire.h)

pNext must be NULL, pAllocator must be NULL, pSpecializationInfo
must be NULL, pInheritanceInfo must be NULL (primary command
buffers only). Caps: 8 pipelines per vkCreateGraphicsPipelines,
64 layer/extension names, 64 queue create infos, 16 subpasses,
32 attachments, 16 clear values, 8 submits per vkQueueSubmit.
A stub that hits a cap returns TW_VK_ERROR_OUT_OF_HOST_MEMORY
without a round-trip. Void-returning stubs record failures in
the sticky turnip_last_error(); transport failure is
TURNIP_ERR_TRANSPORT (-100).

## Two helper modes

- Host mode (`--mode host`, default library `libvulkan.so.1`):
  dlopens the system loader and resolves entries with
  vkGetInstanceProcAddr. Used by the host test against Mesa
  llvmpipe. The loader does NOT export vk_icdGetInstanceProcAddr,
  so host mode must not use it.
- Device mode (`--mode device`, default library
  `libvulkan_freedreno.so`): dlopens the Turnip ICD directly and
  resolves entries with vk_icdGetInstanceProcAddr. Device-level
  entries are resolved lazily through vkGetDeviceProcAddr,
  cached from any live instance.

The helper takes `--socket <path>` (required), `--mode
host|device`, `--icd <path>` (override the library), and
`--verbose`. It ignores SIGPIPE; SIGTERM/SIGINT stops the
accept loop and unlinks the socket. One client at a time,
sequential request/response.

## Host test

`tests/host/turnip_test.c` starts the helper in host mode
(fork+exec, per-process socket path under /tmp, killed at the
end), calls turnip_init + vkshim_load(turnip_get_proc_addr,
&shim), asserts all 38 entries resolved (missing_count == 0),
then drives a real instance, physical device enumeration, queue
family and memory queries, a logical device, a queue, a buffer
and a memory allocation through the stubs, and prints the
physical device name. It also carries layout checks
(`_Static_assert` sizeof equality for the `tw_` structs against
the real Vulkan headers).

Run it with:

```
./build-integration.sh
```

That builds turnip.c and vkshim.c freestanding (NDK r28c
aarch64 clang, -ffreestanding -Wall -Wextra -Werror), builds
the helper and the host test with host gcc (plain and
ASan/UBSan), and runs the test in both configurations.

Current result (2026-11-12, this host): PASS in both
configurations, zero UBSan reports. Physical device:
`llvmpipe (LLVM 20.1.2, 256 bits)`, 1 queue family, 1 memory
type, 1 heap. Logical device, queue, 256-byte vertex buffer
bound to memory type 0, device wait idle all succeed through
the stubs.

## Build notes and gotchas

- The src tree ships its own `libsystem_kernel/src/errno.h`
  (the lsk's freestanding errno mapping). With that directory
  on `-I`, a host `#include <errno.h>` resolves to it instead
  of the system header, so `errno`/`EINTR` go undeclared.
  build-integration.sh therefore compiles turnip.c for the host test
  WITHOUT the src tree on `-I` (it needs no src header in host
  mode); turnip_test.c and vkshim.c, which need vkshim.h, are
  compiled separately and linked.
- The two-call enumerate pattern (count query, then list) is
  the one place the stub and the helper disagree about the
  response shape: a count query returns only the count, no
  array. The enumerate and queue-family stubs only require the
  trailing array when it was requested. (Found by the host
  test: the count query returned VK_ERROR_DEVICE_LOST from the
  stub's own length check.)
- vkCreateInstance resolves through vkGetInstanceProcAddr in
  host mode; do not use vk_icdGetInstanceProcAddr against the
  loader, it is not exported there.

## What is NOT done

- No device test. The host test proves the round-trip
  (vkshim to stub to socket to helper to real Vulkan) with a
  real driver, but on llvmpipe, not on Turnip/Adreno 730.
  Device mode has never run; the first run on the test phone is the
  acceptance test.
- No dma-buf export. The render target image is created
  through vkCreateImage like any other image; there is no path
  yet that hands its dma-buf fd to the presenter or IOSurface.
- No swapchain. The 38-entry set deliberately excludes it;
  presentation is handled separately and the render target
  here is an IOSurface-backed memfd image, not a swapchain
  image.
- No vkDestroyInstance / vkDestroyDevice. They are not in the
  38-entry set, so the instance and device live until the
  helper process exits. The host test cleans up what the set
  can destroy (buffer, memory) and leaves the rest to the
  helper exit.
- No vkGetPhysicalDeviceProperties in the entry set; the
  device name is a test-only helper op (39), not a vkshim
  entry.
