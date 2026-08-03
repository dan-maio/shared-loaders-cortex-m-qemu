/**
 * @file mylib1.h
 * @brief Public C11 API of mylib1 — a loadable test shared library.
 */
#ifndef MYLIB1_H_
#define MYLIB1_H_

#include <stdio.h>
#include <string.h>

#if defined(__cplusplus)
extern "C" {
#endif

struct Str_Lib {
    char         *a;
    float         f;
    char         *bb;
    unsigned int  b;
    unsigned int  c;
};

typedef unsigned int (*func)(unsigned int);

unsigned int library_function(unsigned int x);
unsigned int library_function2(unsigned int x);
unsigned int library_function3(unsigned int x);
unsigned int callback_f(unsigned int x);
unsigned int g(unsigned int x);
unsigned int gg(unsigned int x);
unsigned int fff(unsigned int x);
unsigned int ptrfunc(func p, unsigned int x);
char *retstr(void);
char *retstr_(void);
void print_lib(void);
void print_lib2(char *s);
struct Str_Lib ret_struct(struct Str_Lib stm);
struct Str_Lib ret_struct_lib(void);

#pragma weak foo
unsigned int foo(void);
#pragma weak boo
unsigned int boo(void);
#pragma weak boo2
unsigned int boo2(void);
unsigned int lkj(void);
unsigned int my_systick_handler(unsigned int ticks);

#if defined(__cplusplus)
}
#endif

#endif /* MYLIB1_H_ */
