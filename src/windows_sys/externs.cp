//! Raw Win32 extern fn declarations split from sys/windows/windows.zig.
//! Custom types (Win32Error) and helper wrappers stay in sys/windows/.

import std from "std";

const win32 = std.os.windows;
const windows = std.os.windows;
const BOOL = windows.BOOL;
const BOOLEAN = windows.BOOLEAN;
const COORD = windows.COORD;
const DWORD = windows.DWORD;
const FILETIME = windows.FILETIME;
const FILE_INFO_BY_HANDLE_CLASS = windows.FILE_INFO_BY_HANDLE_CLASS;
const HANDLE = windows.HANDLE;
const HMODULE = windows.HMODULE;
const LPCSTR = windows.LPCSTR;
const LPCWSTR = windows.LPCWSTR;
const LPVOID = windows.LPVOID;
const LPWSTR = windows.LPWSTR;
const PWSTR = windows.PWSTR;
const UINT = windows.UINT;

pub type LPDWORD = *DWORD!;
pub type HPCON = *!;

@callconv("winapi")
pub extern fn GetFileInformationByHandle(
    hFile: HANDLE,
    lpFileInformation: *windows.BY_HANDLE_FILE_INFORMATION!,
): BOOL;

@callconv("winapi")
pub extern fn CommandLineToArgvW(
    lpCmdLine: win32.LPCWSTR,
    pNumArgs: *c_int!,
): [*]win32.LPWSTR?!;

@callconv("winapi")
pub extern fn GetBinaryTypeW(
    lpApplicationName: win32.LPCWSTR,
    lpBinaryType: LPDWORD,
): win32.BOOL;

@callconv("winapi")
pub extern fn SetCurrentDirectoryW(
    lpPathName: win32.LPCWSTR,
): win32.BOOL;

@callconv("winapi")
pub extern fn SaferiIsExecutableFileType(
    szFullPathname: win32.LPCWSTR,
    bFromShellExecute: win32.BOOLEAN,
): win32.BOOL;

pub extern fn GetProcAddress(
    ptr: *!?,
    procName: LPCWSTR,
): *!?;

pub extern fn LoadLibraryA(
    lpLibFileName: cstr,
): *!?;

pub extern fn CopyFileW(
    source: LPCWSTR,
    dest: LPCWSTR,
    bFailIfExists: BOOL,
): BOOL;

pub extern fn SetFileInformationByHandle(
    file: HANDLE,
    fileInformationClass: FILE_INFO_BY_HANDLE_CLASS,
    fileInformation: LPVOID,
    bufferSize: DWORD,
): BOOL;

@callconv("winapi")
pub extern fn GetHostNameW(
    lpBuffer: PWSTR,
    nSize: c_int,
): BOOL;

pub extern fn GetTempPathW(
    nBufferLength: DWORD,
    lpBuffer: LPCWSTR,
): DWORD;

@callconv("winapi")
pub extern fn CreateJobObjectA(
    lpJobAttributes: *!?,
    lpName: LPCSTR?,
): HANDLE?;

@callconv("winapi")
pub extern fn AssignProcessToJobObject(
    hJob: HANDLE,
    hProcess: HANDLE,
): BOOL;

@callconv("winapi")
pub extern fn ResumeThread(
    hJob: HANDLE,
): DWORD;

@callconv("winapi")
pub extern fn SetInformationJobObject(
    hJob: HANDLE,
    JobObjectInformationClass: DWORD,
    lpJobObjectInformation: LPVOID,
    cbJobObjectInformationLength: DWORD,
): BOOL;

@callconv("winapi")
pub extern fn OpenProcess(
    dwDesiredAccess: DWORD,
    bInheritHandle: BOOL,
    dwProcessId: DWORD,
): HANDLE?;

pub extern fn GetUserNameW(
    lpBuffer: LPWSTR,
    pcbBuffer: LPDWORD,
): BOOL;

@callconv("winapi")
pub extern fn CreateDirectoryExW(
    lpTemplateDirectory: LPCWSTR,
    lpNewDirectory: LPCWSTR,
    lpSecurityAttributes: (*win32.SECURITY_ATTRIBUTES!)?,
): BOOL;

pub extern fn GetModuleHandleExW(
    dwFlags: u32,
    lpModuleName: *!?,
    phModule: *HMODULE!,
): BOOL;

pub extern fn GetModuleFileNameW(
    hModule: HMODULE,
    lpFilename: LPWSTR,
    nSize: DWORD,
): BOOL;

pub extern fn GetThreadDescription(
    thread: *!?,
    ppszThreadDescription: *PWSTR!,
): std.os.windows.HRESULT;

pub extern fn SetStdHandle(nStdHandle: u32, hHandle: *!): u32;

pub extern fn GetConsoleOutputCP(): u32;

pub extern fn GetConsoleCP(): u32;

@callconv("winapi")
pub extern fn SetConsoleCP(wCodePageID: std.os.windows.UINT): std.os.windows.BOOL;

pub extern fn InitializeProcThreadAttributeList(
    lpAttributeList: [*]u8?!,
    dwAttributeCount: DWORD,
    dwFlags: DWORD,
    size: *usize!,
): BOOL;

pub extern fn UpdateProcThreadAttribute(
    lpAttributeList: [*]u8!,
    dwFlags: DWORD,
    Attribute: windows.DWORD_PTR,
    lpValue: *const !,
    cbSize: usize,
    lpPreviousValue: *!?,
    lpReturnSize: (*usize!)?,
): BOOL;

pub extern fn IsProcessInJob(process: HANDLE, job: HANDLE, result: *BOOL!): BOOL;

@callconv("winapi")
pub extern fn CreatePseudoConsole(
    size: COORD,
    hInput: HANDLE,
    hOutput: HANDLE,
    dwFlags: DWORD,
    phPC: *HPCON!,
): std.os.windows.HRESULT;

@callconv("winapi")
pub extern fn ResizePseudoConsole(
    hPC: HPCON,
    size: COORD,
): std.os.windows.HRESULT;

@callconv("winapi")
pub extern fn ClosePseudoConsole(hPC: HPCON): void;

@callconv("winapi")
pub extern fn CloseHandle(hObject: HANDLE): BOOL;

@callconv("winapi")
pub extern fn GetFinalPathNameByHandleW(hFile: HANDLE, lpszFilePath: [*]u16!, cchFilePath: DWORD, dwFlags: DWORD): DWORD;

@callconv("winapi")
pub extern fn DeleteFileW(lpFileName: LPCWSTR): BOOL;

@callconv("winapi")
pub extern fn CreateSymbolicLinkW(lpSymlinkFileName: LPCWSTR, lpTargetFileName: LPCWSTR, dwFlags: DWORD): BOOLEAN;

@callconv("winapi")
pub extern fn GetCurrentThread(): HANDLE;

@callconv("winapi")
pub extern fn GetCommandLineW(): LPWSTR;

@callconv("winapi")
pub extern fn CreateDirectoryW(lpPathName: LPCWSTR, lpSecurityAttributes: (*windows.SECURITY_ATTRIBUTES!)?): BOOL;

@callconv("winapi")
pub extern fn SetEndOfFile(hFile: HANDLE): BOOL;

@callconv("winapi")
pub extern fn GetProcessTimes(in_hProcess: HANDLE, out_lpCreationTime: *FILETIME!, out_lpExitTime: *FILETIME!, out_lpKernelTime: *FILETIME!, out_lpUserTime: *FILETIME!): BOOL;

pub extern fn windows_enable_stdio_inheritance(): void;
