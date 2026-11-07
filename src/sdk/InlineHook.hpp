#pragma once

#include <Windows.h>
#include <intrin.h>
#include <cstdint>
#include <cstring>
#include <initializer_list>

namespace InlineHook
{
    namespace Detail
    {
        
        inline void EmitAbs(uint8_t*& p, uint8_t modrm, const void* dst)
        {
            p[0] = 0xFF;
            p[1] = modrm;                 
            const uint32_t zero = 0;
            std::memcpy(p + 2, &zero, 4);
            const uint64_t a = reinterpret_cast<uint64_t>(dst);
            std::memcpy(p + 6, &a, 8);
            p += 14;
        }

        inline void Emit(std::initializer_list<uint8_t> bs, uint8_t*& p)
        {
            for (uint8_t b : bs) *p++ = b;
        }
    }

    inline void* Install(void* target, void* helper, size_t steal, void** publish = nullptr)
    {
        if (!target || !helper || steal < 16 || steal > 64) return nullptr;

        auto* t = static_cast<uint8_t*>(target);

        auto* tramp = static_cast<uint8_t*>(::VirtualAlloc(
            nullptr, steal + 32, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
        if (!tramp) return nullptr;

        std::memcpy(tramp, t, steal);
        {
            uint8_t* q = tramp + steal;
            Detail::EmitAbs(q, 0x25, reinterpret_cast<void*>(t + steal));
        }

        auto* relay = static_cast<uint8_t*>(::VirtualAlloc(
            nullptr, 256, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
        if (!relay) { ::VirtualFree(tramp, 0, MEM_RELEASE); return nullptr; }

        uint8_t* p = relay;
        using Detail::Emit;

        Emit({ 0x55 }, p);                                       
        Emit({ 0x48, 0x89, 0xE5 }, p);                           
        Emit({ 0x48, 0x81, 0xEC, 0xC0, 0x00, 0x00, 0x00 }, p);   
        Emit({ 0x48, 0x83, 0xE4, 0xF0 }, p);                     

        Emit({ 0x48, 0x89, 0x4C, 0x24, 0x20 }, p);               
        Emit({ 0x48, 0x89, 0x54, 0x24, 0x28 }, p);               
        Emit({ 0x4C, 0x89, 0x44, 0x24, 0x30 }, p);               
        Emit({ 0x4C, 0x89, 0x4C, 0x24, 0x38 }, p);               
        Emit({ 0x48, 0x89, 0x44, 0x24, 0x40 }, p);               
        Emit({ 0x4C, 0x89, 0x54, 0x24, 0x48 }, p);               
        Emit({ 0x4C, 0x89, 0x5C, 0x24, 0x50 }, p);               
        
        Emit({ 0x0F, 0x11, 0x44, 0x24, 0x60 }, p);               
        Emit({ 0x0F, 0x11, 0x4C, 0x24, 0x70 }, p);               
        Emit({ 0x0F, 0x11, 0x54, 0x24, 0x80 }, p);               
        Emit({ 0x0F, 0x11, 0x5C, 0x24, 0x90 }, p);               
        Emit({ 0x0F, 0x11, 0x64, 0x24, 0xA0 }, p);               
        Emit({ 0x0F, 0x11, 0x6C, 0x24, 0xB0 }, p);               

        Emit({ 0x48, 0xB8 }, p);                                 
        {
            const uint64_t ha = reinterpret_cast<uint64_t>(helper);
            std::memcpy(p, &ha, 8);
            p += 8;
        }
        Emit({ 0xFF, 0xD0 }, p);                                 

        Emit({ 0x48, 0x8B, 0x4C, 0x24, 0x20 }, p);               
        Emit({ 0x48, 0x8B, 0x54, 0x24, 0x28 }, p);               
        Emit({ 0x4C, 0x8B, 0x44, 0x24, 0x30 }, p);               
        Emit({ 0x4C, 0x8B, 0x4C, 0x24, 0x38 }, p);               
        Emit({ 0x48, 0x8B, 0x44, 0x24, 0x40 }, p);               
        Emit({ 0x4C, 0x8B, 0x54, 0x24, 0x48 }, p);               
        Emit({ 0x4C, 0x8B, 0x5C, 0x24, 0x50 }, p);               
        Emit({ 0x0F, 0x10, 0x44, 0x24, 0x60 }, p);               
        Emit({ 0x0F, 0x10, 0x4C, 0x24, 0x70 }, p);               
        Emit({ 0x0F, 0x10, 0x54, 0x24, 0x80 }, p);               
        Emit({ 0x0F, 0x10, 0x5C, 0x24, 0x90 }, p);               
        Emit({ 0x0F, 0x10, 0x64, 0x24, 0xA0 }, p);               
        Emit({ 0x0F, 0x10, 0x6C, 0x24, 0xB0 }, p);               

        Emit({ 0xC9 }, p);                                       

        Detail::EmitAbs(p, 0x25, tramp);                         

        if (publish) *publish = tramp;
        
        DWORD old = 0;
        if (!::VirtualProtect(t, steal, PAGE_EXECUTE_READWRITE, &old))
        {
            ::VirtualFree(relay, 0, MEM_RELEASE);
            ::VirtualFree(tramp, 0, MEM_RELEASE);
            return nullptr;
        }

        {
            uint8_t blob[16];
            uint8_t* q = blob;
            Detail::EmitAbs(q, 0x25, relay);                     
            blob[14] = 0x90;                                     
            blob[15] = 0x90;

            if ((reinterpret_cast<uintptr_t>(t) & 15) == 0)
            {
                
                uint64_t expect[2];
                std::memcpy(expect, t, 16);
                uint64_t wanted[2];
                std::memcpy(wanted, blob, 16);
                ::InterlockedCompareExchange128(
                    reinterpret_cast<volatile LONG64*>(t),
                    static_cast<LONG64>(wanted[1]),
                    static_cast<LONG64>(wanted[0]),
                    reinterpret_cast<LONG64*>(expect));
            }
            else
            {
                std::memcpy(t, blob, 16);                        
            }

            for (size_t i = 16; i < steal; ++i) t[i] = 0x90;     
        }

        DWORD tmp = 0;
        ::VirtualProtect(t, steal, old, &tmp);
        
        ::FlushInstructionCache(::GetCurrentProcess(), t, steal);
        ::FlushInstructionCache(::GetCurrentProcess(), relay, 256);
        ::FlushInstructionCache(::GetCurrentProcess(), tramp, steal + 32);

        if (t[0] != 0xFF || t[1] != 0x25 ||
            t[2] != 0x00 || t[3] != 0x00 || t[4] != 0x00 || t[5] != 0x00)
        {
            ::VirtualFree(relay, 0, MEM_RELEASE);
            ::VirtualFree(tramp, 0, MEM_RELEASE);
            return nullptr;
        }
        const void* back = nullptr;
        std::memcpy(&back, t + 6, 8);
        if (back != relay)
        {
            ::VirtualFree(relay, 0, MEM_RELEASE);
            ::VirtualFree(tramp, 0, MEM_RELEASE);
            return nullptr;
        }

        return tramp;
    }
}
