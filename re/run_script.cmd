@echo off
rem usage: run_script.cmd <Script.java> -- runs a script on the already analysed program (no re-analysis)
set "JAVA_HOME=E:\FoundationCoopTest\tools\jdk-25.0.5+8"
set "PATH=%JAVA_HOME%\bin;%PATH%"
set "GHIDRA_HEADLESS_MAXMEM=12G"
call "E:\FoundationCoopTest\tools\ghidra_12.2_PUBLIC\support\analyzeHeadless.bat" E:\FoundationCoopTest\re\proj foundation -process foundation.exe -noanalysis -scriptPath E:\FoundationCoopTest\re\scripts -postScript %1
