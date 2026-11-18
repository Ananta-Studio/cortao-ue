# Third-party notices

This plugin is a port of CORTAO to Unreal Engine. The port (the Unreal Engine side: C++ modules, the pass shaders
in `Shaders/Private/*.usf` and `CORTAOCommon.ush`, the documentation) is Copyright (c) 2026 Ananta Studio and is
licensed under the MIT License in [`LICENSE`](LICENSE).

It includes the software below, which keeps its own copyright and licence.

## CORTAO (Circular Occluder Ray-Traced Ambient Occlusion)

- Author: Daniel Isheden
- Source: <https://github.com/theagentd/CORTAO>, reference implementation 1.0.3
- Licence: MIT
- Files in this repository:
  - `Shaders/Private/ThirdParty/CORTAOBase.ush`, `CORTAOPrepass.ush`, `CORTAOMainPass.ush`, `CORTAOFilterPass.ush`:
    upstream's `src/shaders/*.h`, renamed `.h` to `.ush`. Their licence headers are intact. Every line this port
    changed or added is marked `UE PORT` (the `#include` line of each file) or `UE PORT PATCH` (three optional
    hooks in `CORTAOMainPass.ush`); the README lists them.
  - `Source/CORTAO/Private/CORTAOOccluderTable.inl`: generated from upstream's
    `tables/occluder/24x24x16x16_R16_UINT.bin` by `Tools/GenerateOccluderTable.py`.
  - `Shaders/Private/ThirdParty/LICENSE-CORTAO.txt`: upstream's `LICENSE`, unchanged.

```
MIT License

Copyright (c) 2026 Daniel Isheden

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
```

## Unreal Engine

The plugin builds against Unreal Engine and includes none of it. Unreal Engine is a trademark of Epic Games, Inc.
This project is not affiliated with or endorsed by Epic Games.

## If you ship a game with this plugin

Both MIT licences ask that their copyright and permission notices travel with copies of the software. Put the two
notices (Ananta Studio's from `LICENSE` and Daniel Isheden's above) in your game's third-party notices or credits.
The README has a short credit line you can paste.
