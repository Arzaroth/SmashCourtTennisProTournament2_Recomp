#include "sct2_vu1_port.h"

#include "runtime/native/native_vu1_sandbox.h"
#include "runtime/native/native_vu1_stream.h"
#include "runtime/ps2_vu1_lifted_ops.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <vector>

namespace ps2x::vu1lift
{
    template <bool kSticky>
    uint64_t sct2Program(uint32_t entry, VU1State &state, uint8_t *data, uint32_t top, Vu1Kick out, void *user);
}

namespace sct2
{
    namespace
    {
        using ps2x::native::Vu1StreamAdapter;

        constexpr uint64_t kImageHash = 0x95d44bbc45fcd2feull;
        constexpr uint32_t kImageBytes = 1496u * 8u;
        constexpr uint32_t kVuMemory = 16u * 1024u;

        struct Stats
        {
            uint64_t ported = 0, checked = 0, mismatches = 0, unknown = 0;
            std::map<uint32_t, uint64_t> mismatched;
        };

        struct State
        {
            Vu1Port mode = Vu1Port::Off;
            uint64_t codeVersion = ~0ull;
            bool resident = false;
            Stats stats;
        } g_state;

        uint64_t fnv1a(const uint8_t *data, size_t size)
        {
            uint64_t hash = 0xCBF29CE484222325ull;
            for (size_t i = 0; i < size; ++i)
                hash = (hash ^ data[i]) * 0x100000001B3ull;
            return hash;
        }

        bool resident(Vu1StreamAdapter &adapter)
        {
            if (adapter.codeVersion() != g_state.codeVersion)
            {
                g_state.codeVersion = adapter.codeVersion();
                g_state.resident = fnv1a(adapter.code(), kImageBytes) == kImageHash;
            }
            return g_state.resident;
        }

        void kickToGif(void *user, const uint8_t *packet, uint32_t bytes)
        {
            static_cast<ps2x::native::GifToNative *>(user)->packet(packet, bytes);
        }

        void kickToList(void *user, const uint8_t *packet, uint32_t bytes)
        {
            static_cast<std::vector<std::vector<uint8_t>> *>(user)->emplace_back(packet, packet + bytes);
        }

        // The lifted program on the sandbox's VU state, its clock moved on by the cycles it took.
        uint64_t runLifted(Vu1StreamAdapter &adapter, uint32_t pc, ps2x::vu1lift::Vu1Kick kick, void *user)
        {
            VU1Interpreter &vu = adapter.sandbox().vu();
            Vu1Lifted::landQueued(vu);
            VU1State &state = vu.state();
            state.top = adapter.top();
            state.itop = adapter.itop();
            const ps2x::vu1lift::RoundTowardZero rounding;
            const bool sticky = (state.status & 0x0C0u) != 0x0C0u;
            uint8_t *data = adapter.dataMemory();
            const uint64_t cycles = sticky ? ps2x::vu1lift::sct2Program<true>(pc, state, data, adapter.top(), kick, user)
                                           : ps2x::vu1lift::sct2Program<false>(pc, state, data, adapter.top(), kick, user);
            if (cycles == ps2x::vu1lift::kUnknownTarget)
                return cycles;
            Vu1Lifted::setClip(vu, state.clip);
            Vu1Lifted::advanceTo(vu, Vu1Lifted::cycle(vu) + cycles);
            state.cycles = Vu1Lifted::cycle(vu);
            return cycles;
        }

        std::string difference(const VU1State &lifted, const VU1State &sandbox)
        {
            std::string out;
            char item[96];
            for (int r = 0; r < 32; ++r)
                if (std::memcmp(lifted.vf[r], sandbox.vf[r], sizeof(lifted.vf[r])) != 0)
                {
                    std::snprintf(item, sizeof(item), " vf%d", r);
                    out += item;
                }
            for (int r = 0; r < 16; ++r)
                if (lifted.vi[r] != sandbox.vi[r])
                {
                    std::snprintf(item, sizeof(item), " vi%d %#x/%#x", r, lifted.vi[r], sandbox.vi[r]);
                    out += item;
                }
            if (std::memcmp(lifted.acc, sandbox.acc, sizeof(lifted.acc)) != 0)
                out += " acc";
            if (lifted.clip != sandbox.clip || lifted.mac != sandbox.mac || lifted.status != sandbox.status)
            {
                std::snprintf(item, sizeof(item), " clip %#x/%#x mac %#x/%#x status %#x/%#x", lifted.clip, sandbox.clip,
                              lifted.mac, sandbox.mac, lifted.status, sandbox.status);
                out += item;
            }
            if (lifted.pc != sandbox.pc)
            {
                std::snprintf(item, sizeof(item), " pc %#x/%#x", lifted.pc, sandbox.pc);
                out += item;
            }
            return out;
        }

        // The lifted program and the sandbox from the same state; the sandbox's result stays and draws.
        void check(Vu1StreamAdapter &adapter, uint32_t pc, bool resumed)
        {
            ps2x::native::Vu1Sandbox &sandbox = adapter.sandbox();
            VU1Interpreter &vu = sandbox.vu();
            uint8_t *data = adapter.dataMemory();
            static std::vector<uint8_t> s_before(kVuMemory), s_liftedData(kVuMemory);
            static std::vector<std::vector<uint8_t>> s_liftedPackets, s_packets;
            s_liftedPackets.clear();
            s_packets.clear();
            std::memcpy(s_before.data(), data, kVuMemory);
            Vu1Lifted::landQueued(vu);
            const VU1State before = vu.state();
            const uint64_t liftedCycles = runLifted(adapter, pc, kickToList, &s_liftedPackets);
            const VU1State lifted = vu.state();
            std::memcpy(s_liftedData.data(), data, kVuMemory);
            std::memcpy(data, s_before.data(), kVuMemory);
            const uint64_t clock = vu.state().cycles;
            vu.state() = before;
            vu.state().cycles = clock;
            Vu1Lifted::setClip(vu, before.clip);
            vu.setPacketObserver([&sandbox](const uint8_t *packet, uint32_t bytes)
                                 {
                                     s_packets.emplace_back(packet, packet + bytes);
                                     sandbox.gif().packet(packet, bytes);
                                 });
            const uint64_t cyclesBefore = sandbox.cycles();
            if (resumed)
                sandbox.resume(const_cast<uint8_t *>(adapter.code()), kVuMemory, data, kVuMemory, adapter.top(), adapter.itop());
            else
                sandbox.call(const_cast<uint8_t *>(adapter.code()), kVuMemory, data, kVuMemory, pc, adapter.top(), adapter.itop());
            vu.setPacketObserver([&sandbox](const uint8_t *packet, uint32_t bytes) { sandbox.gif().packet(packet, bytes); });
            const uint64_t cycles = sandbox.cycles() - cyclesBefore;
            adapter.addPortCycles(cycles);
            ++g_state.stats.checked;
            std::string diff;
            if (liftedCycles == ps2x::vu1lift::kUnknownTarget)
            {
                ++g_state.stats.unknown;
                diff = " JR to a target the spec does not list";
            }
            else
            {
                diff = difference(lifted, vu.state());
                if (s_liftedPackets != s_packets)
                    diff += " packets";
                if (std::memcmp(s_liftedData.data(), data, kVuMemory) != 0)
                    diff += " memory";
                if (liftedCycles != cycles)
                    diff += " cycles " + std::to_string(liftedCycles) + "/" + std::to_string(cycles);
            }
            if (diff.empty())
                return;
            ++g_state.stats.mismatches;
            ++g_state.stats.mismatched[pc];
            static int s_logged = 0;
            if (s_logged++ < 50)
                std::fprintf(stderr, "[sct2:vu1] lifted %#x differs from the sandbox at TOP %#x:%s\n", pc, adapter.top(),
                             diff.c_str());
        }
    }

    Vu1Port vu1PortMode()
    {
        static const Vu1Port s_mode = []
        {
            const char *value = std::getenv("SCT2_VU1_PORT");
            if (!value || !*value || std::strcmp(value, "0") == 0)
                return Vu1Port::Off;
            return std::strcmp(value, "check") == 0 ? Vu1Port::Check : Vu1Port::On;
        }();
        return s_mode;
    }

    bool vu1Ported(Vu1StreamAdapter &adapter, uint32_t)
    {
        return vu1PortMode() != Vu1Port::Off && resident(adapter);
    }

    void vu1PortDraw(Vu1StreamAdapter &adapter, uint32_t pc, bool resumed)
    {
        // An MSCNT continues where the program's E bit stopped it, not at the MSCAL the adapter passes again.
        if (resumed)
            pc = adapter.sandbox().vu().state().pc;
        ps2x::native::GifToNative &gif = adapter.sandbox().gif();
        gif.setSource(pc);
        if (vu1PortMode() == Vu1Port::Check)
        {
            check(adapter, pc, resumed);
            return;
        }
        ++g_state.stats.ported;
        const uint64_t cycles = runLifted(adapter, pc, kickToGif, &gif);
        if (cycles == ps2x::vu1lift::kUnknownTarget)
        {
            ++g_state.stats.unknown;
            return;
        }
        adapter.addPortCycles(cycles);
        gif.flush();
    }

    void vu1PortReport()
    {
        static uint32_t s_frames = 0;
        if (vu1PortMode() == Vu1Port::Off || ++s_frames % 600u != 0u)
            return;
        Stats &s = g_state.stats;
        std::fprintf(stderr, "[sct2:vu1] lifted over 600 frames: %llu drawn, %llu checked, %llu mismatches, %llu unknown targets",
                     static_cast<unsigned long long>(s.ported), static_cast<unsigned long long>(s.checked),
                     static_cast<unsigned long long>(s.mismatches), static_cast<unsigned long long>(s.unknown));
        for (const auto &[pc, count] : s.mismatched)
            std::fprintf(stderr, " %#x=%llu", pc, static_cast<unsigned long long>(count));
        std::fprintf(stderr, "\n");
        s = Stats{};
    }
}
