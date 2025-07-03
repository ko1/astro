
//#define A __attribute__((noinline)) 
#define A

#if 0
static A int f2(int n){ return n; }
static A int f1(int n){ return f2(n); }
static A int f0(int n){ return f1(n); }
#else
extern int f0(int n);
#endif

int zero(void){return 0;}

int main(void)
{
    int sz = 1000 * 1000 * 1000;
    for (int i=0; i<sz; i++) {
        zero();
    }
    return 0;
}
