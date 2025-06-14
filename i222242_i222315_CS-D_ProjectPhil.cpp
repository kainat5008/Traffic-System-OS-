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
#include <thread>      // Added to resolve std::this_thread
#include <cerrno>

// ------------------------------------------------------------
// Enums and Structures
// ------------------------------------------------------------
enum class TrafficLightState { GREEN, YELLOW, RED };
enum VehicleType { LIGHT, HEAVY, EMERGENCY };

struct Vehicle {
    sf::Sprite sprite;
    sf::Vector2f speedVector;
    VehicleType type;
    float maxSpeed;
    std::string numberPlate;
    float currentSpeed;
    bool challanActive = false;
    bool outOfOrder = false;
};

struct LaneQueue {
    // Using deque to allow emergency vehicles to be pushed front
    std::deque<Vehicle> vehicles;
    int maxCapacity = 10;
};

// Mock Date/Time for testing peak hours
struct MockDateTime {
    int hour = 7;
    int minute = 0;

    void incrementTime(int seconds) {
        minute += seconds / 60;
        if (minute >= 60) {
            hour += minute / 60;
            minute %= 60;
        }
        if (hour >= 24) {
            hour %= 24;
        }
    }

    bool isPeakHours() const {
        // Peak hours: 7:00–9:30 AM and 4:30–8:30 PM
        if ((hour > 7 && hour < 9) || (hour == 7 && minute >= 0) || (hour == 9 && minute <= 30) ||
            (hour > 16 && hour < 20) || (hour == 16 && minute >= 30) || (hour == 20 && minute <= 30)) {
            return true;
        }
        return false;
    }
};

// ------------------------------------------------------------
// Global Simulation Variables (Shared within the SmartTraffix process)
// ------------------------------------------------------------
static TrafficLightState currentLightState = TrafficLightState::GREEN;
static MockDateTime mockTime;
static std::map<std::string, LaneQueue> laneQueues;
static std::vector<Vehicle> activeVehicles;

// Positions, directions, and rotations of lanes
static std::map<std::string, sf::Vector2f> lanePositions = {
    {"North1", {380, 0}}, {"North2", {400, 0}}, {"South1", {410, 600}}, {"South2", {430, 600}},
    {"East1", {800, 250}}, {"East2", {800, 290}}, {"West1", {0, 310}}, {"West2", {0, 350}}};

static std::map<std::string, sf::Vector2f> laneDirections = {
    {"North1", {0, 1}}, {"North2", {0, 1}}, {"South1", {0, -1}}, {"South2", {0, -1}},
    {"East1", {-1, 0}}, {"East2", {-1, 0}}, {"West1", {1, 0}}, {"West2", {1, 0}}};

static std::map<std::string, float> laneRotations = {
    {"North1", 180.0f}, {"North2", 180.0f}, {"South1", 0.0f}, {"South2", 0.0f},
    {"East1", -90.0f}, {"East2", -90.0f}, {"West1", 90.0f}, {"West2", 90.0f}};

// ------------------------------------------------------------
// Synchronization Primitives
// ------------------------------------------------------------
sem_t *laneSem = SEM_FAILED;           // Protects laneQueues
sem_t *activeVehiclesSem = SEM_FAILED; // Protects activeVehicles
std::atomic<bool> running(true);

// Random number generation
static std::random_device rd;
static std::mt19937 gen(rd());
float generateRandomSpeed(float maxSpeed) {
    std::uniform_real_distribution<> dis(0.0f, maxSpeed);
    return static_cast<float>(dis(gen));
}

// ------------------------------------------------------------
// IPC Message Queues
// ------------------------------------------------------------
#define MQ_SMART_TO_CHALLAN "/smart_to_challan"
#define MQ_STRIPE_TO_CHALLAN "/stripe_to_challan"
#define MQ_MAX_SIZE 256

struct SpeedViolationMsg {
    char vehicleID[32];
    int vehicleType; // 1=Light,2=Heavy,3=Emergency
    float speed;
};

struct PaymentMsg {
    char vehicleID[32];
    bool paid;
};

// Message Queues handles
mqd_t mqSmartToChallan = (mqd_t)-1;
mqd_t mqStripeToChallan = (mqd_t)-1;

// ------------------------------------------------------------
// Deadlock Prevention: Banker’s Algorithm (Simplified Demo)
// ------------------------------------------------------------
/*
 * For demonstration, assume:
 * - Resource: Intersection "slots" = 2 (like two lanes through intersection).
 * - Each vehicle requires 1 slot to pass.
 * - activeVehicles represent processes holding resources.
 *
 * We'll maintain:
 * - totalResources = 2
 * - allocatedResources = number of active vehicles currently "in intersection"
 */

static int totalResources = 2;
static int allocatedResources = 0;

// Check if system remains safe after adding one vehicle (requesting 1 resource).
bool checkSafeState() {
    // Simplified check: if allocatedResources + 1 <= totalResources, it's safe
    return (allocatedResources + 1) <= totalResources;
}

bool requestResourceForVehicle() {
    // Request 1 resource
    if (checkSafeState()) {
        allocatedResources++;
        return true;
    }
    return false;
}

void releaseResourceForVehicle() {
    if (allocatedResources > 0)
        allocatedResources--;
}

// ------------------------------------------------------------
// Function Prototypes
// ------------------------------------------------------------
void runDeadlockPrevention(); // Forward declaration

// ------------------------------------------------------------
// Vehicle Arrival and Queue Processing Logic with Priority
// ------------------------------------------------------------
void spawnVehiclesThread(sf::Texture *carTexture1, sf::Texture *carTexture2, sf::Texture *carTexture3) {
    std::vector<std::string> lanes = {"North1", "North2", "South1", "South2", "East1", "East2", "West1", "West2"};
    std::uniform_int_distribution<> laneDist(0, static_cast<int>(lanes.size()) - 1);
    std::uniform_int_distribution<> typeDist(1, 3); // 1=Light,2=Heavy,3=Emergency

    while (running) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1000));
        std::string selectedLane = lanes[laneDist(gen)];

        if (sem_wait(laneSem) == -1) { 
            perror("sem_wait laneSem"); 
            continue; 
        }

        if (static_cast<int>(laneQueues[selectedLane].vehicles.size()) < laneQueues[selectedLane].maxCapacity) {
            int vehicleTypeChoice = typeDist(gen);
            // Check peak hours restriction for heavy
            if (vehicleTypeChoice == 2 && mockTime.isPeakHours()) {
                sem_post(laneSem);
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
                newVehicle.maxSpeed = 80.0f;
                newVehicle.sprite.setTexture(*carTexture3);
            }

            newVehicle.sprite.setScale(0.05f, 0.05f); // Reduced scale for better alignment
            newVehicle.sprite.setPosition(lanePositions[selectedLane]);
            newVehicle.sprite.setRotation(laneRotations[selectedLane]);
            newVehicle.speedVector = laneDirections[selectedLane];
            newVehicle.currentSpeed = generateRandomSpeed(newVehicle.maxSpeed);
            newVehicle.numberPlate = "ABC-" + std::to_string(rand() % 9999);

            // Priority Handling: emergency front, else back
            if (newVehicle.type == EMERGENCY) {
                laneQueues[selectedLane].vehicles.push_front(newVehicle);
            } else {
                laneQueues[selectedLane].vehicles.push_back(newVehicle);
            }

            std::cout << "[DEBUG] Spawned vehicle: " << newVehicle.numberPlate 
                      << " Type: " << newVehicle.type 
                      << " Speed:" << newVehicle.currentSpeed << std::endl;
        }

        sem_post(laneSem);
    }
}

void processQueues() {
    if (sem_wait(laneSem) == -1) { 
        perror("sem_wait laneSem"); 
        return; 
    }
    if (sem_wait(activeVehiclesSem) == -1) { 
        perror("sem_wait activeVehiclesSem"); 
        sem_post(laneSem); 
        return; 
    }

    for (auto &lane : laneQueues) {
        while (!lane.second.vehicles.empty() && static_cast<int>(activeVehicles.size()) < 50) {
            // Try to allocate resource using Banker’s algorithm
            if (requestResourceForVehicle()) {
                // Safe to move vehicle into active
                activeVehicles.push_back(lane.second.vehicles.front());
                lane.second.vehicles.pop_front();
            } else {
                // Can't allocate now, potential deadlock risk
                // Break out to wait until conditions improve
                break;
            }
        }
    }

    if (sem_post(activeVehiclesSem) == -1) perror("sem_post activeVehiclesSem");
    if (sem_post(laneSem) == -1) perror("sem_post laneSem");
}

// ------------------------------------------------------------
// Vehicle Management Threads
// ------------------------------------------------------------
void speedManagerThread() {
    while (running) {
        std::this_thread::sleep_for(std::chrono::seconds(5));
        if (sem_wait(activeVehiclesSem) == -1) { 
            perror("sem_wait activeVehiclesSem"); 
            continue; 
        }
        for (auto &v : activeVehicles) {
            v.currentSpeed += 5.0f;
            if (v.currentSpeed > v.maxSpeed && !v.challanActive) {
                // Speed violation
                v.challanActive = true;
                SpeedViolationMsg msg;
                std::strncpy(msg.vehicleID, v.numberPlate.c_str(), 31);
                msg.vehicleID[31] = '\0';
                msg.vehicleType = (v.type == LIGHT) ? 1 : (v.type == HEAVY) ? 2 : 3;
                msg.speed = v.currentSpeed;
                if (mqSmartToChallan != (mqd_t)-1) {
                    if (mq_send(mqSmartToChallan, reinterpret_cast<const char*>(&msg), sizeof(msg), 0) == -1) {
                        std::cerr << "mq_send error: " << strerror(errno) << std::endl;
                    }
                }
            }
        }
        if (sem_post(activeVehiclesSem) == -1) perror("sem_post activeVehiclesSem");
    }
}

void trafficLightControllerThread() {
    while (running) {
        std::this_thread::sleep_for(std::chrono::seconds(10));
        if (currentLightState == TrafficLightState::GREEN) {
            currentLightState = TrafficLightState::YELLOW;
        }
        else if (currentLightState == TrafficLightState::YELLOW) {
            currentLightState = TrafficLightState::RED;
        }
        else {
            currentLightState = TrafficLightState::GREEN;
        }
    }
}

void outOfOrderThread() {
    while (running) {
        std::this_thread::sleep_for(std::chrono::seconds(15));
        if (sem_wait(activeVehiclesSem) == -1) { 
            perror("sem_wait activeVehiclesSem"); 
            continue; 
        }
        if (!activeVehicles.empty()) {
            int idx = rand() % activeVehicles.size();
            activeVehicles[idx].outOfOrder = true;
            std::cout << "[DEBUG] Vehicle " << activeVehicles[idx].numberPlate << " is out of order." << std::endl;
            // Tow truck insertion placeholder if needed
        }
        if (sem_post(activeVehiclesSem) == -1) perror("sem_post activeVehiclesSem");
    }
}

void mockTimeThread() {
    while (running) {
        std::this_thread::sleep_for(std::chrono::seconds(60));
        mockTime.incrementTime(60);
        std::cout << "[DEBUG] Mock Time Updated: " << mockTime.hour << ":" << mockTime.minute << std::endl;
    }
}

// ------------------------------------------------------------
// Collision Detection and Vehicle Removal
// ------------------------------------------------------------
void handleCollisionsAndCleanup() {
    if (sem_wait(activeVehiclesSem) == -1) { 
        perror("sem_wait activeVehiclesSem"); 
        return; 
    }
    // Check collisions
    for (size_t i = 0; i < activeVehicles.size(); ++i) {
        for (size_t j = i + 1; j < activeVehicles.size(); ++j) {
            if (activeVehicles[i].sprite.getGlobalBounds().intersects(activeVehicles[j].sprite.getGlobalBounds())) {
                // Collision detected
                // If neither is emergency, generate challan
                if (activeVehicles[i].type != EMERGENCY && activeVehicles[j].type != EMERGENCY) {
                    std::cout << "[DEBUG] Collision detected between " 
                              << activeVehicles[i].numberPlate << " and " 
                              << activeVehicles[j].numberPlate << std::endl;
                    // Generate challan for both vehicles
                    SpeedViolationMsg msg1, msg2;
                    std::strncpy(msg1.vehicleID, activeVehicles[i].numberPlate.c_str(), 31);
                    msg1.vehicleID[31] = '\0';
                    msg1.vehicleType = (activeVehicles[i].type == LIGHT) ? 1 : 2;
                    msg1.speed = activeVehicles[i].currentSpeed;
                    
                    std::strncpy(msg2.vehicleID, activeVehicles[j].numberPlate.c_str(), 31);
                    msg2.vehicleID[31] = '\0';
                    msg2.vehicleType = (activeVehicles[j].type == LIGHT) ? 1 : 2;
                    msg2.speed = activeVehicles[j].currentSpeed;

                    if (mqSmartToChallan != (mqd_t)-1) {
                        if (mq_send(mqSmartToChallan, reinterpret_cast<const char*>(&msg1), sizeof(msg1), 0) == -1) {
                            std::cerr << "mq_send error (collision msg1): " << strerror(errno) << std::endl;
                        }
                        if (mq_send(mqSmartToChallan, reinterpret_cast<const char*>(&msg2), sizeof(msg2), 0) == -1) {
                            std::cerr << "mq_send error (collision msg2): " << strerror(errno) << std::endl;
                        }
                    }

                    // Mark vehicles to prevent multiple challans
                    activeVehicles[i].challanActive = true;
                    activeVehicles[j].challanActive = true;
                }
            }
        }
    }

    // Remove vehicles that have left the screen
    size_t oldSize = activeVehicles.size();
    activeVehicles.erase(
        std::remove_if(activeVehicles.begin(), activeVehicles.end(), [&](const Vehicle &veh) {
            sf::Vector2f pos = veh.sprite.getPosition();
            bool offScreen = (pos.x < 0 || pos.x > 800 || pos.y < 0 || pos.y > 600);
            if (offScreen) {
                // Vehicle leaving intersection - release resource
                releaseResourceForVehicle();
                std::cout << "[DEBUG] Vehicle " << veh.numberPlate << " has left the intersection." << std::endl;
            }
            return offScreen;
        }),
        activeVehicles.end()
    );
    size_t newSize = activeVehicles.size();
    if (newSize < oldSize) {
        std::cout << "[DEBUG] Removed " << (oldSize - newSize) << " vehicles. Active now: " << activeVehicles.size() << std::endl;
    }
    if (sem_post(activeVehiclesSem) == -1) perror("sem_post activeVehiclesSem");
}

// ------------------------------------------------------------
// Visualization Logic
// ------------------------------------------------------------
void visualizeTraffic(sf::RenderWindow &window, sf::Sprite &roadSprite) {
    window.clear();
    window.draw(roadSprite);

    // Draw Traffic Lights
    sf::RectangleShape northLight(sf::Vector2f(20,20));
    sf::RectangleShape southLight(sf::Vector2f(20,20));
    sf::RectangleShape eastLight(sf::Vector2f(20,20));
    sf::RectangleShape westLight(sf::Vector2f(20,20));

    northLight.setPosition(380, 50);
    southLight.setPosition(410, 500);
    eastLight.setPosition(700, 250);
    westLight.setPosition(100, 310);

    auto setLightColor = [&](sf::RectangleShape &light){
        switch(currentLightState) {
            case TrafficLightState::GREEN: light.setFillColor(sf::Color::Green); break;
            case TrafficLightState::YELLOW: light.setFillColor(sf::Color::Yellow); break;
            case TrafficLightState::RED: light.setFillColor(sf::Color::Red); break;
        }
    };

    setLightColor(northLight);
    setLightColor(southLight);
    setLightColor(eastLight);
    setLightColor(westLight);

    window.draw(northLight);
    window.draw(southLight);
    window.draw(eastLight);
    window.draw(westLight);

    // Move and Draw Vehicles
    if (sem_wait(activeVehiclesSem) == -1) { 
        perror("sem_wait activeVehiclesSem"); 
        return; 
    }
    for (auto &v : activeVehicles) {
        float movementFactor = v.currentSpeed / 100.0f; 
        v.sprite.move(v.speedVector.x * movementFactor, v.speedVector.y * movementFactor);
        window.draw(v.sprite);
    }
    if (sem_post(activeVehiclesSem) == -1) perror("sem_post activeVehiclesSem");

    window.display();

    // Handle collisions and cleanup after drawing
    handleCollisionsAndCleanup();
}

// ------------------------------------------------------------
// Child Processes Logic (ChallanGenerator, UserPortal, StripePayment)
// ------------------------------------------------------------
void challanGeneratorProcess() {
    mqd_t mqStC = mq_open(MQ_SMART_TO_CHALLAN, O_RDONLY);
    mqd_t mqStpC = mq_open(MQ_STRIPE_TO_CHALLAN, O_RDONLY);

    if (mqStC == (mqd_t)-1 || mqStpC == (mqd_t)-1) {
        std::cerr << "ChallanGenerator: Failed to open message queues in child." << std::endl;
        return;
    }

    // For simplicity, store challans in memory
    struct Challan {
        std::string vehicleID;
        float amount;
        bool paid = false;
        time_t issueDate;
        time_t dueDate;
        VehicleType vtype;
    };
    std::map<std::string, Challan> challans;

    char buffer[MQ_MAX_SIZE];
    while (running) {
        // Non-blocking receive
        ssize_t n = mq_receive(mqStC, buffer, MQ_MAX_SIZE, NULL);
        if (n > 0) {
            SpeedViolationMsg *msg = reinterpret_cast<SpeedViolationMsg*>(buffer);
            // Create a challan
            Challan ch;
            ch.vehicleID = msg->vehicleID;
            if (msg->vehicleType == 1) { ch.amount = 5000 + (5000 * 0.17f); ch.vtype = LIGHT; }
            else if (msg->vehicleType == 2) { ch.amount = 7000 + (7000 * 0.17f); ch.vtype = HEAVY; }
            else { 
                // Emergency vehicles exempt
                continue; 
            }
            ch.issueDate = time(NULL);
            ch.dueDate = ch.issueDate + (3 * 24 * 3600); // 3 days after issue
            challans[ch.vehicleID] = ch;
            std::cout << "Challan Issued to Vehicle " << ch.vehicleID << " Amount: " << ch.amount << std::endl;
        }

        // Check payment updates from StripePayment
        n = mq_receive(mqStpC, buffer, MQ_MAX_SIZE, NULL);
        if (n > 0) {
            PaymentMsg *pmsg = reinterpret_cast<PaymentMsg*>(buffer);
            auto it = challans.find(pmsg->vehicleID);
            if (it != challans.end() && pmsg->paid) {
                it->second.paid = true;
                std::cout << "Challan Paid for Vehicle " << pmsg->vehicleID << std::endl;
                // Notify SmartTraffix that challan is cleared if needed.
                // For full integration, another queue could be used or shared memory updates.
            }
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }

    mq_close(mqStC);
    mq_close(mqStpC);
}

void userPortalProcess() {
    // Placeholder: In a full implementation, provide user interface for viewing and paying challans.
    while (running) {
        std::this_thread::sleep_for(std::chrono::seconds(5));
        // Implement user interactions here.
    }
}

void stripePaymentProcess() {
    mqd_t mqSTC = mq_open(MQ_STRIPE_TO_CHALLAN, O_WRONLY);
    if (mqSTC == (mqd_t)-1) {
        std::cerr << "StripePayment: Failed to open message queue." << std::endl;
        return;
    }

    while (running) {
        std::this_thread::sleep_for(std::chrono::seconds(20));
        PaymentMsg pmsg;
        std::string vid = "ABC-" + std::to_string(rand() % 9999);
        std::strncpy(pmsg.vehicleID, vid.c_str(), 31);
        pmsg.vehicleID[31] = '\0';
        pmsg.paid = true;
        if (mq_send(mqSTC, reinterpret_cast<const char*>(&pmsg), sizeof(pmsg), 0) == -1) {
            std::cerr << "mq_send (StripePayment) error: " << strerror(errno) << std::endl;
        } else {
            std::cout << "StripePayment: Simulated payment for vehicle " << vid << std::endl;
        }
    }

    mq_close(mqSTC);
}

// ------------------------------------------------------------
// Cleanup IPC and wait for children
// ------------------------------------------------------------
void cleanupAndExit(pid_t pid1, pid_t pid2, pid_t pid3) {
    running = false;

    // Wait for child processes
    if (pid1 > 0) waitpid(pid1, NULL, 0);
    if (pid2 > 0) waitpid(pid2, NULL, 0);
    if (pid3 > 0) waitpid(pid3, NULL, 0);

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

    exit(0);
}

// ------------------------------------------------------------
// Deadlock Prevention Function Definition
// ------------------------------------------------------------
void runDeadlockPrevention() {
    // Implement Banker’s Algorithm here for managing intersection resources.
    // Placeholder: currently no real logic beyond resource allocation count.
    // For a full implementation, maintain matrices for allocations, maximums, and needs.
    // Here, we're simplifying by only checking if allocated resources exceed limits.
}

// ------------------------------------------------------------
// Main (SmartTraffix) Process
// ------------------------------------------------------------
int main() {
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

    // Create message queues
    struct mq_attr mqAttr;
    mqAttr.mq_flags = 0;
    mqAttr.mq_maxmsg = 10;
    mqAttr.mq_msgsize = MQ_MAX_SIZE;
    mqAttr.mq_curmsgs = 0;

    mq_unlink(MQ_SMART_TO_CHALLAN);
    mq_unlink(MQ_STRIPE_TO_CHALLAN);

    mqSmartToChallan = mq_open(MQ_SMART_TO_CHALLAN, O_CREAT | O_WRONLY, 0644, &mqAttr);
    mqStripeToChallan = mq_open(MQ_STRIPE_TO_CHALLAN, O_CREAT | O_WRONLY, 0644, &mqAttr);

    if (mqSmartToChallan == (mqd_t)-1 || mqStripeToChallan == (mqd_t)-1) {
        std::cerr << "Failed to create message queues: " << strerror(errno) << std::endl;
        cleanupAndExit(-1, -1, -1);
    }

    // Load textures
    sf::Texture roadTexture, carTexture1, carTexture2, carTexture3;
    if (!roadTexture.loadFromFile("road.jpg") ||
        !carTexture1.loadFromFile("car1.png") ||
        !carTexture2.loadFromFile("car2.png") ||
        !carTexture3.loadFromFile("car3.png")) {
        std::cerr << "Failed to load textures!" << std::endl;
        cleanupAndExit(-1, -1, -1);
    }

    // Fork processes for ChallanGenerator, UserPortal, StripePayment
    pid_t pid1 = fork();
    if (pid1 == 0) {
        // Child: ChallanGenerator
        // Close write ends in child
        mq_close(mqSmartToChallan);
        mq_close(mqStripeToChallan);
        challanGeneratorProcess();
        exit(0);
    }
    else if (pid1 < 0) {
        std::cerr << "Failed to fork ChallanGenerator: " << strerror(errno) << std::endl;
        cleanupAndExit(-1, -1, -1);
    }

    pid_t pid2 = fork();
    if (pid2 == 0) {
        // Child: UserPortal
        userPortalProcess();
        exit(0);
    }
    else if (pid2 < 0) {
        std::cerr << "Failed to fork UserPortal: " << strerror(errno) << std::endl;
        cleanupAndExit(pid1, -1, -1);
    }

    pid_t pid3 = fork();
    if (pid3 == 0) {
        // Child: StripePayment
        // Close read ends in child
        mq_close(mqSmartToChallan);
        mq_close(mqStripeToChallan);
        stripePaymentProcess();
        exit(0);
    }
    else if (pid3 < 0) {
        std::cerr << "Failed to fork StripePayment: " << strerror(errno) << std::endl;
        cleanupAndExit(pid1, pid2, -1);
    }

    // Parent: SmartTraffix
    sf::RenderWindow window(sf::VideoMode(800, 600), "SmartTraffix Simulation");
    sf::Sprite roadSprite(roadTexture);
    roadSprite.setScale(1.0f, 1.0f);

    // Create threads in SmartTraffix
    pthread_t tLight, tSpawn, tSpeed, tOutOfOrder, tMockTime;

    // Traffic Light Controller
    {
        int rc = pthread_create(&tLight, nullptr, [](void*)->void* {
            trafficLightControllerThread();
            return nullptr;
        }, nullptr);
        if (rc != 0) {
            std::cerr << "Failed to create tLight thread: " << strerror(rc) << std::endl;
            cleanupAndExit(pid1, pid2, pid3);
        }
    }

    // Spawn Vehicles
    {
        sf::Texture* textures[3] = { &carTexture1, &carTexture2, &carTexture3 };
        int rc = pthread_create(&tSpawn, nullptr, [](void* arg)->void* {
            auto t = reinterpret_cast<sf::Texture**>(arg);
            spawnVehiclesThread(t[0], t[1], t[2]);
            return nullptr;
        }, textures);
        if (rc != 0) {
            std::cerr << "Failed to create tSpawn thread: " << strerror(rc) << std::endl;
            cleanupAndExit(pid1, pid2, pid3);
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
            cleanupAndExit(pid1, pid2, pid3);
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
            cleanupAndExit(pid1, pid2, pid3);
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
            cleanupAndExit(pid1, pid2, pid3);
        }
    }

    while (window.isOpen()) {
        sf::Event event;
        while (window.pollEvent(event)) {
            if (event.type == sf::Event::Closed) {
                running = false;
                window.close();
            }
        }

        processQueues(); 
        // Run deadlock prevention periodically if desired
        runDeadlockPrevention();
        visualizeTraffic(window, roadSprite);
    }

    // Cleanup
    running = false;
    pthread_join(tLight, nullptr);
    pthread_join(tSpawn, nullptr);
    pthread_join(tSpeed, nullptr);
    pthread_join(tOutOfOrder, nullptr);
    pthread_join(tMockTime, nullptr);

    cleanupAndExit(pid1, pid2, pid3);
    return 0;
}

