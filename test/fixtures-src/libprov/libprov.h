/** @file libprov.h @brief Provider library: exports prov_value for another library to import. */
#ifndef LIBPROV_H
#define LIBPROV_H
unsigned int prov_value(unsigned int x); /**< Exported: returns x + 100. */
#endif
