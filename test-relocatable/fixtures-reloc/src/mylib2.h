/**
 * @file mylib2.h
 * @brief Public C11 API of mylib2.
 */
#ifndef MYLIB2_H_
#define MYLIB2_H_

#include <stdio.h>

#if defined(__cplusplus)
extern "C" {
#endif

unsigned int lf(unsigned int x);
unsigned int lf2(unsigned int x);
unsigned int lf3(unsigned int x);
unsigned int xxx(void);
void pl(void);
void pl2(char *s);
#pragma weak mm
unsigned int mm(void);

#if defined(__cplusplus)
}
#endif

#endif /* MYLIB2_H_ */
