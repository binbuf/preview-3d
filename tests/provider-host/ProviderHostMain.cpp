// T17 host entry point. Catch2 session exactly like Tests.Unit.exe, so the host
// takes the same filters (`x64\Release\Tests.ProviderHost.exe "[host][golden]"`).
#include <catch2/catch_session.hpp>
#include <catch2/catch_test_macros.hpp>

#ifdef _DEBUG
#include <crtdbg.h>
#endif

int main(int argc, char* argv[])
{
#ifdef _DEBUG
    // A CI/host run must not block on a modal CRT assertion dialog; route any
    // report to stderr so a failure is visible in the captured log instead.
    _CrtSetReportMode(_CRT_ASSERT, _CRTDBG_MODE_FILE);
    _CrtSetReportFile(_CRT_ASSERT, _CRTDBG_FILE_STDERR);
    _CrtSetReportMode(_CRT_ERROR, _CRTDBG_MODE_FILE);
    _CrtSetReportFile(_CRT_ERROR, _CRTDBG_FILE_STDERR);
    _CrtSetReportMode(_CRT_WARN, _CRTDBG_MODE_FILE);
    _CrtSetReportFile(_CRT_WARN, _CRTDBG_FILE_STDERR);
#endif
    return Catch::Session().run(argc, argv);
}