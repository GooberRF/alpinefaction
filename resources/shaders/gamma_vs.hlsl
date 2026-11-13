struct VsOutput
{
    float4 pos : SV_POSITION;
    float2 uv : TEXCOORD0;
};

VsOutput main(uint vid : SV_VertexID)
{
    VsOutput output;
    // Generate a fullscreen triangle from vertex ID (0, 1, 2)
    output.uv = float2((vid << 1) & 2, vid & 2);
    output.pos = float4(output.uv * float2(2, -2) + float2(-1, 1), 0, 1);
    return output;
}
