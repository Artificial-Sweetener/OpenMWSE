#version 120

uniform mat4 osg_ViewMatrixInverse;

attribute vec4 aOffset;
attribute vec3 aRotation;

mat4 rotation(in vec3 angle)
{
    float sin_x = sin(angle.x);
    float cos_x = cos(angle.x);
    float sin_y = sin(angle.y);
    float cos_y = cos(angle.y);
    float sin_z = sin(angle.z);
    float cos_z = cos(angle.z);

    return mat4(
        cos_z*cos_y+sin_x*sin_y*sin_z, -sin_z*cos_x, cos_z*sin_y+sin_z*sin_x*cos_y, 0.0,
        sin_z*cos_y+cos_z*sin_x*sin_y, cos_z*cos_x, sin_z*sin_y-cos_z*sin_x*cos_y, 0.0,
        -sin_y*cos_x, sin_x, cos_x*cos_y, 0.0,
        0.0, 0.0, 0.0, 1.0);
}

mat3 rotation3(in mat4 rot4)
{
    return mat3(
        rot4[0].xyz,
        rot4[1].xyz,
        rot4[2].xyz);
}

// World placement of a groundcover instance.
mat4 instanceWorld()
{
    mat4 placement = rotation(aRotation) * mat4(aOffset.w);
    placement[3] = vec4(aOffset.xyz, 1.0);
    return osg_ViewMatrixInverse * gl_ModelViewMatrix * placement;
}

mat3 instanceNormal()
{
    return rotation3(rotation(aRotation));
}

vec4 instanceTangent(vec4 tangent)
{
    return tangent * rotation(aRotation);
}
