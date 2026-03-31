cbuffer OutlineColor : register(b2)
{
    float4 outline_color;
};

float4 main() : SV_TARGET
{
    return outline_color;
}
