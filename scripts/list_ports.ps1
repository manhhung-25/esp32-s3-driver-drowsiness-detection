$ErrorActionPreference = "Stop"

Get-CimInstance Win32_SerialPort |
    Select-Object DeviceID, Name, Description |
    Format-Table -AutoSize
