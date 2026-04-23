#pragma once

#include <cstddef>
#include "zym/zym.h"

bool has_embedded_bytecode();
char* get_executable_path(char* buffer, size_t size);
int runtime_main(int argc, char** argv, ZymAllocator* allocator);
