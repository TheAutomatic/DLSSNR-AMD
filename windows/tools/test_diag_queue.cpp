#include "diag_queue_test/tlhelp32.h"
#include "../src/pe/nr_pe_diag_queue.hpp"

#include <cassert>
#include <chrono>
#include <iostream>

namespace diag = nr::pe::vkdevice::diag;

int main() {
    const auto device = reinterpret_cast<VkDevice>(3);
    VkDeviceQueueCreateInfo qi{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO}; qi.queueCount = 1;
    VkDeviceCreateInfo ci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO}; ci.queueCreateInfoCount = 1; ci.pQueueCreateInfos = &qi;
    diag::install(device, &ci);
    assert(messages.empty() && hooks == 0);
    diag::enabled = true;
    diag::install(device, &ci);
    assert(messages.back().find("INCOMPLETE") != std::string::npos && hooks == 0);
    have_driver = true; hook_failure = true;
    diag::install(device, &ci);
    assert(messages.back().find("INCOMPLETE") != std::string::npos && hooks == 0);
    hook_failure = false;
    diag::install(device, &ci);
    assert(hooks == 6 && diag::states.size() == 1);
    assert(messages.back().find("vkQueueSubmit2KHR=") != std::string::npos);
    assert(messages.back().find("/alias") != std::string::npos);
    diag::install(device, &ci);
    assert(hooks == 6 && messages.back().find("INCOMPLETE") == std::string::npos);
    assert(diag::device_queues(reinterpret_cast<VkDevice>(4)).empty());
    assert(diag::hooked_submit(test_queue, 0, nullptr, VK_NULL_HANDLE) == VK_SUCCESS);
    assert(diag::hooked_device_wait_idle(device) == VK_SUCCESS);
    {
        diag::Call first(test_queue);
        diag::Call recursive(test_queue);
        assert(diag::contended == 0);
    }
    const auto owner = GetCurrentThreadId();
    std::thread worker;
    {
        diag::Call first(test_queue);
        worker = std::thread([] {
            diag::build_thread = GetCurrentThreadId();
            diag::Call blocked(test_queue);
        });
        const auto limit = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (diag::contended == 0 && std::chrono::steady_clock::now() < limit) std::this_thread::yield();
        assert(diag::contended == 1);
        assert(diag::build_contended == 1);
        assert(uint32_t(diag::threads.load()) == owner);
        assert(DWORD(diag::threads.load() >> 32) == diag::build_thread);
    }
    worker.join();
    const auto q2 = reinterpret_cast<VkQueue>(5), q3 = reinterpret_cast<VkQueue>(6);
    diag::states[q2] = std::make_shared<diag::QueueState>(device);
    diag::states[q3] = std::make_shared<diag::QueueState>(reinterpret_cast<VkDevice>(4));
    assert(diag::device_queues(device).size() == 2);
    std::atomic<bool> entered{false};
    {
        diag::Call all(diag::device_queues(device));
        worker = std::thread([&] { diag::Call blocked(q2); entered = true; });
        const auto limit = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (diag::contended < 2 && std::chrono::steady_clock::now() < limit) std::this_thread::yield();
        assert(diag::contended == 2 && !entered);
    }
    worker.join();
    assert(entered && diag::unknown == 0);
    { diag::Call unknown(reinterpret_cast<VkQueue>(7)); }
    assert(diag::unknown == 1);
    std::cout << "PASS: install disabled/missing-ICD/hook-failure/success/alias/reinstall; recursive and contended queue calls; build-thread IDs; device-wide locking; unknown queue accounting\n";
}
