static char const* g_cstr_frag_shader709 = R"(

#version 150

in vec4 pixel_coord;

//x => y
//y => u
//z => v
//w => alpha

uniform vec4 v4_plane_width;
uniform vec4 v4_plane_height;
uniform vec2 v2_chroma_shift;

uniform sampler2D tex_y;
uniform sampler2D tex_u;
uniform sampler2D tex_v;

#ifdef USE_ALPHA_CHANNEL
uniform sampler2D tex_alpha;

#endif

out vec4 outputColor;

void main()
{   
	vec2 tex_y_coord = vec2(pixel_coord.x / v4_plane_width.x, pixel_coord.y / v4_plane_height.x);
	vec2 uv_coord = pixel_coord.xy * v2_chroma_shift;
	vec2 tex_uv_coord = vec2(uv_coord.x / v4_plane_width.y, uv_coord.y / v4_plane_height.y);
	
	vec3 yuv;
	yuv.x = texture(tex_y, tex_y_coord).x;
	yuv.y = texture(tex_u, tex_uv_coord).x;
	yuv.z = texture(tex_v, tex_uv_coord).x;
	
	vec3 ycbcr = vec3(yuv.x - 0.0625, yuv.y - 0.5, yuv.z - 0.5);
	vec3 rgb;
	rgb.x = clamp(dot(vec3(1.1644, 0.0, 	1.7927), 	ycbcr), 0.0, 1.0);
	rgb.y = clamp(dot(vec3(1.1644, -0.2133, -0.5329), 	ycbcr), 0.0, 1.0);
	rgb.z = clamp(dot(vec3(1.1644, 2.1124, 	0.0), 		ycbcr), 0.0, 1.0);
	
#ifdef USE_ALPHA_CHANNEL
	vec2 tex_alpha_coord = vec2(pixel_coord.x / v4_plane_width.w, pixel_coord.y / v4_plane_height.w);
	float alpha = texture(tex_alpha, tex_alpha_coord);
	
#else
	float alpha = 1.0;

#endif		
	
	outputColor = vec4(rgb.xyz, alpha);
}

)";
