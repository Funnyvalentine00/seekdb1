/*
 * Copyright (c) 2025 OceanBase.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#define USING_LOG_PREFIX STORAGE

#include "ob_text_daat_iter.h"

namespace oceanbase
{
namespace storage
{
int ObTextDaaTIter::init(const ObTextDaaTParam &param)
{
  int ret = OB_SUCCESS;
  if (OB_ISNULL(param.base_param_) || OB_ISNULL(param.dim_iters_) || OB_ISNULL(param.allocator_)
      || OB_ISNULL(param.relevance_collector_)) {
    ret = OB_INVALID_ARGUMENT;
  } else if (OB_FAIL(ObSRDaaTIterImpl::init(*param.base_param_, *param.dim_iters_,
                                            *param.allocator_, *param.relevance_collector_))) {
  } else if (OB_NOT_NULL(param.base_param_->relevance_expr_)
             && OB_FAIL(bm25_param_estimator_.init(param.bm25_param_est_ctx_))) {
  } else {
    mode_flag_ = param.mode_flag_;
    function_lookup_mode_ = param.function_lookup_mode_;
    const ObDatumMeta &meta = iter_param_->id_proj_expr_->datum_meta_;
    batch_merge_enabled_ = no_score_ && !function_lookup_mode_
        && sql::ObMatchAgainstMode::NATURAL_LANGUAGE_MODE == mode_flag_
        && !iter_param_->need_filter() && iter_param_->eval_ctx_->is_vectorized()
        && dim_iters_->count() > 0 && dim_iters_->count() <= MAX_BATCH_MERGE_TERMS
        && (common::ObUInt64Type == meta.type_
            || (common::CS_TYPE_BINARY == meta.cs_type_ && ob_is_string_type(meta.type_)));
    clear_batch_state();
  }
  return ret;
}

void ObTextDaaTIter::reset()
{
  clear_batch_state();
  batch_merge_enabled_ = false;
  bm25_param_estimator_.reset();
  // this class can know the type of dim_iters_, but ObSRDaaTIterImpl maybe not
  if (OB_NOT_NULL(dim_iters_)) {
    for (int64_t i = 0; i < dim_iters_->count(); ++i) {
      static_cast<ObTextRetrievalDaaTTokenIter *>(dim_iters_->at(i))->reset();
    }
  }
  ObSRDaaTIterImpl::reset();
}

void ObTextDaaTIter::reuse(const bool switch_tablet)
{
  clear_batch_state();
  if ((OB_NOT_NULL(dim_iters_) && 0 == dim_iters_->count())) {
    // do nothing
  } else {
    bm25_param_estimator_.reuse(switch_tablet);
  }
  if (OB_NOT_NULL(dim_iters_)) {
    for (int64_t i = 0; i < dim_iters_->count(); ++i) {
      static_cast<ObTextRetrievalDaaTTokenIter *>(dim_iters_->at(i))->reuse();
    }
  }
  ObSRDaaTIterImpl::reuse(switch_tablet);
}

void ObTextDaaTIter::clear_batch_state()
{
  for (int64_t i = 0; i < MAX_BATCH_MERGE_TERMS; ++i) {
    batch_ids_[i] = nullptr;
    batch_counts_[i] = 0;
    batch_pos_[i] = 0;
    batch_end_[i] = false;
  }
}

int ObTextDaaTIter::compare_batch_ids(const ObDatum &left, const ObDatum &right) const
{
  int cmp = 0;
  if (OB_UNLIKELY(left.is_null() || right.is_null())) {
    cmp = left.is_null() ? (right.is_null() ? 0 : -1) : 1;
  } else if (common::ObUInt64Type == iter_param_->id_proj_expr_->datum_meta_.type_) {
    cmp = left.get_uint() < right.get_uint() ? -1 : (left.get_uint() > right.get_uint() ? 1 : 0);
  } else {
    const int64_t len = MIN(left.len_, right.len_);
    if (OB_LIKELY(16 == len)) {
      cmp = memcmp(left.ptr_, right.ptr_, 16);
    } else if (8 == len) {
      cmp = memcmp(left.ptr_, right.ptr_, 8);
    } else if (len > 0) {
      cmp = memcmp(left.ptr_, right.ptr_, len);
    }
    if (0 == cmp) {
      cmp = left.len_ < right.len_ ? -1 : (left.len_ > right.len_ ? 1 : 0);
    }
  }
  return cmp;
}

int ObTextDaaTIter::get_next_rows(const int64_t capacity, int64_t &count)
{
  int ret = OB_SUCCESS;
  if (!batch_merge_enabled_ || capacity <= 0) {
    ret = ObSRDaaTIterImpl::get_next_rows(capacity, count);
  } else {
    count = 0;
    const common::ObLimitParam &limit = *iter_param_->limit_param_;
    const int64_t real_capacity = MIN(capacity, iter_param_->max_batch_size_);
    if (limit.is_valid() && output_row_cnt_ >= limit.limit_) {
      ret = OB_ITER_END;
    }
    while (OB_SUCC(ret) && count < real_capacity) {
      const ObDatum *minimum = nullptr;
      uint64_t equal_terms = 0;
      for (int64_t i = 0; OB_SUCC(ret) && i < dim_iters_->count(); ++i) {
        if (!batch_end_[i] && batch_pos_[i] == batch_counts_[i]) {
          auto *token = static_cast<ObTextRetrievalDaaTTokenIter *>(dim_iters_->at(i));
          if (OB_FAIL(token->get_next_doc_id_batch(batch_ids_[i], batch_counts_[i]))) {
            if (OB_ITER_END == ret) {
              batch_end_[i] = true;
              ret = OB_SUCCESS;
            }
          }
          batch_pos_[i] = 0;
        }
        if (OB_SUCC(ret) && !batch_end_[i]) {
          const ObDatum &id = batch_ids_[i][batch_pos_[i]].get_datum();
          const int cmp = nullptr == minimum ? -1 : compare_batch_ids(id, *minimum);
          if (cmp < 0) {
            minimum = &id;
            equal_terms = 1ULL << i;
          } else if (0 == cmp) {
            equal_terms |= 1ULL << i;
          }
        }
      }
      if (OB_FAIL(ret)) {
      } else if (nullptr == minimum) {
        ret = OB_ITER_END;
      } else {
        // Copy into the existing output buffer before any token can refill.
        ret = cache_result(count, *minimum, 1.0);
        for (int64_t i = 0; i < dim_iters_->count(); ++i) {
          if (equal_terms & (1ULL << i)) {
            ++batch_pos_[i];
          }
        }
      }
    }
    if (OB_SUCC(ret) || OB_ITER_END == ret) {
      if (count > 0 && OB_FAIL(project_results(count))) {
      }
    }
  }
  return ret;
}

int ObTextDaaTIter::pre_process()
{
  int ret = OB_SUCCESS;
  if (dim_iters_->count() == 0) {
    ret = OB_ITER_END;
  } else if (iter_param_->need_project_relevance() && OB_NOT_NULL(iter_param_->relevance_expr_)) {
    if (OB_FAIL(bm25_param_estimator_.do_estimation(*iter_param_->eval_ctx_))) {
    }
  }
  return ret;
}

ObTextBMWIter::ObTextBMWIter()
  : ObSRBMWIterImpl(),
    bm25_param_estimator_() {}

void ObTextBMWIter::reuse(const bool switch_tablet)
{
  bm25_param_estimator_.reuse(switch_tablet);
  if (OB_NOT_NULL(dim_iters_)) {
    for (int64_t i = 0; i < dim_iters_->count(); ++i) {
      static_cast<ObTextRetrievalBlockMaxIter *>(dim_iters_->at(i))->reuse();
    }
  }
  ObSRBMWIterImpl::reuse(switch_tablet);
}

void ObTextBMWIter::reset()
{
  bm25_param_estimator_.reset();
  if (OB_NOT_NULL(dim_iters_)) {
    for (int64_t i = 0; i < dim_iters_->count(); ++i) {
      static_cast<ObTextRetrievalBlockMaxIter *>(dim_iters_->at(i))->reset();
    }
  }
  ObSRBMWIterImpl::reset();
}

int ObTextBMWIter::init(const ObTextDaaTParam &param)
{
  int ret = OB_SUCCESS;
  if (OB_ISNULL(param.base_param_) || OB_ISNULL(param.dim_iters_) || OB_ISNULL(param.allocator_)
      || OB_ISNULL(param.relevance_collector_)) {
    ret = OB_INVALID_ARGUMENT;
  } else if (OB_FAIL(ObSRBMWIterImpl::init(*param.base_param_, *param.dim_iters_,
                                           *param.allocator_, *param.relevance_collector_))) {
  } else if (OB_FAIL(bm25_param_estimator_.init(param.bm25_param_est_ctx_))) {
  }
  return ret;
}

int ObTextBMWIter::get_next_rows(const int64_t capacity, int64_t &count)
{
  int ret = OB_SUCCESS;
  if (IS_NOT_INIT) {
    ret = OB_NOT_INIT;
  } else if (dim_iters_->count() == 0) {
    ret = OB_ITER_END;
  } else if (OB_FAIL(bm25_param_estimator_.do_estimation(*iter_param_->eval_ctx_))) {
  }

  if (FAILEDx(ObSRBMWIterImpl::get_next_rows(capacity, count))) {
    if (OB_UNLIKELY(OB_ITER_END != ret)) {
    }
  }
  return ret;
}

int ObTextBMWIter::init_before_wand_process()
{
  int ret = OB_SUCCESS;
  if (OB_UNLIKELY(!bm25_param_estimator_.is_estimated())) {
    ret = OB_ERR_UNEXPECTED;
  }

  for (int64_t i = 0; OB_SUCC(ret) && i < dim_iters_->count(); ++i) {
    ObTextRetrievalBlockMaxIter *block_max_iter = static_cast<ObTextRetrievalBlockMaxIter *>(dim_iters_->at(i));
    if (OB_FAIL(block_max_iter->init_block_max_iter(
        bm25_param_estimator_.get_total_doc_cnt(), bm25_param_estimator_.get_avg_doc_token_cnt()))) {
    }
  }
  return ret;
}

} // namespace storage
} // namespace oceanbase
