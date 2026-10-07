/* Universal macOS helpers may inherit DYLD_INSERT_LIBRARIES from Wine.
 * D3DMetal and the actual adapter are x86_64; the ARM slice stays inert. */
int utm_native_helper_noop(void) { return 0; }
