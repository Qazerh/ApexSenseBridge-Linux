// Read-only native Windows HID diagnostic. No controller effects are written.
// Build on Linux with scripts/build-proton-hid-probe.sh (Wine development headers).
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <setupapi.h>
#include <ddk/hidsdi.h>
#include <ddk/hidpi.h>

static HANDLE console;
static HANDLE logFile = INVALID_HANDLE_VALUE;
static WCHAR logPath[2048];
static BYTE report[4096], previous[11];
static union { ULONG_PTR alignment; BYTE bytes[4096]; } detail;

static void print(const char *text) {
    DWORD count = 0, size = 0;
    while (text[size]) ++size;
    WriteFile(console, text, size, &count, NULL);
    if (logFile != INVALID_HANDLE_VALUE) {
        WriteFile(logFile, text, size, &count, NULL);
        if (size && text[size - 1] == '\n') FlushFileBuffers(logFile);
    }
}
static void number(ULONG value, unsigned base, unsigned width) {
    char buffer[32]; unsigned used = 0;
    do { buffer[used++] = "0123456789abcdef"[value % base]; value /= base; } while (value);
    while (used < width) buffer[used++] = '0';
    while (used) { char out[2] = {buffer[--used], 0}; print(out); }
}
static void wide(const WCHAR *text) {
    char buffer[2048];
    if (WideCharToMultiByte(CP_UTF8, 0, text, -1, buffer, sizeof(buffer), NULL, NULL)) print(buffer);
}
static void bytes(const BYTE *data, unsigned size) {
    for (unsigned i = 0; i < size && i < 16; ++i) { number(data[i], 16, 2); print(" "); }
}
static void error(const char *operation) {
    DWORD code = GetLastError(); print(operation); print(" failed, error="); number(code, 10, 0); print("\r\n");
}
static void values(PHIDP_PREPARSED_DATA prepared, DWORD length) {
    const USAGE axes[] = {0x30, 0x31, 0x32, 0x35, 0x33, 0x34};
    print("  HID-parsed axes X,Y,RX,RY,LT,RT: ");
    for (unsigned i = 0; i < 6; ++i) {
        ULONG value = 0;
        NTSTATUS status = HidP_GetUsageValue(HidP_Input, 1, 0, axes[i], &value,
                                             prepared, (CHAR *)report, length);
        if (status == HIDP_STATUS_SUCCESS) number(value, 10, 0); else print("?");
        print(" ");
    }
    print("\r\n");
}

static void inspect(const WCHAR *path) {
    HANDLE device = CreateFileW(path, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                NULL, OPEN_EXISTING, FILE_FLAG_OVERLAPPED, NULL);
    if (device == INVALID_HANDLE_VALUE) { error("Open DualSense"); return; }
    PHIDP_PREPARSED_DATA prepared = NULL;
    HIDP_CAPS caps;
    WCHAR string[256];
    if (HidD_GetProductString(device, string, sizeof(string))) { print("Product: "); wide(string); print("\r\n"); }
    if (HidD_GetSerialNumberString(device, string, sizeof(string))) { print("Serial: "); wide(string); print("\r\n"); }
    print("Path: "); wide(path); print("\r\n");
    if (!HidD_GetPreparsedData(device, &prepared)) { error("Get HID layout"); CloseHandle(device); return; }
    if (HidP_GetCaps(prepared, &caps) != HIDP_STATUS_SUCCESS) {
        print("HidP_GetCaps failed\r\n"); HidD_FreePreparsedData(prepared); CloseHandle(device); return;
    }
    print("HID lengths input/output/feature: ");
    number(caps.InputReportByteLength, 10, 0); print("/");
    number(caps.OutputReportByteLength, 10, 0); print("/");
    number(caps.FeatureReportByteLength, 10, 0); print("; expected 64/48/64\r\n");
    print("Usage page/usage: "); number(caps.UsagePage, 16, 4); print("/"); number(caps.Usage, 16, 4); print("\r\n");
    if (!caps.InputReportByteLength || caps.InputReportByteLength > sizeof(report)) {
        print("Invalid input size\r\n"); HidD_FreePreparsedData(prepared); CloseHandle(device); return;
    }
    report[0] = 1;
    if (HidD_GetInputReport(device, report, caps.InputReportByteLength)) {
        print("GET_INPUT_REPORT: "); bytes(report, caps.InputReportByteLength); print("\r\n");
        values(prepared, caps.InputReportByteLength);
    } else error("GET_INPUT_REPORT");

    const BYTE ids[] = {5, 9, 0x20};
    const ULONG sizes[] = {41, 20, 64};
    for (unsigned i = 0; i < 3; ++i) {
        for (unsigned n = 0; n < sizeof(report); ++n) report[n] = 0;
        report[0] = ids[i];
        print("FEATURE 0x"); number(ids[i], 16, 2); print(": ");
        if (HidD_GetFeature(device, report, sizes[i])) { bytes(report, sizes[i]); print("\r\n"); }
        else error("GET_FEATURE");
    }

    print("ReadFile input sample phase (12 seconds): move sticks and press buttons.\r\n");
    DWORD started = GetTickCount(), samples = 0, changes = 0, printed = 0, lastPrint = 0;
    OVERLAPPED pending = {0};
    pending.hEvent = CreateEventW(NULL, TRUE, FALSE, NULL);
    BOOL havePrevious = FALSE;
    while (GetTickCount() - started < 12000) {
        DWORD length = 0;
        ResetEvent(pending.hEvent);
        BOOL ready = ReadFile(device, report, caps.InputReportByteLength, &length, &pending);
        if (!ready && GetLastError() == ERROR_IO_PENDING) {
            if (WaitForSingleObject(pending.hEvent, 250) != WAIT_OBJECT_0) {
                CancelIoEx(device, &pending);
                GetOverlappedResult(device, &pending, &length, TRUE);
                continue;
            }
            ready = GetOverlappedResult(device, &pending, &length, FALSE);
        }
        if (!ready) { error("ReadFile"); break; }
        ++samples;
        BOOL different = !havePrevious;
        for (unsigned i = 0; i < 11 && i < length; ++i) {
            if (i != 7 && previous[i] != report[i]) different = TRUE; // Ignore sequence counter.
            previous[i] = report[i];
        }
        havePrevious = TRUE;
        if (different) ++changes;
        if (different && printed < 40 && GetTickCount() - lastPrint >= 200) {
            ++printed; lastPrint = GetTickCount();
            print("INPUT bytes="); number(length, 10, 0); print(" data="); bytes(report, length); print("\r\n");
            values(prepared, length);
        }
    }
    CloseHandle(pending.hEvent);
    print("ReadFile samples="); number(samples, 10, 0); print(" control changes="); number(changes, 10, 0); print("\r\n");
    HidD_FreePreparsedData(prepared);
    CloseHandle(device);
}

void WINAPI probe_entry(void) {
    GUID guid;
    SP_DEVICE_INTERFACE_DATA item = {0};
    DWORD count = 0, sony = 0;
    console = GetStdHandle(STD_OUTPUT_HANDLE);
    // Steam/Proton may detach a child console from the launcher's stdout.
    // Always keep a native file beside the executable as a second capture.
    DWORD pathLength = GetModuleFileNameW(NULL, logPath, 1900);
    if (pathLength && pathLength < 1900) {
        while (pathLength && logPath[pathLength - 1] != '\\' && logPath[pathLength - 1] != '/') --pathLength;
        const WCHAR name[] = L"proton-hid-probe-native.log";
        for (unsigned i = 0; i < sizeof(name) / sizeof(name[0]); ++i) logPath[pathLength + i] = name[i];
        logFile = CreateFileW(logPath, GENERIC_WRITE, FILE_SHARE_READ, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    }
    print("ApexSenseBridge native Proton HID probe v2 (read-only)\r\n");
    HidD_GetHidGuid(&guid);
    HDEVINFO devices = SetupDiGetClassDevsW(&guid, NULL, NULL, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    if (devices == INVALID_HANDLE_VALUE) { error("Enumerate HID devices"); ExitProcess(1); }
    item.cbSize = sizeof(item);
    for (DWORD index = 0; SetupDiEnumDeviceInterfaces(devices, NULL, &guid, index, &item); ++index) {
        SP_DEVICE_INTERFACE_DETAIL_DATA_W *path = (void *)detail.bytes;
        path->cbSize = sizeof(*path);
        if (!SetupDiGetDeviceInterfaceDetailW(devices, &item, path, sizeof(detail), NULL, NULL)) continue;
        HANDLE query = CreateFileW(path->DevicePath, 0, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
        if (query == INVALID_HANDLE_VALUE) continue;
        HIDD_ATTRIBUTES attributes = {0}; attributes.Size = sizeof(attributes);
        BOOL valid = HidD_GetAttributes(query, &attributes);
        CloseHandle(query);
        if (!valid) continue;
        ++count;
        if (attributes.VendorID != 0x054c || attributes.ProductID != 0x0ce6) continue;
        ++sony;
        print("\r\nDualSense "); number(sony, 10, 0); print(" VID:PID 054c:0ce6\r\n");
        inspect(path->DevicePath);
    }
    SetupDiDestroyDeviceInfoList(devices);
    print("\r\nEnumerated HID devices="); number(count, 10, 0); print(" DualSense devices="); number(sony, 10, 0); print("\r\n");
    if (!sony) print("No native DualSense HID interface found in this Proton session.\r\n");
    if (logFile != INVALID_HANDLE_VALUE) CloseHandle(logFile);
    ExitProcess(sony ? 0 : 2);
}
