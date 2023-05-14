# Build Instructions

Download the `cheats_ws.zip` and `cheats_ni.zip` files from here: https://github.com/PCSX2/pcsx2_patches/releases,

Then move them to the [bin/resources](https://github.com/SirMangler/pcsx2/tree/master/bin/resources) folder.

Once that's done open the `pcsx_qt.sln` file, and choose the `Release` configuration to make SSE4 builds (compatible with Xbox One consoles), or select the `Release AVX2` Configuration to make AVX2 builds (faster, but only works on the Xbox Series consoles).

**Note**: Even with the SSE4 builds working on the Xbox One consoles, their subpar CPUs mean that performance in the majority of games will be terrible no matter what settings you tweak. Only use this on the Xbox One consoles if you are really curious about it.