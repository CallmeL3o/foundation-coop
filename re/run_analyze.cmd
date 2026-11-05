@echo off
set "JAVA_HOME=E:\FoundationCoopTest\tools\jdk-25.0.5+8"
set "PATH=%JAVA_HOME%\bin;%PATH%"
set "GHIDRA_HEADLESS_MAXMEM=12G"
call "E:\FoundationCoopTest\tools\ghidra_12.2_PUBLIC\support\analyzeHeadless.bat" E:\FoundationCoopTest\re\proj foundation -import E:\FoundationCoopTest\re\foundation.exe -max-cpu 10 -analysisTimeoutPerFile 10800 -scriptPath E:\FoundationCoopTest\re\scripts -postScript DumpByStrings.java
