// license:GPLv3+

$input a_position, a_normal, a_texcoord0
$output v_color0, v_texcoord0, v_texcoord1

#include "common.sh"

#ifdef STEREO
    // One matrix per eye: identical for stereo displays (depth given by stereoOfs), or a projection of a panel placed in the room for VR
    uniform mat4 matWorldView[2];
#else
    uniform mat4 matWorldView;
#endif

uniform vec4 staticColor_Alpha;
#define stereoOfs staticColor_Alpha.x
#define sdrScale staticColor_Alpha.a

void main()
{
    v_texcoord1 = a_position.xy;

    vec4 ofsPos = vec4(a_position.xy, 0.0, 1.0);
    #ifdef STEREO
        if (gl_InstanceID == 0)
            ofsPos.x += stereoOfs;
        else
            ofsPos.x -= stereoOfs;
        gl_Layer = gl_InstanceID;
        vec4 pos = mul(matWorldView[gl_InstanceID], ofsPos);
    #else
        vec4 pos = mul(matWorldView, ofsPos);
    #endif
    // w is kept for the perspective of a panel placed in the room (it is 1 for the orthographic projection of the screen)
    gl_Position = vec4(pos.x, pos.y, 0.0, pos.w);

    v_texcoord0 = a_texcoord0;

    v_color0 = vec4(sdrScale * a_normal.rgb, a_position.z);
}
