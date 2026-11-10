// Stand-in for a game's generated code (build with -DPS2_GENERATED=<this folder>): no translated
// main program, so the runtime runs the game's code with the EE interpreter (ee_interp.cpp) straight
// from the ELF the player supplies. VU1 programs fall back to the VU interpreter / JIT the same way
// (register_generated_vu is weak in vu_micro.cpp).
void register_generated_code() {}
