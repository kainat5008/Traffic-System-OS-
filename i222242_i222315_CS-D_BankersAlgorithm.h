// BankersAlgorithm.h

#ifndef BANKERS_ALGORITHM_H
#define BANKERS_ALGORITHM_H

#include <vector>
#include <mutex>

class BankersAlgorithm {
private:
    int num_resources;
    int num_processes;
    std::vector<int> total_resources;
    std::vector<int> available;
    std::vector<std::vector<int>> maximum;
    std::vector<std::vector<int>> allocation;
    std::vector<std::vector<int>> need;
    std::mutex mtx; // Mutex for thread safety

public:
    BankersAlgorithm(int resources, int processes);
    void setTotalResources(const std::vector<int>& total);
    void setMaximum(int process, const std::vector<int>& max_demand);
    bool requestResources(int process, const std::vector<int>& request);
    void releaseResources(int process, const std::vector<int>& release);
    bool isSafe();
};

#endif // BANKERS_ALGORITHM_H

