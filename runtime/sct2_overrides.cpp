#include "game_overrides.h"
#include "ps2_runtime.h"

#include <filesystem>
#include <iostream>

namespace
{
    void applySmashCourtTennis2(PS2Runtime &runtime)
    {
        (void)runtime;

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
