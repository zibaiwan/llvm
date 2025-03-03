//==--- kernel_program_cache.hpp - Cache for kernel and program -*- C++-*---==//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#pragma once

#include <detail/platform_impl.hpp>
#include <sycl/detail/common.hpp>
#include <sycl/detail/locked.hpp>
#include <sycl/detail/os_util.hpp>
#include <sycl/detail/pi.hpp>
#include <sycl/detail/util.hpp>

#include <atomic>
#include <condition_variable>
#include <map>
#include <mutex>
#include <type_traits>

// For testing purposes
class MockKernelProgramCache;

namespace sycl {
__SYCL_INLINE_VER_NAMESPACE(_V1) {
namespace detail {
class context_impl;
class KernelProgramCache {
public:
  /// Denotes build error data. The data is filled in from sycl::exception
  /// class instance.
  struct BuildError {
    std::string Msg;
    pi_int32 Code;

    bool isFilledIn() const { return !Msg.empty(); }
  };

  /// Denotes the state of a build.
  enum BuildState { BS_InProgress, BS_Done, BS_Failed };

  /// Denotes pointer to some entity with its general state and build error.
  /// The pointer is not null if and only if the entity is usable.
  /// State of the entity is provided by the user of cache instance.
  /// Currently there is only a single user - ProgramManager class.
  template <typename T> struct BuildResult {
    std::atomic<T *> Ptr;
    T Val;
    std::atomic<BuildState> State;
    BuildError Error;

    /// Condition variable to signal that build result is ready.
    /// A per-object (i.e. kernel or program) condition variable is employed
    /// instead of global one in order to eliminate the following deadlock.
    /// A thread T1 awaiting for build result BR1 to be ready may be awakened by
    /// another thread (due to use of global condition variable), which made
    /// build result BR2 ready. Meanwhile, a thread which made build result BR1
    /// ready notifies everyone via a global condition variable and T1 will skip
    /// this notification as it's not in condition_variable::wait()'s wait cycle
    /// now. Now T1 goes to sleep again and will wait until either a spurious
    /// wake-up or another thread will wake it up.
    std::condition_variable MBuildCV;
    /// A mutex to be employed along with MBuildCV.
    std::mutex MBuildResultMutex;

    BuildResult(T *P, BuildState S) : Ptr{P}, State{S}, Error{"", 0} {}
  };

  using ProgramWithBuildStateT = BuildResult<RT::PiProgram>;
  using ProgramCacheKeyT = std::pair<std::pair<SerializedObj, std::uintptr_t>,
                                     std::pair<RT::PiDevice, std::string>>;
  using CommonProgramKeyT = std::pair<std::uintptr_t, RT::PiDevice>;

  struct ProgramCache {
    std::map<ProgramCacheKeyT, ProgramWithBuildStateT> Cache;
    std::multimap<CommonProgramKeyT, ProgramCacheKeyT> KeyMap;

    size_t size() const noexcept { return Cache.size(); }
  };

  using ContextPtr = context_impl *;

  using KernelArgMaskPairT = std::pair<RT::PiKernel, const KernelArgMask *>;
  using KernelByNameT = std::map<std::string, BuildResult<KernelArgMaskPairT>>;
  using KernelCacheT = std::map<RT::PiProgram, KernelByNameT>;

  using KernelFastCacheKeyT =
      std::tuple<SerializedObj, OSModuleHandle, RT::PiDevice, std::string,
                 std::string>;
  using KernelFastCacheValT = std::tuple<RT::PiKernel, std::mutex *,
                                         const KernelArgMask *, RT::PiProgram>;
  using KernelFastCacheT = std::map<KernelFastCacheKeyT, KernelFastCacheValT>;

  ~KernelProgramCache();

  void setContextPtr(const ContextPtr &AContext) { MParentContext = AContext; }

  Locked<ProgramCache> acquireCachedPrograms() {
    return {MCachedPrograms, MProgramCacheMutex};
  }

  Locked<KernelCacheT> acquireKernelsPerProgramCache() {
    return {MKernelsPerProgramCache, MKernelsPerProgramCacheMutex};
  }

  std::pair<ProgramWithBuildStateT *, bool>
  getOrInsertProgram(const ProgramCacheKeyT &CacheKey) {
    auto LockedCache = acquireCachedPrograms();
    auto &ProgCache = LockedCache.get();
    auto Inserted = ProgCache.Cache.emplace(
        std::piecewise_construct, std::forward_as_tuple(CacheKey),
        std::forward_as_tuple(nullptr, BS_InProgress));
    if (Inserted.second) {
      // Save reference between the common key and the full key.
      CommonProgramKeyT CommonKey =
          std::make_pair(CacheKey.first.second, CacheKey.second.first);
      ProgCache.KeyMap.emplace(std::piecewise_construct,
                               std::forward_as_tuple(CommonKey),
                               std::forward_as_tuple(CacheKey));
    }
    return std::make_pair(&Inserted.first->second, Inserted.second);
  }

  std::pair<BuildResult<KernelArgMaskPairT> *, bool>
  getOrInsertKernel(RT::PiProgram Program, const std::string &KernelName) {
    auto LockedCache = acquireKernelsPerProgramCache();
    auto &Cache = LockedCache.get()[Program];
    auto Inserted = Cache.emplace(
        std::piecewise_construct, std::forward_as_tuple(KernelName),
        std::forward_as_tuple(nullptr, BS_InProgress));
    return std::make_pair(&Inserted.first->second, Inserted.second);
  }

  template <typename T, class Predicate>
  void waitUntilBuilt(BuildResult<T> &BR, Predicate Pred) const {
    std::unique_lock<std::mutex> Lock(BR.MBuildResultMutex);

    BR.MBuildCV.wait(Lock, Pred);
  }

  template <typename ExceptionT, typename RetT>
  RetT *waitUntilBuilt(BuildResult<RetT> *BuildResult) {
    // Any thread which will find nullptr in cache will wait until the pointer
    // is not null anymore.
    waitUntilBuilt(*BuildResult, [BuildResult]() {
      int State = BuildResult->State.load();
      return State == BuildState::BS_Done || State == BuildState::BS_Failed;
    });

    if (BuildResult->Error.isFilledIn()) {
      const BuildError &Error = BuildResult->Error;
      throw ExceptionT(Error.Msg, Error.Code);
    }

    return BuildResult->Ptr.load();
  }

  template <typename T> void notifyAllBuild(BuildResult<T> &BR) const {
    BR.MBuildCV.notify_all();
  }

  template <typename KeyT>
  KernelFastCacheValT tryToGetKernelFast(KeyT &&CacheKey) {
    std::unique_lock<std::mutex> Lock(MKernelFastCacheMutex);
    auto It = MKernelFastCache.find(CacheKey);
    if (It != MKernelFastCache.end()) {
      return It->second;
    }
    return std::make_tuple(nullptr, nullptr, nullptr, nullptr);
  }

  template <typename KeyT, typename ValT>
  void saveKernel(KeyT &&CacheKey, ValT &&CacheVal) {
    std::unique_lock<std::mutex> Lock(MKernelFastCacheMutex);
    // if no insertion took place, thus some other thread has already inserted
    // smth in the cache
    MKernelFastCache.emplace(CacheKey, CacheVal);
  }

  /// Clears cache state.
  ///
  /// This member function should only be used in unit tests.
  void reset() {
    MCachedPrograms = ProgramCache{};
    MKernelsPerProgramCache = KernelCacheT{};
    MKernelFastCache = KernelFastCacheT{};
  }

private:
  std::mutex MProgramCacheMutex;
  std::mutex MKernelsPerProgramCacheMutex;

  ProgramCache MCachedPrograms;
  KernelCacheT MKernelsPerProgramCache;
  ContextPtr MParentContext;

  std::mutex MKernelFastCacheMutex;
  KernelFastCacheT MKernelFastCache;
  friend class ::MockKernelProgramCache;
};
} // namespace detail
} // __SYCL_INLINE_VER_NAMESPACE(_V1)
} // namespace sycl
