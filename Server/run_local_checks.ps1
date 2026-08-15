<#
run_local_checks.ps1
Helper script to verify the FastAPI/uvicorn server is reachable locally and from the LAN.

Usage:
  - Open PowerShell (recommended: Run as Administrator for firewall checks)
  - cd to the Server folder and run: .\run_local_checks.ps1

The script prints network interfaces, netstat listening ports, process owning port 8000,
does quick Test-NetConnection and attempts HTTP GET on /health for localhost and the LAN IP.
#>

Write-Host "== Run local checks for Smart Car Access server ==`n"

Write-Host "[1] Network interfaces (ipconfig):`n"
ipconfig

Write-Host "`n[2] Listening on port 8000 (netstat):`n"
netstat -ano | findstr ":8000"

Write-Host "`n[3] Process owning port 8000 (if any):`n"
try {
    $conn = Get-NetTCPConnection -LocalPort 8000 -ErrorAction Stop
    $pid = $conn.OwningProcess
    Write-Host "Found TCP connection, PID = $pid"
    Get-Process -Id $pid | Select-Object Id,ProcessName
} catch {
    Write-Host "No NetTCPConnection found for port 8000 or insufficient privileges."
}

Write-Host "`n[4] Test-NetConnection (localhost):`n"
Test-NetConnection -ComputerName 127.0.0.1 -Port 8000 -InformationLevel Detailed

Write-Host "`n[5] Test-NetConnection (LAN IP) - replace <LAN_IP> if different:`n"
# Attempt to find likely LAN IP reported by server mDNS (10.2.0.2). If you have another IP, edit below.
$lanIp = (Get-NetIPAddress -AddressFamily IPv4 -InterfaceAlias 'Wi-Fi' -ErrorAction SilentlyContinue | Where-Object {$_.IPAddress -ne '127.0.0.1'}).IPAddress
if (-not $lanIp) { $lanIp = (Get-NetIPAddress -AddressFamily IPv4 | Where-Object {$_.IPAddress -like '10.*' -or $_.IPAddress -like '192.*' -or $_.IPAddress -like '172.*'} | Select-Object -First 1 -ExpandProperty IPAddress) }
if (-not $lanIp) { $lanIp = '10.2.0.2' }
Write-Host "Using LAN IP: $lanIp`n"
Test-NetConnection -ComputerName $lanIp -Port 8000 -InformationLevel Detailed

Write-Host "`n[6] HTTP GET /health (localhost):`n"
try {
    $r = Invoke-WebRequest -Uri http://127.0.0.1:8000/health -UseBasicParsing -TimeoutSec 5 -ErrorAction Stop
    Write-Host "HTTP/localhost status:" $r.StatusCode
    Write-Host "Body:`n" $r.Content
} catch {
    Write-Host "Request to http://127.0.0.1:8000/health failed: $_"
}

Write-Host "`n[7] HTTP GET /health (LAN IP):`n"
try {
    $r2 = Invoke-WebRequest -Uri ("http://$lanIp:8000/health") -UseBasicParsing -TimeoutSec 5 -ErrorAction Stop
    Write-Host "HTTP/LAN status:" $r2.StatusCode
    Write-Host "Body:`n" $r2.Content
} catch {
    Write-Host "Request to http://$lanIp:8000/health failed: $_"
}

Write-Host "`n[8] Quick firewall check (netsh) - look for rules mentioning 8000:`n"
netsh advfirewall firewall show rule name=all | findstr /I "8000"

Write-Host "`nDone. If remote device cannot reach http://$lanIp:8000, try these steps:" -ForegroundColor Yellow
Write-Host " - Ensure Windows Firewall allows inbound on port 8000 or temporarily disable firewall to test." -ForegroundColor Yellow
Write-Host " - From remote device, run: curl http://$lanIp:8000/health" -ForegroundColor Yellow
Write-Host " - If laptop is on VPN, disconnect VPN when testing LAN access." -ForegroundColor Yellow

Write-Host "`nPaste output here if you want help interpreting results."
