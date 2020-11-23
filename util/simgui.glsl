// D:\ikrima\src\personal\tolva\tools\sokol-tools-bin\sokol-shdc.exe --input BoneViz.glsl --output BoneViz.glsl.h --slang hlsl5 -f sokol_impl -e msvc
// D:\ikrima\src\personal\tolva\tools\sokol-tools-bin\sokol-shdc.exe -i simgui.glsl -o simgui.h -l glsl330:glsl100:hlsl4:metal_macos:metal_ios:metal_sim:wgpu -b

@module _simgui

@vs vs
uniform vs_params {
  vec4 disp_rect;
};
in vec2 position;
in vec2 texcoord0;
in vec4 color0;
out vec2 uv;
out vec4 color;
void main() {
  gl_Position = vec4((((position-disp_rect.xy)/disp_rect.zw)-0.5)*vec2(2.0,-2.0), 0.5, 1.0);
  uv = texcoord0;
  color = color0;
}
@end

@fs fs
uniform sampler2D tex;
in vec2 uv;
in vec4 color;
out vec4 frag_color;
void main() {
  frag_color = texture(tex, uv) * color;
}
@end

@program simgui vs fs
