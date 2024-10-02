#include "LibKokkosEDPLogger.hpp"

#include <cstring>
#include <err.h>
#include <fcntl.h>
#include <omp.h>
#include <stdlib.h>
#include <unistd.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <format>
#include <iostream>
#include <stdexcept>
#include <string>

constexpr int AVG_WINDOW_SIZE = 5;

KokkosEDPLogger::KokkosEDPLogger() {
  // Open log file
  logFile.open("KokkosEDPLogger.kernel.csv",
               std::fstream::trunc | std::fstream::out | std::fstream::in);
  // Open global log file
  globalLogFile.open("KokkosEDPLogger.global.csv", std::fstream::trunc |
                                              std::fstream::out |
                                              std::fstream::in);

  // Get number of power zones
  auto numPowerZones_env = getenv("KEDP_NUM_POWER_ZONES");
  if (numPowerZones_env == NULL) {
    throw std::runtime_error(
        "KEDP_NUM_POWER_ZONES environment variable not set!");
  }
  auto numPowerZones = std::atoi(numPowerZones_env);

  // Open energy counter files
  for (int i = 0; i < numPowerZones; i++) {
    auto filename = std::format(
        "/sys/devices/virtual/powercap/intel-rapl/intel-rapl:{}/energy_uj", i);
    if (!std::filesystem::exists(filename)) {
      auto err = std::format("powercap file {} doesn't exist", filename);
      throw new std::runtime_error(err);
    }
    auto desc = energyDescriptor();
    desc.filename = filename;
    energyDescs.push_back(std::move(desc));
  }

  // Print headers
  logFile << "kernel_name,count,time_ms";
  for (int i = 0; i < numPowerZones; i++) {
    logFile << ",energy_uj" << i;
  }
  logFile << "\n";

  // Print headers
  globalLogFile << "time_ms";
  for (int i = 0; i < numPowerZones; i++) {
    globalLogFile << ",energy_uj" << i;
  }
  globalLogFile << "\n";

  maxThreads = omp_get_max_threads();
}

inline void KokkosEDPLogger::LogWrite(std::string line) { logFile << line << std::endl; }

inline void KokkosEDPLogger::Tick() {
  // If need to create a new quantum or
  // If previous kernel != current kernel, start a new quantum
  if (shouldCreateQuantum || prevKernel != curKernel) {
    for (auto &desc : energyDescs) {
      char buf[32] = {0};
      desc.fd = open(desc.filename.c_str(), O_RDONLY);
      read(desc.fd, buf, sizeof(buf));
      close(desc.fd);

      desc.tick = atoll(buf);
    }

    timeDesc.tick = timeDesc.clock.now();
    quantumSize = 0;
  }
}

inline void KokkosEDPLogger::Tock() {
  quantumSize++;
  shouldCreateQuantum = true;
  for (auto &desc : energyDescs) {
    char buf[32] = {0};
    desc.fd = open(desc.filename.c_str(), O_RDONLY);
    if (desc.fd < 0) {
      auto err = std::strerror(errno);
      auto s = std::format("cannot read file {}, {}", desc.filename, err);
      throw std::runtime_error(s);
    }
    read(desc.fd, buf, sizeof(buf));
    close(desc.fd);

    uint64_t tock = atoll(buf);
    // RAPL hasn't updated, don't end quantum
    if (tock == desc.tick) {
      shouldCreateQuantum = false;
      return;
    }
    desc.tock = tock;
  }

  timeDesc.tock = timeDesc.clock.now();
}

inline void KokkosEDPLogger::GlobalTick() {
  for (auto &desc : energyDescs) {
    char buf[32] = {0};
    desc.fd = open(desc.filename.c_str(), O_RDONLY);
    if (desc.fd < 0) {
      auto err = std::strerror(errno);
      auto s = std::format("cannot read file {}, {}", desc.filename, err);
      throw std::runtime_error(s);
    }
    read(desc.fd, buf, sizeof(buf));
    close(desc.fd);

    desc.globalTick = atoll(buf);
  }

  timeDesc.globalTick = timeDesc.clock.now();
}

inline void KokkosEDPLogger::GlobalTock() {
  for (auto &desc : energyDescs) {
    char buf[32] = {0};
    desc.fd = open(desc.filename.c_str(), O_RDONLY);
    if (desc.fd < 0) {
      auto err = std::strerror(errno);
      auto s = std::format("cannot read file {}, {}", desc.filename, err);
      throw std::runtime_error(s);
    }
    read(desc.fd, buf, sizeof(buf));
    close(desc.fd);

    desc.globalTock = atoll(buf);
  }

  timeDesc.globalTock = timeDesc.clock.now();
}

KokkosEDPLogger klaant;

extern "C" void kokkosp_init_library(const int loadSeq,
                                     const uint64_t interfaceVer,
                                     const uint32_t devInfoCount,
                                     void *deviceInfo) {
  klaant = KokkosEDPLogger();
  klaant.GlobalTick();
}

extern "C" void kokkosp_finalize_library() {
  klaant.GlobalTock();
  auto timeDiff = klaant.timeDesc.globalTock - klaant.timeDesc.globalTick;
  auto timeDiff_ms = std::chrono::duration<float, std::milli>(timeDiff);
  klaant.globalLogFile << timeDiff_ms.count();
  for (auto &energyDesc : klaant.energyDescs) {
    auto energyDiff = energyDesc.globalTock - energyDesc.globalTick;
    klaant.globalLogFile << "," << energyDiff;
  }
  klaant.globalLogFile << std::endl;
}

extern "C" void kokkosp_begin_parallel_for(const char *name,
                                           const uint32_t devID,
                                           uint64_t *kID) {
  klaant.prevKernel = klaant.curKernel;
  klaant.curKernel = name;
  klaant.Tick();
}

extern "C" void kokkosp_end_parallel_for(const uint64_t kID) {
  klaant.Tock();

  if (klaant.HasValidMeasure()) {
    auto timeDiff = klaant.timeDesc.tock - klaant.timeDesc.tick;
    auto timeDiff_ms = std::chrono::duration<float, std::milli>(timeDiff);
    klaant.logFile << "'" << klaant.curKernel << "',";
    klaant.logFile << klaant.quantumSize << ",";
    klaant.logFile << timeDiff_ms.count();
    for (auto &energyDesc : klaant.energyDescs) {
      auto energyDiff = energyDesc.tock - energyDesc.tick;
      klaant.logFile << "," << energyDiff;
    }
    klaant.logFile << std::endl;
  }
}

extern "C" void kokkosp_begin_parallel_scan(const char *name,
                                            const uint32_t devID,
                                            uint64_t *kID) {}

extern "C" void kokkosp_end_parallel_scan(const uint64_t kID) {}

extern "C" void kokkosp_begin_parallel_reduce(const char *name,
                                              const uint32_t devID,
                                              uint64_t *kID) {}

extern "C" void kokkosp_end_parallel_reduce(const uint64_t kID) {}
