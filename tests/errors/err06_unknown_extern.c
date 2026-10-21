// EXPECT: link line=6 match=undefined|unresolved|not found|missing
int not_in_firmware(int x);

int main(void)
{
    return not_in_firmware(3);
}
