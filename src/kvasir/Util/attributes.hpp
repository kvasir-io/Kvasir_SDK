#pragma once

// Attribute spellings that differ between the two toolchains, so each compiler gets its nearest
// equivalent instead of an ignored-attribute warning.
//
// A RAM function may run while the flash is not readable, so it carries no check whose handler
// lives in flash (the UB sanitizer's, __stack_chk_fail): one that fired could only lock the core up.
//
// Its code goes to the input section .ramfunc, not .data: the linker scripts copy it from flash with
// .data (linker/common_data_body.inc.ld), and a RAM-only image puts it with .text
// (linker/common_ram_only.ld), read-only and covered by an IMAGE_CRC check of the running image.
#ifdef __clang__
    #define KVASIR_RAM_FUNC_ATTRIBUTES                                            \
        gnu::section(".ramfunc"), gnu::noinline, clang::no_sanitize("undefined"), \
          gnu::no_stack_protector
    // inlined into a RAM function, it brings its instrumentation along: exempt as well
    #define KVASIR_RAM_FUNC_INLINE_ATTRIBUTES                                         \
        gnu::section(".ramfunc"), gnu::always_inline, clang::no_sanitize("undefined")
    // no atexit() registration for the static's destructor
    #define KVASIR_NO_DESTROY                    clang::no_destroy
    #define KVASIR_NO_SANITIZE_UNSIGNED_OVERFLOW clang::no_sanitize("unsigned-integer-overflow")
#else
    // gcc rejects a section attribute on inline functions and needs long_call for the RAM copy.
    // noipa: under LTO gcc clones (constprop) member, template and inline functions and puts the
    // clone in .text, dropping the section - it keeps it only with noipa (or used).
    #define KVASIR_RAM_FUNC_ATTRIBUTES                                            \
        gnu::section(".ramfunc"), gnu::noinline, gnu::long_call, gnu::noipa,      \
          gnu::no_sanitize("undefined", "bounds-strict"), gnu::no_stack_protector
    #define KVASIR_RAM_FUNC_INLINE_ATTRIBUTES                              \
        gnu::always_inline, gnu::no_sanitize("undefined", "bounds-strict")
    // gcc has no no_destroy; the atexit() registration lands in StartUp/Runtime/Libc.hpp's stub instead
    #define KVASIR_NO_DESTROY
    // gcc has no unsigned-overflow sanitizer; naming it anyway earns a -Wattributes
    #define KVASIR_NO_SANITIZE_UNSIGNED_OVERFLOW
#endif

// The section of an INLINE variable: [[KVASIR_SECTION(".eeprom")]] on a namespace-scope inline
// variable or a variable template, [[KVASIR_SECTION_MEMBER(".eeprom")]] on a class's static inline
// member. gcc's LTO makes such a variable local to the link and forgets its section attribute with
// that - measured with arm-none-eabi-g++ 16.2 -flto: StackProtector's sentinel in .bss
// instead of at the stack's bottom, a store's value in .data instead of .eeprom.
// Two attributes keep the section, each with a price that decides where it fits:
//   externally_visible  emits nothing that no code refers to (a record in .noInit costs RAM only in
//                       the images that use it) - but is refused on a variable without external
//                       linkage, which a member of a class template with a file-local argument is
//                       ("'externally_visible' attribute have effect only on public objects");
//   used                works with any linkage - but emits the variable wherever it is declared,
//                       which for a member is wherever its class is instantiated, and that is where
//                       it is used anyway.
// clang keeps the section by itself. A variable that is not inline (plain, static, in an unnamed
// namespace) keeps it with either compiler.
#ifdef __clang__
    #define KVASIR_SECTION(name)        gnu::section(name)
    #define KVASIR_SECTION_MEMBER(name) gnu::section(name)
#else
    #define KVASIR_SECTION(name)        gnu::section(name), gnu::externally_visible
    #define KVASIR_SECTION_MEMBER(name) gnu::section(name), gnu::used
#endif

// .noInit for an INLINE variable: [[KVASIR_NOINIT]]. Its own input section, .noInit.inline (the linker's
// *(.noInit*) puts it into .noInit): gcc refuses a TU where an inline (COMDAT) variable and a plain one share a
// named section ("'x' causes a section type conflict with 'y' in section '.noInit'") - every TU that uses one of
// the SDK's records (Panic::lastPanic, Fault::lastFault, ...) and keeps a plain variable of its own in .noInit.
// A class's static inline member takes [[KVASIR_NOINIT_MEMBER]] (KVASIR_SECTION_MEMBER's linkage rule above).
// A plain, static or unnamed-namespace variable takes [[gnu::section(".noInit")]].
#define KVASIR_NOINIT        KVASIR_SECTION(".noInit.inline")
#define KVASIR_NOINIT_MEMBER KVASIR_SECTION_MEMBER(".noInit.inline")

// Kept through the compiler AND the linker's garbage collection though nothing refers to it (a
// descriptor only a tool reads). arm-none-eabi-gcc has no `retain` ("'retain' attribute ignored
// [-Wattributes]", 16.2): there it is `used` alone, and the linker may still drop the section.
#ifdef __clang__
    #define KVASIR_USED_RETAIN gnu::used, gnu::retain
#else
    #define KVASIR_USED_RETAIN gnu::used
#endif

// One function optimised for size whatever the variant: clang's minsize. gcc has no counterpart
// that is safe on any function (optimize("Os") resets other options of the TU), so there it is nothing.
#ifdef __clang__
    #define KVASIR_MINSIZE clang::minsize
#else
    #define KVASIR_MINSIZE
#endif

// No profile counters in this function under -fprofile-instr-generate (the coverage variant): code that runs
// before memory is set up. clang instruments even a naked function - a counter increment (a literal load of the
// counter's address) ahead of its asm - which breaks the reset entry's "cpsid i first" (check_reset_entry.py),
// and whatever counts before initMemory() lands in .data/.bss that is wiped next. Only spelled out where clang
// instruments (it defines __LLVM_INSTR_PROFILE_GENERATE then): LLVM inlines nothing across a mismatch of the
// attribute (areInlineCompatible, isEqual<NoProfileAttr>), so on resetBody it would keep the whole start-up out
// of line in every other image too. gcc has no coverage variant here. Empty, it leaves `[[a, ]]`: valid C++.
#ifdef __LLVM_INSTR_PROFILE_GENERATE
    #define KVASIR_NO_PROFILE gnu::no_profile_instrument_function
#else
    #define KVASIR_NO_PROFILE
#endif

// Per-core memory: the RP2040's SRAM4 (core 1, "scratch_x") and SRAM5 (core 0, "scratch_y"), banks
// outside the striped main RAM that the datasheet offers "for per-core purposes, e.g. stack and
// frequently-executed code" (RP2040 datasheet 2.6.2). The chip package defines KVASIR_CORE_SCRATCH
// when its linker script has the sections; elsewhere these expand to nothing and the object stays
// in .data/.bss/.text. Placement only: every bus master can still read and write the bank.
//   [[KVASIR_CORE1_BSS]]  static std::array<std::int32_t, 64> taps;   // zeroed at boot
//   [[KVASIR_CORE0_DATA]] static std::uint32_t index{1};              // copied from flash at boot
//   [[KVASIR_CORE1_CODE]] void sampleIsr() { ... }                    // copied from flash at boot
// _CODE is a speed placement, not a RAM function: it may call flash (each call goes through a
// linker veneer, flash is out of BL range) and is not checked or exempted like
// KVASIR_RAM_FUNC_ATTRIBUTES. No const objects in _DATA (a read-only object in a writable section
// is a section type conflict).
#ifdef KVASIR_CORE_SCRATCH
    // the ".bss." prefix makes the section NOBITS: it costs no flash
    #define KVASIR_CORE0_BSS  gnu::section(".bss.scratch_y")
    #define KVASIR_CORE1_BSS  gnu::section(".bss.scratch_x")
    #define KVASIR_CORE0_DATA gnu::section(".scratch_y.data")
    #define KVASIR_CORE1_DATA gnu::section(".scratch_x.data")
    #ifdef __clang__
        #define KVASIR_CORE0_CODE gnu::section(".scratch_y.text"), gnu::noinline
        #define KVASIR_CORE1_CODE gnu::section(".scratch_x.text"), gnu::noinline
    #else
        // as KVASIR_RAM_FUNC_ATTRIBUTES: gcc's LTO keeps the section only with noipa
        #define KVASIR_CORE0_CODE                                                      \
            gnu::section(".scratch_y.text"), gnu::noinline, gnu::long_call, gnu::noipa
        #define KVASIR_CORE1_CODE                                                      \
            gnu::section(".scratch_x.text"), gnu::noinline, gnu::long_call, gnu::noipa
    #endif
#else
    #define KVASIR_CORE0_BSS
    #define KVASIR_CORE1_BSS
    #define KVASIR_CORE0_DATA
    #define KVASIR_CORE1_DATA
    #define KVASIR_CORE0_CODE
    #define KVASIR_CORE1_CODE
#endif

// The first statement of every KVASIR_RAM_FUNC_ATTRIBUTES function. It costs no instruction: a label
// whose address goes into the non-loaded section kvasir_ram_funcs (linker/common.ld). The label
// travels with the code, so it names where the compiler really put the function, and the post-link
// check (cmake/tools/check_ram_funcs.py) fails the build unless that is RAM and nothing the function
// reaches runs from flash. The check also refuses a RAM function in the image without this mark.
#if defined(__arm__) || defined(__thumb__)
    #define KVASIR_RAM_FUNC_MARK_IN(section)                                                  \
        asm volatile(".pushsection " section ",\"\",%progbits\n.4byte 1f\n.popsection\n1:\n")
#else
    #define KVASIR_RAM_FUNC_MARK_IN(section)
#endif
#define KVASIR_RAM_FUNC_MARK() KVASIR_RAM_FUNC_MARK_IN("kvasir_ram_funcs")
// For a RAM function that calls into flash on purpose, once the flash is readable again (say
// why next to it): it must be in RAM itself, what it calls is not checked.
#define KVASIR_RAM_FUNC_MARK_CALLS_FLASH() KVASIR_RAM_FUNC_MARK_IN("kvasir_ram_funcs_calls_flash")
