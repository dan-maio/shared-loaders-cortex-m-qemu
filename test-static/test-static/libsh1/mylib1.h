/*
 * mylibc.h
 *
 *  Created on: Jan 18, 2022
 *      Author: dan
 */

#ifndef MYLIB_H_
#define MYLIB_H_

#include <stdio.h>
#include <string.h>

#if defined(__cplusplus)
extern "C"
{
#endif

struct str_lib {
	char* a;
	float f;
	char * bb;
	unsigned int b;
	unsigned int c;



};


typedef unsigned int(*func)(unsigned int);

unsigned int library_function( unsigned int  );
unsigned int library_function2( unsigned int  );
unsigned int library_function3( unsigned int  );
unsigned int callback_f(unsigned int);
unsigned int g(unsigned int );
unsigned int fff(unsigned int );

char *retstr(void);
char *retstr_(void);
unsigned int ptrfunc(func,unsigned );
unsigned int gg(unsigned int );
void print_lib(void);
void print_lib2(char *);

#pragma weak foo
unsigned int foo(void);

#pragma weak boo
unsigned int boo(void);

#pragma weak boo2
unsigned int boo2(void);

unsigned int lkj();

struct str_lib ret_struct_lib(void);
struct str_lib ret_struct(struct str_lib );

#if defined(__cplusplus)
}
#endif

#endif /* MYLIB_H_ */
