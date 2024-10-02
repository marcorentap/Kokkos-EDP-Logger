#include <stdint.h>

#include <chrono>
#include <fstream>
#include <vector>

class KokkosEDPLogger { private:
  struct energyDescriptor {
    std::fstream file;
    int fd;
    std::string filename;
    uint64_t globalTick, globalTock;
    uint64_t tick, tock;
  };
  struct timeDecriptor {
    std::chrono::steady_clock clock;
    std::chrono::time_point<std::chrono::steady_clock> globalTick, globalTock;
    std::chrono::time_point<std::chrono::steady_clock> tick, tock;
  };

  bool shouldCreateQuantum;
  int maxThreads;

public:
  std::vector<struct energyDescriptor> energyDescs;
  struct timeDecriptor timeDesc;
  std::fstream logFile, globalLogFile;
  std::string curKernel, prevKernel;
  uint64_t quantumSize; // How many parallel_* calls in this quantum

  KokkosEDPLogger();

  inline void LogWrite(std::string line);

  // First kernel tick starts a quantum
  inline void Tick();

  // Last valid kernel tock ends a quantum
  inline void Tock();

  // Get statistics for the whole library
  inline void GlobalTick();

  inline void GlobalTock();

  inline bool HasValidMeasure() { return shouldCreateQuantum; }

};

extern "C" void kokkosp_init_library(const int loadSeq,
                                     const uint64_t interfaceVer,
                                     const uint32_t devInfoCount,
                                     void *deviceInfo);

extern "C" void kokkosp_finalize_library();

extern "C" void kokkosp_begin_parallel_for(const char *name,
                                           const uint32_t devID, uint64_t *kID);

extern "C" void kokkosp_end_parallel_for(const uint64_t kID);

extern "C" void kokkosp_begin_parallel_scan(const char *name,
                                            const uint32_t devID,
                                            uint64_t *kID);

extern "C" void kokkosp_end_parallel_scan(const uint64_t kID);

extern "C" void kokkosp_begin_parallel_reduce(const char *name,
                                              const uint32_t devID,
                                              uint64_t *kID);

extern "C" void kokkosp_end_parallel_reduce(const uint64_t kID);
