#pragma once

#include "library.h"
#include <array>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <thread>
#include <unordered_map>

struct Thumbnail {
    static constexpr int width = 192, height = 108;
    alignas(64) std::array<std::uint32_t, width * height> pixels{};
};

class Thumbnails {
public:
    ~Thumbnails();
    void reset();
    void show(const std::vector<int>& indices, const std::vector<LibraryEntry>& entries);
    std::shared_ptr<const Thumbnail> get(int index) const;
    bool changed();

private:
    struct Request { int index; fs::path path; };
    struct Cached { std::shared_ptr<const Thumbnail> image; std::uint64_t lastVisible; };

    mutable std::mutex mutex_;
    std::condition_variable wake_;
    std::thread worker_;
    std::vector<int> indices_;
    std::vector<Request> visible_;
    std::unordered_map<int, Cached> cache_;
    std::uint64_t generation_ = 0, revision_ = 0;
    bool stopping_ = false, changed_ = false;

    bool wanted(int index) const;
    void run();
};
