static char const* g_cstr_frag_shaderARGB = R"(
#version 150

in vec4 pixel_coord;

uniform vec4 v4_plane_width;
uniform vec4 v4_plane_height;

uniform sampler2D tex_y;

out vec4 outputColor;

void main()
{   
	vec2 tex_y_coord = vec2(pixel_coord.x / v4_plane_width.x, pixel_coord.y / v4_plane_height.x);	
	outputColor = texture(tex_y, tex_y_coord).yxwz;
}
)";
