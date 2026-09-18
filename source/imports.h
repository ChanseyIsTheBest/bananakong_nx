#ifndef BK_IMPORTS_H
#define BK_IMPORTS_H

#include <stdint.h>
#include "so_util.h"

extern DynLibFunction g_imports[];
extern const int g_num_imports;

uintptr_t imports_lookup(const char *name);

#endif
