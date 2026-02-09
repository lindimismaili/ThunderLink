/*
 * ThunderLink - Fullscreen Triangle Vertex Shader
 * Generates a fullscreen triangle from SV_VertexID (no vertex buffer needed).
 * Draw with 3 vertices, 1 instance.
 */

struct VSOutput {
    float4 position : SV_POSITION;
    float2 texcoord : TEXCOORD0;
};

VSOutput main(uint vertexID : SV_VertexID) {
    VSOutput output;

    /* Fullscreen triangle covering [-1,1] clip space:
     *   vertexID 0: (-1, -1) -> uv (0, 1)
     *   vertexID 1: (-1,  3) -> uv (0, -1)  [clipped to top]
     *   vertexID 2: ( 3, -1) -> uv (2, 1)   [clipped to right]
     */
    output.texcoord = float2((vertexID << 1) & 2, vertexID & 2);
    output.position = float4(
        output.texcoord.x * 2.0 - 1.0,
        1.0 - output.texcoord.y * 2.0,
        0.0, 1.0
    );

    return output;
}
