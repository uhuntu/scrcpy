@ctype mat4 hmm_mat4

@vs vs
in vec2 vertex;
out vec2 uv;

void main() {
    uv = vertex;
    gl_Position = vec4((vertex * 2.0 - 1.0) * vec2(1, -1), 0.0, 1.0);
}
@end

@fs fs
in vec2 uv;
out vec4 frag_color;

uniform sampler2D tex_y;
uniform sampler2D tex_cb;
uniform sampler2D tex_cr;

mat4 rec601 = mat4(
    1.16438,  0.00000,  1.59603, -0.87079,
    1.16438, -0.39176, -0.81297,  0.52959,
    1.16438,  2.01723,  0.00000, -1.08139,
    0, 0, 0, 1
);

void main() {
    float y = texture(tex_y, uv).r;
    float cb = texture(tex_cb, uv).r;
    float cr = texture(tex_cr, uv).r;

    frag_color = vec4(y, cb, cr, 1.0) * rec601;
}
@end

@program scrcpy vs fs
