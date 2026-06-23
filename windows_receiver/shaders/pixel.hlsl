/*
 * ThunderLink - Frame Display Pixel Shader (NV12 -> RGB)
 * Samples the decoded Y (t0, R8) and UV (t1, R8G8) planes and converts
 * BT.709 limited-range YCbCr to RGB. Must match the encoder's color settings
 * on the Mac (kCVImageBufferYCbCrMatrix_ITU_R_709_2, video range).
 */

Texture2D<float>  texY  : register(t0);
Texture2D<float2> texUV : register(t1);
SamplerState      samp  : register(s0);

float4 main(float4 position : SV_POSITION, float2 texcoord : TEXCOORD0) : SV_TARGET {
    // Limited ("video") range: Y in [16,235], C in [16,240] (8-bit).
    float  y  = texY.Sample(samp, texcoord);
    float2 uv = texUV.Sample(samp, texcoord);

    float Y = (y - 16.0 / 255.0) * (255.0 / 219.0);
    float U = (uv.x - 128.0 / 255.0) * (255.0 / 224.0);
    float V = (uv.y - 128.0 / 255.0) * (255.0 / 224.0);

    // BT.709 YCbCr -> RGB
    float r = Y + 1.5748 * V;
    float g = Y - 0.1873 * U - 0.4681 * V;
    float b = Y + 1.8556 * U;

    return float4(saturate(float3(r, g, b)), 1.0);
}
