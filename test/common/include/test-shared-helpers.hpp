#pragma once
#include <cstddef>
#include <cstdint>

void put_hex(const char* label, std::uint32_t v);
void put_dec(std::uint32_t v);
void make_path(char* out, std::size_t cap, const char* name);

void check(int cond, int test_n, const char* desc);
void check_ok(int test_n);
