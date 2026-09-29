struct VSInput
{
    float3 position : POSITION;
    float4 color : COLOR;
};

struct VSOutput
{
    float4 position : SV_POSITION;
    float4 color : COLOR;
};

VSOutput VSMain(VSInput input)
{
    VSOutput o;
    o.position = float4(input.position, 1.0f);
    o.color = input.color;
    return o;
}

float4 PSMain(VSOutput input) : SV_TARGET
{
    return input.color;
}