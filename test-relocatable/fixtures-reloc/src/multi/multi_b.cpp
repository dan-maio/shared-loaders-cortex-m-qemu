/**
 * @file multi_b.cpp
 * @brief Middle file. Calls c_add() from multi_c.cpp — an inter-file reference
 *        that starts UND in multi_b.o and becomes DEFINED after `ld -r` merges
 *        the three objects. b_double is itself internal (called by multi_a).
 */
int c_add(int);                                // from multi_c (UND until -r)

int b_double(int x) { return c_add(x) * 2; }   // internal; called by multi_a
