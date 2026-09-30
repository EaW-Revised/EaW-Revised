#include "llimits.h"

#include <cstddef>
#include <iostream>

int main() {
    std::cout << "Instruction=" << sizeof(Instruction)
              << " unsigned_int=" << sizeof(unsigned int)
              << " unsigned_long=" << sizeof(unsigned long) << '\n';
    return sizeof(Instruction) == 4 && sizeof(unsigned int) == 4 ? 0 : 1;
}
