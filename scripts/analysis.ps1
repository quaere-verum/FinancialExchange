# Get the directory of the script (\scripts) and then get its parent (Project Root)
$ScriptDir = Split-Path -Parent $MyInvocation.MyCommand.Definition
$ProjectRoot = Split-Path -Parent $ScriptDir

# Move the script's focus to the Project Root
Set-Location $ProjectRoot

Write-Host "Working in: $ProjectRoot" -ForegroundColor Cyan

# 1. Clear Directories (Relative to project root)
# If your folders are named 'bin' and 'logs' inside the project:
Remove-Item -Path ".\out\*" -Recurse -Force
Remove-Item -Path ".\logs\*" -Recurse -Force

# 2. Start Processes
# Use '.\' to indicate the current folder
$processToKill = Start-Process -FilePath ".\build\apps\exchange\FinancialExchange.exe" -PassThru
Start-Process -FilePath ".\build\apps\market_simulator\MarketSimulator.exe"

# 3. Wait (Time passed as an argument, defaults to 30s)
$timeout = if($args[0]) { $args[0] } else { 30 }
Start-Sleep -Seconds $timeout

# 4. Terminate the first process
Stop-Process -Id $processToKill.Id -Force

# 5. Run Python script
python ".\python\scripts\log_parsing.py"