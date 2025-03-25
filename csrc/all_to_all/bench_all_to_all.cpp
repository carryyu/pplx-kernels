// All-to-all scatter benchmark

#include "all_to_all/internode.h"
#include "all_to_all/test_utils.h"
#include "core/cuda_utils.h"
#include "core/mpi_utils.h"

#include <cuda.h>
#include <cuda_profiler_api.h>
#include <mpi.h>
#include <nvshmem.h>
#include <nvshmemx.h>
#include <nvtx3/nvToolsExt.h>

#include <iomanip>
#include <iostream>

using namespace pplx;

namespace {

struct BenchConfig {
  int numTokens;
  int numExperts;
  int expertsPerToken;
  int hiddenDim;
  int blockSize;
};

using Time = std::pair<float, float>;

Time average(const std::vector<float> &timesUs) {
  float sum = 0.0f, sumSquared = 0.0f;
  for (const float time : timesUs) {
    sum += time;
    sumSquared += time * time;
  }
  const float mean = sum / timesUs.size();
  const float stddev = std::sqrt(sumSquared / timesUs.size() - mean * mean);
  return std::make_pair(mean, stddev);
}

template <typename T>
std::pair<Time, Time> benchmark(
    unsigned repeat,
    const BenchConfig &config,
    unsigned currentPE,
    unsigned numPEs,
    cudaStream_t stream
) {
  // Generate test data.
  std::mt19937 gen(currentPE);
  RankTestData<T> data(
      gen,
      config.numTokens,
      config.numExperts,
      config.expertsPerToken,
      config.hiddenDim,
      config.blockSize
  );

  const unsigned expertsPerRank = config.numExperts / numPEs;
  DeviceBuffer<int32_t> outTokensPerExpertDevice(expertsPerRank);
  DeviceBuffer<T> outExpertDevice(expertsPerRank * config.numTokens * numPEs * config.hiddenDim);
  DeviceBuffer<float> outExpertScaleDevice(
      expertsPerRank * config.numTokens * numPEs * data.hiddenDimScale
  );
  DeviceBuffer<nv_bfloat16> outTokensDevice(config.numTokens * data.hiddenDim);
  DeviceBuffer<T> xDevice(data.x);
  DeviceBuffer<float> xScaleDevice(data.xScale);
  DeviceBuffer<uint32_t> indicesDevice(data.indices);
  DeviceBuffer<float> weightsDevice(data.weights);

  const size_t hiddenDimBytes = data.hiddenDim * sizeof(T);
  const size_t hiddenDimScaleBytes = data.hiddenDimScale * sizeof(float);

  AllToAllInterNode allToAll(
      config.numTokens,
      config.numExperts,
      config.expertsPerToken,
      currentPE,
      numPEs,
      1,
      config.hiddenDim,
      hiddenDimBytes,
      hiddenDimScaleBytes
  );

  MPI_Barrier(MPI_COMM_WORLD);

  constexpr size_t numSamples = 10;

  std::array<std::tuple<cudaEvent_t, cudaEvent_t, cudaEvent_t>, numSamples> events;
  for (size_t i = 0; i < numSamples; ++i) {
    CUDACHECK(cudaEventCreate(&std::get<0>(events[i])));
    CUDACHECK(cudaEventCreate(&std::get<1>(events[i])));
    CUDACHECK(cudaEventCreate(&std::get<2>(events[i])));
  }

  // Warmup
  auto run = [&]() -> std::pair<float, float> {
    nvshmemx_barrier_all_on_stream(stream);
    // Scatter.
    for (size_t i = 0; i < numSamples; i++) {
      nvshmemx_barrier_all_on_stream(stream);
      CUDACHECK(cudaStreamSynchronize(stream));

      CUDACHECK(cudaEventRecord(std::get<0>(events[i]), stream));

      allToAll.scatter(
          Strided1D<int32_t>(outTokensPerExpertDevice, 1),
          Strided2D<std::byte>(
              outExpertDevice, hiddenDimBytes, hiddenDimBytes * config.numTokens * numPEs
          ),
          Strided2D<std::byte>(
              outExpertScaleDevice,
              hiddenDimScaleBytes,
              hiddenDimScaleBytes * config.numTokens * numPEs
          ),
          Strided1D<std::byte>(xDevice, hiddenDimBytes),
          Strided1D<std::byte>(xScaleDevice, hiddenDimScaleBytes),
          Strided2D<uint32_t>(indicesDevice, 1, config.expertsPerToken),
          data.m,
          nullptr,
          SplitMode::NONE,
          stream
      );

      CUDACHECK(cudaEventRecord(std::get<1>(events[i]), stream));

      allToAll.gather<T>(
          Strided1D<nv_bfloat16>(outTokensDevice, config.hiddenDim),
          Strided2D<uint32_t>(indicesDevice, 1, config.expertsPerToken),
          Strided2D<float>(weightsDevice, 1, config.expertsPerToken),
          Strided2D<T>(
              outExpertDevice, config.hiddenDim, config.hiddenDim * config.numTokens * numPEs
          ),
          data.m,
          nullptr,
          SplitMode::NONE,
          stream
      );

      CUDACHECK(cudaEventRecord(std::get<2>(events[i]), stream));
    }

    CUDACHECK(cudaStreamSynchronize(stream));
    float totalScatterMs = 0.0f, totalGatherMs = 0.0f;
    for (size_t i = 0; i < numSamples; i++) {
      float scatterMs = 0.0f, gatherMs = 0.0f;
      CUDACHECK(cudaEventElapsedTime(&scatterMs, std::get<0>(events[i]), std::get<1>(events[i])));
      CUDACHECK(cudaEventElapsedTime(&gatherMs, std::get<1>(events[i]), std::get<2>(events[i])));
      totalScatterMs += scatterMs;
      totalGatherMs += gatherMs;
    }
    return {totalScatterMs / numSamples, totalGatherMs / numSamples};
  };

  MPI_Barrier(MPI_COMM_WORLD);
  nvtxRangePush("warmup");
  for (int i = 0; i < 10; i++) {
    run();
  }
  nvtxRangePop();

  MPI_Barrier(MPI_COMM_WORLD);
  nvtxRangePush("benchmark");
  std::vector<float> scatterTimeUs, gatherTimeUs;
  for (int i = 0; i < repeat; i++) {
    auto [scatterTimeMs, gatherTimeMs] = run();
    scatterTimeUs.push_back(scatterTimeMs * 1000);
    gatherTimeUs.push_back(gatherTimeMs * 1000);
  }
  nvtxRangePop();

  return {average(scatterTimeUs), average(gatherTimeUs)};
}

} // namespace

int main(int argc, char **argv) {
  // Set up MPI.
  int world_size, rank;
  MPICHECK(MPI_Init(&argc, &argv));
  MPICHECK(MPI_Comm_rank(MPI_COMM_WORLD, &rank));
  MPICHECK(MPI_Comm_size(MPI_COMM_WORLD, &world_size));
  if (world_size < 4) {
    std::cout << "This test requires at least 4 workers" << std::endl;
    MPICHECK(MPI_Finalize());
    return EXIT_FAILURE;
  }

  // Set up NVSHMEM.
  MPI_Comm mpi_comm = MPI_COMM_WORLD;
  nvshmemx_init_attr_t attr = NVSHMEMX_INIT_ATTR_INITIALIZER;
  attr.mpi_comm = &mpi_comm;
  nvshmemx_init_attr(NVSHMEMX_INIT_WITH_MPI_COMM, &attr);
  int currentPE = nvshmem_my_pe();
  int numPEs = nvshmem_n_pes();

  // Set up the current rank.
  int deviceId = nvshmem_team_my_pe(NVSHMEMX_TEAM_NODE);
  CUDACHECK(cudaSetDevice(deviceId));
  cudaStream_t stream;
  CUDACHECK(cudaStreamCreate(&stream));
  cudaProfilerStart();

  // Run the benchmarks.
  std::vector<BenchConfig> configs = {
      // Tiny examples for debugging.
      {1, 8, 4, 128, 16},
      {4, 8, 6, 2048, 128},
      // 8 Experts
      {1, 8, 8, 7168, 128},
      {4, 8, 8, 7168, 128},
      {16, 8, 8, 7168, 128},
      {64, 8, 8, 7168, 128},
      {128, 8, 8, 7168, 128},
      // 16 experts
      {1, 16, 8, 7168, 128},
      {4, 16, 8, 7168, 128},
      {16, 16, 8, 7168, 128},
      {32, 16, 8, 7168, 128},
      {64, 16, 8, 7168, 128},
      {128, 16, 8, 7168, 128},
      // 64 experts
      {1, 64, 8, 7168, 128},
      {4, 64, 8, 7168, 128},
      {16, 64, 8, 7168, 128},
      {32, 64, 8, 7168, 128},
      {64, 64, 8, 7168, 128},
      {128, 64, 8, 7168, 128},
      // 256 experts
      {1, 256, 8, 7168, 128},
      {4, 256, 8, 7168, 128},
      {16, 256, 8, 7168, 128},
      {32, 256, 8, 7168, 128},
      {64, 256, 8, 7168, 128},
      {128, 256, 8, 7168, 128},
  };

  for (const auto &config : configs) {
    auto [scatter, gather] = benchmark<nv_bfloat16>(10, config, currentPE, numPEs, stream);
    if (currentPE == 0) {
      auto [scatterMean, scatterStddev] = scatter;
      auto [gatherMean, gatherStddev] = gather;
      std::cout << std::setw(3) << config.numTokens << " " << std::setw(3) << config.numExperts
                << " " << std::setw(3) << config.expertsPerToken << " " << std::setw(4)
                << config.hiddenDim << " " << std::fixed << std::setprecision(3)
                << "Scatter: " << std::setw(10) << scatterMean << "us ± " << scatterStddev << "us "
                << "Gather: " << std::setw(10) << gatherMean << "us ± " << gatherStddev << "us"
                << std::endl;
    }
  }

  // Cleanup.
  CUDACHECK(cudaStreamDestroy(stream));
  nvshmem_barrier_all();
  nvshmem_finalize();
  cudaProfilerStop();
  MPICHECK(MPI_Finalize());
  return EXIT_SUCCESS;
}
