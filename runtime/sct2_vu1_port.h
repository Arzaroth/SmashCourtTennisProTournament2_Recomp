#pragma once

#include <cstdint>

namespace ps2x::native
{
    class Vu1StreamAdapter;
}

namespace sct2
{
    // SCT2_VU1_PORT: the VU1 program as C++ (generated/vu1lift, PS2Recomp/tools/vu1_lift.py) in the sandbox's place.
    // "check" runs it beside the sandbox on the same state and reports where they differ, the sandbox still drawing;
    // unset or "1" lets it draw. "0": the sandbox alone.
    enum class Vu1Port
    {
        Off,
        Check,
        On,
    };

    Vu1Port vu1PortMode();
    // Whether the lifted program covers this start pc of the resident microcode.
    bool vu1Ported(ps2x::native::Vu1StreamAdapter &adapter, uint32_t pc);
    // A Port program, at its MSCAL and at each MSCNT continuing it.
    void vu1PortDraw(ps2x::native::Vu1StreamAdapter &adapter, uint32_t pc, bool resumed);
    // Every 600 frames, on the thread the adapter runs on: the calls ported and checked, and the mismatches.
    void vu1PortReport();
}
