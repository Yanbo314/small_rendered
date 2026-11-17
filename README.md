# small_rendered

Real-time Direct3D 11 renderer by **Yanbo Liu**.

The project starts from a triangle and currently loads **Crytek Sponza**, with physically based shading, a directional shadow map, MSAA, and a small CPU-side constant-buffer ring.

Repository: [https://github.com/Yanbo314/small_rendered](https://github.com/Yanbo314/small_rendered)

## Features

- Direct3D 11, HLSL Shader Model 5.0, Win32 window
- Free-look camera (click to capture mouse)
- Crytek Sponza (OBJ) with per-material diffuse and tangent-space normal maps
- Cook–Torrance PBR (GGX, Schlick Fresnel, Smith geometry)
- One directional light, 4096² shadow map, 5×5 PCF
- 4× MSAA resolve into the swap chain
- Anisotropic filtering (16×) and CPU-generated mipmaps
- Two-frame dynamic constant-buffer ring (`Map` once per frame, bind by 256-byte slot)

## Requirements

- Windows 10 or later
- Visual Studio 2022 (Desktop development with C++)
- Windows 10 SDK (project uses `WindowsTargetPlatformVersion` 10.0)
- GPU with Direct3D Feature Level 11_0
- Working directory = project directory (`$(ProjectDir)`), so `shaders.hlsl` and `Assets/` resolve at runtime

## Getting Sponza

Sponza textures are not in this repository (large binary assets). Place the OBJ package here:

```text
Assets/sponza/sponza.obj
Assets/sponza/sponza.mtl
Assets/sponza/textures/*.tga
```

Clone (example, Frank Meinl / Crytek OBJ with TGA maps):

```bat
git clone --depth 1 https://github.com/jimmiebergmann/Sponza.git Assets/sponza
```

Scale in code is `0.01` (OBJ centimeters to meters).

## Build and run

1. Open `triangle.sln` in Visual Studio 2022.
2. Set configuration **Debug | x64** or **Release | x64**.
3. Confirm **Working Directory** is `$(ProjectDir)` (already set for x64).
4. Build and run (F5). First launch parses the OBJ and uploads textures; wait a few seconds.

Command line:

```bat
msbuild triangle.sln /p:Configuration=Debug /p:Platform=x64
cd /d path\to\repo
x64\Debug\triangle.exe
```

## Controls

| Input | Action |
| --- | --- |
| Left mouse | Capture look |
| Mouse move | Yaw / pitch |
| Esc | Release mouse |
| W A S D or arrows | Move |
| Space / Ctrl | Up / down |
| Shift | Faster move |

## Frame graph

1. Fit an orthographic light volume to the scene AABB.
2. **Shadow pass:** depth-only, front-face cull, slope-scaled depth bias, `ShadowVS`.
3. **Scene pass:** MSAA color + depth, PBR pixel shader, albedo (sRGB) + normal (linear) + shadow comparison sampler.
4. `ResolveSubresource` into the swap-chain back buffer, then `Present`.

Dynamic constants (view, light VP, per-mesh metallic/roughness, camera position) are written into `cbRing_[frame % 2]` with a bump allocator, unmapped, then bound with `VSSetConstantBuffers1` / `PSSetConstantBuffers1`.

## Project layout

```text
Source.cpp          Win32 + D3D11 renderer
shaders.hlsl        ShadowVS, VSMain, PSMain
triangle.sln        Visual Studio solution
third_party/        tinyobjloader, stb_image
Assets/sponza/      local scene data (gitignored)
brick.jpg, ground.jpg   leftover demo textures
```

## Notes

- Crytek Sponza is not a glTF PBR pack. Metallic and roughness are estimated from material names; there is no IBL.
- This renderer is **Direct3D 11**, not 12.
- `Source.cpp` is a single translation unit on purpose (learning / interview walkthrough).

## Credits

- **Sponza** atrium: Frank Meinl, Crytek. Typical redistribution is CC BY 3.0. Cite the asset if you ship screenshots.
- **tinyobjloader** — Syoyo Fujita (MIT)
- **stb_image** — Sean Barrett (MIT / public domain)

## Author

Yanbo Liu
