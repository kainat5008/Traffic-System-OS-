// BankersAlgorithm.cpp

#include "BankersAlgorithm.h"
#include <algorithm>

BankersAlgorithm::BankersAlgorithm(int resources, int processes)
    : num_resources(resources), num_processes(processes),
      total_resources(resources, 0), available(resources, 0),
      maximum(processes, std::vector<int>(resources, 0)),
      allocation(processes, std::vector<int>(resources, 0)),
      need(processes, std::vector<int>(resources, 0)) {}

void BankersAlgorithm::setTotalResources(const std::vector<int>& total) {
    std::lock_guard<std::mutex> lock(mtx);
    total_resources = total;
    available = total;
}

void BankersAlgorithm::setMaximum(int process, const std::vector<int>& max_demand) {
    std::lock_guard<std::mutex> lock(mtx);
    maximum[process] = max_demand;
    for (int i = 0; i < num_resources; ++i) {
        need[process][i] = maximum[process][i] - allocation[process][i];
    }
}

bool BankersAlgorithm::isSafe() {
    std::vector<int> work = available;
    std::vector<bool> finish(num_processes, false);
    bool progress = true;

    while (progress) {
        progress = false;
        for (int p = 0; p < num_processes; ++p) {
            if (!finish[p]) {
                bool can_finish = true;
                for (int r = 0; r < num_resources; ++r) {
                    if (need[p][r] > work[r]) {
                        can_finish = false;
                        break;
                    }
                }
                if (can_finish) {
                    for (int r = 0; r < num_resources; ++r) {
                        work[r] += allocation[p][r];
                    }
                    finish[p] = true;
                    progress = true;
                }
            }
        }
    }

    return std::all_of(finish.begin(), finish.end(), [](bool f) { return f; });
}

bool BankersAlgorithm::requestResources(int process, const std::vector<int>& request) {
    std::lock_guard<std::mutex> lock(mtx);

    // Check if request <= need
    for (int r = 0; r < num_resources; ++r) {
        if (request[r] > need[process][r]) {
            return false; // Exceeds maximum demand
        }
    }

    // Check if request <= available
    for (int r = 0; r < num_resources; ++r) {
        if (request[r] > available[r]) {
            return false; // Not enough resources
        }
    }

    // Try to allocate
    for (int r = 0; r < num_resources; ++r) {
        available[r] -= request[r];
        allocation[process][r] += request[r];
        need[process][r] -= request[r];
    }

    // Check if state is safe
    if (isSafe()) {
        return true; // Allocation successful
    } else {
        // Rollback
        for (int r = 0; r < num_resources; ++r) {
            available[r] += request[r];
            allocation[process][r] -= request[r];
            need[process][r] += request[r];
        }
        return false; // Allocation not safe
    }
}

void BankersAlgorithm::releaseResources(int process, const std::vector<int>& release) {
    std::lock_guard<std::mutex> lock(mtx);
    for (int r = 0; r < num_resources; ++r) {
        allocation[process][r] -= release[r];
        available[r] += release[r];
        need[process][r] += release[r];
    }
}

