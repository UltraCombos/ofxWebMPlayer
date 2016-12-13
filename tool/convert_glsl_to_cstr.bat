@SET path_tool=%1
@SET in_path=%2
@SET in_filnename=%3
@SET out_filename=%in_filnename%.h

@IF EXIST "%out_filename%" (
@DEL %out_filename%
)

@cscript /nologo %path_tool%convert_glsl_to_cstr.vbs %in_path% %in_filnename% > tmpFile_7878
@REN tmpFile_7878 %out_filename%
