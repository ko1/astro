
static int zero(void){return 0;}

int main(void)
{
    int sz = 1000 * 1000 * 1000;
    for (int i=0; i<sz; i++) {
        int a = zero();
    }
    return 0;
}
