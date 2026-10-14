#pragma once

#include <cstdint>
#include <cstdio>
#include <stdexcept>
#include <string>

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include "byteswap.hpp"
#include "parser.hpp"

namespace itch {

class MmapReader {
public:
    explicit MmapReader(const std::string& path) {
        fd_ = open(path.c_str(), O_RDONLY);
        if (fd_ < 0) {
            throw std::runtime_error("failed to open file: " + path);
        }
        struct stat st;
        if (fstat(fd_, &st) != 0) {
            close(fd_);
            throw std::runtime_error("fstat failed: " + path);
        }
        size_ = static_cast<size_t>(st.st_size);
        data_ = static_cast<const uint8_t*>(
            mmap(nullptr, size_, PROT_READ, MAP_PRIVATE, fd_, 0));
        if (data_ == MAP_FAILED) {
            close(fd_);
            throw std::runtime_error("mmap failed: " + path);
        }
    }

    ~MmapReader() {
        if (data_ && data_ != MAP_FAILED) munmap(const_cast<uint8_t*>(data_), size_);
        if (fd_ >= 0) close(fd_);
    }

    MmapReader(const MmapReader&) = delete;
    MmapReader& operator=(const MmapReader&) = delete;

    // Walks every 2-byte length-prefixed message in the file and dispatches
    // it via the given callbacks. Returns the number of messages processed.
    size_t process_all(const ParserCallbacks& cb) const {
        size_t offset = 0;
        size_t count = 0;
        while (offset + 2 <= size_) {
            uint16_t msg_len = read_be16(data_ + offset);
            offset += 2;
            if (msg_len == 0 || offset + msg_len > size_) break;
            dispatch_message(data_ + offset, msg_len, cb);
            offset += msg_len;
            ++count;
        }
        return count;
    }

private:
    int fd_ = -1;
    const uint8_t* data_ = nullptr;
    size_t size_ = 0;
};

} // namespace itch
