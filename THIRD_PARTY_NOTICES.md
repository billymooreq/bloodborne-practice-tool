# Third-party notices

The code of Bloodborne Practice Tool itself is under the MIT License (see `LICENSE`).
It is built with the following third-party software. Their sources are in `third_party/`,
each with its own license file; the release zip carries the licenses in `practice-tool/licenses/`.

| Component | Version | License | Copyright | Used for |
|---|---|---|---|---|
| [Dear ImGui](https://github.com/ocornut/imgui) | 1.93.0 | MIT | (c) 2014-2026 Omar Cornut | the in-game menu |
| [Vulkan-Headers](https://github.com/KhronosGroup/Vulkan-Headers) | 1.4.309 | Apache-2.0 OR MIT (used under MIT) | (c) 2015-2023 The Khronos Group Inc. | drawing the menu (headers only) |
| [LZ4](https://github.com/lz4/lz4) (lib/) | 1.10.0 | BSD 2-Clause | (c) 2011-2020 Yann Collet | savestate compression |
| [Zstandard](https://github.com/facebook/zstd) (lib/) | 1.5.6 | BSD 3-Clause (dual-licensed with GPLv2; used under BSD) | (c) Meta Platforms, Inc. and affiliates | savestate compression |
| [xxHash](https://github.com/Cyan4973/xxHash) | 0.8.2 | BSD 2-Clause | (c) 2012-2021 Yann Collet | page hashes for deduplication |
| [IM Fell English SC](https://fonts.google.com/specimen/IM+Fell+English+SC) | | SIL Open Font License 1.1 | (c) 2010 Igino Marini | menu font (embedded in the DLL) |
| [Cormorant Garamond](https://github.com/CatharsisFonts/Cormorant) | | SIL Open Font License 1.1 | (c) 2015 the Cormorant Project Authors | menu font (embedded in the DLL) |

License texts:

- Dear ImGui: `third_party/imgui/src/LICENSE.txt`
- Vulkan-Headers: `third_party/vulkan/LICENSE.md` (and the SPDX line in each header)
- LZ4: `third_party/lz4/LICENSE`, `third_party/lz4/lib/LICENSE`
- Zstandard: `third_party/zstd/LICENSE` (BSD), `third_party/zstd/COPYING` (GPLv2, not used)
- xxHash: `third_party/xxhash/LICENSE`
- Fonts: `third_party/fonts/OFL-IMFellEnglishSC.txt`, `third_party/fonts/OFL-CormorantGaramond.txt`

The tool works with [bbport](https://github.com/Supermedo/bloodborne_pc) by Super Medo but contains none of its code;
it reads and patches the running port in memory. Bloodborne is a trademark of Sony Interactive Entertainment;
the game was developed by FromSoftware. This project is not affiliated with either.
