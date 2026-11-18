# Changelog

## 1.0.0 (2026-11-18)

First public release. Tracks CORTAO reference implementation **1.0.3**, for Unreal Engine **5.8**.

- CORTAO's prepass, main pass and filter pass as RDG compute passes on the engine's global illumination plugin
  hook; the result replaces the content of the engine's `ScreenSpaceAO` texture. No engine changes.
- A temporal accumulation pass of the port's own (`r.CORTAO.Temporal`), with reprojection, a depth test and an
  optional neighbourhood clamp.
- Normals from the GBuffer or rebuilt from depth (`r.CORTAO.DepthNormals`).
- Foliage handling for stylised grass and leaf cards: a foliage flag in the packed buffer, plane rejection, porous
  foliage occluders and no slope compensation for them, through three optional hooks added to
  `CORTAOMainPass.ush`.
- `CORTAOFoliage` module: marks two-sided masked static meshes with a lighting channel so that the renderer can
  tell them apart, while `r.CORTAO` is on. Settings in Project Settings > Plugins > CORTAO Foliage.
- Log lines when `r.CORTAO` is on but cannot render (the Global Illumination method is not Plugin, or the engine
  produced no `ScreenSpaceAO` texture).
- `Tools/GenerateOccluderTable.py` regenerates the compiled-in occluder table from upstream's `.bin`.
