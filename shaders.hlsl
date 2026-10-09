cbuffer CB : register(b0)
{
    float4x4 gMvp;
    float4x4 gWorld;
    float3 gLightDir;
    float gAmbient;
};

Texture2D gTex : register(t0);
SamplerState gSamp : register(s0);

struct VSInput
{
    float3 position : POSITION;
    float3 normal : NORMAL;
    float2 uv : TEXCOORD;
};

struct VSOutput
{
    float4 position : SV_POSITION;
    float3 normal : NORMAL;
    float2 uv : TEXCOORD;
};

VSOutput VSMain(VSInput input)
{
    VSOutput o;
    o.position = mul(float4(input.position, 1.0f), gMvp);
    o.normal = mul(input.normal, (float3x3)gWorld);
    o.uv = input.uv;
    return o;
}

float4 PSMain(VSOutput input) : SV_TARGET
{
    float3 n = normalize(input.normal);
    float3 l = normalize(gLightDir);
    float ndotl = saturate(dot(n, l));
    float3 albedo = gTex.Sample(gSamp, input.uv).rgb;
    float3 color = albedo * (gAmbient + ndotl);
    return float4(color, 1.0f);
}
