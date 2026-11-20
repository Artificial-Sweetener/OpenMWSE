#version 120

uniform sampler2D sourceColor;
uniform vec4 targetPlane;
uniform float targetClipEnabled;

varying vec2 sourceUV;
varying float targetPlaneDistance;

void main()
{
    if (targetClipEnabled > 0.5 && targetPlaneDistance < -2.5)
        discard;
    gl_FragColor = texture2D(sourceColor, sourceUV);
}
