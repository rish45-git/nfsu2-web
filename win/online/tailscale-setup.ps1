# tailscale-setup.ps1 -- the local nfsu2 server (in WSL, on 127.0.0.1:8691) for friends over Tailscale.
# Run once on the server's PC, in PowerShell as administrator, after Tailscale is installed and signed in:
#
#   powershell -ExecutionPolicy Bypass -File win\online\tailscale-setup.ps1
#
# It adds, beside the LAN's (PROGRESS.md, phase 4):
#   - a port proxy <this PC's Tailscale IPv4>:8691 -> 127.0.0.1:8691 (WSL's server answers on loopback only);
#   - a firewall rule "NFSU2 server (Tailscale)": inbound TCP 8691 from Tailscale's addresses (100.64.0.0/10) only.
# Then the server needs that address among its names (NFSU2_HOST=192.168.0.109,<it>), and the friends'
# Server = ws://<it>:8691/lan.
# Undo:
#   netsh interface portproxy delete v4tov4 listenaddress=<it> listenport=8691
#   Remove-NetFirewallRule -DisplayName "NFSU2 server (Tailscale)"
$ErrorActionPreference = 'Stop'
$port = 8691
$ts = Join-Path $env:ProgramFiles 'Tailscale\tailscale.exe'
if (-not (Test-Path $ts)) { throw "Tailscale is not installed ($ts): install it from https://tailscale.com/download and sign in first" }
$ip = (& $ts ip -4 | Select-Object -First 1).Trim()
if ($ip -notmatch '^100\.') { throw "no Tailscale IPv4 address ('$ip'): is Tailscale signed in and connected?" }
$id = [Security.Principal.WindowsIdentity]::GetCurrent()
if (-not (New-Object Security.Principal.WindowsPrincipal $id).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
    throw "run this in PowerShell as administrator"
}
netsh interface portproxy delete v4tov4 listenaddress=$ip listenport=$port 2>$null | Out-Null
netsh interface portproxy add v4tov4 listenaddress=$ip listenport=$port connectaddress=127.0.0.1 connectport=$port | Out-Null
if (-not (Get-NetFirewallRule -DisplayName 'NFSU2 server (Tailscale)' -ErrorAction SilentlyContinue)) {
    New-NetFirewallRule -DisplayName 'NFSU2 server (Tailscale)' -Direction Inbound -Protocol TCP -LocalPort $port `
        -RemoteAddress '100.64.0.0/10' -Action Allow -Profile Any | Out-Null
}
Write-Host "Tailscale address: $ip"
Write-Host "Port proxy $($ip):$port -> 127.0.0.1:$port and firewall rule 'NFSU2 server (Tailscale)' are in."
Write-Host "Next: the server with NFSU2_HOST=192.168.0.109,$ip; friends' Server = ws://$($ip):$port/lan"
netsh interface portproxy show v4tov4
