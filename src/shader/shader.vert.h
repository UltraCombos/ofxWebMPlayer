static char const* g_cstr_vert_shader = R"(

#version 150

uniform mat4 mat4_projection;
uniform mat4 mat4_model_view;

in vec4 position;
out vec4 pixel_coord;

void main()
{
	pixel_coord = position;
	gl_Position = mat4_projection * mat4_model_view * position;
}

)";
