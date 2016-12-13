@setlocal EnableDelayedExpansion
@SET path_tool=%~dp0
@SET in_folder=..\src\glsl
@CD %in_folder%

@REM filedrive=%%~di
@REM filepath=%%~pi
@REM filename=%%~ni
@REM fileextension=%%~xi

@REM Find out all files in the folder
@FOR /r %%i IN (*) DO @(
@CALL %path_tool%convert_glsl_to_cstr.bat %path_tool% %%i %%~ni%%~xi
@REM Creating the content of the "intern_shader.h"
@ECHO #include "%%~ni%%~xi.h" >> "%path_tool%tmptmptmp7878"
)

@XCOPY /Y *.h ..\shader\*.h
@DEL *.h

@XCOPY /Y "%path_tool%tmptmptmp7878" ..\shader\intern_shader.h
@DEL "%path_tool%tmptmptmp7878"

@PAUSE
