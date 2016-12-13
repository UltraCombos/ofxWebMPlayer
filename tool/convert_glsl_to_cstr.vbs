Set objFS = CreateObject("Scripting.FileSystemObject")
glslPath = WScript.Arguments(0)
glslFilename = WScript.Arguments(1)

If InStr(glslFilename, ".frag")> 0 Then
	glslType = "frag_"
	glslName = Replace(glslFilename, ".frag", "")
	
ElseIf InStr(WScript.Arguments(0),".vert")> 0 Then
	glslType = "vert_"
	glslName = Replace(glslFilename, ".vert", "")
	
Else
	glslType = "xxxx_"
	glslName = glslFilename
	
End If

Set objFile = objFS.OpenTextFile(glslPath)

WScript.Echo "static char const* g_cstr_" & glslType & glslName & " = R""("

Do Until objFile.AtEndOfStream
	strLine = objFile.ReadLine
    WScript.Echo strLine
Loop

WScript.Echo ")"";"
 