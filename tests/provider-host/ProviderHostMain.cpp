// T17 host entry point. Catch2 session exactly like Tests.Unit.exe, so the host
// takes the same filters (`x64\Release\Tests.ProviderHost.exe "[host][golden]"`).
#include <catch2/catch_session.hpp>
#include <catch2/catch_test_macros.hpp>

int main(int argc, char* argv[])
{
    return Catch::Session().run(argc, argv);
}