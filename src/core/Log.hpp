#pragma once

#include <Windows.h>
#include <cstdio>
#include <cstdarg>
#include <string>
#include <mutex>

namespace Log
{
    inline std::mutex& mtx() { static std::mutex m; return m; }

    inline std::wstring ModuleDir()
    {
        wchar_t path[MAX_PATH]{};
        HMODULE self = nullptr;
        GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           reinterpret_cast<LPCWSTR>(&ModuleDir), &self);
        GetModuleFileNameW(self, path, MAX_PATH);
        std::wstring s(path);
        const size_t p = s.find_last_of(L"\\/");
        return (p == std::wstring::npos) ? s : s.substr(0, p);
    }

    inline std::wstring UserProfile()
    {
        wchar_t buf[512]{};
        const DWORD n = GetEnvironmentVariableW(L"USERPROFILE", buf, 512);
        return (n > 0 && n < 512) ? std::wstring(buf, n) : std::wstring();
    }

    inline std::wstring TempDir()
    {
        wchar_t buf[MAX_PATH + 1]{};
        const DWORD n = GetTempPathW(MAX_PATH, buf);
        return (n > 0) ? std::wstring(buf, n) : std::wstring();
    }

    inline HANDLE TryOpen(const std::wstring& path)
    {
        if (path.empty()) return INVALID_HANDLE_VALUE;
        return CreateFileW(path.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ,
                           nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    }

    inline std::wstring PickPath()
    {
        std::wstring cand[3];

        cand[0] = ModuleDir() + L"\\NewDLL.log";

        const std::wstring up = UserProfile();
        if (!up.empty())
        {
            const std::wstring dir =
                up + L"\\AppData\\Local\\Packages\\Microsoft.MinecraftUWP_8wekyb3d8bbwe\\RoamingState\\NewDLL";
            CreateDirectoryW(dir.c_str(), nullptr);   
            cand[1] = dir + L"\\NewDLL.log";
        }

        std::wstring tmp = TempDir();
        while (!tmp.empty() && (tmp.back() == L'\\' || tmp.back() == L'/'))
            tmp.pop_back();
        if (!tmp.empty())
            cand[2] = tmp + L"\\NewDLL.log";

        for (int i = 0; i < 3; ++i)
        {
            HANDLE h = TryOpen(cand[i]);
            if (h != INVALID_HANDLE_VALUE)
            {
                CloseHandle(h);
                return cand[i];
            }
        }
        return std::wstring();
    }

    inline std::wstring LogPath()
    {
        static std::wstring p = PickPath();
        return p;
    }

    inline std::string LogPathAnsi()
    {
        const std::wstring w = LogPath();
        if (w.empty()) return std::string("<none writable>");
        char a[MAX_PATH * 2]{};
        const int n = WideCharToMultiByte(CP_ACP, 0, w.c_str(), -1, a, sizeof(a) - 1, nullptr, nullptr);
        return (n > 0) ? std::string(a) : std::string("<convert failed>");
    }

    inline void Raw(const char* line)
    {
        std::lock_guard<std::mutex> lk(mtx());
        HANDLE h = TryOpen(LogPath());
        if (h == INVALID_HANDLE_VALUE) return;
        SetFilePointer(h, 0, nullptr, FILE_END);
        DWORD written = 0;
        WriteFile(h, line, static_cast<DWORD>(strlen(line)), &written, nullptr);
        CloseHandle(h);
    }

    inline void Line(const char* level, const char* fmt, ...)
    {
        SYSTEMTIME st{};
        GetLocalTime(&st);

        char body[2048]{};
        va_list ap;
        va_start(ap, fmt);
        _vsnprintf_s(body, sizeof(body), _TRUNCATE, fmt, ap);
        va_end(ap);

        char out[2200]{};
        _snprintf_s(out, sizeof(out), _TRUNCATE,
                    "[%02d:%02d:%02d.%03d] [%s] %s\r\n",
                    st.wHour, st.wMinute, st.wSecond, st.wMilliseconds, level, body);
        Raw(out);
    }

    inline void info(const char* fmt, ...)
    {
        char body[2048]{};
        va_list ap; va_start(ap, fmt);
        _vsnprintf_s(body, sizeof(body), _TRUNCATE, fmt, ap);
        va_end(ap);
        Line("info", "%s", body);
    }
    inline void warn(const char* fmt, ...)
    {
        char body[2048]{};
        va_list ap; va_start(ap, fmt);
        _vsnprintf_s(body, sizeof(body), _TRUNCATE, fmt, ap);
        va_end(ap);
        Line("warn", "%s", body);
    }
    inline void error(const char* fmt, ...)
    {
        char body[2048]{};
        va_list ap; va_start(ap, fmt);
        _vsnprintf_s(body, sizeof(body), _TRUNCATE, fmt, ap);
        va_end(ap);
        Line("error", "%s", body);
    }
}
