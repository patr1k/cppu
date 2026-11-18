#include <array>
#include <cstdint>

namespace cppu {

struct Pixel {
    std::uint8_t color      = 0;
    std::uint8_t palette    = 0;
    bool bg_priority        = false;
};

class PixelFifo {
public:
    void push(Pixel p) { 
        buf_[(head_ + size_++) & kMask] = p; 
    }
    
    Pixel pop() {
        Pixel p = buf_[head_];
        head_ = (head_ + 1) & kMask;
        --size_;
        return p;
    }
    
    Pixel& at(std::uint8_t i) {
        return buf_[(head_ + i) & kMask];
    }

    std::uint8_t size() const { return size_; }

    bool empty() const { return size_ == 0; }

    void clear() {
        head_ = 0;
        size_ = 0;
    }

private:
    static constexpr std::uint8_t kCapacity = 16;
    static constexpr std::uint8_t kMask = kCapacity - 1;
    std::array<Pixel, kCapacity> buf_{};
    std::uint8_t head_ = 0;
    std::uint8_t size_ = 0;
};

}