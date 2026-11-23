# NFSU2 online: host a game for your friends on this PC, or join a friend's.
#
#   Play Online.bat                the menu
#   Play Online.bat host           start the server on this PC, point the game at it, start the game
#   Play Online.bat join <address> point the game at a friend's server (their address, as their window showed it)
#   Play Online.bat play           start the game as it was set last time
#   Play Online.bat stop           stop the server on this PC and close the router's port again
#   Play Online.bat check          hosting, looked at only: the addresses, the router, the firewall (changes nothing)
#
# This file and nfsu2-server.exe sit in the game folder's "online" folder; the game is one folder up.  The server
# (nfsu2-server.exe --friends) answers only the game's plugin: no web page, and none of the game's files.  Its own
# files (who is on, logs) are in %LOCALAPPDATA%\NFSU2-online-server.
param([string]$Mode = "", [string]$Address = "")

$ErrorActionPreference = "Stop"
$Port    = 8691
$Here    = Split-Path -Parent $MyInvocation.MyCommand.Path
$Game    = Split-Path -Parent $Here
$GameExe = Join-Path $Game "SPEED2.EXE"
$Ini     = Join-Path $Game "scripts\NFSU2FreeRoam.ini"
$Server  = Join-Path $Here "nfsu2-server.exe"
$State   = Join-Path $env:LOCALAPPDATA "NFSU2-online-server"
$Rule    = "NFSU2 online server"

function Say($text, $colour = "Gray") { Write-Host $text -ForegroundColor $colour }

# ---- the plugin's settings file: its Server line ----------------------------------------------------------------
function Get-IniServer {
    if (-not (Test-Path $Ini)) { return "" }
    foreach ($l in [IO.File]::ReadAllLines($Ini, [Text.Encoding]::Default)) {
        if ($l -match '^[ \t]*Server[ \t]*=[ \t]*(.*?)[ \t]*$') { return $Matches[1] }
    }
    return ""
}
function Set-IniServer($url) {
    if (-not (Test-Path $Ini)) { throw "No $Ini. This launcher belongs in the 'online' folder of a game folder that has the plugin in 'scripts'." }
    $enc = [Text.Encoding]::Default
    $lines = [IO.File]::ReadAllLines($Ini, $enc)
    $done = $false
    for ($i = 0; $i -lt $lines.Length; $i++) {
        if (-not $done -and $lines[$i] -match '^[ \t]*Server[ \t]*=') { $lines[$i] = "Server = $url"; $done = $true }
    }
    if (-not $done) {
        $out = New-Object Collections.Generic.List[string]
        foreach ($l in $lines) { $out.Add($l); if (-not $done -and $l -match '^\[FreeRoam\]') { $out.Add("Server = $url"); $done = $true } }
        if (-not $done) { $out.Add("Server = $url") }
        $lines = $out.ToArray()
    }
    [IO.File]::WriteAllLines($Ini, $lines, $enc)
}

# ---- small things ------------------------------------------------------------------------------------------------
function Test-Port($hostName, $port, $ms = 3000) {
    $c = New-Object Net.Sockets.TcpClient
    try {
        $a = $c.BeginConnect($hostName, $port, $null, $null)
        if (-not $a.AsyncWaitHandle.WaitOne($ms)) { return $false }
        $c.EndConnect($a); return $true
    } catch { return $false } finally { $c.Close() }
}
# "1.2.3.4", "1.2.3.4:8691", "ws://1.2.3.4:8691/lan", "my.name.org" -> ws://host:port/lan (and the host and port)
function Get-ServerUrl($text) {
    $t = ("" + $text).Trim().Trim('"')
    $scheme = "ws"
    if ($t -match '^(wss?|https?)://(.*)$') { if ($Matches[1] -eq "wss" -or $Matches[1] -eq "https") { $scheme = "wss" }; $t = $Matches[2] }
    $t = ($t -split '/')[0]
    if ($t -eq "") { return $null }
    $h = $t; $p = $Port
    if ($scheme -eq "wss") { $p = 443 }
    if ($t -match '^\[(.+)\](?::(\d+))?$') { $h = "[" + $Matches[1] + "]"; if ($Matches[2]) { $p = [int]$Matches[2] } }
    elseif ($t -match '^([^:]+):(\d+)$') { $h = $Matches[1]; $p = [int]$Matches[2] }
    elseif (@($t.ToCharArray() | Where-Object { $_ -eq ':' }).Count -ge 2) { $h = "[$t]" }
    if ($h -notmatch '^[\[\]A-Za-z0-9:.\-]+$') { return $null }
    $url = "${scheme}://${h}:${p}/lan"
    if ($scheme -eq "wss" -and $p -eq 443) { $url = "wss://${h}/lan" }
    return @{ Url = $url; Host = $h.Trim('[', ']'); Port = $p }
}
function Start-Game {
    if (-not (Test-Path $GameExe)) { throw "No SPEED2.EXE in $Game." }
    if (Get-Process SPEED2 -ErrorAction SilentlyContinue) { Say "The game is already running: close it and start it again for the new setting." Yellow; return }
    Start-Process -FilePath $GameExe -WorkingDirectory $Game
    Say "The game is starting. Load your career: the others show up in free roam."
}
function Test-Private($ip) {
    return ($ip -match '^(10\.|192\.168\.|172\.(1[6-9]|2[0-9]|3[01])\.|169\.254\.|127\.)')
}
function Test-CarrierNat($ip) {                    # 100.64.0.0/10: the provider's own NAT (and Tailscale's addresses)
    return ($ip -match '^100\.(6[4-9]|[7-9][0-9]|1[01][0-9]|12[0-7])\.')
}

# ---- this PC's addresses -----------------------------------------------------------------------------------------
function Get-LanAddress {
    try {
        $c = Get-NetIPConfiguration | Where-Object { $_.IPv4DefaultGateway -ne $null -and $_.NetAdapter.Status -eq "Up" } | Select-Object -First 1
        if ($c) { return @($c.IPv4Address)[0].IPAddress }
    } catch {}
    try {                                          # the address the route to the internet leaves from (nothing is sent)
        $u = New-Object Net.Sockets.UdpClient; $u.Connect("203.0.113.1", 9)
        $a = $u.Client.LocalEndPoint.Address.ToString(); $u.Close(); return $a
    } catch { return "" }
}
function Get-TailscaleAddress {
    foreach ($exe in @("tailscale.exe", "$env:ProgramFiles\Tailscale\tailscale.exe")) {
        try { $a = (& $exe ip -4 2>$null | Select-Object -First 1); if ($a -match '^\d+\.\d+\.\d+\.\d+$') { return $a } } catch {}
    }
    return ""
}
function Get-PublicAddress {                       # asked of api.ipify.org (it sees this PC's public address, nothing else)
    try { $a = (Invoke-WebRequest -UseBasicParsing -Uri "https://api.ipify.org" -TimeoutSec 6).Content.Trim(); if ($a -match '^\d+\.\d+\.\d+\.\d+$') { return $a } } catch {}
    return ""
}

# ---- the router (UPnP): find it, ask its public address, open and close the port ---------------------------------
function Find-Router($lan) {
    $msg = "M-SEARCH * HTTP/1.1`r`nHOST: 239.255.255.250:1900`r`nMAN: `"ssdp:discover`"`r`nMX: 2`r`nST: urn:schemas-upnp-org:device:InternetGatewayDevice:1`r`n`r`n"
    $bytes = [Text.Encoding]::ASCII.GetBytes($msg)
    $seen = @{}
    try {
        $local = New-Object Net.IPEndPoint ([Net.IPAddress]::Parse($lan)), 0
        $udp = New-Object Net.Sockets.UdpClient $local
    } catch { return $null }
    try {
        $udp.Client.ReceiveTimeout = 1500
        $to = New-Object Net.IPEndPoint ([Net.IPAddress]::Parse("239.255.255.250")), 1900
        [void]$udp.Send($bytes, $bytes.Length, $to); [void]$udp.Send($bytes, $bytes.Length, $to)
        $until = (Get-Date).AddSeconds(4)
        while ((Get-Date) -lt $until) {
            $from = New-Object Net.IPEndPoint ([Net.IPAddress]::Any), 0
            try { $r = [Text.Encoding]::ASCII.GetString($udp.Receive([ref]$from)) } catch { continue }
            if ($r -notmatch '(?im)^LOCATION:\s*(\S+)') { continue }
            $loc = $Matches[1]
            if ($seen[$loc]) { continue }
            $seen[$loc] = $true
            try {
                $xml = [xml](Invoke-WebRequest -UseBasicParsing -Uri $loc -TimeoutSec 5).Content
                foreach ($s in $xml.GetElementsByTagName("service")) {
                    $type = "" + $s.serviceType
                    if ($type -match 'WAN(IP|PPP)Connection') {
                        $base = "" + $xml.root.URLBase
                        if ($base -eq "") { $base = $loc }
                        $ctl = (New-Object Uri ((New-Object Uri $base), ("" + $s.controlURL))).AbsoluteUri
                        return @{ Control = $ctl; Service = $type }
                    }
                }
            } catch {}
        }
    } finally { $udp.Close() }
    return $null
}
function Invoke-Router($router, $action, $arguments) {
    $svc = $router.Service
    $body = "<?xml version=`"1.0`"?><s:Envelope xmlns:s=`"http://schemas.xmlsoap.org/soap/envelope/`" s:encodingStyle=`"http://schemas.xmlsoap.org/soap/encoding/`"><s:Body><u:$action xmlns:u=`"$svc`">$arguments</u:$action></s:Body></s:Envelope>"
    $r = Invoke-WebRequest -UseBasicParsing -Uri $router.Control -Method Post -ContentType 'text/xml; charset="utf-8"' -Headers @{ SOAPAction = "`"$svc#$action`"" } -Body $body -TimeoutSec 6
    return "" + $r.Content
}
function Get-RouterAddress($router) {
    try { if ((Invoke-Router $router "GetExternalIPAddress" "") -match '<NewExternalIPAddress>([^<]*)<') { return $Matches[1] } } catch {}
    return ""
}
function Open-RouterPort($router, $lan) {
    $a = "<NewRemoteHost></NewRemoteHost><NewExternalPort>$Port</NewExternalPort><NewProtocol>TCP</NewProtocol><NewInternalPort>$Port</NewInternalPort><NewInternalClient>$lan</NewInternalClient><NewEnabled>1</NewEnabled><NewPortMappingDescription>NFSU2 online server</NewPortMappingDescription><NewLeaseDuration>0</NewLeaseDuration>"
    try { [void](Invoke-Router $router "AddPortMapping" $a); return $true } catch { return $false }
}
function Close-RouterPort($router) {
    $a = "<NewRemoteHost></NewRemoteHost><NewExternalPort>$Port</NewExternalPort><NewProtocol>TCP</NewProtocol>"
    try { [void](Invoke-Router $router "DeletePortMapping" $a); return $true } catch { return $false }
}

# ---- Windows Firewall: the server's port, for the friends' games --------------------------------------------------
function Test-Firewall {
    # an enabled rule that lets TCP in on the port, or lets the server's program in on any (what Windows' own
    # "allow access" question makes); read through the firewall's COM object: no rights needed, any language
    try {
        $fw = New-Object -ComObject HNetCfg.FwPolicy2
        foreach ($r in $fw.Rules) {
            if (-not $r.Enabled -or $r.Direction -ne 1 -or $r.Action -ne 1) { continue }
            if ($r.Protocol -ne 6 -and $r.Protocol -ne 256) { continue }
            $ports = ("" + $r.LocalPorts).Trim()
            $app = "" + $r.ApplicationName
            if ($app -ne "" -and $app -ine $Server) { continue }
            if ($ports -eq "" -or $ports -eq "*") { if ($app -ne "") { return $true } else { continue } }
            foreach ($p in $ports.Split(',')) {
                $p = $p.Trim()
                if ($p -eq "$Port") { return $true }
                if ($p -match '^(\d+)-(\d+)$' -and [int]$Matches[1] -le $Port -and $Port -le [int]$Matches[2]) { return $true }
            }
        }
    } catch {}
    return $false
}
function Open-Firewall {
    if (Test-Firewall) { return $true }
    Say "Windows Firewall has to let your friends' games in on port $Port. Windows asks for your OK once: click Yes." Yellow
    try {
        Start-Process netsh -Verb RunAs -WindowStyle Hidden -Wait -ArgumentList "advfirewall firewall add rule name=`"$Rule`" dir=in action=allow protocol=TCP localport=$Port"
    } catch {}
    return (Test-Firewall)
}

# ---- hosting -------------------------------------------------------------------------------------------------------
function Get-Players {
    try { return (Invoke-RestMethod -Uri "http://127.0.0.1:$Port/players" -TimeoutSec 3).players } catch { return $null }
}
# An older set-up on this PC (the Linux server in WSL, before there was one for Windows) can be in the way: its server
# still running on the port, or the Windows port forward that led the friends' games to it.
function Get-OldForwards {
    $found = @()
    try {
        foreach ($l in @(& netsh interface portproxy show v4tov4 2>$null)) {
            if ("$l" -match '^\s*(\S+)\s+(\d+)\s+(\S+)\s+(\d+)\s*$' -and [int]$Matches[2] -eq $Port) {
                $o = @{ Listen = $Matches[1]; To = $Matches[3]; ToPort = [int]$Matches[4] }
                if ($o.Listen -eq "*") { $o.Listen = "0.0.0.0" }
                # in the way: it holds the port on every address (the server cannot listen then), or it leads
                # somewhere other than this PC's own server
                $o.Bad = ($o.Listen -eq "0.0.0.0") -or (($o.To -ne "127.0.0.1") -and ($o.To -ne "localhost")) -or ($o.ToPort -ne $Port)
                $found += $o
            }
        }
    } catch {}
    return $found
}
function Clear-OldForwards {
    $bad = @(Get-OldForwards | Where-Object { $_.Bad })
    if ($bad.Count -eq 0) { return }
    Say "An older set-up left a Windows port forward on port $Port (to $($bad[0].To)). It would catch your friends' games" Yellow
    Say "before this server does. Windows asks for your OK to remove it: click Yes." Yellow
    foreach ($b in $bad) {
        try { Start-Process netsh -Verb RunAs -WindowStyle Hidden -Wait -ArgumentList "interface portproxy delete v4tov4 listenaddress=$($b.Listen) listenport=$Port" } catch {}
    }
    foreach ($b in @(Get-OldForwards | Where-Object { $_.Bad })) {
        Say "It is still there. In an administrator PowerShell, run:" Red
        Say "    netsh interface portproxy delete v4tov4 listenaddress=$($b.Listen) listenport=$Port" Red
    }
}
function Start-Server {
    if ($null -ne (Get-Players)) {
        if (Get-Process nfsu2-server -ErrorAction SilentlyContinue) { Say "The server already runs on this PC (port $Port): using it."; return $true }
        # a server answers, and it is not this one: the older set-up's, in WSL
        Say "Another server answers on port ${Port} of this PC: the older Linux (WSL) one, most likely. The new one cannot start beside it." Yellow
        $a = ""
        try { $a = ("" + (Read-Host "Shut the Linux side (WSL) down now, to use the new server? Y or N")).Trim().ToLower() } catch {}
        if ($a -eq "y" -or $a -eq "yes") {
            try { & wsl.exe --shutdown 2>$null | Out-Null } catch {}
            for ($i = 0; $i -lt 16 -and $null -ne (Get-Players); $i++) { Start-Sleep -Milliseconds 500 }
        }
        if ($null -ne (Get-Players)) { Say "It still answers. Close its window, then choose Host again." Red; return $false }
    }
    if (Test-Port "127.0.0.1" $Port 1000) { Say "Port $Port on this PC is taken by another program: close it, or restart the PC." Red; return $false }
    if (-not (Test-Path $Server)) { Say "No nfsu2-server.exe in $Here." Red; return $false }
    try { Unblock-File $Server } catch {}
    [void](New-Item -ItemType Directory -Force $State)
    $run = "`"$Server`" --data `"$Game`" --state `"$State`" --http-port $Port --friends"
    Start-Process cmd.exe -WindowStyle Minimized -ArgumentList "/c `"title NFSU2 online server (close this window to stop hosting) & $run`""
    for ($i = 0; $i -lt 40; $i++) { Start-Sleep -Milliseconds 500; if ($null -ne (Get-Players)) { return $true } }
    Say "The server did not start. Its own message is in the window that opens now." Red
    Start-Process cmd.exe -ArgumentList "/k `"$run`""
    return $false
}
function Show-Addresses($check) {
    $lan = Get-LanAddress
    $tail = Get-TailscaleAddress
    $internet = ""; $why = ""
    Say "Looking for your router..."
    $router = $null
    if ($lan) { $router = Find-Router $lan }
    if ($router) {
        $wan = Get-RouterAddress $router
        if ($wan -and -not (Test-Private $wan) -and -not (Test-CarrierNat $wan)) {
            if ($check) { $internet = $wan; $why = "the router can open port $Port by itself (not done: check only)" }
            elseif (Open-RouterPort $router $lan) { $internet = $wan; $why = "port $Port opened on your router automatically" }
            else { $internet = $wan; $why = "your router refused to open port $Port by itself: forward TCP port $Port to $lan in its settings" }
        } elseif ($wan) {
            $why = "your internet provider puts your router behind its own network ($wan): nobody on the internet can reach this PC directly"
        } else { $why = "your router did not say its public address" }
    } else {
        $pub = Get-PublicAddress
        if ($pub) { $internet = $pub; $why = "your router does not open ports by itself (UPnP is off): forward TCP port $Port to $lan in its settings" }
        else { $why = "no router found and no internet address: only your own network can join" }
    }
    Say ""
    Say "  ADDRESSES FOR YOUR FRIENDS (they choose Join and type one of these)" White
    if ($internet) { Say ("    over the internet : {0}" -f $internet) Green; Say "                        ($why)" }
    else           { Say "    over the internet : not possible from this PC" Yellow; Say "                        ($why)" }
    if ($lan)      { Say ("    same Wi-Fi / LAN  : {0}" -f $lan) Green }
    if ($tail)     { Say ("    over Tailscale    : {0}   (friends you shared this PC with)" -f $tail) Green }
    if (-not $internet -and -not $tail) { Say "    Tip: with Tailscale (free) on both PCs, or with your friend hosting instead, it works anyway." }
    Say ""
    $give = $internet; if (-not $give) { $give = $tail }; if (-not $give) { $give = $lan }
    if ($give -and -not $check) { try { Set-Clipboard $give; Say "  ($give is on your clipboard: paste it to your friends.)" } catch {} }
}
function Invoke-Host {
    Say "HOST A GAME: this PC is the server." White
    Clear-OldForwards
    if (-not (Start-Server)) { throw "The server is not running." }
    if (-not (Open-Firewall)) { Say "Windows Firewall still blocks port ${Port}: friends cannot join until it is allowed (run this again and click Yes)." Red }
    Show-Addresses $false
    Set-IniServer "ws://127.0.0.1:$Port/lan"
    Start-Game
    Say ""
    Say "The server runs in its own window (minimised, 'NFSU2 online server'). Leave it open while anyone plays;" White
    Say "close it, or choose Stop hosting here, when you are done." White
}
function Invoke-Check {
    Say "CHECK (nothing is changed): can this PC host?" White
    $n = Get-Players
    $s = "not running"; if ($null -ne $n) { $s = "running, $n player(s) on it" }
    if ($null -ne $n -and -not (Get-Process nfsu2-server -ErrorAction SilentlyContinue)) { $s = "ANOTHER server answers on port $Port (the older Linux / WSL one?): Host offers to shut it down" }
    Say "  server    : $s"
    foreach ($b in @(Get-OldForwards | Where-Object { $_.Bad })) {
        Say "  old set-up: a Windows port forward $($b.Listen):${Port} -> $($b.To):$($b.ToPort) is in the way (Host removes it, with your OK)" Yellow
    }
    $f = "closed for port $Port (Host opens it, with your OK)"; if (Test-Firewall) { $f = "open for port $Port" }
    Say "  firewall  : $f"
    Say "  game      : Server = $(Get-IniServer)"
    Show-Addresses $true
}
function Invoke-Stop {
    $p = Get-Process nfsu2-server -ErrorAction SilentlyContinue
    if ($p) { $p | Stop-Process -Force; Say "The server on this PC is stopped." } else { Say "No server was running on this PC." }
    $lan = Get-LanAddress
    if ($lan) { $router = Find-Router $lan; if ($router -and (Close-RouterPort $router)) { Say "Port $Port on your router is closed again." } }
}

# ---- joining -------------------------------------------------------------------------------------------------------
function Invoke-Join($text) {
    Say "JOIN A FRIEND" White
    while ($true) {
        if (-not $text) { $text = Read-Host "Your friend's address (as their window showed it, e.g. 71.212.48.130)" }
        $s = Get-ServerUrl $text
        if ($s) { break }
        Say "That is not an address. Examples: 71.212.48.130   192.168.1.20   100.76.38.37" Yellow
        $text = ""
    }
    Say "Trying $($s.Host), port $($s.Port)..."
    if (Test-Port $s.Host $s.Port 4000) { Say "Your friend's server answers." Green }
    else {
        Say "No answer from $($s.Host) right now. Your friend has to choose 'Host a game' first, and keep its window open." Yellow
        Say "The address is saved anyway: the game keeps trying by itself and connects when the server is up."
    }
    Set-IniServer $s.Url
    Start-Game
}

# ---- the menu ------------------------------------------------------------------------------------------------------
function Show-Menu {
    while ($true) {
        Say ""
        Say "  NFSU2 ONLINE" White
        Say "  the game is set to: $(Get-IniServer)"
        Say ""
        Say "   1  Host a game     (this PC is the server; your friends join you)"
        Say "   2  Join a friend   (type the address your friend's window shows)"
        Say "   3  Play            (start the game as it is set now)"
        Say "   4  Stop hosting    (stop the server on this PC)"
        Say "   5  Check           (can this PC host? changes nothing)"
        Say "   Q  Quit"
        Say ""
        $k = ("" + (Read-Host "Choose")).Trim().ToLower()
        try {
            switch ($k) {
                "1" { Invoke-Host }
                "2" { Invoke-Join "" }
                "3" { Start-Game }
                "4" { Invoke-Stop }
                "5" { Invoke-Check }
                "q" { return }
                default { Say "1, 2, 3, 4, 5 or Q." Yellow }
            }
        } catch { Say ("" + $_.Exception.Message) Red }
    }
}

try {
    switch ($Mode.ToLower()) {
        "host"  { Invoke-Host;  Show-Menu }
        "join"  { Invoke-Join $Address }
        "play"  { Start-Game }
        "stop"  { Invoke-Stop }
        "check" { Invoke-Check }
        default { Show-Menu }
    }
} catch {
    Say ("" + $_.Exception.Message) Red
    exit 1
}
