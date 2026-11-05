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
