#include "proj_extensions.h"
/* Compile the public header as C as well as C++. */
unsigned int extension_test_c_abi(void) { return PROJ_EXT_ABI_VERSION; }
