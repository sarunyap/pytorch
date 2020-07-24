#include <c10d/ProcessGroupNCCL.hpp>

#include <map>
#include <tuple>
#include <unordered_set>

#include <THC/THC.h>

#include <ATen/cuda/CUDAContext.h>
#include <c10/cuda/CUDAGuard.h>

#include <c10d/Utils.hpp>

namespace c10d {

constexpr const char* const kNCCLAbortedCommStoreKey = "NCCLABORTEDCOMM";

namespace {

// RAII helper class to manage NCCL group API and CUDA free mutex.
// The destructor is allowed to throw since this helper class only
// manages group and lock lifetimes.
struct AutoNcclGroup {
  AutoNcclGroup() {
    (c10::cuda::CUDACachingAllocator::getFreeMutex())->lock();
#if defined(NCCL_MAJOR) && (NCCL_MAJOR >= 2)
    C10D_NCCL_CHECK(ncclGroupStart());
#endif
  }
  ~AutoNcclGroup() noexcept(false) {
#if defined(NCCL_MAJOR) && (NCCL_MAJOR >= 2)
    C10D_NCCL_CHECK(ncclGroupEnd());
#endif
    (c10::cuda::CUDACachingAllocator::getFreeMutex())->unlock();
  }
};

// NCCL op mapping
std::map<ReduceOp, ncclRedOp_t> ncclOp = {
    {ReduceOp::MIN, ncclMin},
    {ReduceOp::MAX, ncclMax},
    {ReduceOp::SUM, ncclSum},
    {ReduceOp::PRODUCT, ncclProd},
};

// NCCL type typing
std::map<at::ScalarType, ncclDataType_t> ncclDataType = {
    {at::kChar, ncclInt8},
    {at::kByte, ncclUint8},
    {at::kFloat, ncclFloat},
    {at::kDouble, ncclDouble},
    {at::kInt, ncclInt32},
    {at::kLong, ncclInt64},
    {at::kHalf, ncclHalf},
};

// Helper function that gets the data type and issues error if not supported
ncclDataType_t getNcclDataType(at::ScalarType type) {
  try {
    return ncclDataType.at(type);
  } catch (std::out_of_range& e) {
    throw std::runtime_error("Unsupported data type for NCCL process group");
  }
}

// Get the deviceList String from the list of devices
std::string getKeyFromDevices(const std::vector<at::Device>& devices) {
  std::string deviceList;
  for (auto& device : devices) {
    if (deviceList.empty()) {
      deviceList = std::to_string(device.index());
    } else {
      deviceList += "," + std::to_string(device.index());
    }
  }
  return deviceList;
}

// Get the list of devices from list of tensors
std::vector<at::Device> getDeviceList(const std::vector<at::Tensor>& tensors) {
  std::vector<at::Device> res;
  res.reserve(tensors.size());
  for (auto& tensor : tensors) {
    res.push_back(tensor.device());
  }
  return res;
}

// [Sync Streams] Helper that lets the input ncclStreams to wait for the current
// stream. NCCL communications run on ncclStreams, but input tensors are
// allocated on different streams (i.e., current streams). Communications on
// ncclStreams cannot start before pending input tensor ops on current streams
// finish. Otherwise, ops on two streams might read/write same tensors
// concurrently.
//
// The synchronization above alone is not enough. We also need to make sure
// input tensors are not freed before their usages on ncclStreams finish. This
// can be achieved by calling c10::cuda::CUDACachingAllocator::recordStream,
// which remembers the usage stream (ncclStream), creates an event on the usage
// stream when GC attempts to free the input tensor, and delays GC until that
// event is done.
void syncStreams(
    const std::vector<at::Device>& devices,
    std::vector<at::cuda::CUDAEvent>& ncclEvents,
    std::vector<at::cuda::CUDAStream>& ncclStreams) {
  for (size_t i = 0; i < devices.size(); ++i) {
    at::cuda::CUDAStream& ncclStream = ncclStreams[i];
    at::cuda::CUDAEvent& ncclEvent = ncclEvents[i];
    ncclEvent.record(at::cuda::getCurrentCUDAStream(devices[i].index()));
    ncclEvent.block(ncclStream);
  }
}

// Given a ncclUniqueId, convert it to a string representation that can be put
// in the store.
std::string buildNcclUniqueIdStr(const ncclUniqueId& ncclID) {
  const uint8_t* bytes = reinterpret_cast<const uint8_t*>(&ncclID);
  std::ostringstream oss;
  for (size_t i = 0; i < NCCL_UNIQUE_ID_BYTES; i++) {
    oss << std::hex << static_cast<int>(bytes[i]);
  }
  return oss.str();
}

std::string getNcclAbortedCommStoreKey(const std::string ncclIdStr) {
  return std::string(kNCCLAbortedCommStoreKey) + ":" + ncclIdStr;
}

void checkSplitSizes(
    const std::vector<int64_t>& split_sizes,
    const at::Tensor& tensor,
    int group_size) {
  if (split_sizes.size() == 0) {
    if (tensor.size(0) % group_size != 0) {
      throw std::runtime_error(
          "Tensor's dim 0 does not divide equally across group size");
    }
  } else {
    if (split_sizes.size() != group_size) {
      throw std::runtime_error(
          "Number of tensor splits not equal to group size");
    }
    int sum = std::accumulate(split_sizes.begin(), split_sizes.end(), 0);
    if (sum != tensor.size(0)) {
      throw std::runtime_error("Split sizes doesn't match total dim 0 size");
    }
  }
}

} // namespace

const int64_t ProcessGroupNCCL::kWatchdogThreadSleepMillis = 10000;
constexpr int64_t kWaitForAbortCommStoreKey = 1000;
constexpr int64_t kSynchronizeBusyWaitMillis = 10;
const int64_t ProcessGroupNCCL::kProcessGroupNCCLOpTimeoutMillis = 10 * 1000;

ProcessGroupNCCL::WorkNCCL::WorkNCCL(const std::vector<at::Device>& devices)
    : devices_(devices), workStartTime_(std::chrono::steady_clock::now()) {
  // Creates the CUDA event wrappers
  // Note: The actual events are lazily created when first recorded to with
  // DEFAULT_FLAGS = cudaEventDisableTiming.
  cudaEvents_.resize(devices.size());
  ncclComms_.resize(devices.size());
}

ProcessGroupNCCL::WorkNCCL::~WorkNCCL() {}

bool ProcessGroupNCCL::WorkNCCL::isCompleted() {
  checkAndSetException();
  return exception() || finishedGPUExecutionInternal();
}

bool ProcessGroupNCCL::WorkNCCL::isSuccess() const {
  if (exception()) {
    // Already detected an exception.
    return false;
  }

  return !checkForNCCLErrors(ncclComms_) && finishedGPUExecutionInternal();
}

void ProcessGroupNCCL::WorkNCCL::checkAndSetException() {
  if (exception()) {
    // We already have an exception.
    return;
  }

  auto exception_ptr = checkForNCCLErrors(ncclComms_);
  std::unique_lock<std::mutex> lock(mutex_);
  exception_ = exception_ptr;
}

// Helper that checks if the NCCL kernels are completed on the GPUs
bool ProcessGroupNCCL::WorkNCCL::finishedGPUExecution() {
  checkAndSetException();
  return finishedGPUExecutionInternal();
}

bool ProcessGroupNCCL::WorkNCCL::finishedGPUExecutionInternal() const {
  for (size_t i = 0; i < devices_.size(); ++i) {
    // Checking the work's corresponding CUDA events' status
    auto ret = cudaEventQuery(cudaEvents_[i]);
    if (ret != cudaSuccess && ret != cudaErrorNotReady) {
      AT_CUDA_CHECK(ret);
    }
    if (ret == cudaErrorNotReady) {
      return false;
    }
  }
  return true;
}

void ProcessGroupNCCL::WorkNCCL::checkAndThrowException() {
  // Set the appropriate exception if found.
  checkAndSetException();

  // Throw an exception, only if we have a valid exception.
  if (exception()) {
    std::rethrow_exception(exception());
  }
}

// Waiting on the work's corresponding CUDA events
void ProcessGroupNCCL::WorkNCCL::synchronize() {
  for (size_t i = 0; i < devices_.size(); ++i) {
    auto currentStream = at::cuda::getCurrentCUDAStream(devices_[i].index());
    // Block the current stream on the NCCL stream
    cudaEvents_[i].block(currentStream);
    // If we use the work to do barrier, we should block here
    if (!barrierTensors_.empty()) {
      at::cuda::CUDAGuard gpuGuard(devices_[i]);
      AT_CUDA_CHECK(cudaDeviceSynchronize());
    }
  }

  // In case of blocking, wait for the operation to complete.
  if (blockingWait_) {
    // Wait for the operation to complete.
    while (!isCompleted()) {
      auto currentTimepoint = std::chrono::steady_clock::now();
      if (std::chrono::duration_cast<std::chrono::milliseconds>(
              currentTimepoint - workStartTime_) > opTimeout_) {
        // When operation times out due to some errors that are not
        // detected by nccl communicators, ncclCommWatchdog can not check this
        // time out error and thus can not abort ncclComms accordingly.
        // So explicitly abort ncclComms here before throwing this timed out
        // exception to users, after this, ncclCommWatchdog can detect nccl
        // communicators are aborted and clean up devNCCLCommMap_ accordingly.
        // if throwing timed out excepiton without aborting nccl communicators
        // here, it was observed that CUDA GPU will have 100% utilization and
        // can not run new events successfully.
        for (const auto& ncclComm : ncclComms_) {
          ncclComm->ncclCommAbort();
          const auto& storeKey = getNcclAbortedCommStoreKey(
              buildNcclUniqueIdStr(ncclComm->getNcclId()));
          store_->set(storeKey, {});
          LOG(INFO) << "Wrote aborted communicator id to store: " << storeKey;
        }
        throw std::runtime_error("Operation timed out!");
      }
      // Check for errors and throw appropriate exception.
      checkAndThrowException();
      std::this_thread::sleep_for(
          std::chrono::milliseconds(kSynchronizeBusyWaitMillis));
    }
    checkAndThrowException();
  }
}

// Same as calling synchronize().
bool ProcessGroupNCCL::WorkNCCL::wait() {
  synchronize();
  // Always return true, because abort API is not implemented.
  return true;
}

void ProcessGroupNCCL::WorkNCCL::abort() {
  TORCH_CHECK(false, "ProcessGroupNCCL::WorkNCCL::abort not implemented.");
}

ProcessGroupNCCL::ProcessGroupNCCL(
    const std::shared_ptr<Store>& store,
    int rank,
    int size,
    const std::chrono::milliseconds& opTimeout)
    : ProcessGroup(rank, size),
      store_(store),
      ncclCommCounter_(0),
      terminateWatchdog_(false),
      opTimeout_(opTimeout) {
  char* blockingWait = getenv(NCCL_BLOCKING_WAIT);
  try {
    if (blockingWait != nullptr) {
      auto val = std::stoi(blockingWait);
      if (val == 1) {
        // Make wait() and synchronize() a blocking call.
        blockingWait_ = true;
      } else if (val != 0) {
        throw std::runtime_error(
            "Invalid value for environment variable: " +
            std::string(NCCL_BLOCKING_WAIT));
      }
    }
  } catch (std::exception& e) {
    throw std::runtime_error(
        "Invalid value for environment variable: " +
        std::string(NCCL_BLOCKING_WAIT));
  }

#ifdef ENABLE_NCCL_ERROR_CHECKING
  ncclCommWatchdogThread_ =
      std::thread(&ProcessGroupNCCL::ncclCommWatchdog, this);
#endif
}

ProcessGroupNCCL::~ProcessGroupNCCL() {
  terminateWatchdog_.store(true);
  watchdogCV_.notify_one();
#ifdef ENABLE_NCCL_ERROR_CHECKING
  ncclCommWatchdogThread_.join();
#endif
}

void ProcessGroupNCCL::ncclCommWatchdog() {
  try {
    ncclCommWatchdogInternal();
    LOG(INFO) << "NCCL watchdog thread terminated normally";
  } catch (std::exception& e) {
    LOG(INFO) << "NCCL watchdog thread terminated with exception: " << e.what();
  } catch (...) {
    LOG(INFO) << "NCCL watchdog thread terminated with unknown exception";
  }
}

void ProcessGroupNCCL::ncclCommWatchdogInternal() {
  while (!terminateWatchdog_.load()) {
    std::unordered_set<std::string> abortedCommIds;
    std::unordered_set<std::string> allCommIds;

    {
      // Loop through the cache of communicators for NCCL errors.
      std::lock_guard<std::mutex> lock(mutex_);
      for (auto it = devNCCLCommMap_.begin(); it != devNCCLCommMap_.end();
           it++) {
        auto& ncclComms = it->second;

        for (const auto& ncclComm : ncclComms) {
          allCommIds.emplace(buildNcclUniqueIdStr(ncclComm->getNcclId()));
        }

        if (checkForNCCLErrors(ncclComms)) {
          LOG(INFO) << "Received NCCL errors for communicators in the cache";

          if (blockingWait_) {
            LOG(INFO) << "Aborting communicators that received errors";
            // We should not abort the communicators if we are performing a
            // non-blocking wait(). The reason for this is that if we abort the
            // nccl communicator, wait() might not throw exceptions and
            // subsequent operations might run on garbage results.
            // The current model is that when we call wait(), subsequent
            // operations only run after this work is done or we hang forever
            // waiting for the operation to complete.
            for (const auto& ncclComm : ncclComms) {
              ncclComm->ncclCommAbort();
              // Note that we don't remove the aborted communicators from the
              // cache. The reason is that if we do remove the communicator
              // from the cache, it is possible that a new collective operation
              // calls `ncclCommInitRank` to create a new communicator whereas
              // other ranks might have failed/timed out and didn't enter
              // `ncclCommInitRank`. As a result, when there is a failure on
              // a communicator the application receives an exception and its
              // their responsibility to destroy the process group and recreate
              // it to recover from errors.
              abortedCommIds.emplace(
                  buildNcclUniqueIdStr(ncclComm->getNcclId()));
            }
          }
        }
      }
    }

    if (blockingWait_) {
      // When we abort a communicator on one rank, it is likely that might cause
      // other ranks to hang indefinitely. As a result, whenever we abort a
      // communicator, we write its ID to the store. The watchdog on other ranks
      // then monitor the store, find an aborted communicator ID and abort their
      // respective communicator as well.

      // Record the aborted communicators locally and in the store.
      for (const auto& abortedCommId : abortedCommIds) {
        abortedComms_.emplace(abortedCommId);
        const auto& storeKey = getNcclAbortedCommStoreKey(abortedCommId);
        store_->set(storeKey, {});
        LOG(INFO) << "Watchdog wrote aborted communicator id to store: "
                  << storeKey;
      }

      // Check for any communicators in the store and abort them if needed.
      for (const auto& commId : allCommIds) {
        if (abortedComms_.find(commId) == abortedComms_.end()) {
          // Check if we need to abort them if not already aborted (shouldn't
          // wait more than the watchdog sleep time.).
          const auto& storeKey = getNcclAbortedCommStoreKey(commId);
          try {
            store_->wait(
                {storeKey},
                std::chrono::milliseconds(kWaitForAbortCommStoreKey));
            LOG(INFO) << "Found key in store: " << storeKey
                      << ", aborting appropriate communicators";

            // Now abort the appropriate communicators.
            std::lock_guard<std::mutex> lock(mutex_);
            auto it = ncclIdToCommMap_.find(commId);
            TORCH_INTERNAL_ASSERT(it != ncclIdToCommMap_.end());
            for (const auto& ncclComm : it->second) {
              ncclComm->ncclCommAbort();
            }
            abortedComms_.emplace(commId);
            LOG(INFO) << "Aborted communicators for key in store: " << storeKey;
          } catch (std::exception& e) {
            VLOG(1) << "Did not find key in store: " << storeKey
                    << ", error: " << e.what();
          }
        }
      }
    }

    std::unique_lock<std::mutex> lock(watchdogCVMutex_);
    watchdogCV_.wait_for(
        lock,
        std::chrono::milliseconds(kWatchdogThreadSleepMillis),
        [&]() -> bool { return terminateWatchdog_.load(); });
  }
}

std::exception_ptr ProcessGroupNCCL::WorkNCCL::checkForNCCLErrors(
    const std::vector<std::shared_ptr<NCCLComm>>& ncclComms) const {
  return checkForNCCLErrorsInternal(ncclComms);
}

std::exception_ptr ProcessGroupNCCL::checkForNCCLErrors(
    const std::vector<std::shared_ptr<NCCLComm>>& ncclComms) {
  return checkForNCCLErrorsInternal(ncclComms);
}

std::exception_ptr ProcessGroupNCCL::checkForNCCLErrorsInternal(
    const std::vector<std::shared_ptr<NCCLComm>>& ncclComms) {
  for (const auto& ncclComm : ncclComms) {
    ncclResult_t ncclAsyncErr = ncclComm->checkForNcclError();
    if (ncclAsyncErr != ncclSuccess) {
      return std::make_exception_ptr(std::runtime_error(
          "NCCL error: " + ncclGetErrorWithVersion(ncclAsyncErr)));
    }
  }

  return nullptr;
}

void ProcessGroupNCCL::broadcastUniqueNCCLID(ncclUniqueId* ncclID) {
  // For every NCCL communicator that we create we need to broadcast
  // a unique ID from rank 0 to all other ranks. This broadcast is
  // done by rank 0 setting a key in the store and all other ranks
  // retrieving the contents of that key. A single process group
  // may create multiple NCCL communicators, so we use a sequence
  // number to differentiate between them.
  std::string storeKey = std::to_string(ncclCommCounter_++);
  if (rank_ == 0) {
    auto vec = std::vector<uint8_t>(
        reinterpret_cast<uint8_t*>(ncclID),
        reinterpret_cast<uint8_t*>(ncclID) + NCCL_UNIQUE_ID_BYTES);
    store_->set(storeKey, vec);
  } else {
    auto vec = store_->get(storeKey);
    TORCH_CHECK(vec.size() == NCCL_UNIQUE_ID_BYTES);
    std::memcpy(ncclID, vec.data(), vec.size());
  }
}

std::vector<std::shared_ptr<NCCLComm>>& ProcessGroupNCCL::getNCCLComm(
    const std::string& devicesKey,
    const std::vector<at::Device>& devices) {
  // Sanity check
  if (devicesKey.empty()) {
    throw std::runtime_error(
        "Not able to create/get the NCCL Communicator since "
        "the GPU devices are not known");
  }

  for (auto& device : devices) {
    usedDeviceIdxs_.insert(device.index());
  }

  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (devNCCLCommMap_.find(devicesKey) != devNCCLCommMap_.end()) {
      // Reuse the cached communicator if there is one.
      return devNCCLCommMap_[devicesKey];
    }
  }

  // NCCL communicator not cached, create a new entry
  std::vector<std::shared_ptr<NCCLComm>> ncclComms;
  ncclComms.resize(devices.size());

  // Create the unique NCCL ID and broadcast it
  ncclUniqueId ncclID;

  if (rank_ == 0) {
    C10D_NCCL_CHECK(ncclGetUniqueId(&ncclID));
  }

  // Broadcast so that each process can have a unique NCCL ID
  broadcastUniqueNCCLID(&ncclID);

  at::cuda::OptionalCUDAGuard gpuGuard;

  std::vector<at::cuda::CUDAStream> streamVal;
  streamVal.reserve(devices.size());

  // Create the NCCL communicators for each GPU
  C10D_NCCL_CHECK(ncclGroupStart());

  for (size_t i = 0; i < devices.size(); ++i) {
    // GPU world size and GPU rank
    int numRanks = getSize() * devices.size();
    int rank = getRank() * devices.size() + i;

    gpuGuard.set_index(devices[i].index());
    ncclComms[i] = NCCLComm::create(numRanks, rank, ncclID);

    // Creates the NCCL streams
    streamVal.push_back(at::cuda::getStreamFromPool());
  }

  C10D_NCCL_CHECK(ncclGroupEnd());

  ncclStreams_.emplace(devicesKey, std::move(streamVal));

  // Note: these events are created with the (default) cudaEventDisableTiming
  // flag This flag provides the best performance when used with
  // cudaStreamWaitEvent() and cudaEventQuery(). Since we here don't measure the
  // performance using cudaEvent, this should be set.
  ncclEvents_.emplace(
      std::piecewise_construct,
      std::make_tuple(devicesKey),
      std::make_tuple(devices.size()));

  // Hold the lock before modifying the cache.
  std::lock_guard<std::mutex> lock(mutex_);

  // Record the communicators based on ncclUniqueId.
  ncclIdToCommMap_.emplace(buildNcclUniqueIdStr(ncclID), ncclComms);

  // Move the NCCL resource to cache
  devNCCLCommMap_.emplace(devicesKey, std::move(ncclComms));
  return devNCCLCommMap_[devicesKey];
}

namespace {

// Check that all `tensors' have the same type and shape and are distributed
// across distinct GPUs. If alltoallv is true, distinct gpu and identical size
// tests are waived
void check_gpu_tensors_multi(
    const std::vector<at::Tensor>& tensors,
    bool alltoallv) {
  if (tensors.size() == 0) {
    throw std::runtime_error("Tensor list must be nonempty");
  }
  if (!alltoallv &&
      tensors.size() > static_cast<size_t>(at::cuda::getNumGPUs())) {
    throw std::runtime_error(
        "Tensor list mustn't be larger than the number of available GPUs");
  }

  const auto& first = tensors.front();

  // Set for ensuring that tensors are on separate devices.
  std::unordered_set<decltype(first.get_device())> usedDevices;
  usedDevices.reserve(tensors.size());

  for (const auto& t : tensors) {
    if (!t.is_cuda() || t.is_sparse()) {
      throw std::runtime_error("Tensors must be CUDA and dense");
    }
    if (t.scalar_type() != first.scalar_type()) {
      throw std::runtime_error("Tensors must have identical type");
    }
    if (!alltoallv && t.sizes() != first.sizes()) {
      throw std::runtime_error("Tensors must have identical size");
    }
    if (!t.is_contiguous()) {
      throw std::runtime_error("Tensors must be contiguous");
    }
    if (!alltoallv) {
      const auto inserted = usedDevices.insert(t.get_device()).second;
      if (!inserted) {
        throw std::runtime_error("Tensors must be on distinct GPU devices");
      }
    }
  }
}

void check_gpu_tensors(const std::vector<at::Tensor>& tensors) {
  check_gpu_tensors_multi(tensors, false);
}

// Flatten each list in `tensor_lists' for a gather or scatter operation, and
// ensure compatibility with the corresponding tensor in `other'.
std::vector<at::Tensor> flatten_for_scatter_gather(
    std::vector<std::vector<at::Tensor>>& tensor_lists,
    std::vector<at::Tensor>& other,
    size_t world_size,
    size_t rank,
    bool no_copy) {
  if (tensor_lists.size() != other.size()) {
    throw std::runtime_error(
        "Tensor list operands to scatter/gather must have the same length");
  }
  const auto num_devices = tensor_lists.size();

  std::vector<at::Tensor> flattened;
  flattened.resize(num_devices);

  for (auto i = size_t{}; i < num_devices; ++i) {
    if (tensor_lists[i].size() != world_size * num_devices) {
      throw std::runtime_error(
          "Tensor list input to scatter/gather must match number of collective"
          " participants");
    }

    // Only check device match for the first tensor in the list; the call to
    // newLikeFlat() below will check the rest.
    if (tensor_lists[i].front().get_device() != other[i].get_device()) {
      throw std::runtime_error(
          "Corresponding input/output tensors to scatter/gather must all reside"
          " on the same device");
    }

    for (const auto& t : tensor_lists[i]) {
      if (t.numel() != other[i].numel()) {
        throw std::runtime_error(
            "All tensor operands to scatter/gather must have the same size");
      }
    }

    if (no_copy) {
      // no_copy operation requires all tensors in tensor_list are contiguous views
      // into a single flattened tensor
      for (auto j = size_t{}; j < tensor_lists[i].size(); ++j) {
        auto t = tensor_lists[i][j];
        if (!tensor_lists[i][j].storage().is_alias_of(tensor_lists[i][0].storage()) ||
            tensor_lists[i][j].storage_offset() != (tensor_lists[i][0].storage_offset() +
              j * tensor_lists[i][0].numel())) {
          no_copy = false;
          break;
        }
      }
      // no_copy operation is allowed if other tensor does not share storage with tensors
      // in tensor_list or other tensor is properly aligned according to rank.
      if (other[i].storage().is_alias_of(tensor_lists[i][0].storage()) &&
          other[i].storage_offset() != (tensor_lists[i][0].storage_offset() +
            rank * tensor_lists[i][0].numel())) {
        no_copy = false;
      }
    }

    if (no_copy) {
      flattened[i] = at::empty({0}, other[i].options()).set_(
          tensor_lists[i][0].storage(),
          tensor_lists[i][0].storage_offset(),
          world_size * other[i].numel(), {});
    } else {
      // Flatten the tensors (from all ranks) into a single big tensor.
      flattened[i] = newLikeFlat(tensor_lists, i);
    }
  }
  return flattened;
}

} // namespace

std::shared_ptr<ProcessGroupNCCL::WorkNCCL> ProcessGroupNCCL::initWork(
    std::vector<at::Device> devices) {
  return std::make_shared<ProcessGroupNCCL::WorkNCCL>(devices);
}

template <typename Fn, typename PreProcess, typename PostProcess>
std::shared_ptr<ProcessGroup::Work> ProcessGroupNCCL::collective(
    std::vector<at::Tensor>& inputs,
    std::vector<at::Tensor>& outputs,
    Fn fn,
    PreProcess pre,
    PostProcess post) {
  const auto devices = getDeviceList(inputs);
  const auto key = getKeyFromDevices(devices);
  auto& ncclComms = getNCCLComm(key, devices);

  // First let NCCL streams wait for input tensors allocation streams
  syncStreams(devices, ncclEvents_[key], ncclStreams_[key]);

  // Work itself will create the CUDA events on all GPUs of tensors
  auto work = initWork(devices);

  at::cuda::OptionalCUDAGuard gpuGuard;

  pre(ncclStreams_[key]);

  for (size_t i = 0; i < inputs.size(); ++i) {
    gpuGuard.set_index(devices[i].index());
    at::cuda::CUDAStream& ncclStream = ncclStreams_[key][i];

    // Both `inputs' and `outputs' are created on a worker stream and used in
    // different ncclStreams.  Hence, both must record the ncclStream to
    // prevent being freed before the collective finishes.
    //
    // We only record `inputs' here, and leave recording `outputs' to `fn' for
    // operations where `inputs' and `outputs' are not the same.
    //
    // See [Sync Streams].
    c10::cuda::CUDACachingAllocator::recordStream(
        inputs[i].storage().data_ptr(), ncclStream);
  }

  {
    AutoNcclGroup nccl_group_guard;
    for (size_t i = 0; i < inputs.size(); ++i) {
      gpuGuard.set_index(devices[i].index());
      at::cuda::CUDAStream& ncclStream = ncclStreams_[key][i];
      C10D_NCCL_CHECK(
          fn(inputs[i], outputs[i], ncclComms[i]->getNcclComm(), ncclStream));
    }
  }

  post(ncclStreams_[key]);

  // Event should only be recorded after the ncclGroupEnd()
  for (size_t i = 0; i < inputs.size(); ++i) {
    at::cuda::CUDAStream& ncclStream = ncclStreams_[key][i];
    work->cudaEvents_[i].record(ncclStream);
    work->ncclComms_[i] = ncclComms[i];
    work->blockingWait_ = blockingWait_;
    work->opTimeout_ = opTimeout_;
    work->store_ = store_;
  }

  return work;
}

template <typename Fn>
std::shared_ptr<ProcessGroup::Work> ProcessGroupNCCL::collective(
    std::vector<at::Tensor>& inputs,
    std::vector<at::Tensor>& outputs,
    Fn fn) {
  return collective(
      inputs,
      outputs,
      fn,
      [](std::vector<at::cuda::CUDAStream>&) {},
      [](std::vector<at::cuda::CUDAStream>&) {});
}

// Should be redesigned to support multigpu alltoall.
// Currently inputs and outputs vectors always have size 1
// send_* and recv_* should be changed to std::vector<std::vector<T>>
std::shared_ptr<ProcessGroup::Work> ProcessGroupNCCL::batchedp2p(
    std::vector<void*>& send_ptrs,
    std::vector<int>& send_lengths,
    std::vector<void*>& recv_ptrs,
    std::vector<int>& recv_lengths,
    ncclDataType_t datatype,
    std::vector<at::Tensor>& inputs,
    std::vector<at::Tensor>& outputs) {
  const auto devices = getDeviceList(inputs);
  const auto key = getKeyFromDevices(devices);
  auto& ncclComms = getNCCLComm(key, devices);

  // First let NCCL streams wait for input tensors allocation streams
  syncStreams(devices, ncclEvents_[key], ncclStreams_[key]);

  // Work itself will create the CUDA events on all GPUs of tensors
  auto work = initWork(devices);

  at::cuda::OptionalCUDAGuard gpuGuard;

  for (size_t i = 0; i < inputs.size(); ++i) {
    gpuGuard.set_index(devices[i].index());
    at::cuda::CUDAStream& ncclStream = ncclStreams_[key][i];
    c10::cuda::CUDACachingAllocator::recordStream(
        inputs[i].storage().data_ptr(), ncclStream);
  }

  {
    AutoNcclGroup nccl_group_guard;
    for (int i = 0; i < size_; ++i) {
      for (size_t j = 0; j < inputs.size(); ++j)
        gpuGuard.set_index(devices[j].index());
      at::cuda::CUDAStream& ncclStream =
          ncclStreams_[key][0]; // FIXME single stream anyway
      C10D_NCCL_CHECK(ncclSend(
          send_ptrs[i],
          send_lengths[i],
          datatype,
          i,
          ncclComms[0]->getNcclComm(),
          ncclStream));
      C10D_NCCL_CHECK(ncclRecv(
          recv_ptrs[i],
          recv_lengths[i],
          datatype,
          i,
          ncclComms[0]->getNcclComm(),
          ncclStream));
    }
  }

  for (size_t i = 0; i < outputs.size(); ++i) {
    gpuGuard.set_index(devices[i].index());
    at::cuda::CUDAStream& ncclStream = ncclStreams_[key][i];
    c10::cuda::CUDACachingAllocator::recordStream(
        outputs[i].storage().data_ptr(), ncclStream);
  }

  // Event should only be recorded after the ncclGroupEnd()
  for (size_t i = 0; i < inputs.size(); ++i) {
    at::cuda::CUDAStream& ncclStream = ncclStreams_[key][i];
    work->cudaEvents_[i].record(ncclStream);
    work->ncclComms_[i] = ncclComms[i];
    work->blockingWait_ = blockingWait_;
    work->opTimeout_ = opTimeout_;
  }

  return work;
}

std::shared_ptr<ProcessGroup::Work> ProcessGroupNCCL::allreduce(
    std::vector<at::Tensor>& tensors,
    const AllreduceOptions& opts) {
  check_gpu_tensors(tensors);

  return collective(
      tensors,
      tensors,
      [&](at::Tensor& input,
          at::Tensor& output,
          ncclComm_t comm,
          at::cuda::CUDAStream& stream) {
        return ncclAllReduce(
            input.data_ptr(),
            output.data_ptr(),
            input.numel(),
            getNcclDataType(input.scalar_type()),
            ncclOp[opts.reduceOp],
            comm,
            stream.stream());
      });
}

std::shared_ptr<ProcessGroup::Work> ProcessGroupNCCL::allreduce_coalesced(
    std::vector<at::Tensor>& tensors,
    const AllreduceCoalescedOptions& opts) {
  throw std::runtime_error(
      "allreduce_coalesced is currently not supported with NCCL");
}

std::shared_ptr<ProcessGroup::Work> ProcessGroupNCCL::broadcast(
    std::vector<at::Tensor>& tensors,
    const BroadcastOptions& opts) {
  check_gpu_tensors(tensors);

  return collective(
      tensors,
      tensors,
      [&](at::Tensor& input,
          at::Tensor& output,
          ncclComm_t comm,
          at::cuda::CUDAStream& stream) {
        const auto root = opts.rootRank * tensors.size() + opts.rootTensor;
        return ncclBcast(
            input.data_ptr(),
            input.numel(),
            getNcclDataType(input.scalar_type()),
            root,
            comm,
            stream.stream());
      });
}

std::shared_ptr<ProcessGroup::Work> ProcessGroupNCCL::reduce(
    std::vector<at::Tensor>& tensors,
    const ReduceOptions& opts) {
  check_gpu_tensors(tensors);

  return collective(
      tensors,
      tensors,
      [&](at::Tensor& input,
          at::Tensor& output,
          ncclComm_t comm,
          at::cuda::CUDAStream& stream) {
        const auto root = opts.rootRank * tensors.size() + opts.rootTensor;
        return ncclReduce(
            input.data_ptr(),
            output.data_ptr(),
            input.numel(),
            getNcclDataType(input.scalar_type()),
            ncclOp[opts.reduceOp],
            root,
            comm,
            stream.stream());
      });
}

std::shared_ptr<ProcessGroup::Work> ProcessGroupNCCL::allgather(
    std::vector<std::vector<at::Tensor>>& outputTensors,
    std::vector<at::Tensor>& inputTensors,
    const AllgatherOptions& opts) {
  check_gpu_tensors(inputTensors);

  auto outputFlattened = 
      flatten_for_scatter_gather(outputTensors, inputTensors, size_, rank_, opts.noCopy);

  check_gpu_tensors(outputFlattened);

  return collective(inputTensors, outputFlattened,
    [&] (at::Tensor& input, at::Tensor& output,
         ncclComm_t comm, at::cuda::CUDAStream& stream) {
      c10::cuda::CUDACachingAllocator::recordStream(
        output.storage().data_ptr(), stream
      );
      return ncclAllGather(
        input.data_ptr(),
        output.data_ptr(),
        input.numel(),
        getNcclDataType(input.scalar_type()),
        comm,
        stream.stream()
      );
    },
    [&] (std::vector<at::cuda::CUDAStream>& ncclStreams) {},
    [&] (std::vector<at::cuda::CUDAStream>& ncclStreams) {
      // Copy the flattened output tensors to the outputs.
      for (size_t i = 0; i < outputTensors.size(); ++i) {
        at::cuda::CUDAStreamGuard guard(ncclStreams[i]);
        for (size_t j = 0; j < outputTensors[0].size(); ++j) {
          // Skip copy if it's in-place operation, where the input and
          // output tensors share the same storage
          if (outputFlattened[i][j].storage().is_alias_of(
              outputTensors[i][j].storage()) &&
              outputTensors[i][j].storage_offset() ==
              (outputTensors[i][0].storage_offset() +
              outputTensors[i][j].numel() * j)) {
             break;
          }
          // See [Sync Streams].
          c10::cuda::CUDACachingAllocator::recordStream(
            outputTensors[i][j].storage().data_ptr(), ncclStreams[i]);

          outputTensors[i][j].copy_(outputFlattened[i][j], true);
	}
      }
    });
}

std::shared_ptr<ProcessGroup::Work> ProcessGroupNCCL::allgather_coalesced(
    std::vector<std::vector<at::Tensor>>& /* unused */,
    std::vector<at::Tensor>& /* unused */,
    const AllgatherOptions& /* unused */) {
  throw std::runtime_error(
      "ProcessGroupNCCL does not support allgather_coalesced");
}

std::shared_ptr<ProcessGroup::Work> ProcessGroupNCCL::reduce_scatter(
    std::vector<at::Tensor>& outputTensors,
    std::vector<std::vector<at::Tensor>>& inputTensors,
    const ReduceScatterOptions& opts) {
  check_gpu_tensors(outputTensors);

  auto inputFlattened =
      flatten_for_scatter_gather(inputTensors, outputTensors, size_, rank_, opts.noCopy);
  check_gpu_tensors(inputFlattened);

  return collective(inputFlattened, outputTensors,
    [&] (at::Tensor& input, at::Tensor& output,
         ncclComm_t comm, at::cuda::CUDAStream& stream) {
      c10::cuda::CUDACachingAllocator::recordStream(
        output.storage().data_ptr(), stream
      );
      return ncclReduceScatter(
        input.data_ptr(),
        output.data_ptr(),
        output.numel(),
        getNcclDataType(input.scalar_type()),
        ncclOp[opts.reduceOp],
        comm,
        stream.stream()
      );
    },
    [&] (std::vector<at::cuda::CUDAStream>& ncclStreams) {
      // Copy the input tensors to the flattened inputs.
      for (size_t i = 0; i < inputTensors.size(); ++i) {
        at::cuda::CUDAStreamGuard guard(ncclStreams[i]);
        for (size_t j = 0; j < inputTensors[0].size(); ++j) {
          // Skip copy if it's in-place operation, where the input and
          // output tensors share the same storage
          if (inputFlattened[i][j].storage().is_alias_of(
              inputTensors[i][j].storage()) &&
              inputTensors[i][j].storage_offset() ==
              (inputTensors[i][0].storage_offset() +
              inputTensors[i][j].numel() * j)) {
             break;
          }
          // See [Sync Streams].
          c10::cuda::CUDACachingAllocator::recordStream(
            inputTensors[i][j].storage().data_ptr(), ncclStreams[i]);

          inputFlattened[i][j].copy_(inputTensors[i][j], true);
        }
      }
      },
      [&](std::vector<at::cuda::CUDAStream>& ncclStreams) {});
}

std::shared_ptr<ProcessGroup::Work> ProcessGroupNCCL::barrier(
    const BarrierOptions& opts) {
  std::vector<at::Device> devices;
  if (usedDeviceIdxs_.empty()) {
    // This means there is not yet a NCCL collective being called
    // Here we have to use the best guesses and will use a single GPU to call
    // allreduce to achieve barrier.
    // In case the multiple processes fall into the same node, we use rank to
    // ensure that each process is on a different GPU
    auto numGPUs = at::cuda::getNumGPUs();
    int16_t deviceIdx = static_cast<int16_t>(rank_ % numGPUs);
    devices.push_back(at::Device(at::DeviceType::CUDA, deviceIdx));
  } else {
    for (auto usedDeviceIdx : usedDeviceIdxs_) {
      devices.push_back(at::Device(at::DeviceType::CUDA, usedDeviceIdx));
    }
  }

  std::vector<at::Tensor> barrierTensors;
  barrierTensors.reserve(devices.size());

  at::cuda::OptionalCUDAGuard gpuGuard;
  for (auto& device : devices) {
    gpuGuard.set_index(device.index());
    barrierTensors.push_back(at::empty(
        {1},
        at::TensorOptions().device(at::DeviceType::CUDA).dtype(at::kByte)));
  }

  // All reduce to achieve the barrier
  auto work = allreduce(barrierTensors);

  // Work will take over barrierTensors
  auto ncclWork = dynamic_cast<ProcessGroupNCCL::WorkNCCL*>(work.get());
  TORCH_CHECK(ncclWork);
  ncclWork->barrierTensors_ = std::move(barrierTensors);

  return work;
}

std::shared_ptr<ProcessGroup::Work> ProcessGroupNCCL::alltoall_base(
    at::Tensor& outputTensor,
    at::Tensor& inputTensor,
    std::vector<int64_t>& outputSplitSizes,
    std::vector<int64_t>& inputSplitSizes,
    const AllToAllOptions& opts) {
  std::vector<at::Tensor> inputTensors = {inputTensor};
  std::vector<at::Tensor> outputTensors = {outputTensor};
  check_gpu_tensors(inputTensors);
  check_gpu_tensors(outputTensors);

  std::vector<int> send_lengths(size_);
  std::vector<int> recv_lengths(size_);
  std::vector<void*> send_ptrs(size_);
  std::vector<void*> recv_ptrs(size_);
  int typesize = at::elementSize(inputTensor.scalar_type());
  if (outputSplitSizes.size() == 0 && inputSplitSizes.size() == 0) {
    // We can use alltoall
    if ((outputTensor.numel() != inputTensor.numel()) ||
        (outputTensor.scalar_type() != inputTensor.scalar_type())) {
      throw std::runtime_error("Tensors are not equal in size or data type");
    }
    if (outputTensor.size(0) % size_ != 0) {
      throw std::runtime_error(
          "Tensor's dim 0 does not divide equally across group size");
    }

    int len = inputTensor.numel() / size_;

    for (int i = 0; i < size_; i++) {
      send_lengths[i] = len;
      recv_lengths[i] = len;
      send_ptrs[i] = (char*)inputTensor.data_ptr() + typesize * len * i;
      recv_ptrs[i] = (char*)outputTensor.data_ptr() + typesize * len * i;
    }

  } else {
    // Need alltoallv
    checkSplitSizes(inputSplitSizes, inputTensor, size_);
    checkSplitSizes(outputSplitSizes, outputTensor, size_);

    int row_size = inputTensor.numel() / inputTensor.size(0);
    int64_t inputOffset = 0, outputOffset = 0;

    for (int i = 0; i < size_; i++) {
      send_ptrs[i] = (char*)inputTensor.data_ptr() + inputOffset * typesize;
      recv_ptrs[i] = (char*)outputTensor.data_ptr() + outputOffset * typesize;
      send_lengths[i] = row_size * inputSplitSizes[i];
      recv_lengths[i] = row_size * outputSplitSizes[i];
      inputOffset += send_lengths[i];
      outputOffset += recv_lengths[i];
    }
  }

  return batchedp2p(
      send_ptrs,
      send_lengths,
      recv_ptrs,
      recv_lengths,
      getNcclDataType(inputTensor.scalar_type()),
      inputTensors,
      outputTensors);
}

std::shared_ptr<ProcessGroup::Work> ProcessGroupNCCL::alltoall(
    std::vector<at::Tensor>& outputTensors,
    std::vector<at::Tensor>& inputTensors,
    const AllToAllOptions& opts) {
  if (inputTensors.size() != size_ || outputTensors.size() != size_) {
    throw std::runtime_error(
        "Number of input or output tensors are not equal to group size");
  }
  check_gpu_tensors_multi(inputTensors, true);
  check_gpu_tensors_multi(outputTensors, true);
  std::vector<int> send_lengths(size_);
  std::vector<int> recv_lengths(size_);
  std::vector<void*> send_ptrs(size_);
  std::vector<void*> recv_ptrs(size_);

  for (int i = 0; i < size_; i++) {
    send_lengths[i] = inputTensors[i].numel();
    recv_lengths[i] = outputTensors[i].numel();
    send_ptrs[i] = inputTensors[i].data_ptr();
    recv_ptrs[i] = outputTensors[i].data_ptr();
  }
  std::vector<at::Tensor> inputTensor = {inputTensors[0]};
  std::vector<at::Tensor> outputTensor = {outputTensors[0]};
  return batchedp2p(
      send_ptrs,
      send_lengths,
      recv_ptrs,
      recv_lengths,
      getNcclDataType(inputTensors[0].scalar_type()),
      inputTensor,
      outputTensor);
}

std::shared_ptr<ProcessGroup::Work> ProcessGroupNCCL::gather(
    std::vector<std::vector<at::Tensor>>& /* unused */,
    std::vector<at::Tensor>& /* unused */,
    const GatherOptions& /* unused */) {
  throw std::runtime_error("ProcessGroupNCCL does not support gather");
}

std::shared_ptr<ProcessGroup::Work> ProcessGroupNCCL::scatter(
    std::vector<at::Tensor>& /* unused */,
    std::vector<std::vector<at::Tensor>>& /* unused */,
    const ScatterOptions& /* unused */) {
  throw std::runtime_error("ProcessGroupNCCL does not support scatter");
}

std::shared_ptr<ProcessGroup::Work> ProcessGroupNCCL::send(
    std::vector<at::Tensor>& /* unused */,
    int /* unused */,
    int /* unused */) {
  throw std::runtime_error("ProcessGroupNCCL does not support send");
}

std::shared_ptr<ProcessGroup::Work> ProcessGroupNCCL::recv(
    std::vector<at::Tensor>& /* unused */,
    int /* unused */,
    int /* unused */) {
  throw std::runtime_error("ProcessGroupNCCL does not support recv");
}

std::shared_ptr<ProcessGroup::Work> ProcessGroupNCCL::recvAnysource(
    std::vector<at::Tensor>& /* unused */,
    int /* unused */) {
  throw std::runtime_error("ProcessGroupNCCL does not support recv");
}

std::shared_ptr<ProcessGroup::Work> ProcessGroupNCCL::allgather_base(
    at::Tensor& /*unused */,
    at::Tensor& /*unused */,
    const AllgatherOptions& /*unused */) {
  throw std::runtime_error(
      "no support for allgather_base in NCCL process group");
}

} // namespace c10d
