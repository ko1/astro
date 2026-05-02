#if 0
static int f100(int n){ return n; }
static int f99(int n){ return f100(n); }
static int f98(int n){ return f99(n); }
static int f97(int n){ return f98(n); }
static int f96(int n){ return f97(n); }
static int f95(int n){ return f96(n); }
static int f94(int n){ return f95(n); }
static int f93(int n){ return f94(n); }
static int f92(int n){ return f93(n); }
static int f91(int n){ return f92(n); }
static int f90(int n){ return f91(n); }
static int f89(int n){ return f90(n); }
static int f88(int n){ return f89(n); }
static int f87(int n){ return f88(n); }
static int f86(int n){ return f87(n); }
static int f85(int n){ return f86(n); }
static int f84(int n){ return f85(n); }
static int f83(int n){ return f84(n); }
static int f82(int n){ return f83(n); }
static int f81(int n){ return f82(n); }
static int f80(int n){ return f81(n); }
static int f79(int n){ return f80(n); }
static int f78(int n){ return f79(n); }
static int f77(int n){ return f78(n); }
static int f76(int n){ return f77(n); }
static int f75(int n){ return f76(n); }
static int f74(int n){ return f75(n); }
static int f73(int n){ return f74(n); }
static int f72(int n){ return f73(n); }
static int f71(int n){ return f72(n); }
static int f70(int n){ return f71(n); }
static int f69(int n){ return f70(n); }
static int f68(int n){ return f69(n); }
static int f67(int n){ return f68(n); }
static int f66(int n){ return f67(n); }
static int f65(int n){ return f66(n); }
static int f64(int n){ return f65(n); }
static int f63(int n){ return f64(n); }
static int f62(int n){ return f63(n); }
static int f61(int n){ return f62(n); }
static int f60(int n){ return f61(n); }
static int f59(int n){ return f60(n); }
static int f58(int n){ return f59(n); }
static int f57(int n){ return f58(n); }
static int f56(int n){ return f57(n); }
static int f55(int n){ return f56(n); }
static int f54(int n){ return f55(n); }
static int f53(int n){ return f54(n); }
static int f52(int n){ return f53(n); }
static int f51(int n){ return f52(n); }
static int f50(int n){ return f51(n); }
static int f49(int n){ return f50(n); }
static int f48(int n){ return f49(n); }
static int f47(int n){ return f48(n); }
static int f46(int n){ return f47(n); }
static int f45(int n){ return f46(n); }
static int f44(int n){ return f45(n); }
static int f43(int n){ return f44(n); }
static int f42(int n){ return f43(n); }
static int f41(int n){ return f42(n); }
static int f40(int n){ return f41(n); }
static int f39(int n){ return f40(n); }
static int f38(int n){ return f39(n); }
static int f37(int n){ return f38(n); }
static int f36(int n){ return f37(n); }
static int f35(int n){ return f36(n); }
static int f34(int n){ return f35(n); }
static int f33(int n){ return f34(n); }
static int f32(int n){ return f33(n); }
static int f31(int n){ return f32(n); }
static int f30(int n){ return f31(n); }
static int f29(int n){ return f30(n); }
static int f28(int n){ return f29(n); }
static int f27(int n){ return f28(n); }
static int f26(int n){ return f27(n); }
static int f25(int n){ return f26(n); }
static int f24(int n){ return f25(n); }
static int f23(int n){ return f24(n); }
static int f22(int n){ return f23(n); }
static int f21(int n){ return f22(n); }
static int f20(int n){ return f21(n); }
static int f19(int n){ return f20(n); }
static int f18(int n){ return f19(n); }
static int f17(int n){ return f18(n); }
static int f16(int n){ return f17(n); }
static int f15(int n){ return f16(n); }
static int f14(int n){ return f15(n); }
static int f13(int n){ return f14(n); }
static int f12(int n){ return f13(n); }
static int f11(int n){ return f12(n); }
static int f10(int n){ return f11(n); }
static int f9(int n){ return f10(n); }
static int f8(int n){ return f9(n); }
static int f7(int n){ return f8(n); }
static int f6(int n){ return f7(n); }
static int f5(int n){ return f6(n); }
static int f4(int n){ return f5(n); }
static int f3(int n){ return f4(n); }
static int f2(int n){ return f3(n); }
static int f1(int n){ return f2(n); }
static int f0(int n){ return f1(n); }
#endif

//#define A __attribute__((noinline)) 
#define A

#if 0
static A int f2(int n){ return n; }
static A int f1(int n){ return f2(n); }
static A int f0(int n){ return f1(n); }
#else
extern int f0(int n);
#endif

#include <stdio.h>

int main(void)
{
    int sz = 100 * 1000 * 1000;
    for (int i=0; i<sz; i++) {
        f0(42);
    }
    printf("%d\n", f0(42));
    return 0;
}
