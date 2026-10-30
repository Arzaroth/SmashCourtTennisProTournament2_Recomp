#include "game_overrides.h"
#include "sct2_vu1_port.h"
#include "ps2_recompiled_functions.h"
#include "ps2_runtime.h"
#include "ps2_runtime_macros.h"
#include "runtime/ee_scheduler.h"
#include "runtime/native/native_vif1.h"
#include "runtime/native/native_vu1_stream.h"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <set>
#include <vector>

namespace
{
    // The native target by default: the GS translated to OpenGL at twice the PS2's resolution, the IOP modules
    // recompiled (scripts/recomp_iop.sh), the SPU2 and the IOP on the host clock, VIF1 and the VU1 programs on their own
    // thread (the practice court sends much more VU1 work than a match). The runtime reads these switches at
    // different points of its start, some before game overrides apply, so they are set before main; a value already
    // in the environment wins.
    [[maybe_unused]] const bool kNativeDefaults = []
    {
        constexpr const char *kDefaults[][2] = {{"PS2X_GS_RENDERER", "native"}, {"PS2X_GS_SCALE", "2"},
                                                {"PS2X_IOP_NATIVE", "1"}, {"PS2X_SPU2_WALLCLOCK", "1"},
                                                {"PS2X_IOP_WALLCLOCK", "1"}, {"PS2X_VIF1_THREAD", "1"}};
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

    // The frame wait: FlushCache, CancelWakeupThread(main), SleepThread until the vblank handler (0x178D98) has counted
    // the frame interval down and wakes the main thread. On the native frame loop the main thread waits for the host
    // frame deadline instead, the loader and sound threads running meanwhile, and the wake-up charges the EE the real
    // time of the frame and presents it. The handler's wake-ups are cancelled each frame as before.
    constexpr uint32_t kFrameWait = 0x00178EC8u;
    constexpr uint32_t kMainThreadId = 0x00463358u;

    // VU1 off the emulated path: every microprogram runs on the native sandbox and what it sends goes to the GL GS
    // back end with its register writes, in stream order. The game's frame ends at its frame wait, after it has sent the
    // frame. SCT2_NATIVE_VU1=0 leaves the programs on VU1.
    bool g_nativeVu1 = false;
    bool g_nativeVu1OnVif1Thread = false;

    // SCT2_VU1_STATS=1: the VU1 programs the game starts (resident microcode, start PC), counted and printed every
    // 600 frames, to tell which programs a scene runs.
    bool g_vu1Stats = false;
    std::map<std::pair<uint64_t, uint32_t>, uint64_t> g_vu1Calls;

    void reportVu1Calls()
    {
        static uint32_t s_frames = 0;
        if (!g_vu1Stats || ++s_frames % 600u != 0u)
            return;
        std::cerr << "[sct2] VU1 programs over 600 frames:";
        for (const auto &[entry, count] : g_vu1Calls)
            std::cerr << " " << std::hex << (entry.first >> 48) << ":" << entry.second << std::dec << "=" << count;
        std::cerr << std::endl;
        g_vu1Calls.clear();
    }

    void nativeVu1FrameEnd()
    {
        if (!g_nativeVu1)
            return;
        const auto report = []
        {
            reportVu1Calls();
            sct2::vu1PortReport();
        };
        if (g_nativeVu1OnVif1Thread)
            ps2x::native::runOnVif1Thread(report);
        else
            report();
        if (g_nativeVu1OnVif1Thread)
            ps2x::native::runOnVif1Thread([] { ps2x::native::Vu1StreamAdapter::instance().frameEnd(); });
        else
            ps2x::native::Vu1StreamAdapter::instance().frameEnd();
    }

    void installNativeVu1(PS2Runtime &runtime)
    {
        const char *gs = std::getenv("PS2X_GS_RENDERER");
        const char *value = std::getenv("SCT2_NATIVE_VU1");
        if (!(gs && std::strcmp(gs, "native") == 0) || (value && value[0] == '0'))
            return;
        ps2x::native::Vu1StreamAdapter::Config config;
        config.screen.originX = 2048.0f - 320.0f;
        config.screen.originY = 2048.0f - 224.0f;
        config.screen.width = 640.0f;
        config.screen.height = 448.0f;
        g_vu1Stats = std::getenv("SCT2_VU1_STATS") != nullptr;
        config.choose = [](ps2x::native::Vu1StreamAdapter &adapter, uint32_t pc)
        {
            if (g_vu1Stats)
                ++g_vu1Calls[{fnv1a(adapter.code(), 16u * 1024u), pc}];
            return sct2::vu1Ported(adapter, pc) ? ps2x::native::Vu1StreamAdapter::Choice::Port
                                                : ps2x::native::Vu1StreamAdapter::Choice::Sandbox;
        };
        config.draw = sct2::vu1PortDraw;
        config.gsWrites = true;
        g_nativeVu1OnVif1Thread = runtime.memory().vif1Threaded();
        config.onVif1Thread = g_nativeVu1OnVif1Thread;
        ps2x::native::Vu1StreamAdapter::instance().install(config);
        g_nativeVu1 = true;
        std::cerr << "[sct2] native VU1: every program on the sandbox, drawn by the GS back end"
                  << (g_nativeVu1OnVif1Thread ? ", on the VIF1 thread" : ", on the game thread") << std::endl;
    }

    void frameWaitVu1FrameEnd(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        nativeVu1FrameEnd();
        sub_00178EC8_0x178ec8(rdram, ctx, runtime);
    }

    void nativeFrameEnd(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        nativeVu1FrameEnd();
        using Clock = std::chrono::steady_clock;
        static Clock::time_point s_deadline = Clock::now();
        static Clock::time_point s_previousWake = Clock::now();
        static uint64_t s_previousCycle = runtime->eeScheduler().currentCycle();

        const uint32_t ra = getRegU32(ctx, 31);
        uint32_t mainThread;
        std::memcpy(&mainThread, rdram + kMainThreadId, sizeof(mainThread));
        SET_GPR_U32(ctx, 4, mainThread);
        sub_00195EF0_0x195ef0(rdram, ctx, runtime);

        const uint32_t cap = runtime->framePacer().cap();
        const Clock::time_point now = Clock::now();
        s_deadline = cap == 0u ? now : std::max(s_deadline + std::chrono::microseconds(1000000 / cap), now);
        runtime->eeScheduler().waitHostDeadline(
            s_deadline,
            [runtime]
            {
                EeScheduler &ee = runtime->eeScheduler();
                const Clock::time_point wake = Clock::now();
                const double seconds = std::chrono::duration<double>(wake - s_previousWake).count();
                const uint64_t real = static_cast<uint64_t>(std::min(seconds, 0.1) * EeScheduler::kEeClockHz);
                const uint64_t accounted = ee.currentCycle() - s_previousCycle;
                if (real > accounted)
                    ee.accountCycles(static_cast<uint32_t>(real - accounted));
                s_previousWake = wake;
                s_previousCycle = ee.currentCycle();
                ee.presentGuestFrame();
                runtime->noteGameFrame();
            },
            [ra](R5900Context &resume) { resume.pc = ra; });
    }

    void applySmashCourtTennis2(PS2Runtime &runtime)
    {
        runtime.configureReservedMemory(kReservedBase, kReservedTop, kMainStackBottom);
        runtime.registerFunction(kSceSifSearchModuleByName, searchModuleByNameUnknown);
        runtime.registerFunction(kNetCrossingTest, netCrossingTestSkippingOverflow);
        installNativeVu1(runtime);
        if (const char *frames = std::getenv("SCT2_NATIVE_FRAMES"); !(frames && frames[0] == '0'))
        {
            runtime.eeScheduler().setNativeFrames(true);
            runtime.registerFunction(kFrameWait, nativeFrameEnd);
        }
        else if (g_nativeVu1)
            runtime.registerFunction(kFrameWait, frameWaitVu1FrameEnd);

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
