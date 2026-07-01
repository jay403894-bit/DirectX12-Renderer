// Define a structure for what goes from Vertex Shader to Pixel Shader
struct PS_INPUT
{
    float4 pos : SV_POSITION;
    float4 color : COLOR;
};

// Vertex Shader
PS_INPUT VSMain(float3 pos : POSITION, float4 color : COLOR)
{
    PS_INPUT result;
    result.pos = float4(pos, 1.0f);
    result.color = color;
    return result;
}

// Pixel Shader
float4 PSMain(PS_INPUT input) : SV_TARGET
{
    return input.color;
}