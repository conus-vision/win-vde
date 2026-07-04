// vdtest.cpp — tiny CHECK-macro harness for win-vde pure logic.
// Build/run via build-test.bat. Exit code 0 = all passed.
#define UNICODE
#define _UNICODE
#include <cstdio>
#include "str_util.hpp"

static int g_fail = 0, g_total = 0;
#define CHECK(c) do{ g_total++; if(!(c)){ g_fail++; printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c);} }while(0)

static void test_etld1(){
    CHECK(etld1("mail.google.com") == "google.com");
    CHECK(etld1("docs.python.org") == "python.org");
    CHECK(hostOf("https://www.GitHub.com/x/y") == "github.com");
}
static void test_b64(){
    std::string s = "PR #42 \xE2\x80\x94 Mozilla";   // includes a UTF-8 em dash
    CHECK(b64dec(b64enc(s)) == s);
}

int main(){
    test_etld1();
    test_b64();
    printf("%d/%d passed\n", g_total - g_fail, g_total);
    return g_fail ? 1 : 0;
}
