#pragma once
#include "PinFactories.hpp"
#include "Types.hpp"
#include "kvasir/Mpl/Utility.hpp"

//#include "PortFactories.hpp"

namespace Kvasir { namespace Io {
    static constexpr HwPort<0>  port0{};
    static constexpr HwPort<1>  port1{};
    static constexpr HwPort<2>  port2{};
    static constexpr HwPort<3>  port3{};
    static constexpr HwPort<4>  port4{};
    static constexpr HwPort<5>  port5{};
    static constexpr HwPort<6>  port6{};
    static constexpr HwPort<7>  port7{};
    static constexpr HwPort<8>  port8{};
    static constexpr HwPort<9>  port9{};
    static constexpr HwPort<10> port10{};
    static constexpr HwPort<11> port11{};
    static constexpr HwPort<12> port12{};
    static constexpr HwPort<13> port13{};
    static constexpr HwPort<14> port14{};
    static constexpr HwPort<15> port15{};

    static constexpr HwPort<0>  portA{};
    static constexpr HwPort<1>  portB{};
    static constexpr HwPort<2>  portC{};
    static constexpr HwPort<3>  portD{};
    static constexpr HwPort<4>  portE{};
    static constexpr HwPort<5>  portF{};
    static constexpr HwPort<6>  portG{};
    static constexpr HwPort<7>  portH{};
    static constexpr HwPort<8>  portI{};
    static constexpr HwPort<9>  portJ{};
    static constexpr HwPort<10> portK{};
    static constexpr HwPort<11> portL{};
    static constexpr HwPort<12> portM{};
    static constexpr HwPort<13> portN{};
    static constexpr HwPort<14> portO{};
    static constexpr HwPort<15> portP{};

    static constexpr Pin<0>  pin0{};
    static constexpr Pin<1>  pin1{};
    static constexpr Pin<2>  pin2{};
    static constexpr Pin<3>  pin3{};
    static constexpr Pin<4>  pin4{};
    static constexpr Pin<5>  pin5{};
    static constexpr Pin<6>  pin6{};
    static constexpr Pin<7>  pin7{};
    static constexpr Pin<8>  pin8{};
    static constexpr Pin<9>  pin9{};
    static constexpr Pin<10> pin10{};
    static constexpr Pin<11> pin11{};
    static constexpr Pin<12> pin12{};
    static constexpr Pin<13> pin13{};
    static constexpr Pin<14> pin14{};
    static constexpr Pin<15> pin15{};
    static constexpr Pin<16> pin16{};
    static constexpr Pin<17> pin17{};
    static constexpr Pin<18> pin18{};
    static constexpr Pin<19> pin19{};
    static constexpr Pin<20> pin20{};
    static constexpr Pin<21> pin21{};
    static constexpr Pin<22> pin22{};
    static constexpr Pin<23> pin23{};
    static constexpr Pin<24> pin24{};
    static constexpr Pin<25> pin25{};
    static constexpr Pin<26> pin26{};
    static constexpr Pin<27> pin27{};
    static constexpr Pin<28> pin28{};
    static constexpr Pin<29> pin29{};
    static constexpr Pin<30> pin30{};
    static constexpr Pin<31> pin31{};

    template<typename T = void>
    struct PinLocationTraits;   // must be specialized in a chipxxxInterrupt file
}}   // namespace Kvasir::Io
