#ifndef LIB_CORE_INSTANCE_H
#define LIB_CORE_INSTANCE_H

@link "lib/core/instance.glsl" if @gpuInstanced
@link "lib/core/instance_identity.glsl" if !@gpuInstanced

mat4 instanceWorld();
mat3 instanceNormal();
vec4 instanceTangent(vec4 tangent);
vec3 instanceWorldOffset(vec3 nodePosition);

#endif
