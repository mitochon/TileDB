/**
 * @file   result_cell_slab_iter.cc
 *
 * @section LICENSE
 *
 * The MIT License
 *
 * @copyright Copyright (c) 2017-2019 TileDB, Inc.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 *
 * @section DESCRIPTION
 *
 * This file implements class ResultCellSlabIter.
 */

#include "tiledb/sm/query/result_cell_slab_iter.h"
#include "tiledb/sm/array/array.h"
#include "tiledb/sm/misc/logger.h"

#include <cassert>
#include <iostream>
#include <list>

/* ****************************** */
/*             MACROS             */
/* ****************************** */

#define MIN(a, b) ((a) < (b) ? (a) : (b))
#define MAX(a, b) ((a) > (b) ? (a) : (b))

namespace tiledb {
namespace sm {

/* ****************************** */
/*   CONSTRUCTORS & DESTRUCTORS   */
/* ****************************** */

template <class T>
ResultCellSlabIter<T>::ResultCellSlabIter(
    const Subarray* subarray,
    std::map<const T*, ResultSpaceTile<T>>* result_space_tiles,
    std::vector<ResultCoords<T>>* result_coords,
    uint64_t result_coords_pos)
    : result_space_tiles_(result_space_tiles)
    , result_coords_(result_coords)
    , result_coords_pos_(result_coords_pos)
    , init_result_coords_pos_(result_coords_pos) {
  domain_ = (subarray != nullptr) ?
                subarray->array()->array_schema()->domain() :
                nullptr;
  layout_ = (subarray != nullptr) ? subarray->layout() : Layout::ROW_MAJOR;
  cell_slab_iter_ = CellSlabIter<T>(subarray);
  end_ = true;
  compute_cell_offsets();
}

/* ****************************** */
/*               API              */
/* ****************************** */

template <class T>
Status ResultCellSlabIter<T>::begin() {
  end_ = true;
  RETURN_NOT_OK(cell_slab_iter_.begin());
  result_coords_pos_ = init_result_coords_pos_;
  update_result_cell_slab();

  return Status::Ok();
}

template <class T>
ResultCellSlab ResultCellSlabIter<T>::result_cell_slab() const {
  assert(result_cell_slabs_pos_ < result_cell_slabs_.size());
  return result_cell_slabs_[result_cell_slabs_pos_];
}

template <class T>
void ResultCellSlabIter<T>::operator++() {
  // Get one result cell slab from the temporary ones
  ++result_cell_slabs_pos_;
  if (result_cell_slabs_pos_ >= result_cell_slabs_.size()) {
    // Advance the cell slab iter and get a new vector of result slabs
    ++cell_slab_iter_;
    update_result_cell_slab();
  }
}

/* ****************************** */
/*          PRIVATE METHODS       */
/* ****************************** */

template <class T>
void ResultCellSlabIter<T>::compute_cell_offsets() {
  if (domain_ == nullptr)
    return;

  assert(
      domain_->cell_order() == Layout::ROW_MAJOR ||
      domain_->cell_order() == Layout::COL_MAJOR);

  if (domain_->cell_order() == Layout::ROW_MAJOR)
    compute_cell_offsets_row();
  else  // COL-MAJOR
    compute_cell_offsets_col();
}

template <class T>
void ResultCellSlabIter<T>::compute_cell_offsets_col() {
  assert(std::is_integral<T>::value);
  auto dim_num = domain_->dim_num();
  auto tile_extents = (const T*)domain_->tile_extents();
  cell_offsets_.reserve(dim_num);

  cell_offsets_.push_back(1);
  for (unsigned int i = 1; i < dim_num; ++i)
    cell_offsets_.push_back(cell_offsets_.back() * tile_extents[i - 1]);
}

template <class T>
void ResultCellSlabIter<T>::compute_cell_offsets_row() {
  assert(std::is_integral<T>::value);
  auto dim_num = domain_->dim_num();
  auto tile_extents = (const T*)domain_->tile_extents();
  cell_offsets_.reserve(dim_num);

  cell_offsets_.push_back(1);
  if (dim_num > 1) {
    for (unsigned int i = dim_num - 2;; --i) {
      cell_offsets_.push_back(cell_offsets_.back() * tile_extents[i + 1]);
      if (i == 0)
        break;
    }
  }

  std::reverse(cell_offsets_.begin(), cell_offsets_.end());
}

template <class T>
void ResultCellSlabIter<T>::compute_cell_slab_start(
    const T* cell_slab_coords,
    const std::vector<T>& tile_start_coords,
    uint64_t* start) {
  auto dim_num = domain_->dim_num();

  // Compute start
  *start = 0;
  for (unsigned i = 0; i < dim_num; ++i)
    *start += (cell_slab_coords[i] - tile_start_coords[i]) * cell_offsets_[i];
}

template <class T>
void ResultCellSlabIter<T>::compute_cell_slab_overlap(
    const CellSlab<T>& cell_slab,
    const T* frag_domain,
    std::vector<T>* slab_overlap,
    uint64_t* overlap_length,
    unsigned* overlap_type) {
  auto dim_num = domain_->dim_num();
  assert(slab_overlap->size() == dim_num);
  unsigned slab_dim = (layout_ == Layout::ROW_MAJOR) ? dim_num - 1 : 0;
  T slab_end, slab_start;
  slab_start = cell_slab.coords_[slab_dim];
  slab_end = slab_start + cell_slab.length_ - 1;

  // Check if there is any overlap
  for (unsigned i = 0; i < dim_num; ++i) {
    if (i == slab_dim) {
      if (slab_end < frag_domain[2 * i] ||
          slab_start > frag_domain[2 * i + 1]) {
        *overlap_type = 0;
        *overlap_length = 0;
        return;
      }
    } else if (
        cell_slab.coords_[i] < frag_domain[2 * i] ||
        cell_slab.coords_[i] > frag_domain[2 * i + 1]) {
      *overlap_type = 0;
      *overlap_length = 0;
      return;
    }
  }

  // There is some overlap
  T overlap_start = MAX(slab_start, frag_domain[2 * slab_dim]);
  T overlap_end = MIN(slab_end, frag_domain[2 * slab_dim + 1]);
  *slab_overlap = cell_slab.coords_;
  (*slab_overlap)[slab_dim] = overlap_start;
  *overlap_length = overlap_end - overlap_start + 1;
  *overlap_type = (*overlap_length == cell_slab.length_) ? 1 : 2;
}

template <class T>
void ResultCellSlabIter<T>::compute_result_cell_slabs(
    const CellSlab<T>& cell_slab) {
  // Find the result space tile
  auto it = result_space_tiles_->find(cell_slab.tile_coords_);
  assert(it != result_space_tiles_->end());
  auto& result_space_tile = it->second;

  // Note: this functions assumes that `result_coords_` are certain
  // results (i.e., appropriate filtering has already taken place).
  // Only the valid result coordinates are considered (non-valid
  // coordinates are the filtered ones).

  auto dim_num = domain_->dim_num();
  unsigned slab_dim = (layout_ == Layout::ROW_MAJOR) ? dim_num - 1 : 0;
  CellSlab<T> cell_slab_copy = cell_slab;
  auto slab_start = cell_slab_copy.coords_[slab_dim];
  auto slab_end = (T)(slab_start + cell_slab.length_ - 1);
  bool must_break = false;

  size_t i;
  for (; result_coords_pos_ < result_coords_->size(); ++result_coords_pos_) {
    // For easy reference
    i = result_coords_pos_;

    // Ignore if the result coordinates are invalid
    if (!(*result_coords_)[i].valid_)
      continue;

    // Check overlap
    for (unsigned d = 0; d < dim_num; ++d) {
      if (d != slab_dim) {
        // No overlap
        if ((*result_coords_)[i].coords_[d] != cell_slab_copy.coords_[d]) {
          must_break = true;
          break;
        }
      } else if (
          (*result_coords_)[i].coords_[d] < slab_start ||
          (*result_coords_)[i].coords_[d] > slab_end) {
        must_break = true;
        break;
      }
    }

    if (must_break)
      break;

    // Add left slab
    if ((*result_coords_)[i].coords_[slab_dim] > slab_start) {
      cell_slab_copy.length_ = (*result_coords_)[i].coords_[slab_dim] -
                               cell_slab_copy.coords_[slab_dim];
      compute_result_cell_slabs_dense(cell_slab_copy, &result_space_tile);
    }

    // Add result
    result_cell_slabs_.emplace_back(
        (*result_coords_)[i].tile_, (*result_coords_)[i].pos_, 1);

    // Update cell slab copy
    cell_slab_copy.coords_[slab_dim] =
        (*result_coords_)[i].coords_[slab_dim] + 1;
    cell_slab_copy.length_ = slab_end - cell_slab_copy.coords_[slab_dim] + 1;
    slab_start = cell_slab_copy.coords_[slab_dim];
    slab_end = (T)(slab_start + cell_slab_copy.length_ - 1);
  }

  // Add remaining slab
  auto cell_slab_end = (T)(cell_slab.coords_[slab_dim] + cell_slab.length_ - 1);
  if (slab_start <= cell_slab_end) {
    cell_slab_copy.length_ = slab_end - slab_start + 1;
    compute_result_cell_slabs_dense(cell_slab_copy, &result_space_tile);
  }
}

template <class T>
void ResultCellSlabIter<T>::compute_result_cell_slabs_dense(
    const CellSlab<T>& cell_slab, ResultSpaceTile<T>* result_space_tile) {
  std::list<CellSlab<T>> to_process;
  to_process.push_back(cell_slab);
  const auto& frag_domains = result_space_tile->frag_domains_;
  auto& result_tiles = result_space_tile->result_tiles_;
  auto dim_num = domain_->dim_num();
  std::vector<T> slab_overlap;
  slab_overlap.resize(dim_num);
  unsigned overlap_type;  // 0: no overlap, 1: full overlap, 2: partial overlap
  uint64_t overlap_length, start;
  CellSlab<T> p1, p2;
  bool two_slabs;
  std::vector<ResultCellSlab> result_cell_slabs;

  // Process all slabs in the `to_process` list for each fragment
  // in the result space tile
  for (const auto& fd : frag_domains) {
    for (auto pit = to_process.begin(); pit != to_process.end();) {
      compute_cell_slab_overlap(
          *pit, fd.second, &slab_overlap, &overlap_length, &overlap_type);

      // No overlap
      if (overlap_type == 0) {
        ++pit;
        continue;
      }

      // Compute new result cell slab
      compute_cell_slab_start(
          &slab_overlap[0], result_space_tile->start_coords_, &start);
      auto tit = result_tiles.find(fd.first);
      assert(tit != result_tiles.end());
      result_cell_slabs.emplace_back(&(tit->second), start, overlap_length);

      // If it is partial overlap, we need to create up to two new cell slabs
      // and re-insert to the head of `to_process` (so that the rest of the
      // fragments can process them).
      if (overlap_type == 2) {
        split_cell_slab(
            *pit, slab_overlap, overlap_length, &p1, &p2, &two_slabs);
        to_process.push_front(p1);
        if (two_slabs)
          to_process.push_front(p2);
      }

      // Erase the processed slab
      pit = to_process.erase(pit);
    }
  }

  // Append temporary results for empty cell slabs
  compute_result_cell_slabs_empty(
      *result_space_tile, to_process, &result_cell_slabs);

  // Sort the temporary result cell slabs on starting position
  std::sort(result_cell_slabs.begin(), result_cell_slabs.end());

  // Insert the temporary results to `result_cell_slabs_`
  result_cell_slabs_.insert(
      result_cell_slabs_.end(),
      result_cell_slabs.begin(),
      result_cell_slabs.end());
}

template <class T>
void ResultCellSlabIter<T>::compute_result_cell_slabs_empty(
    const ResultSpaceTile<T>& result_space_tile,
    const std::list<CellSlab<T>>& to_process,
    std::vector<ResultCellSlab>* result_cell_slabs) {
  // Nothing to process
  if (to_process.empty())
    return;

  // Create result cell slabs that belong to no fragment
  uint64_t start;
  for (auto pit = to_process.begin(); pit != to_process.end(); ++pit) {
    compute_cell_slab_start(
        &pit->coords_[0], result_space_tile.start_coords_, &start);
    result_cell_slabs->emplace_back(nullptr, start, pit->length_);
  }
}

template <class T>
void ResultCellSlabIter<T>::split_cell_slab(
    const CellSlab<T>& cell_slab,
    const std::vector<T>& slab_overlap,
    uint64_t overlap_length,
    CellSlab<T>* p1,
    CellSlab<T>* p2,
    bool* two_slabs) {
  auto dim_num = domain_->dim_num();
  auto slab_dim = (layout_ == Layout::ROW_MAJOR) ? dim_num - 1 : 0;
  auto slab_start = cell_slab.coords_[slab_dim];
  auto slab_end = slab_start + cell_slab.length_ - 1;
  auto overlap_start = slab_overlap[slab_dim];
  auto overlap_end = overlap_start + overlap_length - 1;

  // Two slabs
  if (overlap_start > slab_start && overlap_end < slab_end) {
    // Create left
    *p1 = cell_slab;
    p1->length_ = overlap_start - slab_start;

    // Create right
    *p2 = cell_slab;
    p2->coords_[slab_dim] = overlap_end + 1;
    p2->length_ = cell_slab.length_ - overlap_length - p1->length_;

    *two_slabs = true;
    return;
  }

  *two_slabs = false;

  // Create only left
  if (overlap_start > slab_start) {
    *p1 = cell_slab;
    p1->length_ = overlap_start - slab_start;
    return;
  }

  // Create only right
  if (overlap_end < slab_end) {
    *p1 = cell_slab;
    p1->coords_[slab_dim] = overlap_end + 1;
    p1->length_ = cell_slab.length_ - overlap_length;
    return;
  }

  // All the possible cases are covered above
  assert(false);
}

template <class T>
void ResultCellSlabIter<T>::update_result_cell_slab() {
  if (cell_slab_iter_.end()) {
    end_ = true;
    return;
  }

  end_ = false;
  result_cell_slabs_pos_ = 0;
  result_cell_slabs_.clear();
  auto cell_slab = cell_slab_iter_.cell_slab();

  compute_result_cell_slabs(cell_slab);
}

// Explicit template instantiations
template class ResultCellSlabIter<int8_t>;
template class ResultCellSlabIter<uint8_t>;
template class ResultCellSlabIter<int16_t>;
template class ResultCellSlabIter<uint16_t>;
template class ResultCellSlabIter<int32_t>;
template class ResultCellSlabIter<uint32_t>;
template class ResultCellSlabIter<int64_t>;
template class ResultCellSlabIter<uint64_t>;
template class ResultCellSlabIter<float>;
template class ResultCellSlabIter<double>;

}  // namespace sm
}  // namespace tiledb
