cbuffer CB : register(b0)
{
    float4x4 gMvp;
    float4x4 gWorld;
    float4x4 gLightVP;
    float3 gLightDir;
    float gAmbient;
    float3 gLightColor;
    float gShadowTexel;
    float gNormalBias;
    float gMetallic;
    float gRoughness;
    float gPad0;
    float3 gCamPos;
    float gPad1;
};

Texture2D gTex : register(t0);
Texture2D gShadow : register(t1);
Texture2D gNormalMap : register(t2);
SamplerState gSamp : register(s0);
SamplerComparisonState gShadowSamp : register(s1);

static const float kPi = 3.14159265f;

struct VSInput
{
    float3 position : POSITION;
    float3 normal : NORMAL;
    float2 uv : TEXCOORD;
};

struct VSOutput
{
    float4 position : SV_POSITION;
    float3 worldPos : TEXCOORD0;
    float3 normal : NORMAL;
    float2 uv : TEXCOORD1;
    float4 lightPos : TEXCOORD2;
};

float4 ShadowVS(VSInput input) : SV_POSITION
{
    float4 world = mul(float4(input.position, 1.0f), gWorld);
    return mul(world, gLightVP);
}

VSOutput VSMain(VSInput input)
{
    VSOutput o;
    float4 world = mul(float4(input.position, 1.0f), gWorld);
    float3 n = normalize(mul(input.normal, (float3x3)gWorld));
    o.position = mul(float4(input.position, 1.0f), gMvp);
    o.worldPos = world.xyz;
    o.normal = n;
    o.uv = input.uv;
    float4 shadowWorld = world;
    shadowWorld.xyz += n * gNormalBias;
    o.lightPos = mul(shadowWorld, gLightVP);
    return o;
}

float SampleShadow(float4 lightPos, float ndotl)
{
    float3 proj = lightPos.xyz / max(lightPos.w, 1e-6f);
    if (proj.z < 0.0f || proj.z > 1.0f)
        return 1.0f;

    float2 uv = float2(proj.x * 0.5f + 0.5f, 0.5f - proj.y * 0.5f);
    if (uv.x < 0.0f || uv.x > 1.0f || uv.y < 0.0f || uv.y > 1.0f)
        return 1.0f;

    float2 edge = smoothstep(0.00f, 0.04f, uv) * smoothstep(0.00f, 0.04f, 1.0f - uv);
    float fade = edge.x * edge.y;

    float bias = lerp(0.0008f, 0.006f, 1.0f - ndotl);
    float cmp = saturate(proj.z - bias);
    float shadow = 0.0f;
    [unroll]
    for (int y = -2; y <= 2; ++y)
    {
        [unroll]
        for (int x = -2; x <= 2; ++x)
        {
            shadow += gShadow.SampleCmpLevelZero(
                gShadowSamp, uv + float2(x, y) * gShadowTexel, cmp);
        }
    }
    shadow *= (1.0f / 25.0f);
    return lerp(1.0f, shadow, fade);
}

float3 PerturbNormal(float3 N, float3 worldPos, float2 uv)
{
    float3 map = gNormalMap.Sample(gSamp, uv).xyz * 2.0f - 1.0f;
    map.y = -map.y;

    float3 dp1 = ddx(worldPos);
    float3 dp2 = ddy(worldPos);
    float2 duv1 = ddx(uv);
    float2 duv2 = ddy(uv);
    float3 dp2perp = cross(dp2, N);
    float3 dp1perp = cross(N, dp1);
    float3 T = dp2perp * duv1.x + dp1perp * duv2.x;
    float3 B = dp2perp * duv1.y + dp1perp * duv2.y;
    float invMax = rsqrt(max(dot(T, T), dot(B, B)));
    T *= invMax;
    B *= invMax;
    return normalize(T * map.x + B * map.y + N * map.z);
}

float D_GGX(float NdotH, float a)
{
    float a2 = a * a;
    float d = (NdotH * NdotH) * (a2 - 1.0f) + 1.0f;
    return a2 / max(kPi * d * d, 1e-6f);
}

float G_Smith(float NdotV, float NdotL, float roughness)
{
    float r = roughness + 1.0f;
    float k = (r * r) / 8.0f;
    float gV = NdotV / max(NdotV * (1.0f - k) + k, 1e-6f);
    float gL = NdotL / max(NdotL * (1.0f - k) + k, 1e-6f);
    return gV * gL;
}

float3 F_Schlick(float cosTheta, float3 F0)
{
    return F0 + (1.0f - F0) * pow(saturate(1.0f - cosTheta), 5.0f);
}

float3 F_SchlickRoughness(float cosTheta, float3 F0, float roughness)
{
    return F0 + (max(1.0f - roughness, F0) - F0) * pow(saturate(1.0f - cosTheta), 5.0f);
}

float4 PSMain(VSOutput input) : SV_TARGET
{
    float4 tex = gTex.Sample(gSamp, input.uv);
    clip(tex.a - 0.4f);

    float3 albedo = tex.rgb;
    float metallic = saturate(gMetallic);
    float roughness = clamp(gRoughness, 0.045f, 1.0f);

    float3 N = PerturbNormal(normalize(input.normal), input.worldPos, input.uv);
    float3 V = normalize(gCamPos - input.worldPos);
    float3 L = normalize(gLightDir);
    float3 H = normalize(V + L);

    float NdotL = saturate(dot(N, L));
    float NdotV = saturate(dot(N, V));
    float NdotH = saturate(dot(N, H));
    float HdotV = saturate(dot(H, V));

    float shadow = 1.0f;
    if (NdotL > 0.0f)
        shadow = SampleShadow(input.lightPos, NdotL);
    shadow = lerp(1.0f, shadow, saturate(NdotL * 6.0f));

    float3 F0 = lerp(float3(0.04f, 0.04f, 0.04f), albedo, metallic);
    float a = roughness * roughness;
    float D = D_GGX(NdotH, a);
    float G = G_Smith(NdotV, NdotL, roughness);
    float3 F = F_Schlick(HdotV, F0);

    float3 spec = (D * G * F) / max(4.0f * NdotV * NdotL, 1e-4f);
    float3 kS = F;
    float3 kD = (1.0f - kS) * (1.0f - metallic);
    float3 diffuse = kD * albedo / kPi;
    float3 radiance = gLightColor;
    float3 lit = (diffuse + spec) * radiance * NdotL * shadow;

    float3 Famb = F_SchlickRoughness(NdotV, F0, roughness);
    float3 kDamb = (1.0f - Famb) * (1.0f - metallic);
    float3 ambient = (kDamb * albedo + Famb * 0.12f) * gAmbient;
    return float4(ambient + lit, 1.0f);
}
