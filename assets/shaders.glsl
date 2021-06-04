@vs vs
in vec2 position;
out vec2 vxPosition;

uniform vs_params
{
    mat4 projection;
    mat4 transform;
    mat4 transformLocal;
};

void main() {
    vec4 p      = vec4(position, 0.0, 1.0);
    gl_Position = projection * transform * p;
    vxPosition  = (transformLocal * p).xy;
}
@end

@fs fs

#define FILL_TYPE_NONE   0
#define FILL_TYPE_SOLID  1
#define FILL_TYPE_LINEAR 2
#define FILL_TYPE_RADIAL 3
#define MAX_STOPS        16

uniform fs_paint
{
    vec4  colors[MAX_STOPS];
    vec4  stops[MAX_STOPS];
    vec2  gradientStart;
    vec2  gradientStop;
    float fillType;
    float stopCount;
};

in vec2  vxPosition;
out vec4 fragColor;

void main()
{
    int iStopCount = int(stopCount);
    int iFillType  = int(fillType);

    if (iFillType == FILL_TYPE_SOLID)
    {
        fragColor = vec4(colors[0].rgb * colors[0].a, colors[0].a);
    }
    else if (iFillType == FILL_TYPE_LINEAR)
    {
        vec2 toEnd          = gradientStop - gradientStart;
        float lengthSquared = toEnd.x * toEnd.x + toEnd.y * toEnd.y;
        float f             = dot(vxPosition - gradientStart, toEnd) / lengthSquared;
        vec4 color          = mix(colors[0], colors[1],
            smoothstep(stops[0].x, stops[1].x, f));

        for (int i=1; i < MAX_STOPS; ++i)
        {
            if(i >= iStopCount-1)
            {
                break;
            }
            color = mix(color, colors[i+1], smoothstep( stops[i].x, stops[i+1].x, f ));
        }

        fragColor = vec4(color.xyz * color.w, color.w);
    }
    else if (iFillType == FILL_TYPE_RADIAL)
    {
        float f    = distance(gradientStart, vxPosition)/distance(gradientStart, gradientStop);
        vec4 color = mix(colors[0], colors[1],
            smoothstep(stops[0].x, stops[1].x, f));

        for (int i=1; i < MAX_STOPS; ++i)
        {
            if(i >= iStopCount-1)
            {
                break;
            }
            color = mix(color, colors[i+1], smoothstep( stops[i].x, stops[i+1].x, f ));
        }

        fragColor = vec4(color.xyz * color.w, color.w);
    }
    else
    {
        fragColor = vec4(vec3(0.0), 1.0);
    }
}
@end

@fs debug_contour
in vec2  vxPosition;
out vec4 fragColor;

uniform fs_contour
{
    vec4 color;
    vec4 solidColor;
};

void main()
{
    vec3 colorOut = mix(color.rgb, solidColor.rgb, solidColor.a);
    fragColor = vec4(colorOut.rgb, 1.0);
}
@end

@program rive_shader        vs fs
@program rive_debug_contour vs debug_contour


