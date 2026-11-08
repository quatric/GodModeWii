/* devkitPPC's powerpc-eabi target has main call __eabi(); TinyLoad sets everything up in crt0.S itself. */
void __eabi(void) {}
