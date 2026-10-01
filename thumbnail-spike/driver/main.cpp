// SPIKE-8a (T02) standalone driver. No COM, no GPU: it links only the prototype
// rasterizer library and measures the prototype outside Explorer. T03 embeds
// the same Render() entry point in its Shell-surrogate probe.

#include "../rasterizer/PamImage.h"
#include "../rasterizer/SceneFixtures.h"
#include "../rasterizer/ThumbnailRasterizer.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include <windows.h>
#include <psapi.h>

namespace
{

using thumbnail_rasterizer::GeometryView;
using thumbnail_rasterizer::Image;
using thumbnail_rasterizer::Options;
using thumbnail_rasterizer::Render;
using thumbnail_rasterizer::Status;

double NowMs()
{
    static LARGE_INTEGER frequency{};
    if (frequency.QuadPart == 0) QueryPerformanceFrequency(&frequency);
    LARGE_INTEGER counter{};
    QueryPerformanceCounter(&counter);
    return (static_cast<double>(counter.QuadPart) / static_cast<double>(frequency.QuadPart)) * 1000.0;
}

struct MemoryStats
{
    std::uint64_t privateBytes = 0;
    std::uint64_t peakCommit = 0;
};

MemoryStats ReadMemory()
{
    PROCESS_MEMORY_COUNTERS_EX pmc{};
    pmc.cb = sizeof(pmc);
    MemoryStats stats;
    if (GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&pmc),
                             sizeof(pmc))) {
        stats.privateBytes = pmc.PrivateUsage;
        stats.peakCommit = pmc.PeakPagefileUsage;
    }
    return stats;
}

std::uint64_t Fnv1a(const std::vector<std::uint8_t>& bytes)
{
    std::uint64_t hash = 1469598103934665603ull;
    for (std::uint8_t byte : bytes) {
        hash ^= byte;
        hash *= 1099511628211ull;
    }
    return hash;
}

std::string Join(const std::string& dir, const char* name)
{
    if (dir.empty()) return name;
    const char last = dir.back();
    if (last == '\\' || last == '/') return dir + name;
    return dir + "\\" + name;
}

int RenderGolden(const std::string& dir, bool points, int size, double& elapsedOut)
{
    GeometryView geometry;
    thumbnail_spike::MeshScene mesh;
    thumbnail_spike::PointScene cloud;
    if (points) {
        cloud = thumbnail_spike::MakeGoldenPointCloud();
        geometry.points = cloud.points;
    } else {
        mesh = thumbnail_spike::MakeGoldenMesh();
        geometry.triangles = mesh.triangles;
    }

    Options options;
    options.size = size;
    options.supersample = 2;
    options.floorShadow = true;

    Image image;
    const double start = NowMs();
    const Status status = Render(geometry, options, image);
    elapsedOut = NowMs() - start;
    if (status != Status::Ok) {
        std::printf("  %s-%d: Render failed (%d)\n", points ? "points" : "mesh", size,
                    static_cast<int>(status));
        return 1;
    }
    const std::string path =
        Join(dir, (std::string(points ? "points-" : "mesh-") + std::to_string(size) + ".pam").c_str());
    if (!thumbnail_rasterizer::SaveBgraAsPam(path.c_str(), image.width, image.height,
                                             image.bgraPremultiplied)) {
        std::printf("  failed to write %s\n", path.c_str());
        return 1;
    }
    std::printf("  %-14s %4d px  %8.2f ms  fnv=%016llx  %s\n", points ? "points" : "mesh", size,
                elapsedOut, static_cast<unsigned long long>(Fnv1a(image.bgraPremultiplied)),
                path.c_str());
    return 0;
}

int RunGoldens(const std::string& dir)
{
    std::printf("goldens -> %s\n", dir.empty() ? "(cwd)" : dir.c_str());
    const int sizes[] = { 32, 64, 256, 512 };
    int failures = 0;
    for (int size : sizes) {
        double elapsed = 0.0;
        failures += RenderGolden(dir, false, size, elapsed);
        failures += RenderGolden(dir, true, size, elapsed);
    }
    return failures == 0 ? 0 : 1;
}

int RunPerf()
{
    std::printf("provider-cap measurements (512 px, 2x supersampling)\n");

    auto mesh = thumbnail_spike::GenerateLargeMesh(2000000, 250000, thumbnail_spike::SeedFromString("spike8-mesh"));
    auto cloud = thumbnail_spike::GenerateLargePointCloud(6000000, 250000, thumbnail_spike::SeedFromString("spike8-points"));

    GeometryView meshView;
    meshView.triangles = mesh.triangles;
    GeometryView cloudView;
    cloudView.points = cloud.points;

    const int failures = 0;
    for (int ss : { 2, 1 }) {
        Options options;
        options.size = 512;
        options.supersample = ss;
        options.floorShadow = true;

        Image meshImage;
        const double meshStart = NowMs();
        const Status meshStatus = Render(meshView, options, meshImage);
        const double meshMs = NowMs() - meshStart;
        const MemoryStats meshMem = ReadMemory();

        Image cloudImage;
        const double cloudStart = NowMs();
        const Status cloudStatus = Render(cloudView, options, cloudImage);
        const double cloudMs = NowMs() - cloudStart;
        const MemoryStats cloudMem = ReadMemory();

        std::printf("  supersample=%d\n", ss);
        std::printf("    mesh   inspected=%llu kept=%llu status=%d time=%.1f ms "
                    "private=%.1f MiB peakCommit=%.1f MiB\n",
                    static_cast<unsigned long long>(mesh.inspected),
                    static_cast<unsigned long long>(mesh.triangles.size()),
                    static_cast<int>(meshStatus), meshMs,
                    static_cast<double>(meshMem.privateBytes) / (1024.0 * 1024.0),
                    static_cast<double>(meshMem.peakCommit) / (1024.0 * 1024.0));
        std::printf("    points inspected=%llu kept=%llu status=%d time=%.1f ms "
                    "private=%.1f MiB peakCommit=%.1f MiB\n",
                    static_cast<unsigned long long>(cloud.inspected),
                    static_cast<unsigned long long>(cloud.points.size()),
                    static_cast<int>(cloudStatus), cloudMs,
                    static_cast<double>(cloudMem.privateBytes) / (1024.0 * 1024.0),
                    static_cast<double>(cloudMem.peakCommit) / (1024.0 * 1024.0));
    }

    Options cancelledOptions;
    cancelledOptions.size = 512;
    cancelledOptions.supersample = 2;
    Image cancelled;
    const Status cancelStatus = Render(meshView, cancelledOptions, cancelled,
                                       [](void*) { return true; }, nullptr);
    std::printf("    cooperative-cancel status=%d\n", static_cast<int>(cancelStatus));
    return failures;
}

void PrintUsage()
{
    std::printf("usage: RasterizerSpike <goldens [outDir] | perf>\n");
}

} // namespace

int main(int argc, char** argv)
{
    if (argc < 2) {
        PrintUsage();
        return 2;
    }
    const std::string mode = argv[1];
    if (mode == "goldens") {
        const std::string dir = argc >= 3 ? argv[2] : "thumbnail-spike\\goldens";
        return RunGoldens(dir);
    }
    if (mode == "perf") return RunPerf();
    PrintUsage();
    return 2;
}