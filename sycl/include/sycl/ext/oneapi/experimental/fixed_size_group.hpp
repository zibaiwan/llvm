//==--- fixed_size_group.hpp --- SYCL extension for non-uniform groups -----==//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#pragma once

#include <sycl/ext/oneapi/experimental/non_uniform_groups.hpp>

namespace sycl {
__SYCL_INLINE_VER_NAMESPACE(_V1) {
namespace ext::oneapi::experimental {

template <size_t PartitionSize, typename ParentGroup> class fixed_size_group;

template <size_t PartitionSize, typename Group>
inline std::enable_if_t<sycl::is_group_v<std::decay_t<Group>> &&
                            std::is_same_v<Group, sycl::sub_group>,
                        fixed_size_group<PartitionSize, Group>>
get_fixed_size_group(Group group);

template <size_t PartitionSize, typename ParentGroup> class fixed_size_group {
public:
  using id_type = id<1>;
  using range_type = range<1>;
  using linear_id_type = typename ParentGroup::linear_id_type;
  static constexpr int dimensions = 1;
  static constexpr sycl::memory_scope fence_scope = ParentGroup::fence_scope;

  id_type get_group_id() const {
#ifdef __SYCL_DEVICE_ONLY__
    return __spirv_SubgroupLocalInvocationId() / PartitionSize;
#else
    throw runtime_error("Non-uniform groups are not supported on host device.",
                        PI_ERROR_INVALID_DEVICE);
#endif
  }

  id_type get_local_id() const {
#ifdef __SYCL_DEVICE_ONLY__
    return __spirv_SubgroupLocalInvocationId() % PartitionSize;
#else
    throw runtime_error("Non-uniform groups are not supported on host device.",
                        PI_ERROR_INVALID_DEVICE);
#endif
  }

  range_type get_group_range() const {
#ifdef __SYCL_DEVICE_ONLY__
    return __spirv_SubgroupMaxSize() / PartitionSize;
#else
    throw runtime_error("Non-uniform groups are not supported on host device.",
                        PI_ERROR_INVALID_DEVICE);
#endif
  }

  range_type get_local_range() const {
#ifdef __SYCL_DEVICE_ONLY__
    return PartitionSize;
#else
    throw runtime_error("Non-uniform groups are not supported on host device.",
                        PI_ERROR_INVALID_DEVICE);
#endif
  }

  linear_id_type get_group_linear_id() const {
#ifdef __SYCL_DEVICE_ONLY__
    return static_cast<linear_id_type>(get_group_id()[0]);
#else
    throw runtime_error("Non-uniform groups are not supported on host device.",
                        PI_ERROR_INVALID_DEVICE);
#endif
  }

  linear_id_type get_local_linear_id() const {
#ifdef __SYCL_DEVICE_ONLY__
    return static_cast<linear_id_type>(get_local_id()[0]);
#else
    throw runtime_error("Non-uniform groups are not supported on host device.",
                        PI_ERROR_INVALID_DEVICE);
#endif
  }

  linear_id_type get_group_linear_range() const {
#ifdef __SYCL_DEVICE_ONLY__
    return static_cast<linear_id_type>(get_group_range()[0]);
#else
    throw runtime_error("Non-uniform groups are not supported on host device.",
                        PI_ERROR_INVALID_DEVICE);
#endif
  }

  linear_id_type get_local_linear_range() const {
#ifdef __SYCL_DEVICE_ONLY__
    return static_cast<linear_id_type>(get_local_range()[0]);
#else
    throw runtime_error("Non-uniform groups are not supported on host device.",
                        PI_ERROR_INVALID_DEVICE);
#endif
  }

  bool leader() const {
#ifdef __SYCL_DEVICE_ONLY__
    return get_local_linear_id() == 0;
#else
    throw runtime_error("Non-uniform groups are not supported on host device.",
                        PI_ERROR_INVALID_DEVICE);
#endif
  }

protected:
  fixed_size_group() {}

  friend fixed_size_group<PartitionSize, ParentGroup>
  get_fixed_size_group<PartitionSize, ParentGroup>(ParentGroup g);
};

template <size_t PartitionSize, typename Group>
inline std::enable_if_t<sycl::is_group_v<std::decay_t<Group>> &&
                            std::is_same_v<Group, sycl::sub_group>,
                        fixed_size_group<PartitionSize, Group>>
get_fixed_size_group(Group group) {
  (void)group;
#ifdef __SYCL_DEVICE_ONLY__
  return fixed_size_group<PartitionSize, sycl::sub_group>();
#else
  throw runtime_error("Non-uniform groups are not supported on host device.",
                      PI_ERROR_INVALID_DEVICE);
#endif
}

template <size_t PartitionSize, typename ParentGroup>
struct is_user_constructed_group<fixed_size_group<PartitionSize, ParentGroup>>
    : std::true_type {};

} // namespace ext::oneapi::experimental
} // __SYCL_INLINE_VER_NAMESPACE(_V1)
} // namespace sycl
