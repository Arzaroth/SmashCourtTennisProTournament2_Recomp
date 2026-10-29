#include "game_overrides.h"
#include "ps2_recompiled_functions.h"
#include "ps2_runtime.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <set>
#include <vector>

namespace
{
    // The native target by default: the GS translated to OpenGL, the IOP modules recompiled (scripts/recomp_iop.sh),
    // the SPU2 and the IOP on the host clock. The runtime reads these switches at different points of its start, some
    // before game overrides apply, so they are set before main; a value already in the environment wins.
    [[maybe_unused]] const bool kNativeDefaults = []
    {
        constexpr const char *kDefaults[][2] = {{"PS2X_GS_RENDERER", "native"}, {"PS2X_IOP_NATIVE", "1"},
                                                {"PS2X_SPU2_WALLCLOCK", "1"}, {"PS2X_IOP_WALLCLOCK", "1"}};
        for (const auto &[name, value] : kDefaults)
        {
            if (std::getenv(name))
                continue;
#ifdef _WIN32
            _putenv_s(name, value);
#else
            setenv(name, value, 0);
#endif
        }
        return true;
    }();

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

    constexpr uint32_t kOverlayBase = 0x0046EA80u;
    constexpr uint32_t kStaytrashFileOffset = 0x0010D510u;
    constexpr uint32_t kStaytrashSize = 0x000393B7u;
    constexpr uint32_t kTaskRunner = 0x00176760u;
    constexpr uint32_t kLoaderRequest = 0x00464640u;
    constexpr uint32_t kGuardSize = 64u;

    struct OverlayCapture
    {
        std::filesystem::path dir;
        std::vector<uint8_t> boot;
        std::set<uint64_t> seen;
        uint32_t overlayEnd = 0u;
    };
    OverlayCapture *g_capture = nullptr;

    uint64_t fnv1a(const uint8_t *data, size_t size)
    {
        uint64_t hash = 0xCBF29CE484222325ull;
        for (size_t i = 0; i < size; ++i)
            hash = (hash ^ data[i]) * 0x100000001B3ull;
        return hash;
    }

    // PS2X_SCT2_OVERLAY_DUMP=<dir>: before each run of the per-frame task list, write the bytes of .staytrash that
    // differ from the ELF's boot code as <dir>/ovl_<hash>.bin, once per overlay: the hash covers the guard bytes the
    // runtime tells overlay modules apart by, so the overlay's own data changing does not make a new capture.
    void captureOverlay(const uint8_t *rdram)
    {
        uint32_t end = kOverlayBase;
        for (uint32_t offset = 0u; offset + 4u <= g_capture->boot.size(); offset += 4u)
        {
            const uint32_t address = kOverlayBase + offset;
            uint32_t original = 0u;
            std::memcpy(&original, g_capture->boot.data() + offset, 4u);
            uint32_t now = 0u;
            std::memcpy(&now, rdram + address, 4u);
            if (now != original)
                end = address + 4u;
        }
        if (end == kOverlayBase)
            return;
        // Overlays run past .staytrash into memory the ELF does not cover; the decompressor's request gives their size.
        end = std::max(end, g_capture->overlayEnd);
        const uint64_t hash = fnv1a(rdram + kOverlayBase, kGuardSize);
        if (!g_capture->seen.insert(hash).second)
            return;
        char name[64];
        std::snprintf(name, sizeof(name), "ovl_%016llx.bin", static_cast<unsigned long long>(hash));
        std::ofstream out(g_capture->dir / name, std::ios::binary);
        out.write(reinterpret_cast<const char *>(rdram + kOverlayBase), end - kOverlayBase);
        std::cerr << "[sct2] overlay 0x" << std::hex << kOverlayBase << "-0x" << end << " -> " << name << std::dec
                  << std::endl;
    }

    void taskRunnerCapturing(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        uint32_t size = 0u;
        uint32_t destination = 0u;
        std::memcpy(&size, rdram + kLoaderRequest + 0x8u, 4u);
        std::memcpy(&destination, rdram + kLoaderRequest + 0xCu, 4u);
        if ((destination & PS2_RAM_MASK) == kOverlayBase && size != 0u && size < 0x400000u)
            g_capture->overlayEnd = kOverlayBase + ((size + 3u) & ~3u);
        // The decompressor thread fills an overlay over several frames and sets the status when it is done.
        if (rdram[kLoaderRequest + 0x1Eu] != 0u)
            captureOverlay(rdram);
        sub_00176760_0x176760(rdram, ctx, runtime);
    }

    void startOverlayCapture(PS2Runtime &runtime, const std::filesystem::path &elf, const char *dir)
    {
        static OverlayCapture capture;
        capture.dir = dir;
        std::filesystem::create_directories(capture.dir);
        capture.boot.resize(kStaytrashSize);
        std::ifstream in(elf, std::ios::binary);
        in.seekg(kStaytrashFileOffset);
        in.read(reinterpret_cast<char *>(capture.boot.data()), kStaytrashSize);
        g_capture = &capture;
        runtime.registerFunction(kTaskRunner, taskRunnerCapturing);
        std::cerr << "[sct2] capturing .staytrash overlays into " << capture.dir << std::endl;
    }

    constexpr uint32_t kNetCrossingTest = 0x0010FB20u;

    bool overflowed(uint32_t bits) { return ((bits >> 23) & 0xFFu) >= 0xE0u; }

    // A player who has not moved gets a previous point of 0/0 * 0.1, which overflows to the float maximum, and the
    // VU0 test then reports the segment from there as crossing the net: the player lands in the net. The console
    // keeps the player in place, so such a segment is a miss.
    void netCrossingTestSkippingOverflow(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        uint32_t from[3];
        std::memcpy(from, rdram + (getRegU32(ctx, 4) & 0x1FFFFFFu), sizeof(from));
        if (overflowed(from[0]) || overflowed(from[2]))
        {
            setReturnS32(ctx, 0);
            ctx->pc = getRegU32(ctx, 31);
            return;
        }
        sub_0010FB20_0x10fb20(rdram, ctx, runtime);
    }

    void applySmashCourtTennis2(PS2Runtime &runtime)
    {
        runtime.configureReservedMemory(kReservedBase, kReservedTop, kMainStackBottom);
        runtime.registerFunction(kSceSifSearchModuleByName, searchModuleByNameUnknown);
        runtime.registerFunction(kNetCrossingTest, netCrossingTestSkippingOverflow);

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

        if (const char *dir = std::getenv("PS2X_SCT2_OVERLAY_DUMP"); dir && *dir)
            startOverlayCapture(runtime, paths.elfPath, dir);

        std::cerr << "[sct2] Smash Court Tennis Pro Tournament 2 overrides applied" << std::endl;
    }
}

PS2_REGISTER_GAME_OVERRIDE("Smash Court Tennis Pro Tournament 2 (SLUS-20933)", "SLUS_209.33", 0x00100000u, 0x97DCFAA3u, applySmashCourtTennis2)
