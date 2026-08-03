/**
 * @file image-memory.hpp
 * @brief Owns a loaded library's PT_LOAD segments in RAM.
 */
#pragma once
#include "loader/elf-image.hpp"
#include <cstddef>
#include <memory>

namespace firmware {

/** @brief Allocated RAM buffer containing the loaded image of a shared library. */
class image_memory {
public:
    /** @brief Default constructor — no allocation. */
    image_memory() noexcept = default;

    /** @brief Load PT_LOAD segments from ELF into allocated buffer, with extra tail space for journal. */
    [[nodiscard]] loader::result<void>
    load_segments(loader::elf_image& image,
                  std::size_t extra_bytes = 0) noexcept;

    /** @brief Start of the allocated buffer. */
    [[nodiscard]] std::byte* base() const noexcept { return buf_.get(); }
    /** @brief Total allocated size (image + extra_bytes tail). */
    [[nodiscard]] std::size_t size() const noexcept { return size_; }
    /** @brief True after load_segments succeeded. */
    [[nodiscard]] bool is_loaded() const noexcept { return buf_ != nullptr; }

    image_memory(image_memory&&) noexcept = default;
    image_memory& operator=(image_memory&&) noexcept = default;
    image_memory(const image_memory&) = delete;
    image_memory& operator=(const image_memory&) = delete;

private:
    std::unique_ptr<std::byte[]> buf_; ///< Heap-allocated image buffer
    std::size_t size_ = 0;             ///< Total buffer size in bytes
};

} // namespace firmware
