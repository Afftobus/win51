# Stops and removes the win51svc service and its files; the log in ProgramData\Win51 stays.
$ErrorActionPreference = 'Stop'
$name = 'win51svc'
if (Get-Service $name -ErrorAction SilentlyContinue) {
    Stop-Service $name
    sc.exe delete $name | Out-Null
}
Remove-Item -Recurse -Force "$env:ProgramFiles\Win51" -ErrorAction SilentlyContinue
