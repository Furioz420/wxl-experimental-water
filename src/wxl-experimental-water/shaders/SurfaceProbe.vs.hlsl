// Diagnostic only. Same world-to-clip transform as Surface.vs.hlsl, no waves,
// texture fetches or extra interpolators. Pair only with SurfaceProbe.ps.hlsl.
// Compile: fxc /T vs_3_0 /E main /Fh SurfaceProbeVs.h /Vn kSurfaceProbeVs SurfaceProbe.vs.hlsl
float4x4 gProj : register(c0);
float4x4 gView : register(c5);
float4 main(float4 position : POSITION) : POSITION
{
    return mul(gProj, mul(gView, float4(position.xyz, 1.0)));
}
