#pragma once

#include <Windows.h>
#include <cstdint>
#include <cstring>

namespace Mem
{
    inline uintptr_t Base()
    {
        static const uintptr_t s = reinterpret_cast<uintptr_t>(::GetModuleHandleW(nullptr));
        return s;
    }

    inline size_t ImageSize()
    {
        const uintptr_t b = Base();
        if (b < 0x10000) return 0;
        const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(b);
        if (dos->e_magic != IMAGE_DOS_SIGNATURE) return 0;
        const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(b + dos->e_lfanew);
        if (nt->Signature != IMAGE_NT_SIGNATURE) return 0;
        return nt->OptionalHeader.SizeOfImage;
    }

    inline bool SectionRange(const char* name, uintptr_t& start, size_t& size)
    {
        start = 0;
        size  = 0;
        const uintptr_t b = Base();
        if (b < 0x10000) return false;

        const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(b);
        if (dos->e_magic != IMAGE_DOS_SIGNATURE) return false;
        const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(b + dos->e_lfanew);
        if (nt->Signature != IMAGE_NT_SIGNATURE) return false;

        const auto* sec = IMAGE_FIRST_SECTION(nt);
        for (int i = 0; i < nt->FileHeader.NumberOfSections; ++i)
        {
            if (std::strncmp(reinterpret_cast<const char*>(sec[i].Name), name, 8) == 0)
            {
                start = b + sec[i].VirtualAddress;
                size  = sec[i].Misc.VirtualSize;
                return true;
            }
        }
        return false;
    }

    inline uintptr_t Addr(uintptr_t rva) { return Base() + rva; }

    inline bool RangeOk(uintptr_t p, size_t n, bool needWrite)
    {
        if (p < 0x10000 || n == 0) return false;
        const uintptr_t end = p + n;
        if (end < p) return false;                 

        uintptr_t cur = p & ~static_cast<uintptr_t>(0xFFF);
        while (cur < end)
        {
            MEMORY_BASIC_INFORMATION mbi{};
            if (::VirtualQuery(reinterpret_cast<LPCVOID>(cur), &mbi, sizeof(mbi)) == 0)
                return false;
            if (mbi.State != MEM_COMMIT) return false;
            if (mbi.Protect & PAGE_GUARD) return false;
            if (mbi.Protect & PAGE_NOACCESS) return false;

            const DWORD rw = mbi.Protect & 0xFF;
            const bool ok =
                rw == PAGE_READONLY  || rw == PAGE_READWRITE ||
                rw == PAGE_EXECUTE_READ || rw == PAGE_EXECUTE_READWRITE ||
                rw == PAGE_WRITECOPY || rw == PAGE_EXECUTE_WRITECOPY ||
                rw == PAGE_EXECUTE;          
            if (!ok) return false;
            if (needWrite && !(rw == PAGE_READWRITE || rw == PAGE_EXECUTE_READWRITE ||
                               rw == PAGE_WRITECOPY || rw == PAGE_EXECUTE_WRITECOPY))
                return false;

            const uintptr_t next = reinterpret_cast<uintptr_t>(mbi.BaseAddress) + mbi.RegionSize;
            if (next <= cur) return false;
            cur = next;
        }
        return true;
    }

    template <typename T>
    inline bool Read(uintptr_t p, T& out)
    {
        if (p < 0x10000) return false;

        SIZE_T got = 0;
        if (!::ReadProcessMemory(::GetCurrentProcess(), reinterpret_cast<LPCVOID>(p),
                                 &out, sizeof(T), &got))
            return false;
        return got == sizeof(T);
    }

    template <typename T>
    inline T ReadOr(uintptr_t p, T def = T{})
    {
        T v{};
        return Read<T>(p, v) ? v : def;
    }

    template <typename T>
    inline bool Write(uintptr_t p, const T& v)
    {
        if (p < 0x10000) return false;
        if (!RangeOk(p, sizeof(T), true)) return false;      

        DWORD old = 0;
        if (!::VirtualProtect(reinterpret_cast<void*>(p), sizeof(T),
                              PAGE_READWRITE, &old))
            return false;

        std::memcpy(reinterpret_cast<void*>(p), &v, sizeof(T));

        DWORD tmp = 0;
        ::VirtualProtect(reinterpret_cast<void*>(p), sizeof(T), old, &tmp);
        return true;
    }

    template <typename T>
    inline bool WriteProt(uintptr_t p, const T& v)
    {
        if (p < 0x10000) return false;
        DWORD old = 0;
        if (!::VirtualProtect(reinterpret_cast<void*>(p), sizeof(T),
                              PAGE_EXECUTE_READWRITE, &old))
            return false;
        std::memcpy(reinterpret_cast<void*>(p), &v, sizeof(T));
        ::VirtualProtect(reinterpret_cast<void*>(p), sizeof(T), old, &old);
        return true;
    }

    inline uintptr_t Scan(const char* pattern, uintptr_t start = 0, size_t size = 0)
    {
        uint8_t bytes[512]{};
        bool    wild[512]{};
        int     len = 0;

        auto hexv = [](char c) -> int {
            if (c >= '0' && c <= '9') return c - '0';
            if (c >= 'a' && c <= 'f') return c - 'a' + 10;
            if (c >= 'A' && c <= 'F') return c - 'A' + 10;
            return -1;
        };

        for (const char* p = pattern; *p && len < 512; )
        {
            while (*p == ' ') ++p;
            if (!*p) break;

            if (*p == '?')
            {
                bytes[len] = 0;
                wild[len]  = true;
                ++len;
                ++p;
                if (*p == '?') ++p;
                continue;
            }

            const int h = hexv(p[0]);
            const int l = (h >= 0) ? hexv(p[1]) : -1;
            if (h < 0 || l < 0) break;

            bytes[len] = static_cast<uint8_t>((h << 4) | l);
            wild[len]  = false;
            ++len;
            p += 2;
        }

        if (len < 4) return 0;

        if (start == 0 || size == 0)
        {
            if (!SectionRange(".text", start, size)) return 0;
        }

        const auto* s = reinterpret_cast<const uint8_t*>(start);
        const size_t n = size;

        for (size_t i = 0; i + static_cast<size_t>(len) <= n; ++i)
        {
            if (!wild[0] && s[i] != bytes[0]) continue;
            bool hit = true;
            for (int j = 1; j < len; ++j)
            {
                if (!wild[j] && s[i + j] != bytes[j]) { hit = false; break; }
            }
            if (hit) return start + i;
        }
        return 0;
    }
}
