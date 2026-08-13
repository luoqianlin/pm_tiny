#ifdef PM_TINY_DEBUG
bool debug_mode = true;
#else
bool debug_mode = false;
#endif

// This variable exists so the tests can point
// it to a mockup proc dir
const char* procdir_path = "/proc";
