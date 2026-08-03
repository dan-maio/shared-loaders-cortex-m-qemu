#include "mylib2.h"

#include <math.h>


unsigned int aaa = 10;

unsigned int XX=25;
unsigned int bbb;
static unsigned int ccc = 5;
static unsigned int ddd = 7;


extern unsigned int AA;
extern unsigned int BB;
extern char *s1;



extern unsigned int w(unsigned int);

char *str2 = "Hello mylib!";

__attribute__((constructor)) void pr() {
    aaa +=aaa;
    bbb = aaa + 20;

	printf
	(
		"I am a Constructor in Lib2!! %d \n",bbb+10
	);

}


unsigned int lf(unsigned int x) {
	return (x + 3 + aaa);
}

unsigned int lf2(unsigned int x) {

	return (x * 3 - ccc + ddd);
}

unsigned int lf3(unsigned int x) {

	unsigned int a = lf(x);
	unsigned int b = lf2(x);

	bbb = a + b + w(x);

	return (bbb + bbb);
}


unsigned int xxx(){

	printf("...%d.\n",BB);
	return BB+AA;
}

void pl() {
	bbb -= 10;
	printf("String In Lib  %d\n", bbb);
}

void pl2(char *str2) {

	printf("in lib ...%s\n", str2);
	printf("in lib2 ...%s...\n", s1);

}

#if 1

unsigned int mm() {  //weak function in Main

	return 0;
}
#endif









