#include "mylib1.h"

#include <math.h>


unsigned int aa = 10;
unsigned int bb;
static unsigned int cc = 5;
static unsigned int dd = 7;
static unsigned int bss1;
static unsigned int bss2;

extern unsigned int AA;
extern unsigned int BB;
extern char *s1;



struct str_lib s_lib = { "15_",125.236, "1235",120, 30};


extern unsigned int f(unsigned int);
extern unsigned int w(unsigned int);

/* File-local helper, defined below and called from callback_f(). */
static void ww(unsigned int);


unsigned int fff(unsigned int x){
	return AA + BB + x;
}

char *str = "Hello mylib!";
char *str_ = "Hello from mylib!";

__attribute__((constructor)) void print_constr() {
    aa +=aa;
    bb = aa + 20;

	printf
	(
		"I am a Constructor!! %d \n",bb
	);

}

__attribute__((constructor)) void print_constr1() {

	printf
	(
		"I am a Constructor1!!\n"
	);
}
__attribute__((constructor)) void print_constr2() {
	printf
	(
		"I am a Constructor2!!\n"
	);
	printf("in Constructor2  %s\n",retstr());
}

__attribute__((constructor)) void print_constr3() {
	printf
	(
		"I am a Constructor3!!\n"
	);
}
struct str_lib ret_struct(struct str_lib stm) {

	struct str_lib ssl;
    ssl.a = s_lib.a;
    ssl.bb = s_lib.bb;
	strcat(ssl.a,stm.a);
	strcat(ssl.a,"_12345");
	strcat(ssl.bb,stm.bb);
	ssl.b = stm.b + 10;
	ssl.c = stm.c * 2;
	ssl.f = stm.f;
	ssl.f = stm.f * 6.2   + log(ssl.f) ;


	return ssl;
}

struct str_lib ret_struct_lib() {

	return s_lib;
}
unsigned int library_function(unsigned int x) {
	return (x + 3 + aa);
}

unsigned int library_function2(unsigned int x) {

	return (x * 3 - cc + dd);
}

unsigned int library_function3(unsigned int x) {

	unsigned int a = library_function(x);
	unsigned int b = library_function2(x);

	bb = a + b + w(x);
	bss1 = bb + a;
	return (bb + bss1);

}

unsigned int callback_f(unsigned int x) {
	ww(x);
	return AA + f(aa + bb + x);
}

#if 0
unsigned int w(unsigned int x) {

	cc = x;
	printf("Weak in Lib %d %d \n", cc, AA);
	return x;
}
#endif

void print_lib() {
	bss2 = 10;
	printf("String In Lib %d %d\n", bb, bss2);
}

#if 1
void print_lib2(char *str2) {

	printf("in lib ...%s\n", str2);
	printf("in lib2 ...%s...\n", s1);

}
#endif

static void ww(unsigned int x) {
	BB += 20;
	w(x + 2 + BB);
}

char* retstr() {

	return str;
}

char* retstr_() {

	return str_;
}

unsigned int g(unsigned int x) {

	return aa + bb + x;
}

unsigned int ptrfunc(func p, unsigned x) {

	return p(x) + x;
}

unsigned int gg(unsigned int x) {

	return x + ptrfunc(g, x);
}



#if 0

unsigned int mm() {  //weak function in Main

	return AA +AA + AA;
}
#endif


unsigned int foo(){
	return 20;
}

unsigned int boo(){
	return 30;
}

unsigned int boo2(){
	return boo() + 30;
}

unsigned int lkj(){
	return BB+100;
}






