/*
 * mylibc.h
 *
 *  Created on: Jan 18, 2022
 *      Author: dan
 */

#ifndef MYLIB2_H_
#define MYLIB2_H_

#include <stdio.h>
#include <string.h>


#if defined(__cplusplus)
extern "C"
{
#endif

typedef unsigned int(*func)(unsigned int);

unsigned int lf( unsigned int  );
unsigned int lf2( unsigned int  );
unsigned int lf3( unsigned int  );

unsigned int mm(void);
unsigned xxx();



void pl(void);
void pl2(char *);


#if defined(__cplusplus)
}
#endif

#endif /* MYLIB2_H_ */
