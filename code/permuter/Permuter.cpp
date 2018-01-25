#include <list>
#include <vector>

#include <cassert>

#include "Permuter.h"
#include "../utils/utils.h"

namespace fs_testing {
namespace permuter {

using std::list;
using std::pair;
using std::size_t;
using std::vector;

using fs_testing::utils::disk_write;

namespace {

static const unsigned int kRetryMultiplier = 2;
static const unsigned int kMinRetries = 1000;

}  // namespace


size_t BioVectorHash::operator() (const vector<unsigned int>& permutation)
    const {
  unsigned int seed = permutation.size();
  for (const auto& bio_pos : permutation) {
    seed ^= bio_pos + 0x9e3779b9 + (seed << 6) + (seed >> 2);
  }
  return seed;
}

bool BioVectorEqual::operator() (const std::vector<unsigned int>& a,
    const std::vector<unsigned int>& b) const {
  if (a.size() != b.size()) {
    return false;
  }
  for (unsigned int i = 0; i < a.size(); ++i) {
    if (a.at(i) != b.at(i)) {
      return false;
    }
  }
  return true;
}

/*
 * Given a disk_write operation and a *sorted* list of already existing ranges,
 * determine if the current operation partially or completely overlaps any of
 * the operations already in the list.
 *
 * Returns false if the operation does not belong to any range.
 * Else, returns true.
 */
bool Permuter::FindOverlapsAndInsert(disk_write &dw,
    list<pair<unsigned int, unsigned int>> &ranges) const {

  unsigned int start = dw.metadata.write_sector;
  unsigned int end = start + dw.metadata.size;
  for (auto range_iter = ranges.begin(); range_iter != ranges.end();
      range_iter++) {
    if ((range_iter->first <= start && range_iter->second >= start) ||
        (range_iter->first <= end && range_iter->second >= end) ||
        (range_iter->first >= start && range_iter->second <= end)) {
      // We need to extend our range to cover what we are looking at.
      if (range_iter->first > dw.metadata.write_sector) {
        range_iter->first = dw.metadata.write_sector;
      }
      unsigned int end =
        dw.metadata.write_sector + dw.metadata.size;
      if (range_iter->second < end) {
        range_iter->second = end;
      }
      return true;
    } else if (range_iter->first > end) {
      // We assume the list we are given is ordered. Therefore, if the next item
      // in the list has a start that is greater than the end of the disk_write
      // in question, we know we won't find anything else in the list this
      // disk_write overlaps with. In this case, we should insert the
      // disk_write in the list where we currently are.
      ranges.insert(range_iter,
          {dw.metadata.write_sector,
          dw.metadata.write_sector + dw.metadata.size});
      return false;
    }
  }

  // We reached the end of the list of ranges without finding anything starting
  // after the end of what we are looking at.
  ranges.emplace_back(dw.metadata.write_sector,
      dw.metadata.write_sector + dw.metadata.size);
  return false;
}

void Permuter::InitDataVector(vector<disk_write> &data) {
  epochs_.clear();
  list<pair<unsigned int, unsigned int>> epoch_overlaps;
  struct epoch *current_epoch = NULL;
  // Make sure that the first time we mark a checkpoint epoch, we start at 0 and
  // not 1.
  int curr_checkpoint_epoch = -1;
  // Aligns with the index of the bio in the profile dump, 0 indexed.
  unsigned int abs_index = 0;

  auto curr_op = data.begin();
  while (curr_op != data.end()) {
    if (current_epoch == NULL) {
      epochs_.emplace_back();
      current_epoch = &epochs_.back();
      // Overlaps are only searched for within the current epoch, not across
      // epochs.
      epoch_overlaps.clear();
      current_epoch->has_barrier = false;
      current_epoch->overlaps = false;
      current_epoch->checkpoint_epoch = curr_checkpoint_epoch;
    }

    // While the operation is not a barrier operation (flush or FUA of some
    // sort), we should add it to the current epoch. If the operation is a
    // barrier operation, we want to add it to the current epoch and switch
    // epochs.
    while (curr_op != data.end() && !curr_op->is_barrier()) {
      // Checkpoint operations will only be seen once we have switched over
      // epochs, so we need to edit the checkpoint epoch of the current epoch as
      // well as incrementing the curr_checkpoint_epoch counter.
      if (curr_op->is_checkpoint()) {
        ++curr_checkpoint_epoch;
        current_epoch->checkpoint_epoch = curr_checkpoint_epoch;
        // Checkpoint operations should not appear in the bio stream passed to
        // actual permuters.
        ++curr_op;
        ++abs_index;
        continue;
      }

      // Check if the current operation overlaps anything we have seen already
      // in this epoch.
      if (FindOverlapsAndInsert(*curr_op, epoch_overlaps)) {
        current_epoch->overlaps = true;
      }

      current_epoch->ops.push_back({abs_index, *curr_op});
      current_epoch->num_meta += curr_op->is_meta();
      ++abs_index;
      ++curr_op;
    }

    // Check is the op at the current index is a "barrier." If it is then add it
    // to the end of the epoch, otherwise just push the current epoch onto the
    // list and move to the next segment of the log. The only reasons you should
    // get here really are 1. you reached the end of the epoch (saw a barrier)
    // or 2. you reached the end of the recorded workload.
    if (curr_op != data.end()) {
      assert(curr_op->is_barrier());

      // Check if the op at the current index has a flush flag with data. If it
      // does, then divide it into an operation with the flush flag and an
      // operation with the data where the data is available only in the start
      // of the next epoch. This is necessary because a flush flag only
      // stipulates the previous data is persisted, and says nothing about the
      // data in the current operation's persistence. If the FUA flag is
      // present, then the current data is persisted with the previous data,
      // meaning this block does not apply.
      if ((curr_op->has_flush_flag() || curr_op->has_flush_seq_flag()) &&
          curr_op->has_write_flag() && !curr_op->has_FUA_flag() &&
          curr_op->metadata.size > 0) {
        disk_write flag_half(*curr_op);
        disk_write data_half(*curr_op);
        
        if(curr_op->has_flush_flag()) {
          flag_half.set_flush_flag();
          data_half.clear_flush_flag();
        }
        
        if(curr_op->has_flush_seq_flag()) {
          flag_half.set_flush_seq_flag();
          data_half.clear_flush_seq_flag();
        }

        flag_half.metadata.size = 0;
        flag_half.clear_data();

        // Add the flush to the current epoch.
        current_epoch->ops.push_back({abs_index, flag_half});
        current_epoch->num_meta += flag_half.is_meta();
        current_epoch->has_barrier = true;
        epochs_.push_back(*current_epoch);

        // Switch epochs.
        epochs_.emplace_back();
        current_epoch = &epochs_.back();
        epoch_overlaps.clear();
        current_epoch->has_barrier = false;
        current_epoch->overlaps = false;
        current_epoch->checkpoint_epoch = curr_checkpoint_epoch;
        // We are adding a new operation to the new epoch, so we need to record
        // it in the list of things to check for overlaps.
        epoch_overlaps.emplace_back(data_half.metadata.write_sector,
            data_half.metadata.write_sector + data_half.metadata.size);

        // Setup the rest of the data part of the operation.
        // TODO(ashmrtn): Find a better way to handle matching an index to a bio
        // in the profile dump.
        //++abs_index;
        current_epoch->ops.push_back({abs_index, data_half});
        current_epoch->num_meta += data_half.is_meta();

        ++abs_index;
        ++curr_op;
      } else {
        // This is just the case where we have a normal barrier operation ending
        // the epoch.
        current_epoch->ops.push_back({abs_index, *curr_op});
        current_epoch->num_meta += curr_op->is_meta();
        current_epoch->has_barrier = true;
        ++abs_index;
        ++curr_op;

        // This will cause us to create a new epoch at the end of our vector on
        // the next loop.
        current_epoch = NULL;
      }
    }
  }
}

vector<epoch>* Permuter::GetEpochs() {
  return &epochs_;
}


bool Permuter::GenerateCrashState(vector<disk_write>& res,
    PermuteTestResult &log_data) {
  vector<epoch_op> crash_state;
  unsigned long retries = 0;
  unsigned int exists = 0;
  bool new_state = true;
  vector<unsigned int> crash_state_hash;

  unsigned long max_retries =
    ((kRetryMultiplier * completed_permutations_.size()) < kMinRetries)
      ? kMinRetries
      : kRetryMultiplier * completed_permutations_.size();
  do {
    new_state = gen_one_state(crash_state, log_data);

    crash_state_hash.clear();
    crash_state_hash.resize(crash_state.size());
    for (unsigned int i = 0; i < crash_state.size(); ++i) {
      crash_state_hash.at(i) = crash_state.at(i).abs_index;
    }

    ++retries;
    exists = completed_permutations_.count(crash_state_hash);
    if (!new_state || retries >= max_retries) {
      // We've likely found all possible crash states so just break. The
      // constant in the multiplier was randomly chosen in the hopes that it
      // would be a good hueristic. This is more to make sure that we don't spin
      // endlessly than it is for it to be a good way to break out of trying to
      // make unique permutations.
      break;
    }
  } while (exists > 0);

  // Move the permuted crash state data over into the returned crash state
  // vector.
  res.clear();
  res.resize(crash_state.size());
  for (unsigned int i = 0; i < crash_state.size(); ++i) {
    res.at(i) = crash_state.at(i).op;
  }

  if (exists == 0) {
    completed_permutations_.insert(crash_state_hash);
    // We broke out of the above loop because this state is unique.
    return new_state;
  }

  // We broke out of the above loop because we haven't found a new state in some
  // time.
  return false;
}

}  // namespace permuter
}  // namespace fs_testing
