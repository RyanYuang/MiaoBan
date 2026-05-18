#ifndef _SYSTEM_INFO_H_
#define _SYSTEM_INFO_H_

#include <string>

#include <esp_err.h>
#include <freertos/FreeRTOS.h>

#include <cstddef>

struct InternalHeapStats {
    size_t free_bytes = 0;
    size_t largest_block = 0;
    size_t min_free_bytes = 0;
};

class SystemInfo {
public:
    static size_t GetFlashSize();
    static size_t GetMinimumFreeHeapSize();
    static size_t GetFreeHeapSize();
    static std::string GetMacAddress();
    static std::string GetChipModelName();
    static std::string GetUserAgent();
    static esp_err_t PrintTaskCpuUsage(TickType_t xTicksToWait);
    static void PrintTaskList();
    static InternalHeapStats GetInternalHeapStats();
    static void LogInternalHeap(const char* label);
    static void PrintHeapStats();
    static void PrintPmLocks();
};

#endif // _SYSTEM_INFO_H_
