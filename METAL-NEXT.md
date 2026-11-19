# Metal at user quality - cepheus plan

Copied from ios-flame's METAL-NEXT.md. The plan is the same because cepheus
and flame share the same SoC (SM8150) and GPU (Adreno 640). Only board-level
addresses differ (display, touch, etc.).

The approach: port UniXNU's on-device Metal stack:
Metal frontend -> metal2vulkan (AIR -> SPIR-V, Metal API on Vulkan)
-> Turnip with a cepheus kernel backend -> our kernel.

Host-made templates cannot cover apps; shaders must compile on the phone.

Read ios-flame/METAL-NEXT.md for the full plan. This file exists as a
placeholder; adapt as cepheus bring-up progresses.

## Build

The development Mac has the Command Line Tools but no Xcode/iPhoneOS SDK, so the
userland is built by `.github/workflows/userland.yml` on a `macos-15` runner (manual
`workflow_dispatch`). It applies UniXNU's Mesa patch series to Mesa 85fda3b3, builds
Turnip and the metal2vulkan bridge, and uploads `sweetmetal2-ios16`:

```
SweetMetal2.bundle  m2vtest  vktest  libvulkan_freedreno.dylib
```

Unzip the artifact at the repository root, install with
`tools/cepheus-metal2-install.sh`, and boot with `sweetgpu=1 sweetmtl=2` (the kernel's
v2 client is in patch 0115, the plugin switch in 0116).
