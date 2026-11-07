# Embeds a binary file as a C++ byte array.
# cmake -DIN=<file> -DOUT=<file.cpp> -DNAME=<symbol> -P Bin2Cpp.cmake
file(READ "${IN}" hex HEX)
string(LENGTH "${hex}" n)
math(EXPR bytes "${n} / 2")
string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1," body "${hex}")
string(REGEX REPLACE "(0x..,0x..,0x..,0x..,0x..,0x..,0x..,0x..,0x..,0x..,0x..,0x..,0x..,0x..,0x..,0x..,)" "\\1\n" body "${body}")
file(WRITE "${OUT}" "// Generated from ${IN}; do not edit.\n#include <cstddef>\n#include <cstdint>\nextern const std::uint8_t ${NAME}[];\nextern const std::size_t ${NAME}_size;\nalignas(4) const std::uint8_t ${NAME}[] = {\n${body}\n};\nconst std::size_t ${NAME}_size = ${bytes};\n")
