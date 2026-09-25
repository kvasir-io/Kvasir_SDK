#pragma once

// ram_funcs.cpp CASE 4: a RAM function without its mark
[[KVASIR_RAM_FUNC_ATTRIBUTES]] void unmarked(int v) { sink = v; }
