struct Instance
{
    vec4 row0;
    vec4 row1;
    vec4 row2;
    vec4 bounds;
    vec4 lod;
    uvec4 meta;
    uvec4 material;
};

struct Batch
{
    uint firstIndex;
    uint indexCount;
    int baseVertex;
    uint firstSlot;
    uint command;
    uint pad0;
    uint pad1;
    uint pad2;
};

struct View
{
    vec4 planes[16];
    vec4 pixelSizeVector;
    vec4 viewPoint;
    vec4 params;
    uvec4 meta;
    mat4 viewProj;
};
