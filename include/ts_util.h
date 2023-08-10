/******************************************************************
 * Code for ds with tombstones.
 ******************************************************************/
#ifndef TS_UTIL_H
#define TS_UTIL_H

#include "util.h"

/* Find next tombstone/empty `available` in range [index, nslots)
 * Shift everything in range [index, available) by 1 to the big direction.
 * Make a tombstone at `index`
 * Return:
 *     >=0: Distance between index and `available`.
 */
static inline int _insert_ts_at(QF *const qf, size_t index) {
  if (is_tombstone(qf, index)) return 0;
  uint64_t available_slot_index;
  int ret = find_first_tombstone(qf, index, &available_slot_index);
  // TODO: Handle return code correctly.
  if (ret != 0) abort();
  if (available_slot_index >= qf->metadata->xnslots) return QF_NO_SPACE;
  // Change counts
  if (is_empty(qf, available_slot_index))
    modify_metadata(&qf->runtimedata->pc_noccupied_slots, 1);
  // shift slot and metadata
  shift_remainders(qf, index, available_slot_index);
  shift_runends_tombstones(qf, index, available_slot_index, 1);
  SET_T(qf, index);
  /* Increment the offset for each block between the hash bucket index
   * and block of the empty slot  
   */
  uint64_t i;
  for (i = index / QF_SLOTS_PER_BLOCK;
        i <= available_slot_index / QF_SLOTS_PER_BLOCK; i++) {
    uint8_t *block_offset = &(get_block(qf, i)->offset);
    if (i > 0 && i * QF_SLOTS_PER_BLOCK + *block_offset -1 >= index &&
      i * QF_SLOTS_PER_BLOCK + *block_offset <= available_slot_index) {
      if (*block_offset < BITMASK(8 * sizeof(qf->blocks[0].offset)))
        *block_offset += 1;
      assert(*block_offset != 0);
    }
  }
  return available_slot_index - index;
}

/* Push tombstones over an existing run (has at least one non-tombstone).
 * Range of pushing tombstones is [push_start, push_end).
 * push_start is also the start of the run.
 * After this, push_start-1 is the end of the run.
 */
static void _push_over_run(QF *qf, size_t *push_start, size_t *push_end) {
  do {
    // push 1 slot at a time.
    if (!is_tombstone(qf, *push_end)) {
      if (*push_start != *push_end) {
        RESET_T(qf, *push_start);
        SET_T(qf, *push_end);
        set_slot(qf, *push_start, get_slot(qf, *push_end));
      }
      ++*push_start;
    }
    ++*push_end;
  } while (!is_runend(qf, *push_end-1));
  // reached the end of the run
  // reset first, because push_start may equal to push_end.
  RESET_R(qf, *push_end - 1);
  SET_R(qf, *push_start - 1);
}

/* update the offset bits.
 * find the number of occupied slots in the original_bucket block.
 * Then find the runend slot corresponding to the last run in the
 * original_bucket block.
 * Update the offset of the block to which it belongs.
 */
static void _recalculate_block_offsets(QF *qf, size_t index) {
  size_t original_block = index / QF_SLOTS_PER_BLOCK;
  while (1) {
    size_t last_occupieds_hash_index =
        QF_SLOTS_PER_BLOCK * original_block + (QF_SLOTS_PER_BLOCK - 1);
    size_t runend_index = run_end(qf, last_occupieds_hash_index);
    // runend spans across the block
    // update the offset of the next block
    if (runend_index / QF_SLOTS_PER_BLOCK ==
        original_block) { // if the run ends in the same block
      if (get_block(qf, original_block + 1)->offset == 0)
        break;
      get_block(qf, original_block + 1)->offset = 0;
    } else { // if the last run spans across the block
      if (get_block(qf, original_block + 1)->offset ==
          (runend_index - last_occupieds_hash_index))
        break;
      get_block(qf, original_block + 1)->offset =
          (runend_index - last_occupieds_hash_index);
    }
    original_block++;
  }
}


/* Rebuild quotien [`from_run`, `until_run`). Leave the pushing tombstones at
 * the beginning of until_run. Here we do rebuild run by run. 
 * Return the number of pushing tombstones at the end.
 */
static void _clear_tombstones(QF *qf) {
  // TODO: multi thread this.
  size_t curr_quotien = find_next_occupied(qf, 0);
  size_t push_start = run_start(qf, curr_quotien);
  size_t push_end = push_start;
  while (curr_quotien < qf->metadata->nslots) {
    // Range of pushing tombstones is [push_start, push_end).
    _push_over_run(qf, &push_start, &push_end);
    // fix block offset if necessary.
    _recalculate_block_offsets(qf, curr_quotien);
    // find the next run
    curr_quotien = find_next_occupied(qf, ++curr_quotien);
    if (push_start < curr_quotien) {  // Reached the end of the cluster.
      size_t n_to_free = MIN(curr_quotien, push_end) - push_start;
      if (n_to_free > 0)
        modify_metadata(&qf->runtimedata->pc_noccupied_slots, -n_to_free);
      push_start = curr_quotien;
      push_end = MAX(push_end, push_start);
    }
  }
}

#endif // TS_UTIL_H