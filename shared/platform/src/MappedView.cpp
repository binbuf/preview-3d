#include "platform/MappedView.h"

#include <utility>

namespace platform {

MappedView::MappedView(MappedView&& other) noexcept
    : view_(std::exchange(other.view_, nullptr))
    , size_(std::exchange(other.size_, 0))
{
}

MappedView& MappedView::operator=(MappedView&& other) noexcept
{
    if (this != &other) {
        Reset();
        view_ = std::exchange(other.view_, nullptr);
        size_ = std::exchange(other.size_, 0);
    }
    return *this;
}

MappedView::~MappedView()
{
    Reset();
}

void MappedView::Reset() noexcept
{
    if (view_ != nullptr) {
        UnmapViewOfFile(view_);
        view_ = nullptr;
    }
    size_ = 0;
}

MappedView MappedView::Map(HANDLE section, DWORD desiredAccess, SIZE_T sizeBytes, uint64_t offset)
{
    DWORD offsetHigh = static_cast<DWORD>(offset >> 32);
    DWORD offsetLow = static_cast<DWORD>(offset & 0xFFFFFFFFu);
    void* view = MapViewOfFile(section, desiredAccess, offsetHigh, offsetLow, sizeBytes);
    if (view == nullptr) {
        return MappedView();
    }

    SIZE_T actualSize = sizeBytes;
    if (actualSize == 0) {
        MEMORY_BASIC_INFORMATION info{};
        if (VirtualQuery(view, &info, sizeof(info)) != 0) {
            actualSize = info.RegionSize;
        }
    }

    return MappedView(view, actualSize);
}

std::span<std::byte> MappedView::bytes() noexcept
{
    return std::span<std::byte>(static_cast<std::byte*>(view_), size_);
}

std::span<const std::byte> MappedView::bytes() const noexcept
{
    return std::span<const std::byte>(static_cast<const std::byte*>(view_), size_);
}

} // namespace platform
