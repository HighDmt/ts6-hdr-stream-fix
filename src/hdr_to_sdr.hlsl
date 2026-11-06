// HDR (scRGB) desktop frame -> SDR.
//
// Input:  R16G16B16A16_FLOAT, scRGB = linear light, BT.709 primaries, 1.0 = 80 nits.
//         SDR content (windows, text) sits at "SDR white" = sdrWhiteNits / 80, e.g. 3.25.
// Output: BGRA8 sRGB-encoded (g_Mode bit 1 set) or linear FP16 with 1.0 = SDR white.

cbuffer Params : register(b0)
{
    float g_Scale;  // 80 / sdrWhiteNits: SDR white becomes exactly 1.0
    float g_Knee;   // the shoulder starts here (in SDR-white units, < 1)
    float g_InvW2;  // 1 / W^2, W = HDR peak position in shoulder space (see Shoulder)
    uint  g_Mode;   // bit 0: shoulder on (else hard clip), bit 1: sRGB-encode output
};

Texture2D<float4> g_Src : register(t0);

// Fullscreen triangle generated from SV_VertexID: no vertex buffer, no input layout.
float4 VSMain(uint id : SV_VertexID) : SV_Position
{
    float2 uv = float2((id << 1) & 2, id & 2);
    return float4(uv * float2(2, -2) + float2(-1, 1), 0, 1);
}

// Highlight roll-off on m = max(R,G,B) >= knee.
// x is m rescaled so the knee is at 0 and 1.0 output is at 1. Extended Reinhard
// y = x (1 + x / W^2) / (1 + x) has slope 1 at x = 0 (joins the identity part
// smoothly), is monotonic, and reaches exactly 1 at x = W (the display peak).
float Shoulder(float m)
{
    float x = (m - g_Knee) / (1.0 - g_Knee);
    float y = min(x * (1.0 + x * g_InvW2) / (1.0 + x), 1.0);
    return g_Knee + y * (1.0 - g_Knee);
}

float3 SrgbOetf(float3 c)
{
    return c <= 0.0031308 ? c * 12.92 : 1.055 * pow(abs(c), 1.0 / 2.4) - 0.055; // c >= 0 here
}

float4 PSMain(float4 pos : SV_Position) : SV_Target
{
    // Scale so SDR white = 1.0; negative (out-of-BT.709-gamut) values are clamped.
    float3 c = max(g_Src.Load(int3(pos.xy, 0)).rgb * g_Scale, 0.0);

    // Tone-map on max(R,G,B) and scale all channels by the same ratio: keeps hue.
    float m = max(c.r, max(c.g, c.b));
    if ((g_Mode & 1) != 0 && m > g_Knee)
        c *= Shoulder(m) / m;
    c = min(c, 1.0);

    if ((g_Mode & 2) != 0)
        c = SrgbOetf(c);
    return float4(c, 1.0);
}
