/* Copyright (c) 2016 PaddlePaddle Authors. All Rights Reserved.

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License. */

#pragma once

#include <algorithm>
#include <initializer_list>
#include <memory>
#include <mutex>  // NOLINT
#include <utility>
#include <vector>

#include "glog/logging.h"
#include "paddle/fluid/framework/details/cow_ptr.h"
#include "paddle/fluid/memory/allocation/allocator.h"
#include "paddle/utils/none.h"
#include "paddle/utils/optional.h"

namespace paddle {
namespace framework {

inline paddle::optional<platform::CUDAPlace> OptionalCUDAPlace(
    const paddle::memory::allocation::AllocationPtr &gpu_) {
  return gpu_ == nullptr
             ? paddle::none
             : paddle::optional<platform::CUDAPlace>(
                   BOOST_GET_CONST(platform::CUDAPlace, gpu_->place()));
}

// Vector<T> implements the std::vector interface, and can get Data or
// MutableData from any place. The data will be synced implicitly inside.
template <typename T>
class Vector {
 public:
  using value_type = T;
  using iterator = typename std::vector<T>::iterator;
  using const_iterator = typename std::vector<T>::const_iterator;

 private:
  // The actual class to implement vector logic
  class VectorData {
   public:
    VectorData() : flag_(kDataInCPU) {}
    VectorData(size_t count, const T &value)
        : cpu_(count, value), flag_(kDataInCPU) {}
    VectorData(std::initializer_list<T> init) : cpu_(init), flag_(kDataInCPU) {}
    template <typename U>
    explicit VectorData(const std::vector<U> &dat)
        : cpu_(dat), flag_(kDataInCPU) {}
    ~VectorData() {}

    VectorData(const VectorData &o) {
      o.ImmutableCPU();
      cpu_ = o.cpu_;
      flag_ = kDataInCPU;
    }

    VectorData &operator=(const VectorData &o) {
      o.ImmutableCPU();
      cpu_ = o.cpu_;
      flag_ = kDataInCPU;
      return *this;
    }

    T &operator[](size_t i) {
      MutableCPU();
      return cpu_[i];
    }

    const T &operator[](size_t i) const {
      ImmutableCPU();
      return cpu_[i];
    }

    size_t size() const { return cpu_.size(); }

    iterator begin() {
      MutableCPU();
      return cpu_.begin();
    }

    iterator end() {
      MutableCPU();
      return cpu_.end();
    }

    T &front() {
      MutableCPU();
      return cpu_.front();
    }

    T &back() {
      MutableCPU();
      return cpu_.back();
    }

    const_iterator begin() const {
      ImmutableCPU();
      return cpu_.begin();
    }

    const_iterator end() const {
      ImmutableCPU();
      return cpu_.end();
    }

    const T &back() const {
      ImmutableCPU();
      return cpu_.back();
    }

    T *data() { return &(*this)[0]; }

    const T *data() const { return &(*this)[0]; }

    const T &front() const {
      ImmutableCPU();
      return cpu_.front();
    }

    // assign this from iterator.
    // NOTE: the iterator must support `end-begin`
    template <typename Iter>
    void assign(Iter begin, Iter end) {
      MutableCPU();
      cpu_.assign(begin, end);
    }

    // push_back. If the previous capacity is not enough, the memory will
    // double.
    void push_back(T elem) {
      MutableCPU();
      cpu_.push_back(elem);
    }

    // extend a vector by iterator.
    // NOTE: the iterator must support end-begin
    template <typename It>
    void Extend(It begin, It end) {
      MutableCPU();
      auto out_it = std::back_inserter<std::vector<T>>(this->cpu_);
      std::copy(begin, end, out_it);
    }

    // resize the vector
    void resize(size_t size) {
      MutableCPU();
      cpu_.resize(size);
    }

    // get cuda ptr. immutable
    const T *CUDAData(platform::Place place) const {
      PADDLE_ENFORCE_EQ(
          platform::is_gpu_place(place), true,
          platform::errors::Unavailable(
              "Place mismatch, CUDA Data must be on CUDA place."));
      ImmutableCUDA(place);
      return reinterpret_cast<T *>(gpu_->ptr());
    }

    // get cuda ptr. mutable
    T *CUDAMutableData(platform::Place place) {
      const T *ptr = CUDAData(place);
      flag_ = kDirty | kDataInCUDA;
      return const_cast<T *>(ptr);
    }

    // clear
    void clear() {
      cpu_.clear();
      flag_ = kDirty | kDataInCPU;
    }

    size_t capacity() const { return cpu_.capacity(); }

    // reserve data
    void reserve(size_t size) const { cpu_.reserve(size); }

    // implicit cast operator. Vector can be cast to std::vector implicitly.
    operator std::vector<T>() const {
      ImmutableCPU();
      return cpu_;
    }

    bool operator==(const VectorData &other) const {
      ImmutableCPU();
      other.ImmutableCPU();
      return cpu_ == other.cpu_;
    }

    std::mutex &Mutex() const { return mtx_; }

    paddle::optional<platform::CUDAPlace> CUDAPlace() const {
      return OptionalCUDAPlace(gpu_);
    }

   private:
    enum DataFlag {
      kDataInCPU = 0x01,
      kDataInCUDA = 0x02,
      // kDirty means the data has been changed in one device.
      kDirty = 0x10
    };

    void CopyToCPU() const;

    void MutableCPU() {
      if (IsInCUDA() && IsDirty()) {
        CopyToCPU();
      }
      flag_ = kDirty | kDataInCPU;
    }

    void ImmutableCUDA(platform::Place place) const {
      if (IsDirty()) {
        if (IsInCPU()) {
          CopyCPUDataToCUDA(place);
          UnsetFlag(kDirty);
          SetFlag(kDataInCUDA);
        } else if (IsInCUDA() && !(place == gpu_->place())) {
          PADDLE_THROW(
              platform::errors::Unavailable("Unexpected data place mismatch."));
          // Still dirty
        } else {
          // Dirty && DataInCUDA && Device is same
          // Do nothing
        }
      } else {
        if (!IsInCUDA()) {
          // Even data is not dirty. However, data is not in CUDA. Copy data.
          CopyCPUDataToCUDA(place);
          SetFlag(kDataInCUDA);
        } else if (!(place == gpu_->place())) {
          PADDLE_THROW(
              platform::errors::Unavailable("Unexpected data place mismatch."));
        } else {
          // Not Dirty && DataInCUDA && Device is same
          // Do nothing.
        }
      }
    }

    void CopyCPUDataToCUDA(const platform::Place &place) const;

    void ImmutableCPU() const {
      if (IsDirty() && !IsInCPU()) {  // If data has been changed in CUDA, or
                                      // CPU has no data.
        CopyToCPU();
        UnsetFlag(kDirty);
      }
      SetFlag(kDataInCPU);
    }

    void UnsetFlag(int flag) const { flag_ &= ~flag; }
    void SetFlag(int flag) const { flag_ |= flag; }

    bool IsDirty() const { return flag_ & kDirty; }

    bool IsInCUDA() const { return flag_ & kDataInCUDA; }

    bool IsInCPU() const { return flag_ & kDataInCPU; }

    mutable std::vector<T> cpu_;
    mutable paddle::memory::allocation::AllocationPtr gpu_;
    mutable size_t gpu_memory_size_{0};
    mutable int flag_;

    mutable std::mutex mtx_;
  };

 public:
  // Default ctor. Create empty Vector
  Vector() : m_(new VectorData()) {}

  // Fill vector with value. The vector size is `count`.
  explicit Vector(size_t count, const T &value = T())
      : m_(new VectorData(count, value)) {}

  // Ctor with init_list
  Vector(std::initializer_list<T> init) : m_(new VectorData(init)) {}

  // implicit cast from std::vector.
  template <typename U>
  Vector(const std::vector<U> &dat) : m_(new VectorData(dat)) {  // NOLINT
  }

  // Copy ctor
  Vector(const Vector<T> &other) { m_ = other.m_; }

  // Copy operator
  Vector<T> &operator=(const Vector<T> &other) {
    m_ = other.m_;
    return *this;
  }

  // Move ctor
  Vector(Vector<T> &&other) { m_ = std::move(other.m_); }

  // CPU data access method. Mutable.
  T &operator[](size_t i) { return (*m_.MutableData())[i]; }

  // CPU data access method. Immutable.
  const T &operator[](size_t i) const { return m_.Data()[i]; }

  // std::vector iterator methods. Based on CPU data access method
  size_t size() const { return m_.Data().size(); }

  iterator begin() { return m_.MutableData()->begin(); }

  iterator end() { return m_.MutableData()->end(); }

  T &front() { return m_.MutableData()->front(); }

  T &back() { return m_.MutableData()->back(); }

  const_iterator begin() const { return m_.Data().begin(); }

  const_iterator end() const { return m_.Data().end(); }

  const_iterator cbegin() const { return begin(); }

  const_iterator cend() const { return end(); }

  const T &back() const { return m_.Data().back(); }

  T *data() { return m_.MutableData()->data(); }

  const T *data() const { return m_.Data().data(); }

  const T &front() const { return m_.Data().front(); }
  // end of std::vector iterator methods

  // assign this from iterator.
  // NOTE: the iterator must support `end-begin`
  template <typename Iter>
  void assign(Iter begin, Iter end) {
    m_.MutableData()->assign(begin, end);
  }

  // push_back. If the previous capacity is not enough, the memory will
  // double.
  void push_back(T elem) { m_.MutableData()->push_back(elem); }

  // extend a vector by iterator.
  // NOTE: the iterator must support end-begin
  template <typename It>
  void Extend(It begin, It end) {
    m_.MutableData()->Extend(begin, end);
  }

  // resize the vector
  void resize(size_t size) {
    if (m_.Data().size() != size) {
      m_.MutableData()->resize(size);
    }
  }

  // get cuda ptr. immutable
  const T *CUDAData(platform::Place place) const {
    {
      auto &mtx = m_.Data().Mutex();
      std::lock_guard<std::mutex> guard(mtx);
      auto cuda_place = m_.Data().CUDAPlace();
      if (cuda_place == paddle::none ||
          cuda_place == BOOST_GET(platform::CUDAPlace, place)) {
        return m_.Data().CUDAData(place);
      }
    }
    // If m_ contains CUDAData in a different place. Detach manually.
    m_.Detach();
    return CUDAData(place);
  }

  // get cuda ptr. mutable
  T *CUDAMutableData(platform::Place place) {
    {
      auto &mtx = m_.Data().Mutex();
      std::lock_guard<std::mutex> guard(mtx);
      auto cuda_place = m_.Data().CUDAPlace();
      if (cuda_place == paddle::none ||
          cuda_place == BOOST_GET(platform::CUDAPlace, place)) {
        return m_.MutableData()->CUDAMutableData(place);
      }
    }
    // If m_ contains CUDAData in a different place. Detach manually.
    m_.Detach();
    return CUDAMutableData(place);
  }

  // clear
  void clear() { m_.MutableData()->clear(); }

  size_t capacity() const { return m_.Data().capacity(); }

  // reserve data
  void reserve(size_t size) { m_.Data().reserve(size); }

  // the unify method to access CPU or CUDA data. immutable.
  const T *Data(platform::Place place) const {
    if (platform::is_gpu_place(place)) {
      return CUDAData(place);
    } else {
      return data();
    }
  }

  // the unify method to access CPU or CUDA data. mutable.
  T *MutableData(platform::Place place) {
    if (platform::is_gpu_place(place)) {
      return CUDAMutableData(place);
    } else {
      return data();
    }
  }

  // implicit cast operator. Vector can be cast to std::vector implicitly.
  operator std::vector<T>() const { return m_.Data(); }

  bool operator==(const Vector<T> &other) const {
    if (size() != other.size()) return false;
    auto it1 = cbegin();
    auto it2 = other.cbegin();
    for (; it1 < cend(); ++it1, ++it2) {
      if (*it1 != *it2) {
        return false;
      }
    }
    return true;
  }

  const void *Handle() const { return &m_.Data(); }

 private:
  // Vector is an COW object.
  mutable details::COWPtr<VectorData> m_;
};

};  // namespace framework
}  // namespace paddle
