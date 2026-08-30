// Stub LoraInterface — the TX path produces buffers that CmdDispatcher's
// send_tx_buffer pulls from a free-buffer queue. The test captures the
// bytes that get queued for transmission.
#pragma once
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include "Packet.h"      // CmdDispatcher.h relies on Packet via LoraInterface
#include <cstdint>
#include <cstring>
#include <vector>

class LoraInterface {
public:
    static const int  BUFFER_SIZE          = 256;
    static const int  POOL_SIZE            = 5;
    static const uint8_t broadcastAddressing = 0xFF;

    struct rx_buffer_t {
        uint8_t data[BUFFER_SIZE];
        size_t  length{0};
        uint32_t timestamp{0};
        uint32_t magic{0};
        uint8_t  in_use{0};
        uint8_t  ref_count{0};
    };

    struct {
        QueueHandle_t data_queue{nullptr};
    } tx_memory_pool;

    LoraInterface() {
        tx_memory_pool.data_queue = xQueueCreate(POOL_SIZE, sizeof(rx_buffer_t*));
        for (auto& b : pool_) {
            free_.push_back(&b);
        }
    }

    rx_buffer_t* get_free_tx_buffer(uint32_t /*timeout*/) {
        if (free_.empty()) return nullptr;
        auto* b = free_.back();
        free_.pop_back();
        return b;
    }

    void return_tx_buffer_to_pool(rx_buffer_t* b) {
        if (b) free_.push_back(b);
    }

    void stopLoraPollingTimer() {}

    // Test inspection: drain the TX queue and return the buffer bytes.
    std::vector<std::vector<uint8_t>> drain_tx_queue() {
        std::vector<std::vector<uint8_t>> out;
        rx_buffer_t* b = nullptr;
        while (xQueueReceive(tx_memory_pool.data_queue, &b, 0) == pdTRUE) {
            out.emplace_back(b->data, b->data + b->length);
            return_tx_buffer_to_pool(b);
        }
        return out;
    }

    // Drift test: the real class switches the radio between windowed rxSingle
    // and continuous RX. Nothing here drives a radio, but the sim asserts on
    // the flag — a test can then prove the node LEAVES continuous RX, which is
    // the property that keeps a bench test from flattening the pack.
    void setContinuousRx(bool on) { continuous_rx_ = on; }
    bool continuousRx() const { return continuous_rx_; }

private:
    bool                     continuous_rx_{false};
    rx_buffer_t              pool_[POOL_SIZE];
    std::vector<rx_buffer_t*> free_;
};
