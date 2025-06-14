// main.cpp

#include "BankersAlgorithm.h" // Include the Banker's Algorithm header
#include <SFML/Graphics.hpp>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#include <semaphore.h>
#include <fcntl.h>
#include <pthread.h>
#include <mqueue.h>
#include <csignal>
#include <iostream>
#include <random>
#include <deque>
#include <map>
#include <vector>
#include <mutex>
#include <atomic>
#include <chrono>
#include <cstring>
#include <string>
#include <thread>
#include <cerrno>
#include <cstddef> // For size_t
#include <fstream>

// Resource types
enum ResourceType { LANE_SEM, ACTIVE_VEHICLES_SEM, NUM_RESOURCE_TYPES };

// Process identifiers
enum ProcessID { TRAFFIC_LIGHT_CONTROLLER, SPAWN_VEHICLES, SPEED_MANAGER, OUT_OF_ORDER, MOCK_TIME, CHALLAN_GENERATOR, STRIPE_PAYMENT, USER_PORTAL, NUM_PROCESSES };

// Constants for message queues
#define MQ_PORTAL_STATUS "/portal_status"
#define MQ_SMART_TO_CHALLAN "/smart_to_challan"
#define MQ_STRIPE_TO_CHALLAN "/stripe_to_challan"
#define MQ_CHALLAN_TO_SMART "/challan_to_smart"
#define MQ_MAX_SIZE 256

// Structures for messages
struct PortalStatusMsg {
    char status[16]; // "active" or "inactive"
};

struct SpeedViolationMsg {
    char vehicleID[32];
    int vehicleType; // 1=Light,2=Heavy,3=Emergency
    float speed;
};

struct PaymentMsg {
    char vehicleID[32];
    bool paid;
};

struct ChallanUpdateMsg {
    char vehicleID[32];
    bool paid;
};

// Enum for vehicle types
enum VehicleType { LIGHT, HEAVY, EMERGENCY };

// Enum for traffic light states
enum TrafficLightState { RED, YELLOW, GREEN };

// Vehicle structure
struct Vehicle {
    VehicleType type;
    float maxSpeed;
    sf::Sprite sprite;
    sf::Vector2f speedVector;
    float currentSpeed;
    std::string numberPlate;
    bool challanActive = false;
    bool outOfOrder = false;
    bool isTowed = false;
    std::string laneName; // Added lane information
};

// Traffic Light structure
struct TrafficLight {
    std::string direction; // e.g., North, South, East, West
    TrafficLightState state;
    sf::CircleShape lightShape;
};

// LaneQueue structure
struct LaneQueue {
    std::deque<Vehicle> vehicles;
    int maxCapacity = 10;
};

// MockTime structure
struct MockTime {
    int hour = 0;
    int minute = 0;
    
    void incrementTime(int minutes) {
        minute += minutes;
        while (minute >= 60) {
            minute -= 60;
            hour = (hour + 1) % 24;
        }
    }
    
    bool isPeakHours() const {
        return (hour >= 7 && hour <= 9) || (hour >= 16 && hour <= 19);
    }
};

// Global variables and synchronization primitives
std::atomic<bool> portalActive(false);
std::atomic<bool> running(true);
std::mutex printMutex;

// Safe print function
template<typename... Args>
void safePrint(Args... args) {
    if (!portalActive.load()) {
        std::lock_guard<std::mutex> lock(printMutex);
        (std::cout << ... << args) << std::endl;
    }
}

// Global texture variables
sf::Texture roadTexture, carTexture1, carTexture2, towTruckTexture;

// Other global variables (queues, semaphores)
static std::vector<Vehicle> activeVehicles;
static std::map<std::string, LaneQueue> laneQueues;
static std::map<std::string, bool> activeChallans; // vehicleID -> challanActive

sem_t *laneSem = SEM_FAILED;            // Protects laneQueues
sem_t *activeVehiclesSem = SEM_FAILED;  // Protects activeVehicles

// Analytics Counters
static int totalChallansIssued = 0;
static int totalChallansPaid = 0;
static int totalVehiclesOutOfOrder = 0;

// Message Queue Handles
mqd_t mqSmartToChallan = (mqd_t)-1;
mqd_t mqStripeToChallan = (mqd_t)-1;
mqd_t mqChallanToSmart = (mqd_t)-1;
mqd_t mqPortalStatusHandle = (mqd_t)-1;

// Mock Time
MockTime mockTime;

// Traffic Lights
std::map<std::string, TrafficLight> trafficLights;

// Initialize Banker's Algorithm
BankersAlgorithm banker(NUM_RESOURCE_TYPES, NUM_PROCESSES);

// Function Declarations
void handleCollisionsAndCleanup();
void performCleanup(pid_t pid1, pid_t pid2, pid_t pid3, pid_t pid4);
void cleanupAndExit(int signum);
void trafficLightControllerThread();
void spawnVehiclesThread(sf::Texture *carTexture1, sf::Texture *carTexture2, sf::Texture *towTruckTexture);
void speedManagerThread();
void outOfOrderThread();
void mockTimeThread();
void challanGeneratorProcess();
void stripePaymentProcess();
void userPortalProcess();
void processQueues();
void runDeadlockPrevention();
void visualizeTraffic(sf::RenderWindow &window, sf::Sprite &roadSprite, sf::Font &font, sf::Text &analyticsText);
bool acquireResource(int process, ResourceType res);
void releaseResource(int process, ResourceType res);
void initializeBankers();
void initializeTrafficLights();

// Function Definitions

// Initialize Banker's Algorithm
void initializeBankers() {
    // Define total resources
    std::vector<int> total = {1, 1}; // LANE_SEM and ACTIVE_VEHICLES_SEM
    banker.setTotalResources(total);

    // Define maximum demands for each process
    // For simplicity, assuming each process may require up to 1 unit of each semaphore
    std::vector<int> max_demand = {1, 1};
    for (int p = 0; p < NUM_PROCESSES; ++p) {
        banker.setMaximum(p, max_demand);
    }
}

// Acquire resource using Banker's Algorithm
bool acquireResource(int process, ResourceType res) {
    std::vector<int> request(NUM_RESOURCE_TYPES, 0);
    request[res] = 1; // Requesting 1 unit of the semaphore

    if (banker.requestResources(process, request)) {
        // If allocation is safe, proceed to acquire the semaphore
        if (res == LANE_SEM) {
            if (sem_wait(laneSem) == -1) {
                perror("sem_wait laneSem");
                banker.releaseResources(process, request); // Release resources on failure
                return false;
            }
        }
        else if (res == ACTIVE_VEHICLES_SEM) {
            if (sem_wait(activeVehiclesSem) == -1) {
                perror("sem_wait activeVehiclesSem");
                banker.releaseResources(process, request); // Release resources on failure
                return false;
            }
        }
        return true;
    }
    else {
        // Allocation not safe or resources not available
        return false;
    }
}

// Release resource using Banker's Algorithm
void releaseResource(int process, ResourceType res) {
    std::vector<int> release(NUM_RESOURCE_TYPES, 0);
    release[res] = 1; // Releasing 1 unit of the semaphore

    // Release the semaphore
    if (res == LANE_SEM) {
        if (sem_post(laneSem) == -1) {
            perror("sem_post laneSem");
        }
    }
    else if (res == ACTIVE_VEHICLES_SEM) {
        if (sem_post(activeVehiclesSem) == -1) {
            perror("sem_post activeVehiclesSem");
        }
    }

    // Inform the Banker's Algorithm about the release
    banker.releaseResources(process, release);
}

// Initialize Traffic Lights
void initializeTrafficLights() {
    // Define directions
    std::vector<std::string> directions = {"North", "South", "East", "West"};

    // Initialize traffic lights with initial states
    for (const auto &dir : directions) {
        TrafficLight tl;
        tl.direction = dir;
        tl.state = RED;

        // Initialize SFML CircleShape for visualization
        tl.lightShape = sf::CircleShape(10.f);
        if (dir == "North") {
            tl.lightShape.setPosition(380.f, 50.f);
        }
        else if (dir == "South") {
            tl.lightShape.setPosition(410.f, 500.f);
        }
        else if (dir == "East") {
            tl.lightShape.setPosition(700.f, 250.f);
        }
        else if (dir == "West") {
            tl.lightShape.setPosition(100.f, 310.f);
        }

        tl.lightShape.setFillColor(sf::Color::Red);
        trafficLights[dir] = tl;
    }

    // Set initial green light for North-South
    trafficLights["North"].state = GREEN;
    trafficLights["South"].state = GREEN;
    trafficLights["North"].lightShape.setFillColor(sf::Color::Green);
    trafficLights["South"].lightShape.setFillColor(sf::Color::Green);
}

// Traffic Light Controller Thread
void trafficLightControllerThread() {
    std::vector<std::string> directions = {"North", "South", "East", "West"};
    size_t currentGreenIndex = 0; // Start with North-South

    while (running) {
        // Green phase
        std::this_thread::sleep_for(std::chrono::seconds(10)); // Green lasts 10 seconds

        // Transition to Yellow
        {
            std::lock_guard<std::mutex> lock(printMutex);
            trafficLights[directions[currentGreenIndex]].state = YELLOW;
            trafficLights[directions[currentGreenIndex]].lightShape.setFillColor(sf::Color::Yellow);
            trafficLights[directions[currentGreenIndex] + (currentGreenIndex == 0 ? "" : "")].state = YELLOW;
            trafficLights[directions[currentGreenIndex] + (currentGreenIndex == 0 ? "" : "")].lightShape.setFillColor(sf::Color::Yellow);
            safePrint("[TrafficLightController] " + directions[currentGreenIndex] + " traffic lights turned YELLOW.");
        }

        std::this_thread::sleep_for(std::chrono::seconds(3)); // Yellow lasts 3 seconds

        // Transition to Red
        {
            std::lock_guard<std::mutex> lock(printMutex);
            trafficLights[directions[currentGreenIndex]].state = RED;
            trafficLights[directions[currentGreenIndex]].lightShape.setFillColor(sf::Color::Red);
            trafficLights[directions[currentGreenIndex] + (currentGreenIndex == 0 ? "" : "")].state = RED;
            trafficLights[directions[currentGreenIndex] + (currentGreenIndex == 0 ? "" : "")].lightShape.setFillColor(sf::Color::Red);
            safePrint("[TrafficLightController] " + directions[currentGreenIndex] + " traffic lights turned RED.");
        }

        // Move to next direction
        currentGreenIndex = (currentGreenIndex + 2) % directions.size(); // Toggle between NS and EW

        // Set next direction to Green
        {
            std::lock_guard<std::mutex> lock(printMutex);
            trafficLights[directions[currentGreenIndex]].state = GREEN;
            trafficLights[directions[currentGreenIndex]].lightShape.setFillColor(sf::Color::Green);
            trafficLights[directions[currentGreenIndex] + (currentGreenIndex == 0 ? "" : "")].state = GREEN;
            trafficLights[directions[currentGreenIndex] + (currentGreenIndex == 0 ? "" : "")].lightShape.setFillColor(sf::Color::Green);
            safePrint("[TrafficLightController] " + directions[currentGreenIndex] + " traffic lights turned GREEN.");
        }
    }
}

// Spawn Vehicles Thread
void spawnVehiclesThread(sf::Texture *carTexture1, sf::Texture *carTexture2, sf::Texture *towTruckTexture) {
    std::vector<std::string> lanes = {"North1", "North2", "South1", "South2", "East1", "East2", "West1", "West2"};
    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<> laneDist(0, static_cast<int>(lanes.size()) - 1);
    std::uniform_int_distribution<> typeDist(1, 3); // 1=Light,2=Heavy,3=Emergency

    while (running) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1000));
        std::string selectedLane = lanes[laneDist(gen)];

        // Acquire LANE_SEM using Banker's Algorithm
        if (!acquireResource(SPAWN_VEHICLES, LANE_SEM)) {
            safePrint("[Banker] SpawnVehicles: Waiting for LANE_SEM resource.");
            continue; // Retry in the next iteration
        }

        if (static_cast<int>(laneQueues[selectedLane].vehicles.size()) < laneQueues[selectedLane].maxCapacity) {
            int vehicleTypeChoice = typeDist(gen);
            // Check peak hours restriction for heavy
            if (vehicleTypeChoice == 2 && mockTime.isPeakHours()) {
                safePrint("[SpawnVehicles] Heavy vehicle attempted to spawn during peak hours. Skipping.");
                releaseResource(SPAWN_VEHICLES, LANE_SEM);
                continue;
            }

            Vehicle newVehicle;
            if (vehicleTypeChoice == 1) {
                newVehicle.type = LIGHT;
                newVehicle.maxSpeed = 60.0f;
                newVehicle.sprite.setTexture(*carTexture1);
            } else if (vehicleTypeChoice == 2) {
                newVehicle.type = HEAVY;
                newVehicle.maxSpeed = 40.0f;
                newVehicle.sprite.setTexture(*carTexture2);
            } else {
                newVehicle.type = EMERGENCY;
                newVehicle.maxSpeed = 90.0f; // Faster for emergency
                newVehicle.sprite.setTexture(*towTruckTexture); // Use tow truck texture for emergency
            }

            newVehicle.sprite.setScale(0.05f, 0.05f); // Reduced scale for better alignment
            newVehicle.speedVector = sf::Vector2f(0.f, 0.f); // Will be set based on lane
            newVehicle.currentSpeed = newVehicle.maxSpeed;
            newVehicle.numberPlate = "ABC-" + std::to_string(rand() % 9999);
            newVehicle.laneName = selectedLane;

            // Set initial position based on lane
            if (selectedLane.find("North") != std::string::npos) {
                newVehicle.sprite.setPosition(400.f, 0.f);
                newVehicle.speedVector = sf::Vector2f(0.f, 1.f);
            }
            else if (selectedLane.find("South") != std::string::npos) {
                newVehicle.sprite.setPosition(400.f, 600.f);
                newVehicle.speedVector = sf::Vector2f(0.f, -1.f);
            }
            else if (selectedLane.find("East") != std::string::npos) {
                newVehicle.sprite.setPosition(800.f, 300.f);
                newVehicle.speedVector = sf::Vector2f(-1.f, 0.f);
            }
            else if (selectedLane.find("West") != std::string::npos) {
                newVehicle.sprite.setPosition(0.f, 300.f);
                newVehicle.speedVector = sf::Vector2f(1.f, 0.f);
            }

            // Priority Handling: emergency front, else back
            if (newVehicle.type == EMERGENCY) {
                laneQueues[selectedLane].vehicles.push_front(newVehicle);
            } else {
                laneQueues[selectedLane].vehicles.push_back(newVehicle);
            }

            safePrint("[SpawnVehicles] Spawned vehicle: " + newVehicle.numberPlate +
                      " Type: " + (newVehicle.type == LIGHT ? "Light" :
                                   newVehicle.type == HEAVY ? "Heavy" : "Emergency") +
                      " Speed:" + std::to_string(newVehicle.currentSpeed) +
                      " Lane:" + newVehicle.laneName);
        }

        // Release LANE_SEM after processing
        releaseResource(SPAWN_VEHICLES, LANE_SEM);
    }
}

// Speed Manager Thread
void speedManagerThread() {
    // Implement speed management by monitoring vehicle speeds
    while (running) {
        std::this_thread::sleep_for(std::chrono::milliseconds(500)); // Adjust frequency as needed

        // Acquire ACTIVE_VEHICLES_SEM
        if (!acquireResource(SPEED_MANAGER, ACTIVE_VEHICLES_SEM)) {
            safePrint("[Banker] SpeedManager: Waiting for ACTIVE_VEHICLES_SEM resource.");
            continue;
        }

        // Check speeds of all active vehicles
        for (auto &vehicle : activeVehicles) {
            if (vehicle.currentSpeed > vehicle.maxSpeed) {
                // Detected speed violation
                SpeedViolationMsg violationMsg;
                std::strncpy(violationMsg.vehicleID, vehicle.numberPlate.c_str(), sizeof(violationMsg.vehicleID) - 1);
                violationMsg.vehicleID[sizeof(violationMsg.vehicleID) - 1] = '\0';
                violationMsg.vehicleType = vehicle.type;
                violationMsg.speed = vehicle.currentSpeed;

                if (mq_send(mqSmartToChallan, reinterpret_cast<const char*>(&violationMsg), sizeof(violationMsg), 0) == -1) {
                    std::cerr << "[SpeedManager] Failed to send speed violation message: " << strerror(errno) << std::endl;
                } else {
                    safePrint("[SpeedManager] Speed violation detected for Vehicle " + vehicle.numberPlate +
                              " Speed: " + std::to_string(vehicle.currentSpeed));
                    totalChallansIssued++;
                }
            }
        }

        // Release ACTIVE_VEHICLES_SEM
        releaseResource(SPEED_MANAGER, ACTIVE_VEHICLES_SEM);
    }
}

// Out-of-Order Thread
void outOfOrderThread() {
    // Implement out-of-order vehicle handling
    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<> vehicleDist(0, 100); // Probability distribution

    while (running) {
        std::this_thread::sleep_for(std::chrono::seconds(30)); // Adjust frequency as needed

        // Randomly decide if a vehicle goes out of order
        int chance = vehicleDist(gen);
        if (chance < 10) { // 10% chance every 30 seconds
            // Acquire ACTIVE_VEHICLES_SEM
            if (!acquireResource(OUT_OF_ORDER, ACTIVE_VEHICLES_SEM)) {
                safePrint("[Banker] OutOfOrder: Waiting for ACTIVE_VEHICLES_SEM resource.");
                continue;
            }

            if (!activeVehicles.empty()) {
                // Select a random vehicle to go out of order
                std::uniform_int_distribution<> selectDist(0, static_cast<int>(activeVehicles.size()) - 1);
                int index = selectDist(gen);
                Vehicle &vehicle = activeVehicles[index];
                vehicle.outOfOrder = true;
                totalVehiclesOutOfOrder++;

                safePrint("[OutOfOrder] Vehicle " + vehicle.numberPlate + " has gone out of order.");

                // Release ACTIVE_VEHICLES_SEM
                releaseResource(OUT_OF_ORDER, ACTIVE_VEHICLES_SEM);

                // Summon tow truck by spawning an emergency vehicle
                std::vector<std::string> lanes = {"North1", "North2", "South1", "South2", "East1", "East2", "West1", "West2"};
                std::uniform_int_distribution<> laneDist(0, static_cast<int>(lanes.size()) - 1);
                std::string selectedLane = lanes[laneDist(gen)];

                // Acquire LANE_SEM to modify lane queues
                if (!acquireResource(OUT_OF_ORDER, LANE_SEM)) {
                    safePrint("[Banker] OutOfOrder: Waiting for LANE_SEM resource to summon tow truck.");
                    continue;
                }

                // Create tow truck vehicle
                Vehicle towTruck;
                towTruck.type = EMERGENCY;
                towTruck.maxSpeed = 90.0f; // Faster than regular emergency vehicles
                towTruck.sprite.setTexture(towTruckTexture); // Set texture properly
                towTruck.sprite.setScale(0.05f, 0.05f); // Adjust scale as needed
                towTruck.speedVector = sf::Vector2f(0.f, 1.f); // Moving south
                towTruck.currentSpeed = towTruck.maxSpeed;
                towTruck.numberPlate = "TOW-" + std::to_string(rand() % 9999);
                towTruck.laneName = selectedLane;

                // Set initial position based on lane
                if (selectedLane.find("North") != std::string::npos) {
                    towTruck.sprite.setPosition(400.f, 0.f);
                    towTruck.speedVector = sf::Vector2f(0.f, 1.f);
                }
                else if (selectedLane.find("South") != std::string::npos) {
                    towTruck.sprite.setPosition(400.f, 600.f);
                    towTruck.speedVector = sf::Vector2f(0.f, -1.f);
                }
                else if (selectedLane.find("East") != std::string::npos) {
                    towTruck.sprite.setPosition(800.f, 300.f);
                    towTruck.speedVector = sf::Vector2f(-1.f, 0.f);
                }
                else if (selectedLane.find("West") != std::string::npos) {
                    towTruck.sprite.setPosition(0.f, 300.f);
                    towTruck.speedVector = sf::Vector2f(1.f, 0.f);
                }

                // Push tow truck to the front of the lane queue
                laneQueues[selectedLane].vehicles.push_front(towTruck);
                activeVehicles.push_back(towTruck);

                safePrint("[OutOfOrder] Tow Truck " + towTruck.numberPlate + " summoned to lane " + selectedLane + ".");
                
                // Release LANE_SEM after modification
                releaseResource(OUT_OF_ORDER, LANE_SEM);
            } else {
                // Release ACTIVE_VEHICLES_SEM if no vehicles are active
                releaseResource(OUT_OF_ORDER, ACTIVE_VEHICLES_SEM);
            }
        }
    }
}

// Mock Time Thread
void mockTimeThread() {
    while (running) {
        std::this_thread::sleep_for(std::chrono::seconds(60)); // Increment time every minute
        mockTime.incrementTime(1); // Increment by 1 minute
        safePrint("[MockTime] Time Updated: " + std::to_string(mockTime.hour) + ":" +
                  (mockTime.minute < 10 ? "0" : "") + std::to_string(mockTime.minute));
    }
}

// Challan Generator Process
void challanGeneratorProcess() {
    // Open the message queue to receive speed violations
    mqd_t mqSmartToChallanLocal = mq_open(MQ_SMART_TO_CHALLAN, O_RDONLY);
    if (mqSmartToChallanLocal == (mqd_t)-1) {
        std::cerr << "[ChallanGenerator] Failed to open MQ_SMART_TO_CHALLAN: " << strerror(errno) << std::endl;
        exit(1);
    }

    // Open the message queue to send challan updates
    mqd_t mqChallanToSmartLocal = mq_open(MQ_CHALLAN_TO_SMART, O_WRONLY);
    if (mqChallanToSmartLocal == (mqd_t)-1) {
        std::cerr << "[ChallanGenerator] Failed to open MQ_CHALLAN_TO_SMART: " << strerror(errno) << std::endl;
        mq_close(mqSmartToChallanLocal);
        exit(1);
    }

    while (running) {
        char buffer[sizeof(SpeedViolationMsg)];
        ssize_t bytesRead = mq_receive(mqSmartToChallanLocal, buffer, sizeof(SpeedViolationMsg), NULL);
        if (bytesRead >= 0) {
            SpeedViolationMsg *msg = reinterpret_cast<SpeedViolationMsg*>(buffer);
            std::string vehicleID(msg->vehicleID);
            bool alreadyChallaned = false;

            // Check if vehicle already has an active challan
            if (activeChallans.find(vehicleID) != activeChallans.end()) {
                if (activeChallans[vehicleID]) {
                    alreadyChallaned = true;
                }
            }

            if (!alreadyChallaned) {
                // Create a challan update message
                ChallanUpdateMsg challanMsg;
                std::strncpy(challanMsg.vehicleID, vehicleID.c_str(), sizeof(challanMsg.vehicleID) - 1);
                challanMsg.vehicleID[sizeof(challanMsg.vehicleID) - 1] = '\0';
                challanMsg.paid = false;

                // Send challan to SmartTraffix
                if (mq_send(mqChallanToSmartLocal, reinterpret_cast<const char*>(&challanMsg), sizeof(challanMsg), 0) == -1) {
                    std::cerr << "[ChallanGenerator] Failed to send challan update: " << strerror(errno) << std::endl;
                } else {
                    safePrint("[ChallanGenerator] Issued challan to Vehicle " + vehicleID);
                    activeChallans[vehicleID] = true;
                }
            } else {
                safePrint("[ChallanGenerator] Vehicle " + vehicleID + " already has an active challan.");
            }
        } else {
            if (errno != EAGAIN && errno != EWOULDBLOCK) {
                std::cerr << "[ChallanGenerator] Failed to receive message: " << strerror(errno) << std::endl;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
        }
    }

    // Close message queues
    mq_close(mqSmartToChallanLocal);
    mq_close(mqChallanToSmartLocal);
}

// Stripe Payment Process
void stripePaymentProcess() {
    // Open the message queue to receive payment messages
    mqd_t mqStripeToChallanLocal = mq_open(MQ_STRIPE_TO_CHALLAN, O_RDONLY | O_NONBLOCK);
    if (mqStripeToChallanLocal == (mqd_t)-1) {
        std::cerr << "[StripePayment] Failed to open MQ_STRIPE_TO_CHALLAN: " << strerror(errno) << std::endl;
        exit(1);
    }

    // Open the message queue to send challan updates
    mqd_t mqChallanToSmartLocal = mq_open(MQ_CHALLAN_TO_SMART, O_WRONLY);
    if (mqChallanToSmartLocal == (mqd_t)-1) {
        std::cerr << "[StripePayment] Failed to open MQ_CHALLAN_TO_SMART: " << strerror(errno) << std::endl;
        mq_close(mqStripeToChallanLocal);
        exit(1);
    }

    while (running) {
        char buffer[sizeof(PaymentMsg)];
        ssize_t bytesRead = mq_receive(mqStripeToChallanLocal, buffer, sizeof(PaymentMsg), NULL);
        if (bytesRead >= 0) {
            PaymentMsg *msg = reinterpret_cast<PaymentMsg*>(buffer);
            std::string vehicleID(msg->vehicleID);
            bool paid = msg->paid;

            // Update challan status
            ChallanUpdateMsg challanMsg;
            std::strncpy(challanMsg.vehicleID, vehicleID.c_str(), sizeof(challanMsg.vehicleID) - 1);
            challanMsg.vehicleID[sizeof(challanMsg.vehicleID) - 1] = '\0';
            challanMsg.paid = paid;

            // Send challan update to SmartTraffix
            if (mq_send(mqChallanToSmartLocal, reinterpret_cast<const char*>(&challanMsg), sizeof(challanMsg), 0) == -1) {
                std::cerr << "[StripePayment] Failed to send challan update: " << strerror(errno) << std::endl;
            } else {
                if (paid) {
                    safePrint("[StripePayment] Vehicle " + vehicleID + " has paid the challan.");
                    totalChallansPaid++;
                } else {
                    safePrint("[StripePayment] Vehicle " + vehicleID + " challan payment failed.");
                }
            }
        } else {
            if (errno != EAGAIN && errno != EWOULDBLOCK) {
                std::cerr << "[StripePayment] Failed to receive message: " << strerror(errno) << std::endl;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
        }
    }

    // Close message queues
    mq_close(mqStripeToChallanLocal);
    mq_close(mqChallanToSmartLocal);
}

// User Portal Process
void userPortalProcess() {
    // Open the portal status message queue for writing
    mqd_t mqPortalStatus = mq_open(MQ_PORTAL_STATUS, O_WRONLY);
    if (mqPortalStatus == (mqd_t)-1) {
        std::cerr << "UserPortal: Failed to open portal status message queue." << std::endl;
        exit(1);
    }

    // Open the message queue to receive challan updates
    mqd_t mqChallanToSmartLocal = mq_open(MQ_CHALLAN_TO_SMART, O_RDONLY | O_NONBLOCK);
    if (mqChallanToSmartLocal == (mqd_t)-1) {
        std::cerr << "UserPortal: Failed to open MQ_CHALLAN_TO_SMART." << std::endl;
        mq_close(mqPortalStatus);
        exit(1);
    }

    // Open the message queue to send payment messages
    mqd_t mqStripeToChallanLocal = mq_open(MQ_STRIPE_TO_CHALLAN, O_WRONLY);
    if (mqStripeToChallanLocal == (mqd_t)-1) {
        std::cerr << "UserPortal: Failed to open MQ_STRIPE_TO_CHALLAN." << std::endl;
        mq_close(mqPortalStatus);
        mq_close(mqChallanToSmartLocal);
        exit(1);
    }

    // Send 'active' status before opening the portal
    PortalStatusMsg activeMsg;
    std::strncpy(activeMsg.status, "active", sizeof(activeMsg.status) - 1);
    activeMsg.status[sizeof(activeMsg.status) - 1] = '\0';
    if (mq_send(mqPortalStatus, reinterpret_cast<const char*>(&activeMsg), sizeof(activeMsg), 0) == -1) {
        std::cerr << "UserPortal: Failed to send 'active' status." << std::endl;
    }

    // User interaction loop
    while (running) {
        std::cout << "\n--- User Portal ---\n";
        std::cout << "1. View Challans\n2. Pay Challan\n3. Exit\nEnter choice: ";
        int choice;
        std::cin >> choice;

        if (!running) break;

        if (choice == 1) {
            std::cout << "\n--- Active Challans ---\n";
            // Acquire ACTIVE_VEHICLES_SEM to access challans
            if (!acquireResource(USER_PORTAL, ACTIVE_VEHICLES_SEM)) {
                safePrint("[Banker] UserPortal: Waiting for ACTIVE_VEHICLES_SEM resource.");
                continue;
            }

            bool hasChallans = false;
            for (const auto &entry : activeChallans) {
                if (entry.second) { // If challan is active
                    std::cout << "Vehicle ID: " << entry.first << " | Paid: No\n";
                    hasChallans = true;
                }
            }

            if (!hasChallans) {
                std::cout << "No active challans.\n";
            }

            // Release ACTIVE_VEHICLES_SEM
            releaseResource(USER_PORTAL, ACTIVE_VEHICLES_SEM);
        }
        else if (choice == 2) {
            std::cout << "Enter Vehicle ID to pay challan: ";
            std::string vid;
            std::cin >> vid;

            // Check if challan exists
            bool challanExists = false;
            {
                std::lock_guard<std::mutex> lock(printMutex); // Protect access
                if (activeChallans.find(vid) != activeChallans.end() && activeChallans[vid]) {
                    challanExists = true;
                }
            }

            if (challanExists) {
                // Create payment message
                PaymentMsg paymentMsg;
                std::strncpy(paymentMsg.vehicleID, vid.c_str(), sizeof(paymentMsg.vehicleID) - 1);
                paymentMsg.vehicleID[sizeof(paymentMsg.vehicleID) - 1] = '\0';
                paymentMsg.paid = true;

                // Send payment message to StripePayment
                if (mq_send(mqStripeToChallanLocal, reinterpret_cast<const char*>(&paymentMsg), sizeof(paymentMsg), 0) == -1) {
                    std::cerr << "UserPortal: Failed to send payment message." << std::endl;
                } else {
                    std::cout << "Challan for Vehicle ID " << vid << " has been submitted for payment.\n";
                }
            } else {
                std::cout << "No active challan found for Vehicle ID " << vid << ".\n";
            }
        }
        else if (choice == 3) {
            std::cout << "Exiting User Portal.\n";
            break;
        }
        else {
            std::cout << "Invalid choice. Try again.\n";
        }

        // Process any incoming challan updates
        char buffer[sizeof(ChallanUpdateMsg)];
        ssize_t bytesRead = mq_receive(mqChallanToSmartLocal, buffer, sizeof(ChallanUpdateMsg), NULL);
        if (bytesRead >= 0) {
            ChallanUpdateMsg *msg = reinterpret_cast<ChallanUpdateMsg*>(buffer);
            std::string vehicleID(msg->vehicleID);
            bool paid = msg->paid;

            if (paid) {
                // Update challan status
                std::lock_guard<std::mutex> lock(printMutex);
                activeChallans[vehicleID] = false;
                totalChallansPaid++;
                std::cout << "[UserPortal] Challan for Vehicle " << vehicleID << " has been paid.\n";
            } else {
                // Handle failed payment or other statuses if needed
                std::cout << "[UserPortal] Challan update for Vehicle " << vehicleID << " received.\n";
            }
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }

    // Send 'inactive' status after closing the portal
    PortalStatusMsg inactiveMsg;
    std::strncpy(inactiveMsg.status, "inactive", sizeof(inactiveMsg.status) - 1);
    inactiveMsg.status[sizeof(inactiveMsg.status) - 1] = '\0';
    if (mq_send(mqPortalStatus, reinterpret_cast<const char*>(&inactiveMsg), sizeof(inactiveMsg), 0) == -1) {
        std::cerr << "UserPortal: Failed to send 'inactive' status." << std::endl;
    }

    // Close message queues
    mq_close(mqPortalStatus);
    mq_close(mqChallanToSmartLocal);
    mq_close(mqStripeToChallanLocal);
}

// Handle Collisions and Cleanup
void handleCollisionsAndCleanup() {
    // Acquire ACTIVE_VEHICLES_SEM to access activeVehicles
    if (!acquireResource(TRAFFIC_LIGHT_CONTROLLER, ACTIVE_VEHICLES_SEM)) {
        safePrint("[Banker] CollisionHandler: Waiting for ACTIVE_VEHICLES_SEM resource.");
        return;
    }

    // Check for collisions between vehicles
    for (size_t i = 0; i < activeVehicles.size(); ++i) {
        for (size_t j = i + 1; j < activeVehicles.size(); ++j) {
            if (activeVehicles[i].isTowed || activeVehicles[j].isTowed)
                continue; // Skip if either vehicle is being towed

            if (activeVehicles[i].sprite.getGlobalBounds().intersects(activeVehicles[j].sprite.getGlobalBounds())) {
                // Collision detected
                std::string vehicleA = activeVehicles[i].numberPlate;
                std::string vehicleB = activeVehicles[j].numberPlate;
                safePrint("[CollisionHandler] Collision detected between Vehicle " + vehicleA + " and Vehicle " + vehicleB + ".");

                // Remove both vehicles from activeVehicles
                activeVehicles[i].isTowed = true;
                activeVehicles[j].isTowed = true;

                // Increment analytics counter
                // For simplicity, we can consider them out of order
                totalVehiclesOutOfOrder += 2;
            }
        }
    }

    // Remove towed vehicles from activeVehicles
    activeVehicles.erase(
        std::remove_if(activeVehicles.begin(), activeVehicles.end(),
                       [](const Vehicle &v) { return v.isTowed; }),
        activeVehicles.end());

    // Release ACTIVE_VEHICLES_SEM
    releaseResource(TRAFFIC_LIGHT_CONTROLLER, ACTIVE_VEHICLES_SEM);
}

// Process Queues
void processQueues() {
    // Iterate through each lane and move vehicles to activeVehicles based on traffic light state
    for (auto &entry : laneQueues) {
        std::string lane = entry.first;
        LaneQueue &queue = entry.second;

        // Determine direction based on lane name
        std::string direction;
        if (lane.find("North") != std::string::npos) {
            direction = "North";
        }
        else if (lane.find("South") != std::string::npos) {
            direction = "South";
        }
        else if (lane.find("East") != std::string::npos) {
            direction = "East";
        }
        else if (lane.find("West") != std::string::npos) {
            direction = "West";
        }

        // Check if traffic light for this direction is GREEN
        if (trafficLights[direction].state == GREEN) {
            // Move vehicle from queue to activeVehicles
            if (!queue.vehicles.empty()) {
                // Acquire ACTIVE_VEHICLES_SEM to modify activeVehicles
                if (!acquireResource(TRAFFIC_LIGHT_CONTROLLER, ACTIVE_VEHICLES_SEM)) {
                    safePrint("[Banker] processQueues: Waiting for ACTIVE_VEHICLES_SEM resource.");
                    continue;
                }

                Vehicle vehicle = queue.vehicles.front();
                queue.vehicles.pop_front();
                activeVehicles.push_back(vehicle);

                safePrint("[processQueues] Vehicle " + vehicle.numberPlate + " entered traffic from lane " + lane + ".");

                // Release ACTIVE_VEHICLES_SEM after modification
                releaseResource(TRAFFIC_LIGHT_CONTROLLER, ACTIVE_VEHICLES_SEM);
            }
        }
    }
}

// Deadlock Prevention Loop
void runDeadlockPrevention() {
    // Implement deadlock prevention using Banker's algorithm
    // In this implementation, resource requests and releases are handled within each thread/process
    // Therefore, this function can remain empty or perform periodic checks
    while (running) {
        // For demonstration, just sleep
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }
}

// Visualization Function
void visualizeTraffic(sf::RenderWindow &window, sf::Sprite &roadSprite, sf::Font &font, sf::Text &analyticsText) {
    window.clear();
    window.draw(roadSprite);

    // Draw Traffic Lights
    for (const auto &entry : trafficLights) {
        window.draw(entry.second.lightShape);
    }

    // Move and Draw Vehicles
    // Acquire ACTIVE_VEHICLES_SEM to access activeVehicles
    if (!acquireResource(TRAFFIC_LIGHT_CONTROLLER, ACTIVE_VEHICLES_SEM)) {
        safePrint("[Banker] visualizeTraffic: Waiting for ACTIVE_VEHICLES_SEM resource.");
        return;
    }

    for (auto &v : activeVehicles) {
        if (v.outOfOrder || v.isTowed)
            continue; // Skip out-of-order or towed vehicles

        // Update vehicle position based on speed vector
        v.sprite.move(v.speedVector.x * v.currentSpeed * 0.01f, v.speedVector.y * v.currentSpeed * 0.01f);

        // Check if vehicle has exited the window bounds
        if (v.sprite.getPosition().x < -50.f || v.sprite.getPosition().x > 850.f ||
            v.sprite.getPosition().y < -50.f || v.sprite.getPosition().y > 650.f) {
            // Remove vehicle from activeVehicles
            safePrint("[visualizeTraffic] Vehicle " + v.numberPlate + " has exited the simulation.");
            // Find and remove vehicle from laneQueues if necessary
            for (auto &laneEntry : laneQueues) {
                auto &laneQueue = laneEntry.second.vehicles;
                laneQueue.erase(std::remove_if(laneQueue.begin(), laneQueue.end(),
                    [&](const Vehicle &laneV) { return laneV.numberPlate == v.numberPlate; }),
                    laneQueue.end());
            }
            v.isTowed = true; // Mark for removal
            continue;
        }

        window.draw(v.sprite);
    }

    // Remove towed vehicles from activeVehicles
    activeVehicles.erase(
        std::remove_if(activeVehicles.begin(), activeVehicles.end(),
                       [](const Vehicle &v) { return v.isTowed; }),
        activeVehicles.end());

    // Release ACTIVE_VEHICLES_SEM
    releaseResource(TRAFFIC_LIGHT_CONTROLLER, ACTIVE_VEHICLES_SEM);

    // Update Analytics
    analyticsText.setString(
        "Active Vehicles: " + std::to_string(activeVehicles.size()) + "\n" +
        "Total Challans Issued: " + std::to_string(totalChallansIssued) + "\n" +
        "Total Challans Paid: " + std::to_string(totalChallansPaid) + "\n" +
        "Vehicles Out of Order: " + std::to_string(totalVehiclesOutOfOrder)
    );

    window.draw(analyticsText);
    window.display();

    // Handle collisions and cleanup after drawing
    handleCollisionsAndCleanup();
}

// Cleanup and Exit Function
void performCleanup(pid_t pid1, pid_t pid2, pid_t pid3, pid_t pid4) {
    running = false;

    // Terminate child processes
    if (pid1 > 0) kill(pid1, SIGTERM);
    if (pid2 > 0) kill(pid2, SIGTERM);
    if (pid3 > 0) kill(pid3, SIGTERM);
    if (pid4 > 0) kill(pid4, SIGTERM);

    // Wait for child processes to terminate
    if (pid1 > 0) waitpid(pid1, NULL, 0);
    if (pid2 > 0) waitpid(pid2, NULL, 0);
    if (pid3 > 0) waitpid(pid3, NULL, 0);
    if (pid4 > 0) waitpid(pid4, NULL, 0);

    // Close and unlink semaphores
    if (laneSem != SEM_FAILED) {
        sem_close(laneSem);
        sem_unlink("/laneSem");
    }
    if (activeVehiclesSem != SEM_FAILED) {
        sem_close(activeVehiclesSem);
        sem_unlink("/activeVehiclesSem");
    }

    // Close and unlink message queues
    if (mqSmartToChallan != (mqd_t)-1) {
        mq_close(mqSmartToChallan);
        mq_unlink(MQ_SMART_TO_CHALLAN);
    }

    if (mqStripeToChallan != (mqd_t)-1) {
        mq_close(mqStripeToChallan);
        mq_unlink(MQ_STRIPE_TO_CHALLAN);
    }

    if (mqChallanToSmart != (mqd_t)-1) {
        mq_close(mqChallanToSmart);
        mq_unlink(MQ_CHALLAN_TO_SMART);
    }

    if (mqPortalStatusHandle != (mqd_t)-1) {
        mq_close(mqPortalStatusHandle);
        mq_unlink(MQ_PORTAL_STATUS);
    }

    std::cout << "Cleanup complete. Exiting.\n";
    exit(0);
}

// Signal Handler
void cleanupAndExit(int signum) {
    std::cout << "\nInterrupt signal (" << signum << ") received.\n";
    // Assuming pid1, pid2, pid3, pid4 are accessible
    performCleanup(-1, -1, -1, -1); // Placeholder; adjust as needed
}

// Main Function
int main() {
    // Register signal handler
    signal(SIGINT, cleanupAndExit);

    // Initialize Banker's Algorithm
    initializeBankers();

    // Create and initialize semaphores
    laneSem = sem_open("/laneSem", O_CREAT | O_EXCL, 0644, 1);
    activeVehiclesSem = sem_open("/activeVehiclesSem", O_CREAT | O_EXCL, 0644, 1);
    if (laneSem == SEM_FAILED || activeVehiclesSem == SEM_FAILED) {
        std::cerr << "Failed to create semaphores: " << strerror(errno) << std::endl;
        if (laneSem != SEM_FAILED) sem_close(laneSem);
        if (activeVehiclesSem != SEM_FAILED) sem_close(activeVehiclesSem);
        sem_unlink("/laneSem");
        sem_unlink("/activeVehiclesSem");
        return EXIT_FAILURE;
    }

    // Initialize lane queues
    std::vector<std::string> lanes = {"North1", "North2", "South1", "South2", "East1", "East2", "West1", "West2"};
    for (const auto &lane : lanes) {
        laneQueues[lane] = LaneQueue();
    }

    // Initialize Traffic Lights
    initializeTrafficLights();

    // Create message queues
    struct mq_attr mqAttr;
    mqAttr.mq_flags = O_NONBLOCK;
    mqAttr.mq_maxmsg = 10;
    mqAttr.mq_msgsize = MQ_MAX_SIZE;
    mqAttr.mq_curmsgs = 0;

    // Unlink in case they already exist
    mq_unlink(MQ_SMART_TO_CHALLAN);
    mq_unlink(MQ_STRIPE_TO_CHALLAN);
    mq_unlink(MQ_CHALLAN_TO_SMART);
    mq_unlink(MQ_PORTAL_STATUS);

    // Open message queues
    mqSmartToChallan = mq_open(MQ_SMART_TO_CHALLAN, O_CREAT | O_WRONLY, 0644, &mqAttr);
    mqStripeToChallan = mq_open(MQ_STRIPE_TO_CHALLAN, O_CREAT | O_WRONLY, 0644, &mqAttr);
    mqChallanToSmart = mq_open(MQ_CHALLAN_TO_SMART, O_CREAT | O_RDONLY | O_NONBLOCK, 0644, &mqAttr);
    // Portal Status message queue is opened as O_RDONLY | O_NONBLOCK in main
    // Not opened yet; will be opened in portalStatusListener

    if (mqSmartToChallan == (mqd_t)-1 || mqStripeToChallan == (mqd_t)-1 || mqChallanToSmart == (mqd_t)-1) {
        std::cerr << "Failed to create message queues: " << strerror(errno) << std::endl;
        performCleanup(-1, -1, -1, -1);
    }

    // Load textures
    if (!roadTexture.loadFromFile("road.jpg") ||
        !carTexture1.loadFromFile("car1.png") ||
        !carTexture2.loadFromFile("car2.png") ||
        !towTruckTexture.loadFromFile("vehicle.png")) { // Changed to 'vehicle.png'
        std::cerr << "Failed to load textures!" << std::endl;
        performCleanup(-1, -1, -1, -1);
    }

    // Load font
    sf::Font font;
    if (!font.loadFromFile("DejaVuSans.ttf")) { // Ensure DejaVuSans.ttf is present
        std::cerr << "Failed to load font 'DejaVuSans.ttf'!" << std::endl;
        performCleanup(-1, -1, -1, -1);
    }

    sf::Text analyticsText;
    analyticsText.setFont(font);
    analyticsText.setCharacterSize(14);
    analyticsText.setFillColor(sf::Color::White);
    analyticsText.setPosition(10.f, 10.f);

    // Fork child processes for ChallanGenerator, StripePayment, UserPortal
    pid_t pidChallanGenerator = fork();
    if (pidChallanGenerator == 0) {
        // Child process: Challan Generator
        challanGeneratorProcess();
        exit(0);
    }
    else if (pidChallanGenerator < 0) {
        std::cerr << "Failed to fork ChallanGenerator: " << strerror(errno) << std::endl;
        performCleanup(-1, -1, -1, -1);
    }

    pid_t pidStripePayment = fork();
    if (pidStripePayment == 0) {
        // Child process: StripePayment
        stripePaymentProcess();
        exit(0);
    }
    else if (pidStripePayment < 0) {
        std::cerr << "Failed to fork StripePayment: " << strerror(errno) << std::endl;
        performCleanup(pidChallanGenerator, -1, -1, -1);
    }

    pid_t pidUserPortal = fork();
    if (pidUserPortal == 0) {
        // Child process: User Portal
        userPortalProcess();
        exit(0);
    }
    else if (pidUserPortal < 0) {
        std::cerr << "Failed to fork UserPortal: " << strerror(errno) << std::endl;
        performCleanup(pidChallanGenerator, pidStripePayment, -1, -1);
    }

    // Initialize and open the portal status message queue for reading
    mqd_t mqPortalStatus = mq_open(MQ_PORTAL_STATUS, O_CREAT | O_RDONLY | O_NONBLOCK, 0644, &mqAttr);
    if (mqPortalStatus == (mqd_t)-1) {
        std::cerr << "Failed to create/open portal status message queue in main." << std::endl;
        performCleanup(pidChallanGenerator, pidStripePayment, pidUserPortal, -1);
    }

    // Start the portal status listener thread
    std::thread portalStatusListener([&]() {
        char buffer[sizeof(PortalStatusMsg)];
        while (running) {
            ssize_t bytesRead = mq_receive(mqPortalStatus, buffer, sizeof(PortalStatusMsg), NULL);
            if (bytesRead > 0) {
                PortalStatusMsg* msg = reinterpret_cast<PortalStatusMsg*>(buffer);
                std::string status(msg->status);

                if (status == "active") {
                    portalActive.store(true);
                    std::cout << "[INFO] User Portal is now ACTIVE. Suppressing main simulation output." << std::endl;
                }
                else if (status == "inactive") {
                    portalActive.store(false);
                    std::cout << "[INFO] User Portal is now INACTIVE. Resuming main simulation output." << std::endl;
                }
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
        }
    });

    // Start simulation threads
    pthread_t tLight, tSpawn, tSpeed, tOutOfOrder, tMockTime;

    // Traffic Light Controller
    {
        int rc = pthread_create(&tLight, nullptr, [](void*)->void* {
            trafficLightControllerThread();
            return nullptr;
        }, nullptr);
        if (rc != 0) {
            std::cerr << "Failed to create tLight thread: " << strerror(rc) << std::endl;
            performCleanup(pidChallanGenerator, pidStripePayment, pidUserPortal, -1);
        }
    }

    // Spawn Vehicles
    {
        sf::Texture* textures[3] = { &carTexture1, &carTexture2, &towTruckTexture };
        int rc = pthread_create(&tSpawn, nullptr, [](void* arg)->void* {
            sf::Texture** tex = reinterpret_cast<sf::Texture**>(arg);
            spawnVehiclesThread(tex[0], tex[1], tex[2]);
            return nullptr;
        }, textures);
        if (rc != 0) {
            std::cerr << "Failed to create tSpawn thread: " << strerror(rc) << std::endl;
            performCleanup(pidChallanGenerator, pidStripePayment, pidUserPortal, -1);
        }
    }

    // Speed Manager
    {
        int rc = pthread_create(&tSpeed, nullptr, [](void*)->void* {
            speedManagerThread();
            return nullptr;
        }, nullptr);
        if (rc != 0) {
            std::cerr << "Failed to create tSpeed thread: " << strerror(rc) << std::endl;
            performCleanup(pidChallanGenerator, pidStripePayment, pidUserPortal, -1);
        }
    }

    // Out-of-Order
    {
        int rc = pthread_create(&tOutOfOrder, nullptr, [](void*)->void* {
            outOfOrderThread();
            return nullptr;
        }, nullptr);
        if (rc != 0) {
            std::cerr << "Failed to create tOutOfOrder thread: " << strerror(rc) << std::endl;
            performCleanup(pidChallanGenerator, pidStripePayment, pidUserPortal, -1);
        }
    }

    // Mock Time
    {
        int rc = pthread_create(&tMockTime, nullptr, [](void*)->void* {
            mockTimeThread();
            return nullptr;
        }, nullptr);
        if (rc != 0) {
            std::cerr << "Failed to create tMockTime thread: " << strerror(rc) << std::endl;
            performCleanup(pidChallanGenerator, pidStripePayment, pidUserPortal, -1);
        }
    }

    // Start a listener thread for challan updates (optional)
    std::thread challanListenerThread([&]() {
        // This thread can be used for additional processing if needed
        // Currently, challan updates are handled in userPortalProcess and other processes
        // Keeping it empty for now
        while (running) {
            std::this_thread::sleep_for(std::chrono::seconds(1));
        }
    });

    // Create SFML window
    sf::RenderWindow window(sf::VideoMode(800, 600), "SmartTraffix Simulation");
    sf::Sprite roadSprite(roadTexture);
    roadSprite.setScale(1.0f, 1.0f);

    // Main loop
    while (window.isOpen()) {
        sf::Event event;
        while (window.pollEvent(event)) {
            if (event.type == sf::Event::Closed) {
                performCleanup(pidChallanGenerator, pidStripePayment, pidUserPortal, -1);
                window.close();
            }
        }

        processQueues(); 
        runDeadlockPrevention();
        visualizeTraffic(window, roadSprite, font, analyticsText);
    }

    // Cleanup (in case window is closed without signal)
    performCleanup(pidChallanGenerator, pidStripePayment, pidUserPortal, -1);
    return 0;
}

