#include <Windows.h>
#include <Psapi.h>
#include <cassert>
#include <cinttypes>
#include <iostream>
#include <fstream>
#include <string>
#include <locale>
#include <codecvt>
#include <memory>
#include "Detouring.h"
#include "resource.h"

#include "../include/detours.h"

using namespace std;

bool stopRequested = false;

typedef struct {
    bool ShowHelp;
    bool WaitForTarget;
} CommandLineOptions;

BOOL IsWow64(HANDLE processHandle);
std::wstring UnpackDependencies();
void Launch64BitVersion(const wstring& binariesPath, bool waitForTarget, const wchar_t *argument);
CommandLineOptions ParseOptions(wchar_t* argv[], int argc, int& argumentIndex);
void ShowUsage();
BOOL WINAPI HandleConsoleInterrupt(DWORD dwCtrlType);

int32_t wmain(int32_t argc, wchar_t *argv[])
{
    int argumentIndex = 1;
    auto options = ParseOptions(argv, argc, argumentIndex);

    assert(argumentIndex <= argc);
    if (argumentIndex == argc) {
        cout << "ERROR: invalid arguments." << endl << endl;
        ShowUsage();
        return 1;
    }
    if (options.ShowHelp) {
        ShowUsage();
        return 0;
    }

    try {
        DWORD pid = wcstoul(argv[argumentIndex], nullptr, 10);
        wstring binariesPath = UnpackDependencies();
        bool startingNewProcess = pid == 0;

        HANDLE targetProcess = NULL;
        wstring injectdll = binariesPath + L"InjectDll32.dll";
        if (startingNewProcess) {
            wstring exe = argv[argumentIndex];
            wstring args;
            for (int32_t i = argumentIndex + 1; i < argc; i++) {
                args.append(argv[i]).append(L" ");
            }
            wcout << L"INFO: Starting the '" << exe << L"' process." << endl;
            targetProcess = StartProcess(ws2s(injectdll), exe, args);
        } else {
            if (IsWow64(GetCurrentProcess())) {
                HANDLE tempProcessHandle = OpenProcess(PROCESS_QUERY_INFORMATION, FALSE, pid);
                if (tempProcessHandle == NULL) {
                    ThrowWin32Exception("OpenProcess");
                }
                // if we are under WOW64, the target process could be 64-bit
                if (!IsWow64(tempProcessHandle)) {
                    CloseHandle(tempProcessHandle);
                    cout << "INFO: Target process is 64-bit, launching the 64-bit TakeDetour version." << endl;
                    Launch64BitVersion(binariesPath, options.WaitForTarget, argv[argumentIndex]);
                    return 0;
                }
                // target process is 32-bit too, continue
                CloseHandle(tempProcessHandle);
                cout << "INFO: Attaching to the target process (" << pid << ")." << endl;
                targetProcess = AttachToProcess(pid, injectdll);
            } else {
                // it's 32 bit system, let's continue
                cout << "INFO: Attaching to the target process (" << pid << ")." << endl;
                targetProcess = AttachToProcess(pid, injectdll);
            }
        }

        if (!SetConsoleCtrlHandler(HandleConsoleInterrupt, TRUE)) {
            ThrowWin32Exception("SetConsoleCtrlHandler");
        }
        if (options.WaitForTarget) {
            cout << endl << (startingNewProcess ? "Press Ctrl + C to stop the target process." :
                    "Press Ctrl + C to stop and unload the DLL from the target process." ) << endl;
            while (!stopRequested) {
                DWORD waitResult = WaitForSingleObject(targetProcess, 200);
                if (waitResult == WAIT_OBJECT_0) {
                    cout << "INFO: Target process exited. Stopping." << endl;
                    break;
                }
                if (waitResult == WAIT_FAILED) {
                    ThrowWin32Exception("WaitForSingleObject");
                }
            }

            DWORD exitCode;
            if (stopRequested && GetExitCodeProcess(targetProcess, &exitCode) && exitCode == STILL_ACTIVE) {
                if (startingNewProcess) {
                    if (!TerminateProcess(targetProcess, 1)) {
                        ThrowWin32Exception("TerminateProcess");
                    }
                    cout << "INFO: Target process killed." << endl;
                } else {
                    DetachFromProcess(targetProcess, injectdll);
                    cout << "INFO: Injected DLL detached from the target process." << endl;
                }
            }
        }
        assert(targetProcess != NULL);
        CloseHandle(targetProcess);
    } catch (exception& ex) {
        cerr << endl << "ERROR: " << ex.what() << endl;
        return 1;
    }

    return 0;
}

CommandLineOptions ParseOptions(wchar_t* argv[], int argc, int& argumentIndex)
{
    CommandLineOptions options = { 0 };

    while (argumentIndex < argc) {
        wstring option(argv[argumentIndex]);
        if (option[0] != L'-') {
            break;
        }
        if (option.find(L'h') != wstring::npos) {
            options.ShowHelp = true;
        }
        if (option.find(L'w') != wstring::npos) {
            options.WaitForTarget = true;
        }
        argumentIndex++;
    }

    return options;
}

//----------------------------------------------------------------

void ShowUsage()
{
    cout << "TakeDetour v1.0" << endl;
    cout << "Copyright (C) 2018 Sebastian Solnica (@lowleveldesign)" << endl;
    cout << endl;

    cout << "TakeDetour.exe OPTIONS <pid | exe_path args>" << endl;
    cout << endl;
    cout << "OPTIONS include:" << endl;
    cout << "  -w    wait for Ctrl+C to finish and unload the injected DLL" << endl;
    cout << "  -h    show help" << endl;
    cout << "  -v    show verbose logs" << endl;
}

//----------------------------------------------------------------

typedef BOOL(WINAPI *LPFN_ISWOW64PROCESS) (HANDLE, PBOOL);
LPFN_ISWOW64PROCESS fnIsWow64Process;

BOOL IsWow64(HANDLE processHandle)
{
    BOOL bIsWow64 = FALSE;

    if (fnIsWow64Process == NULL) {
        fnIsWow64Process = (LPFN_ISWOW64PROCESS)GetProcAddress(
            GetModuleHandle(L"kernel32"), "IsWow64Process");
    }

    if (fnIsWow64Process != NULL) {
        if (!fnIsWow64Process(processHandle, &bIsWow64)) {
            ThrowWin32Exception("IsWow64Process");
        }
    }
    return bIsWow64;
}

//----------------------------------------------------------------

void Launch64BitVersion(const wstring& binariesPath, bool waitForTarget, const wchar_t *argument)
{
    STARTUPINFO si;
    PROCESS_INFORMATION pi;

    ZeroMemory(&si, sizeof(si));
    ZeroMemory(&pi, sizeof(pi));
    si.cb = sizeof(si);

    wstring commandLine = binariesPath + L"TakeDetour64.exe" + (waitForTarget ? L" -w " : L" ") + argument;
    size_t commandLineLength = commandLine.length() + 1;
    auto commandLineChangeableCopy = make_unique<wchar_t[]>(commandLineLength);
    wcsncpy_s(commandLineChangeableCopy.get(), commandLineLength, commandLine.c_str(), commandLineLength);

    if (!CreateProcess(NULL, commandLineChangeableCopy.get(),
        NULL, NULL, FALSE, CREATE_DEFAULT_ERROR_MODE | CREATE_NEW_CONSOLE, NULL, NULL, &si, &pi)) {
        ThrowWin32Exception("CreateProcess");
    }

    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
}

//----------------------------------------------------------------

void UnpackBinaryFile(int resId, const wstring& destPath)
{
    // find location of the resource and get handle to it
    auto resourceDll = FindResource(NULL, MAKEINTRESOURCE(resId), L"BINARY");
    if (resourceDll == NULL) {
        ThrowWin32Exception("FindResource");
    }

    // loads the specified resource into global memory.
    auto resource = LoadResource(NULL, resourceDll);
    if (resource == NULL) {
        ThrowWin32Exception("LoadResource");
    }

    // get a pointer to the loaded resource!
    const char* resourceData = (char *)LockResource(resource);
    if (resourceData == NULL) {
        ThrowWin32Exception("LockResource");
    }

    // determine the size of the resource, so we know how much to write out to file!
    DWORD resourceSize = SizeofResource(NULL, resourceDll);
    if (resourceSize == 0) {
        ThrowWin32Exception("SizeofResource");
    }

    ofstream outputFile(destPath, ios::binary);
    outputFile.write(resourceData, resourceSize);
    outputFile.close();
}

//----------------------------------------------------------------

BOOL WINAPI HandleConsoleInterrupt(DWORD dwCtrlType)
{
    cout << "INFO: Received Ctrl + C. Stopping." << endl;
    stopRequested = true;
    return TRUE;
}

//----------------------------------------------------------------

wstring UnpackDependencies()
{
    wchar_t buffer[MAX_PATH + 1];
    DWORD len = GetTempPath(MAX_PATH + 1, buffer);
    assert(len <= MAX_PATH + 1);
    if (len == 0) {
        ThrowWin32Exception("GetTempPath");
    }
    wstring binariesPath(buffer, len);
    binariesPath += L"takedetour\\";
    if (!CreateDirectory(binariesPath.c_str(), NULL) && GetLastError() != ERROR_ALREADY_EXISTS) {
        ThrowWin32Exception("CreateDirectory");
    }

    // unpack all the binaries from resources
    UnpackBinaryFile(IDR_BINARY1, binariesPath + L"InjectDll64.dll");
    UnpackBinaryFile(IDR_BINARY2, binariesPath + L"InjectDll32.dll");
    UnpackBinaryFile(IDR_BINARY3, binariesPath + L"TakeDetour64.exe");

    assert(binariesPath[binariesPath.length() - 1] == L'\\');
    return binariesPath;
}
