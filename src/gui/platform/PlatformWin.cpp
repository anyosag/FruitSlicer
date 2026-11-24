// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 FruitSlicer contributors
// Windows-only pieces: the USB drives a print can go to, ejecting one,
// showing a file in Explorer, and keeping Windows from sleeping or slowing
// the app down while a job runs. <windows.h> stays in this file: its macros
// (near, far, min, max and more) would break names elsewhere.
#include "platform/Platform.h"

#include <windows.h>
#include <cfgmgr32.h>
#include <setupapi.h>
#include <shellapi.h>
#include <shlobj.h>
#include <winioctl.h>

#include <cstddef>
#include <cstdio>
#include <cwctype>
#include <filesystem>
#include <string>
#include <vector>

namespace gui::platform {

namespace {

std::wstring wide(const std::string& s) {
    if (s.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring w(static_cast<std::size_t>(n > 0 ? n : 0), L'\0');
    if (n > 0) MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
    return w;
}

std::string narrow(const wchar_t* w) {
    if (!w || !*w) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
    if (n <= 1) return {};
    std::string s(static_cast<std::size_t>(n - 1), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w, -1, s.data(), n, nullptr, nullptr);
    return s;
}

// The device of the volume at drive `letter`: \\.\E:
std::wstring volumeDevice(wchar_t letter) { return std::wstring(L"\\\\.\\") + letter + L":"; }

// Which storage device a volume or a disk is (asked with no access rights: no administrator needed).
bool deviceNumber(const wchar_t* device, STORAGE_DEVICE_NUMBER& out) {
    HANDLE h = CreateFileW(device, 0, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    DWORD got = 0;
    const BOOL ok = DeviceIoControl(h, IOCTL_STORAGE_GET_DEVICE_NUMBER, nullptr, 0, &out, sizeof out, &got, nullptr);
    CloseHandle(h);
    return ok != FALSE;
}

// Whether the drive's disk sits on USB: some USB sticks and card readers call themselves fixed disks.
bool onUsb(wchar_t letter) {
    const std::wstring device = volumeDevice(letter);
    HANDLE h = CreateFileW(device.c_str(), 0, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    STORAGE_PROPERTY_QUERY query{};
    query.PropertyId = StorageDeviceProperty;
    query.QueryType = PropertyStandardQuery;
    alignas(8) unsigned char buf[1024] = {};
    DWORD got = 0;
    bool usb = false;
    if (DeviceIoControl(h, IOCTL_STORAGE_QUERY_PROPERTY, &query, sizeof query, buf, sizeof buf, &got, nullptr) &&
        got >= offsetof(STORAGE_DEVICE_DESCRIPTOR, BusType) + sizeof(STORAGE_BUS_TYPE))
        usb = reinterpret_cast<const STORAGE_DEVICE_DESCRIPTOR*>(buf)->BusType == BusTypeUsb;
    CloseHandle(h);
    return usb;
}

// The disks Windows knows, as device interfaces (GUID_DEVINTERFACE_DISK, written out so that no
// header order has to define it).
const GUID kDiskInterface = {0x53f56307, 0xb6bf, 0x11d0, {0x94, 0xf2, 0x00, 0xa0, 0xc9, 0x1e, 0xfb, 0x8b}};

// The device node of the disk that `volume` is on, or 0.
DEVINST diskOf(const STORAGE_DEVICE_NUMBER& volume) {
    HDEVINFO set = SetupDiGetClassDevsW(&kDiskInterface, nullptr, nullptr, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    if (set == INVALID_HANDLE_VALUE) return 0;
    DEVINST disk = 0;
    for (DWORD i = 0; disk == 0; ++i) {
        SP_DEVICE_INTERFACE_DATA iface{};
        iface.cbSize = sizeof iface;
        if (!SetupDiEnumDeviceInterfaces(set, nullptr, &kDiskInterface, i, &iface)) break;
        DWORD need = 0;
        SetupDiGetDeviceInterfaceDetailW(set, &iface, nullptr, 0, &need, nullptr);
        if (need < sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W)) continue;
        std::vector<DWORD> buf((need + sizeof(DWORD) - 1) / sizeof(DWORD));  // DWORD-aligned, as the API wants
        auto* detail = reinterpret_cast<SP_DEVICE_INTERFACE_DETAIL_DATA_W*>(buf.data());
        detail->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W);
        SP_DEVINFO_DATA info{};
        info.cbSize = sizeof info;
        if (!SetupDiGetDeviceInterfaceDetailW(set, &iface, detail, need, nullptr, &info)) continue;
        STORAGE_DEVICE_NUMBER n{};
        if (deviceNumber(detail->DevicePath, n) && n.DeviceType == volume.DeviceType &&
            n.DeviceNumber == volume.DeviceNumber)
            disk = info.DevInst;
    }
    SetupDiDestroyDeviceInfoList(set);
    return disk;
}

}  // namespace

void attachConsole() {
    // Output that already goes somewhere (a pipe or a file, as in CI) stays there.
    const HANDLE out = GetStdHandle(STD_OUTPUT_HANDLE);
    if (out && out != INVALID_HANDLE_VALUE && GetFileType(out) != FILE_TYPE_UNKNOWN) return;
    if (!AttachConsole(ATTACH_PARENT_PROCESS)) return;  // started from Explorer: no console to borrow
    FILE* f = nullptr;
    freopen_s(&f, "CONOUT$", "w", stdout);
    freopen_s(&f, "CONOUT$", "w", stderr);
}

// While a job runs: the PC stays awake (the screen may still turn off), and
// Windows 11 does not slow the app down when its window is in the background
// (power throttling, which Task Manager calls efficiency mode). Both are
// called on the UI thread, where the job runner starts and delivers jobs.
void beginBusy(const std::string&) {
    SetThreadExecutionState(ES_CONTINUOUS | ES_SYSTEM_REQUIRED);
    PROCESS_POWER_THROTTLING_STATE state{};
    state.Version = PROCESS_POWER_THROTTLING_CURRENT_VERSION;
    state.ControlMask = PROCESS_POWER_THROTTLING_EXECUTION_SPEED;
    state.StateMask = 0;  // no throttling
    SetProcessInformation(GetCurrentProcess(), ProcessPowerThrottling, &state, sizeof state);
}

void endBusy() {
    SetThreadExecutionState(ES_CONTINUOUS);
    PROCESS_POWER_THROTTLING_STATE state{};
    state.Version = PROCESS_POWER_THROTTLING_CURRENT_VERSION;  // no control mask: Windows decides again
    SetProcessInformation(GetCurrentProcess(), ProcessPowerThrottling, &state, sizeof state);
}

std::vector<Volume> removableVolumes() {
    std::vector<Volume> out;
    // A card reader with no card in it must not make Windows ask to insert one.
    DWORD old_mode = 0;
    SetThreadErrorMode(SEM_FAILCRITICALERRORS, &old_mode);
    const DWORD drives = GetLogicalDrives();
    for (int i = 0; i < 26; ++i) {
        if (!(drives & (1u << i))) continue;
        const wchar_t letter = static_cast<wchar_t>(L'A' + i);
        const wchar_t root[] = {letter, L':', L'\\', 0};
        const UINT type = GetDriveTypeW(root);
        // USB sticks and memory cards, and a USB disk that calls itself fixed;
        // not the computer's own disks, network shares or CD drives.
        if (type != DRIVE_REMOVABLE && !(type == DRIVE_FIXED && onUsb(letter))) continue;
        wchar_t label[MAX_PATH + 1] = {}, format[MAX_PATH + 1] = {};
        DWORD flags = 0;
        if (!GetVolumeInformationW(root, label, MAX_PATH + 1, nullptr, nullptr, &flags, format, MAX_PATH + 1))
            continue;  // no card in the reader
        if (flags & FILE_READ_ONLY_VOLUME) continue;
        Volume v;
        // As Explorer shows it: "ELEGOO (E:)", or "USB Drive (E:)" when it has no name.
        v.name = (label[0] ? narrow(label) : std::string("USB Drive")) + " (" + static_cast<char>('A' + i) + ":)";
        v.path = narrow(root);
        v.format = narrow(format);
        ULARGE_INTEGER free{};
        if (GetDiskFreeSpaceExW(root, &free, nullptr, nullptr)) v.free_bytes = free.QuadPart;
        out.push_back(v);
    }
    SetThreadErrorMode(old_mode, nullptr);
    return out;
}

bool ejectVolume(const std::string& path, std::string* error) {
    auto fail = [&](const std::string& why) {
        if (error) *error = why;
        return false;
    };
    const std::wstring w = wide(path);
    if (w.size() < 2 || w[1] != L':') return fail("That is not a drive letter.");
    const wchar_t letter = static_cast<wchar_t>(std::towupper(w[0]));
    // FruitSlicer's own working folder on that drive would hold it.
    wchar_t cwd[MAX_PATH + 1] = {};
    if (GetCurrentDirectoryW(MAX_PATH + 1, cwd) >= 2 && std::towupper(cwd[0]) == letter && cwd[1] == L':') {
        wchar_t temp[MAX_PATH + 1] = {};
        if (GetTempPathW(MAX_PATH + 1, temp)) SetCurrentDirectoryW(temp);
    }
    STORAGE_DEVICE_NUMBER volume{};
    if (!deviceNumber(volumeDevice(letter).c_str(), volume))
        return fail("Windows doesn't list it as a drive any more: it may be ejected already.");
    const DEVINST disk = diskOf(volume);
    if (!disk) return fail("Windows did not say which disk the drive is on.");
    // As Explorer's Eject does it: the USB device that holds the disk, else the disk itself.
    DEVINST parent = 0;
    const DEVINST targets[2] = {CM_Get_Parent(&parent, disk, 0) == CR_SUCCESS ? parent : disk, disk};
    PNP_VETO_TYPE veto = PNP_VetoTypeUnknown;
    wchar_t veto_name[MAX_PATH] = {};
    for (DEVINST target : targets)
        for (int attempt = 0; attempt < 3; ++attempt) {  // a handle closing a moment late is common
            veto = PNP_VetoTypeUnknown;
            veto_name[0] = 0;
            if (CM_Request_Device_EjectW(target, &veto, veto_name, MAX_PATH, 0) == CR_SUCCESS &&
                veto == PNP_VetoTypeUnknown)
                return true;
            Sleep(300);
        }
    if (veto == PNP_VetoOutstandingOpen)
        return fail("A program still has a file on it open. Close it there, then eject again.");
    return fail("Windows would not eject it. Close any program or window that uses it, then try again.");
}

void revealFile(const std::string& path) {
    namespace fs = std::filesystem;
    const fs::path p = fs::path(wide(path)).make_preferred();  // the shell wants backslashes
    const HRESULT com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    bool shown = false;
    if (PIDLIST_ABSOLUTE item = ILCreateFromPathW(p.c_str())) {
        shown = SUCCEEDED(SHOpenFolderAndSelectItems(item, 0, nullptr, 0));
        ILFree(item);
    }
    if (!shown) {  // the file is gone: its folder, if that is there
        const std::wstring dir = p.parent_path().wstring();
        if (!dir.empty()) ShellExecuteW(nullptr, L"open", dir.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    }
    if (SUCCEEDED(com)) CoUninitialize();
}

}  // namespace gui::platform
