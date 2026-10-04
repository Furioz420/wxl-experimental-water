// Diagnostic only: isolate pixel shading while retaining the stock-water vertex
// shader, geometry, draw count, texture bindings and render-state restoration.
// Compile: fxc /T ps_3_0 /E main /Fh SurfaceProbePs.h /Vn kSurfaceProbePs SurfaceProbe.ps.hlsl
float4 main() : COLOR0
{
    return float4(0.07, 0.19, 0.22, 1.0);
}
