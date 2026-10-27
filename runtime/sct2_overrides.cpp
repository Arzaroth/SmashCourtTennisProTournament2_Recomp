#include "game_overrides.h"
#include "ps2_runtime.h"

#include <filesystem>
#include <iostream>

namespace
{
    // crt0 calls SetupThread(stack 0x01FF8000, size 0x8000): the main stack ends at the top of RAM, where the runtime
    // puts its interrupt and callback stacks by default. Keep them in the kernel RAM below the ELF instead.
    constexpr uint32_t kReservedBase = 0x00080000u;
    constexpr uint32_t kReservedTop = 0x00100000u;
    constexpr uint32_t kMainStackBottom = 0x01FF8000u;

    constexpr uint32_t kSceSifSearchModuleByName = 0x0019EBD0u;
    constexpr int32_t kKeUnknownModule = -202;

    // Without a loadfile RPC server the libkernel code binds sid 0x80000006 for ever. The game only searches for
    // cdvd_ee_driver right after rebooting the IOP, to stop an old copy before loading its own.
    void searchModuleByNameUnknown(uint8_t *, R5900Context *ctx, PS2Runtime *)
    {
        setReturnS32(ctx, kKeUnknownModule);
        ctx->pc = getRegU32(ctx, 31);
    }

    void applySmashCourtTennis2(PS2Runtime &runtime)
    {
        runtime.configureReservedMemory(kReservedBase, kReservedTop, kMainStackBottom);
        runtime.registerFunction(kSceSifSearchModuleByName, searchModuleByNameUnknown);

        // The game data sits in raw sectors outside the ISO 9660 file system, read by LSN.
        PS2Runtime::IoPaths paths = PS2Runtime::getIoPaths();
        const std::filesystem::path image = paths.elfDirectory / "disc.iso";
        std::error_code ec;
        if (std::filesystem::exists(image, ec))
        {
            paths.cdImage = image;
            PS2Runtime::setIoPaths(paths);
        }
        else
        {
            std::cerr << "[sct2] no " << image << ": raw sector reads will fail, run scripts/extract_iso.sh" << std::endl;
        }

        std::cerr << "[sct2] Smash Court Tennis Pro Tournament 2 overrides applied" << std::endl;
    }
}

PS2_REGISTER_GAME_OVERRIDE("Smash Court Tennis Pro Tournament 2 (SLUS-20933)", "SLUS_209.33", 0x00100000u, 0x97DCFAA3u, applySmashCourtTennis2)
