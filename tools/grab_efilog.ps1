# Read KestrelOS's shutdown and boot snapshots from UEFI NVRAM on Windows.
# This is read-only, but Windows requires SeSystemEnvironmentPrivilege, so run
# it from an elevated PowerShell (or let Start-Process -Verb RunAs launch it).
[CmdletBinding()]
param([string]$OutputDirectory = "$PSScriptRoot\..\out\efi_logs")

$ErrorActionPreference = "Stop"

$principal = New-Object Security.Principal.WindowsPrincipal(
    [Security.Principal.WindowsIdentity]::GetCurrent())
if (-not $principal.IsInRole(
        [Security.Principal.WindowsBuiltInRole]::Administrator)) {
    throw "Reading firmware variables requires an elevated PowerShell."
}

Add-Type -TypeDefinition @'
using System;
using System.ComponentModel;
using System.Runtime.InteropServices;

public static class KestrelFirmwareLog {
    const UInt32 TOKEN_QUERY = 0x0008;
    const UInt32 TOKEN_ADJUST_PRIVILEGES = 0x0020;
    const UInt32 SE_PRIVILEGE_ENABLED = 0x00000002;

    [StructLayout(LayoutKind.Sequential)]
    struct LUID { public UInt32 LowPart; public Int32 HighPart; }

    [StructLayout(LayoutKind.Sequential)]
    struct TOKEN_PRIVILEGES {
        public UInt32 PrivilegeCount;
        public LUID Luid;
        public UInt32 Attributes;
    }

    [DllImport("kernel32.dll")]
    static extern IntPtr GetCurrentProcess();

    [DllImport("advapi32.dll", SetLastError=true)]
    static extern bool OpenProcessToken(IntPtr process, UInt32 access,
                                        out IntPtr token);

    [DllImport("advapi32.dll", CharSet=CharSet.Unicode, SetLastError=true)]
    static extern bool LookupPrivilegeValue(string system, string name,
                                             out LUID luid);

    [DllImport("advapi32.dll", SetLastError=true)]
    static extern bool AdjustTokenPrivileges(IntPtr token, bool disableAll,
                                              ref TOKEN_PRIVILEGES state,
                                              UInt32 length, IntPtr previous,
                                              IntPtr returnedLength);

    [DllImport("kernel32.dll", SetLastError=true)]
    static extern bool CloseHandle(IntPtr handle);

    [DllImport("kernel32.dll", CharSet=CharSet.Unicode, SetLastError=true)]
    static extern UInt32 GetFirmwareEnvironmentVariableEx(
        string name, string vendorGuid, byte[] data, UInt32 size,
        out UInt32 attributes);

    static void EnableFirmwarePrivilege() {
        IntPtr token;
        if (!OpenProcessToken(GetCurrentProcess(),
                              TOKEN_QUERY | TOKEN_ADJUST_PRIVILEGES,
                              out token))
            throw new Win32Exception(Marshal.GetLastWin32Error());
        try {
            LUID luid;
            if (!LookupPrivilegeValue(null, "SeSystemEnvironmentPrivilege",
                                      out luid))
                throw new Win32Exception(Marshal.GetLastWin32Error());
            TOKEN_PRIVILEGES state = new TOKEN_PRIVILEGES();
            state.PrivilegeCount = 1;
            state.Luid = luid;
            state.Attributes = SE_PRIVILEGE_ENABLED;
            if (!AdjustTokenPrivileges(token, false, ref state, 0,
                                       IntPtr.Zero, IntPtr.Zero))
                throw new Win32Exception(Marshal.GetLastWin32Error());
            int error = Marshal.GetLastWin32Error();
            if (error != 0) throw new Win32Exception(error);
        } finally {
            CloseHandle(token);
        }
    }

    public static byte[] Read(string name, string vendorGuid) {
        EnableFirmwarePrivilege();
        byte[] buffer = new byte[65536];
        UInt32 attributes;
        UInt32 length = GetFirmwareEnvironmentVariableEx(
            name, vendorGuid, buffer, (UInt32)buffer.Length, out attributes);
        if (length == 0)
            throw new Win32Exception(Marshal.GetLastWin32Error());
        byte[] result = new byte[length];
        Array.Copy(buffer, result, length);
        return result;
    }
}
'@

$guid = "{B34C1A7E-9D62-4E47-9C31-5A6F2E88D140}"
New-Item -ItemType Directory -Force -Path $OutputDirectory | Out-Null
$report = @("=== Kestrel UEFI log retrieval $(Get-Date -Format o) ===")
$found = 0

foreach ($name in @("KestrelLog", "KestrelBoot")) {
    try {
        [byte[]]$bytes = [KestrelFirmwareLog]::Read($name, $guid)
        $path = Join-Path $OutputDirectory "$name.log"
        [IO.File]::WriteAllBytes($path, $bytes)
        $report += "read $name ($($bytes.Length) bytes) -> $path"
        $found++
    } catch {
        $report += "$name unavailable: $($_.Exception.Message)"
    }
}

$reportPath = Join-Path $OutputDirectory "retrieval.log"
$report | Set-Content -Path $reportPath -Encoding utf8
$report | ForEach-Object { Write-Output $_ }
if (-not $found) { exit 1 }
