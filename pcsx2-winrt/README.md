# Build Instructions

Download the `patches.zip` file from [here](https://github.com/PCSX2/pcsx2_patches/releases/tag/latest), then move the file to the [bin/resources](https://github.com/SirMangler/pcsx2/tree/master/bin/resources). 

Then, open the `PCSX2_winrt.sln` file, and choose the `Release WinRT` configuration to make SSE4 builds (compatible with Xbox One consoles), or select the `Release WinRT AVX2` Configuration to make AVX2 builds (faster, but only works on the Xbox Series consoles).

**Note**: Even with the SSE4 builds working on the Xbox One consoles, their subpar CPUs mean that performance in the majority of games will be terrible no matter what settings you tweak. Only use this on the Xbox One consoles if you are really curious about it.